// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>
#include "common/types.h"

namespace Shader {
struct HwFragmentRuntimeInfo;
}

namespace Shader::Backend::SPIRV {

enum class AuxShaderType : u32 {
    RectListTCS,
    QuadListTCS,
    PassthroughTES,
};

[[nodiscard]] std::vector<u32> EmitAuxilaryTessShader(AuxShaderType type,
                                                      const HwFragmentRuntimeInfo& fs_info);

/// bbport (#16/#18): a geometry shader that clips each triangle against the camera plane, the
/// depth planes (when depth clipping is on) and a guard band itself, computing every cut point
/// from the edge's endpoints in a canonical order, so triangles sharing an edge get bit-identical
/// cut points and the host GPU has nothing left to clip. Triangles needing no clipping pass
/// through unchanged.
[[nodiscard]] std::vector<u32> EmitClipGeometryShader(const HwFragmentRuntimeInfo& fs_info,
                                                      bool provoking_last, bool near_minus_w,
                                                      bool depth_clip);

} // namespace Shader::Backend::SPIRV
