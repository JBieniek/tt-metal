# SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.

# SPDX-License-Identifier: Apache-2.0

import torch
import pytest
import math
from time import time
from loguru import logger
import ttnn
from tests.tt_eager.python_api_testing.sweep_tests.comparison_funcs import comp_equal, comp_pcc
from models.utility_functions import skip_for_grayskull
from tests.ttnn.unit_tests.operations.ccl.test_ccl_common import (
    create_and_load_sub_device_manager_with_fabric_interface,
    teardown_fabric_interface,
    create_global_semaphore_with_same_address,
)

from tests.tt_eager.python_api_testing.unit_testing.misc.test_matmul_1d_gather_in0 import (
    num_cores_to_rectangle_grid,
    round_up,
)
from models.perf.benchmarking_utils import BenchmarkProfiler
from tracy import signpost


def check_mesh_tensor_alloc(tensor):
    device_tensors = ttnn.get_device_tensors(tensor)
    buffer_addr = device_tensors[0].buffer_address()

    if len(device_tensors) > 1:
        for i in range(1, len(device_tensors)):
            addr = device_tensors[i].buffer_address()
            if not addr == buffer_addr:
                return False
    return True


def run_all_reduce_impl(
    mesh_device,
    output_shape,
    cluster_axis,
    input_dtype,
    num_links,
    input_num_cores,
    output_num_cores,
    loopback_size=1,
    num_iters=1,
    warmup_iters=0,
    enable_async=False,
    trace_mode=False,
    validate_all=True,
    profiler=BenchmarkProfiler(),
):
    cluster_shape = (8, 4)

    create_persistent_fabric = True
    teardown_persistent_fabric = True
    enable_persistent_fabric = True
    if num_iters < 1:
        pytest.fail("num_iters must be >= 1")
    # Use Async mode based on test input config
    mesh_device.enable_async(enable_async)

    if enable_async:
        logger.info(f"Using Async Mode for All Gather Op Dispatch")

    ##################################
    ##### Set up fabric stuff
    ##################################
    compute_grid_size = mesh_device.compute_with_storage_grid_size()
    ccl_sub_device_crs = ttnn.CoreRangeSet(
        {ttnn.CoreRange(ttnn.CoreCoord(0, 0), ttnn.CoreCoord(compute_grid_size.x - 1, compute_grid_size.y - 1))}
    )
    worker_sub_device = ttnn.SubDevice(
        [
            ccl_sub_device_crs,
        ]
    )
    worker_sub_device_id = ttnn.SubDeviceId(0)
    sub_device_stall_group = [worker_sub_device_id]
    if create_persistent_fabric:
        mesh_sub_device_manager_id = create_and_load_sub_device_manager_with_fabric_interface(
            mesh_device, [worker_sub_device], 0, 0, enable_persistent_fabric
        )
        mesh_device.set_sub_device_stall_group(sub_device_stall_group)

    # create global semaphore handles
    num_buffers = 8
    ccl_semaphore_handles = [
        create_global_semaphore_with_same_address(mesh_device, ccl_sub_device_crs, 0) for _ in range(num_buffers)
    ]

    logger.info(f"Output shape: {output_shape}")

    try:
        ##################################
        ##### Set up input tensors/configs
        ##################################

        ##### FF2 Case #####
        M, N = output_shape[2:]
        N_per_shard = round_up(math.ceil(N / input_num_cores), ttnn.TILE_SIZE)
        output_N_per_shard = round_up(math.ceil(N / output_num_cores), ttnn.TILE_SIZE)
        input_shape = [*cluster_shape, M, N]
        intermediate_shape = [*input_shape[:-1], N * cluster_shape[cluster_axis]]

        CORE_RANGE = [(x, y) for y in range(compute_grid_size.y) for x in range(compute_grid_size.x)]
        core_range_set = ttnn.CoreRangeSet(
            [
                ttnn.CoreRange(
                    ttnn.CoreCoord(x, y),
                    ttnn.CoreCoord(x, y),
                )
                for x, y in CORE_RANGE[:input_num_cores]
            ]
        )
        input_mem_config = ttnn.MemoryConfig(
            ttnn.TensorMemoryLayout.WIDTH_SHARDED,
            ttnn.BufferType.L1,
            ttnn.ShardSpec(
                core_range_set,
                [M, N_per_shard],
                ttnn.ShardOrientation.ROW_MAJOR,
            ),
        )
        output_core_range_set = ttnn.CoreRangeSet(
            [
                ttnn.CoreRange(
                    ttnn.CoreCoord(x, y),
                    ttnn.CoreCoord(x, y),
                )
                for x, y in CORE_RANGE[:output_num_cores]
            ]
        )
        output_mem_config = ttnn.MemoryConfig(
            ttnn.TensorMemoryLayout.WIDTH_SHARDED,
            ttnn.BufferType.L1,
            ttnn.ShardSpec(
                output_core_range_set,
                [M, output_N_per_shard],
                ttnn.ShardOrientation.ROW_MAJOR,
            ),
        )
        intermediate_mem_config = ttnn.MemoryConfig(
            ttnn.TensorMemoryLayout.WIDTH_SHARDED,
            ttnn.BufferType.L1,
            ttnn.ShardSpec(
                output_core_range_set,
                [M, output_N_per_shard * cluster_shape[cluster_axis]],
                ttnn.ShardOrientation.ROW_MAJOR,
            ),
        )

        logger.info(f"Input shape: {input_shape[2:]}, Padded shape: {[M, N_per_shard * input_num_cores]}")
        input_tensor = torch.randn(input_shape)
        tt_input_tensor = ttnn.from_torch(
            input_tensor,
            device=mesh_device,
            layout=ttnn.TILE_LAYOUT,
            dtype=input_dtype,
            memory_config=input_mem_config,
            mesh_mapper=ttnn.ShardTensor2dMesh(mesh_device, dims=(0, 1), mesh_shape=cluster_shape),
        )
        check_mesh_tensor_alloc(tt_input_tensor)

        intermediate_tensor = torch.zeros(intermediate_shape)
        tt_intermediate_tensors = []
        for i in range(num_buffers):
            tt_intermediate_tensor = ttnn.from_torch(
                intermediate_tensor,
                device=mesh_device,
                layout=ttnn.TILE_LAYOUT,
                dtype=input_dtype,
                memory_config=intermediate_mem_config,
                mesh_mapper=ttnn.ShardTensor2dMesh(mesh_device, dims=(0, 1), mesh_shape=cluster_shape),
            )

            # Validate that the tensor is allocated in same location across devices
            check_mesh_tensor_alloc(tt_intermediate_tensor)
            tt_intermediate_tensors.append(tt_intermediate_tensor)

        # All-Reduce Golden
        # Inputs reduce sequentially for 10 iters
        output_tensor_goldens_list = []
        for i in range(num_iters):
            if i % loopback_size == 0:
                ar_input_tensor = input_tensor

            output_tensor_goldens_list.append(torch.sum(ar_input_tensor, dim=cluster_axis))
            ar_input_tensor = torch.concat(
                [output_tensor_goldens_list[-1].unsqueeze(cluster_axis)] * cluster_shape[cluster_axis], dim=cluster_axis
            )

        ##################################
        ##### Run the op
        ##################################

        def run_op(n_iters, store_all_results=True):
            outs = []
            for i in range(n_iters):
                if i % loopback_size == 0:
                    tt_input = tt_input_tensor

                out = ttnn.experimental.all_reduce_async(
                    tt_input,
                    tt_intermediate_tensors[i % num_buffers],
                    cluster_axis=cluster_axis,
                    mesh_device=mesh_device,
                    multi_device_global_semaphore=ccl_semaphore_handles[i % num_buffers],
                    memory_config=output_mem_config,
                    topology=ttnn.Topology.Linear,
                    num_links=num_links,
                    subdevice_id=worker_sub_device_id,
                )
                if not trace_mode:
                    ttnn.synchronize_device(mesh_device)
                if store_all_results:
                    outs.append(out)

                # Loop back the output to the input
                if loopback_size != 1:
                    tt_input = ttnn.reshard(out, input_mem_config)

            if store_all_results:
                return outs
            else:
                return [out]

        if trace_mode:
            ##### Compile Model #####
            logger.info("Compiling model")
            tt_outs = run_op(num_iters, store_all_results=validate_all)

            ##### Capture Trace #####
            logger.info("Capturing trace")
            if warmup_iters > 0:
                trace_id_warmup = ttnn.begin_trace_capture(mesh_device, cq_id=0)
                tt_outs = run_op(warmup_iters, store_all_results=validate_all)
                ttnn.end_trace_capture(mesh_device, trace_id_warmup, cq_id=0)
                ttnn.synchronize_device(mesh_device)

            trace_id = ttnn.begin_trace_capture(mesh_device, cq_id=0)
            tt_outs = run_op(num_iters, store_all_results=validate_all)
            ttnn.end_trace_capture(mesh_device, trace_id, cq_id=0)
            ttnn.synchronize_device(mesh_device)

            ##### Run Trace #####
            logger.info("Starting Trace perf test...")
            profiler.start("all-reduce-async-trace-warmup")
            if warmup_iters > 0:
                ttnn.execute_trace(mesh_device, trace_id_warmup, blocking=False)
                ttnn.release_trace(mesh_device, trace_id_warmup)
                ttnn.synchronize_device(mesh_device)
            profiler.end("all-reduce-async-trace-warmup")

            signpost("start")
            profiler.start("all-reduce-async-trace")
            ttnn.execute_trace(mesh_device, trace_id, blocking=False)
            ttnn.release_trace(mesh_device, trace_id)
            ttnn.synchronize_device(mesh_device)
            profiler.end("all-reduce-async-trace")
            signpost("stop")
            time_taken = profiler.get_duration("all-reduce-async-trace") - profiler.get_duration(
                "all-reduce-async-trace-warmup"
            )
            effective_iter = num_iters - warmup_iters
            logger.info(f"Time taken e2e: {time_taken} s")
            logger.info(f"Time per iter e2e: {time_taken / effective_iter} s")
            logger.info(f"Time per iter e2e: {time_taken / effective_iter * 1e6} us")

        else:
            signpost("start")
            tt_outs = run_op(num_iters, store_all_results=validate_all)
            signpost("stop")

        ##################################
        ##### Validation
        ##################################
        def validate(tt_out_tensor, output_tensor):
            for i, t in enumerate(ttnn.get_device_tensors(tt_out_tensor)):
                # get_device_tensors returns row major, so we need to select the correct golden tensor
                if cluster_axis == 0:
                    output_tensor_ = output_tensor[i % cluster_shape[not (cluster_axis)]].unsqueeze(0).unsqueeze(0)
                else:
                    output_tensor_ = output_tensor[i // cluster_shape[cluster_axis]].unsqueeze(0).unsqueeze(0)

                tt_output_tensor = t.cpu().to_torch()
                # logger.info(f"Checking for device {t.device().id()}")

                if input_dtype == ttnn.bfloat16:
                    eq, output = comp_pcc(tt_output_tensor, output_tensor_)
                else:
                    eq, output = comp_pcc(tt_output_tensor, output_tensor_)
                assert eq, f"{i} FAILED: {output}"
            logger.info(f"PCC output is: {output}")

        if validate_all:
            for tensor_index in range(len(tt_outs)):
                tt_out_tensor = tt_outs[tensor_index]
                output_tensor = output_tensor_goldens_list[tensor_index]
                validate(tt_out_tensor, output_tensor)
        else:
            tt_out_tensor = tt_outs[-1]
            output_tensor = output_tensor_goldens_list[-1]
            validate(tt_out_tensor, output_tensor)

        for i in range(mesh_device.get_num_devices()):
            reshard_op_cnt = 1 if loopback_size > 1 else 0
            assert (
                mesh_device.get_devices()[i].num_program_cache_entries() == 1 + reshard_op_cnt
                or mesh_device.get_devices()[i].num_program_cache_entries() == num_iters + reshard_op_cnt
            ), f"Device {i} has {mesh_device.get_devices()[i].num_program_cache_entries()} program cache entries"

    finally:
        if enable_persistent_fabric and teardown_persistent_fabric:
            mesh_device.reset_sub_device_stall_group()
            t1 = time()
            teardown_fabric_interface(mesh_device)
            t2 = time()
            logger.info(f"Teardown time: {t2 - t1}")


@skip_for_grayskull("Requires eth connected devices to run")
@pytest.mark.timeout(900)
@pytest.mark.parametrize(
    "output_shape, cluster_axis, num_links, input_num_cores, output_num_cores",
    [
        ([1, 1, 32, 2048], 0, 4, 24, 16),  # FF2/DO all reduce
        ([1, 1, 32, 1280], 1, 3, 24, 40),  # QKV all reduce
        ([1, 1, 32, 3584], 1, 3, 24, 24),  # FF1 all reduce
        ([1, 1, 32, 2048], 0, 3, 24, 16),  # FF2/DO all reduce
        ([1, 1, 32, 16 * 1024], 1, 3, 32, 32),  # LM Head all reduce
        ([1, 1, 32, 1280], 1, 2, 24, 40),  # QKV all reduce
        ([1, 1, 32, 3584], 1, 2, 24, 24),  # FF1 all reduce
        ([1, 1, 32, 2048], 0, 2, 24, 16),  # FF2/DO all reduce
        ([1, 1, 32, 16 * 1024], 1, 2, 32, 32),  # LM Head all reduce
        ([1, 1, 32, 1280], 1, 1, 24, 40),  # QKV all reduce
        ([1, 1, 32, 3584], 1, 1, 24, 24),  # FF1 all reduce
        ([1, 1, 32, 2048], 0, 1, 24, 16),  # FF2/DO all reduce
        ([1, 1, 32, 16 * 1024], 1, 1, 32, 32),  # LM Head all reduce
    ],
)
@pytest.mark.parametrize(
    "input_dtype",
    [
        ttnn.bfloat16,
        ttnn.bfloat8_b,
    ],
)
@pytest.mark.parametrize(
    "num_iters, warmup_iters",
    [
        (1000, 100),
    ],
)
@pytest.mark.parametrize("enable_async", [True])
@pytest.mark.parametrize("trace_mode", [True])
@pytest.mark.parametrize(
    "device_params",
    [{"trace_region_size": 23887872}],
    indirect=True,
)
@pytest.mark.parametrize(
    "mesh_device",
    [
        (8, 4),
    ],
    indirect=True,
)
def test_all_reduce(
    mesh_device,
    output_shape,
    cluster_axis,
    input_dtype,
    num_links,
    input_num_cores,
    output_num_cores,
    num_iters,
    warmup_iters,
    enable_async,
    trace_mode,
    use_program_cache,
    function_level_defaults,
):
    if len(mesh_device.get_devices()) != 32:
        pytest.skip("Not TG!")

    profiler = BenchmarkProfiler()

    run_all_reduce_impl(
        mesh_device,
        output_shape,
        cluster_axis,
        input_dtype,
        num_links,
        input_num_cores,
        output_num_cores,
        num_iters=num_iters,
        warmup_iters=warmup_iters,
        enable_async=enable_async,
        trace_mode=trace_mode,
        validate_all=False,
        profiler=profiler,
    )

    time_taken = profiler.get_duration("all-reduce-async-trace") - profiler.get_duration(
        "all-reduce-async-trace-warmup"
    )
    effective_iter = num_iters - warmup_iters
    latency_us = time_taken / effective_iter * 1e6
    logger.info(f"Time taken: {time_taken} s")
    logger.info(f"Time per iter: {latency_us} us")


@skip_for_grayskull("Requires eth connected devices to run")
@pytest.mark.timeout(600)
@pytest.mark.parametrize(
    "output_shape, cluster_axis, num_links, input_num_cores, output_num_cores",
    [
        ([1, 1, 32, 1280], 1, 1, 24, 40),  # QKV all reduce
        ([1, 1, 32, 3584], 1, 1, 24, 24),  # FF1 all reduce
        ([1, 1, 32, 2048], 0, 1, 24, 16),  # FF2/DO all reduce
    ],
)
@pytest.mark.parametrize(
    "input_dtype",
    [
        ttnn.bfloat8_b,
    ],
)
@pytest.mark.parametrize(
    "num_iters, warmup_iters",
    [
        (100, 10),
    ],
)
@pytest.mark.parametrize("enable_async", [True])
@pytest.mark.parametrize("trace_mode", [True])
@pytest.mark.parametrize(
    "device_params",
    [{"trace_region_size": 23887872}],
    indirect=True,
)
@pytest.mark.parametrize(
    "mesh_device",
    [
        (8, 4),
    ],
    indirect=True,
)
def test_all_reduce_loopback(
    mesh_device,
    output_shape,
    cluster_axis,
    input_dtype,
    num_links,
    input_num_cores,
    output_num_cores,
    num_iters,
    warmup_iters,
    enable_async,
    trace_mode,
    use_program_cache,
    function_level_defaults,
):
    if len(mesh_device.get_devices()) != 32:
        pytest.skip("Not TG!")

    run_all_reduce_impl(
        mesh_device,
        output_shape,
        cluster_axis,
        input_dtype,
        num_links,
        input_num_cores,
        output_num_cores,
        loopback_size=4,
        num_iters=num_iters,
        warmup_iters=warmup_iters,
        enable_async=enable_async,
        trace_mode=trace_mode,
        validate_all=False,
    )
