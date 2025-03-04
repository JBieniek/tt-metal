// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
//
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
/**
 * NOC APIs are prefixed w/ "ncrisc" (legacy name) but there's nothing NCRISC specific, they can be used on BRISC or
 * other RISCs Any two RISC processors cannot use the same CMD_BUF non_blocking APIs shouldn't be mixed with slow noc.h
 * APIs explicit flushes need to be used since the calls are non-blocking
 * */
constexpr static std::uint32_t VALID_VAL = 0x1234;
constexpr static std::uint32_t INVALID_VAL = 0x4321;
void kernel_main() {
    std::uint32_t dram_buffer_src_addr_base         = get_arg_val<uint32_t>(0);
    std::uint32_t bank_id                           = get_arg_val<uint32_t>(1);
    std::uint32_t local_buffer_addr                 = get_arg_val<uint32_t>(2);
    std::uint32_t consumer_core_noc_x               = get_arg_val<uint32_t>(3);
    std::uint32_t consumer_core_noc_y               = get_arg_val<uint32_t>(4);
    std::uint32_t stream_register_address           = get_arg_val<uint32_t>(5);
    std::uint32_t num_tiles                         = get_arg_val<uint32_t>(6);
    std::uint32_t transient_buffer_size_tiles       = get_arg_val<uint32_t>(7);
    std::uint32_t transient_buffer_size_bytes       = get_arg_val<uint32_t>(8);

    // Scratch address in L1, to write register value before we copy it to into local/remote registers
    volatile tt_l1_ptr uint32_t* constant_ptr = reinterpret_cast<volatile tt_l1_ptr uint32_t*>(CONSTANT_REGISTER_VALUE);
    *(constant_ptr) = VALID_VAL;
    // Local and remote register addresses (used for sync)
    std::uint64_t local = get_noc_addr(stream_register_address);
    std::uint64_t remote = get_noc_addr(consumer_core_noc_x, consumer_core_noc_y, stream_register_address);

    // keeps track of how many tiles we moved so far
    std::uint32_t counter = 0;
    std::uint32_t dram_buffer_src_addr = dram_buffer_src_addr_base;
    while (counter < num_tiles) {
        // DRAM NOC src address
        std::uint64_t dram_buffer_src_noc_addr = get_noc_addr_from_bank_id<true>(bank_id, dram_buffer_src_addr);
        // Wait until sync register is INVALID_VAL (means its safe to corrupt destination buffer)
        wait_for_sync_register_value(stream_register_address, INVALID_VAL);
        // Copy data from dram into destination buffer
        noc_async_read(dram_buffer_src_noc_addr, local_buffer_addr, transient_buffer_size_bytes);
        dram_buffer_src_addr += transient_buffer_size_bytes;
        // wait all reads flushed (ie received)
        noc_async_read_barrier();

        // Write VALID_VAL into local register
        noc_async_write(CONSTANT_REGISTER_VALUE, local, 4);
        noc_async_write_barrier();

        // Write VALID_VAL into remote register
        noc_async_write(CONSTANT_REGISTER_VALUE, remote, 4);
        noc_async_write_barrier();

        counter += transient_buffer_size_tiles;
    }
}
