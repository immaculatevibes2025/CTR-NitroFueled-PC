// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <sirit/sirit.h>
#include "shader_recompiler/backend/spirv/emit_spirv_quad_rect.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Backend::SPIRV {

using Sirit::Id;

constexpr u32 SPIRV_VERSION_1_5 = 0x00010500;

struct QuadRectListEmitter : public Sirit::Module {
    explicit QuadRectListEmitter(const HwFragmentRuntimeInfo& fs_info_)
        : Sirit::Module{SPIRV_VERSION_1_5}, fs_info{fs_info_} {
        void_id = TypeVoid();
        bool_id = TypeBool();
        float_id = TypeFloat(32);
        uint_id = TypeUInt(32U);
        int_id = TypeInt(32U, true);
        bvec2_id = TypeVector(bool_id, 2);
        vec2_id = TypeVector(float_id, 2);
        vec3_id = TypeVector(float_id, 3);
        vec4_id = TypeVector(float_id, 4);

        float_one = Constant(float_id, 1.0f);
        float_min_one = Constant(float_id, -1.0f);
        int_zero = Constant(int_id, 0);

        const Id float_arr{TypeArray(float_id, Constant(uint_id, 1U))};
        gl_per_vertex_type = TypeStruct(vec4_id, float_id, float_arr, float_arr);
        Decorate(gl_per_vertex_type, spv::Decoration::Block);
        MemberDecorate(gl_per_vertex_type, 0U, spv::Decoration::BuiltIn,
                       static_cast<u32>(spv::BuiltIn::Position));
        MemberDecorate(gl_per_vertex_type, 1U, spv::Decoration::BuiltIn,
                       static_cast<u32>(spv::BuiltIn::PointSize));
        MemberDecorate(gl_per_vertex_type, 2U, spv::Decoration::BuiltIn,
                       static_cast<u32>(spv::BuiltIn::ClipDistance));
        MemberDecorate(gl_per_vertex_type, 3U, spv::Decoration::BuiltIn,
                       static_cast<u32>(spv::BuiltIn::CullDistance));
    }

    /// Emits tessellation control shader for interpolating the 4th vertex of rectange primitive
    void EmitRectListTCS() {
        DefineEntry(spv::ExecutionModel::TessellationControl);

        // Set passthrough tessellation factors
        const Id output_float_id{TypePointer(spv::StorageClass::Output, float_id)};
        for (int i = 0; i < 4; i++) {
            const Id ptr{OpAccessChain(output_float_id, gl_tess_level_outer, Int(i))};
            OpStore(ptr, float_one);
        }
        for (int i = 0; i < 2; i++) {
            const Id ptr{OpAccessChain(output_float_id, gl_tess_level_inner, Int(i))};
            OpStore(ptr, float_one);
        }

        const Id input_vec4{TypePointer(spv::StorageClass::Input, vec4_id)};
        const Id output_vec4{TypePointer(spv::StorageClass::Output, vec4_id)};

        // Emit interpolation block of the 4th vertex in rect.
        // Load positions
        std::array<Id, 3> pos;
        for (int i = 0; i < 3; i++) {
            pos[i] = OpLoad(vec4_id, OpAccessChain(input_vec4, gl_in, Int(i), int_zero));
        }

        std::array<Id, 3> point_coord_equal;
        for (int i = 0; i < 3; i++) {
            // point_coord_equal[i] = equal(gl_in[i].gl_Position.xy, gl_in[(i + 1) %
            // 3].gl_Position.xy);
            const Id pos_l_xy{OpVectorShuffle(vec2_id, pos[i], pos[i], 0, 1)};
            const Id pos_r_xy{OpVectorShuffle(vec2_id, pos[(i + 1) % 3], pos[(i + 1) % 3], 0, 1)};
            point_coord_equal[i] = OpFOrdEqual(bvec2_id, pos_l_xy, pos_r_xy);
        }

        std::array<Id, 3> bary_coord;
        std::array<Id, 3> is_edge_vertex;
        for (int i = 0; i < 3; i++) {
            // bool xy_equal = point_coord_equal[i].x && point_coord_equal[(i + 2) % 3].y;
            const Id xy_equal{
                OpLogicalAnd(bool_id, OpCompositeExtract(bool_id, point_coord_equal[i], 0),
                             OpCompositeExtract(bool_id, point_coord_equal[(i + 2) % 3], 1))};
            // bool yx_equal = point_coord_equal[i].y && point_coord_equal[(i + 2) % 3].x;
            const Id yx_equal{
                OpLogicalAnd(bool_id, OpCompositeExtract(bool_id, point_coord_equal[i], 1),
                             OpCompositeExtract(bool_id, point_coord_equal[(i + 2) % 3], 0))};
            // bary_coord[i] = (xy_equal || yx_equal) ? -1.f : 1.f;
            is_edge_vertex[i] = OpLogicalOr(bool_id, xy_equal, yx_equal);
            bary_coord[i] = OpSelect(float_id, is_edge_vertex[i], float_min_one, float_one);
        }

        const auto interpolate = [&](Id v0, Id v1, Id v2) {
            // return v0 * bary_coord.x + v1 * bary_coord.y + v2 * bary_coord.z;
            const Id p0{OpVectorTimesScalar(vec4_id, v0, bary_coord[0])};
            const Id p1{OpVectorTimesScalar(vec4_id, v1, bary_coord[1])};
            const Id p2{OpVectorTimesScalar(vec4_id, v2, bary_coord[2])};
            return OpFAdd(vec4_id, p0, OpFAdd(vec4_id, p1, p2));
        };

        // int vertex_index_id = is_edge_vertex[1] ? 1 : (is_edge_vertex[2] ? 2 : 0);
        Id vertex_index{OpSelect(int_id, is_edge_vertex[2], Int(2), Int(0))};
        vertex_index = OpSelect(int_id, is_edge_vertex[1], Int(1), vertex_index);

        // int index = (vertex_index_id + gl_InvocationID) % 3;
        const Id invocation_id{OpLoad(int_id, gl_invocation_id)};
        const Id invocation_3{OpIEqual(bool_id, invocation_id, Int(3))};
        const Id index{OpSMod(int_id, OpIAdd(int_id, vertex_index, invocation_id), Int(3))};

        // gl_out[gl_InvocationID].gl_Position = gl_InvocationID == 3 ? pos3 :
        // gl_in[index].gl_Position;
        const Id pos3{interpolate(pos[0], pos[1], pos[2])};
        const Id in_ptr{OpAccessChain(input_vec4, gl_in, index, Int(0))};
        const Id position{OpSelect(vec4_id, invocation_3, pos3, OpLoad(vec4_id, in_ptr))};
        OpStore(OpAccessChain(output_vec4, gl_out, invocation_id, Int(0)), position);

        // Set attributes
        for (int i = 0; i < inputs.size(); i++) {
            // vec4 in_paramN3 = interpolate(bary_coord, in_paramN[0], in_paramN[1], in_paramN[2]);
            const Id v0{OpLoad(vec4_id, OpAccessChain(input_vec4, inputs[i], Int(0)))};
            const Id v1{OpLoad(vec4_id, OpAccessChain(input_vec4, inputs[i], Int(1)))};
            const Id v2{OpLoad(vec4_id, OpAccessChain(input_vec4, inputs[i], Int(2)))};
            const Id in_param3{interpolate(v0, v1, v2)};
            // out_paramN[gl_InvocationID] = gl_InvocationID == 3 ? in_paramN3 : in_paramN[index];
            const Id in_param{OpLoad(vec4_id, OpAccessChain(input_vec4, inputs[i], index))};
            const Id out_param{OpSelect(vec4_id, invocation_3, in_param3, in_param)};
            OpStore(OpAccessChain(output_vec4, outputs[i], invocation_id), out_param);
        }

        OpReturn();
        OpFunctionEnd();
    }

    /// Emits a passthrough quad tessellation control shader that outputs 4 control points.
    void EmitQuadListTCS() {
        DefineEntry(spv::ExecutionModel::TessellationControl);
        const Id array_type{TypeArray(int_id, Int(4))};
        const Id values{ConstantComposite(array_type, Int(1), Int(2), Int(0), Int(3))};
        const Id indices{AddLocalVariable(TypePointer(spv::StorageClass::Function, array_type),
                                          spv::StorageClass::Function, values)};

        // Set passthrough tessellation factors
        const Id output_float{TypePointer(spv::StorageClass::Output, float_id)};
        for (int i = 0; i < 4; i++) {
            const Id ptr{OpAccessChain(output_float, gl_tess_level_outer, Int(i))};
            OpStore(ptr, float_one);
        }
        for (int i = 0; i < 2; i++) {
            const Id ptr{OpAccessChain(output_float, gl_tess_level_inner, Int(i))};
            OpStore(ptr, float_one);
        }

        const Id input_vec4{TypePointer(spv::StorageClass::Input, vec4_id)};
        const Id output_vec4{TypePointer(spv::StorageClass::Output, vec4_id)};
        const Id func_int{TypePointer(spv::StorageClass::Function, int_id)};
        const Id invocation_id{OpLoad(int_id, gl_invocation_id)};
        const Id index{OpLoad(int_id, OpAccessChain(func_int, indices, invocation_id))};

        // gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
        const Id in_position{OpLoad(vec4_id, OpAccessChain(input_vec4, gl_in, index, Int(0)))};
        OpStore(OpAccessChain(output_vec4, gl_out, invocation_id, Int(0)), in_position);

        for (int i = 0; i < inputs.size(); i++) {
            // out_paramN[gl_InvocationID] = in_paramN[gl_InvocationID];
            const Id in_param{OpLoad(vec4_id, OpAccessChain(input_vec4, inputs[i], index))};
            OpStore(OpAccessChain(output_vec4, outputs[i], invocation_id), in_param);
        }

        OpReturn();
        OpFunctionEnd();
    }

    /// Emits a passthrough quad tessellation evaluation shader that outputs 4 control points.
    void EmitPassthroughTES() {
        DefineEntry(spv::ExecutionModel::TessellationEvaluation);

        // const int index = int(gl_TessCoord.y) * 2 + int(gl_TessCoord.x);
        const Id input_float{TypePointer(spv::StorageClass::Input, float_id)};
        const Id tess_coord_x{OpLoad(float_id, OpAccessChain(input_float, gl_tess_coord, Int(0)))};
        const Id tess_coord_y{OpLoad(float_id, OpAccessChain(input_float, gl_tess_coord, Int(1)))};
        const Id index{OpIAdd(int_id, OpIMul(int_id, OpConvertFToS(int_id, tess_coord_y), Int(2)),
                              OpConvertFToS(int_id, tess_coord_x))};

        // gl_Position = gl_in[index].gl_Position;
        const Id input_vec4{TypePointer(spv::StorageClass::Input, vec4_id)};
        const Id output_vec4{TypePointer(spv::StorageClass::Output, vec4_id)};
        const Id position{OpLoad(vec4_id, OpAccessChain(input_vec4, gl_in, index, Int(0)))};
        OpStore(OpAccessChain(output_vec4, gl_per_vertex, Int(0)), position);

        // out_paramN = in_paramN[index];
        for (int i = 0; i < inputs.size(); i++) {
            const Id param{OpLoad(vec4_id, OpAccessChain(input_vec4, inputs[i], index))};
            OpStore(outputs[i], param);
        }

        OpReturn();
        OpFunctionEnd();
    }

private:
    Id Int(s32 value) {
        return Constant(int_id, value);
    }

    Id AddInput(Id type) {
        const Id input{AddGlobalVariable(TypePointer(spv::StorageClass::Input, type),
                                         spv::StorageClass::Input)};
        interfaces.push_back(input);
        return input;
    }

    Id AddOutput(Id type) {
        const Id output{AddGlobalVariable(TypePointer(spv::StorageClass::Output, type),
                                          spv::StorageClass::Output)};
        interfaces.push_back(output);
        return output;
    }

    void DefineEntry(spv::ExecutionModel model) {
        AddCapability(spv::Capability::Shader);
        AddCapability(spv::Capability::Tessellation);
        const Id void_function{TypeFunction(void_id)};
        main = OpFunction(void_id, spv::FunctionControlMask::MaskNone, void_function);
        if (model == spv::ExecutionModel::TessellationControl) {
            AddExecutionMode(main, spv::ExecutionMode::OutputVertices, 4U);
        } else {
            AddExecutionMode(main, spv::ExecutionMode::Quads);
            AddExecutionMode(main, spv::ExecutionMode::SpacingEqual);
            AddExecutionMode(main, spv::ExecutionMode::VertexOrderCw);
        }
        DefineInputs(model);
        DefineOutputs(model);
        AddEntryPoint(model, main, "main", interfaces);
        AddLabel(OpLabel());
    }

    void DefineOutputs(spv::ExecutionModel model) {
        if (model == spv::ExecutionModel::TessellationControl) {
            const Id gl_per_vertex_array{TypeArray(gl_per_vertex_type, Constant(uint_id, 4U))};
            gl_out = AddOutput(gl_per_vertex_array);

            const Id arr2_id{TypeArray(float_id, Constant(uint_id, 2U))};
            gl_tess_level_inner = AddOutput(arr2_id);
            Decorate(gl_tess_level_inner, spv::Decoration::BuiltIn, spv::BuiltIn::TessLevelInner);
            Decorate(gl_tess_level_inner, spv::Decoration::Patch);

            const Id arr4_id{TypeArray(float_id, Constant(uint_id, 4U))};
            gl_tess_level_outer = AddOutput(arr4_id);
            Decorate(gl_tess_level_outer, spv::Decoration::BuiltIn, spv::BuiltIn::TessLevelOuter);
            Decorate(gl_tess_level_outer, spv::Decoration::Patch);
        } else {
            gl_per_vertex = AddOutput(gl_per_vertex_type);
        }
        outputs.reserve(fs_info.num_inputs);
        for (int i = 0; i < fs_info.num_inputs; i++) {
            const auto& input = fs_info.inputs[i];
            if (input.IsDefault()) {
                continue;
            }
            outputs.emplace_back(AddOutput(model == spv::ExecutionModel::TessellationControl
                                               ? TypeArray(vec4_id, Int(4))
                                               : vec4_id));
            Decorate(outputs.back(), spv::Decoration::Location, input.param_index);
        }
    }

    void DefineInputs(spv::ExecutionModel model) {
        if (model == spv::ExecutionModel::TessellationEvaluation) {
            gl_tess_coord = AddInput(vec3_id);
            Decorate(gl_tess_coord, spv::Decoration::BuiltIn, spv::BuiltIn::TessCoord);
        } else {
            gl_invocation_id = AddInput(int_id);
            Decorate(gl_invocation_id, spv::Decoration::BuiltIn, spv::BuiltIn::InvocationId);
        }
        const Id gl_per_vertex_array{TypeArray(gl_per_vertex_type, Constant(uint_id, 32U))};
        gl_in = AddInput(gl_per_vertex_array);
        const Id float_arr{TypeArray(vec4_id, Int(32))};
        inputs.reserve(fs_info.num_inputs);
        for (int i = 0; i < fs_info.num_inputs; i++) {
            const auto& input = fs_info.inputs[i];
            if (input.IsDefault()) {
                continue;
            }
            inputs.emplace_back(AddInput(float_arr));
            Decorate(inputs.back(), spv::Decoration::Location, input.param_index);
        }
    }

private:
    HwFragmentRuntimeInfo fs_info;
    Id main;
    Id void_id;
    Id bool_id;
    Id float_id;
    Id uint_id;
    Id int_id;
    Id bvec2_id;
    Id vec2_id;
    Id vec3_id;
    Id vec4_id;
    Id float_one;
    Id float_min_one;
    Id int_zero;
    Id gl_per_vertex_type;
    Id gl_in;
    union {
        Id gl_out;
        Id gl_per_vertex;
    };
    Id gl_tess_level_inner;
    Id gl_tess_level_outer;
    union {
        Id gl_tess_coord;
        Id gl_invocation_id;
    };
    std::vector<Id> inputs;
    std::vector<Id> outputs;
    std::vector<Id> interfaces;
};

std::vector<u32> EmitAuxilaryTessShader(AuxShaderType type, const HwFragmentRuntimeInfo& fs_info) {
    QuadRectListEmitter ctx{fs_info};
    switch (type) {
    case AuxShaderType::RectListTCS:
        ctx.EmitRectListTCS();
        break;
    case AuxShaderType::QuadListTCS:
        ctx.EmitQuadListTCS();
        break;
    case AuxShaderType::PassthroughTES:
        ctx.EmitPassthroughTES();
        break;
    }
    return ctx.Assemble();
}

struct ClipGsEmitter : public Sirit::Module {
    static constexpr u32 MaxVerts = 12; // 3 + one per plane (7 planes at most)
    static constexpr u32 MaxTris = MaxVerts - 2;

    ClipGsEmitter(const HwFragmentRuntimeInfo& fs_info_, bool provoking_last_, bool near_minus_w_,
                  bool depth_clip_)
        : Sirit::Module{SPIRV_VERSION_1_5}, fs_info{fs_info_}, provoking_last{provoking_last_},
          near_minus_w{near_minus_w_}, depth_clip{depth_clip_} {
        void_id = TypeVoid();
        bool_id = TypeBool();
        float_id = TypeFloat(32);
        uint_id = TypeUInt(32U);
        int_id = TypeInt(32U, true);
        vec3_id = TypeVector(float_id, 3);
        vec4_id = TypeVector(float_id, 4);
        const Id float_arr{TypeArray(float_id, Constant(uint_id, 1U))};
        per_vertex_type = TypeStruct(vec4_id, float_id, float_arr, float_arr);
        Decorate(per_vertex_type, spv::Decoration::Block);
        MemberDecorate(per_vertex_type, 0U, spv::Decoration::BuiltIn,
                       static_cast<u32>(spv::BuiltIn::Position));
        MemberDecorate(per_vertex_type, 1U, spv::Decoration::BuiltIn,
                       static_cast<u32>(spv::BuiltIn::PointSize));
        MemberDecorate(per_vertex_type, 2U, spv::Decoration::BuiltIn,
                       static_cast<u32>(spv::BuiltIn::ClipDistance));
        MemberDecorate(per_vertex_type, 3U, spv::Decoration::BuiltIn,
                       static_cast<u32>(spv::BuiltIn::CullDistance));
    }

    Id Int(s32 v) {
        return Constant(int_id, v);
    }
    Id F(float v) {
        return Constant(float_id, v);
    }

    void If(Id cond, auto&& body) {
        const Id then_label = OpLabel();
        const Id merge_label = OpLabel();
        OpSelectionMerge(merge_label, spv::SelectionControlMask::MaskNone);
        OpBranchConditional(cond, then_label, merge_label);
        AddLabel(then_label);
        body();
        OpBranch(merge_label);
        AddLabel(merge_label);
    }

    // Signed distance to each clip plane (inside >= 0). Plane normals in clip space.
    std::vector<std::array<float, 4>> Planes() const {
        constexpr float guard = 4.0f;
        std::vector<std::array<float, 4>> planes;
        planes.push_back({0.0f, 0.0f, 0.0f, 1.0f}); // w >= eps (constant handled separately)
        if (depth_clip) {
            planes.push_back(near_minus_w ? std::array{0.0f, 0.0f, 1.0f, 1.0f}
                                          : std::array{0.0f, 0.0f, 1.0f, 0.0f}); // near
            planes.push_back({0.0f, 0.0f, -1.0f, 1.0f});                       // far
        }
        planes.push_back({-1.0f, 0.0f, 0.0f, guard});
        planes.push_back({1.0f, 0.0f, 0.0f, guard});
        planes.push_back({0.0f, -1.0f, 0.0f, guard});
        planes.push_back({0.0f, 1.0f, 0.0f, guard});
        return planes;
    }

    Id Distance(Id pos, const std::array<float, 4>& plane, bool is_w) {
        const Id n = ConstantComposite(vec4_id, F(plane[0]), F(plane[1]), F(plane[2]), F(plane[3]));
        Id d = OpDot(float_id, pos, n);
        if (is_w) {
            d = OpFSub(float_id, d, F(1e-6f));
        }
        return d;
    }

    // Lexicographic a < b on (x, y, z, w): the canonical edge order.
    Id Less(Id a, Id b) {
        Id result = OpFOrdLessThan(bool_id, OpCompositeExtract(float_id, a, 3U),
                                   OpCompositeExtract(float_id, b, 3U));
        for (int c = 2; c >= 0; --c) {
            const Id ac = OpCompositeExtract(float_id, a, u32(c));
            const Id bc = OpCompositeExtract(float_id, b, u32(c));
            result = OpLogicalOr(bool_id, OpFOrdLessThan(bool_id, ac, bc),
                                 OpLogicalAnd(bool_id, OpFOrdEqual(bool_id, ac, bc), result));
        }
        return result;
    }

    void EmitVertex(Id pos, Id bary) {
        const Id out_pos_ptr{TypePointer(spv::StorageClass::Output, vec4_id)};
        const Id in_vec4_ptr{TypePointer(spv::StorageClass::Input, vec4_id)};
        OpStore(OpAccessChain(out_pos_ptr, gl_out, Int(0)), pos);
        const Id b0 = OpCompositeExtract(float_id, bary, 0U);
        const Id b1 = OpCompositeExtract(float_id, bary, 1U);
        const Id b2 = OpCompositeExtract(float_id, bary, 2U);
        for (size_t i = 0; i < inputs.size(); ++i) {
            const Id a0 = OpLoad(vec4_id, OpAccessChain(in_vec4_ptr, inputs[i], Int(0)));
            const Id a1 = OpLoad(vec4_id, OpAccessChain(in_vec4_ptr, inputs[i], Int(1)));
            const Id a2 = OpLoad(vec4_id, OpAccessChain(in_vec4_ptr, inputs[i], Int(2)));
            Id value;
            if (flat[i]) {
                value = provoking_last ? a2 : a0;
            } else {
                value = OpFAdd(vec4_id, OpVectorTimesScalar(vec4_id, a0, b0),
                               OpFAdd(vec4_id, OpVectorTimesScalar(vec4_id, a1, b1),
                                      OpVectorTimesScalar(vec4_id, a2, b2)));
            }
            OpStore(outputs[i], value);
        }
        OpEmitVertex();
    }

    void Emit() {
        AddCapability(spv::Capability::Shader);
        AddCapability(spv::Capability::Geometry);
        const Id void_function{TypeFunction(void_id)};
        main = OpFunction(void_id, spv::FunctionControlMask::MaskNone, void_function);

        // Interface
        gl_in = AddGlobal(TypeArray(per_vertex_type, Constant(uint_id, 3U)), true);
        gl_out = AddGlobal(per_vertex_type, false);
        for (u32 i = 0; i < fs_info.num_inputs; ++i) {
            const auto& input = fs_info.inputs[i];
            if (input.IsDefault()) {
                continue;
            }
            inputs.push_back(AddGlobal(TypeArray(vec4_id, Constant(uint_id, 3U)), true));
            Decorate(inputs.back(), spv::Decoration::Location, input.param_index);
            outputs.push_back(AddGlobal(vec4_id, false));
            Decorate(outputs.back(), spv::Decoration::Location, input.param_index);
            flat.push_back(input.is_flat);
        }
        AddEntryPoint(spv::ExecutionModel::Geometry, main, "main", interfaces);
        AddExecutionMode(main, spv::ExecutionMode::Triangles);
        AddExecutionMode(main, spv::ExecutionMode::OutputTriangleStrip);
        AddExecutionMode(main, spv::ExecutionMode::OutputVertices, 3U * MaxTris);
        AddExecutionMode(main, spv::ExecutionMode::Invocations, 1U);
        AddLabel(OpLabel());

        const Id arr_pos_t = TypeArray(vec4_id, Constant(uint_id, MaxVerts + 1));
        const Id arr_bary_t = TypeArray(vec3_id, Constant(uint_id, MaxVerts + 1));
        const Id fn_arr_pos = TypePointer(spv::StorageClass::Function, arr_pos_t);
        const Id fn_arr_bary = TypePointer(spv::StorageClass::Function, arr_bary_t);
        const Id fn_vec4 = TypePointer(spv::StorageClass::Function, vec4_id);
        const Id fn_vec3 = TypePointer(spv::StorageClass::Function, vec3_id);
        const Id fn_int = TypePointer(spv::StorageClass::Function, int_id);
        const Id pos_var = AddLocalVariable(fn_arr_pos, spv::StorageClass::Function);
        const Id bary_var = AddLocalVariable(fn_arr_bary, spv::StorageClass::Function);
        const Id npos_var = AddLocalVariable(fn_arr_pos, spv::StorageClass::Function);
        const Id nbary_var = AddLocalVariable(fn_arr_bary, spv::StorageClass::Function);
        const Id cnt_var = AddLocalVariable(fn_int, spv::StorageClass::Function);
        const Id ncnt_var = AddLocalVariable(fn_int, spv::StorageClass::Function);

        const Id in_vec4_ptr{TypePointer(spv::StorageClass::Input, vec4_id)};
        std::array<Id, 3> p;
        for (int i = 0; i < 3; ++i) {
            p[i] = OpLoad(vec4_id, OpAccessChain(in_vec4_ptr, gl_in, Int(i), Int(0)));
        }
        const std::array<Id, 3> e{ConstantComposite(vec3_id, F(1), F(0), F(0)),
                                  ConstantComposite(vec3_id, F(0), F(1), F(0)),
                                  ConstantComposite(vec3_id, F(0), F(0), F(1))};

        const auto planes = Planes();
        Id inside = ConstantTrue(bool_id);
        for (size_t k = 0; k < planes.size(); ++k) {
            for (int i = 0; i < 3; ++i) {
                inside = OpLogicalAnd(bool_id, inside,
                                      OpFOrdGreaterThanEqual(bool_id, Distance(p[i], planes[k], k == 0), F(0)));
            }
        }

        const Id fast_label = OpLabel();
        const Id slow_label = OpLabel();
        const Id done_label = OpLabel();
        OpSelectionMerge(done_label, spv::SelectionControlMask::MaskNone);
        OpBranchConditional(inside, fast_label, slow_label);

        // Fast path: the triangle as it came.
        AddLabel(fast_label);
        for (int i = 0; i < 3; ++i) {
            EmitVertex(p[i], e[i]);
        }
        OpEndPrimitive();
        OpBranch(done_label);

        // Slow path: Sutherland-Hodgman over the planes, branch-free per vertex.
        AddLabel(slow_label);
        for (int i = 0; i < 3; ++i) {
            OpStore(OpAccessChain(fn_vec4, pos_var, Int(i)), p[i]);
            OpStore(OpAccessChain(fn_vec3, bary_var, Int(i)), e[i]);
        }
        OpStore(cnt_var, Int(3));
        for (size_t k = 0; k < planes.size(); ++k) {
            OpStore(ncnt_var, Int(0));
            const Id cnt = OpLoad(int_id, cnt_var);
            for (u32 i = 0; i < MaxVerts; ++i) {
                const Id valid = OpSLessThan(bool_id, Int(s32(i)), cnt);
                const Id next = OpSelect(int_id, OpSLessThan(bool_id, Int(s32(i + 1)), cnt),
                                         Int(s32(i + 1)), Int(0));
                const Id a = OpLoad(vec4_id, OpAccessChain(fn_vec4, pos_var, Int(s32(i))));
                const Id ba = OpLoad(vec3_id, OpAccessChain(fn_vec3, bary_var, Int(s32(i))));
                const Id b = OpLoad(vec4_id, OpAccessChain(fn_vec4, pos_var, next));
                const Id bb = OpLoad(vec3_id, OpAccessChain(fn_vec3, bary_var, next));
                const Id da = Distance(a, planes[k], k == 0);
                const Id db = Distance(b, planes[k], k == 0);
                const Id ina = OpFOrdGreaterThanEqual(bool_id, da, F(0));
                const Id inb = OpFOrdGreaterThanEqual(bool_id, db, F(0));

                // Keep a.
                Id ncnt = OpLoad(int_id, ncnt_var);
                OpStore(OpAccessChain(fn_vec4, npos_var, ncnt), a);
                OpStore(OpAccessChain(fn_vec3, nbary_var, ncnt), ba);
                ncnt = OpIAdd(int_id, ncnt,
                              OpSelect(int_id, OpLogicalAnd(bool_id, valid, ina), Int(1), Int(0)));

                // Crossing: cut point from the endpoints in canonical order.
                const Id swap = Less(b, a);
                const Id p0 = OpSelect(vec4_id, swap, b, a);
                const Id p1 = OpSelect(vec4_id, swap, a, b);
                const Id q0 = OpSelect(vec3_id, swap, bb, ba);
                const Id q1 = OpSelect(vec3_id, swap, ba, bb);
                const Id d0 = OpSelect(float_id, swap, db, da);
                const Id d1 = OpSelect(float_id, swap, da, db);
                const Id t = OpFDiv(float_id, d0, OpFSub(float_id, d0, d1));
                const Id cut = OpFAdd(vec4_id, p0,
                                      OpVectorTimesScalar(vec4_id, OpFSub(vec4_id, p1, p0), t));
                const Id cut_bary = OpFAdd(vec3_id, q0,
                                           OpVectorTimesScalar(vec3_id, OpFSub(vec3_id, q1, q0), t));
                OpStore(OpAccessChain(fn_vec4, npos_var, ncnt), cut);
                OpStore(OpAccessChain(fn_vec3, nbary_var, ncnt), cut_bary);
                const Id crossing = OpLogicalAnd(bool_id, valid, OpLogicalNotEqual(bool_id, ina, inb));
                ncnt = OpIAdd(int_id, ncnt, OpSelect(int_id, crossing, Int(1), Int(0)));
                OpStore(ncnt_var, ncnt);
            }
            OpStore(cnt_var, OpLoad(int_id, ncnt_var));
            for (u32 i = 0; i < MaxVerts; ++i) {
                OpStore(OpAccessChain(fn_vec4, pos_var, Int(s32(i))),
                        OpLoad(vec4_id, OpAccessChain(fn_vec4, npos_var, Int(s32(i)))));
                OpStore(OpAccessChain(fn_vec3, bary_var, Int(s32(i))),
                        OpLoad(vec3_id, OpAccessChain(fn_vec3, nbary_var, Int(s32(i)))));
            }
        }
        // Fan out the polygon.
        const Id count = OpLoad(int_id, cnt_var);
        const Id v0 = OpLoad(vec4_id, OpAccessChain(fn_vec4, pos_var, Int(0)));
        const Id w0 = OpLoad(vec3_id, OpAccessChain(fn_vec3, bary_var, Int(0)));
        for (u32 k = 1; k <= MaxTris; ++k) {
            If(OpSLessThan(bool_id, Int(s32(k + 1)), count), [&] {
                const Id vk = OpLoad(vec4_id, OpAccessChain(fn_vec4, pos_var, Int(s32(k))));
                const Id wk = OpLoad(vec3_id, OpAccessChain(fn_vec3, bary_var, Int(s32(k))));
                const Id vn = OpLoad(vec4_id, OpAccessChain(fn_vec4, pos_var, Int(s32(k + 1))));
                const Id wn = OpLoad(vec3_id, OpAccessChain(fn_vec3, bary_var, Int(s32(k + 1))));
                EmitVertex(v0, w0);
                EmitVertex(vk, wk);
                EmitVertex(vn, wn);
                OpEndPrimitive();
            });
        }
        OpBranch(done_label);

        AddLabel(done_label);
        OpReturn();
        OpFunctionEnd();
    }

private:
    Id AddGlobal(Id type, bool input) {
        const auto storage = input ? spv::StorageClass::Input : spv::StorageClass::Output;
        const Id var{AddGlobalVariable(TypePointer(storage, type), storage)};
        interfaces.push_back(var);
        return var;
    }

    HwFragmentRuntimeInfo fs_info;
    bool provoking_last;
    bool near_minus_w;
    bool depth_clip;
    Id main{};
    Id void_id{}, bool_id{}, float_id{}, uint_id{}, int_id{}, vec3_id{}, vec4_id{};
    Id per_vertex_type{};
    Id gl_in{}, gl_out{};
    std::vector<Id> inputs, outputs, interfaces;
    std::vector<bool> flat;
};

std::vector<u32> EmitClipGeometryShader(const HwFragmentRuntimeInfo& fs_info, bool provoking_last,
                                        bool near_minus_w, bool depth_clip) {
    ClipGsEmitter ctx{fs_info, provoking_last, near_minus_w, depth_clip};
    ctx.Emit();
    return ctx.Assemble();
}

} // namespace Shader::Backend::SPIRV
