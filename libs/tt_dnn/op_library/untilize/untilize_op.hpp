#pragma once

#include "tensor/tensor.hpp"

namespace tt {

namespace tt_metal {

// TODO: Accept parallelization

Tensor untilize (const Tensor &a);
Tensor untilize_with_unpadding(const Tensor &a, const std::array<uint32_t, 4> &output_tensor_start, const std::array<uint32_t, 4> &output_tensor_end);

}  // namespace tt_metal

}  // namespace tt
