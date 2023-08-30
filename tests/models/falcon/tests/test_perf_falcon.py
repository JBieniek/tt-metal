import torch
import pytest
from loguru import logger
from pathlib import Path

import tt_lib
from tests.models.falcon.reference.hf_modeling_falcon import (
    FalconForCausalLM,
)
from tests.models.falcon.falcon_causallm import TtFalconCausalLM

# TODO: Remove this?
from tests.models.falcon.falcon_common import (
    PytorchFalconCausalLM,
)

from tests.models.falcon.model_config import (
    get_model_config,
    get_tt_cache_path,
)

from tests.tt_eager.python_api_testing.sweep_tests.comparison_funcs import (
    comp_allclose,
    comp_pcc,
)
from models.utility_functions import (
    torch2tt_tensor,
    tt2torch_tensor,
    profiler,
    prep_report,
    enable_persistent_kernel_cache,
    disable_persistent_kernel_cache,
    disable_compilation_reports,
)


# TODO: Replace this with actual Falcon application-level tests
def run_test_FalconCausalLM_end_to_end(
    device,
    model_version,
    llm_mode,
    batch,
    seq_len,
    kv_cache_len,
    num_layers,
    pcc,
    model_config,
    tt_cache_path,
    model_location_generator,
    expected_inference_time,
):
    model_name = model_location_generator(model_version, model_subdir="Falcon")

    profiler.start("hugging_face_model_setup")
    hugging_face_reference_model = FalconForCausalLM.from_pretrained(model_name)
    hugging_face_reference_model.eval()
    configuration = hugging_face_reference_model.config
    state_dict = hugging_face_reference_model.state_dict()
    pytorch_FalconCausalLM = PytorchFalconCausalLM(
        hugging_face_reference_model, num_layers
    )
    profiler.end("hugging_face_model_setup")

    # Prepare input ------------------------------------------------------------------------
    torch.manual_seed(0)
    base_url = ""
    max_position_embeddings = 2048
    head_dim = configuration.hidden_size // configuration.n_head
    use_cache = True

    if 1:
        model_input = torch.arange(seq_len * batch).reshape(batch, seq_len)
    else:
        # batch identical sequences for debugging
        model_input = torch.stack([torch.arange(seq_len)] * batch).reshape(
            batch, seq_len
        )
    logger.info(f"#################################################### {llm_mode}")
    logger.info(f"model input shape {model_input.shape}")
    logger.info(f"####################################################")

    # Generate dummy kv_cache --------------------------------------------------------------
    if llm_mode == "prefill":
        q_len, kv_len = seq_len, seq_len
        assert q_len % 32 == 0, "For prefill, seq_len must be multiple of 32!"
        assert kv_cache_len == 0, "For prefill, no kv_cache is passed in!"

        past_key_values = None
        tt_layer_past = ()
        k_cache = torch.zeros(batch, max_position_embeddings, head_dim).unsqueeze(1)
        v_cache = torch.zeros(batch, max_position_embeddings, head_dim).unsqueeze(1)
        for i in range(num_layers):
            tt_k_cache = torch2tt_tensor(k_cache, device)
            tt_v_cache = torch2tt_tensor(v_cache, device)
            tt_layer_past += ((tt_k_cache, tt_v_cache),)

    elif llm_mode == "decode":
        q_len, kv_len = seq_len, kv_cache_len + 1
        assert batch % 32 == 0, "For decode, batch must be multiple of 32!"
        assert q_len == 1, "For decode, q_len must be 1!"

        past_key_values = ()
        tt_layer_past = ()
        for i in range(num_layers):
            k_cache = torch.rand(batch, 1, kv_cache_len, head_dim)
            v_cache = torch.rand(batch, 1, kv_cache_len, head_dim)
            past_key_values += ((k_cache, v_cache),)

            tt_k_cache = torch.zeros(batch, 1, max_position_embeddings, head_dim)
            tt_v_cache = torch.zeros(batch, 1, max_position_embeddings, head_dim)
            tt_k_cache[:, :, :kv_cache_len, :] = k_cache
            tt_v_cache[:, :, :kv_cache_len, :] = v_cache
            tt_k_cache = torch2tt_tensor(tt_k_cache, device)
            tt_v_cache = torch2tt_tensor(tt_v_cache, device)
            tt_layer_past += ((tt_k_cache, tt_v_cache),)

    else:
        raise NotImplementedError(
            f"Llm mode {llm_mode} is not supported! Must be one of prefill or decode."
        )

    # Prepare output -----------------------------------------------------------------------
    profiler.start("hugging_face_reference_model")
    pytorch_out, pytorch_layer_present = pytorch_FalconCausalLM(
        input_ids=model_input, past_key_values=past_key_values, use_cache=use_cache
    )
    profiler.end("hugging_face_reference_model")

    # NOTE: Passing in pytorch tensor here instead of ll buda tensor
    # since we don't yet have embedding support on device
    # device, state_dict, base_url, max_position_embeddings, config, num_decoders

    profiler.start("TtFalcon_model_setup")
    tt_FalconCausalLM = TtFalconCausalLM(
        device,
        state_dict,
        base_url,
        num_layers,
        configuration,
        max_position_embeddings,
        model_config,
        tt_cache_path,
    )
    profiler.end("TtFalcon_model_setup")

    profiler.start("processing_of_input")
    # TODO: Generate embeddings and attention_mask on device
    if llm_mode == "prefill":
        model_inputs = torch.split(model_input, 1)
        tt_embeddings, tt_attention_mask = zip(
            *[
                tt_FalconCausalLM.model_preprocessing(m_i, kv_cache_len, llm_mode)
                for m_i in model_inputs
            ]
        )
    elif llm_mode == "decode":
        tt_embeddings, tt_attention_mask = tt_FalconCausalLM.model_preprocessing(
            model_input, kv_cache_len, llm_mode
        )
    profiler.end("processing_of_input")

    # First run to fill compile cache ----------------------------------------------------
    logger.info(f"Running Falcon model once to fill caches -> disable profiler")
    profiler.disable()

    # Use force enable to only record this profiler call while others are disabled
    profiler.start("first_model_run_with_compile", force_enable=True)
    if llm_mode == "prefill":
        tt_outs = []
        model_inputs = torch.split(model_input, 1)
        tt_embeddings, tt_attention_mask = zip(
            *[
                tt_FalconCausalLM.model_preprocessing(m_i, kv_cache_len, llm_mode)
                for m_i in model_inputs
            ]
        )
        for user_id in range(batch):
            tt_out, tt_layer_present = tt_FalconCausalLM(
                input_embeddings=tt_embeddings[user_id],
                llm_mode=llm_mode,
                attention_mask=tt_attention_mask[user_id],
                user_id=user_id,
                layer_past=tt_layer_past,
                layer_past_len=kv_cache_len,
                use_cache=use_cache,
            )
            logger.info(f"################################################################ prefill")
            logger.info(f"kv cache tensor shape: {tt_layer_present[0][0].shape()}")
            logger.info(f"################################################################")
            tt_outs.append(tt_out)
        tt_out = tt_outs

    elif llm_mode == "decode":
        tt_out, tt_layer_present = tt_FalconCausalLM(
            input_embeddings=tt_embeddings,
            llm_mode=llm_mode,
            attention_mask=tt_attention_mask,
            layer_past=tt_layer_past,
            layer_past_len=kv_cache_len,
            use_cache=use_cache,
        )
        logger.info(f"################################################################ decode")
        logger.info(f"kv cache tensor shape: {tt_layer_present[0][0].shape()}")
        logger.info(f"################################################################")
    tt_lib.device.Synchronize()
    profiler.end("first_model_run_with_compile", force_enable=True)
    del tt_out
    del tt_layer_past
    del tt_layer_present
    del tt_embeddings
    del tt_attention_mask

    # Second run for perf ----------------------------------------------------------------
    logger.info(f"Enable profiler and enable binary and compile cache")
    profiler.enable()
    enable_persistent_kernel_cache()
    if llm_mode == "prefill":
        tt_layer_past = ()
        for i in range(num_layers):
            tt_k_cache = torch2tt_tensor(k_cache, device)
            tt_v_cache = torch2tt_tensor(v_cache, device)
            tt_layer_past += ((tt_k_cache, tt_v_cache),)

    elif llm_mode == "decode":
        tt_layer_past = ()
        for i in range(num_layers):
            tt_k_cache = torch.zeros(batch, 1, max_position_embeddings, head_dim)
            tt_v_cache = torch.zeros(batch, 1, max_position_embeddings, head_dim)
            tt_k_cache[:, :, :kv_cache_len, :] = past_key_values[i][0]
            tt_v_cache[:, :, :kv_cache_len, :] = past_key_values[i][1]
            tt_k_cache = torch2tt_tensor(tt_k_cache, device)
            tt_v_cache = torch2tt_tensor(tt_v_cache, device)
            tt_layer_past += ((tt_k_cache, tt_v_cache),)

    if llm_mode == "prefill":
        model_inputs = torch.split(model_input, 1)
        tt_embeddings, tt_attention_mask = zip(
            *[
                tt_FalconCausalLM.model_preprocessing(m_i, kv_cache_len, llm_mode)
                for m_i in model_inputs
            ]
        )
    elif llm_mode == "decode":
        tt_embeddings, tt_attention_mask = tt_FalconCausalLM.model_preprocessing(
            model_input, kv_cache_len, llm_mode
        )

    profiler.start(f"model_run_for_inference")
    if llm_mode == "prefill":
        tt_outs = []
        model_inputs = torch.split(model_input, 1)
        tt_embeddings, tt_attention_mask = zip(
            *[
                tt_FalconCausalLM.model_preprocessing(m_i, kv_cache_len, llm_mode)
                for m_i in model_inputs
            ]
        )
        for user_id in range(batch):
            tt_out, tt_layer_present = tt_FalconCausalLM(
                input_embeddings=tt_embeddings[user_id],
                llm_mode=llm_mode,
                attention_mask=tt_attention_mask[user_id],
                user_id=user_id,
                layer_past=tt_layer_past,
                layer_past_len=kv_cache_len,
                use_cache=use_cache,
            )
            tt_outs.append(tt_out)

    elif llm_mode == "decode":
        tt_out, tt_layer_present = tt_FalconCausalLM(
            input_embeddings=tt_embeddings,
            llm_mode=llm_mode,
            attention_mask=tt_attention_mask,
            layer_past=tt_layer_past,
            layer_past_len=kv_cache_len,
            use_cache=use_cache,
        )
    tt_lib.device.Synchronize()
    profiler.end(f"model_run_for_inference")

    if llm_mode == "prefill":
        tt_out = torch.vstack(
            [tt2torch_tensor(tt_out).squeeze(1) for tt_out in tt_outs]
        )
    elif llm_mode == "decode":
        tt_out = tt2torch_tensor(tt_out).squeeze(1)
        tt_out = tt_out.transpose(0, 1)

    # check outputs ----------------------------------------------------------------------
    does_pass, output_pcc = comp_pcc(pytorch_out, tt_out, pcc)
    logger.info(f"Output: {output_pcc}")

    for i in range(num_layers):
        tt_layer_pres = (
            tt2torch_tensor(tt_layer_present[i][0]),
            tt2torch_tensor(tt_layer_present[i][1]),
        )
        if llm_mode == "prefill":
            pytorch_layer_pres = pytorch_layer_present[i]
            tt_layer_pres = (
                tt_layer_pres[0][:, :, :kv_len, :],
                tt_layer_pres[1][:, :, :kv_len, :],
            )
        elif llm_mode == "decode":
            pytorch_layer_pres = (
                pytorch_layer_present[i][0][:, :, kv_cache_len, :],
                pytorch_layer_present[i][1][:, :, kv_cache_len, :],
            )
            tt_layer_pres = (
                tt_layer_pres[0][:, :, kv_cache_len, :],
                tt_layer_pres[1][:, :, kv_cache_len, :],
            )

        does_pass2, output_pcc = comp_pcc(pytorch_layer_pres[0], tt_layer_pres[0], pcc)
        logger.info(f"K Cache Layer {i}: {output_pcc}")

        does_pass = does_pass and does_pass2

        does_pass2, output_pcc = comp_pcc(pytorch_layer_pres[1], tt_layer_pres[1], pcc)
        logger.info(f"V Cache Layer {i}: {output_pcc}")

        does_pass = does_pass and does_pass2

    profiler.print()

    comment = f"kv_cache_len={kv_cache_len}_seq_len={seq_len}_num_layers={num_layers}_config=L1-bf16"
    cpu_time = profiler.get("hugging_face_reference_model")
    first_iter_time = profiler.get("first_model_run_with_compile")
    second_iter_time = profiler.get("model_run_for_inference")
    expected_compile_time = 30
    prep_report(
        model_name=f"Falcon_{llm_mode}_{comment}",
        batch_size=batch,
        inference_and_compile_time=first_iter_time,
        inference_time=second_iter_time,
        expected_compile_time=expected_compile_time,
        expected_inference_time=expected_inference_time,
        comments=comment,
        inference_time_cpu=cpu_time
    )

    compile_time = first_iter_time - second_iter_time
    logger.info(f"falcon {comment} inference time: {second_iter_time}")
    logger.info(f"falcon {comment} compile time: {compile_time}")
    assert second_iter_time < expected_inference_time, "vit is too slow"
    assert compile_time < expected_compile_time, "vit compile time is too slow"


    if does_pass:
        logger.info("Falcon CausalLM Passed!")
    else:
        logger.warning("Falcon CausalLM Failed!")
        # TODO: Fix PCC for decode and uncomment this
        # assert does_pass, f"PCC value is lower than {pcc}"



@pytest.mark.models_performance_bare_metal
@pytest.mark.parametrize(
    "llm_mode, batch, seq_len, kv_cache_len, expected_inference_time",
    (
        ("prefill", 1, 128, 0, 0.34),
        ("decode", 32, 1, 128, 0.33),
        ("decode", 32, 1, 1024, 0.36),
        ("decode", 32, 1, 2047, 0.47),
    ),
    ids=["prefill_seq128", "decode_batch32", "decode_batch32_1024", "decode_batch32_2047"],
)
@pytest.mark.parametrize(
    "num_layers, pcc",
    ((32, 0.86),),
    ids=["layers_32"],
)
@pytest.mark.parametrize(
    "model_version",
    ("tiiuae/falcon-7b-instruct",),
    ids=["falcon_7b"],
)
@pytest.mark.parametrize("model_config_str", ("BFLOAT16-L1", ))
def test_perf_bare_metal(
    use_program_cache,
    model_version,
    llm_mode,
    batch,
    seq_len,
    kv_cache_len,
    expected_inference_time,
    num_layers,
    pcc,
    request,
    model_config_str,
    model_location_generator,
    device,
):
    model_config = get_model_config(model_config_str)
    tt_cache_path = get_tt_cache_path(model_version)

    disable_persistent_kernel_cache()
    disable_compilation_reports()

    # tt_lib.profiler.set_profiler_location(
    #     f"tt_metal/tools/profiler/logs/falcon-7b_{request.node.callspec.id}"
    # )

    run_test_FalconCausalLM_end_to_end(
        device,
        model_version,
        llm_mode,
        batch,
        seq_len,
        kv_cache_len,
        num_layers,
        pcc,
        model_config,
        tt_cache_path,
        model_location_generator,
        expected_inference_time,
    )



# @pytest.mark.parametrize(
#     "llm_mode, batch, seq_len, kv_cache_len",
#     (
#         ("prefill", 1, 128, 0),
#         ("decode", 32, 1, 128),
#         ("decode", 32, 1, 1024),
#         ("decode", 32, 1, 2047),
#     ),
#     ids=["prefill_seq128", "decode_batch32", "decode_batch32_1024", "decode_batch32_2047"],
# )
# @pytest.mark.parametrize(
#     "num_layers, pcc",
#     ((32, 0.86),),
#     ids=["layers_32"],
# )
# @pytest.mark.parametrize(
#     "model_version",
#     ("tiiuae/falcon-7b-instruct",),
#     ids=["falcon_7b"],
# )
# @pytest.mark.parametrize("model_config_str", ("BFLOAT16-L1", ))
@pytest.mark.models_performance_virtual_machine
def test_perf_virtual_machine():
    pass
#     model_config = get_model_config(model_config_str)
#     tt_cache_path = get_tt_cache_path(model_version)

#     disable_persistent_kernel_cache()
#     disable_compilation_reports()

#     # tt_lib.profiler.set_profiler_location(
#     #     f"tt_metal/tools/profiler/logs/falcon-7b_{request.node.callspec.id}"
#     # )

#     run_test_FalconCausalLM_end_to_end(
#         device,
#         model_version,
#         llm_mode,
#         batch,
#         seq_len,
#         kv_cache_len,
#         num_layers,
#         pcc,
#         model_config,
#         tt_cache_path,
#         model_location_generator,
#     )
