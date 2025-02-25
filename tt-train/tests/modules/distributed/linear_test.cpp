// SPDX-FileCopyrightText: (c) 2025 Tenstorrent AI ULC
//
// SPDX-License-Identifier: Apache-2.0

#include "modules/distributed/linear.hpp"

#include <gtest/gtest.h>
#include <umd/device/tt_cluster_descriptor.h>

#include <core/ttnn_all_includes.hpp>
#include <core/xtensor_utils.hpp>

#include "autograd/auto_context.hpp"
#include "core/distributed_mapping.hpp"
#include "core/tt_tensor_utils.hpp"

namespace {

auto check_board_is_n300() {
    return tt_ClusterDescriptor::create()->get_board_type(0) == BoardType::N300;
}

ttml::autograd::TensorPtr get_parameter(auto& parameters, const std::string& name_substring) {
    for (const auto& [name, parameter] : parameters) {
        if (name.find(name_substring) != std::string::npos) {
            return parameter;
        }
    }
    throw std::logic_error(fmt::format("Parameter for a given name substring {} not found", name_substring));
}

}  // namespace

class N300TensorParallelLinearTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!check_board_is_n300()) {
            GTEST_SKIP() << "Skipping N300 specific tests";
        }
        ttml::autograd::ctx().set_mesh_shape(tt::tt_metal::distributed::MeshShape(1, 2));
        ttml::autograd::ctx().open_device();
    }

    void TearDown() override {
        ttml::autograd::ctx().close_device();
    }
};

TEST_F(N300TensorParallelLinearTest, RowParallelLinearHasBiasNotInputParallel) {
    uint32_t in_features = 64U;
    uint32_t out_features = 64U;
    bool has_bias = true;
    bool input_is_parallel = false;

    auto layer = ttml::modules::distributed::RowParallelLinear(in_features, out_features, has_bias, input_is_parallel);
    auto parameters = layer.parameters();
    EXPECT_EQ(parameters.size(), 1UL + static_cast<size_t>(has_bias));

    auto weight = get_parameter(parameters, "weight");
    auto bias = get_parameter(parameters, "bias");

    auto* device = &ttml::autograd::ctx().get_device();
    auto mesh_shape = device->shape();

    xt::xarray<float> test_data = xt::random::rand({in_features}, 0.F, 1.F).reshape({1U, 1U, 1U, in_features});
    ttml::core::XTensorToMeshVariant<float> replicate_composer = ttml::core::ReplicateXTensorToMesh<float>(mesh_shape);
    auto tt_tensor = ttml::core::from_xtensor<float, DataType::BFLOAT16>(test_data, device, replicate_composer);
    auto tensor = ttml::autograd::create_tensor(tt_tensor);
    auto output = layer(tensor);

    ttml::core::MeshToXTensorVariant<float> identity_composer = ttml::core::VectorMeshToXTensor<float>(mesh_shape);
    auto output_xtensor = ttml::core::to_xtensor<float>(output->get_value(), identity_composer);
    EXPECT_TRUE(xt::allclose(output_xtensor[0], output_xtensor[1], /* rtol */ 1e-3, /* atol */ 1e-2));

    ttml::core::MeshToXTensorVariant<float> concat_composer = ttml::core::ConcatMeshToXTensor<float>(mesh_shape, 3U);
    // (1, 1, out_features, in_features)
    auto weight_xtensor = ttml::core::to_xtensor<float>(weight->get_value(), concat_composer);
    auto bias_xtensor = ttml::core::to_xtensor<float>(bias->get_value(), identity_composer);

    auto weight_xtensor_shape = weight_xtensor[0].shape();
    auto test_data_shape = test_data.shape();

    auto expected_output = xt::linalg::dot(test_data, xt::transpose(weight_xtensor[0], {0, 1, 3, 2}));
    if (has_bias) {
        expected_output += bias_xtensor[0];
    }

    EXPECT_TRUE(xt::allclose(expected_output, output_xtensor[0], /* rtol */ 1e-3, /* atol */ 1e-2));
    EXPECT_TRUE(xt::allclose(expected_output, output_xtensor[1], /* rtol */ 1e-3, /* atol */ 1e-2));
};

TEST_F(N300TensorParallelLinearTest, RowParallelLinearNoBiasNotInputParallel) {
    uint32_t in_features = 64U;
    uint32_t out_features = 64U;
    bool has_bias = false;
    bool input_is_parallel = false;

    auto layer = ttml::modules::distributed::RowParallelLinear(in_features, out_features, has_bias, input_is_parallel);
    auto parameters = layer.parameters();
    EXPECT_EQ(parameters.size(), 1UL + static_cast<size_t>(has_bias));

    auto weight = get_parameter(parameters, "weight");

    auto* device = &ttml::autograd::ctx().get_device();
    auto mesh_shape = device->shape();

    xt::xarray<float> test_data = xt::random::rand({in_features}, 0.F, 1.F).reshape({1U, 1U, 1U, in_features});
    ttml::core::XTensorToMeshVariant<float> replicate_composer = ttml::core::ReplicateXTensorToMesh<float>(mesh_shape);
    auto tt_tensor = ttml::core::from_xtensor<float, DataType::BFLOAT16>(test_data, device, replicate_composer);
    auto tensor = ttml::autograd::create_tensor(tt_tensor);
    auto output = layer(tensor);

    ttml::core::MeshToXTensorVariant<float> identity_composer = ttml::core::VectorMeshToXTensor<float>(mesh_shape);
    auto output_xtensor = ttml::core::to_xtensor<float>(output->get_value(), identity_composer);
    EXPECT_TRUE(xt::allclose(output_xtensor[0], output_xtensor[1], /* rtol */ 1e-3, /* atol */ 1e-2));

    ttml::core::MeshToXTensorVariant<float> concat_composer = ttml::core::ConcatMeshToXTensor<float>(mesh_shape, 3U);
    // (1, 1, out_features, in_features)
    auto weight_xtensor = ttml::core::to_xtensor<float>(weight->get_value(), concat_composer);

    auto weight_xtensor_shape = weight_xtensor[0].shape();
    auto test_data_shape = test_data.shape();

    auto expected_output = xt::linalg::dot(test_data, xt::transpose(weight_xtensor[0], {0, 1, 3, 2}));
    EXPECT_TRUE(xt::allclose(expected_output, output_xtensor[0], /* rtol */ 1e-3, /* atol */ 1e-2));
    EXPECT_TRUE(xt::allclose(expected_output, output_xtensor[1], /* rtol */ 1e-3, /* atol */ 1e-2));
};

TEST_F(N300TensorParallelLinearTest, RowParallelLinearHasBiasInputParallel) {
    uint32_t in_features = 64U;
    uint32_t out_features = 64U;
    bool has_bias = true;
    bool input_is_parallel = true;

    auto layer = ttml::modules::distributed::RowParallelLinear(in_features, out_features, has_bias, input_is_parallel);
    auto parameters = layer.parameters();
    EXPECT_EQ(parameters.size(), 1UL + static_cast<size_t>(has_bias));

    auto weight = get_parameter(parameters, "weight");
    auto bias = get_parameter(parameters, "bias");

    auto* device = &ttml::autograd::ctx().get_device();
    auto mesh_shape = device->shape();

    xt::xarray<float> test_data = xt::random::rand({in_features}, 0.F, 1.F).reshape({1U, 1U, 1U, in_features});
    ttml::core::XTensorToMeshVariant<float> shard_composer = ttml::core::ShardXTensorToMesh<float>(mesh_shape, 3);
    auto tt_tensor = ttml::core::from_xtensor<float, DataType::BFLOAT16>(test_data, device, shard_composer);
    auto tensor = ttml::autograd::create_tensor(tt_tensor);
    auto output = layer(tensor);

    ttml::core::MeshToXTensorVariant<float> identity_composer = ttml::core::VectorMeshToXTensor<float>(mesh_shape);
    auto output_xtensor = ttml::core::to_xtensor<float>(output->get_value(), identity_composer);
    EXPECT_TRUE(xt::allclose(output_xtensor[0], output_xtensor[1], /* rtol */ 1e-3, /* atol */ 1e-2));

    ttml::core::MeshToXTensorVariant<float> concat_composer = ttml::core::ConcatMeshToXTensor<float>(mesh_shape, 3U);
    // (1, 1, out_features, in_features)
    auto weight_xtensor = ttml::core::to_xtensor<float>(weight->get_value(), concat_composer);
    auto bias_xtensor = ttml::core::to_xtensor<float>(bias->get_value(), identity_composer);
    auto expected_output = xt::linalg::dot(test_data, xt::transpose(weight_xtensor[0], {0, 1, 3, 2}));
    if (has_bias) {
        expected_output += bias_xtensor[0];
    }

    EXPECT_TRUE(xt::allclose(expected_output, output_xtensor[0], /* rtol */ 1e-3, /* atol */ 1e-2));
    EXPECT_TRUE(xt::allclose(expected_output, output_xtensor[1], /* rtol */ 1e-3, /* atol */ 1e-2));
};

TEST_F(N300TensorParallelLinearTest, RowParallelLinearNoBiasInputParallel) {
    uint32_t in_features = 64U;
    uint32_t out_features = 64U;
    bool has_bias = false;
    bool input_is_parallel = true;

    auto layer = ttml::modules::distributed::RowParallelLinear(in_features, out_features, has_bias, input_is_parallel);
    auto parameters = layer.parameters();
    EXPECT_EQ(parameters.size(), 1UL + static_cast<size_t>(has_bias));

    auto weight = get_parameter(parameters, "weight");

    auto* device = &ttml::autograd::ctx().get_device();
    auto mesh_shape = device->shape();

    xt::xarray<float> test_data = xt::random::rand({in_features}, 0.F, 1.F).reshape({1U, 1U, 1U, in_features});
    ttml::core::XTensorToMeshVariant<float> shard_composer = ttml::core::ShardXTensorToMesh<float>(mesh_shape, 3);
    auto tt_tensor = ttml::core::from_xtensor<float, DataType::BFLOAT16>(test_data, device, shard_composer);
    auto tensor = ttml::autograd::create_tensor(tt_tensor);
    auto output = layer(tensor);

    ttml::core::MeshToXTensorVariant<float> identity_composer = ttml::core::VectorMeshToXTensor<float>(mesh_shape);
    auto output_xtensor = ttml::core::to_xtensor<float>(output->get_value(), identity_composer);
    EXPECT_TRUE(xt::allclose(output_xtensor[0], output_xtensor[1], /* rtol */ 1e-3, /* atol */ 1e-2));

    ttml::core::MeshToXTensorVariant<float> concat_composer = ttml::core::ConcatMeshToXTensor<float>(mesh_shape, 3U);
    // (1, 1, out_features, in_features)
    auto weight_xtensor = ttml::core::to_xtensor<float>(weight->get_value(), concat_composer);
    auto expected_output = xt::linalg::dot(test_data, xt::transpose(weight_xtensor[0], {0, 1, 3, 2}));

    EXPECT_TRUE(xt::allclose(expected_output, output_xtensor[0], /* rtol */ 1e-3, /* atol */ 1e-2));
    EXPECT_TRUE(xt::allclose(expected_output, output_xtensor[1], /* rtol */ 1e-3, /* atol */ 1e-2));
};

TEST_F(N300TensorParallelLinearTest, ColumnParallelLinearHasBiasAllGather) {
    uint32_t in_features = 64U;
    uint32_t out_features = 64U;
    bool has_bias = true;
    bool use_all_gather = true;

    auto layer = ttml::modules::distributed::ColumnParallelLinear(in_features, out_features, has_bias, use_all_gather);
    auto parameters = layer.parameters();
    EXPECT_EQ(parameters.size(), 1UL + static_cast<size_t>(has_bias));

    auto weight = get_parameter(parameters, "weight");
    auto bias = get_parameter(parameters, "bias");

    auto* device = &ttml::autograd::ctx().get_device();
    auto mesh_shape = device->shape();

    xt::xarray<float> test_data = xt::random::rand({in_features}, 0.F, 1.F).reshape({1U, 1U, 1U, in_features});
    ttml::core::XTensorToMeshVariant<float> replicate_composer = ttml::core::ReplicateXTensorToMesh<float>(mesh_shape);
    auto tt_tensor = ttml::core::from_xtensor<float, DataType::BFLOAT16>(test_data, device, replicate_composer);
    auto tensor = ttml::autograd::create_tensor(tt_tensor);
    auto output = layer(tensor);

    ttml::core::MeshToXTensorVariant<float> identity_composer = ttml::core::VectorMeshToXTensor<float>(mesh_shape);
    auto output_xtensor = ttml::core::to_xtensor<float>(output->get_value(), identity_composer);
    EXPECT_TRUE(xt::allclose(output_xtensor[0], output_xtensor[1], /* rtol */ 1e-3, /* atol */ 1e-2));

    ttml::core::MeshToXTensorVariant<float> concat_composer_2 = ttml::core::ConcatMeshToXTensor<float>(mesh_shape, 2U);
    ttml::core::MeshToXTensorVariant<float> concat_composer_3 = ttml::core::ConcatMeshToXTensor<float>(mesh_shape, 3U);
    // (1, 1, out_features, in_features)
    auto weight_xtensor = ttml::core::to_xtensor<float>(weight->get_value(), concat_composer_2);
    auto bias_xtensor = ttml::core::to_xtensor<float>(bias->get_value(), concat_composer_3);

    auto expected_output = xt::linalg::dot(test_data, xt::transpose(weight_xtensor[0], {0, 1, 3, 2}));
    if (has_bias) {
        expected_output += bias_xtensor[0];
    }

    EXPECT_TRUE(xt::allclose(expected_output, output_xtensor[0], /* rtol */ 1e-2, /* atol */ 1e-2));
    EXPECT_TRUE(xt::allclose(expected_output, output_xtensor[1], /* rtol */ 1e-2, /* atol */ 1e-2));
};

TEST_F(N300TensorParallelLinearTest, ColumnParallelLinearNoBiasAllGather) {
    uint32_t in_features = 64U;
    uint32_t out_features = 64U;
    bool has_bias = false;
    bool use_all_gather = true;

    auto layer = ttml::modules::distributed::ColumnParallelLinear(in_features, out_features, has_bias, use_all_gather);
    auto parameters = layer.parameters();
    EXPECT_EQ(parameters.size(), 1UL + static_cast<size_t>(has_bias));

    auto weight = get_parameter(parameters, "weight");

    auto* device = &ttml::autograd::ctx().get_device();
    auto mesh_shape = device->shape();

    xt::xarray<float> test_data = xt::random::rand({in_features}, 0.F, 1.F).reshape({1U, 1U, 1U, in_features});
    ttml::core::XTensorToMeshVariant<float> replicate_composer = ttml::core::ReplicateXTensorToMesh<float>(mesh_shape);
    auto tt_tensor = ttml::core::from_xtensor<float, DataType::BFLOAT16>(test_data, device, replicate_composer);
    auto tensor = ttml::autograd::create_tensor(tt_tensor);
    auto output = layer(tensor);

    ttml::core::MeshToXTensorVariant<float> identity_composer = ttml::core::VectorMeshToXTensor<float>(mesh_shape);
    auto output_xtensor = ttml::core::to_xtensor<float>(output->get_value(), identity_composer);
    EXPECT_TRUE(xt::allclose(output_xtensor[0], output_xtensor[1], /* rtol */ 1e-3, /* atol */ 1e-2));

    ttml::core::MeshToXTensorVariant<float> concat_composer_2 = ttml::core::ConcatMeshToXTensor<float>(mesh_shape, 2U);
    ttml::core::MeshToXTensorVariant<float> concat_composer_3 = ttml::core::ConcatMeshToXTensor<float>(mesh_shape, 3U);
    // (1, 1, out_features, in_features)
    auto weight_xtensor = ttml::core::to_xtensor<float>(weight->get_value(), concat_composer_2);
    auto expected_output = xt::linalg::dot(test_data, xt::transpose(weight_xtensor[0], {0, 1, 3, 2}));

    EXPECT_TRUE(xt::allclose(expected_output, output_xtensor[0], /* rtol */ 1e-2, /* atol */ 1e-2));
    EXPECT_TRUE(xt::allclose(expected_output, output_xtensor[1], /* rtol */ 1e-2, /* atol */ 1e-2));
};

TEST_F(N300TensorParallelLinearTest, ColumnParallelLinearHasBiasNoAllGather) {
    uint32_t in_features = 64U;
    uint32_t out_features = 64U;
    bool has_bias = true;
    bool use_all_gather = false;

    auto layer = ttml::modules::distributed::ColumnParallelLinear(in_features, out_features, has_bias, use_all_gather);
    auto parameters = layer.parameters();
    EXPECT_EQ(parameters.size(), 1UL + static_cast<size_t>(has_bias));

    auto weight = get_parameter(parameters, "weight");
    auto bias = get_parameter(parameters, "bias");

    auto* device = &ttml::autograd::ctx().get_device();
    auto mesh_shape = device->shape();

    xt::xarray<float> test_data = xt::random::rand({in_features}, 0.F, 1.F).reshape({1U, 1U, 1U, in_features});
    ttml::core::XTensorToMeshVariant<float> replicate_composer = ttml::core::ReplicateXTensorToMesh<float>(mesh_shape);
    auto tt_tensor = ttml::core::from_xtensor<float, DataType::BFLOAT16>(test_data, device, replicate_composer);
    auto tensor = ttml::autograd::create_tensor(tt_tensor);
    auto output = layer(tensor);

    ttml::core::MeshToXTensorVariant<float> identity_composer = ttml::core::VectorMeshToXTensor<float>(mesh_shape);
    auto output_xtensor = ttml::core::to_xtensor<float>(output->get_value(), identity_composer);

    ttml::core::MeshToXTensorVariant<float> concat_composer_2 = ttml::core::ConcatMeshToXTensor<float>(mesh_shape, 2U);
    ttml::core::MeshToXTensorVariant<float> concat_composer_3 = ttml::core::ConcatMeshToXTensor<float>(mesh_shape, 3U);
    // (1, 1, out_features, in_features)
    auto weight_xtensor = ttml::core::to_xtensor<float>(weight->get_value(), concat_composer_2);
    auto bias_xtensor = ttml::core::to_xtensor<float>(bias->get_value(), concat_composer_3);

    auto expected_output = xt::linalg::dot(test_data, xt::transpose(weight_xtensor[0], {0, 1, 3, 2}));
    expected_output = expected_output.reshape({1U, 1U, 1U, out_features});
    if (has_bias) {
        expected_output += bias_xtensor[0];
    }

    EXPECT_TRUE(xt::allclose(
        xt::view(expected_output, xt::all(), xt::all(), xt::all(), xt::range(0, out_features / 2)),
        output_xtensor[0],
        /* rtol */ 1e-2,
        /* atol */ 1e-2));
    EXPECT_TRUE(xt::allclose(
        xt::view(expected_output, xt::all(), xt::all(), xt::all(), xt::range(out_features / 2, out_features)),
        output_xtensor[1],
        /* rtol */ 1e-2,
        /* atol */ 1e-2));
};

TEST_F(N300TensorParallelLinearTest, ColumnParallelLinearNoBiasNoAllGather) {
    uint32_t in_features = 64U;
    uint32_t out_features = 64U;
    bool has_bias = false;
    bool use_all_gather = false;

    auto layer = ttml::modules::distributed::ColumnParallelLinear(in_features, out_features, has_bias, use_all_gather);
    auto parameters = layer.parameters();
    EXPECT_EQ(parameters.size(), 1UL + static_cast<size_t>(has_bias));

    auto weight = get_parameter(parameters, "weight");

    auto* device = &ttml::autograd::ctx().get_device();
    auto mesh_shape = device->shape();

    xt::xarray<float> test_data = xt::random::rand({in_features}, 0.F, 1.F).reshape({1U, 1U, 1U, in_features});
    ttml::core::XTensorToMeshVariant<float> replicate_composer = ttml::core::ReplicateXTensorToMesh<float>(mesh_shape);
    auto tt_tensor = ttml::core::from_xtensor<float, DataType::BFLOAT16>(test_data, device, replicate_composer);
    auto tensor = ttml::autograd::create_tensor(tt_tensor);
    auto output = layer(tensor);

    ttml::core::MeshToXTensorVariant<float> identity_composer = ttml::core::VectorMeshToXTensor<float>(mesh_shape);
    auto output_xtensor = ttml::core::to_xtensor<float>(output->get_value(), identity_composer);

    ttml::core::MeshToXTensorVariant<float> concat_composer_2 = ttml::core::ConcatMeshToXTensor<float>(mesh_shape, 2U);
    ttml::core::MeshToXTensorVariant<float> concat_composer_3 = ttml::core::ConcatMeshToXTensor<float>(mesh_shape, 3U);
    // (1, 1, out_features, in_features)
    auto weight_xtensor = ttml::core::to_xtensor<float>(weight->get_value(), concat_composer_2);

    auto expected_output = xt::linalg::dot(test_data, xt::transpose(weight_xtensor[0], {0, 1, 3, 2}));
    expected_output = expected_output.reshape({1U, 1U, 1U, out_features});

    EXPECT_TRUE(xt::allclose(
        xt::view(expected_output, xt::all(), xt::all(), xt::all(), xt::range(0, out_features / 2)),
        output_xtensor[0],
        /* rtol */ 1e-2,
        /* atol */ 1e-2));
    EXPECT_TRUE(xt::allclose(
        xt::view(expected_output, xt::all(), xt::all(), xt::all(), xt::range(out_features / 2, out_features)),
        output_xtensor[1],
        /* rtol */ 1e-2,
        /* atol */ 1e-2));
};
