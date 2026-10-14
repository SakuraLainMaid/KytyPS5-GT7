#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInstructions.h"

#include <algorithm>
#include <bit>
#include <cstdlib>

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {
namespace {

// Whether a tessellation interface variable is a stage input: the TES reads the HS's patch
// constants as its own.
bool TessellationInput(const EmitterState& state, IR::TessellationAttribute kind) {
	using Attribute = IR::TessellationAttribute;
	return kind == Attribute::ControlInput || kind == Attribute::EvaluationInput ||
	       (kind == Attribute::PatchOutput &&
	        state.program.stage == ShaderType::TessellationEvaluation);
}

uint32_t TessellationPointer(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto& state          = ctx.state;
	using Attribute      = IR::TessellationAttribute;
	const auto  kind     = static_cast<Attribute>(inst.Arg(0).U32());
	const auto& tess     = state.input_info.vertex->tess;
	const auto  variable = state.tess_variables.at(static_cast<uint32_t>(kind));
	EXIT_IF(variable == 0);
	const auto storage =
	    TessellationInput(state, kind) ? spv::StorageClassInput : spv::StorageClassOutput;
	const auto pointer = state.builder.AllocateId();
	if (kind == Attribute::Factor) {
		EXIT_NOT_IMPLEMENTED(!inst.Arg(1).IsImmediate());
		// The guest's factor layout: a triangle's three outer factors and its inner one, a
		// quad's four outer and two inner.
		const auto index         = inst.Arg(1).U32() / 4u;
		const auto outer_factors = tess.domain == 2u ? 4u : 3u;
		const auto outer         = index < outer_factors;
		EXIT_NOT_IMPLEMENTED(index >= outer_factors + (tess.domain == 2u ? 2u : 1u));
		state.builder.AddFunction(spv::OpAccessChain, TypePointer(state, storage, TypeF32(state)),
		                          pointer, outer ? variable : state.tess_inner_variable,
		                          ConstantU32(state, outer ? index : index - outer_factors));
		return pointer;
	}
	auto address = ctx.Arg(inst, 1);
	if (kind == Attribute::PatchOutput) {
		address =
		    EmitBinaryU32(state, spv::OpISub, address, ConstantU32(state, state.tess_patch_base));
	}
	const bool local  = kind == Attribute::LocalOutput || kind == Attribute::ControlInput;
	const auto stride = local ? tess.ls_stride : tess.hs_stride;
	const auto offset = kind == Attribute::PatchOutput ? address
	                                                   : EmitBinaryU32(state, spv::OpUMod, address,
	                                                                   ConstantU32(state, stride));
	const auto attribute =
	    EmitBinaryU32(state, spv::OpShiftRightLogical, offset, ConstantU32(state, 4));
	const auto component =
	    EmitBinaryU32(state, spv::OpBitwiseAnd,
	                  EmitBinaryU32(state, spv::OpShiftRightLogical, offset, ConstantU32(state, 2)),
	                  ConstantU32(state, 3));
	const auto type = TypePointer(state, storage, TypeU32(state));
	if (kind == Attribute::LocalOutput || kind == Attribute::PatchOutput) {
		state.builder.AddFunction(spv::OpAccessChain, type, pointer, variable, attribute,
		                          component);
	} else {
		// An HS invocation writes its own control point; it reads any of them back, an address
		// past the last one (computed patch constants the analysis could not fold) clamped to
		// stay within the array.
		const auto vertices = kind == Attribute::ControlInput ? tess.input_control_points
		                                                      : tess.output_control_points;
		const auto vertex =
		    kind == Attribute::ControlOutput &&
		            inst.GetOpcode() == IR::ValueOpcode::SetTessellationAttribute
		        ? EmitLaneId(state)
		        : EmitUMin32(state,
		                     EmitBinaryU32(state, spv::OpUDiv, address, ConstantU32(state, stride)),
		                     ConstantU32(state, vertices - 1u));
		state.builder.AddFunction(spv::OpAccessChain, type, pointer, variable, vertex, attribute,
		                          component);
	}
	return pointer;
}

} // namespace

void DefineTessellationInterfaces(EmitterState& state) {
	using Attribute = IR::TessellationAttribute;
	std::array<bool, 6> used {};
	uint32_t            patch_begin = UINT32_MAX, patch_end = 0;
	for (const auto* block: state.program.blocks) {
		for (const auto& inst: *block) {
			if (inst.GetOpcode() != IR::ValueOpcode::GetTessellationAttribute &&
			    inst.GetOpcode() != IR::ValueOpcode::SetTessellationAttribute)
				continue;
			const auto kind = inst.Arg(0).U32();
			used.at(kind)   = true;
			if (kind == static_cast<uint32_t>(Attribute::PatchOutput)) {
				EXIT_NOT_IMPLEMENTED(!inst.Arg(1).IsImmediate());
				patch_begin = std::min(patch_begin, inst.Arg(1).U32());
				patch_end   = std::max(patch_end, inst.Arg(1).U32() + 4u);
			}
		}
	}
	if (std::ranges::none_of(used, [](bool value) { return value; })) return;
	const auto& tess  = state.input_info.vertex->tess;
	const auto  array = [&](uint32_t type, uint32_t count) {
		return state.builder.Type(spv::OpTypeArray, type, ConstantU32(state, count));
	};
	for (uint32_t index = 0; index < used.size(); index++) {
		if (!used[index]) continue;
		const auto kind    = static_cast<Attribute>(index);
		const auto storage =
		    TessellationInput(state, kind) ? spv::StorageClassInput : spv::StorageClassOutput;
		uint32_t   type;
		if (kind == Attribute::Factor) {
			type = array(TypeF32(state), 4u);
		} else if (kind == Attribute::PatchOutput) {
			// The region the analysis found the HS writing, so that the HS's patch outputs and
			// the TES's inputs agree whatever each of them touches.
			if (tess.patch_size != 0u) {
				EXIT_NOT_IMPLEMENTED(patch_begin < tess.patch_base ||
				                     patch_end > tess.patch_base + tess.patch_size);
				patch_begin = tess.patch_base;
				patch_end   = tess.patch_base + tess.patch_size;
			}
			state.tess_patch_base = patch_begin;
			type = array(TypeU32Vector(state, 4), (patch_end - patch_begin + 15u) / 16u);
		} else {
			const bool local  = kind == Attribute::LocalOutput || kind == Attribute::ControlInput;
			const auto stride = local ? tess.ls_stride : tess.hs_stride;
			type              = array(TypeU32Vector(state, 4), (stride + 15u) / 16u);
			if (kind != Attribute::LocalOutput) {
				type = array(type, local ? tess.input_control_points : tess.output_control_points);
			}
		}
		auto& variable = state.tess_variables[index];
		variable       = DefineInterfaceVariable(state, type, storage, "tess_attributes");
		if (kind == Attribute::Factor) {
			state.builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationBuiltIn,
			                            spv::BuiltInTessLevelOuter);
			state.tess_inner_variable =
			    DefineInterfaceVariable(state, array(TypeF32(state), 2u), storage, "tess_inner");
			state.builder.AddAnnotation(spv::OpDecorate, state.tess_inner_variable,
			                            spv::DecorationBuiltIn, spv::BuiltInTessLevelInner);
			state.builder.AddAnnotation(spv::OpDecorate, state.tess_inner_variable,
			                            spv::DecorationPatch);
		} else {
			state.builder.AddAnnotation(
			    spv::OpDecorate, variable, spv::DecorationLocation,
			    kind == Attribute::PatchOutput ? (tess.hs_stride + 15u) / 16u : 0u);
		}
		if (kind == Attribute::Factor || kind == Attribute::PatchOutput) {
			state.builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationPatch);
		}
	}
}

void DefineTessellationExecutionModes(EmitterState& state) {
	const auto& tess = state.input_info.vertex->tess;
	// AnalyzeTessellationPrograms rejects the rest (isolines, a line topology).
	EXIT_NOT_IMPLEMENTED((tess.domain != 1u && tess.domain != 2u) || tess.partitioning > 3u ||
	                     tess.output_topology == 1u || tess.output_topology > 3u);
	state.builder.RequireCapability(spv::CapabilityTessellation);
	if (state.program.stage == ShaderType::TessellationControl) {
		state.builder.AddExecutionMode(state.main_func, spv::ExecutionModeOutputVertices,
		                               tess.output_control_points);
	} else {
		state.builder.AddExecutionMode(state.main_func, tess.domain == 2u
		                                                    ? spv::ExecutionModeQuads
		                                                    : spv::ExecutionModeTriangles);
		// Integer and pow2 partitioning both as equal spacing (Vulkan has no pow2).
		state.builder.AddExecutionMode(
		    state.main_func, tess.partitioning == 2u   ? spv::ExecutionModeSpacingFractionalOdd
		                     : tess.partitioning == 3u ? spv::ExecutionModeSpacingFractionalEven
		                                               : spv::ExecutionModeSpacingEqual);
		if (tess.output_topology == 0u) {
			state.builder.AddExecutionMode(state.main_func, spv::ExecutionModePointMode);
		} else {
			state.builder.AddExecutionMode(state.main_func, tess.output_topology == 3u
			                                                    ? spv::ExecutionModeVertexOrderCcw
			                                                    : spv::ExecutionModeVertexOrderCw);
		}
	}
}

uint32_t EmitGetTessellationAttribute(ValueEmitContext& ctx, const IR::Inst& inst) {
	return EmitValueOrZeroIfCondition(ctx.state, ctx.Arg(inst, 2), [&] {
		const auto value = ctx.state.builder.AllocateId();
		ctx.state.builder.AddFunction(spv::OpLoad, TypeU32(ctx.state), value,
		                              TessellationPointer(ctx, inst));
		return value;
	});
}

// Diagnostics: KYTY_TESS_FACTOR=<f> (above 0) replaces every tessellation factor an HS writes
// above zero (culled patches stay culled), so that cracks from factors that differ between
// neighbouring patches can be told from cracks in the evaluation.
static float TessellationFactorOverride() {
	static const float value = [] {
		const char* text = std::getenv("KYTY_TESS_FACTOR");
		return text != nullptr ? std::strtof(text, nullptr) : 0.0f;
	}();
	return value;
}

void EmitSetTessellationAttribute(ValueEmitContext& ctx, const IR::Inst& inst) {
	EmitIfCondition(ctx.state, ctx.Arg(inst, 3), [&] {
		auto value = ctx.Arg(inst, 2);
		if (inst.Arg(0).U32() == static_cast<uint32_t>(IR::TessellationAttribute::Factor)) {
			auto&       state    = ctx.state;
			const auto& tess     = state.input_info.vertex->tess;
			const auto  floating = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpBitcast, TypeF32(state), floating, value);
			value = floating;
			// A factor above zero draws the patch; zero, a negative or NaN culls it, whatever
			// the clamp below.
			const auto drawn = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpFOrdGreaterThan, TypeBool(state), drawn, floating,
			                          ConstantF32Value(state, 0.0f));
			// The guest tessellator clamps the factors it draws to VGT_HOS_MIN/MAX_TESS_LEVEL;
			// the host's only to its own limit.
			const auto low  = std::max(std::bit_cast<float>(tess.min_level_bits), 0.0f);
			const auto high = std::bit_cast<float>(tess.max_level_bits);
			if (tess.max_level_bits != 0u && high > 0.0f && high >= low) {
				const auto clamped = EmitGlsl<GLSLstd450FClamp, IR::Type::F32>(
				    state, floating, ConstantF32Value(state, low), ConstantF32Value(state, high));
				const auto selected = state.builder.AllocateId();
				state.builder.AddFunction(spv::OpSelect, TypeF32(state), selected, drawn, clamped,
				                          floating);
				value = selected;
			}
			if (const auto factor = TessellationFactorOverride(); factor > 0.0f) {
				const auto replaced = state.builder.AllocateId();
				state.builder.AddFunction(spv::OpSelect, TypeF32(state), replaced, drawn,
				                          ConstantF32Value(state, factor), value);
				value = replaced;
			}
		}
		ctx.state.builder.AddFunction(spv::OpStore, TessellationPointer(ctx, inst), value);
	});
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
