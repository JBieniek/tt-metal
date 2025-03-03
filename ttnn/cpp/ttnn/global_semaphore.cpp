// SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.
//
// SPDX-License-Identifier: Apache-2.0

#include "global_semaphore.hpp"

#include <tt-metalium/host_api.hpp>
#include <tt-metalium/global_semaphore.hpp>
#include <tt-metalium/span.hpp>

namespace ttnn::global_semaphore {

MultiDeviceGlobalSemaphore::MultiDeviceGlobalSemaphore(MeshDevice* mesh_device) {
    TT_ASSERT(
        mesh_device != nullptr,
        "Must provide a valid mesh_device when initializing a global semaphore on multiple devices.");
    this->global_semaphores.reserve(mesh_device->num_devices());
}

GlobalSemaphore create_global_semaphore(
    IDevice* device, const CoreRangeSet& cores, uint32_t initial_value, BufferType buffer_type) {
    return CreateGlobalSemaphore(device, cores, initial_value, buffer_type);
}

tt::tt_metal::DeviceAddr get_global_semaphore_address(const GlobalSemaphore& global_semaphore) {
    return global_semaphore.address();
}

void reset_global_semaphore_value(const GlobalSemaphore& global_semaphore, uint32_t reset_value) {
    global_semaphore.reset_semaphore_value(reset_value);
}

MultiDeviceGlobalSemaphore create_global_semaphore(
    MeshDevice* mesh_device, const CoreRangeSet& cores, uint32_t initial_value, BufferType buffer_type) {
    MultiDeviceGlobalSemaphore multi_device_global_semaphore(mesh_device);
    auto& global_semaphores = multi_device_global_semaphore.global_semaphores;
    const auto& devices = mesh_device->get_devices();
    for (uint32_t i = 0; i < devices.size(); ++i) {
        auto* device = devices[i];
        global_semaphores.push_back(create_global_semaphore(device, cores, initial_value, buffer_type));
    }
    return multi_device_global_semaphore;
}
MultiDeviceGlobalSemaphore create_global_semaphore_with_same_address(
    MeshDevice* mesh_device,
    const CoreRangeSet& cores,
    uint32_t initial_value,
    BufferType buffer_type,
    uint32_t attempts,
    bool search_max) {
    MultiDeviceGlobalSemaphore multi_device_global_semaphore(mesh_device);
    const auto& devices = mesh_device->get_devices();
    for (uint32_t i = 0; i < devices.size(); ++i) {
        auto* device = devices[i];
        multi_device_global_semaphore.global_semaphores.push_back(
            create_global_semaphore(device, cores, initial_value, buffer_type));
    }

    auto global_semaphores = multi_device_global_semaphore.global_semaphores;
    auto first_addr = get_global_semaphore_address(global_semaphores.front());
    bool all_same = std::all_of(global_semaphores.begin(), global_semaphores.end(), [first_addr](const auto& sem) {
        return get_global_semaphore_address(sem) == first_addr;
    });

    if (!all_same) {
        tt::log_debug("chkpt 1, attempts: {}", attempts);
        tt::tt_metal::DeviceAddr target_addr = get_global_semaphore_address(global_semaphores.front());
        for (auto i = 1; i < global_semaphores.size(); i++) {
            tt::log_debug(
                "chkpt 1.1, i: {}, global_semaphores[i]->address(): {}",
                i,
                get_global_semaphore_address(global_semaphores[i]));
            if (search_max) {
                if (get_global_semaphore_address(global_semaphores[i]) > target_addr) {
                    target_addr = get_global_semaphore_address(global_semaphores[i]);
                }
            } else {
                if (get_global_semaphore_address(global_semaphores[i]) < target_addr) {
                    target_addr = get_global_semaphore_address(global_semaphores[i]);
                }
            }
        };
        tt::log_debug("chkpt 2, target_addr: {}", target_addr);
        for (auto i = 0; i < global_semaphores.size(); i++) {
            auto* device = devices[i];
            tt::log_debug("pushed, i: {}", i);
            device->push_work([i,
                               device,
                               attempts,
                               target_addr,
                               &cores,
                               initial_value,
                               buffer_type,
                               global_semaphore = &multi_device_global_semaphore.global_semaphores[i]] {
                size_t attempt = 0;
                std::vector<GlobalSemaphore> garbage;
                tt::log_debug("global_semaphore->address(): {}", get_global_semaphore_address(*global_semaphore));
                while (get_global_semaphore_address(*global_semaphore) != target_addr) {
                    auto sem = create_global_semaphore(device, cores, initial_value, buffer_type);

                    if (i == 0) {
                        tt::log_debug("chkpt 3, sem->address(): {}", get_global_semaphore_address(sem));
                    }

                    if (get_global_semaphore_address(sem) == target_addr) {
                        *global_semaphore = std::move(sem);
                    } else {
                        garbage.push_back(std::move(sem));
                        attempt++;
                    }

                    if (attempt > attempts) {
                        TT_THROW("Failed to create global semaphores with the same address");
                    }
                }
            });
        }
        for (auto device : devices) {
            device->synchronize();
        }
    }

    return multi_device_global_semaphore;
}
std::vector<tt::tt_metal::DeviceAddr> get_global_semaphore_address(const MultiDeviceGlobalSemaphore& global_semaphore) {
    std::vector<tt::tt_metal::DeviceAddr> addresses(global_semaphore.global_semaphores.size());
    const auto& global_semaphores = global_semaphore.global_semaphores;
    for (uint32_t i = 0; i < global_semaphores.size(); ++i) {
        addresses[i] = get_global_semaphore_address(global_semaphores[i]);
    }
    return addresses;
}

void reset_global_semaphore_value(const MultiDeviceGlobalSemaphore& global_semaphore, uint32_t reset_value) {
    for (const auto& global_semaphore : global_semaphore.global_semaphores) {
        reset_global_semaphore_value(global_semaphore, reset_value);
    }
}

}  // namespace ttnn::global_semaphore
