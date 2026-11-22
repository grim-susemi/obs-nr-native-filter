// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <onnxruntime_c_api.h>
#include <array>
#include <exception>
#include <string_view>

namespace nr::fg::vfg::pushflow {
using Shape = std::array<int64_t, 4>;
enum class OutputLayout { Rgb, Grid, Mask };

inline OutputLayout output_layout(size_t slot)
{
    return slot == 1 ? OutputLayout::Rgb : slot == 4 ? OutputLayout::Mask : OutputLayout::Grid;
}

inline Shape output_shape(OutputLayout layout, const Shape& input)
{
    const auto h = input[2], w = input[3];
    switch (layout) {
    case OutputLayout::Rgb: return {1, 3, h, w};
    case OutputLayout::Grid: return {1, h, w, 2};
    case OutputLayout::Mask: return {1, 1, h, w};
    }
    std::terminate();
}

struct OutputMetadata {
    ONNXTensorElementDataType element{};
    size_t rank{};
    Shape shape{};
    std::array<std::string_view, 4> symbols{};
};

inline bool compatible_output(const OutputMetadata& output, OutputLayout layout, const Shape& input)
{
    if (output.element != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT || output.rank != output.shape.size()) return false;
    const auto expected = output_shape(layout, input);
    const size_t h = layout == OutputLayout::Grid ? 1 : 2;
    for (size_t axis = 0; axis < expected.size(); ++axis) {
        if (output.shape[axis] == expected[axis]) continue;
        if (output.shape[axis] != -1) return false;
        // ORT may retain dimension parameters on outputs of the specialized
        // graph. Accept only the known raw or companion names on the right axes.
        const auto name = output.symbols[axis];
        if ((axis == 0 && name == "batch") ||
            (axis == h && (name == "height" || name == "pushflow_h")) ||
            (axis == h + 1 && (name == "width" || name == "pushflow_w"))) continue;
        return false;
    }
    return true;
}
} // namespace nr::fg::vfg::pushflow
