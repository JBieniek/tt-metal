#pragma once

#include "tensor/host_buffer.hpp"

#include "tt_metal/impl/buffers/buffer.hpp"
#include "common/bfloat16.hpp"
#include "third_party/magic_enum/magic_enum.hpp"

#include <memory>
#include <variant>
#include <vector>
#include <fmt/ranges.h>


namespace tt {

namespace tt_metal {

using Shape = std::array<uint32_t, 4>;

struct MemoryConfig {
    bool interleaved = true;    // Interleave the data across multiple DRAM banks
    BufferType buffer_type = BufferType::DRAM; // Can be either DRAM or L1

    std::string str() const {
            return fmt::format(
            "MemoryConfig(interleaved={},buffer_type={})",
            this->interleaved,
            magic_enum::enum_name(this->buffer_type)
        );
    }
};

using HostBuffer = std::variant<
    host_buffer::HostBufferForDataType<uint32_t>,
    host_buffer::HostBufferForDataType<float>,
    host_buffer::HostBufferForDataType<bfloat16>
>;
struct HostStorage {
    HostBuffer buffer;
};

using DeviceBuffer = std::shared_ptr<Buffer>;
struct DeviceStorage {
    std::shared_ptr<Buffer> buffer;
    Device* device;
    MemoryConfig memory_config;
};

using Storage = std::variant<
    HostStorage,
    DeviceStorage
>;

}  // namespace tt_metal

}  // namespace tt
