// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
//
// SPDX-License-Identifier: Apache-2.0

#include <mesh_device.hpp>

#include <cstddef>
#include <memory>
#include <unordered_map>
#include <utility>
#include <source_location>

#include <logger.hpp>
#include <host_api.hpp>
#include <tt_metal.hpp>
#include <system_mesh.hpp>
#include <mesh_device_view.hpp>
#include <mesh_command_queue.hpp>
#include <device_impl.hpp>
#include <sub_device.hpp>
#include <sub_device_manager_tracker.hpp>
#include <sub_device_manager.hpp>
#include <sub_device_types.hpp>

#include <hal.hpp>
#include <mesh_coord.hpp>
#include <small_vector.hpp>

namespace tt::tt_metal::distributed {

namespace {
MeshDeviceID generate_unique_mesh_id() {
    static std::atomic<MeshDeviceID> next_id{0};
    return next_id++;
}

// Helper function to verify all devices in the MeshDevice have the same value
template <typename F>
decltype(auto) validate_and_get_reference_value(
    const std::vector<IDevice*>& devices, F&& func, const std::source_location& loc = std::source_location::current()) {
    if (devices.empty()) {
        TT_THROW("{} [{}:{}] failed: MeshDevice has no devices", loc.function_name(), loc.file_name(), loc.line());
    }

    // Get reference to first device's value
    decltype(auto) reference_value = std::forward<F>(func)(devices.front());

    // Validate all other devices match
    for (auto it = devices.begin() + 1; it != devices.end(); ++it) {
        const auto& current_value = std::forward<F>(func)(*it);
        if (current_value != reference_value) {
            TT_THROW(
                "{} [{}:{}] failed: Device at index {} returned value that differs from reference. "
                "Expected: {}, Actual: {}",
                loc.function_name(),
                loc.file_name(),
                loc.line(),
                std::distance(devices.begin(), it),
                reference_value,
                current_value);
        }
    }
    return reference_value;
}

}  // namespace

MeshDevice::ScopedDevices::ScopedDevices(
    size_t l1_small_size,
    size_t trace_region_size,
    size_t num_command_queues,
    const DispatchCoreConfig& dispatch_core_config,
    const MeshDeviceConfig& config) {
    auto physical_device_ids = SystemMesh::instance().request_available_devices(config);
    opened_devices_ = tt::tt_metal::detail::CreateDevices(
        physical_device_ids, num_command_queues, l1_small_size, trace_region_size, dispatch_core_config);

    for (auto physical_device_id : physical_device_ids) {
        devices_.push_back(opened_devices_.at(physical_device_id));
    }
}

MeshDevice::ScopedDevices::~ScopedDevices() {
    if (!opened_devices_.empty()) {
        tt::tt_metal::detail::CloseDevices(opened_devices_);
    }
}

const std::vector<IDevice*>& MeshDevice::ScopedDevices::root_devices() const { return devices_; }

uint8_t MeshDevice::num_hw_cqs() const {
    return validate_and_get_reference_value(
        scoped_devices_->root_devices(), [](const auto& device) { return device->num_hw_cqs(); });
}

bool MeshDevice::is_initialized() const {
    return validate_and_get_reference_value(
        scoped_devices_->root_devices(), [](const auto& device) { return device->is_initialized(); });
}

uint32_t MeshDevice::l1_size_per_core() const {
    return validate_and_get_reference_value(
        scoped_devices_->root_devices(), [](const auto& device) { return device->l1_size_per_core(); });
}

uint32_t MeshDevice::dram_size_per_channel() const {
    return validate_and_get_reference_value(
        scoped_devices_->root_devices(), [](const auto& device) { return device->dram_size_per_channel(); });
}

IDevice* MeshDevice::reference_device() const { return this->get_devices().at(0); }

MeshDevice::MeshDevice(
    std::shared_ptr<ScopedDevices> mesh_handle,
    std::unique_ptr<MeshDeviceView> mesh_device_view,
    std::weak_ptr<MeshDevice> parent_mesh) :
    scoped_devices_(std::move(mesh_handle)),
    view_(std::move(mesh_device_view)),
    mesh_id_(generate_unique_mesh_id()),
    parent_mesh_(std::move(parent_mesh)) {}

std::shared_ptr<MeshDevice> MeshDevice::create(
    const MeshDeviceConfig& config,
    size_t l1_small_size,
    size_t trace_region_size,
    size_t num_command_queues,
    const DispatchCoreConfig& dispatch_core_config,
    tt::stl::Span<const std::uint32_t> l1_bank_remap) {
    auto scoped_devices = std::make_shared<ScopedDevices>(
        l1_small_size, trace_region_size, num_command_queues, dispatch_core_config, config);
    MeshContainer<IDevice*> devices(config.mesh_shape, scoped_devices->root_devices());
    auto mesh_device = std::make_shared<MeshDevice>(
        std::move(scoped_devices), std::make_unique<MeshDeviceView>(devices), std::weak_ptr<MeshDevice>());

    mesh_device->initialize(num_command_queues, l1_small_size, trace_region_size, l1_bank_remap);
    return mesh_device;
}

std::shared_ptr<MeshDevice> MeshDevice::create_submesh(
    const MeshShape& submesh_shape, const std::optional<MeshCoordinate>& offset) {
    TT_FATAL(
        std::all_of(submesh_shape.cbegin(), submesh_shape.cend(), [](size_t dim) { return dim > 0; }),
        "Invalid submesh shape: ({}). All dimensions must be positive.",
        submesh_shape);
    TT_FATAL(
        submesh_shape.dims() == view_->shape().dims(),
        "Submesh shape {} and mesh device shape {} must have the same number of dimensions.",
        submesh_shape,
        view_->shape());

    const MeshCoordinate offset_coord = [&offset, &submesh_shape]() {
        if (offset.has_value()) {
            TT_FATAL(
                submesh_shape.dims() == offset->dims(),
                "Submesh shape {} and offset {} must have the same number of dimensions.",
                submesh_shape,
                *offset);
            return *offset;
        } else {
            return MeshCoordinate::zero_coordinate(submesh_shape.dims());
        }
    }();

    tt::stl::SmallVector<uint32_t> end_coords;
    for (size_t i = 0; i < submesh_shape.dims(); i++) {
        TT_FATAL(
            offset_coord[i] + submesh_shape[i] - 1 < view_->shape()[i],
            "Submesh shape {} and offset {} does not fit within parent mesh ({}).",
            submesh_shape,
            offset,
            view_->shape());
        end_coords.push_back(offset_coord[i] + submesh_shape[i] - 1);
    }
    auto end_coordinate = MeshCoordinate(end_coords);

    MeshContainer<IDevice*> submesh_devices_container(
        submesh_shape, view_->get_devices(MeshCoordinateRange{offset_coord, end_coordinate}));

    auto submesh = std::make_shared<MeshDevice>(
        scoped_devices_, std::make_unique<MeshDeviceView>(submesh_devices_container), shared_from_this());

    submeshes_.push_back(submesh);
    log_trace(LogMetal, "Instantiating submesh {}: {} with offset: {}", submesh->id(), submesh_shape, offset);
    log_trace(LogMetal, "Submesh {} instantiated with {} devices", submesh->id(), submesh->get_devices().size());
    return submesh;
}

std::vector<std::shared_ptr<MeshDevice>> MeshDevice::create_submeshes(const MeshShape& submesh_shape) {
    // Calculate how many submeshes fit in each dimension.
    tt::stl::SmallVector<uint32_t> steps;
    for (size_t dim = 0; dim < shape().dims(); dim++) {
        TT_FATAL(
            shape()[dim] % submesh_shape[dim] == 0,
            "Shape {} is not divisible by submesh shape {} along dimension {}",
            shape(),
            submesh_shape,
            dim);
        uint32_t num_steps = shape()[dim] / submesh_shape[dim];
        steps.push_back(num_steps);
    }

    // Stamp `submesh_shape` along each dimension, `steps` number of times.
    std::vector<std::shared_ptr<MeshDevice>> submeshes;
    for (const auto& step_position : MeshCoordinateRange(MeshShape(steps))) {
        tt::stl::SmallVector<uint32_t> offset_coords;
        for (size_t dim = 0; dim < submesh_shape.dims(); dim++) {
            offset_coords.push_back(step_position[dim] * submesh_shape[dim]);
        }
        submeshes.push_back(create_submesh(submesh_shape, MeshCoordinate(offset_coords)));
    }

    return submeshes;
}

MeshDevice::~MeshDevice() { close(); }

IDevice* MeshDevice::get_device(chip_id_t physical_device_id) const {
    for (auto device : this->get_devices()) {
        if (device->id() == physical_device_id) {
            return device;
        }
    }
    TT_THROW("Physical Device ID: {} not found in assigned devices", physical_device_id);
}

std::vector<IDevice*> MeshDevice::get_devices() const { return view_->get_devices(); }

// TODO: Remove this function once we have a proper view interface
IDevice* MeshDevice::get_device(size_t row_idx, size_t col_idx) const {
    return get_device(MeshCoordinate{row_idx, col_idx});
}

IDevice* MeshDevice::get_device(const MeshCoordinate& coord) const { return view_->get_device(coord); }

MeshCommandQueue& MeshDevice::mesh_command_queue(std::size_t cq_id) const {
    TT_FATAL(this->using_fast_dispatch(), "Can only access the MeshCommandQueue when using Fast Dispatch.");
    TT_FATAL(cq_id < mesh_command_queues_.size(), "cq_id {} is out of range", cq_id);
    return *(mesh_command_queues_[cq_id]);
}

const DeviceIds MeshDevice::get_device_ids() const {
    DeviceIds device_ids;
    for (auto device : this->get_devices()) {
        device_ids.push_back(device->id());
    }
    return device_ids;
}

size_t MeshDevice::num_devices() const { return view_->num_devices(); }

CoreCoord MeshDevice::compute_with_storage_grid_size() const {
    return validate_and_get_reference_value(
        scoped_devices_->root_devices(), [](const auto& device) { return device->compute_with_storage_grid_size(); });
}

tt::ARCH MeshDevice::arch() const {
    return validate_and_get_reference_value(
        scoped_devices_->root_devices(), [](const auto& device) { return device->arch(); });
}

size_t MeshDevice::num_rows() const { return view_->num_rows(); }

size_t MeshDevice::num_cols() const { return view_->num_cols(); }

const MeshShape& MeshDevice::shape() const { return view_->shape(); }

std::vector<IDevice*> MeshDevice::get_row_major_devices(const MeshShape& new_shape) const {
    // MeshDeviceView requires devices to be provided as a 1D array in row-major order for the target mesh shape.
    // The physical connectivity between devices must be preserved when reshaping.
    //
    // Example:
    // Given 4 devices physically connected in a 2x2 grid like this:
    //   [0]--[1]
    //    |    |
    //   [3]--[2]
    //
    // For a 1x4 mesh shape:
    // - Devices must form a line: 0->1->2->3
    // - Row-major order will be: [0,1,2,3]
    //
    // For a 2x2 mesh shape:
    // - Preserves original 2x2 physical connectivity
    // - Row-major order will be: [0,1,3,2]
    std::unordered_map<chip_id_t, size_t> physical_device_id_to_linearized_index;
    for (size_t i = 0; i < this->num_devices(); i++) {
        physical_device_id_to_linearized_index[this->get_devices()[i]->id()] = i;
    }

    // From an MxN mesh, we can always reduce rank to a 1xM*N Line mesh.
    // However, going from a Line mesh to an MxN mesh is not always possible.
    if (is_line_topology(new_shape)) {
        return view_->get_line_devices();
    }

    auto new_physical_device_ids =
        SystemMesh::instance().request_available_devices(MeshDeviceConfig{.mesh_shape = new_shape});

    for (size_t i = 0; i < new_physical_device_ids.size(); i++) {
        if (physical_device_id_to_linearized_index.find(new_physical_device_ids[i]) ==
            physical_device_id_to_linearized_index.end()) {
            TT_THROW(
                "User has requested a reshape of the MeshDevice to shape: {}, but it is not possible to form a "
                "physically connected mesh grid with the opened devices from the original shape: {}.",
                new_shape,
                view_->shape());
        }
    }

    std::vector<IDevice*> new_device_order;
    for (size_t i = 0; i < new_physical_device_ids.size(); i++) {
        new_device_order.push_back(this->get_device(new_physical_device_ids[i]));
    }
    return new_device_order;
}

void MeshDevice::reshape(const MeshShape& new_shape) {
    TT_FATAL(
        new_shape.mesh_size() == this->num_devices(),
        "New shape must have the same number of devices as current shape");

    MeshContainer<IDevice*> devices(new_shape, this->get_row_major_devices(new_shape));
    auto new_view = std::make_unique<MeshDeviceView>(devices);
    view_ = std::move(new_view);
}

bool MeshDevice::close() {
    for (const auto& submesh : submeshes_) {
        submesh->close();
    }
    submeshes_.clear();
    sub_device_manager_tracker_.reset();
    if (scoped_devices_) {
        scoped_devices_.reset();
    }
    parent_mesh_.reset();
    view_.reset();
    return true;
}

std::string MeshDevice::to_string() const {
    return fmt::format("MeshDevice({}x{} grid, {} devices)", this->num_rows(), this->num_cols(), this->num_devices());
}

const MeshDeviceView& MeshDevice::get_view() const {
    TT_FATAL(view_, "MeshDeviceView is not initialized");
    return *view_;
}

MeshDeviceID MeshDevice::id() const { return mesh_id_; }
// For a mesh, build id is the same as the device id for the reference device
chip_id_t MeshDevice::build_id() const { return reference_device()->id(); }

bool MeshDevice::is_parent_mesh() const { return parent_mesh_.expired(); }

std::vector<std::shared_ptr<MeshDevice>> MeshDevice::get_submeshes() const { return submeshes_; }

std::ostream& operator<<(std::ostream& os, const MeshDevice& mesh_device) { return os << mesh_device.to_string(); }

void MeshDevice::enable_async(bool enable) {
    auto devices = this->get_devices();
    if (enable && devices.size() == 1) {
        tt::log_warning("Async mode is always disabled for a single device, ignoring enable_async call");
        return;
    }
    for (auto device : devices) {
        dynamic_cast<Device*>(device)->force_enable_async(enable);
    }
}

void MeshDevice::enable_program_cache() {
    for (auto device : this->get_devices()) {
        device->enable_program_cache();
    }
}

void MeshDevice::disable_and_clear_program_cache() {
    for (auto device : this->get_devices()) {
        device->disable_and_clear_program_cache();
    }
}

size_t MeshDevice::num_program_cache_entries() {
    size_t total_entries = 0;
    for (auto device : this->get_devices()) {
        total_entries += device->num_program_cache_entries();
    }
    return total_entries;
}

SubDeviceManagerId MeshDevice::create_sub_device_manager(
    tt::stl::Span<const SubDevice> sub_devices, DeviceAddr local_l1_size) {
    return sub_device_manager_tracker_->create_sub_device_manager(sub_devices, local_l1_size);
}
void MeshDevice::remove_sub_device_manager(SubDeviceManagerId sub_device_manager_id) {
    sub_device_manager_tracker_->remove_sub_device_manager(sub_device_manager_id);
}
void MeshDevice::load_sub_device_manager(SubDeviceManagerId sub_device_manager_id) {
    sub_device_manager_tracker_->load_sub_device_manager(sub_device_manager_id);
}
void MeshDevice::clear_loaded_sub_device_manager() { sub_device_manager_tracker_->clear_loaded_sub_device_manager(); }

std::tuple<SubDeviceManagerId, SubDeviceId> MeshDevice::create_sub_device_manager_with_fabric(
    tt::stl::Span<const SubDevice> sub_devices, DeviceAddr local_l1_size) {
    return sub_device_manager_tracker_->create_sub_device_manager_with_fabric(sub_devices, local_l1_size);
}
CoreCoord MeshDevice::dram_grid_size() const {
    return validate_and_get_reference_value(
        scoped_devices_->root_devices(), [](const auto& device) { return device->dram_grid_size(); });
}

bool MeshDevice::using_slow_dispatch() const {
    return validate_and_get_reference_value(
        scoped_devices_->root_devices(), [](const auto& device) { return device->using_slow_dispatch(); });
}

bool MeshDevice::using_fast_dispatch() const {
    return validate_and_get_reference_value(
        scoped_devices_->root_devices(), [](const auto& device) { return device->using_fast_dispatch(); });
}

// Device property methods that can be delegated to reference device
CoreCoord MeshDevice::grid_size() const {
    return validate_and_get_reference_value(
        scoped_devices_->root_devices(), [](const auto& device) { return device->grid_size(); });
}
CoreCoord MeshDevice::logical_grid_size() const {
    return validate_and_get_reference_value(
        scoped_devices_->root_devices(), [](const auto& device) { return device->logical_grid_size(); });
}
CoreType MeshDevice::core_type_from_virtual_core(const CoreCoord& virtual_coord) const {
    return validate_and_get_reference_value(scoped_devices_->root_devices(), [virtual_coord](const auto& device) {
        return device->core_type_from_virtual_core(virtual_coord);
    });
}
CoreCoord MeshDevice::virtual_noc_coordinate(uint8_t noc_index, CoreCoord coord) const {
    return validate_and_get_reference_value(scoped_devices_->root_devices(), [noc_index, coord](const auto& device) {
        return device->virtual_noc_coordinate(noc_index, coord);
    });
}
CoreCoord MeshDevice::virtual_noc0_coordinate(uint8_t noc_index, CoreCoord coord) const {
    return validate_and_get_reference_value(scoped_devices_->root_devices(), [noc_index, coord](const auto& device) {
        return device->virtual_noc0_coordinate(noc_index, coord);
    });
}
std::vector<CoreCoord> MeshDevice::worker_cores_from_logical_cores(const std::vector<CoreCoord>& logical_cores) const {
    return validate_and_get_reference_value(scoped_devices_->root_devices(), [logical_cores](const auto& device) {
        return device->worker_cores_from_logical_cores(logical_cores);
    });
}
std::vector<CoreCoord> MeshDevice::get_optimal_dram_bank_to_logical_worker_assignment() {
    return validate_and_get_reference_value(scoped_devices_->root_devices(), [](const auto& device) {
        return device->get_optimal_dram_bank_to_logical_worker_assignment();
    });
}
CoreCoord MeshDevice::virtual_core_from_logical_core(const CoreCoord& logical_coord, const CoreType& core_type) const {
    return validate_and_get_reference_value(
        scoped_devices_->root_devices(), [logical_coord, core_type](const auto& device) {
            return device->virtual_core_from_logical_core(logical_coord, core_type);
        });
}
CoreCoord MeshDevice::worker_core_from_logical_core(const CoreCoord& logical_core) const {
    return validate_and_get_reference_value(scoped_devices_->root_devices(), [logical_core](const auto& device) {
        return device->worker_core_from_logical_core(logical_core);
    });
}
CoreCoord MeshDevice::logical_core_from_ethernet_core(const CoreCoord& ethernet_core) const {
    return validate_and_get_reference_value(scoped_devices_->root_devices(), [ethernet_core](const auto& device) {
        return device->logical_core_from_ethernet_core(ethernet_core);
    });
}

// These methods require some change / or assert out for now
std::vector<CoreCoord> MeshDevice::ethernet_cores_from_logical_cores(
    const std::vector<CoreCoord>& logical_cores) const {
    return validate_and_get_reference_value(scoped_devices_->root_devices(), [logical_cores](const auto& device) {
        return device->ethernet_cores_from_logical_cores(logical_cores);
    });
}
CoreCoord MeshDevice::ethernet_core_from_logical_core(const CoreCoord& logical_core) const {
    return validate_and_get_reference_value(scoped_devices_->root_devices(), [logical_core](const auto& device) {
        return device->ethernet_core_from_logical_core(logical_core);
    });
}
std::unordered_set<CoreCoord> MeshDevice::get_active_ethernet_cores(bool skip_reserved_tunnel_cores) const {
    TT_THROW("get_active_ethernet_cores() is not supported on MeshDevice - use individual devices instead");
}

std::unordered_set<CoreCoord> MeshDevice::get_inactive_ethernet_cores() const {
    TT_THROW("get_inactive_ethernet_cores() is not supported on MeshDevice - use individual devices instead");
}

bool MeshDevice::is_inactive_ethernet_core(CoreCoord logical_core) const {
    TT_THROW("is_inactive_ethernet_core() is not supported on MeshDevice - use individual devices instead");
}

std::tuple<chip_id_t, CoreCoord> MeshDevice::get_connected_ethernet_core(CoreCoord eth_core) const {
    TT_THROW("get_connected_ethernet_core() is not supported on MeshDevice - use individual devices instead");
}

bool MeshDevice::is_active_ethernet_core(CoreCoord logical_core, bool skip_reserved_tunnel_cores) const {
    TT_THROW("is_active_ethernet_core() is not supported on MeshDevice - use individual devices instead");
}

std::vector<CoreCoord> MeshDevice::get_ethernet_sockets(chip_id_t connected_chip_id) const {
    TT_THROW("get_ethernet_sockets() is not supported on MeshDevice - use individual devices instead");
}

// Core and worker management methods (These are OK)
CoreRangeSet MeshDevice::worker_cores(HalProgrammableCoreType core_type, SubDeviceId sub_device_id) const {
    return sub_device_manager_tracker_->get_active_sub_device_manager()->sub_device(sub_device_id).cores(core_type);
}
uint32_t MeshDevice::num_worker_cores(HalProgrammableCoreType core_type, SubDeviceId sub_device_id) const {
    return sub_device_manager_tracker_->get_active_sub_device_manager()->sub_device(sub_device_id).num_cores(core_type);
}

// Bank and memory management methods
int MeshDevice::num_dram_channels() const { return reference_device()->num_dram_channels() * this->num_devices(); }

CoreCoord MeshDevice::logical_core_from_dram_channel(uint32_t dram_channel) const {
    return validate_and_get_reference_value(scoped_devices_->root_devices(), [dram_channel](const auto& device) {
        return device->logical_core_from_dram_channel(dram_channel);
    });
}
uint32_t MeshDevice::dram_channel_from_logical_core(const CoreCoord& logical_core) const {
    return validate_and_get_reference_value(scoped_devices_->root_devices(), [logical_core](const auto& device) {
        return device->dram_channel_from_logical_core(logical_core);
    });
}

// Core management and network operations
const std::set<CoreCoord>& MeshDevice::ethernet_cores() const {
    return validate_and_get_reference_value(
        scoped_devices_->root_devices(),
        [](const auto& device) -> const std::set<CoreCoord>& { return device->ethernet_cores(); });
}
const std::set<CoreCoord>& MeshDevice::storage_only_cores() const {
    return validate_and_get_reference_value(
        scoped_devices_->root_devices(),
        [](const auto& device) -> const std::set<CoreCoord>& { return device->storage_only_cores(); });
}
uint32_t MeshDevice::get_noc_unicast_encoding(uint8_t noc_index, const CoreCoord& core) const {
    return validate_and_get_reference_value(scoped_devices_->root_devices(), [noc_index, core](const auto& device) {
        return device->get_noc_unicast_encoding(noc_index, core);
    });
}
uint32_t MeshDevice::get_noc_multicast_encoding(uint8_t noc_index, const CoreRange& cores) const {
    return validate_and_get_reference_value(scoped_devices_->root_devices(), [noc_index, cores](const auto& device) {
        return device->get_noc_multicast_encoding(noc_index, cores);
    });
}

// System memory and command queue management
SystemMemoryManager& MeshDevice::sysmem_manager() {
    TT_THROW("sysmem_manager() is not supported on MeshDevice - use individual devices instead");
    return reference_device()->sysmem_manager();
}

CommandQueue& MeshDevice::command_queue(size_t cq_id) {
    TT_THROW("command_queue() is not supported on MeshDevice - use individual devices instead");
    return reference_device()->command_queue(cq_id);
}

// Trace management
void MeshDevice::begin_trace(const uint8_t cq_id, const uint32_t tid) {
    for (auto& device : scoped_devices_->root_devices()) {
        device->begin_trace(cq_id, tid);
    }
}
void MeshDevice::end_trace(const uint8_t cq_id, const uint32_t tid) {
    for (auto& device : scoped_devices_->root_devices()) {
        device->end_trace(cq_id, tid);
    }
}
void MeshDevice::replay_trace(
    const uint8_t cq_id, const uint32_t tid, const bool block_on_device, const bool block_on_worker_thread) {
    for (auto& device : scoped_devices_->root_devices()) {
        device->replay_trace(cq_id, tid, block_on_device, false /* block_on_worker_thread */);
    }
    // If blocking, wait until worker threads have completed
    if (block_on_worker_thread) {
        for (auto& device : scoped_devices_->root_devices()) {
            device->synchronize();
        }
    }
}
void MeshDevice::release_trace(const uint32_t tid) {
    for (auto& device : scoped_devices_->root_devices()) {
        device->release_trace(tid);
    }
}

std::shared_ptr<MeshTraceBuffer>& MeshDevice::create_mesh_trace(const MeshTraceId& trace_id) {
    auto [trace, emplaced] = trace_buffer_pool_.emplace(trace_id, MeshTrace::create_empty_mesh_trace_buffer());
    TT_FATAL(emplaced, "Trace buffer with tid {} already exists", *trace_id);
    return trace->second;
}

void MeshDevice::release_mesh_trace(const MeshTraceId& trace_id) { trace_buffer_pool_.erase(trace_id); }

std::shared_ptr<MeshTraceBuffer> MeshDevice::get_mesh_trace(const MeshTraceId& trace_id) {
    auto trace = trace_buffer_pool_.find(trace_id);
    if (trace != trace_buffer_pool_.end()) {
        return trace->second;
    }
    TT_THROW("Trace Instance with ID {} is not initialized", *trace_id);
}

void MeshDevice::begin_mesh_trace(uint8_t cq_id, const MeshTraceId& trace_id) {
    auto& mesh_trace_buffer = this->create_mesh_trace(trace_id);
    mesh_command_queues_[cq_id]->record_begin(trace_id, mesh_trace_buffer->desc);
}

void MeshDevice::end_mesh_trace(uint8_t cq_id, const MeshTraceId& trace_id) {
    auto trace_buffer = this->get_mesh_trace(trace_id);
    mesh_command_queues_[cq_id]->record_end();
    MeshTrace::populate_mesh_buffer(*(mesh_command_queues_[cq_id]), trace_buffer);
}

std::shared_ptr<TraceBuffer> MeshDevice::get_trace(uint32_t tid) {
    TT_THROW("get_trace() is not supported on MeshDevice - use individual devices instead");
    return reference_device()->get_trace(tid);
}
uint32_t MeshDevice::get_trace_buffers_size() const { return trace_buffers_size_; }
void MeshDevice::set_trace_buffers_size(uint32_t size) { trace_buffers_size_ = size; }

// Light Metal
void MeshDevice::load_trace(const uint8_t cq_id, const uint32_t trace_id, const TraceDescriptor& trace_desc) {
    TT_THROW("load_trace() is not supported on MeshDevice - use individual devices instead");
    reference_device()->load_trace(cq_id, trace_id, trace_desc);
}

// Dispatch and initialization
bool MeshDevice::initialize(
    const uint8_t num_hw_cqs,
    size_t l1_small_size,
    size_t trace_region_size,
    tt::stl::Span<const std::uint32_t> l1_bank_remap,
    bool minimal) {
    // For MeshDevice, we support uniform sub-devices across all devices and we do not support ethernet subdevices.
    const auto& compute_grid_size = this->compute_with_storage_grid_size();
    auto sub_devices = {
        SubDevice(std::array{CoreRangeSet(CoreRange({0, 0}, {compute_grid_size.x - 1, compute_grid_size.y - 1}))})};

    const auto& allocator = reference_device()->allocator();
    sub_device_manager_tracker_ = std::make_unique<SubDeviceManagerTracker>(
        this, std::make_unique<L1BankingAllocator>(allocator->get_config()), sub_devices);
    mesh_command_queues_.reserve(this->num_hw_cqs());
    if (this->using_fast_dispatch()) {
        for (std::size_t cq_id = 0; cq_id < this->num_hw_cqs(); cq_id++) {
            mesh_command_queues_.push_back(std::make_unique<MeshCommandQueue>(this, cq_id));
        }
    }
    return true;
}

void MeshDevice::reset_cores() {
    TT_THROW("reset_cores() is not supported on MeshDevice - use individual devices instead");
    reference_device()->reset_cores();
}
void MeshDevice::initialize_and_launch_firmware() {
    TT_THROW("initialize_and_launch_firmware() is not supported on MeshDevice - use individual devices instead");
    reference_device()->initialize_and_launch_firmware();
}
void MeshDevice::init_command_queue_host() {
    TT_THROW("init_command_queue_host() is not supported on MeshDevice - use individual devices instead");
    reference_device()->init_command_queue_host();
}
void MeshDevice::init_command_queue_device() {
    TT_THROW("init_command_queue_device() is not supported on MeshDevice - use individual devices instead");
    reference_device()->init_command_queue_device();
}
void MeshDevice::init_fabric() {
    TT_THROW("init_fabric_program() is not supported on MeshDevice - use individual devices instead");
    reference_device()->init_fabric();
}
void MeshDevice::synchronize() {
    // Nothing to synchronize, as all work is executed by MeshDevice is synchronous.
}
WorkExecutorMode MeshDevice::get_worker_mode() { return WorkExecutorMode::SYNCHRONOUS; }
bool MeshDevice::is_worker_queue_empty() const { return true; }
void MeshDevice::push_work(std::function<void()> work, bool blocking) {
    // Execute inline synchronously.
    // Using a lock to provide the same call serialization guarantee as an async single device scheduling.
    std::lock_guard lock(push_work_mutex_);
    work();
}
program_cache::detail::ProgramCache& MeshDevice::get_program_cache() { return reference_device()->get_program_cache(); }
HalProgrammableCoreType MeshDevice::get_programmable_core_type(CoreCoord virtual_core) const { return reference_device()->get_programmable_core_type(virtual_core); }
std::vector<std::pair<transfer_info_cores, uint32_t>> MeshDevice::extract_dst_noc_multicast_info(
    const std::vector<CoreRange>& ranges, const CoreType core_type) {
    return reference_device()->extract_dst_noc_multicast_info(ranges, core_type);
}

size_t MeshDevice::get_device_kernel_defines_hash() {
    return validate_and_get_reference_value(
        scoped_devices_->root_devices(), [](const auto& device) { return device->get_device_kernel_defines_hash(); });
}

// Methods for SubDevice Management
uint8_t MeshDevice::num_noc_mcast_txns(SubDeviceId sub_device_id) const {
    return sub_device_manager_tracker_->get_active_sub_device_manager()->num_noc_mcast_txns(sub_device_id);
}
uint8_t MeshDevice::num_noc_unicast_txns(SubDeviceId sub_device_id) const {
    return sub_device_manager_tracker_->get_active_sub_device_manager()->num_noc_unicast_txns(sub_device_id);
}
uint8_t MeshDevice::noc_data_start_index(SubDeviceId sub_device_id, bool mcast_data, bool unicast_data) const {
    if (mcast_data) {
        return sub_device_manager_tracker_->get_active_sub_device_manager()->noc_mcast_data_start_index(sub_device_id);
    } else if (unicast_data) {
        return sub_device_manager_tracker_->get_active_sub_device_manager()->noc_unicast_data_start_index(
            sub_device_id);
    } else {
        return 0;
    }
}
SubDeviceManagerId MeshDevice::get_active_sub_device_manager_id() const {
    return sub_device_manager_tracker_->get_active_sub_device_manager()->id();
}
SubDeviceManagerId MeshDevice::get_default_sub_device_manager_id() const {
    return sub_device_manager_tracker_->get_default_sub_device_manager()->id();
}
CoreCoord MeshDevice::virtual_program_dispatch_core(uint8_t cq_id) const {
    return validate_and_get_reference_value(scoped_devices_->root_devices(), [cq_id](const auto& device) {
        return device->virtual_program_dispatch_core(cq_id);
    });
}
const std::vector<SubDeviceId>& MeshDevice::get_sub_device_ids() const {
    return sub_device_manager_tracker_->get_active_sub_device_manager()->get_sub_device_ids();
}
const std::vector<SubDeviceId>& MeshDevice::get_sub_device_stall_group() const {
    return sub_device_manager_tracker_->get_active_sub_device_manager()->get_sub_device_stall_group();
}
void MeshDevice::set_sub_device_stall_group(tt::stl::Span<const SubDeviceId> sub_device_ids) {
    sub_device_manager_tracker_->get_active_sub_device_manager()->set_sub_device_stall_group(sub_device_ids);
}
void MeshDevice::reset_sub_device_stall_group() {
    sub_device_manager_tracker_->get_active_sub_device_manager()->reset_sub_device_stall_group();
}

uint32_t MeshDevice::num_sub_devices() const {
    return sub_device_manager_tracker_->get_active_sub_device_manager()->num_sub_devices();
}

bool MeshDevice::is_mmio_capable() const {
    TT_THROW("is_mmio_capable() is not supported on MeshDevice - use individual devices instead");
    return reference_device()->is_mmio_capable();
}
std::vector<std::vector<chip_id_t>> MeshDevice::get_tunnels_from_mmio() const {
    TT_THROW("get_tunnels_from_mmio() is not supported on MeshDevice - use individual devices instead");
    return reference_device()->get_tunnels_from_mmio();
}

// Allocator methods
std::optional<DeviceAddr> MeshDevice::lowest_occupied_compute_l1_address() const {
    return sub_device_manager_tracker_->lowest_occupied_compute_l1_address();
}

std::optional<DeviceAddr> MeshDevice::lowest_occupied_compute_l1_address(
    tt::stl::Span<const SubDeviceId> sub_device_ids) const {
    return sub_device_manager_tracker_->lowest_occupied_compute_l1_address(sub_device_ids);
}

const std::unique_ptr<Allocator>& MeshDevice::allocator() const {
    return sub_device_manager_tracker_->get_default_sub_device_manager()->allocator(SubDeviceId{0});
}
const std::unique_ptr<Allocator>& MeshDevice::allocator(SubDeviceId sub_device_id) const {
    return sub_device_manager_tracker_->get_active_sub_device_manager()->allocator(sub_device_id);
}

MeshSubDeviceManagerId MeshDevice::mesh_create_sub_device_manager(
    tt::stl::Span<const SubDevice> sub_devices, DeviceAddr local_l1_size) {
    MeshSubDeviceManagerId mesh_sub_device_manager_id(*this);
    const auto& devices = scoped_devices_->root_devices();
    for (uint32_t i = 0; i < devices.size(); i++) {
        auto* device = devices[i];
        auto& sub_device_manager_id = mesh_sub_device_manager_id.sub_device_manager_ids[i];
        device->push_work([device, sub_devices, local_l1_size, &sub_device_manager_id]() {
            sub_device_manager_id = device->create_sub_device_manager(sub_devices, local_l1_size);
        });
    }
    for (auto* device : devices) {
        device->synchronize();
    }
    return mesh_sub_device_manager_id;
}

std::tuple<MeshSubDeviceManagerId, SubDeviceId> MeshDevice::mesh_create_sub_device_manager_with_fabric(tt::stl::Span<const SubDevice> sub_devices, DeviceAddr local_l1_size) {
    MeshSubDeviceManagerId mesh_sub_device_manager_id(*this);
    SubDeviceId fabric_sub_device_id;
    const auto& devices = scoped_devices_->root_devices();
    for (uint32_t i = 0; i < devices.size(); i++) {
        auto* device = devices[i];
        auto& sub_device_manager_id = mesh_sub_device_manager_id.sub_device_manager_ids[i];
        // All fabric sub-device ids will be the same, since all managers are created with the same sub_devices input
        device->push_work([device, sub_devices, local_l1_size, &sub_device_manager_id, &fabric_sub_device_id]() {
            std::tie(sub_device_manager_id, fabric_sub_device_id) = device->create_sub_device_manager_with_fabric(sub_devices, local_l1_size);
        });
    }
    for (auto* device : devices){
        device->synchronize();
    }
    return {mesh_sub_device_manager_id, fabric_sub_device_id};
}

void MeshDevice::mesh_load_sub_device_manager(MeshSubDeviceManagerId mesh_sub_device_manager_id) {
    const auto& devices = scoped_devices_->root_devices();
    for (uint32_t i = 0; i < devices.size(); i++) {
        auto* device = devices[i];
        auto sub_device_manager_id = mesh_sub_device_manager_id.sub_device_manager_ids[i];
        device->push_work(
            [device, sub_device_manager_id]() { device->load_sub_device_manager(sub_device_manager_id); });
    }
}
void MeshDevice::mesh_clear_loaded_sub_device_manager() {
    for (auto* device : scoped_devices_->root_devices()) {
        device->push_work([device]() { device->clear_loaded_sub_device_manager(); });
    }
}
void MeshDevice::mesh_remove_sub_device_manager(MeshSubDeviceManagerId mesh_sub_device_manager_id) {
    const auto& devices = scoped_devices_->root_devices();
    for (uint32_t i = 0; i < devices.size(); i++) {
        auto* device = devices[i];
        auto sub_device_manager_id = mesh_sub_device_manager_id.sub_device_manager_ids[i];
        device->push_work(
            [device, sub_device_manager_id]() { device->remove_sub_device_manager(sub_device_manager_id); });
    }
}

void MeshDevice::mesh_set_sub_device_stall_group(tt::stl::Span<const SubDeviceId> sub_device_ids) {
    for (auto* device : scoped_devices_->root_devices()) {
        device->push_work([device, sub_device_ids=std::vector<SubDeviceId>(sub_device_ids.begin(), sub_device_ids.end())]() { device->set_sub_device_stall_group(sub_device_ids); });
    }
}

void MeshDevice::mesh_reset_sub_device_stall_group() {
    for (auto* device : scoped_devices_->root_devices()) {
        device->push_work([device]() { device->reset_sub_device_stall_group(); });
    }
}

MeshSubDeviceManagerId::MeshSubDeviceManagerId(const MeshDevice& mesh_device) {
    this->sub_device_manager_ids.resize(mesh_device.num_devices());
}


}  // namespace tt::tt_metal::distributed
