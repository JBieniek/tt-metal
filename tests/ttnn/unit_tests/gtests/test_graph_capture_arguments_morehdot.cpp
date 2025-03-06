// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
//
// SPDX-License-Identifier: Apache-2.0

#include "gtest/gtest.h"
#include "ttnn_test_fixtures.hpp"
#include "ttnn/device.hpp"
#include "ttnn/graph/graph_processor.hpp"
#include "ttnn/graph/graph_consts.hpp"
#include "ttnn/graph/graph_trace_utils.hpp"
#include "ttnn/operations/moreh/moreh_dot/moreh_dot.hpp"
#include <optional>
#include <string>

namespace ttnn::graph::arguments::test {

class TestGraphCaptureArgumentsMorehDot : public TTNNFixtureWithTensor {};

TEST_P(TestGraphCaptureArgumentsMorehDot, MorehDot) {
    auto tt_input1 = CreateTensor();
    auto tt_input2 = CreateTensor();
    ttnn::graph::GraphProcessor::begin_graph_capture(tt::tt_metal::IGraphProcessor::RunMode::NORMAL);
    ttnn::moreh_dot(tt_input1, tt_input2, std::nullopt, DataType::BFLOAT16, std::nullopt, std::nullopt);
    auto trace = ttnn::graph::GraphProcessor::end_graph_capture();
    auto operations = ttnn::graph::extract_arguments(trace);

    auto operation0 = operations[0];
    EXPECT_EQ(operation0.operation_name, "ttnn::moreh_dot");
    EXPECT_EQ(operation0.arguments.size(), 6);
    EXPECT_EQ(
        operation0.arguments[0],
        "Tensor(storage=DeviceStorage(memory_config=MemoryConfig(memory_layout=TensorMemoryLayout::INTERLEAVED,buffer_"
        "type=BufferType::L1,shard_spec=std::nullopt)),tensor_spec=TensorSpec(logical_shape=Shape([1, 1, 1, "
        "32]),tensor_layout=TensorLayout(dtype=BFLOAT16,page_config=PageConfig(config=TilePageConfig(tile=Tile(tile_"
        "shape={32, 32},face_shape={16, "
        "16},num_faces=4))),memory_config=MemoryConfig(memory_layout=TensorMemoryLayout::INTERLEAVED,buffer_type="
        "BufferType::L1,shard_spec=std::nullopt),alignment=Alignment([32, 32]))))");
    EXPECT_EQ(
        operation0.arguments[1],
        "Tensor(storage=DeviceStorage(memory_config=MemoryConfig(memory_layout=TensorMemoryLayout::INTERLEAVED,buffer_"
        "type=BufferType::L1,shard_spec=std::nullopt)),tensor_spec=TensorSpec(logical_shape=Shape([1, 1, 1, "
        "32]),tensor_layout=TensorLayout(dtype=BFLOAT16,page_config=PageConfig(config=TilePageConfig(tile=Tile(tile_"
        "shape={32, 32},face_shape={16, "
        "16},num_faces=4))),memory_config=MemoryConfig(memory_layout=TensorMemoryLayout::INTERLEAVED,buffer_type="
        "BufferType::L1,shard_spec=std::nullopt),alignment=Alignment([32, 32]))))");
    EXPECT_EQ(operation0.arguments[2], "[ unsupported type , std::__1::reference_wrapper<std::__1::nullopt_t const>]");
    EXPECT_EQ(operation0.arguments[3], "BFLOAT16");
    EXPECT_EQ(operation0.arguments[4], "[ unsupported type , std::__1::reference_wrapper<std::__1::nullopt_t const>]");
    EXPECT_EQ(operation0.arguments[5], "[ unsupported type , std::__1::reference_wrapper<std::__1::nullopt_t const>]");

    auto operation1 = operations[1];
    EXPECT_EQ(operation1.operation_name, "ttnn::prim::moreh_dot");
    EXPECT_EQ(operation1.arguments.size(), 6);
    EXPECT_EQ(
        operation1.arguments[0],
        "Tensor(storage=DeviceStorage(memory_config=MemoryConfig(memory_layout=TensorMemoryLayout::INTERLEAVED,buffer_"
        "type=BufferType::L1,shard_spec=std::nullopt)),tensor_spec=TensorSpec(logical_shape=Shape([1, 1, 1, "
        "32]),tensor_layout=TensorLayout(dtype=BFLOAT16,page_config=PageConfig(config=TilePageConfig(tile=Tile(tile_"
        "shape={32, 32},face_shape={16, "
        "16},num_faces=4))),memory_config=MemoryConfig(memory_layout=TensorMemoryLayout::INTERLEAVED,buffer_type="
        "BufferType::L1,shard_spec=std::nullopt),alignment=Alignment([32, 32]))))");
    EXPECT_EQ(
        operation1.arguments[1],
        "Tensor(storage=DeviceStorage(memory_config=MemoryConfig(memory_layout=TensorMemoryLayout::INTERLEAVED,buffer_"
        "type=BufferType::L1,shard_spec=std::nullopt)),tensor_spec=TensorSpec(logical_shape=Shape([1, 1, 1, "
        "32]),tensor_layout=TensorLayout(dtype=BFLOAT16,page_config=PageConfig(config=TilePageConfig(tile=Tile(tile_"
        "shape={32, 32},face_shape={16, "
        "16},num_faces=4))),memory_config=MemoryConfig(memory_layout=TensorMemoryLayout::INTERLEAVED,buffer_type="
        "BufferType::L1,shard_spec=std::nullopt),alignment=Alignment([32, 32]))))");
    EXPECT_EQ(operation1.arguments[2], "nullopt");
    EXPECT_EQ(operation1.arguments[3], "BFLOAT16");
    EXPECT_EQ(operation1.arguments[4], "nullopt");
    EXPECT_EQ(
        operation1.arguments[5],
        "[ unsupported type , "
        "std::__1::reference_wrapper<std::__1::optional<std::__1::variant<ttnn::GrayskullComputeKernelConfig, "
        "ttnn::WormholeComputeKernelConfig>> const>]");

    auto operation2 = operations[2];
    EXPECT_EQ(operation2.operation_name, "MorehDotOperation");
    EXPECT_EQ(operation2.arguments.size(), 2);
    EXPECT_EQ(
        operation2.arguments[0],
        "[ unsupported type , "
        "std::__1::reference_wrapper<ttnn::operations::moreh::moreh_dot::MorehDotOperation::operation_attributes_t "
        "const>]");
    EXPECT_EQ(
        operation2.arguments[1],
        "[ unsupported type , "
        "std::__1::reference_wrapper<ttnn::operations::moreh::moreh_dot::MorehDotOperation::tensor_args_t const>]");

    auto operation3 = operations[3];
    EXPECT_EQ(operation3.operation_name, "tt::tt_metal::create_device_tensor");
    EXPECT_EQ(operation3.arguments.size(), 5);
    EXPECT_EQ(operation3.arguments[0], "Shape([1, 1, 1, 1])");
    EXPECT_EQ(operation3.arguments[1], "BFLOAT16");
    EXPECT_EQ(operation3.arguments[2], "Tile");
    EXPECT_EQ(operation3.arguments[3], "[ unsupported type , std::__1::reference_wrapper<tt::tt_metal::v0::IDevice*>]");
    EXPECT_EQ(
        operation3.arguments[4],
        "MemoryConfig(memory_layout=TensorMemoryLayout::INTERLEAVED,buffer_type=BufferType::L1,shard_spec=std::"
        "nullopt)");
}

INSTANTIATE_TEST_SUITE_P(
    TestGraphCaptureArgumentsMorehDot_MorehDot,
    TestGraphCaptureArgumentsMorehDot,
    ::testing::Values(CreateTensorParameters{
        .input_shape = ttnn::Shape({1, 1, 1, 32}),
        .dtype = DataType::BFLOAT16,
        .layout = TILE_LAYOUT,
        .mem_cfg = L1_MEMORY_CONFIG}));
}  // namespace ttnn::graph::arguments::test
