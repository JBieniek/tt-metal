// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
//
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>

#include "compute_kernel_api/untilize.h"
// #include "debug/dprint.h"

namespace NAMESPACE {
void MAIN {
    uint32_t per_core_block_cnt = get_compile_time_arg_val(0);
    uint32_t per_core_block_tile_cnt = get_compile_time_arg_val(1);
    uint32_t src_cb_id = get_compile_time_arg_val(2);
    uint32_t out_cb_id = get_compile_time_arg_val(3);
    untilize_init(src_cb_id, out_cb_id);

    // UNPACK(( DPRINT << "Block count=" << uint32_t(per_core_block_cnt) << " tile count=" << per_core_block_tile_cnt <<
    // ENDL() ));

    for (uint32_t b = 0; b < per_core_block_cnt; ++b) {
        cb_wait_front(src_cb_id, per_core_block_tile_cnt);
        cb_reserve_back(out_cb_id, per_core_block_tile_cnt);

        untilize_block(src_cb_id, per_core_block_tile_cnt, out_cb_id);

        cb_push_back(out_cb_id, per_core_block_tile_cnt);
        cb_pop_front(src_cb_id, per_core_block_tile_cnt);
    }
}
}  // namespace NAMESPACE
