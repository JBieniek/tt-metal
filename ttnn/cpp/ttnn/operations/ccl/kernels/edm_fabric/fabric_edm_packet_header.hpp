// SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.
//
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace tt::fabric {

enum TerminationSignal : uint32_t {
    KEEP_RUNNING = 0,

    // Wait for messages to drain
    GRACEFULLY_TERMINATE = 1,

    // Immediately terminate - don't wait for any outstanding messages to arrive or drain out
    IMMEDIATELY_TERMINATE = 2
};

// 3 bits
enum NocSendType : uint8_t {
    NOC_UNICAST_WRITE = 0,
    NOC_UNICAST_INLINE_WRITE = 1,
    NOC_MULTICAST_WRITE = 2,
    NOC_UNICAST_ATOMIC_INC = 3,
    NOC_MULTICAST_ATOMIC_INC = 4,
    NOC_SEND_TYPE_LAST = NOC_MULTICAST_ATOMIC_INC
};
// How to send the payload across the cluster
// 1 bit
enum ChipSendType : uint8_t {
    CHIP_UNICAST = 0,
    CHIP_MULTICAST = 1,
    CHIP_SEND_TYPE_LAST = CHIP_MULTICAST
};

struct RoutingFields {
    static constexpr uint8_t START_DISTANCE_FIELD_BIT_WIDTH = 4;
    static constexpr uint8_t RANGE_HOPS_FIELD_BIT_WIDTH = 4;
    static constexpr uint8_t LAST_HOP_DISTANCE_VAL = 1;
    static constexpr uint8_t LAST_CHIP_IN_MCAST_VAL = 1 << tt::fabric::RoutingFields::START_DISTANCE_FIELD_BIT_WIDTH;
    static constexpr uint8_t HOP_DISTANCE_MASK = (1 << tt::fabric::RoutingFields::RANGE_HOPS_FIELD_BIT_WIDTH) - 1;
    static constexpr uint8_t RANGE_MASK = ((1 << tt::fabric::RoutingFields::RANGE_HOPS_FIELD_BIT_WIDTH) - 1)
                                          << tt::fabric::RoutingFields::START_DISTANCE_FIELD_BIT_WIDTH;
    static constexpr uint8_t LAST_MCAST_VAL = LAST_CHIP_IN_MCAST_VAL | LAST_HOP_DISTANCE_VAL;

    uint8_t value;
};
static_assert(sizeof(RoutingFields) == sizeof(uint8_t), "RoutingFields size is not 1 bytes");
static_assert((RoutingFields::START_DISTANCE_FIELD_BIT_WIDTH + RoutingFields::RANGE_HOPS_FIELD_BIT_WIDTH) <= sizeof(RoutingFields) * 8, "START_DISTANCE_FIELD_BIT_WIDTH + RANGE_HOPS_FIELD_BIT_WIDTH must equal 8");

struct MulticastRoutingCommandHeader {
    uint8_t start_distance_in_hops: RoutingFields::START_DISTANCE_FIELD_BIT_WIDTH;
    uint8_t range_hops: RoutingFields::RANGE_HOPS_FIELD_BIT_WIDTH; // 0 implies unicast
};
static_assert(sizeof(MulticastRoutingCommandHeader) <= sizeof(RoutingFields), "MulticastRoutingCommandHeader size is not 1 byte");

struct NocUnicastCommandHeader {
    uint64_t noc_address;
};
struct NocUnicastInlineWriteCommandHeader {
    uint64_t noc_address;
    uint32_t value;
};
struct NocUnicastAtomicIncCommandHeader {
    NocUnicastAtomicIncCommandHeader(uint64_t noc_address, uint16_t val, uint16_t wrap)
        : noc_address(noc_address), val(val), wrap(wrap) {}

    uint64_t noc_address;
    uint16_t val;
    uint16_t wrap;
};
struct NocMulticastCommandHeader {
    uint32_t address;
    uint8_t noc_x_start;
    uint8_t noc_y_start;
    uint8_t mcast_rect_size_x;
    uint8_t mcast_rect_size_y;
};
struct NocMulticastAtomicIncCommandHeader {
    uint32_t address;
    uint16_t val;
    uint16_t wrap;
    uint8_t noc_x_start;
    uint8_t noc_y_start;
    uint8_t size_x;
    uint8_t size_y;
};
static_assert(sizeof(NocUnicastCommandHeader) == 8, "NocUnicastCommandHeader size is not 8 bytes");
static_assert(sizeof(NocMulticastCommandHeader) == 8, "NocMulticastCommandHeader size is not 8 bytes");
static_assert(sizeof(NocUnicastInlineWriteCommandHeader) == 16, "NocMulticastCommandHeader size is not 16 bytes");
static_assert(sizeof(NocUnicastAtomicIncCommandHeader) == 16, "NocUnicastCommandHeader size is not 16 bytes");
static_assert(sizeof(NocMulticastAtomicIncCommandHeader) == 12, "NocAtomicIncCommandHeader size is not 12 bytes");
union NocCommandFields{
    NocUnicastCommandHeader unicast_write;
    NocUnicastInlineWriteCommandHeader unicast_inline_write;
    NocMulticastCommandHeader mcast_write;
    NocUnicastAtomicIncCommandHeader unicast_seminc;
    NocMulticastAtomicIncCommandHeader mcast_seminc;
} ;
static_assert(sizeof(NocCommandFields) <= 16, "CommandFields size is not 16 bytes");

// TODO: wrap this in a debug version that holds type info so we can assert for field/command/
template <typename Derived>
struct PacketHeaderBase {
    NocCommandFields command_fields; // size = 16B due to uint64_t alignment
    uint16_t payload_size_bytes;
    // TODO: trim this down noc_send_type 2 bits (4 values):
    //   -> unicast_write, mcast_write, unicast_seminc, mcast_seminc
    // For now, kept it separate so I could do reads which would be handled differently
    // but for our purposes we shouldn't need read so we should be able to omit the support
    NocSendType noc_send_type : 3;
    // ChipSendType only used by PacketHeader, but keep here for now for bit-fields
    ChipSendType chip_send_type : 1;
    // Used only by the EDM sender and receiver channels. Populated by EDM sender channel to
    // indicate to the receiver channel what channel was the source of this packet. Reserved
    // otherwise.
    uint8_t src_ch_id : 4;

    // Returns size of payload in bytes - TODO: convert to words (4B)
    size_t get_payload_size_excluding_header() volatile const {
        return this->payload_size_bytes;
    }

    inline size_t get_payload_size_including_header() volatile const {
        return get_payload_size_excluding_header() + sizeof(Derived);
    }

    // Setters for noc_send_type, routing_fields, and command_fields
    inline void set_noc_send_type(NocSendType &type) { this->noc_send_type = type; }
    inline void set_command_fields(NocCommandFields &fields) { this->command_fields = fields; }

    inline Derived &to_chip_unicast(uint8_t distance_in_hops) {
        static_cast<Derived*>(this)->to_chip_unicast_impl(distance_in_hops);
        return *static_cast<Derived*>(this);
    }

    inline Derived &to_chip_multicast(MulticastRoutingCommandHeader const &mcast_routing_command_header) {
        static_cast<Derived*>(this)->to_chip_multicast_impl(mcast_routing_command_header);
        return *static_cast<Derived*>(this);
    }

    inline Derived &to_noc_unicast_write(NocUnicastCommandHeader const &noc_unicast_command_header, size_t payload_size_bytes) {
        this->noc_send_type = NOC_UNICAST_WRITE;
        this->command_fields.unicast_write = noc_unicast_command_header;
        this->payload_size_bytes = payload_size_bytes;
        return *static_cast<Derived*>(this);
    }

    inline Derived &to_noc_unicast_inline_write(NocUnicastInlineWriteCommandHeader const &noc_unicast_command_header) {
        this->noc_send_type = NOC_UNICAST_INLINE_WRITE;
        this->command_fields.unicast_inline_write = noc_unicast_command_header;
        this->payload_size_bytes = 0;
        return *static_cast<Derived*>(this);
    }

    inline Derived &to_noc_multicast(NocMulticastCommandHeader const &noc_multicast_command_header, size_t payload_size_bytes) {
        this->noc_send_type = NOC_MULTICAST_WRITE;
        this->command_fields.mcast_write = noc_multicast_command_header;
        this->payload_size_bytes = payload_size_bytes;
        return *static_cast<Derived*>(this);
    }

    inline Derived &to_noc_unicast_atomic_inc(NocUnicastAtomicIncCommandHeader const &noc_unicast_atomic_inc_command_header) {
        this->noc_send_type = NOC_UNICAST_ATOMIC_INC;
        this->command_fields.unicast_seminc = noc_unicast_atomic_inc_command_header;
        this->payload_size_bytes = 0;
        return *static_cast<Derived*>(this);
    }

    inline Derived &to_noc_multicast_atomic_inc(
        NocMulticastAtomicIncCommandHeader const &noc_multicast_atomic_inc_command_header, size_t payload_size_bytes) {
        this->noc_send_type = NOC_MULTICAST_ATOMIC_INC;
        this->command_fields.mcast_seminc = noc_multicast_atomic_inc_command_header;
        this->payload_size_bytes = payload_size_bytes;
        return *static_cast<Derived*>(this);
    }

    inline volatile Derived* to_chip_unicast(uint8_t distance_in_hops) volatile {
        static_cast<volatile Derived*>(this)->to_chip_unicast_impl(distance_in_hops);
        return static_cast<volatile Derived*>(this);
    }

    inline volatile Derived* to_chip_multicast(MulticastRoutingCommandHeader const &mcast_routing_command_header) volatile {
        static_cast<volatile Derived*>(this)->to_chip_multicast_impl(mcast_routing_command_header);
        return static_cast<volatile Derived*>(this);
    }

    inline volatile Derived* to_noc_unicast_write(NocUnicastCommandHeader const &noc_unicast_command_header, size_t payload_size_bytes) volatile {
        this->noc_send_type = NOC_UNICAST_WRITE;
        this->command_fields.unicast_write.noc_address = noc_unicast_command_header.noc_address;
        this->payload_size_bytes = payload_size_bytes;
        return static_cast<volatile Derived*>(this);
    }

    inline volatile Derived* to_noc_unicast_inline_write(NocUnicastInlineWriteCommandHeader const &noc_unicast_command_header) volatile {
        this->noc_send_type = NOC_UNICAST_INLINE_WRITE;
        this->command_fields.unicast_inline_write.noc_address = noc_unicast_command_header.noc_address;
        this->command_fields.unicast_inline_write.value = noc_unicast_command_header.value;
        this->payload_size_bytes = 0;
        return static_cast<volatile Derived*>(this);
    }

    inline volatile Derived* to_noc_multicast(NocMulticastCommandHeader const &noc_multicast_command_header, size_t payload_size_bytes) volatile {
        this->noc_send_type = NOC_MULTICAST_WRITE;
        this->command_fields.mcast_write.mcast_rect_size_x = noc_multicast_command_header.mcast_rect_size_x;
        this->command_fields.mcast_write.mcast_rect_size_y = noc_multicast_command_header.mcast_rect_size_y;
        this->command_fields.mcast_write.noc_x_start = noc_multicast_command_header.noc_x_start;
        this->command_fields.mcast_write.noc_y_start = noc_multicast_command_header.noc_y_start;
        this->payload_size_bytes = payload_size_bytes;
        this->command_fields.mcast_write.address = noc_multicast_command_header.address;
        return static_cast<volatile Derived*>(this);
    }

    inline volatile Derived* to_noc_unicast_atomic_inc(NocUnicastAtomicIncCommandHeader const &noc_unicast_atomic_inc_command_header) volatile {
        this->noc_send_type = NOC_UNICAST_ATOMIC_INC;
        this->command_fields.unicast_seminc.noc_address = noc_unicast_atomic_inc_command_header.noc_address;
        this->command_fields.unicast_seminc.val = noc_unicast_atomic_inc_command_header.val;
        this->command_fields.unicast_seminc.wrap = noc_unicast_atomic_inc_command_header.wrap;
        this->payload_size_bytes = 0;
        return static_cast<volatile Derived*>(this);
    }

    inline volatile Derived *to_noc_multicast_atomic_inc(
        NocMulticastAtomicIncCommandHeader const &noc_multicast_atomic_inc_command_header, size_t payload_size_bytes) volatile {
        this->noc_send_type = NOC_MULTICAST_ATOMIC_INC;
        this->command_fields.mcast_seminc.address = noc_multicast_atomic_inc_command_header.address;
        this->command_fields.mcast_seminc.noc_x_start = noc_multicast_atomic_inc_command_header.noc_x_start;
        this->command_fields.mcast_seminc.noc_y_start = noc_multicast_atomic_inc_command_header.noc_y_start;
        this->command_fields.mcast_seminc.size_x = noc_multicast_atomic_inc_command_header.size_x;
        this->command_fields.mcast_seminc.size_y = noc_multicast_atomic_inc_command_header.size_y;
        this->command_fields.mcast_seminc.val = noc_multicast_atomic_inc_command_header.val;
        this->command_fields.mcast_seminc.wrap = noc_multicast_atomic_inc_command_header.wrap;
        this->payload_size_bytes = payload_size_bytes;
        return static_cast<volatile Derived*>(this);
    }

    inline void set_src_ch_id(uint8_t ch_id) volatile {
        this->src_ch_id = ch_id;
    }
};

struct PacketHeader : public PacketHeaderBase<PacketHeader> {
    RoutingFields routing_fields;
    // Sort of hack to work-around DRAM read alignment issues that must be 32B aligned
    // To simplify worker kernel code, we for now decide to pad up the packet header
    // to 32B so the user can simplify shift into their CB chunk by sizeof(tt::fabric::PacketHeader)
    // and automatically work around the DRAM read alignment bug.
    //
    // Future changes will remove this padding and require the worker kernel to be aware of this bug
    // and pad their own CBs conditionally when reading from DRAM. It'll be up to the users to
    // manage this complexity.
    uint32_t padding0;
    uint32_t padding1;

    private:

    inline static uint32_t calculate_chip_unicast_routing_fields_value(uint8_t distance_in_hops) {
        return RoutingFields::LAST_CHIP_IN_MCAST_VAL | distance_in_hops;
    }
    inline static uint32_t calculate_chip_multicast_routing_fields_value(
        const MulticastRoutingCommandHeader& chip_multicast_command_header) {
        return ((static_cast<uint8_t>(chip_multicast_command_header.range_hops) << RoutingFields::START_DISTANCE_FIELD_BIT_WIDTH)) | static_cast<uint8_t>(chip_multicast_command_header.start_distance_in_hops);
    }

    public:

    // Setters for PacketHeader-specific fields
    inline void set_chip_send_type(ChipSendType &type) { this->chip_send_type = type; }

    inline void set_routing_fields(RoutingFields &fields) { this->routing_fields = fields; }

    inline void to_chip_unicast_impl(uint8_t distance_in_hops) {
        this->chip_send_type = CHIP_UNICAST;
        this->routing_fields.value = PacketHeader::calculate_chip_unicast_routing_fields_value(distance_in_hops);
    }
    inline void to_chip_multicast_impl(MulticastRoutingCommandHeader const &chip_multicast_command_header) {
        this->chip_send_type = CHIP_MULTICAST;
        this->routing_fields.value = PacketHeader::calculate_chip_multicast_routing_fields_value(chip_multicast_command_header);
    }

    inline void to_chip_unicast_impl(uint8_t distance_in_hops) volatile {
        this->chip_send_type = CHIP_UNICAST;
        this->routing_fields.value = PacketHeader::calculate_chip_unicast_routing_fields_value(distance_in_hops);
    }
    inline void to_chip_multicast_impl(MulticastRoutingCommandHeader const &chip_multicast_command_header) volatile{
        this->chip_send_type = CHIP_MULTICAST;
        this->routing_fields.value = PacketHeader::calculate_chip_multicast_routing_fields_value(chip_multicast_command_header);
    }
};

struct LowLatencyRoutingFields {
    static constexpr uint32_t FIELD_WIDTH = 2;
    static constexpr uint32_t FIELD_MASK = 0b11;
    static constexpr uint32_t NOOP = 0b00;
    static constexpr uint32_t WRITE_ONLY = 0b01;
    static constexpr uint32_t FORWARD_ONLY = 0b10;
    static constexpr uint32_t WRITE_AND_FORWARD = 0b11;
    static constexpr uint32_t FWD_ONLY_FIELD = 0xAAAAAAAA;
    static constexpr uint32_t WR_AND_FWD_FIELD = 0xFFFFFFFF;
    uint32_t value;
};

struct LowLatencyPacketHeader : public PacketHeaderBase<LowLatencyPacketHeader> {
    uint8_t padding0;
    LowLatencyRoutingFields routing_fields;
    uint32_t padding1;

    private:

    inline static uint32_t calculate_chip_unicast_routing_fields_value(uint8_t distance_in_hops) {
        // Example of unicast 3 hops away
        // First line will do 0xAAAAAAAA & 0b1111 = 0b1010. This means starting from our neighbor, we will forward twice (forward to neighbor is not encoded in the field)
        // Last line will do 0b01 << 4 = 0b010000. This means that on the 3rd chip, we will write only
        // Together this means the final encoding is 0b011010
        return
            (LowLatencyRoutingFields::FWD_ONLY_FIELD & ((1 << (distance_in_hops - 1) * LowLatencyRoutingFields::FIELD_WIDTH) - 1)) |
            (LowLatencyRoutingFields::WRITE_ONLY << (distance_in_hops - 1) * LowLatencyRoutingFields::FIELD_WIDTH);
    }
    inline static uint32_t calculate_chip_multicast_routing_fields_value(
        const MulticastRoutingCommandHeader& chip_multicast_command_header) {
        // Example of starting 3 hops away mcasting to 2 chips
        // First line will do 0xAAAAAAAA & 0b1111 = 0b1010. This means starting from our neighbor, we will forward twice (forward to neighbor is not encoded in the field)
        // Second line will do 0xFFFFFFFF & 0b11 = 0b11. 0b11 << 4 = 0b110000. This means starting from the 3rd chip, we will write and forward once
        // Last line will do 0b01 << 6 = 0b01000000. This means that on the 5th chip, we will write only
        // Together this means the final encoding is 0b01111010
        return
            (LowLatencyRoutingFields::FWD_ONLY_FIELD & ((1 << (chip_multicast_command_header.start_distance_in_hops - 1) * LowLatencyRoutingFields::FIELD_WIDTH) - 1)) |
            (LowLatencyRoutingFields::WR_AND_FWD_FIELD & ((1 << (chip_multicast_command_header.range_hops - 1) * LowLatencyRoutingFields::FIELD_WIDTH) - 1) <<
            ((chip_multicast_command_header.start_distance_in_hops - 1) * LowLatencyRoutingFields::FIELD_WIDTH)) |
            (LowLatencyRoutingFields::WRITE_ONLY << (chip_multicast_command_header.start_distance_in_hops + chip_multicast_command_header.range_hops - 2) * LowLatencyRoutingFields::FIELD_WIDTH);
    }

    public:

    // Specialized implementations for LowLatencyPacketHeader
    inline void set_routing_fields(LowLatencyRoutingFields &fields) {
        this->routing_fields = fields;
    }

    inline void to_chip_unicast_impl(uint8_t distance_in_hops) {
        this->routing_fields.value = LowLatencyPacketHeader::calculate_chip_unicast_routing_fields_value(distance_in_hops);
    }
    inline void to_chip_multicast_impl(
        const MulticastRoutingCommandHeader& chip_multicast_command_header) {
        this->routing_fields.value = LowLatencyPacketHeader::calculate_chip_multicast_routing_fields_value(chip_multicast_command_header);
    }

    inline void to_chip_unicast_impl(uint8_t distance_in_hops) volatile {
        this->routing_fields.value = LowLatencyPacketHeader::calculate_chip_unicast_routing_fields_value(distance_in_hops);
    }
    inline void to_chip_multicast_impl(
        const MulticastRoutingCommandHeader& chip_multicast_command_header) volatile {
        this->routing_fields.value = LowLatencyPacketHeader::calculate_chip_multicast_routing_fields_value(chip_multicast_command_header);
    }
};

// TODO: When we remove the 32B padding requirement, reduce to 16B size check
static_assert(sizeof(PacketHeader) == 32, "sizeof(PacketHeader) is not equal to 32B");
// Host code still hardcoded to sizeof(PacketHeader) so we need to keep this check
static_assert(sizeof(LowLatencyPacketHeader) == sizeof(PacketHeader), "sizeof(LowLatencyPacketHeader) is not equal to 32B");

static constexpr size_t header_size_bytes = sizeof(PacketHeader);

#define FABRIC_LOW_LATENCY_MODE 1

#if defined FABRIC_LOW_LATENCY_MODE and FABRIC_LOW_LATENCY_MODE == 1
#define PACKET_HEADER_TYPE tt::fabric::LowLatencyPacketHeader
#define ROUTING_FIELDS_TYPE tt::fabric::LowLatencyRoutingFields
#else
#define PACKET_HEADER_TYPE tt::fabric::PacketHeader
#define ROUTING_FIELDS_TYPE tt::fabric::RoutingFields
#endif


} // namespace tt::fabric
