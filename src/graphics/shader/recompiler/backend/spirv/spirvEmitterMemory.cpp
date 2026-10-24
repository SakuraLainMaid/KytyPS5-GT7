#include "graphics/host_gpu/renderer/cache/bufferCachePages.h"
#include "graphics/shader/recompiler/backend/spirv/SpirvEmitter.h"
#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInstructions.h"
#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"
#include "graphics/shader/recompiler/frontend/decode/ShaderFunctions.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdlib>
#include <span>
#include <tuple>
#include <utility>

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {
namespace {

uint32_t AndCondition(EmitterState& state, uint32_t lhs, uint32_t rhs) {
	return Binary(state, spv::OpLogicalAnd, TypeBool(state), lhs, rhs);
}

uint32_t EmitDsMaskedLaneRead(EmitterState& state, uint32_t source, uint32_t target,
                              uint32_t exec) {
	if (state.lane_count == 2) {
		target = Binary(state, spv::OpBitwiseAnd, TypeU32(state), target, ConstantU32(state, 31));
	}
	const auto shuffled = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpGroupNonUniformShuffle, TypeU32(state), shuffled,
	                          ConstantU32(state, spv::ScopeSubgroup), source, target);
	const auto source_exec = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpGroupNonUniformShuffle, TypeBool(state), source_exec,
	                          ConstantU32(state, spv::ScopeSubgroup), exec, target);
	const auto source_active =
	    AndCondition(state, source_exec, EmitSubgroupLaneActiveBool(state, target));
	return Select(state, TypeU32(state), source_active, shuffled, ConstantU32(state, 0));
}

struct BufferAddress {
	uint32_t offset;
	uint32_t byte;
};

BufferAddress CalculateBufferAddress(EmitterState& state, uint32_t index, uint32_t offset,
                                     uint32_t soffset, uint32_t immediate, uint32_t stride,
                                     uint32_t swizzle, uint32_t index_stride) {
	const auto zero = ConstantU32(state, 0);
	const auto one  = ConstantU32(state, 1);
	const auto add  = [&](uint32_t lhs, uint32_t rhs) {
		return lhs == zero   ? rhs
		       : rhs == zero ? lhs
		                     : Binary(state, spv::OpIAdd, TypeU32(state), lhs, rhs);
	};
	const auto mul = [&](uint32_t lhs, uint32_t rhs) {
		return lhs == zero || rhs == zero ? zero
		       : lhs == one               ? rhs
		       : rhs == one               ? lhs
		                                  : Binary(state, spv::OpIMul, TypeU32(state), lhs, rhs);
	};
	if (immediate != 0u) {
		offset = add(offset, ConstantU32(state, immediate));
	}
	auto address = add(mul(index, stride), offset);
	if (swizzle != 0u) {
		const auto index_shift =
		    Binary(state, spv::OpIAdd, TypeU32(state), index_stride, ConstantU32(state, 3));
		const auto indices = Binary(state, spv::OpShiftLeftLogical, TypeU32(state),
		                            ConstantU32(state, 1), index_shift);
		const auto index_msb =
		    Binary(state, spv::OpShiftRightLogical, TypeU32(state), index, index_shift);
		const auto index_lsb =
		    Binary(state, spv::OpBitwiseAnd, TypeU32(state), index,
		           Binary(state, spv::OpISub, TypeU32(state), indices, ConstantU32(state, 1)));
		const auto offset_msb =
		    Binary(state, spv::OpBitwiseAnd, TypeU32(state), offset, ConstantU32(state, ~3u));
		const auto offset_lsb =
		    Binary(state, spv::OpBitwiseAnd, TypeU32(state), offset, ConstantU32(state, 3u));
		const auto msb = mul(add(mul(index_msb, stride), offset_msb), indices);
		const auto lsb = add(Binary(state, spv::OpShiftLeftLogical, TypeU32(state), index_lsb,
		                            ConstantU32(state, 2u)),
		                     offset_lsb);
		address        = Select(state, TypeU32(state), swizzle, add(msb, lsb), address);
	}
	return {offset, add(address, soffset)};
}

uint32_t BufferLane(EmitterState& state) {
	return Binary(state, spv::OpBitwiseAnd, TypeU32(state), EmitSubgroupLocalInvocationId(state),
	              ConstantU32(state, 63));
}

uint32_t BufferByteAddress(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem) {
	auto&      state  = ctx.state;
	const auto packed = StorageBufferPackedStride(state, mem);
	const auto stride = packed & 0x3fffu;
	auto       index  = ctx.Arg(inst, 1);
	if ((packed & (1u << 20u)) != 0u) {
		index = Binary(state, spv::OpIAdd, TypeU32(state), index, BufferLane(state));
	}
	const bool swizzle = stride != 0u && (packed & (1u << 14u)) != 0u;
	return CalculateBufferAddress(state, index, ctx.Arg(inst, 2), ctx.Arg(inst, 3), mem.offset,
	                              ConstantU32(state, stride),
	                              swizzle ? ConstantBool(state, true) : 0u,
	                              ConstantU32(state, (packed >> 16u) & 3u))
	    .byte;
}

uint32_t AddU64Low(EmitterState& state, uint32_t low, uint32_t high, uint32_t add_low,
                   uint32_t add_high, uint32_t& out_high) {
	const auto result = Binary(state, spv::OpIAdd, TypeU32(state), low, add_low);
	const auto carry  = Binary(state, spv::OpULessThan, TypeBool(state), result, low);
	out_high =
	    Binary(state, spv::OpIAdd, TypeU32(state),
	           Binary(state, spv::OpIAdd, TypeU32(state), high, add_high),
	           Select(state, TypeU32(state), carry, ConstantU32(state, 1), ConstantU32(state, 0)));
	return result;
}

uint32_t AddAddressOffset(EmitterState& state, const IR::MemoryInfo& mem, uint32_t low,
                          uint32_t& high) {
	const auto immediate = static_cast<int32_t>(mem.offset);
	if (immediate == 0) return low;
	const auto immediate_low  = ConstantU32(state, static_cast<uint32_t>(immediate));
	const auto immediate_high = ConstantU32(state, immediate < 0 ? UINT32_MAX : 0u);
	return AddU64Low(state, low, high, immediate_low, immediate_high, high);
}

uint32_t ScratchByteAddress(ValueEmitContext& ctx, const IR::MemoryInfo& mem, uint32_t low,
                            uint32_t high) {
	auto& state = ctx.state;
	low = AddAddressOffset(state, mem, low, high);
	const auto valid = Binary(state, spv::OpIEqual, TypeBool(state), high, ConstantU32(state, 0));
	return Select(state, TypeU32(state), valid, low, ConstantU32(state, UINT32_MAX));
}

uint32_t GuestAddress(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem) {
	auto& state = ctx.state;
	auto  low   = ctx.Arg(inst, 1);
	if (mem.kind == IR::ResourceKind::ScalarAddress) {
		low = Binary(state, spv::OpBitwiseAnd, TypeU32(state), low, ConstantU32(state, ~3u));
	}
	uint32_t address = 0;
	if (mem.address_is_full) {
		address = DeviceAddressFromWords(state, low, ctx.Arg(inst, 2));
	} else {
		const auto* handle = inst.Arg(0).Resolve().TryInstruction();
		if (handle == nullptr || handle->GetOpcode() != IR::ValueOpcode::GetAddressResource ||
		    handle->NumArgs() != 2) {
			ctx.Fail(inst, "has no address base pair");
			return ConstantDeviceAddress(state, 0);
		}
		auto base_low = ctx.Arg(*handle, 0);
		if (mem.kind == IR::ResourceKind::ScalarAddress) {
			base_low = Binary(state, spv::OpBitwiseAnd, TypeU32(state), base_low,
			                  ConstantU32(state, ~3u));
		}
		const auto base = DeviceAddressFromWords(state, base_low, ctx.Arg(*handle, 1));
		address         = Binary(state, spv::OpIAdd, TypeScalarU64(state), base,
		                         Unary(state, spv::OpUConvert, TypeScalarU64(state), low));
	}
	auto immediate = static_cast<int32_t>(mem.offset);
	if (mem.kind == IR::ResourceKind::ScalarAddress) {
		immediate = static_cast<int32_t>(static_cast<uint32_t>(immediate) & ~3u);
	}
	return immediate == 0
	           ? address
	           : Binary(state, spv::OpIAdd, TypeScalarU64(state), address,
	                    ConstantDeviceAddress(
	                        state, static_cast<uint64_t>(static_cast<int64_t>(immediate))));
}

uint32_t FaultElementPointer(EmitterState& state, uint32_t index) {
	const auto pointer = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpAccessChain, TypeStorageBufferElementPointer(state), pointer,
	                          state.fault_buffer_variable, ConstantU32(state, 0), index);
	return pointer;
}

void RecordBdaFault(EmitterState& state, uint32_t page) {
	const auto word =
	    Binary(state, spv::OpShiftRightLogical, TypeU32(state), page, ConstantU32(state, 5));
	const auto bit =
	    Binary(state, spv::OpShiftLeftLogical, TypeU32(state), ConstantU32(state, 1),
	           Binary(state, spv::OpBitwiseAnd, TypeU32(state), page, ConstantU32(state, 31)));
	const auto pointer = FaultElementPointer(state, word);
	// A page already recorded takes no atomic: a bounded loop over memory the emulator does not
	// have yet (a ray's nodes, until the host maps the faulted pages) faults on it every round,
	// in every lane, and atomics on one word serialize. A stale read only costs the atomic.
	const auto recorded = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpLoad, TypeU32(state), recorded, pointer);
	const auto missing = Binary(state, spv::OpIEqual, TypeBool(state),
	                            Binary(state, spv::OpBitwiseAnd, TypeU32(state), recorded, bit),
	                            ConstantU32(state, 0));
	EmitIfCondition(state, missing, [&]() {
		const auto value = state.builder.AllocateId();
		// Different invocations can fault on distinct pages in the same bitset word.
		// A load/OR/store loses one of those pages, preventing a complete retry.
		state.builder.AddFunction(spv::OpAtomicOr, TypeU32(state), value, pointer,
		                          ConstantU32(state, spv::ScopeDevice), ConstantU32(state, 0), bit);
		if (state.program.info.uses_bvh) {
			// A page this ray tracing shader recorded first (ShaderTrapArea::ray_faults).
			constexpr auto word =
			    static_cast<uint32_t>(BufferCachePages::kNumPages / 8 / sizeof(uint32_t) +
			                          offsetof(ShaderTrapArea, ray_faults) / sizeof(uint32_t));
			const auto first = Binary(state, spv::OpIEqual, TypeBool(state),
			                          Binary(state, spv::OpBitwiseAnd, TypeU32(state), value, bit),
			                          ConstantU32(state, 0));
			EmitIfCondition(state, first, [&]() {
				const auto count = state.builder.AllocateId();
				state.builder.AddFunction(spv::OpAtomicIAdd, TypeU32(state), count,
				                          FaultElementPointer(state, ConstantU32(state, word)),
				                          ConstantU32(state, spv::ScopeDevice),
				                          ConstantU32(state, 0), ConstantU32(state, 1));
			});
		}
	});
}

uint32_t LoadBdaDword(ValueEmitContext& ctx, uint32_t address) {
	auto&      state   = ctx.state;
	const auto bda     = GetBdaPointer(state, address);
	const auto present =
	    Binary(state, spv::OpINotEqual, TypeBool(state), bda, ConstantDeviceAddress(state, 0));
	return EmitValueOrZeroIfCondition(state, present, [&]() {
		const auto pointer = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpConvertUToPtr, TypePhysicalU32Pointer(state), pointer,
		                          bda);
		const auto         value     = state.builder.AllocateId();
		constexpr uint32_t alignment = sizeof(uint32_t);
		state.builder.AddFunction(spv::OpLoad, TypeU32(state), value, pointer,
		                          spv::MemoryAccessAlignedMask, alignment);
		return value;
	});
}

uint32_t LoadBda(ValueEmitContext& ctx, uint32_t address, uint32_t active, uint32_t bits) {
	auto& state = ctx.state;
	return EmitValueOrZeroIfCondition(state, active, [&]() {
		const auto aligned = Binary(state, spv::OpBitwiseAnd, TypeScalarU64(state), address,
		                            ConstantDeviceAddress(state, ~uint64_t {3}));
		const auto first   = LoadBdaDword(ctx, aligned);
		const auto byte =
		    Binary(state, spv::OpBitwiseAnd, TypeU32(state),
		           Unary(state, spv::OpUConvert, TypeU32(state), address), ConstantU32(state, 3));
		const auto crosses =
		    bits == 8u ? ConstantBool(state, false)
		               : Binary(state, bits == 16u ? spv::OpUGreaterThan : spv::OpINotEqual,
		                        TypeBool(state), byte, ConstantU32(state, bits == 16u ? 2u : 0u));
		const auto second = EmitValueOrZeroIfCondition(state, crosses, [&]() {
			return LoadBdaDword(ctx, Binary(state, spv::OpIAdd, TypeScalarU64(state), aligned,
			                                ConstantDeviceAddress(state, sizeof(uint32_t))));
		});
		const auto shift =
		    Binary(state, spv::OpShiftLeftLogical, TypeU32(state), byte, ConstantU32(state, 3));
		const auto upper_shift =
		    Binary(state, spv::OpShiftLeftLogical, TypeU32(state),
		           Binary(state, spv::OpBitwiseAnd, TypeU32(state),
		                  Binary(state, spv::OpISub, TypeU32(state), ConstantU32(state, 4), byte),
		                  ConstantU32(state, 3)),
		           ConstantU32(state, 3));
		const auto merged =
		    Binary(state, spv::OpBitwiseOr, TypeU32(state),
		           Binary(state, spv::OpShiftRightLogical, TypeU32(state), first, shift),
		           Binary(state, spv::OpShiftLeftLogical, TypeU32(state), second, upper_shift));
		return bits == 32u ? merged
		                   : Binary(state, spv::OpBitwiseAnd, TypeU32(state), merged,
		                            ConstantU32(state, bits == 8u ? 0xffu : 0xffffu));
	});
}

uint32_t ByteAddress(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem) {
	if (mem.kind == IR::ResourceKind::Buffer) {
		const auto resource = ResourceForDescriptor(ctx.state, IR::DescriptorBindingKind::Buffers,
		                                            mem.resource);
		const auto prefix = ctx.state.memory_byte_offsets[resource];
		const auto address = Binary(ctx.state, spv::OpIAdd, TypeU32(ctx.state),
		                            BufferByteAddress(ctx, inst, mem), prefix);
		// The host binding prefix must not wrap an out-of-range guest address into the buffer.
		return Select(ctx.state, TypeU32(ctx.state),
		              Binary(ctx.state, spv::OpUGreaterThanEqual, TypeBool(ctx.state), address, prefix),
		              address, ConstantU32(ctx.state, UINT32_MAX));
	}
	if (mem.kind == IR::ResourceKind::Lds || mem.kind == IR::ResourceKind::Gds) {
		if (mem.offset == 0u) {
			return ctx.Arg(inst, 0);
		}
		return Binary(ctx.state, spv::OpIAdd, TypeU32(ctx.state), ctx.Arg(inst, 0),
		              ConstantU32(ctx.state, mem.offset));
	}
	if (mem.kind != IR::ResourceKind::Scratch) {
		EXIT("physical address memory must use the BDA emitter\n");
	}
	return ScratchByteAddress(ctx, mem, ctx.Arg(inst, 1), ctx.Arg(inst, 2));
}

uint32_t DwordIndex(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem) {
	auto address = ByteAddress(ctx, inst, mem);
	if (mem.kind == IR::ResourceKind::Lds || mem.kind == IR::ResourceKind::Gds) {
		// RDNA2 DS region addresses retain bits [15:2] after adding the byte offset.
		address = Binary(ctx.state, spv::OpBitwiseAnd, TypeU32(ctx.state), address,
		                 ConstantU32(ctx.state, 0xffffu));
	}
	return Binary(ctx.state, spv::OpShiftRightLogical, TypeU32(ctx.state),
	              address, ConstantU32(ctx.state, 2));
}

uint32_t LoadWordInBounds(ValueEmitContext& ctx, const MemoryResourceAccess& resource,
                          uint32_t index);
void StoreWordInBounds(ValueEmitContext& ctx, const MemoryResourceAccess& resource, uint32_t index,
                       uint32_t data);

uint32_t LoadSubwordInBounds(ValueEmitContext& ctx, const MemoryResourceAccess& resource,
                             uint32_t address, uint32_t index, uint32_t bits, bool sign_extend);

uint32_t LoadWordPrepared(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem,
                          const MemoryResourceAccess& resource) {
	const auto index = DwordIndex(ctx, inst, mem);
	return EmitValueOrZeroIfCondition(
	    ctx.state, EmitMemoryElementInBounds(ctx.state, resource, index),
	    [&]() { return LoadWordInBounds(ctx, resource, index); });
}

uint32_t LoadWord(ValueEmitContext& ctx, const IR::Inst& inst, IR::MemoryInfo mem) {
	return EmitValueOrZeroIfCondition(ctx.state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		const auto resource = PrepareMemoryResourceAccess(ctx.state, mem);
		return LoadWordPrepared(ctx, inst, mem, resource);
	});
}

uint32_t LoadSubwordPrepared(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem,
                             const MemoryResourceAccess& resource, uint32_t bits,
                             bool sign_extend) {
	const auto address = ByteAddress(ctx, inst, mem);
	const auto index = Binary(
	    ctx.state, spv::OpShiftRightLogical, TypeU32(ctx.state), address,
	    ConstantU32(ctx.state, std::countr_zero(resource.element_bits / 8u)));
	return EmitValueOrZeroIfCondition(
	    ctx.state, EmitMemoryElementInBounds(ctx.state, resource, index),
	    [&]() { return LoadSubwordInBounds(ctx, resource, address, index, bits, sign_extend); });
}

// A coherent (GLC) load of a 32-bit word as GetCoherentLoadMode asks.
static uint32_t EmitCoherentWordLoad(EmitterState& state, uint32_t pointer) {
	const auto mode  = GetCoherentLoadMode();
	const auto value = state.builder.AllocateId();
	if (mode == CoherentLoadMode::Acquire || mode == CoherentLoadMode::Relaxed) {
		const uint32_t semantics =
		    mode == CoherentLoadMode::Acquire
		        ? static_cast<uint32_t>(spv::MemorySemanticsAcquireMask |
		                                spv::MemorySemanticsUniformMemoryMask)
		        : static_cast<uint32_t>(spv::MemorySemanticsMaskNone);
		state.builder.AddFunction(spv::OpAtomicLoad, TypeU32(state), value, pointer,
		                          ConstantU32(state, spv::ScopeDevice),
		                          ConstantU32(state, semantics));
		return value;
	}
	state.builder.AddFunction(spv::OpLoad, TypeU32(state), value, pointer,
	                          spv::MemoryAccessVolatileMask);
	if (mode == CoherentLoadMode::Barrier) {
		EmitAtomicMemoryBarrier(state, IR::ResourceKind::Buffer);
	}
	return value;
}

uint32_t LoadWordInBounds(ValueEmitContext& ctx, const MemoryResourceAccess& resource,
                          uint32_t index) {
	const auto pointer = EmitMemoryElementPointer(ctx.state, resource, index);
	if (resource.memory_access != spv::MemoryAccessMaskNone) {
		return EmitCoherentWordLoad(ctx.state, pointer);
	}
	const auto value = ctx.state.builder.AllocateId();
	ctx.state.builder.AddFunction(spv::OpLoad, TypeU32(ctx.state), value, pointer,
	                              resource.memory_access);
	return value;
}

uint32_t LoadSubwordInBounds(ValueEmitContext& ctx, const MemoryResourceAccess& resource,
                             uint32_t address, uint32_t index, uint32_t bits, bool sign_extend) {
	uint32_t value;
	if (resource.element_bits < 32u) {
		const auto loaded = ctx.state.builder.AllocateId();
		ctx.state.builder.AddFunction(spv::OpLoad, TypeStorageBufferElement(ctx.state, bits), loaded,
		                              EmitMemoryElementPointer(ctx.state, resource, index),
		                              resource.memory_access);
		value = Unary(ctx.state, spv::OpUConvert, TypeU32(ctx.state), loaded);
	} else {
		const auto word = LoadWordInBounds(ctx, resource, index);
		const auto byte = Binary(ctx.state, spv::OpBitwiseAnd, TypeU32(ctx.state), address,
		                         ConstantU32(ctx.state, 3));
		const auto shift = Binary(ctx.state, spv::OpShiftLeftLogical, TypeU32(ctx.state), byte,
		                          ConstantU32(ctx.state, 3));
		value = Binary(ctx.state, spv::OpBitwiseAnd, TypeU32(ctx.state),
		               Binary(ctx.state, spv::OpShiftRightLogical, TypeU32(ctx.state), word, shift),
		               ConstantU32(ctx.state, bits == 8u ? 0xffu : 0xffffu));
	}
	if (!sign_extend) return value;
	const auto left = Binary(ctx.state, spv::OpShiftLeftLogical, TypeU32(ctx.state), value,
	                         ConstantU32(ctx.state, 32u - bits));
	return Binary(ctx.state, spv::OpShiftRightArithmetic, TypeU32(ctx.state), left,
	              ConstantU32(ctx.state, 32u - bits));
}

uint32_t LoadSubword(ValueEmitContext& ctx, const IR::Inst& inst, IR::MemoryInfo mem, uint32_t bits,
                     bool sign_extend) {
	return EmitValueOrZeroIfCondition(ctx.state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		const auto resource = PrepareMemoryResourceAccess(ctx.state, mem);
		return LoadSubwordPrepared(ctx, inst, mem, resource, bits, sign_extend);
	});
}

Prospero::BufferFormat BufferFormat(const ValueEmitContext& ctx, const IR::MemoryInfo& mem) {
	return mem.typed ? Format::DecodeTBufferFormat(mem.data_format, mem.number_format)
	                 : StorageBufferFormat(ctx.state, mem);
}

IR::MemoryInfo RebaseRawComponent(IR::MemoryInfo mem, uint32_t component) {
	mem.offset += component * 4u;
	mem.data_dwords     = 1u;
	mem.component_index = component;
	return mem;
}

using Format::FormattedSource;
using Format::FormattedSourceKind;

FormattedSource ResolveFormattedSource(ValueEmitContext& ctx, const IR::MemoryInfo& mem,
                                       const Format::BufferFormatInfo& info,
                                       uint32_t                        output_component) {
	if (mem.typed) {
		return output_component < info.component_count
		           ? FormattedSource {FormattedSourceKind::Memory, output_component}
		           : FormattedSource {};
	}
	const auto selector = GetDstSel(ctx.state.program.info.buffers[mem.resource].descriptor_swizzle,
	                                output_component);
	const auto source   = Format::ResolveFormattedSource(info, selector);
	if (source.kind == FormattedSourceKind::Invalid) {
		ExitDescriptorBindingFailure(ctx.state, IR::DescriptorBindingKind::Buffers, mem.resource,
		                             "buffer descriptor has reserved dst_sel");
	}
	return source;
}

uint32_t FormattedConstant(ValueEmitContext& ctx, const Format::BufferFormatInfo& info,
                           FormattedSourceKind kind) {
	return ConstantU32(ctx.state, Format::FormattedConstantBits(info, kind));
}

template <typename LoadWordFn, typename LoadSubwordFn>
uint32_t LoadFormattedComponent(ValueEmitContext& ctx, const Format::BufferFormatInfo& info,
                                FormattedSource source, LoadWordFn&& load_word,
                                LoadSubwordFn&& load_subword) {
	if (source.kind != FormattedSourceKind::Memory) {
		return FormattedConstant(ctx, info, source.kind);
	}
	const auto component = source.component;
	const auto bits      = info.component_bits[component];
	auto raw = info.packed_bitfield || bits == 32u ? load_word(component)
	                                                : load_subword(component, bits);
	if (info.packed_bitfield || (bits < 32u && IsSignedFormatComponent(info.type))) {
		const auto type =
		    IsSignedFormatComponent(info.type) ? TypeI32(ctx.state) : TypeU32(ctx.state);
		const auto source_value =
		    type == TypeI32(ctx.state) ? Unary(ctx.state, spv::OpBitcast, type, raw) : raw;
		const auto extracted = ctx.state.builder.AllocateId();
		ctx.state.builder.AddFunction(IsSignedFormatComponent(info.type) ? spv::OpBitFieldSExtract
		                                                                 : spv::OpBitFieldUExtract,
		                              type, extracted, source_value,
		                              ConstantU32(ctx.state, info.packed_bitfield
		                                                         ? info.component_bit_offset[component]
		                                                         : 0u),
		                              ConstantU32(ctx.state, bits));
		raw = type == TypeI32(ctx.state)
		          ? Unary(ctx.state, spv::OpBitcast, TypeU32(ctx.state), extracted)
		          : extracted;
	}
	return NormalizeFormatComponent(ctx.state, info, component, raw);
}

void StoreSubwordInBounds(ValueEmitContext& ctx, const IR::MemoryInfo& mem,
                          const MemoryResourceAccess& resource, uint32_t address, uint32_t index,
                          uint32_t bits, uint32_t data) {
	const auto pointer = EmitMemoryElementPointer(ctx.state, resource, index);
	if (resource.element_bits < 32u) {
		ctx.state.builder.AddFunction(
		    spv::OpStore, pointer,
		    Unary(ctx.state, spv::OpUConvert, TypeStorageBufferElement(ctx.state, bits), data),
		    resource.memory_access);
		return;
	}
	const auto shift   = Binary(ctx.state, spv::OpShiftLeftLogical, TypeU32(ctx.state),
	                            Binary(ctx.state, spv::OpBitwiseAnd, TypeU32(ctx.state), address,
	                                   ConstantU32(ctx.state, 3)),
	                            ConstantU32(ctx.state, 3));
	const auto mask    = Binary(ctx.state, spv::OpShiftLeftLogical, TypeU32(ctx.state),
	                            ConstantU32(ctx.state, bits == 8u ? 0xffu : 0xffffu), shift);
	const auto value   = Binary(ctx.state, spv::OpShiftLeftLogical, TypeU32(ctx.state),
	                            Binary(ctx.state, spv::OpBitwiseAnd, TypeU32(ctx.state), data,
	                                   ConstantU32(ctx.state, bits == 8u ? 0xffu : 0xffffu)),
	                            shift);
	const auto merge   = [&](uint32_t old) {
		return Binary(ctx.state, spv::OpBitwiseOr, TypeU32(ctx.state),
		              Binary(ctx.state, spv::OpBitwiseAnd, TypeU32(ctx.state), old,
		                     Unary(ctx.state, spv::OpNot, TypeU32(ctx.state), mask)),
		              value);
	};
	if (mem.kind == IR::ResourceKind::Scratch) {
		const auto old = ctx.state.builder.AllocateId();
		ctx.state.builder.AddFunction(spv::OpLoad, TypeU32(ctx.state), old, pointer);
		ctx.state.builder.AddFunction(spv::OpStore, pointer, merge(old));
	} else {
		AtomicUpdate(ctx.state, pointer, mem.kind, merge);
	}
}

void StoreSubwordPrepared(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem,
                          const MemoryResourceAccess& resource, uint32_t bits, uint32_t data) {
	const auto address = ByteAddress(ctx, inst, mem);
	const auto index = Binary(
	    ctx.state, spv::OpShiftRightLogical, TypeU32(ctx.state), address,
	    ConstantU32(ctx.state, std::countr_zero(resource.element_bits / 8u)));
	EmitIfCondition(
	    ctx.state, EmitMemoryElementInBounds(ctx.state, resource, index), [&]() {
		    StoreSubwordInBounds(ctx, mem, resource, address, index, bits, data);
	    });
}

void StoreSubword(ValueEmitContext& ctx, const IR::Inst& inst, IR::MemoryInfo mem, uint32_t bits) {
	EmitIfCondition(ctx.state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		const auto resource = PrepareMemoryResourceAccess(ctx.state, mem);
		StoreSubwordPrepared(ctx, inst, mem, resource, bits, ctx.Arg(inst, inst.NumArgs() - 2));
	});
}

void StoreWordInBounds(ValueEmitContext& ctx, const MemoryResourceAccess& resource, uint32_t index,
                       uint32_t data) {
	ctx.state.builder.AddFunction(spv::OpStore,
	                              EmitMemoryElementPointer(ctx.state, resource, index), data,
	                              resource.memory_access);
}

template <typename Fn>
void ForEachLocalFlatAccess(ValueEmitContext& ctx, const IR::Inst& inst, Fn&& emit) {
	auto& state = ctx.state;
	auto mem = ctx.Memory(inst);
	auto high = ctx.Arg(inst, 2);
	const auto low = AddAddressOffset(state, mem, ctx.Arg(inst, 1), high);
	const auto index = Binary(state, spv::OpShiftRightLogical, TypeU32(state), low, ConstantU32(state, 2));
	const auto aligned = Binary(state, spv::OpIEqual, TypeBool(state), ConstantU32(state, 0),
	    Binary(state, spv::OpBitwiseAnd, TypeU32(state), low, ConstantU32(state, 3)));
	for (const auto kind: {IR::ResourceKind::Scratch, IR::ResourceKind::Lds}) {
		mem.kind = kind;
		const auto resource = PrepareMemoryResourceAccess(state, mem);
		const auto aperture = kind == IR::ResourceKind::Scratch ? Decoder::PrivateApertureHigh
		                                                       : Decoder::SharedApertureHigh;
		const auto selected = Binary(state, spv::OpIEqual, TypeBool(state), high, ConstantU32(state, aperture));
		const auto valid = AndCondition(state, aligned,
		    AndCondition(state, selected, EmitMemoryElementInBounds(state, resource, index)));
		emit(resource, index, valid);
	}
}

uint32_t LoadLocalFlat(ValueEmitContext& ctx, const IR::Inst& inst) {
	return EmitValueOrZeroIfCondition(ctx.state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		uint32_t result = 0;
		ForEachLocalFlatAccess(ctx, inst, [&](const auto& resource, uint32_t index, uint32_t valid) {
			const auto value = EmitValueOrZeroIfCondition(ctx.state, valid, [&]() {
				return LoadWordInBounds(ctx, resource, index);
			});
			result = result == 0 ? value : Binary(ctx.state, spv::OpBitwiseOr, TypeU32(ctx.state), result, value);
		});
		return result;
	});
}

void StoreLocalFlat(ValueEmitContext& ctx, const IR::Inst& inst) {
	EmitIfCondition(ctx.state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		ForEachLocalFlatAccess(ctx, inst, [&](const auto& resource, uint32_t index, uint32_t valid) {
			EmitIfCondition(ctx.state, valid, [&]() {
				StoreWordInBounds(ctx, resource, index, ctx.Arg(inst, inst.NumArgs() - 2));
			});
		});
	});
}

void StoreWordPrepared(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem,
                       const MemoryResourceAccess& resource, uint32_t data) {
	const auto index = DwordIndex(ctx, inst, mem);
	EmitIfCondition(ctx.state, EmitMemoryElementInBounds(ctx.state, resource, index), [&]() {
		StoreWordInBounds(ctx, resource, index, data);
	});
}

void StoreWord(ValueEmitContext& ctx, const IR::Inst& inst, IR::MemoryInfo mem) {
	auto& state = ctx.state;
	EmitIfCondition(state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		const auto data = ctx.Arg(inst, inst.NumArgs() - 2);
		const auto store = [&](uint32_t resource) {
			if (mem.kind == IR::ResourceKind::Buffer) {
				const auto& buffer = state.program.info.buffers[resource];
				if (buffer.packed_stride == 0u &&
				    buffer.source < state.program.descriptor_sources.size() &&
				    state.program.descriptor_sources[buffer.source].indirect_descriptor) return;
			}
			mem.resource = resource;
			const auto access = PrepareMemoryResourceAccess(state, mem);
			StoreWordPrepared(ctx, inst, mem, access, data);
		};
		if (mem.kind != IR::ResourceKind::Buffer ||
		    state.program.info.buffers[mem.resource].indirect_root != mem.resource) {
			store(mem.resource);
			return;
		}
		const auto& buffer = state.program.info.buffers[mem.resource];
		const auto* handle = inst.Arg(0).ResolveInstruction();
		if (handle == nullptr || buffer.indirect_resources.empty() ||
		    state.flattened_srt_variable == 0 || buffer.indirect_search_iterations == 0u) {
			ctx.Fail(inst, "has no indirect buffer runtime mapping");
			return;
		}
		const auto selected = EmitIndirectResourceIndex(
		    state, ctx.Arg(*handle, 0), buffer.indirect_mapping_offset,
		    buffer.indirect_search_iterations, UINT32_MAX);
		const auto merge = state.builder.AllocateId();
		std::vector<uint32_t> labels(buffer.indirect_resources.size());
		std::vector<uint32_t> branches {spv::OpSwitch, selected, merge};
		for (uint32_t i = 0; i < labels.size(); ++i) {
			labels[i] = state.builder.AllocateId();
			branches.push_back(i);
			branches.push_back(labels[i]);
		}
		state.builder.AddFunction(spv::OpSelectionMerge, merge, spv::SelectionControlMaskNone);
		state.builder.AddFunction(branches);
		for (uint32_t i = 0; i < labels.size(); ++i) {
			EmitLabel(state, labels[i]);
			store(buffer.indirect_resources[i]);
			state.builder.AddFunction(spv::OpBranch, merge);
		}
		EmitLabel(state, merge);
	});
}

template <typename Fn>
uint32_t EmitAtomicAccess(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem,
                          Fn&& operation) {
	return EmitValueOrZeroIfCondition(ctx.state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		const auto index = DwordIndex(ctx, inst, mem);
		const auto resource = PrepareMemoryResourceAccess(ctx.state, mem);
		return EmitValueOrZeroIfCondition(
		    ctx.state, EmitMemoryElementInBounds(ctx.state, resource, index), [&]() {
			    return operation(EmitMemoryElementPointer(ctx.state, resource, index));
		    });
	});
}

template <typename Fn>
uint32_t EmitAtomicUpdate(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem,
                          Fn&& replacement) {
	const auto value = ctx.Arg(inst, inst.NumArgs() - 2);
	return EmitAtomicAccess(ctx, inst, mem, [&](uint32_t pointer) {
		return AtomicUpdate(ctx.state, pointer, mem.kind,
		                    [&](uint32_t old) { return replacement(ctx.state, old, value); });
	});
}

uint32_t AtomicIncrement(EmitterState& state, uint32_t old, uint32_t limit) {
	// old >= limit ? 0 : old + 1 (unsigned).
	const auto wrap = Binary(state, spv::OpUGreaterThanEqual, TypeBool(state), old, limit);
	const auto next = Binary(state, spv::OpIAdd, TypeU32(state), old, ConstantU32(state, 1));
	return Select(state, TypeU32(state), wrap, ConstantU32(state, 0), next);
}

uint32_t AtomicDecrement(EmitterState& state, uint32_t old, uint32_t limit) {
	// old == 0 || old > limit ? limit : old - 1 (unsigned).
	const auto zero  = Binary(state, spv::OpIEqual, TypeBool(state), old, ConstantU32(state, 0));
	const auto above = Binary(state, spv::OpUGreaterThan, TypeBool(state), old, limit);
	const auto wrap  = Binary(state, spv::OpLogicalOr, TypeBool(state), zero, above);
	const auto next  = Binary(state, spv::OpISub, TypeU32(state), old, ConstantU32(state, 1));
	return Select(state, TypeU32(state), wrap, limit, next);
}

struct PreparedFormattedMemory {
	Format::BufferFormatInfo info;
	MemoryResourceAccess     resource;
	uint32_t                 base = 0;
	uint32_t                 in_bounds = 0;
};

PreparedFormattedMemory PrepareFormattedMemory(ValueEmitContext& ctx, const IR::Inst& inst,
                                               const IR::MemoryInfo& mem,
                                               const MemoryResourceAccess& resource,
                                               const Format::BufferFormatInfo& info) {
	PreparedFormattedMemory plan;
	plan.info      = info;
	plan.resource  = resource;
	const auto start = ByteAddress(ctx, inst, mem);
	// Check the whole, unaligned format span before aligning the actual memory address.
	const auto end = Binary(ctx.state, spv::OpIAdd, TypeU32(ctx.state), start,
	                        ConstantU32(ctx.state, info.byte_size - 1u));
	const auto last_index = Binary(ctx.state, spv::OpShiftRightLogical, TypeU32(ctx.state), end,
	                               ConstantU32(ctx.state, std::countr_zero(resource.element_bits / 8u)));
	plan.in_bounds = AndCondition(
	    ctx.state, Binary(ctx.state, spv::OpUGreaterThanEqual, TypeBool(ctx.state), end, start),
	    EmitMemoryElementInBounds(ctx.state, plan.resource, last_index));
	plan.base = Binary(ctx.state, spv::OpBitwiseAnd, TypeU32(ctx.state), start,
	                   ConstantU32(ctx.state, ~(std::min(4u, info.byte_size) - 1u)));
	return plan;
}

std::pair<uint32_t, uint32_t> FormattedComponentAddress(EmitterState& state,
                                                       const PreparedFormattedMemory& plan,
                                                       uint32_t component) {
	// Components advance sequentially from one address; do not reapply buffer swizzling.
	const auto offset = Format::GetFormatComponentByteOffset(plan.info, component);
	const auto address = offset == 0u ? plan.base : Binary(
	    state, spv::OpIAdd, TypeU32(state), plan.base, ConstantU32(state, offset));
	return {address, Binary(state, spv::OpShiftRightLogical, TypeU32(state), address,
	                        ConstantU32(state, std::countr_zero(plan.resource.element_bits / 8u)))};
}

uint32_t LoadFormattedInBounds(ValueEmitContext& ctx, const IR::MemoryInfo& mem,
                               const PreparedFormattedMemory& plan, uint32_t output_component) {
	return LoadFormattedComponent(
	    ctx, plan.info, ResolveFormattedSource(ctx, mem, plan.info, output_component),
	    [&](uint32_t component) {
		    return LoadWordInBounds(ctx, plan.resource,
		                            FormattedComponentAddress(ctx.state, plan, component).second);
	    },
	    [&](uint32_t component, uint32_t bits) {
		    const auto [address, index] = FormattedComponentAddress(ctx.state, plan, component);
		    return LoadSubwordInBounds(ctx, plan.resource, address, index, bits, false);
	    });
}

uint32_t ConstructU32Composite(EmitterState& state, uint32_t components,
                               const std::array<uint32_t, 4>& values) {
	if (components == 1u) return values[0];
	const auto            result = state.builder.AllocateId();
	std::vector<uint32_t> words {spv::OpCompositeConstruct, TypeU32Composite(state, components),
	                             result};
	words.insert(words.end(), values.begin(), values.begin() + components);
	state.builder.AddFunction(words);
	return result;
}

uint32_t FormattedOutOfBoundsValue(ValueEmitContext& ctx, const IR::MemoryInfo& mem,
                                   const PreparedFormattedMemory& plan, uint32_t components) {
	std::array<uint32_t, 4> values {};
	for (uint32_t component = 0; component < components; component++) {
		const auto source = ResolveFormattedSource(ctx, mem, plan.info, component);
		values[component] = FormattedConstant(ctx, plan.info, source.kind);
	}
	return ConstructU32Composite(ctx.state, components, values);
}

uint32_t FormattedLoad(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem) {
	return EmitValueOrZeroIfCondition(ctx.state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		const auto resource = PrepareMemoryResourceAccess(ctx.state, mem);
		const auto info = Format::GetFormatInfo(BufferFormat(ctx, mem));
		if (info.type == Format::ComponentType::Unknown) {
			return LoadWordPrepared(ctx, inst, RebaseRawComponent(mem, 0u), resource);
		}
		const auto plan = PrepareFormattedMemory(ctx, inst, mem, resource, info);
		return EmitValueOrDefaultIfCondition(
		    ctx.state, plan.in_bounds, TypeU32(ctx.state),
		    FormattedOutOfBoundsValue(ctx, mem, plan, 1u),
		    [&]() { return LoadFormattedInBounds(ctx, mem, plan, 0u); });
	});
}

uint32_t EncodeFormattedStoreComponent(ValueEmitContext& ctx,
                                       const Format::BufferFormatInfo& info,
                                       uint32_t component, uint32_t data) {
	auto& state = ctx.state;
	const auto bits = info.component_bits[component];
	const bool normalized = info.type == Format::ComponentType::Unorm ||
	                        info.type == Format::ComponentType::Snorm;
	const bool scaled = info.type == Format::ComponentType::Uscaled ||
	                    info.type == Format::ComponentType::Sscaled;
	if (normalized || scaled) {
		// Two-bit packed channels are unsigned even in signed formats.
		const bool is_signed = bits != 2u && (info.type == Format::ComponentType::Snorm ||
		                                      info.type == Format::ComponentType::Sscaled);
		const auto max_value = static_cast<float>((1u << (bits - (is_signed ? 1u : 0u))) - 1u);
		const auto lower = normalized ? (is_signed ? -1.0f : 0.0f)
		                              : (is_signed ? -max_value - 1.0f : 0.0f);
		const auto upper = normalized ? 1.0f : max_value;
		auto value = Select(state, TypeF32(state), EmitClassifyF32Bits(state, data).nan,
		                    ConstantF32Value(state, 0.0f), EmitBitCastF32U32(state, data));
		value = EmitGlsl<GLSLstd450FClamp, IR::Type::F32>(
		    state, value, ConstantF32Value(state, lower), ConstantF32Value(state, upper));
		if (normalized) {
			value = EmitFPRoundEven32(
			    state, EmitFPMul32(state, value, ConstantF32Value(state, max_value)));
		}
		// Float-to-integer conversion truncates scaled values toward zero.
		value = Unary(state, is_signed ? spv::OpConvertFToS : spv::OpConvertFToU,
		              is_signed ? TypeI32(state) : TypeU32(state), value);
		return is_signed ? Unary(state, spv::OpBitcast, TypeU32(state), value) : value;
	}
	if (bits == 16u && info.type == Format::ComponentType::Float) {
		// Beyond the half range the guest GPU stores the largest half (D3D's conversion), where
		// packHalf2x16 overflows to infinity.
		auto value = EmitBitCastF32U32(state, data);
		if (IR::FloatStoreClampEnabled()) {
			value = EmitClampToHalfRange(state, value);
		}
		const auto pair = EmitCompositeConstructF32x2(state, value, ConstantF32Value(state, 0.0f));
		return EmitPackHalf2x16(state, pair);
	}
	if ((bits == 11u || bits == 10u) && info.type == Format::ComponentType::Float) {
		// Packed unsigned floats (11_11_10, 10_11_11): encoded, not the float's low bits.
		return EmitF32ToUFloatBits(state, data, bits);
	}
	return data;
}

void StoreFormattedPrepared(ValueEmitContext& ctx, const IR::Inst& inst,
                             const IR::MemoryInfo& mem, const MemoryResourceAccess& resource,
                             const Format::BufferFormatInfo& info, uint32_t data,
                             uint32_t components) {
	// RDNA2 formatted stores transfer the entire format, zero-filling missing VGPRs.
	const auto plan = PrepareFormattedMemory(ctx, inst, mem, resource, info);
	EmitIfCondition(ctx.state, plan.in_bounds, [&]() {
		auto packed = ConstantU32(ctx.state, 0);
		for (uint32_t component = 0; component < info.component_count; component++) {
			auto value = ConstantU32(ctx.state, 0);
			if (component < components) {
				value = data;
				if (components != 1u) {
					value = ctx.state.builder.AllocateId();
					ctx.state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(ctx.state),
					                              value, data, component);
				}
			}
			value = EncodeFormattedStoreComponent(ctx, info, component, value);
			const auto bits = info.component_bits[component];
			if (info.packed_bitfield) {
				packed = EmitBitFieldInsert(
				    ctx.state, packed, value,
				    ConstantU32(ctx.state, info.component_bit_offset[component]),
				    ConstantU32(ctx.state, bits));
			} else {
				const auto [address, index] = FormattedComponentAddress(ctx.state, plan, component);
				if (bits == 8u || bits == 16u) {
					StoreSubwordInBounds(ctx, mem, plan.resource, address, index, bits, value);
				} else {
					StoreWordInBounds(ctx, plan.resource, index, value);
				}
			}
		}
		if (info.packed_bitfield) {
			StoreWordInBounds(ctx, plan.resource,
			                  FormattedComponentAddress(ctx.state, plan, 0u).second, packed);
		}
	});
}

void FormattedStore(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem) {
	EmitIfCondition(ctx.state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		const auto resource = PrepareMemoryResourceAccess(ctx.state, mem);
		const auto data     = ctx.Arg(inst, inst.NumArgs() - 2);
		const auto info     = Format::GetFormatInfo(BufferFormat(ctx, mem));
		if (info.type == Format::ComponentType::Unknown) {
			StoreWordPrepared(ctx, inst, RebaseRawComponent(mem, 0u), resource, data);
			return;
		}
		StoreFormattedPrepared(ctx, inst, mem, resource, info, data, 1u);
	});
}

uint32_t LoadIndirectScalarBuffer(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto&       state  = ctx.state;
	const auto& handle = *inst.Arg(0).ResolveInstruction();
	const auto  word1  = ctx.Arg(handle, 1);
	const auto  stride = EmitBitFieldUExtract(state, word1, ConstantU32(state, 16),
	                                         ConstantU32(state, 14));
	const auto size = Binary(
	    state, spv::OpIMul, TypeScalarU64(state),
	    Unary(state, spv::OpUConvert, TypeScalarU64(state),
	          EmitUMax32(state, stride, ConstantU32(state, 1))),
	    Unary(state, spv::OpUConvert, TypeScalarU64(state), ctx.Arg(handle, 2)));
	const auto offset = Binary(
	    state, spv::OpIAdd, TypeScalarU64(state),
	    Unary(state, spv::OpUConvert, TypeScalarU64(state),
	          Binary(state, spv::OpBitwiseAnd, TypeU32(state), ctx.Arg(inst, 1),
	                 ConstantU32(state, ~3u))),
	    ConstantDeviceAddress(state, ctx.Memory(inst).offset & ~3u));
	const auto end = Binary(state, spv::OpIAdd, TypeScalarU64(state), offset,
	                        ConstantDeviceAddress(state, sizeof(uint32_t)));
	return EmitValueOrZeroIfCondition(
	    state, Binary(state, spv::OpULessThanEqual, TypeBool(state), end, size), [&]() {
		    const auto base = DeviceAddressFromWords(
		        state, Binary(state, spv::OpBitwiseAnd, TypeU32(state), ctx.Arg(handle, 0),
		                      ConstantU32(state, ~3u)),
		        Binary(state, spv::OpBitwiseAnd, TypeU32(state), word1, ConstantU32(state, 0xffffu)));
		    return LoadBdaDword(ctx, Binary(state, spv::OpIAdd, TypeScalarU64(state), base, offset));
	    });
}

struct IndirectBufferAccess {
	uint32_t address;
	uint32_t offset;
	uint32_t stride;
	uint32_t swizzle;
	uint32_t format;
	uint32_t selector;
	uint32_t mode;
	uint32_t records;
	uint32_t index_in_bounds;
	uint32_t scalar_in_bounds;
	uint32_t raw_records;
	uint32_t raw_index_in_bounds;
};

// component: the result component whose destination selector (DST_SEL_X..W) .selector holds.
IndirectBufferAccess PrepareIndirectBuffer(ValueEmitContext& ctx, const IR::Inst& inst,
                                           uint32_t component = 0) {
	auto&       state  = ctx.state;
	const auto& handle = *inst.Arg(0).ResolveInstruction();
	const auto word1   = ctx.Arg(handle, 1);
	const auto records = ctx.Arg(handle, 2);
	const auto word3   = ctx.Arg(handle, 3);
	const auto field = [&](uint32_t word, uint32_t first, uint32_t count) {
		return EmitBitFieldUExtract(state, word, ConstantU32(state, first), ConstantU32(state, count));
	};
	const auto nonzero = [&](uint32_t value) {
		return Binary(state, spv::OpINotEqual, TypeBool(state), value, ConstantU32(state, 0));
	};
	const auto stride  = field(word1, 16, 14);
	const auto swizzle = AndCondition(state, nonzero(stride), nonzero(field(word1, 31, 1)));
	const auto index = Binary(
	    state, spv::OpIAdd, TypeU32(state), ctx.Arg(inst, 1),
	    Select(state, TypeU32(state), nonzero(field(word3, 23, 1)), BufferLane(state),
	           ConstantU32(state, 0)));
	const auto soffset = ctx.Arg(inst, 3);
	const auto address = CalculateBufferAddress(state, index, ctx.Arg(inst, 2), soffset,
	                                            ctx.Memory(inst).offset, stride, swizzle,
	                                            field(word3, 21, 2));
	const auto base = DeviceAddressFromWords(state, ctx.Arg(handle, 0), field(word1, 0, 16));
	const auto raw_records = Binary(state, spv::OpISub, TypeU32(state), records, soffset);
	return {
	    .address = Binary(state, spv::OpIAdd, TypeScalarU64(state), base,
	                      Unary(state, spv::OpUConvert, TypeScalarU64(state), address.byte)),
	    .offset = address.offset,
	    .stride = stride,
	    .swizzle = swizzle,
	    .format = field(word3, 12, 7),
	    .selector = ctx.Memory(inst).formatted ? field(word3, component * 3u, 3) : 0u,
	    .mode = field(word3, 28, 2),
	    .records = records,
	    .index_in_bounds = Binary(state, spv::OpULessThan, TypeBool(state), index, records),
	    .scalar_in_bounds = Binary(state, spv::OpULessThanEqual, TypeBool(state), soffset, records),
	    .raw_records = raw_records,
	    .raw_index_in_bounds = Binary(state, spv::OpULessThan, TypeBool(state), index, raw_records),
	};
}

uint32_t IndirectBufferInBounds(EmitterState& state, const IndirectBufferAccess& buffer,
                                uint32_t displacement, uint32_t bytes, bool formatted) {
	const auto offset = displacement == 0u ? buffer.offset : Binary(
	    state, spv::OpIAdd, TypeU32(state), buffer.offset, ConstantU32(state, displacement));
	const auto has_payload = [&](uint32_t size) {
		return AndCondition(
		    state, Binary(state, spv::OpUGreaterThanEqual, TypeBool(state), size,
		                  ConstantU32(state, bytes)),
		    Binary(state, spv::OpULessThanEqual, TypeBool(state), offset,
		           Binary(state, spv::OpISub, TypeU32(state), size, ConstantU32(state, bytes))));
	};
	const auto structured = AndCondition(
	    state, buffer.index_in_bounds,
	    formatted ? has_payload(buffer.stride)
	              : Binary(state, spv::OpULessThan, TypeBool(state), offset, buffer.stride));
	// OOB_SELECT=3 reduces NUM_RECORDS by SOFFSET before its offset/index checks.
	const auto raw = AndCondition(
	    state, buffer.scalar_in_bounds,
	    Select(state, TypeBool(state), buffer.swizzle,
	           AndCondition(state, buffer.raw_index_in_bounds, has_payload(buffer.stride)),
	           has_payload(buffer.raw_records)));
	auto in_bounds = Select(
	    state, TypeBool(state),
	    Binary(state, spv::OpIEqual, TypeBool(state), buffer.mode, ConstantU32(state, 0)),
	    structured, buffer.index_in_bounds);
	in_bounds = Select(
	    state, TypeBool(state),
	    Binary(state, spv::OpULessThan, TypeBool(state), buffer.mode, ConstantU32(state, 2)),
	    in_bounds,
	    Select(state, TypeBool(state),
	           Binary(state, spv::OpIEqual, TypeBool(state), buffer.mode, ConstantU32(state, 2)),
	           Binary(state, spv::OpINotEqual, TypeBool(state), buffer.records, ConstantU32(state, 0)),
	           raw));
	return in_bounds;
}

uint32_t LoadIndirectBuffer(ValueEmitContext& ctx, const IR::Inst& inst, uint32_t components) {
	auto& state = ctx.state;
	const auto buffer = PrepareIndirectBuffer(ctx, inst);
	const auto valid_format = Binary(state, spv::OpINotEqual, TypeBool(state), buffer.format,
	                                  ConstantU32(state, 0));
	const auto base = Binary(state, spv::OpBitwiseAnd, TypeScalarU64(state), buffer.address,
	                         ConstantDeviceAddress(state, ~uint64_t {3}));
	std::array<uint32_t, 4> values {};
	for (uint32_t component = 0; component < components; ++component) {
		const auto address = component == 0u ? base : Binary(
		    state, spv::OpIAdd, TypeScalarU64(state), base,
		    ConstantDeviceAddress(state, component * 4u));
		values[component] = EmitValueOrZeroIfCondition(
		    state, AndCondition(state, valid_format,
		                        IndirectBufferInBounds(state, buffer, component * 4u, 4u, false)),
		    [&]() { return LoadBdaDword(ctx, address); });
	}
	return components == 1u ? values[0] : ConstructU32Composite(state, components, values);
}

// One result component of a formatted load through a GPU-selected V#: the element's format
// and the component's destination selector come from the descriptor at run time.
uint32_t LoadIndirectFormattedX(ValueEmitContext& ctx, const IR::Inst& inst,
                                uint32_t result_component = 0) {
	auto& state = ctx.state;
	return EmitValueOrZeroIfCondition(state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		const auto buffer = PrepareIndirectBuffer(ctx, inst, result_component);
		return EmitIndexSwitch(
		    state, buffer.format, static_cast<uint32_t>(Prospero::BufferFormat::k32_32_32_32Float) + 1u,
		    TypeU32(state), [&](uint32_t format) {
			    const auto info = Format::GetFormatInfo(static_cast<Prospero::BufferFormat>(format));
			    if (info.type == Format::ComponentType::Unknown) return ConstantU32(state, 0);
			    const auto constant = Select(
			        state, TypeU32(state),
			        Binary(state, spv::OpIEqual, TypeBool(state), buffer.selector, ConstantU32(state, 1)),
			        FormattedConstant(ctx, info, FormattedSourceKind::One), ConstantU32(state, 0));
			    return EmitValueOrDefaultIfCondition(
			        state, Binary(state, spv::OpUGreaterThanEqual, TypeBool(state), buffer.selector,
			                      ConstantU32(state, 4)),
			        TypeU32(state), constant, [&]() {
				        const auto in_bounds = IndirectBufferInBounds(state, buffer, 0u, info.byte_size, true);
				        const auto base = Binary(
				            state, spv::OpBitwiseAnd, TypeScalarU64(state), buffer.address,
				            ConstantDeviceAddress(state, ~(uint64_t(std::min(4u, info.byte_size)) - 1u)));
				        const auto load = [&](uint32_t component, uint32_t bits) {
					        const auto offset = Format::GetFormatComponentByteOffset(info, component);
					        const auto address = offset == 0u ? base : Binary(
					            state, spv::OpIAdd, TypeScalarU64(state), base,
					            ConstantDeviceAddress(state, offset));
					        return LoadBda(ctx, address, in_bounds, bits);
				        };
				        const auto emit_component = [&](uint32_t component) {
					        return LoadFormattedComponent(
					            ctx, info, {FormattedSourceKind::Memory, component},
					            [&](uint32_t source) { return load(source, 32u); }, load);
				        };
				        if (info.component_count == 1u) return emit_component(0u);
				        const auto component = Binary(
				            state, spv::OpUMod, TypeU32(state),
				            Binary(state, spv::OpISub, TypeU32(state), buffer.selector, ConstantU32(state, 4)),
				            ConstantU32(state, info.component_count));
				        return EmitIndexSwitch(state, component, info.component_count, TypeU32(state),
				                               emit_component);
			        });
		    });
	});
}

// The layout of every buffer format (Format::GetFormatInfo) as three words, for formatted loads
// whose format a GPU-selected V# names at run time: the component type, count, element bytes
// and packing; each component's bit offset (a byte each); each component's bit count. A Private
// array, so one table serves every such load of the shader.
constexpr uint32_t IndirectFormatCount =
    static_cast<uint32_t>(Prospero::BufferFormat::k32_32_32_32Float) + 1u;

uint32_t IndirectFormatTable(EmitterState& state) {
	if (state.indirect_format_table != 0) {
		return state.indirect_format_table;
	}
	std::vector<uint32_t> words;
	for (uint32_t format = 0; format < IndirectFormatCount; format++) {
		const auto info   = Format::GetFormatInfo(static_cast<Prospero::BufferFormat>(format));
		uint32_t   layout = 0, offsets = 0, bits = 0;
		if (info.type != Format::ComponentType::Unknown && info.component_count != 0u) {
			layout = static_cast<uint32_t>(info.type) | (info.component_count << 3u) |
			         (info.byte_size << 6u) | (info.packed_bitfield ? 1u << 11u : 0u);
			for (uint32_t component = 0; component < 4u; component++) {
				offsets |= (info.component_bit_offset[component] & 0xffu) << (component * 8u);
				bits |= (info.component_bits[component] & 0xffu) << (component * 8u);
			}
		}
		words.push_back(layout);
		words.push_back(offsets);
		words.push_back(bits);
	}
	const auto array_type = state.builder.Type(
	    spv::OpTypeArray, TypeU32(state), ConstantU32(state, static_cast<uint32_t>(words.size())));
	std::vector<uint32_t> operands;
	operands.reserve(words.size());
	for (const auto word: words) {
		operands.push_back(ConstantU32(state, word));
	}
	const auto initializer = state.builder.Constant(spv::OpConstantComposite, array_type,
	                                                std::span<const uint32_t>(operands));
	state.indirect_format_table =
	    state.builder.DefineGlobalVariable(TypePointer(state, spv::StorageClassPrivate, array_type),
	                                       spv::StorageClassPrivate, initializer);
	state.builder.AddName(state.indirect_format_table, "indirect_format_table");
	return state.indirect_format_table;
}

uint32_t IndirectFormatWord(EmitterState& state, uint32_t format, uint32_t word) {
	const auto table = IndirectFormatTable(state);
	const auto index =
	    Binary(state, spv::OpIAdd, TypeU32(state),
	           Binary(state, spv::OpIMul, TypeU32(state), format, ConstantU32(state, 3)),
	           ConstantU32(state, word));
	const auto pointer = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpAccessChain,
	                          TypePointer(state, spv::StorageClassPrivate, TypeU32(state)), pointer,
	                          table, index);
	const auto value = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpLoad, TypeU32(state), value, pointer);
	return value;
}

// IndirectBufferInBounds with an element size known only at run time.
uint32_t IndirectBufferInBoundsBytes(EmitterState& state, const IndirectBufferAccess& buffer,
                                     uint32_t bytes) {
	const auto boolean     = TypeBool(state);
	const auto u32         = TypeU32(state);
	const auto has_payload = [&](uint32_t size) {
		return Binary(state, spv::OpLogicalAnd, boolean,
		              Binary(state, spv::OpUGreaterThanEqual, boolean, size, bytes),
		              Binary(state, spv::OpULessThanEqual, boolean, buffer.offset,
		                     Binary(state, spv::OpISub, u32, size, bytes)));
	};
	const auto structured = Binary(state, spv::OpLogicalAnd, boolean, buffer.index_in_bounds,
	                               has_payload(buffer.stride));
	const auto raw        = Binary(state, spv::OpLogicalAnd, boolean, buffer.scalar_in_bounds,
	                               Select(state, boolean, buffer.swizzle,
	                                      Binary(state, spv::OpLogicalAnd, boolean,
	                                             buffer.raw_index_in_bounds, has_payload(buffer.stride)),
	                                      has_payload(buffer.raw_records)));
	auto       in_bounds  = Select(
        state, boolean, Binary(state, spv::OpIEqual, boolean, buffer.mode, ConstantU32(state, 0)),
        structured, buffer.index_in_bounds);
	return Select(
	    state, boolean,
	    Binary(state, spv::OpULessThan, boolean, buffer.mode, ConstantU32(state, 2)), in_bounds,
	    Select(state, boolean,
	           Binary(state, spv::OpIEqual, boolean, buffer.mode, ConstantU32(state, 2)),
	           Binary(state, spv::OpINotEqual, boolean, buffer.records, ConstantU32(state, 0)),
	           raw));
}

// The result components of a formatted load through a GPU-selected V#, decoded from the format
// table: the component each destination selector picks is extracted from its dword and
// normalized by its format's component type, as LoadFormattedComponent does for a known format.
// Straight-line code, where a switch over every format (LoadIndirectFormattedX) made one material
// shader 30 MB of SPIR-V. KYTY_INDIRECT_FORMAT_SWITCH=1 restores the switch. Round 99: what the
// components share (the element's address and bounds, its format's layout) is made once per
// load, not once per component: a third of a material pass's SPIR-V was these loads.
std::array<uint32_t, 4> LoadIndirectFormattedComponents(ValueEmitContext& ctx, const IR::Inst& inst,
                                                        uint32_t components) {
	auto&      state   = ctx.state;
	const auto u32     = TypeU32(state);
	const auto i32     = TypeI32(state);
	const auto f32     = TypeF32(state);
	const auto boolean = TypeBool(state);
	const auto buffer  = PrepareIndirectBuffer(ctx, inst, 0);
	const auto word3   = ctx.Arg(*inst.Arg(0).ResolveInstruction(), 3);
	const auto field   = [&](uint32_t word, uint32_t first, uint32_t count) {
        return EmitBitFieldUExtract(state, word, ConstantU32(state, first),
		                              ConstantU32(state, count));
	};
	const auto format  = Select(state, u32,
	                            Binary(state, spv::OpULessThan, boolean, buffer.format,
	                                   ConstantU32(state, IndirectFormatCount)),
	                            buffer.format, ConstantU32(state, 0));
	const auto layout  = IndirectFormatWord(state, format, 0);
	const auto offsets = IndirectFormatWord(state, format, 1);
	const auto widths  = IndirectFormatWord(state, format, 2);
	const auto type    = field(layout, 0, 3);
	const auto count   = field(layout, 3, 3);
	const auto bytes   = field(layout, 6, 5);
	const auto packed =
	    Binary(state, spv::OpINotEqual, boolean, field(layout, 11, 1), ConstantU32(state, 0));
	const auto type_is = [&](Format::ComponentType value) {
		return Binary(state, spv::OpIEqual, boolean, type,
		              ConstantU32(state, static_cast<uint32_t>(value)));
	};
	const auto is_sint    = type_is(Format::ComponentType::Sint);
	const auto is_unorm   = type_is(Format::ComponentType::Unorm);
	const auto is_snorm   = type_is(Format::ComponentType::Snorm);
	const auto is_uscaled = type_is(Format::ComponentType::Uscaled);
	const auto is_sscaled = type_is(Format::ComponentType::Sscaled);
	const auto is_float   = type_is(Format::ComponentType::Float);
	const auto integer =
	    Binary(state, spv::OpLogicalOr, boolean, type_is(Format::ComponentType::Uint), is_sint);
	// Selector 1 reads one (1 for integer formats, 1.0 otherwise), 0, 2 and 3 zero, 4-7 the
	// format's components, wrapped to its count.
	const auto one =
	    Select(state, u32, integer, ConstantU32(state, 1u), ConstantU32(state, 0x3f800000u));
	const auto has_components =
	    Binary(state, spv::OpINotEqual, boolean, count, ConstantU32(state, 0));
	const auto              in_bounds = IndirectBufferInBoundsBytes(state, buffer, bytes);
	std::array<uint32_t, 4> values {};
	for (uint32_t result_component = 0; result_component < components; result_component++) {
		const auto selector =
		    result_component == 0u ? buffer.selector : field(word3, result_component * 3u, 3);
		const auto constant = Select(
		    state, u32, Binary(state, spv::OpIEqual, boolean, selector, ConstantU32(state, 1)), one,
		    ConstantU32(state, 0));
		const auto valid = Binary(
		    state, spv::OpLogicalAnd, boolean, has_components,
		    Binary(state, spv::OpUGreaterThanEqual, boolean, selector, ConstantU32(state, 4)));
		values[result_component] =
		    EmitValueOrDefaultIfCondition(state, valid, u32, constant, [&]() {
			    const auto component =
			        Binary(state, spv::OpUMod, u32,
			               Binary(state, spv::OpISub, u32, selector, ConstantU32(state, 4)), count);
			    const auto shift8 =
			        Binary(state, spv::OpShiftLeftLogical, u32, component, ConstantU32(state, 3));
			    const auto offset =
			        EmitBitFieldUExtract(state, offsets, shift8, ConstantU32(state, 8));
			    const auto width =
			        EmitBitFieldUExtract(state, widths, shift8, ConstantU32(state, 8));
			    // The component's byte (unpacked) or the element's dword (packed bitfields), and
			    // the bit it starts at in the dword that holds it.
			    const auto byte_address =
			        Select(state, TypeScalarU64(state), packed, buffer.address,
			               Binary(state, spv::OpIAdd, TypeScalarU64(state), buffer.address,
			                      Unary(state, spv::OpUConvert, TypeScalarU64(state),
			                            Binary(state, spv::OpShiftRightLogical, u32, offset,
			                                   ConstantU32(state, 3)))));
			    const auto in_dword = Binary(
			        state, spv::OpShiftLeftLogical, u32,
			        Binary(state, spv::OpBitwiseAnd, u32,
			               Unary(state, spv::OpUConvert, u32, byte_address), ConstantU32(state, 3)),
			        ConstantU32(state, 3));
			    const auto bit = Binary(
			        state, spv::OpIAdd, u32, in_dword,
			        Select(state, u32, packed,
			               Binary(state, spv::OpBitwiseAnd, u32, offset, ConstantU32(state, 31)),
			               ConstantU32(state, 0)));
			    const auto dword      = EmitValueOrZeroIfCondition(state, in_bounds, [&]() {
                    return LoadBdaDword(ctx, Binary(state, spv::OpBitwiseAnd, TypeScalarU64(state),
				                                         byte_address,
				                                         ConstantDeviceAddress(state, ~uint64_t {3})));
                });
			    const auto safe_width = Select(
			        state, u32,
			        Binary(state, spv::OpULessThanEqual, boolean,
			               Binary(state, spv::OpIAdd, u32, bit, width), ConstantU32(state, 32)),
			        width, Binary(state, spv::OpISub, u32, ConstantU32(state, 32), bit));
			    const auto raw        = EmitBitFieldUExtract(state, dword, bit, safe_width);
			    const auto signed_raw = EmitBitFieldSExtract(state, dword, bit, safe_width);
			    const auto to_bits    = [&](uint32_t value) {
                    return Unary(state, spv::OpBitcast, u32, value);
			    };
			    const auto as_float = [&](uint32_t value) {
				    const auto result = state.builder.AllocateId();
				    state.builder.AddFunction(spv::OpConvertUToF, f32, result, value);
				    return result;
			    };
			    const auto unsigned_float = as_float(raw);
			    const auto signed_float   = state.builder.AllocateId();
			    state.builder.AddFunction(spv::OpConvertSToF, f32, signed_float,
			                              Unary(state, spv::OpBitcast, i32, signed_raw));
			    const auto masked =
			        Binary(state, spv::OpBitwiseAnd, u32, safe_width, ConstantU32(state, 31));
			    const auto max_unsigned = Binary(
			        state, spv::OpISub, u32,
			        Binary(state, spv::OpShiftLeftLogical, u32, ConstantU32(state, 1), masked),
			        ConstantU32(state, 1));
			    const auto max_signed =
			        Binary(state, spv::OpISub, u32,
			               Binary(state, spv::OpShiftLeftLogical, u32, ConstantU32(state, 1),
			                      Binary(state, spv::OpBitwiseAnd, u32,
			                             Binary(state, spv::OpISub, u32, safe_width,
			                                    ConstantU32(state, 1)),
			                             ConstantU32(state, 31))),
			               ConstantU32(state, 1));
			    const auto unorm = to_bits(
			        Binary(state, spv::OpFDiv, f32, unsigned_float, as_float(max_unsigned)));
			    const auto snorm_value =
			        Binary(state, spv::OpFDiv, f32, signed_float, as_float(max_signed));
			    const auto snorm_clamped = state.builder.AllocateId();
			    state.builder.AddFunction(spv::OpExtInst, f32, snorm_clamped, GlslStd450(state),
			                              GLSLstd450FMax, snorm_value,
			                              ConstantF32Value(state, -1.0f));
			    const auto width_is = [&](uint32_t value) {
				    return Binary(state, spv::OpIEqual, boolean, width, ConstantU32(state, value));
			    };
			    // The 11- and 10-bit unsigned floats (5-bit exponent, the mantissa the rest), in
			    // one.
			    const auto small_float = EmitUFloatToF32BitsRuntime(
			        state, raw, Binary(state, spv::OpISub, u32, width, ConstantU32(state, 5)));
			    const auto float_value = Select(
			        state, u32, width_is(32u), raw,
			        Select(state, u32, width_is(16u),
			               EmitBitcastF32ToU32(state, EmitF16BitsToF32(state, raw)), small_float));
			    auto value = raw; // Uint
			    value      = Select(state, u32, is_sint, signed_raw, value);
			    value      = Select(state, u32, is_unorm, unorm, value);
			    value      = Select(state, u32, is_snorm, to_bits(snorm_clamped), value);
			    value      = Select(state, u32, is_uscaled, to_bits(unsigned_float), value);
			    value      = Select(state, u32, is_sscaled, to_bits(signed_float), value);
			    value      = Select(state, u32, is_float, float_value, value);
			    return Select(state, u32, in_bounds, value, ConstantU32(state, 0));
		    });
	}
	return values;
}

// One result component of a formatted load through a GPU-selected V# (BUFFER_LOAD_FORMAT_X).
uint32_t LoadIndirectFormattedTable(ValueEmitContext& ctx, const IR::Inst& inst) {
	return EmitValueOrZeroIfCondition(ctx.state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		return LoadIndirectFormattedComponents(ctx, inst, 1)[0];
	});
}

bool IndirectFormatSwitch() {
	static const bool value = [] {
		const char* text = std::getenv("KYTY_INDIRECT_FORMAT_SWITCH");
		return text != nullptr && text[0] == '1';
	}();
	return value;
}

uint32_t LoadBuffer(ValueEmitContext& ctx, const IR::Inst& inst, uint32_t components) {
	auto& state = ctx.state;
	return EmitValueOrDefaultIfCondition(
	    state, ctx.Arg(inst, inst.NumArgs() - 1), TypeU32Composite(state, components),
	    ConstantU32CompositeZero(state, components), [&]() {
		    const auto mem = ctx.Memory(inst);
		    if (mem.kind == IR::ResourceKind::IndirectBuffer) {
			    if (mem.formatted) {
				    // BUFFER_LOAD_FORMAT_XY/XYZ/XYZW (GT7's material vertex fetches).
				    // (Guarded already: the components share the load's condition.)
				    std::array<uint32_t, 4> values {};
				    if (IndirectFormatSwitch()) {
					    for (uint32_t component = 0; component < components; ++component) {
						    values[component] = LoadIndirectFormattedX(ctx, inst, component);
					    }
				    } else {
					    values = LoadIndirectFormattedComponents(ctx, inst, components);
				    }
				    return components == 1u ? values[0]
				                            : ConstructU32Composite(state, components, values);
			    }
			    return LoadIndirectBuffer(ctx, inst, components);
		    }
		    const auto resource = PrepareMemoryResourceAccess(state, mem);
		    const auto info     = Format::GetFormatInfo(
		        mem.formatted ? BufferFormat(ctx, mem) : Prospero::BufferFormat::kInvalid);
		    if (info.type != Format::ComponentType::Unknown) {
			    const auto plan = PrepareFormattedMemory(ctx, inst, mem, resource, info);
			    return EmitValueOrDefaultIfCondition(
			        state, plan.in_bounds, TypeU32Composite(state, components),
			        FormattedOutOfBoundsValue(ctx, mem, plan, components), [&]() {
				        std::array<uint32_t, 4> values {};
				        for (uint32_t component = 0; component < components; component++) {
					        values[component] = LoadFormattedInBounds(ctx, mem, plan, component);
				        }
				        return ConstructU32Composite(state, components, values);
			        });
		    }
		    std::array<uint32_t, 4> values {};
		    for (uint32_t component = 0; component < components; component++) {
			    values[component] =
			        LoadWordPrepared(ctx, inst, RebaseRawComponent(mem, component), resource);
		    }
		    return ConstructU32Composite(state, components, values);
	    });
}

void StoreWideBuffer(ValueEmitContext& ctx, const IR::Inst& inst, uint32_t components) {
	auto& state = ctx.state;
	EmitIfCondition(state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		const auto mem       = ctx.Memory(inst);
		const auto resource  = PrepareMemoryResourceAccess(state, mem);
		const auto composite = ctx.Arg(inst, inst.NumArgs() - 2);
		const auto info = Format::GetFormatInfo(mem.formatted ? BufferFormat(ctx, mem)
		                                                      : Prospero::BufferFormat::kInvalid);
		if (info.type != Format::ComponentType::Unknown) {
			StoreFormattedPrepared(ctx, inst, mem, resource, info, composite, components);
			return;
		}
		for (uint32_t component = 0; component < components; component++) {
			const auto data = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(state), data, composite,
			                          component);
			StoreWordPrepared(ctx, inst, RebaseRawComponent(mem, component), resource, data);
		}
	});
}

uint32_t LoadWideShared(ValueEmitContext& ctx, const IR::Inst& inst, uint32_t components) {
	auto& state = ctx.state;
	return EmitValueOrDefaultIfCondition(
	    state, ctx.Arg(inst, inst.NumArgs() - 1), TypeU32Composite(state, components),
	    ConstantU32CompositeZero(state, components), [&]() {
		    const auto              mem      = ctx.Memory(inst);
		    const auto              resource = PrepareMemoryResourceAccess(state, mem);
		    const auto              base     = ByteAddress(ctx, inst, mem);
		    std::array<uint32_t, 4> values {};
		    for (uint32_t component = 0; component < components; component++) {
			    const auto address   = component == 0u
			                               ? base
			                               : Binary(state, spv::OpIAdd, TypeU32(state), base,
			                                        ConstantU32(state, component * 4u));
			    const auto index = Binary(state, spv::OpShiftRightLogical, TypeU32(state),
			                                  address, ConstantU32(state, 2));
			    values[component]    = EmitValueOrZeroIfCondition(
			        state, EmitMemoryElementInBounds(state, resource, index),
			        [&]() { return LoadWordInBounds(ctx, resource, index); });
		    }
		    return ConstructU32Composite(state, components, values);
	    });
}

void StoreWideShared(ValueEmitContext& ctx, const IR::Inst& inst, uint32_t components) {
	auto& state = ctx.state;
	EmitIfCondition(state, ctx.Arg(inst, inst.NumArgs() - 1), [&]() {
		const auto mem      = ctx.Memory(inst);
		const auto resource = PrepareMemoryResourceAccess(state, mem);
		const auto base     = ByteAddress(ctx, inst, mem);
		for (uint32_t component = 0; component < components; component++) {
			const auto address = component == 0u ? base
			                                     : Binary(state, spv::OpIAdd, TypeU32(state), base,
			                                              ConstantU32(state, component * 4u));
			const auto index = Binary(state, spv::OpShiftRightLogical, TypeU32(state), address,
			                              ConstantU32(state, 2));
			EmitIfCondition(state, EmitMemoryElementInBounds(state, resource, index),
			                [&]() {
				                StoreWordInBounds(ctx, resource, index, ctx.Arg(inst, component + 1u));
			                });
		}
	});
}

} // namespace

uint32_t ConstantDeviceAddress(EmitterState& state, uint64_t value) {
	return state.builder.Constant(spv::OpConstant, TypeScalarU64(state),
	                              static_cast<uint32_t>(value),
	                              static_cast<uint32_t>(value >> 32u));
}

uint32_t DeviceAddressFromWords(EmitterState& state, uint32_t low, uint32_t high) {
	const auto low64  = Unary(state, spv::OpUConvert, TypeScalarU64(state), low);
	const auto high64 = Binary(state, spv::OpShiftLeftLogical, TypeScalarU64(state),
	                           Unary(state, spv::OpUConvert, TypeScalarU64(state), high),
	                           ConstantDeviceAddress(state, 32));
	return Binary(state, spv::OpBitwiseOr, TypeScalarU64(state), low64, high64);
}

uint32_t GetBdaPointer(EmitterState& state, uint32_t address) {
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpFunctionCall, TypeScalarU64(state), result,
	                          state.bda_pointer_function, address);
	return result;
}

// A shader-call variant outside the expansion (ShaderCallMissTrapCode): the target and the
// two data words join the fault buffer's miss set (ShaderCallMissSlot), from which the host
// feeds the next expansion. The lanes then go on as if the callee did nothing (BuildExpansion in
// ShaderFunctions.cpp). The pc stays in the IR for diagnostics.
void EmitShaderTrap(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto&      state       = ctx.state;
	const auto target_low  = ctx.Arg(inst, 2);
	const auto target_high = ctx.Arg(inst, 3);
	const auto data0       = ctx.Arg(inst, 4);
	const auto data1       = ctx.Arg(inst, 5);
	// Round 99: a partitioned expansion's miss (ShaderCallMissDropFlag in the code) is keyed by
	// its target alone. Its records' data is left to the shader (runtime records), and its
	// dispatches report the other partitions' targets over and over: with the data in the key,
	// each record of a material table (1,600 in a race) took a slot of its own every frame, and a
	// target no partition has yet found its slots taken.
	const auto code = inst.Arg(1).Resolve();
	const bool drop = code.IsImmediate() && (code.U32() & Decoder::ShaderCallMissDropFlag) != 0u;
	constexpr auto record_word = BufferCachePages::kNumPages / 8 / sizeof(uint32_t);
	constexpr auto slots_word  = record_word + offsetof(ShaderTrapArea, misses) / sizeof(uint32_t);
	constexpr auto slot_words  = sizeof(ShaderCallMissSlot) / sizeof(uint32_t);
	static_assert((kShaderCallMissSlots & (kShaderCallMissSlots - 1u)) == 0);
	const auto u32 = TypeU32(state);
	const auto mix = [&](uint32_t value, uint32_t multiplier) {
		return Binary(state, spv::OpIMul, u32, value, ConstantU32(state, multiplier));
	};
	const auto xor_ = [&](uint32_t a, uint32_t b) {
		return Binary(state, spv::OpBitwiseXor, u32, a, b);
	};
	// The shader is part of the key: material passes that call the same material function each
	// report it. With the target and data alone, the pass that ran first in a frame took the
	// slot, and every later one found its key there, "already reported": they never learned the
	// function while the first trapped on it too, and their lanes that call it stayed on the
	// miss path (unshaded reflections in the ray tracing mode).
	const auto shader_word =
	    static_cast<uint32_t>(state.program.shader_hash ^ (state.program.shader_hash >> 32u));
	const auto target_key = xor_(target_low, mix(target_high, 0x85ebca77u));
	const auto data_key =
	    drop ? target_key
	         : xor_(xor_(target_key, mix(data0, 0xc2b2ae3du)), mix(data1, 0x27d4eb2fu));
	const auto hashed =
	    mix(xor_(data_key, ConstantU32(state, shader_word * 0x165667b1u)), 0x9e3779b1u);
	const auto key = Binary(state, spv::OpBitwiseOr, u32, hashed, ConstantU32(state, 1));
	const auto first = Binary(
	    state, spv::OpShiftRightLogical, u32, hashed,
	    ConstantU32(state, 32u - static_cast<uint32_t>(std::countr_zero(kShaderCallMissSlots))));
	const auto probe = [&](auto& self, uint32_t attempt) -> void {
		const auto slot = Binary(state, spv::OpBitwiseAnd, u32,
		                         Binary(state, spv::OpIAdd, u32, first, ConstantU32(state, attempt)),
		                         ConstantU32(state, kShaderCallMissSlots - 1u));
		const auto base = Binary(state, spv::OpIAdd, u32, ConstantU32(state, slots_word),
		                         Binary(state, spv::OpIMul, u32, slot, ConstantU32(state, slot_words)));
		const auto word = [&](size_t offset) {
			return FaultElementPointer(
			    state, Binary(state, spv::OpIAdd, u32, base,
			                  ConstantU32(state, static_cast<uint32_t>(offset / sizeof(uint32_t)))));
		};
		// Round 99: a plain (atomic) load first: a key already there was reported in this batch,
		// and a partitioned shader's dispatches trap on the other partitions' targets over and
		// over (their reports change nothing), each trap otherwise a device-wide exchange.
		const auto existing = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpAtomicLoad, u32, existing,
		                          word(offsetof(ShaderCallMissSlot, key)),
		                          ConstantU32(state, spv::ScopeDevice), ConstantU32(state, 0));
		EmitIfCondition(
		    state, Binary(state, spv::OpINotEqual, TypeBool(state), existing, key), [&]() {
			    const auto old = state.builder.AllocateId();
			    state.builder.AddFunction(
			        spv::OpAtomicCompareExchange, u32, old, word(offsetof(ShaderCallMissSlot, key)),
			        ConstantU32(state, spv::ScopeDevice), ConstantU32(state, 0),
			        ConstantU32(state, 0), key, ConstantU32(state, 0));
			    const auto won =
			        Binary(state, spv::OpIEqual, TypeBool(state), old, ConstantU32(state, 0));
			    EmitIfCondition(state, won, [&]() {
				    const auto store = [&](size_t offset, uint32_t value) {
					    state.builder.AddFunction(spv::OpStore, word(offset), value);
				    };
				    store(offsetof(ShaderCallMissSlot, target_low), target_low);
				    store(offsetof(ShaderCallMissSlot, target_high), target_high);
				    store(offsetof(ShaderCallMissSlot, data0), data0);
				    store(offsetof(ShaderCallMissSlot, data1), data1);
				    store(offsetof(ShaderCallMissSlot, shader_hash_low),
				          ConstantU32(state, static_cast<uint32_t>(state.program.shader_hash)));
				    store(
				        offsetof(ShaderCallMissSlot, shader_hash_high),
				        ConstantU32(state, static_cast<uint32_t>(state.program.shader_hash >> 32)));
			    });
			    if (attempt + 1u < kShaderCallMissProbes) {
				    // Taken by another variant: try the next slot. (Equal: already reported.)
				    const auto taken =
				        Binary(state, spv::OpLogicalAnd, TypeBool(state),
				               Binary(state, spv::OpINotEqual, TypeBool(state), old,
				                      ConstantU32(state, 0)),
				               Binary(state, spv::OpINotEqual, TypeBool(state), old, key));
				    EmitIfCondition(state, taken, [&]() { self(self, attempt + 1u); });
			    }
		    });
	};
	if (state.program.stage == ShaderType::Compute) {
		// The trap's operands are scalars, the same in every lane: one lane of the subgroup reports
		// (every lane of a material pass contending for one slot cost a device-wide atomic each).
		// Not in pixel shaders, where the elected lane may be a helper, whose stores are dropped.
		state.builder.RequireCapability(spv::CapabilityGroupNonUniform);
		const auto elected = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpGroupNonUniformElect, TypeBool(state), elected,
		                          ConstantU32(state, spv::ScopeSubgroup));
		EmitIfCondition(state, elected, [&]() { probe(probe, 0); });
		return;
	}
	probe(probe, 0);
}

// Diagnostics (KYTY_PROBE): an active lane counts its hit in its point's probe_count and, for the
// hits that pick a record (ShaderProbeRecord), stores the shader, pc and values there. Each
// emulated wave half records its own lanes.
void EmitShaderProbe(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto&          state       = ctx.state;
	constexpr auto record_word = BufferCachePages::kNumPages / 8 / sizeof(uint32_t);
	constexpr auto count_word =
	    record_word + offsetof(ShaderTrapArea, probe_count) / sizeof(uint32_t);
	constexpr auto probes_word = record_word + offsetof(ShaderTrapArea, probes) / sizeof(uint32_t);
	constexpr auto slot_words  = sizeof(ShaderProbeRecord) / sizeof(uint32_t);
	// The power-of-4 hits after the early ones: 4^1 takes the first record after them.
	constexpr uint32_t last_power = kShaderProbeSlotsPerPoint - kShaderProbeEarly;
	static_assert(kShaderProbeEarly == 4u && last_power <= 15u);
	const auto pc    = inst.Arg(0).Resolve();
	const auto point = inst.Arg(2).Resolve();
	const auto count = inst.Arg(3).Resolve();
	EXIT_IF(!pc.IsImmediate() || !point.IsImmediate() || !count.IsImmediate());
	EXIT_IF(point.U32() >= kShaderProbePoints);
	const auto values  = std::min(count.U32(), kShaderProbeValues);
	const auto u32     = TypeU32(state);
	const auto boolean = TypeBool(state);
	EmitIfCondition(state, ctx.Arg(inst, 1), [&]() {
		const auto hit = state.builder.AllocateId();
		state.builder.AddFunction(
		    spv::OpAtomicIAdd, u32, hit,
		    FaultElementPointer(
		        state, ConstantU32(state, static_cast<uint32_t>(count_word) + point.U32())),
		    ConstantU32(state, spv::ScopeDevice), ConstantU32(state, 0), ConstantU32(state, 1));
		const auto early =
		    Binary(state, spv::OpULessThan, boolean, hit, ConstantU32(state, kShaderProbeEarly));
		const auto before = Binary(state, spv::OpISub, u32, hit, ConstantU32(state, 1));
		// A power of 4: one bit, at an even position. Its log2 is the bit count of the number
		// before it.
		const auto power = Binary(
		    state, spv::OpLogicalAnd, boolean,
		    Binary(state, spv::OpIEqual, boolean,
		           Binary(state, spv::OpBitwiseAnd, u32, hit, before), ConstantU32(state, 0)),
		    Binary(state, spv::OpINotEqual, boolean,
		           Binary(state, spv::OpBitwiseAnd, u32, hit, ConstantU32(state, 0x55555555u)),
		           ConstantU32(state, 0)));
		const auto exponent =
		    Binary(state, spv::OpShiftRightLogical, u32, Unary(state, spv::OpBitCount, u32, before),
		           ConstantU32(state, 1));
		const auto late   = Binary(state, spv::OpLogicalAnd, boolean, power,
		                           Binary(state, spv::OpULessThanEqual, boolean, exponent,
		                                  ConstantU32(state, last_power)));
		const auto record = Binary(state, spv::OpLogicalOr, boolean, early, late);
		EmitIfCondition(state, record, [&]() {
			const auto slot = Binary(state, spv::OpIAdd, u32,
			                         ConstantU32(state, point.U32() * kShaderProbeSlotsPerPoint),
			                         Select(state, u32, early, hit,
			                                Binary(state, spv::OpIAdd, u32, exponent,
			                                       ConstantU32(state, kShaderProbeEarly - 1u))));
			const auto base =
			    Binary(state, spv::OpIAdd, u32, ConstantU32(state, probes_word),
			           Binary(state, spv::OpIMul, u32, slot, ConstantU32(state, slot_words)));
			const auto store = [&](size_t word, uint32_t value) {
				state.builder.AddFunction(
				    spv::OpStore,
				    FaultElementPointer(state,
				                        Binary(state, spv::OpIAdd, u32, base,
				                               ConstantU32(state, static_cast<uint32_t>(word)))),
				    value);
			};
			constexpr auto word = [](size_t offset) { return offset / sizeof(uint32_t); };
			store(word(offsetof(ShaderProbeRecord, shader_hash_low)),
			      ConstantU32(state, static_cast<uint32_t>(state.program.shader_hash)));
			store(word(offsetof(ShaderProbeRecord, shader_hash_high)),
			      ConstantU32(state, static_cast<uint32_t>(state.program.shader_hash >> 32)));
			store(word(offsetof(ShaderProbeRecord, pc)), ConstantU32(state, pc.U32()));
			store(word(offsetof(ShaderProbeRecord, count)), ConstantU32(state, values));
			store(word(offsetof(ShaderProbeRecord, hit)), hit);
			for (uint32_t index = 0; index < values; index++) {
				store(word(offsetof(ShaderProbeRecord, values)) + index, ctx.Arg(inst, 4u + index));
			}
		});
	});
}

void DefineRayLoopRound(EmitterState& state);
void DefineStuckProbe(EmitterState& state);

// ray_loop_count(word, cap): one more in the given word of the shader's RayLoopSlot, which the
// first call claims by the shader's key (and stores the shader and the loop budget, cap, in).
// Returns the slot's first word in the fault buffer, or ~0 when the other shaders hold every slot.
void DefineRayLoopCount(EmitterState& state) {
	if (!state.requirements.ray_loop_rounds || state.fault_buffer_variable == 0) {
		return;
	}
	const auto     u32         = TypeU32(state);
	const auto     boolean     = TypeBool(state);
	constexpr auto record_word = BufferCachePages::kNumPages / 8 / sizeof(uint32_t);
	constexpr auto slots_word =
	    static_cast<uint32_t>(record_word + offsetof(ShaderTrapArea, ray_loops) / sizeof(uint32_t));
	constexpr auto slot_words = static_cast<uint32_t>(sizeof(RayLoopSlot) / sizeof(uint32_t));
	constexpr auto word_of    = [](size_t offset) {
        return static_cast<uint32_t>(offset / sizeof(uint32_t));
	};
	const auto hash               = state.program.shader_hash;
	const auto shader_word        = static_cast<uint32_t>(hash ^ (hash >> 32u));
	const auto key                = ConstantU32(state, (shader_word * 0x9e3779b1u) | 1u);
	const auto signature          = state.builder.Type(spv::OpTypeFunction, u32, u32, u32);
	state.ray_loop_count_function = state.builder.AllocateId();
	state.builder.AddName(state.ray_loop_count_function, "ray_loop_count");
	state.builder.AddFunction(spv::OpFunction, u32, state.ray_loop_count_function,
	                          spv::FunctionControlMaskNone, signature);
	const auto word = state.builder.AllocateId();
	const auto cap  = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpFunctionParameter, u32, word);
	state.builder.AddFunction(spv::OpFunctionParameter, u32, cap);
	EmitLabel(state, state.builder.AllocateId());
	const auto element = [&](uint32_t base, uint32_t offset) {
		return FaultElementPointer(state, ConstantU32(state, base + offset));
	};
	// The slot at index, or the next ones when another shader holds it.
	const auto attempt = [&](auto& self, uint32_t index) -> uint32_t {
		const auto base        = slots_word + index * slot_words;
		const auto key_pointer = element(base, word_of(offsetof(RayLoopSlot, key)));
		const auto existing    = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpAtomicLoad, u32, existing, key_pointer,
		                          ConstantU32(state, spv::ScopeDevice), ConstantU32(state, 0));
		const auto mine = EmitValueOrDefaultIfCondition(
		    state, Binary(state, spv::OpINotEqual, boolean, existing, key), boolean,
		    ConstantBool(state, true), [&]() {
			    const auto old = state.builder.AllocateId();
			    state.builder.AddFunction(spv::OpAtomicCompareExchange, u32, old, key_pointer,
			                              ConstantU32(state, spv::ScopeDevice),
			                              ConstantU32(state, 0), ConstantU32(state, 0), key,
			                              ConstantU32(state, 0));
			    const auto won = Binary(state, spv::OpIEqual, boolean, old, ConstantU32(state, 0));
			    EmitIfCondition(state, won, [&]() {
				    state.builder.AddFunction(
				        spv::OpStore,
				        element(base, word_of(offsetof(RayLoopSlot, shader_hash_low))),
				        ConstantU32(state, static_cast<uint32_t>(hash)));
				    state.builder.AddFunction(
				        spv::OpStore,
				        element(base, word_of(offsetof(RayLoopSlot, shader_hash_high))),
				        ConstantU32(state, static_cast<uint32_t>(hash >> 32u)));
				    state.builder.AddFunction(
				        spv::OpStore, element(base, word_of(offsetof(RayLoopSlot, cap))), cap);
			    });
			    return Binary(state, spv::OpLogicalOr, boolean, won,
			                  Binary(state, spv::OpIEqual, boolean, old, key));
		    });
		if (index + 1u == kRayLoopSlots) {
			return Select(state, u32, mine, ConstantU32(state, base), ConstantU32(state, ~0u));
		}
		return EmitValueOrDefaultIfCondition(state, Unary(state, spv::OpLogicalNot, boolean, mine),
		                                     u32, ConstantU32(state, base),
		                                     [&]() { return self(self, index + 1u); });
	};
	const auto base = attempt(attempt, 0);
	EmitIfCondition(
	    state, Binary(state, spv::OpINotEqual, boolean, base, ConstantU32(state, ~0u)), [&]() {
		    const auto count = state.builder.AllocateId();
		    state.builder.AddFunction(
		        spv::OpAtomicIAdd, u32, count,
		        FaultElementPointer(state, Binary(state, spv::OpIAdd, u32, base, word)),
		        ConstantU32(state, spv::ScopeDevice), ConstantU32(state, 0), ConstantU32(state, 1));
	    });
	state.builder.AddFunction(spv::OpReturnValue, base);
	state.builder.AddFunction(spv::OpFunctionEnd);
	DefineRayLoopRound(state);
	DefineStuckProbe(state);
}

// ray_loop_round(rounds, used, left, next, row, pc, cap): the wave's counts for one round of a
// loop (EmitRayLoopRounds): rounds in the row's loop and in all loops (used), the tick's result
// (left) and what is left after it (next). A function, so the loop header that calls it stays one
// block (its branches would split the header from its OpLoopMerge).
void DefineRayLoopRound(EmitterState& state) {
	const auto u32       = TypeU32(state);
	const auto boolean   = TypeBool(state);
	const auto signature = state.builder.Type(spv::OpTypeFunction, TypeVoid(state), u32, u32,
	                                          boolean, u32, u32, u32, u32);
	state.ray_loop_round_function = state.builder.AllocateId();
	state.builder.AddName(state.ray_loop_round_function, "ray_loop_round");
	state.builder.AddFunction(spv::OpFunction, TypeVoid(state), state.ray_loop_round_function,
	                          spv::FunctionControlMaskNone, signature);
	std::array<uint32_t, 7> params {};
	const std::array        types {u32, u32, boolean, u32, u32, u32, u32};
	for (size_t index = 0; index < params.size(); index++) {
		params[index] = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpFunctionParameter, types[index], params[index]);
	}
	const auto [rounds, used, left, next, row, pc, cap] = params;
	EmitLabel(state, state.builder.AllocateId());
	// A bucket's count: 1 (bucket 0), or 2^9 to 2^20 (buckets 1 to 12).
	constexpr uint32_t first   = 1u << kRayLoopFirstPowerBucket;
	constexpr uint32_t last    = first << (kRayLoopBuckets - 2u);
	const auto         reaches = [&](uint32_t value) {
        const auto one = Binary(state, spv::OpIEqual, boolean, value, ConstantU32(state, 1));
        const auto power =
            Binary(state, spv::OpIEqual, boolean,
		                   Binary(state, spv::OpBitwiseAnd, u32, value,
		                          Binary(state, spv::OpISub, u32, value, ConstantU32(state, 1))),
		                   ConstantU32(state, 0));
        const auto in_range =
            Binary(state, spv::OpULessThanEqual, boolean,
		                   Binary(state, spv::OpISub, u32, value, ConstantU32(state, first)),
		                   ConstantU32(state, last - first));
        return Binary(state, spv::OpLogicalAnd, boolean, left,
		                      Binary(state, spv::OpLogicalOr, boolean, one,
		                             Binary(state, spv::OpLogicalAnd, boolean, power, in_range)));
	};
	const auto bucket = [&](uint32_t value) {
		// value - 1 of 2^k has k bits set: bucket k - 8.
		const auto bits = Unary(state, spv::OpBitCount, u32,
		                        Binary(state, spv::OpISub, u32, value, ConstantU32(state, 1)));
		return Select(state, u32,
		              Binary(state, spv::OpIEqual, boolean, value, ConstantU32(state, 1)),
		              ConstantU32(state, 0),
		              Binary(state, spv::OpISub, u32, bits,
		                     ConstantU32(state, kRayLoopFirstPowerBucket - 1u)));
	};
	const auto     row_event = reaches(rounds);
	const auto     all_event = reaches(used);
	const auto     spent     = Binary(state, spv::OpLogicalAnd, boolean, left,
	                                  Binary(state, spv::OpIEqual, boolean, next, ConstantU32(state, 0)));
	const auto     any       = Binary(state, spv::OpLogicalOr, boolean,
	                                  Binary(state, spv::OpLogicalOr, boolean, row_event, all_event), spent);
	constexpr auto word_of   = [](size_t offset) {
        return static_cast<uint32_t>(offset / sizeof(uint32_t));
	};
	const auto add = [&](uint32_t a, uint32_t b) { return Binary(state, spv::OpIAdd, u32, a, b); };
	const auto count = [&](uint32_t word) {
		const auto base = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpFunctionCall, u32, base, state.ray_loop_count_function,
		                          word, cap);
		return base;
	};
	EmitIfCondition(state, any, [&]() {
		// The loops' branches are the wave's: every invocation of the host subgroup is here, and
		// one counts the wave.
		state.builder.RequireCapability(spv::CapabilityGroupNonUniform);
		const auto elected = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpGroupNonUniformElect, boolean, elected,
		                          ConstantU32(state, spv::ScopeSubgroup));
		EmitIfCondition(state, elected, [&]() {
			const auto waves = ConstantU32(state, word_of(offsetof(RayLoopSlot, waves)));
			EmitIfCondition(state, row_event, [&]() {
				const auto row_first =
				    Binary(state, spv::OpIMul, u32, row, ConstantU32(state, kRayLoopBuckets));
				const auto base = count(add(add(waves, row_first), bucket(rounds)));
				const auto first_round =
				    Binary(state, spv::OpLogicalAnd, boolean,
				           Binary(state, spv::OpIEqual, boolean, rounds, ConstantU32(state, 1)),
				           Binary(state, spv::OpINotEqual, boolean, base, ConstantU32(state, ~0u)));
				EmitIfCondition(state, first_round, [&]() {
					const auto at =
					    add(add(base, ConstantU32(state, word_of(offsetof(RayLoopSlot, pc)))), row);
					state.builder.AddFunction(spv::OpStore, FaultElementPointer(state, at),
					                          add(pc, ConstantU32(state, 1)));
				});
			});
			EmitIfCondition(state, all_event, [&]() {
				(void)count(add(add(waves, ConstantU32(state, kRayLoopRows * kRayLoopBuckets)),
				                bucket(used)));
			});
			EmitIfCondition(state, spent, [&]() {
				(void)count(add(ConstantU32(state, word_of(offsetof(RayLoopSlot, spent))), row));
			});
		});
	});
	state.builder.AddFunction(spv::OpReturn);
	state.builder.AddFunction(spv::OpFunctionEnd);
}

// ray_loop_stuck(rounds, label, point, count, values...): when the wave's rounds in the loop
// are kStuckProbeRounds, one lane of it takes the next StuckProbeSample and records the values
// there (KYTY_PROBE <hash>@stuck). A function for the same reason as ray_loop_round.
void DefineStuckProbe(EmitterState& state) {
	if (!state.requirements.stuck_probe) {
		return;
	}
	const auto                   u32     = TypeU32(state);
	const auto                   boolean = TypeBool(state);
	constexpr uint32_t           params  = 4 + kShaderProbeValues;
	std::array<uint32_t, params> types {};
	std::array<uint32_t, params> args {};
	types.fill(u32);
	const auto signature =
	    state.builder.Type(spv::OpTypeFunction, TypeVoid(state), std::span<const uint32_t>(types));
	state.stuck_probe_function = state.builder.AllocateId();
	state.builder.AddName(state.stuck_probe_function, "ray_loop_stuck");
	state.builder.AddFunction(spv::OpFunction, TypeVoid(state), state.stuck_probe_function,
	                          spv::FunctionControlMaskNone, signature);
	for (auto& arg: args) {
		arg = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpFunctionParameter, u32, arg);
	}
	EmitLabel(state, state.builder.AllocateId());
	constexpr auto record_word = BufferCachePages::kNumPages / 8 / sizeof(uint32_t);
	constexpr auto count_word  = static_cast<uint32_t>(
        record_word + offsetof(ShaderTrapArea, stuck_count) / sizeof(uint32_t));
	constexpr auto samples_word =
	    static_cast<uint32_t>(record_word + offsetof(ShaderTrapArea, stuck) / sizeof(uint32_t));
	constexpr auto sample_words =
	    static_cast<uint32_t>(sizeof(StuckProbeSample) / sizeof(uint32_t));
	constexpr auto word_of = [](size_t offset) {
		return static_cast<uint32_t>(offset / sizeof(uint32_t));
	};
	const auto reached =
	    Binary(state, spv::OpIEqual, boolean, args[0], ConstantU32(state, kStuckProbeRounds));
	EmitIfCondition(state, reached, [&]() {
		// As in ray_loop_round: every invocation of the wave's host subgroup is here.
		state.builder.RequireCapability(spv::CapabilityGroupNonUniform);
		const auto elected = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpGroupNonUniformElect, boolean, elected,
		                          ConstantU32(state, spv::ScopeSubgroup));
		EmitIfCondition(state, elected, [&]() {
			const auto index = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpAtomicIAdd, u32, index,
			                          FaultElementPointer(state, ConstantU32(state, count_word)),
			                          ConstantU32(state, spv::ScopeDevice), ConstantU32(state, 0),
			                          ConstantU32(state, 1));
			const auto free = Binary(state, spv::OpULessThan, boolean, index,
			                         ConstantU32(state, kStuckProbeSamples));
			EmitIfCondition(state, free, [&]() {
				const auto base = Binary(
				    state, spv::OpIAdd, u32, ConstantU32(state, samples_word),
				    Binary(state, spv::OpIMul, u32, index, ConstantU32(state, sample_words)));
				const auto store = [&](uint32_t word, uint32_t value) {
					state.builder.AddFunction(
					    spv::OpStore,
					    FaultElementPointer(
					        state, Binary(state, spv::OpIAdd, u32, base, ConstantU32(state, word))),
					    value);
				};
				const auto hash = state.program.shader_hash;
				store(word_of(offsetof(StuckProbeSample, shader_hash_low)),
				      ConstantU32(state, static_cast<uint32_t>(hash)));
				store(word_of(offsetof(StuckProbeSample, shader_hash_high)),
				      ConstantU32(state, static_cast<uint32_t>(hash >> 32u)));
				store(word_of(offsetof(StuckProbeSample, loop_pc)), args[1]);
				store(word_of(offsetof(StuckProbeSample, point)), args[2]);
				store(word_of(offsetof(StuckProbeSample, count)), args[3]);
				for (uint32_t value = 0; value < kShaderProbeValues; value++) {
					store(word_of(offsetof(StuckProbeSample, values)) + value, args[4 + value]);
				}
			});
		});
	});
	state.builder.AddFunction(spv::OpReturn);
	state.builder.AddFunction(spv::OpFunctionEnd);
}

void EmitShaderStuckProbe(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto& state = ctx.state;
	// The scalars are the same in both emulated wave halves; the first half's lanes record.
	if (ctx.half != 0 || state.stuck_probe_function == 0) {
		return;
	}
	const auto* tick = inst.Arg(0).ResolveInstruction();
	const auto  row  = std::ranges::find(state.ray_loop_ticks, static_cast<const void*>(tick));
	if (tick == nullptr || row == state.ray_loop_ticks.end()) {
		return;
	}
	const auto index    = std::min<size_t>(static_cast<size_t>(row - state.ray_loop_ticks.begin()),
	                                       kRayLoopRows - 1u);
	const auto variable = state.ray_loop_round_variables[index];
	if (variable == 0) {
		return;
	}
	const auto rounds = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpLoad, TypeU32(state), rounds, variable);
	std::vector<uint32_t> operands {rounds};
	for (uint32_t arg = 1; arg < 4u + kShaderProbeValues; arg++) {
		operands.push_back(arg < 4u ? ConstantU32(state, inst.Arg(arg).Resolve().U32())
		                            : ctx.Arg(inst, arg));
	}
	const auto call = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpFunctionCall, TypeVoid(state), call,
	                          state.stuck_probe_function, std::span<const uint32_t>(operands));
}

void EmitRayLoopRounds(EmitterState& state, const void* tick, uint32_t cap, uint32_t pc,
                       uint32_t left, uint32_t next) {
	static_assert(std::tuple_size_v<decltype(state.ray_loop_round_variables)> == kRayLoopRows);
	if (state.ray_loop_round_function == 0) {
		return;
	}
	const auto u32 = TypeU32(state);
	// The loop's row: the order the emission met its header's tick in (the last row takes the
	// rest).
	auto row = static_cast<uint32_t>(std::ranges::find(state.ray_loop_ticks, tick) -
	                                 state.ray_loop_ticks.begin());
	if (row == state.ray_loop_ticks.size()) {
		state.ray_loop_ticks.push_back(tick);
	}
	row            = std::min(row, kRayLoopRows - 1u);
	auto& variable = state.ray_loop_round_variables[row];
	if (variable == 0) {
		variable =
		    state.builder.DefineGlobalVariable(TypePointer(state, spv::StorageClassPrivate, u32),
		                                       spv::StorageClassPrivate, ConstantU32(state, 0));
		state.builder.AddName(variable, "ray_loop_rounds");
	}
	// Straight-line code only: this is a loop header's block (DefineRayLoopRound).
	const auto before = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpLoad, u32, before, variable);
	const auto rounds = Select(
	    state, u32, left, Binary(state, spv::OpIAdd, u32, before, ConstantU32(state, 1)), before);
	state.builder.AddFunction(spv::OpStore, variable, rounds);
	// The rounds of all the invocation's loops, this one included.
	const auto used = Binary(state, spv::OpISub, u32, ConstantU32(state, cap), next);
	const auto call = state.builder.AllocateId();
	state.builder.AddFunction(
	    spv::OpFunctionCall, TypeVoid(state), call, state.ray_loop_round_function, rounds, used,
	    left, next, ConstantU32(state, row), ConstantU32(state, pc), ConstantU32(state, cap));
}

void DefineGetBdaPointer(EmitterState& state) {
	if (!state.program.info.uses_dma) {
		return;
	}
	const auto type            = TypeScalarU64(state);
	const auto function_type   = state.builder.Type(spv::OpTypeFunction, type, type);
	state.bda_pointer_function = state.builder.AllocateId();
	const auto address         = state.builder.AllocateId();
	const auto entry_label     = state.builder.AllocateId();
	state.builder.AddName(state.bda_pointer_function, "get_bda_pointer");
	state.builder.AddFunction(spv::OpFunction, type, state.bda_pointer_function,
	                          spv::FunctionControlMaskNone, function_type);
	state.builder.AddFunction(spv::OpFunctionParameter, type, address);
	EmitLabel(state, entry_label);

	// Only the guest's two GPU-visible windows have pages: the lower 1 TiB and the extended
	// memory window. Any other address (a pointer read from data the emulator does not reproduce)
	// has no page-table entry: packed, its page lies past the table (or wraps into it once
	// truncated), so the table load and the fault record would reach past their buffers, which
	// faults the device. Such an address reads as missing and records no fault.
	const auto lower = Binary(state, spv::OpULessThan, TypeBool(state), address,
	                          ConstantDeviceAddress(state, BufferCachePages::kLowerAddressSize));
	const auto into_ext =
	    Binary(state, spv::OpISub, type, address,
	           ConstantDeviceAddress(state, BufferCachePages::kExtendedMemoryBase));
	const auto extended =
	    Binary(state, spv::OpULessThan, TypeBool(state), into_ext,
	           ConstantDeviceAddress(state, BufferCachePages::kExtendedMemorySize));
	const auto in_range = Binary(state, spv::OpLogicalOr, TypeBool(state), lower, extended);
	const auto packed =
	    Select(state, type, extended,
	           Binary(state, spv::OpIAdd, type, into_ext,
	                  ConstantDeviceAddress(state, BufferCachePages::kLowerAddressSize)),
	           address);
	const auto page64 = Binary(state, spv::OpShiftRightLogical, type, packed,
	                           ConstantDeviceAddress(state, BufferCachePages::kPageBits));
	const auto page =
	    Select(state, TypeU32(state), in_range,
	           Unary(state, spv::OpUConvert, TypeU32(state), page64), ConstantU32(state, 0));
	const auto entry_pointer = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpAccessChain, TypeStorageBufferElementPointer(state, 64),
	                          entry_pointer, state.bda_pagetable_variable, ConstantU32(state, 0),
	                          page);
	const auto base = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpLoad, type, base, entry_pointer);
	const auto missing = Binary(
	    state, spv::OpLogicalOr, TypeBool(state),
	    Unary(state, spv::OpLogicalNot, TypeBool(state), in_range),
	    Binary(state, spv::OpIEqual, TypeBool(state), base, ConstantDeviceAddress(state, 0)));
	const auto fault_label     = state.builder.AllocateId();
	const auto available_label = state.builder.AllocateId();
	const auto merge_label     = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpSelectionMerge, merge_label, spv::SelectionControlMaskNone);
	state.builder.AddFunction(spv::OpBranchConditional, missing, fault_label, available_label);

	EmitLabel(state, fault_label);
	EmitIfCondition(state, in_range, [&]() { RecordBdaFault(state, page); });
	const auto fault_exit = state.current_label;
	state.builder.AddFunction(spv::OpBranch, merge_label);

	EmitLabel(state, available_label);
	const auto offset    = Binary(state, spv::OpBitwiseAnd, type, address,
	                              ConstantDeviceAddress(state, BufferCachePages::kPageSize - 1));
	const auto available = Binary(state, spv::OpIAdd, type, base, offset);
	state.builder.AddFunction(spv::OpBranch, merge_label);

	EmitLabel(state, merge_label);
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpPhi, type, result, ConstantDeviceAddress(state, 0), fault_exit,
	                          available, available_label);
	state.builder.AddFunction(spv::OpReturnValue, result);
	state.builder.AddFunction(spv::OpFunctionEnd);
}

uint32_t EmitAtomic32(ValueEmitContext& ctx, const IR::Inst& inst) {
	const auto& mem = ctx.Memory(inst);
	return EmitAtomicAccess(ctx, inst, mem, [&](uint32_t pointer) {
		const auto scope =
		    mem.kind == IR::ResourceKind::Lds ? spv::ScopeWorkgroup : spv::ScopeDevice;
		const auto old = EmitAtomicOperation(ctx, inst, pointer, scope);
		EmitAtomicMemoryBarrier(ctx.state, mem.kind);
		return old;
	});
}

uint32_t EmitBufferAtomic64(ValueEmitContext& ctx, const IR::Inst& inst) {
	const auto& mem   = ctx.Memory(inst);
	auto&       state = ctx.state;
	return EmitValueOrDefaultIfCondition(
	    state, ctx.Arg(inst, inst.NumArgs() - 1), TypeU64(state), ConstantU64(state, 0), [&]() {
		    const auto resource = PrepareStorageBufferResourceAccess(
		        state, mem, state.storage_buffer_u64_variable, TypeStorageBufferPointer(state, 64), 64);
		    const auto byte_address = ByteAddress(ctx, inst, mem);
		    const auto index = Binary(state, spv::OpShiftRightLogical, TypeU32(state), byte_address,
		                              ConstantU32(state, 3u));
		    return EmitValueOrDefaultIfCondition(
		        state, EmitMemoryElementInBounds(state, resource, index), TypeU64(state),
		        ConstantU64(state, 0), [&]() {
			        const auto pointer = EmitStorageBufferElementPointer(
			            state, resource, index, TypeStorageBufferElementPointer(state, 64));
			        const auto old = EmitAtomicOperation(ctx, inst, pointer, spv::ScopeDevice);
			        EmitAtomicMemoryBarrier(state, mem.kind);
			        return old;
		        });
	    });
}

void EmitSharedAtomic64(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto& state = ctx.state;
	const auto& mem = ctx.Memory(inst);
	EnsureLdsStorage(state);
	EmitIfCondition(state, ctx.Arg(inst, 2), [&]() {
		const auto address = Binary(state, spv::OpBitwiseAnd, TypeU32(state),
		                            ByteAddress(ctx, inst, mem), ConstantU32(state, 0xfff8u));
		const auto index = Binary(state, spv::OpShiftRightLogical, TypeU32(state), address,
		                          ConstantU32(state, 3u));
		const auto in_bounds = Binary(state, spv::OpULessThan, TypeBool(state), index,
		                              ConstantU32(state, LdsDwordCount(state) / 2u));
		EmitIfCondition(state, in_bounds, [&]() {
			auto native_index = index;
			auto semantics = spv::MemorySemanticsWorkgroupMemoryMask;
			if (state.lds_storage_class == spv::StorageClassStorageBuffer) {
				native_index = EmitAddU32(state, index, EmitBinaryU32(
				    state, spv::OpShiftRightLogical, state.lds_base_dwords, ConstantU32(state, 1)));
				semantics = spv::MemorySemanticsUniformMemoryMask;
			}
			const auto pointer = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpAccessChain,
			                          TypePointer(state, state.lds_storage_class, TypeScalarU64(state)),
			                          pointer, state.lds_u64_variable, ConstantU32(state, 0), native_index);
			const auto value = Unary(state, spv::OpBitcast, TypeScalarU64(state), ctx.Arg(inst, 1));
			state.builder.AddFunction(
			    SpirvAtomicOpcode(inst.GetOpcode()), TypeScalarU64(state), state.builder.AllocateId(),
			    pointer, ConstantU32(state, spv::ScopeWorkgroup),
			    ConstantU32(state, spv::MemorySemanticsAcquireReleaseMask | semantics), value);
		});
	});
}

uint32_t EmitBufferFloatAtomic(ValueEmitContext& ctx, const IR::Inst& inst) {
	const auto& mem       = ctx.Memory(inst);
	const bool  max_value = inst.GetOpcode() == IR::ValueOpcode::BufferAtomicFMax32;
	return EmitAtomicUpdate(ctx, inst, mem,
	                        [max_value](EmitterState& state, uint32_t old, uint32_t value) {
		                        return EmitFloatAtomicReplacement(state, old, value, max_value);
	                        });
}

void EmitSharedFloatAtomic(ValueEmitContext& ctx, const IR::Inst& inst) {
	const auto& mem       = ctx.Memory(inst);
	const bool  max_value = inst.GetOpcode() == IR::ValueOpcode::SharedAtomicFMax32;
	EmitAtomicUpdate(ctx, inst, mem,
	                 [max_value](EmitterState& state, uint32_t old, uint32_t value) {
		                 return EmitDsFloatAtomicReplacement(state, old, value, max_value);
	                 });
}

uint32_t EmitAppendConsume(ValueEmitContext& ctx, const IR::Inst& inst) {
	const bool append = inst.GetOpcode() == IR::ValueOpcode::DataAppend;
	auto&      state  = ctx.state;
	if (ctx.half == 1) {
		return ctx.other_half->Def(IR::Value(const_cast<IR::Inst*>(&inst)));
	}
	const auto mem           = ctx.Memory(inst);
	const auto offset        = mem.offset & 0xfffcu;
	auto       address       = ConstantU32(state, offset);
	uint32_t   region_bounds = 0;
	if (mem.kind == IR::ResourceKind::Gds) {
		const auto m0 = ctx.Arg(inst, 0);
		const auto base =
		    Binary(state, spv::OpShiftRightLogical, TypeU32(state), m0, ConstantU32(state, 16));
		const auto size =
		    Binary(state, spv::OpBitwiseAnd, TypeU32(state), m0, ConstantU32(state, 0xffffu));
		address = Binary(state, spv::OpIAdd, TypeU32(state), base, address);
		// The entire M0 region must fit the PS5's 48 KiB GDS partition.
		region_bounds = AndCondition(
		    state, Binary(state, spv::OpULessThan, TypeBool(state),
		                  ConstantU32(state, offset + 3u), size),
		    Binary(state, spv::OpULessThanEqual, TypeBool(state),
		           Binary(state, spv::OpIAdd, TypeU32(state), base, size),
		           ConstantU32(state, 0xc000u)));
	}
	const auto index =
	    Binary(state, spv::OpShiftRightLogical, TypeU32(state), address, ConstantU32(state, 2));
	const auto access = PrepareMemoryResourceAccess(state, mem);
	const auto exec   = ctx.Arg(inst, 1);
	const auto ballot = ctx.Ballot(inst.Arg(1));
	const auto low    = state.builder.AllocateId();
	const auto high   = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(state), low, ballot, 0);
	state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(state), high, ballot, 1);
	const auto count = Binary(state, spv::OpIAdd, TypeU32(state),
	                          Unary(state, spv::OpBitCount, TypeU32(state), low),
	                          Unary(state, spv::OpBitCount, TypeU32(state), high));
	const auto first = ctx.FirstLane(ballot);
	const auto source_lane =
	    state.lane_count == 2
	        ? Binary(state, spv::OpBitwiseAnd, TypeU32(state), first, ConstantU32(state, 31))
	        : first;
	const auto is_first       = Binary(state, spv::OpIEqual, TypeBool(state),
	                                   EmitSubgroupLocalInvocationId(state), source_lane);
	auto bounds = EmitMemoryElementInBounds(state, access, index);
	if (mem.kind == IR::ResourceKind::Gds) {
		bounds = AndCondition(state, bounds, region_bounds);
	}
	const auto condition = AndCondition(
	    state, is_first,
	    AndCondition(state,
	                 state.lane_count == 2 ? Binary(state, spv::OpINotEqual, TypeBool(state), count,
	                                                ConstantU32(state, 0))
	                                       : exec,
	                 bounds));
	const auto atomic = EmitValueOrZeroIfCondition(state, condition, [&]() {
		const auto value = state.builder.AllocateId();
		state.builder.AddFunction(append ? spv::OpAtomicIAdd : spv::OpAtomicISub, TypeU32(state),
		                          value, EmitMemoryElementPointer(state, access, index),
		                          ConstantU32(state, mem.kind == IR::ResourceKind::Gds
		                                                 ? spv::ScopeDevice
		                                                 : spv::ScopeWorkgroup),
		                          ConstantU32(state, spv::MemorySemanticsMaskNone), count);
		return value;
	});
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpGroupNonUniformShuffle, TypeU32(state), result,
	                          ConstantU32(state, spv::ScopeSubgroup), atomic, source_lane);
	return result;
}

uint32_t EmitReadConst(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto& state = ctx.state;
	if (state.flattened_srt_variable == 0) {
		ctx.Fail(inst, "requires the flattened SRT descriptor");
	}
	const auto pointer = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpAccessChain, TypeStorageBufferElementPointer(state), pointer,
	                          state.flattened_srt_variable, ConstantU32(state, 0),
	                          ctx.Arg(inst, 1));
	return EmitNative<spv::OpLoad, IR::Type::U32>(state, pointer);
}

// A planning-only read emits no load. If a phi web with no other consumer still references it,
// the read gets an undefined value, so that the phis have one to name. (Other references, such
// as planning references, emit nothing and need none.)
static void DefinePlanningOnlyRead(ValueEmitContext& ctx, const IR::Inst& inst) {
	if (std::ranges::none_of(inst.Uses(), [](const IR::Use& use) {
		    return use.user->GetOpcode() == IR::ValueOpcode::Phi;
	    })) {
		return;
	}
	const auto undefined = ctx.state.builder.AllocateId();
	ctx.state.builder.AddFunction(spv::OpUndef, TypeId(ctx.state, inst.GetType()), undefined);
	ctx.Define(inst, undefined);
}

void EmitReadConstBuffer(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto mem = ctx.Memory(inst);
	if (mem.planning_only) {
		DefinePlanningOnlyRead(ctx, inst);
		return;
	}
	if (mem.kind == IR::ResourceKind::IndirectBuffer) {
		ctx.Define(inst, LoadIndirectScalarBuffer(ctx, inst));
		return;
	}
	auto& state        = ctx.state;
	mem.kind           = IR::ResourceKind::ScalarBuffer;
	const auto offset  = inst.Arg(1).Resolve();
	auto       index   = offset.IsImmediate()
	                         ? ConstantU32(state, (offset.U32() >> 2u) + (mem.offset >> 2u))
	                         : Binary(state, spv::OpShiftRightLogical, TypeU32(state), ctx.Arg(inst, 1),
	                                  ConstantU32(state, 2));
	if (!offset.IsImmediate() && mem.offset >= 4u) {
		index = Binary(state, spv::OpIAdd, TypeU32(state), index, ConstantU32(state, mem.offset >> 2u));
	}
	const auto access    = PrepareMemoryResourceAccess(state, mem);
	// Scalar buffer addressing aligns the base and each offset independently.
	const auto element = Binary(
	    state, spv::OpIAdd, TypeU32(state), index,
	    Binary(state, spv::OpShiftRightLogical, TypeU32(state), access.byte_offset, ConstantU32(state, 2)));
	const auto condition = EmitMemoryElementInBounds(state, access, element);
	ctx.Define(inst, EmitValueOrZeroIfCondition(state, condition, [&]() {
		           const auto pointer = EmitMemoryElementPointer(state, access, element);
		           if (access.memory_access == spv::MemoryAccessMaskNone) {
			           return EmitNative<spv::OpLoad, IR::Type::U32>(state, pointer);
		           }
		           return EmitCoherentWordLoad(state, pointer);
	           }));
}

void EmitLoadMemory(ValueEmitContext& ctx, const IR::Inst& inst) {
	const auto  op  = inst.GetOpcode();
	const auto& mem = ctx.Memory(inst);
	if (op == IR::ValueOpcode::LoadAddressU32 && mem.planning_only) {
		DefinePlanningOnlyRead(ctx, inst);
		return;
	}
	const auto buffer_components = IR::BufferComponentCount(op);
	const auto shared_components = IR::SharedComponentCount(op);
	const auto address_info      = IR::AddressOpcodeInfoOf(op);
	uint32_t   value;
	if (mem.kind == IR::ResourceKind::FlatLocal)
		value = LoadLocalFlat(ctx, inst);
	else if (buffer_components > 1u ||
	         (buffer_components == 1u && mem.kind == IR::ResourceKind::IndirectBuffer && !mem.formatted))
		value = LoadBuffer(ctx, inst, buffer_components);
	else if (shared_components > 1u)
		value = LoadWideShared(ctx, inst, shared_components);
	else if (mem.kind == IR::ResourceKind::ScalarAddress)
		value = LoadBdaDword(ctx, GuestAddress(ctx, inst, mem));
	else if (address_info.access == IR::AddressAccess::Read &&
	         mem.kind != IR::ResourceKind::Scratch)
		value = LoadBda(ctx, GuestAddress(ctx, inst, mem), ctx.Arg(inst, inst.NumArgs() - 1),
		                address_info.data_bits);
	else if (op == IR::ValueOpcode::LoadBufferU32 && mem.formatted)
		value = mem.kind != IR::ResourceKind::IndirectBuffer ? FormattedLoad(ctx, inst, mem)
		        : IndirectFormatSwitch()                     ? LoadIndirectFormattedX(ctx, inst)
		                                 : LoadIndirectFormattedTable(ctx, inst);
	else if (inst.GetType() == IR::Type::U8)
		value = LoadSubword(ctx, inst, mem, 8, false);
	else if (inst.GetType() == IR::Type::U16)
		value = LoadSubword(ctx, inst, mem, 16, false);
	else
		value = LoadWord(ctx, inst, mem);
	ctx.Define(inst, value);
}

void EmitStoreMemory(ValueEmitContext& ctx, const IR::Inst& inst) {
	const auto  op                = inst.GetOpcode();
	const auto& mem               = ctx.Memory(inst);
	const auto  buffer_components = IR::BufferComponentCount(op);
	const auto  shared_components = IR::SharedComponentCount(op);
	const auto  type              = inst.Arg(inst.NumArgs() - 2).GetType();
	if (mem.kind == IR::ResourceKind::FlatLocal)
		StoreLocalFlat(ctx, inst);
	else if (buffer_components > 1u)
		StoreWideBuffer(ctx, inst, buffer_components);
	else if (shared_components > 1u)
		StoreWideShared(ctx, inst, shared_components);
	else if (op == IR::ValueOpcode::StoreBufferU32 && mem.formatted)
		FormattedStore(ctx, inst, mem);
	else if (type == IR::Type::U8)
		StoreSubword(ctx, inst, mem, 8);
	else if (type == IR::Type::U16)
		StoreSubword(ctx, inst, mem, 16);
	else
		StoreWord(ctx, inst, mem);
}

uint32_t EmitSharedIncDec(ValueEmitContext& ctx, const IR::Inst& inst) {
	const auto replacement =
	    inst.GetOpcode() == IR::ValueOpcode::SharedAtomicInc32 ? AtomicIncrement : AtomicDecrement;
	return EmitAtomicUpdate(ctx, inst, ctx.Memory(inst), replacement);
}

void EmitSharedAtomicMaskedOr32(ValueEmitContext& ctx, const IR::Inst& inst) {
	const auto keep = Unary(ctx.state, spv::OpNot, TypeU32(ctx.state), ctx.Arg(inst, 1));
	EmitAtomicUpdate(ctx, inst, ctx.Memory(inst),
	                 [keep](EmitterState& state, uint32_t old, uint32_t value) {
		                 return Binary(state, spv::OpBitwiseOr, TypeU32(state),
		                               Binary(state, spv::OpBitwiseAnd, TypeU32(state), old, keep), value);
	                 });
}

uint32_t EmitSwizzleU32(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto& state = ctx.state;
	state.builder.AddFunction(spv::OpStore, ctx.scratch_u32_variable, ctx.Arg(inst, 0));
	const auto source = EmitNative<spv::OpLoad, IR::Type::U32>(state, ctx.scratch_u32_variable);
	const auto target = EmitDsSwizzleTargetLane(state, EmitSubgroupLocalInvocationId(state),
	                                            inst.Arg(1).IsImmediate() ? inst.Arg(1).U32() : 0);
	return EmitDsMaskedLaneRead(state, source, target, ctx.Arg(inst, 2));
}

uint32_t EmitPermuteU32(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto&      state   = ctx.state;
	auto       lane    = EmitSubgroupLocalInvocationId(state);
	if (state.lane_count == 2) {
		lane = Binary(state, spv::OpBitwiseAnd, TypeU32(state), lane, ConstantU32(state, 31));
	}
	const auto address = ctx.Arg(inst, 1);
	const auto word    = Binary(state, spv::OpShiftRightLogical, TypeU32(state), lane,
	                            ConstantU32(state, 5));
	const auto ballot_word = [&](uint32_t predicate) {
		const auto ballot = state.builder.AllocateId();
		const auto result = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpGroupNonUniformBallot, TypeU32Vector(state, 4), ballot,
		                          ConstantU32(state, spv::ScopeSubgroup), predicate);
		state.builder.AddFunction(spv::OpVectorExtractDynamic, TypeU32(state), result, ballot, word);
		return result;
	};
	// RDNA2 permutes independently within each 32-lane half. Intersect the source
	// address bit ballots to find this destination's enabled writers without LDS.
	auto writers = ballot_word(ctx.Arg(inst, 2));
	for (uint32_t bit = 0; bit < 5; ++bit) {
		const auto address_bit = Binary(state, spv::OpBitwiseAnd, TypeU32(state), address,
		                                ConstantU32(state, 1u << (bit + 2)));
		const auto mask = ballot_word(Binary(state, spv::OpINotEqual, TypeBool(state),
		                                      address_bit, ConstantU32(state, 0)));
		const auto lane_bit = Binary(state, spv::OpBitwiseAnd, TypeU32(state), lane,
		                             ConstantU32(state, 1u << bit));
		const auto selected = Select(
		    state, TypeU32(state),
		    Binary(state, spv::OpINotEqual, TypeBool(state), lane_bit, ConstantU32(state, 0)),
		    mask, Unary(state, spv::OpNot, TypeU32(state), mask));
		writers = Binary(state, spv::OpBitwiseAnd, TypeU32(state), writers, selected);
	}
	const auto active = Binary(state, spv::OpINotEqual, TypeBool(state), writers,
	                           ConstantU32(state, 0));
	const auto base = Binary(state, spv::OpBitwiseAnd, TypeU32(state), lane,
	                         ConstantU32(state, ~31u));
	const auto source = Select(state, TypeU32(state), active,
	                           Binary(state, spv::OpBitwiseOr, TypeU32(state), base,
	                                  EmitFindUMsb32(state, writers)), lane);
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpGroupNonUniformShuffle, TypeU32(state), result,
	                          ConstantU32(state, spv::ScopeSubgroup), ctx.Arg(inst, 0), source);
	return Select(state, TypeU32(state), active, result, ConstantU32(state, 0));
}

uint32_t EmitBpermuteU32(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto&      state  = ctx.state;
	const auto source = ctx.Arg(inst, 0);
	const auto index  = Binary(state, spv::OpBitwiseAnd, TypeU32(state),
	                           Binary(state, spv::OpShiftRightLogical, TypeU32(state),
	                                  ctx.Arg(inst, 1), ConstantU32(state, 2)),
	                           ConstantU32(state, 31));
	const auto base   = Binary(state, spv::OpBitwiseAnd, TypeU32(state),
	                           EmitSubgroupLocalInvocationId(state), ConstantU32(state, ~31u));
	const auto target = Binary(state, spv::OpBitwiseOr, TypeU32(state), base, index);
	return EmitDsMaskedLaneRead(state, source, target, ctx.Arg(inst, 2));
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
