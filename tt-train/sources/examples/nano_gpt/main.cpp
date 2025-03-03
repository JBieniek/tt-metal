// SPDX-FileCopyrightText: (c) 2024 Tenstorrent AI ULC
//
// SPDX-License-Identifier: Apache-2.0

#include <CLI/CLI.hpp>
#include <chrono>
#include <core/ttnn_all_includes.hpp>
#include <csignal>
#include <cstdint>
#include <ttnn/tensor/tensor.hpp>
#include <wandbcpp.hpp>

#include "autograd/tensor.hpp"
#include "core/clip_grad_norm.hpp"
#include "core/distributed/distributed.hpp"
#include "core/tt_tensor_utils.hpp"
#include "datasets/dataloader.hpp"
#include "datasets/in_memory_token_dataset.hpp"
#include "datasets/utils.hpp"
#include "models/distributed/gpt2.hpp"
#include "models/gpt2.hpp"
#include "ops/binary_ops.hpp"
#include "ops/losses.hpp"
#include "optimizers/adamw.hpp"
#include "tokenizers/bpe_tokenizer.hpp"
#include "tokenizers/char_tokenizer.hpp"
#include "utils.hpp"

/* WANDB BLocks this signal.
 Control+C didn't work.
*/
void signal_handler(int signum) {
    std::cout << "\nInterrupt signal (" << signum << ") received.\n";
    wandbcpp::finish();
    exit(signum);
}

using Model = std::variant<
    std::shared_ptr<ttml::models::gpt2::Transformer>,
    std::shared_ptr<ttml::models::distributed::gpt2::DistributedTransformer>>;

void model_to_eval(Model &model) {
    std::visit([](auto &model) { model->eval(); }, model);
}

void model_to_train(Model &model) {
    std::visit([](auto &model) { model->train(); }, model);
}

ttml::autograd::TensorPtr run_model(
    Model &model, const ttml::autograd::TensorPtr &data, const ttml::autograd::TensorPtr &mask) {
    return std::visit([&data, &mask](auto &model) { return (*model)(data, mask); }, model);
}

ttml::serialization::NamedParameters get_model_parameters(Model &model) {
    return std::visit([](auto &model) { return model->parameters(); }, model);
}

using ttml::autograd::TensorPtr;

using DatasetSample = std::pair<std::span<const uint32_t>, std::span<const uint32_t>>;
// tokens, targets, masks
using BatchType = std::tuple<TensorPtr, TensorPtr, TensorPtr>;
using DataLoader = ttml::datasets::DataLoader<
    ttml::datasets::InMemoryTokenDataset,
    std::function<BatchType(std::vector<DatasetSample> &&samples)>,
    BatchType>;

uint32_t sample(std::span<const float> log_softmax) {
    auto probabilities_vector = std::vector<float>(log_softmax.size());
    std::transform(log_softmax.begin(), log_softmax.end(), probabilities_vector.begin(), [](float value) {
        return std::exp(value);
    });
    auto distribution = std::discrete_distribution<uint32_t>(probabilities_vector.begin(), probabilities_vector.end());
    return distribution(ttml::autograd::ctx().get_generator());
}

inline void apply_repetition_penalty(
    std::vector<float> &logits, const std::vector<uint32_t> &history, float repetition_penalty) {
    if (repetition_penalty <= 1.0F) {
        return;  // no penalty
    }
    for (auto token_id : history) {
        float &val = logits[token_id];
        if (val > 0.0F) {
            val /= repetition_penalty;
        } else {
            val *= repetition_penalty;
        }
    }
}

inline void top_k_filter(std::vector<float> &logits, int top_k) {
    if (top_k <= 0 || static_cast<size_t>(top_k) >= logits.size()) {
        return;
    }
    std::vector<float> copy = logits;
    std::nth_element(copy.begin(), copy.end() - top_k, copy.end());
    float cutoff = *(copy.end() - top_k);

    for (auto &val : logits) {
        if (val < cutoff) {
            val = -std::numeric_limits<float>::infinity();
        }
    }
}

inline void top_p_filter(std::vector<float> &logits, float top_p) {
    if (top_p <= 0.0F || top_p >= 1.0F) {
        return;  // no filtering
    }

    std::vector<float> probs(logits.size());
    for (size_t i = 0; i < logits.size(); i++) {
        probs[i] = std::exp(logits[i]);
    }
    float sum = 0.0F;
    for (auto x : probs) {
        sum += x;
    }
    // argsort
    std::vector<size_t> indices(logits.size());
    for (size_t i = 0; i < indices.size(); ++i) {
        indices[i] = i;
    }
    std::sort(indices.begin(), indices.end(), [&](size_t a, size_t b) {
        return probs[a] > probs[b];  // descending by prob
    });
    // smallest set of tokens whose sum >= top_p
    float cum_prob = 0.0F;
    size_t cutoff_idx = 0;
    for (size_t rank = 0; rank < indices.size(); ++rank) {
        auto idx = indices[rank];
        cum_prob += probs[idx] / sum;
        if (cum_prob > top_p) {
            cutoff_idx = rank;
            break;
        }
    }
    for (size_t rank = cutoff_idx + 1; rank < indices.size(); ++rank) {
        auto idx = indices[rank];
        logits[idx] = -std::numeric_limits<float>::infinity();
    }
}

inline uint32_t sample_with_strategy(
    std::span<float> logits_span,
    const std::vector<uint32_t> &history,
    float temperature,
    float repetition_penalty,
    int top_k,
    float top_p) {
    std::vector<float> logits(logits_span.begin(), logits_span.end());
    size_t vocab_size = logits.size();

    apply_repetition_penalty(logits, history, repetition_penalty);

    if (temperature > 0.0F && std::fabs(temperature - 1.0F) > 1e-6f) {
        for (auto &val : logits) {
            val /= temperature;
        }
    }
    auto max_it = std::max_element(logits.begin(), logits.end());
    float max_val = (max_it != logits.end()) ? *max_it : 0.0F;
    for (auto &val : logits) {
        val -= max_val;
    }

    // 4) top-k filter
    top_k_filter(logits, top_k);

    // 5) top-p (nucleus) filter
    top_p_filter(logits, top_p);

    // 6) Convert to probabilities + sample
    //    Recompute stable exponent after filtering
    float sum_exp = 0.0F;
    for (auto val : logits) {
        if (val > -std::numeric_limits<float>::infinity()) {
            sum_exp += std::exp(val);
        }
    }

    auto &rng = ttml::autograd::ctx().get_generator();
    std::uniform_real_distribution<float> dist(0.0F, 1.0F);

    float r = dist(rng);
    float cum = 0.0F;
    for (size_t i = 0; i < vocab_size; ++i) {
        if (logits[i] == -std::numeric_limits<float>::infinity()) {
            continue;
        }
        float p = std::exp(logits[i]) / sum_exp;
        cum += p;
        if (r <= cum) {
            return static_cast<uint32_t>(i);
        }
    }
    // Fallback
    return static_cast<uint32_t>(vocab_size - 1);
}

template <typename Tokenizer>
void generate(
    Model &model,
    const Tokenizer &tokenizer,
    uint32_t max_sequence_length,
    uint32_t num_heads,
    uint32_t tokens_to_generate = 1024U,
    bool enable_tp = false,
    // Additional sampling params:
    float temperature = 1.0F,
    float repetition_penalty = 1.0F,
    int top_k = -1,
    float top_p = 1.0F) {
    model_to_eval(model);

    std::string prompt;
    fmt::print("Enter a prompt: ");
    std::getline(std::cin, prompt);
    if (prompt.empty()) {
        prompt = "\n";
    }

    // Encode the prompt
    auto prompt_tokens = tokenizer.encode(prompt);

    // In case you need a pad token
    auto pad_token_id = 0U;
    auto original_vocab_size = tokenizer.get_vocab_size();
    auto *device = &ttml::autograd::ctx().get_device();
    auto num_devices = static_cast<uint32_t>(device->num_devices());
    // this is workaround for tensor parallel case, we need to have vocab size divisible by 32 per device
    auto vocab_size = round_up_to_tile(original_vocab_size, (enable_tp ? num_devices : 1U) * 32U);

    // Build mask (causal) for attention
    std::vector<float> mask;
    mask.reserve(static_cast<size_t>(max_sequence_length * max_sequence_length));
    for (uint32_t i = 0; i < max_sequence_length; ++i) {
        for (uint32_t j = 0; j < max_sequence_length; ++j) {
            mask.push_back(i >= j ? 1.0F : 0.0F);
        }
    }

    auto mask_tensor = ttml::autograd::create_tensor(ttml::core::from_vector(
        mask, ttml::core::create_shape({1, 1, max_sequence_length, max_sequence_length}), device));

    // Prepare a padded buffer for the prompt
    std::vector<uint32_t> prompt_tokens_padded(max_sequence_length, pad_token_id);

    fmt::print("Generated text:\n");
    fmt::print("*******************\n");
    fmt::print("{}", prompt);

    // Main token generation loop
    for (uint32_t token_idx = 0; token_idx < tokens_to_generate; ++token_idx) {
        // Possibly truncate the prompt if it exceeds max_sequence_length
        uint32_t start_idx = 0;
        if (prompt_tokens.size() > max_sequence_length) {
            start_idx = static_cast<uint32_t>(prompt_tokens.size() - max_sequence_length);
        }

        // Fill padded array
        for (uint32_t i = 0; i < max_sequence_length; ++i) {
            prompt_tokens_padded[i] = pad_token_id;
        }
        for (uint32_t i = start_idx; i < prompt_tokens.size(); ++i) {
            prompt_tokens_padded[i - start_idx] = prompt_tokens[i];
        }
        auto prompt_tokens_padded_size = static_cast<uint32_t>(prompt_tokens_padded.size());
        auto prompt_tensor = ttml::autograd::create_tensor(ttml::core::from_vector<uint32_t, ttnn::DataType::UINT32>(
            prompt_tokens_padded,
            ttml::core::create_shape({1, 1, 1, prompt_tokens_padded_size}),
            device,
            ttnn::Layout::ROW_MAJOR));

        // Forward pass
        // 'output' shape is presumably [batch=1, 1, seq_len, vocab_size] or something similar
        auto output = run_model(model, prompt_tensor, mask_tensor);

        // Convert last position's logits to a std::vector
        auto output_vector = ttml::core::to_vector(output->get_value());

        // The index of the last token in the "effective" input
        // (Your indexing may vary depending on how your model outputs are shaped)
        uint32_t predicted_token_idx =
            (prompt_tokens.size() > max_sequence_length) ? (max_sequence_length - 1U) : (prompt_tokens.size() - 1U);

        // Extract the logits for the last token
        // (Assuming output is flattened so that token dimension is first,
        //  then you'd do: offset = predicted_token_idx * vocab_size)
        size_t offset = static_cast<size_t>(predicted_token_idx) * vocab_size;
        auto logits_ptr = output_vector.data() + offset;

        // Now we do advanced sampling from these logits
        uint32_t next_token_id = sample_with_strategy(
            std::span<float>(logits_ptr, original_vocab_size),
            prompt_tokens,  // entire history for repetition penalty
            temperature,
            repetition_penalty,
            top_k,
            top_p);

        // Append the new token
        prompt_tokens.push_back(next_token_id);

        // Decode and print
        fmt::print("{}", tokenizer.decode({next_token_id}));

        // Reset the autograd graph if needed
        ttml::autograd::ctx().reset_graph();
    }

    fmt::print("\n*******************\n");
    model_to_train(model);  // return model to train mode if needed
}

struct EvalConfig {
    float repetition_penalty = 1.0F;
    float temperature = 1.0F;
    int top_k = -1;
    float top_p = 1.0F;
};

EvalConfig parse_eval_config(const YAML::Node &yaml_config) {
    EvalConfig config;
    if (!yaml_config["eval_config"]) {
        return config;
    }
    auto eval_config = yaml_config["eval_config"];
    config.repetition_penalty = eval_config["repetition_penalty"].as<float>(config.repetition_penalty);
    config.temperature = eval_config["temperature"].as<float>(config.temperature);
    config.top_k = eval_config["top_k"].as<int>(config.top_k);
    config.top_p = eval_config["top_p"].as<float>(config.top_p);
    return config;
}

struct TrainingConfig {
    std::string project_name;
    uint32_t seed = 5489U;
    uint32_t model_save_interval = 500;
    uint32_t batch_size = 64;
    uint32_t num_epochs = 1;
    uint32_t max_steps = 5000;
    float learning_rate = 3e-4F;
    float weight_decay = 1e-2F;
    bool use_moreh_adamw = false;
    // works only for AdamW
    bool use_kahan_summation = false;
    // accumulate batches for gradient update
    uint32_t gradient_accumulation_steps = 1;
    std::string model_path;
    std::string data_path;
    std::string tokenizer_type = "char";
    std::string scheduler_type = "identity";
    bool use_clip_grad_norm = false;
    float clip_grad_norm_max_norm = 1.0F;
    ttml::models::gpt2::TransformerConfig transformer_config;
};

TrainingConfig parse_config(const YAML::Node &yaml_config) {
    TrainingConfig config;
    auto training_config = yaml_config["training_config"];
    config.project_name = training_config["project_name"].as<std::string>("tt_train_nano_gpt");
    config.seed = training_config["seed"].as<uint32_t>();
    config.model_save_interval = training_config["model_save_interval"].as<uint32_t>();
    config.batch_size = training_config["batch_size"].as<uint32_t>();
    config.num_epochs = training_config["num_epochs"].as<uint32_t>();
    config.max_steps = training_config["max_steps"].as<uint32_t>();
    config.learning_rate = training_config["learning_rate"].as<float>();
    config.weight_decay = training_config["weight_decay"].as<float>();
    config.use_moreh_adamw = training_config["use_moreh_adamw"].as<bool>(config.use_moreh_adamw);
    config.use_kahan_summation = training_config["use_kahan_summation"].as<bool>(config.use_kahan_summation);
    config.gradient_accumulation_steps =
        training_config["gradient_accumulation_steps"].as<uint32_t>(config.gradient_accumulation_steps);
    config.model_path = training_config["model_path"].as<std::string>("");
    config.data_path = training_config["data_path"].as<std::string>(std::string(DATA_FOLDER) + "/shakespeare.txt");
    config.tokenizer_type = training_config["tokenizer_type"].as<std::string>(config.tokenizer_type);
    config.scheduler_type = training_config["scheduler_type"].as<std::string>(config.scheduler_type);
    config.use_clip_grad_norm = training_config["use_clip_grad_norm"].as<bool>(config.use_clip_grad_norm);
    config.clip_grad_norm_max_norm =
        training_config["clip_grad_norm_max_norm"].as<float>(config.clip_grad_norm_max_norm);

    config.transformer_config = ttml::models::gpt2::read_config(training_config["transformer_config"]);
    return config;
}

const std::unordered_map<
    std::string,
    std::function<std::unique_ptr<ttml::schedulers::LRSchedulerBase>(ttml::optimizers::OptimizerBase *, size_t)>>
    schedulers = {{"identity", create_idendity_scheduler}, {"warmup_linear", create_warmup_with_linear_scheduler}};

int main(int argc, char **argv) {
    auto start_timer = std::chrono::high_resolution_clock::now();
    CLI::App app{"NanoGPT Example"};
    argv = app.ensure_utf8(argv);

    std::string config_name = std::string(CONFIGS_FOLDER) + "/training_shakespear_nanogpt.yaml";
    std::string run_name = "";
    bool is_eval = false;
    bool add_time_to_name = true;
    bool enable_wandb = true;
    bool ddp = false;
    bool enable_tp = false;
    app.add_option("-c,--config", config_name, "Yaml Config name")->default_val(config_name);
    app.add_option("-e,--eval", is_eval, "Is evaluation")->default_val(is_eval);
    app.add_option("-t,--add_time_to_name", add_time_to_name, "Add time to run name")->default_val(add_time_to_name);
    app.add_option("-w,--wandb", enable_wandb, "Enable wandb logging")->default_val(enable_wandb);
    app.add_option("-d,--ddp", ddp, "Enable DDP")->default_val(ddp);
    app.add_option("-p,--tp", enable_tp, "Enable TP")->default_val(enable_tp);
    app.add_option("-n,--name", run_name, "Run name")->default_val(run_name);
    CLI11_PARSE(app, argc, argv);

    if (ddp && enable_tp) {
        throw std::logic_error("DDP and TP cannot be enabled at the same time. Disable DDP or TP.");
    }

    initialize_device(ddp, enable_tp);

    if (enable_wandb) {
        auto result = signal(SIGINT, signal_handler);
        if (result == SIG_ERR) {
            std::cerr << "Failed to set signal handler\n";
            return -1;
        }
    }

    auto yaml_config = YAML::LoadFile(config_name);
    TrainingConfig config = parse_config(yaml_config);
    EvalConfig eval_config = parse_eval_config(yaml_config);

    if (enable_tp) {
        if (!config.model_path.empty()) {
            throw std::runtime_error("Save and load is not supported with Tensor Parallel model");
        }

        if (is_eval) {
            throw std::runtime_error("Evaluation is not supported with Tensor Parallel model");
        }
    }

    if (enable_wandb) {
        wandbcpp::init({.project = config.project_name, .name = generate_run_name(run_name, config, add_time_to_name)});
        wandbcpp::update_config({
            {"model", "transformer"},
            {"num_heads", static_cast<int>(config.transformer_config.num_heads)},
            {"embedding_dim", static_cast<int>(config.transformer_config.embedding_dim)},
            {"num_blocks", static_cast<int>(config.transformer_config.num_blocks)},
            {"dropout_prob", config.transformer_config.dropout_prob},
            {"learning_rate", config.learning_rate},
            {"weight_decay", config.weight_decay},
            {"batch_size", static_cast<int>(config.batch_size)},
            {"sequence_length", static_cast<int>(config.transformer_config.max_sequence_length)},
            {"max_steps", static_cast<int>(config.max_steps)},
            {"seed", static_cast<int>(config.seed)},
            {"tokenizer_type", config.tokenizer_type},
            {"use_kahan_summation", config.use_kahan_summation},
            {"gradient_accumulation_steps", static_cast<int>(config.gradient_accumulation_steps)},
            {"positional_embedding_type",
             config.transformer_config.positional_embedding_type ==
                     ttml::models::gpt2::PositionalEmbeddingType::Trainable
                 ? "trainable"
                 : "fixed"},
            {"scheduler_type", config.scheduler_type},
            {"using_clip_grad_norm", config.use_clip_grad_norm},
            {"clip_grad_norm_max_norm", config.clip_grad_norm_max_norm},
        });
    }

    // set seed
    ttml::autograd::ctx().set_seed(config.seed);
    auto schedule_func = schedulers.at(config.scheduler_type);

    std::string text;
    try {
        text = read_file_to_str(config.data_path);
    } catch (const std::exception &e) {
        std::cerr << e.what() << std::endl;
        return -1;
    }
    fmt::print("Max steps {}\n", config.max_steps);
    fmt::print("Batch size {}\n", config.batch_size);
    fmt::print("Gradient accumulation steps {}\n", config.gradient_accumulation_steps);
    fmt::print("Total batch size {}\n", config.batch_size * config.gradient_accumulation_steps);
    fmt::print("Scheduler type {}\n", config.scheduler_type);
    fmt::print("Seed {}\n", ttml::autograd::ctx().get_seed());
    auto sequence_length = config.transformer_config.max_sequence_length;

    auto create_dataset_and_tokenizer = [](const auto &text, const auto sequence_length, const auto &tokenizer_type) {
        if (tokenizer_type == "char") {
            return ttml::datasets::create_in_memory_token_dataset<ttml::tokenizers::CharTokenizer>(
                text, sequence_length);
        } else if (tokenizer_type == "bpe") {
            return ttml::datasets::create_in_memory_token_dataset<ttml::tokenizers::BPETokenizer>(
                text, sequence_length);
        } else {
            throw std::runtime_error("Unknown tokenizer type: " + tokenizer_type);
        }
    };

    auto [dataset, tokenizer] = create_dataset_and_tokenizer(text, sequence_length, config.tokenizer_type);
    fmt::print("Dataset size: {}\n", dataset.get_size());
    fmt::print("Vocab size: {}\n", tokenizer->get_vocab_size());
    fmt::print("Tokenizer type: {}\n", config.tokenizer_type);

    auto *device = &ttml::autograd::ctx().get_device();
    device->enable_program_cache();

    // disable for now, unexpected freezes and crashes
    // device->enable_async(true);

    struct CachedHostData {
        std::vector<uint32_t> data;
        std::vector<int32_t> targets;
        ttml::autograd::TensorPtr masks_tensor;
    };
    CachedHostData cached_data;
    std::vector<float> mask;
    auto num_heads = config.transformer_config.num_heads;
    mask.reserve(sequence_length * sequence_length);
    for (int i = 0; i < sequence_length; ++i) {
        for (int j = 0; j < sequence_length; ++j) {
            mask.push_back(i >= j ? 1.0F : 0.0F);
        }
    }
    cached_data.masks_tensor = ttml::autograd::create_tensor(
        ttml::core::from_vector(mask, ttml::core::create_shape({1, 1, sequence_length, sequence_length}), device));

    std::function<BatchType(std::vector<DatasetSample> && samples)> collate_fn =
        [sequence_length, num_heads, device, &cached_data, ddp](std::vector<DatasetSample> &&samples) {
            auto start_timer = std::chrono::high_resolution_clock::now();
            const uint32_t batch_size = samples.size();
            std::vector<uint32_t> &data = cached_data.data;
            std::vector<int32_t> &targets = cached_data.targets;

            data.clear();
            targets.clear();

            data.reserve((size_t)batch_size * sequence_length);
            targets.reserve((size_t)batch_size * sequence_length);
            for (auto &[features, target_span] : samples) {
                std::copy(features.begin(), features.end(), std::back_inserter(data));
                std::copy(target_span.begin(), target_span.end(), std::back_inserter(targets));
            }
            auto end_timer = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_timer - start_timer).count();
            fmt::print("dataloader host only step time {} ms\n", (double)duration / 1000.);

            auto create_data_and_targets = [&]() -> std::tuple<TensorPtr, TensorPtr> {
                if (ddp) {
                    auto data_xtensor = xt::adapt(data, {batch_size, 1U, 1U, sequence_length});
                    auto data_composer = ttml::core::ShardXTensorToMesh<uint32_t>(device->shape(), 0);
                    auto data_tensor =
                        ttml::autograd::create_tensor(ttml::core::from_xtensor<uint32_t, ttnn::DataType::UINT32>(
                            data_xtensor, device, data_composer, ttnn::Layout::ROW_MAJOR));

                    auto targets_xtensor = xt::adapt(targets, {batch_size * sequence_length});
                    auto targets_composer = ttml::core::ShardXTensorToMesh<int32_t>(device->shape(), 0);
                    auto targets_tt_tensor = ttml::core::from_xtensor<int32_t, ttnn::DataType::INT32>(
                        targets_xtensor, device, targets_composer);
                    auto targets_tensor = ttml::autograd::create_tensor(targets_tt_tensor);
                    return {data_tensor, targets_tensor};
                }

                auto data_tensor =
                    ttml::autograd::create_tensor(ttml::core::from_vector<uint32_t, ttnn::DataType::UINT32>(
                        data,
                        ttml::core::create_shape({batch_size, 1, 1, sequence_length}),
                        device,
                        ttnn::Layout::ROW_MAJOR));
                auto targets_tensor =
                    ttml::autograd::create_tensor(ttml::core::from_vector<int32_t, ttnn::DataType::INT32>(
                        targets, ttnn::Shape({batch_size * sequence_length}), device));
                return {data_tensor, targets_tensor};
            };

            auto [data_tensor, targets_tensor] = create_data_and_targets();
            end_timer = std::chrono::high_resolution_clock::now();
            duration = std::chrono::duration_cast<std::chrono::microseconds>(end_timer - start_timer).count();
            fmt::print("dataloader step time {} ms\n", (double)duration / 1000.);
            return std::make_tuple(data_tensor, targets_tensor, cached_data.masks_tensor);
        };

    LossAverageMeter loss_meter;
    auto train_dataloader = DataLoader(dataset, /* batch_size */ config.batch_size, /* shuffle */ true, collate_fn);

    fmt::print("Overriding vocab size to be divisible by 32\n");
    auto num_devices = static_cast<uint32_t>(device->num_devices());
    // this is workaround for tensor parallel case, we need to have vocab size divisible by 32 per device
    config.transformer_config.vocab_size =
        round_up_to_tile(tokenizer->get_vocab_size(), (enable_tp ? num_devices : 1U) * 32U);

    Model model;
    if (enable_tp) {
        model = ttml::models::distributed::gpt2::create(config.transformer_config);
    } else {
        model = ttml::models::gpt2::create(config.transformer_config);
    }

    auto adamw_params = ttml::optimizers::AdamWConfig();
    adamw_params.lr = config.learning_rate;
    adamw_params.weight_decay = config.weight_decay;
    adamw_params.use_kahan_summation = config.use_kahan_summation;
    fmt::print("AdamW configuration:\n");
    fmt::print("    Learning rate: {}\n", adamw_params.lr);
    fmt::print("    Weight decay: {}\n", adamw_params.weight_decay);
    fmt::print("    Use Kahan summation: {}\n", adamw_params.use_kahan_summation);
    auto select_optimizer = [&model,
                             &adamw_params](bool use_moreh_adamw) -> std::unique_ptr<ttml::optimizers::OptimizerBase> {
        if (use_moreh_adamw) {
            return std::make_unique<ttml::optimizers::MorehAdamW>(get_model_parameters(model), adamw_params);
        } else {
            return std::make_unique<ttml::optimizers::AdamW>(get_model_parameters(model), adamw_params);
        }
    };

    auto optimizer = select_optimizer(config.use_moreh_adamw);
    auto scheduler = schedule_func(optimizer.get(), config.max_steps);
    if (!config.model_path.empty() && std::filesystem::exists(config.model_path)) {
        fmt::print("Loading model from {}\n", config.model_path);
        load_training_state(config.model_path, model, scheduler, "transformer", "adamw");
        fmt::print("Model loaded after {} steps\n", optimizer->get_steps());
    }

    if (is_eval) {
        fmt::print("\nEvaluation started\n");
        for (;;) {
            generate(
                model,
                *tokenizer,
                config.transformer_config.max_sequence_length,
                num_heads,
                sequence_length,
                enable_tp,
                eval_config.temperature,
                eval_config.repetition_penalty,
                eval_config.top_k,
                eval_config.top_p);
        }
        fmt::print("\nEvaluation finished\n");
        return 0;
    }

    auto get_samples_count = [&config](uint32_t global_step) {
        return global_step * config.batch_size * config.gradient_accumulation_steps;
    };

    auto get_loss_value = [device](const TensorPtr &loss) {
        ttml::core::MeshToXTensorVariant<float> composer = ttml::core::VectorMeshToXTensor<float>(device->shape());
        auto loss_xtensors = ttml::core::to_xtensor(loss->get_value(), composer);
        // sum of loss xtensors
        float loss_float =
            std::accumulate(loss_xtensors.begin(), loss_xtensors.end(), 0.0F, [](float acc, auto &xtensor) {
                return acc + xtensor(0);
            });

        return loss_float / static_cast<float>(loss_xtensors.size());
    };

    const uint32_t num_epochs = config.num_epochs;
    auto gradient_accumulator_helper = GradientAccumulator(config.gradient_accumulation_steps);
    for (uint32_t epoch = 0; epoch < num_epochs; ++epoch) {
        for (auto [features, target, masks] : train_dataloader) {
            auto start_timer = std::chrono::high_resolution_clock::now();
            if (gradient_accumulator_helper.should_zero_grad()) {
                optimizer->zero_grad();
            }
            auto output = run_model(model, features, masks);
            auto loss = ttml::ops::nll_loss(output, target);
            loss = gradient_accumulator_helper.scale(loss);
            float loss_float = get_loss_value(loss);

            loss->backward();
            ttml::autograd::ctx().reset_graph();

            auto samples = features->get_value().get_logical_shape()[0];
            gradient_accumulator_helper.update(loss_float, samples);

            if (gradient_accumulator_helper.should_step()) {
                // synchronize gradients for multi-device case, no-op if single device
                auto parameters = get_model_parameters(model);
                if (!enable_tp) {
                    ttml::core::distributed::synchronize_parameters(parameters);
                }

                if (config.use_clip_grad_norm) {
                    if (enable_tp) {
                        throw std::logic_error("Clip grad norm is not supported with TP");
                    }
                    ttml::core::clip_grad_norm(parameters, config.clip_grad_norm_max_norm);
                }
                optimizer->step();
                scheduler->step();
                auto global_step = optimizer->get_steps();
                fmt::print("Step: {}, Loss: {}\n", global_step, gradient_accumulator_helper.average_loss());
                loss_meter.update(gradient_accumulator_helper.average_loss());

                if (enable_wandb && global_step % 10 == 0) {
                    wandbcpp::log(
                        {{"Step", (int)global_step},
                         {"Samples", (int)get_samples_count(global_step)},
                         {"Loss", loss_meter.average()},
                         {"Learning rate", optimizer->get_lr()}});
                    loss_meter.reset();
                }
                if (!config.model_path.empty() && global_step % config.model_save_interval == 0) {
                    save_training_state(config.model_path, model, scheduler, "transformer", "adamw");
                }

                if (global_step >= config.max_steps) {
                    break;
                }

                gradient_accumulator_helper.reset();
            }
            auto end_timer = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_timer - start_timer).count();
            fmt::print(
                "Full step time {} ms, cache entries: {}\n",
                (double)duration / 1000,
                device->num_program_cache_entries());
        }
        if (optimizer->get_steps() >= config.max_steps) {
            break;
        }
    }

    if (!config.model_path.empty()) {
        save_training_state(config.model_path, model, scheduler, "transformer", "adamw");
    }

    auto end_timer = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_timer - start_timer).count();
    fmt::print(
        "{} Steps training time: {} s, cache entries: {}\n",
        config.max_steps,
        (double)duration / 1000000.,
        device->num_program_cache_entries());

    if (enable_wandb) {
        wandbcpp::finish();
    }
    return 0;
}
