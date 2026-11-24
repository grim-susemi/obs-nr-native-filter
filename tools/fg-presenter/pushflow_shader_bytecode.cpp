// SPDX-License-Identifier: GPL-3.0-only
#include "vfg_gpu.h"
#include "pushflow_pack_shader.h"
#include "pushflow_unpack_shader.h"
#include "pushflow_reconstruct_shader.h"

namespace nr::fg::vfg {
PushFlowShaderBytecodeSet make_pushflow_shader_bytecode() noexcept
{
    return {
        {pushflow_pack_shader, sizeof(pushflow_pack_shader)},
        {pushflow_unpack_shader, sizeof(pushflow_unpack_shader)},
        {pushflow_reconstruct_shader, sizeof(pushflow_reconstruct_shader)},
    };
}
} // namespace nr::fg::vfg
