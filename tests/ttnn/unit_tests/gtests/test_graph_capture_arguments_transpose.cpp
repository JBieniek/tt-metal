// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
//
// SPDX-License-Identifier: Apache-2.0

#include "gtest/gtest.h"
#include "ttnn_test_fixtures.hpp"
#include "ttnn/device.hpp"
#include "ttnn/graph/graph_processor.hpp"
#include "ttnn/graph/graph_consts.hpp"
#include "ttnn/graph/graph_trace_utils.hpp"
#include "ttnn/operations/data_movement/transpose/transpose.hpp"
#include <optional>
#include <string>

namespace ttnn::graph::arguments::test {

class TestGraphCaptureArgumentsTranspose : public TTNNFixtureWithTensor {};

TEST_P(TestGraphCaptureArgumentsTranspose, Transpose) {
    auto tt_input = CreateTensor();
    tt_input.reshape(ttnn::Shape{1, 2048, 4, 128});
    ttnn::graph::GraphProcessor::begin_graph_capture(tt::tt_metal::IGraphProcessor::RunMode::NORMAL);
    ttnn::transpose(tt_input, 1, 2);
    auto trace = ttnn::graph::GraphProcessor::end_graph_capture();
    auto operations = ttnn::graph::extract_arguments(trace);

    auto operation0 = operations[0];
    EXPECT_EQ(operation0.operation_name, "ttnn::transpose");
    EXPECT_EQ(operation0.arguments.size(), 3);
    EXPECT_EQ(
        operation0.arguments[0],
        "Tensor(storage=DeviceStorage(memory_config=MemoryConfig(memory_layout=TensorMemoryLayout::INTERLEAVED,buffer_"
        "type=BufferType::L1,shard_spec=std::nullopt)),tensor_spec=TensorSpec(logical_shape=Shape([1, 1, 2048, "
        "512]),tensor_layout=TensorLayout(dtype=BFLOAT16,page_config=PageConfig(config=RowMajorPageConfig(tile=Tile("
        "tile_shape={32, 32},face_shape={16, "
        "16},num_faces=4))),memory_config=MemoryConfig(memory_layout=TensorMemoryLayout::INTERLEAVED,buffer_type="
        "BufferType::L1,shard_spec=std::nullopt),alignment=Alignment([1]))))");
    EXPECT_EQ(operation0.arguments[1], "1");
    EXPECT_EQ(operation0.arguments[2], "2");

    auto operation1 = operations[1];
    EXPECT_EQ(operation1.operation_name, "ttnn::prim::permute");
    EXPECT_EQ(operation1.arguments.size(), 5);
    EXPECT_EQ(
        operation1.arguments[0],
        "Tensor(storage=DeviceStorage(memory_config=MemoryConfig(memory_layout=TensorMemoryLayout::INTERLEAVED,buffer_"
        "type=BufferType::L1,shard_spec=std::nullopt)),tensor_spec=TensorSpec(logical_shape=Shape([1, 1, 2048, "
        "512]),tensor_layout=TensorLayout(dtype=BFLOAT16,page_config=PageConfig(config=RowMajorPageConfig(tile=Tile("
        "tile_shape={32, 32},face_shape={16, "
        "16},num_faces=4))),memory_config=MemoryConfig(memory_layout=TensorMemoryLayout::INTERLEAVED,buffer_type="
        "BufferType::L1,shard_spec=std::nullopt),alignment=Alignment([1]))))");
    EXPECT_EQ(operation1.arguments[1], "SmallVector([0, 2, 1, 3])");
    EXPECT_EQ(
        operation1.arguments[2],
        "MemoryConfig(memory_layout=TensorMemoryLayout::INTERLEAVED,buffer_type=BufferType::L1,shard_spec=std::"
        "nullopt)");
    EXPECT_EQ(operation1.arguments[3], "[ unsupported type , std::__1::reference_wrapper<std::__1::nullopt_t const>]");
    EXPECT_EQ(operation1.arguments[4], "0");

    auto operation2 = operations[2];
    EXPECT_EQ(operation2.operation_name, "PermuteDeviceOperation");
    EXPECT_EQ(operation2.arguments.size(), 2);
    EXPECT_EQ(
        operation2.arguments[0],
        "[ unsupported type , "
        "std::__1::reference_wrapper<ttnn::operations::data_movement::PermuteDeviceOperation::operation_attributes_t "
        "const>]");
    EXPECT_EQ(
        operation2.arguments[1],
        "[ unsupported type , "
        "std::__1::reference_wrapper<ttnn::operations::data_movement::PermuteDeviceOperation::tensor_args_t const>]");

    auto operation3 = operations[3];
    EXPECT_EQ(operation3.operation_name, "tt::tt_metal::create_device_tensor");
    EXPECT_EQ(operation3.arguments.size(), 5);
    EXPECT_EQ(operation3.arguments[0], "Shape([1, 2048, 1, 512])");
    EXPECT_EQ(operation3.arguments[1], "BFLOAT16");
    EXPECT_EQ(operation3.arguments[2], "Row Major");
    EXPECT_EQ(operation3.arguments[3], "[ unsupported type , std::__1::reference_wrapper<tt::tt_metal::v0::IDevice*>]");
    EXPECT_EQ(
        operation3.arguments[4],
        "MemoryConfig(memory_layout=TensorMemoryLayout::INTERLEAVED,buffer_type=BufferType::L1,shard_spec=std::"
        "nullopt)");
}

INSTANTIATE_TEST_SUITE_P(
    TestGraphCaptureArgumentsTranspose_Transpose,
    TestGraphCaptureArgumentsTranspose,
    ::testing::Values(CreateTensorParameters{
        .input_shape = ttnn::Shape({1, 1, 2048, 512}),
        .dtype = DataType::BFLOAT16,
        .layout = ROW_MAJOR_LAYOUT,
        .mem_cfg = L1_MEMORY_CONFIG}));
}  // namespace ttnn::graph::arguments::test
