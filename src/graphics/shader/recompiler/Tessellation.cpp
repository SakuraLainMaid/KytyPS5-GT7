#include "graphics/shader/recompiler/Tessellation.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ConstantPropagation.h"
#include "graphics/shader/recompiler/ir/passes/DeadCodeElimination.h"

#include <algorithm>
#include <array>
#include <fmt/format.h>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler {
namespace {

// An address as coefficient * index + constant. Affine: the index is the LS vertex or HS
// control point of the invocation. Indexed: a value the program loaded or computed (a control
// point a stencil table names, say), only usable where the stride is checked, not derived.
struct TessellationAddress {
	enum class Kind { Unknown, Affine, Indexed, PackedControlPoint };
	Kind     kind        = Kind::Unknown;
	uint32_t coefficient = 0;
	uint32_t constant    = 0;
	// Which definition an Unknown value, and so an Indexed one, came from: values of one origin
	// share their unknown index.
	uint32_t origin = 0;
};

bool IsLocalStore(Decoder::Opcode opcode) {
	using Decoder::Opcode;
	switch (opcode) {
		case Opcode::DS_WRITE_B32:
		case Opcode::DS_WRITE2_B32:
		case Opcode::DS_WRITE_B64:
		case Opcode::DS_WRITE2_B64:
		case Opcode::DS_WRITE_B96:
		case Opcode::DS_WRITE_B128: return true;
		default: return false;
	}
}

bool IsLocalRead(Decoder::Opcode opcode) {
	using Decoder::Opcode;
	switch (opcode) {
		case Opcode::DS_READ_B32:
		case Opcode::DS_READ2_B32:
		case Opcode::DS_READ_B64:
		case Opcode::DS_READ2_B64:
		case Opcode::DS_READ_B96:
		case Opcode::DS_READ_B128: return true;
		default: return false;
	}
}

// The byte ranges an HS writes to its off-chip ring at constant addresses (patch constants
// among them), as [begin, end).
using ConstantStores = std::vector<std::pair<uint32_t, uint32_t>>;

std::optional<uint32_t> ReflectTessellationStride(const Decoder::Program& program, bool local,
                                                  uint32_t control_points, uint32_t input_stride,
                                                  ConstantStores* constant_stores = nullptr) {
	using namespace Decoder;
	using Address   = TessellationAddress;
	using Kind      = Address::Kind;
	using Registers = std::array<Address, IR::NumVectorRegs>;
	Registers registers {};
	registers[local ? 3u : 1u] =
	    local ? Address {Kind::Affine, 1u, 0u} : Address {Kind::PackedControlPoint};
	// Scalar registers holding a constant (s_mov of one), for addresses built from them; VCC
	// as s106:s107.
	std::array<std::optional<uint32_t>, 128> scalars {};
	const auto scalar_slot = [](const Operand& operand) -> std::optional<uint32_t> {
		if (operand.kind == OperandKind::Sgpr && operand.reg < 106u) return operand.reg;
		if (operand.kind == OperandKind::VccLo) return 106u;
		if (operand.kind == OperandKind::VccHi) return 107u;
		return std::nullopt;
	};
	const auto constant = [](uint32_t value) { return Address {Kind::Affine, 0u, value}; };
	const auto read     = [&](const Operand& operand) -> Address {
		if (operand.absolute || operand.negate || operand.sdwa_sext || operand.op_sel ||
		    operand.op_sel_hi || operand.negate_hi || operand.dpp)
			return {};
		Address value;
		if (operand.kind == OperandKind::IntegerInlineConstant ||
		    operand.kind == OperandKind::LiteralConstant) {
			value = constant(operand.value);
		} else if (operand.kind == OperandKind::Vgpr) {
			value = registers.at(operand.reg);
		} else if (const auto slot = scalar_slot(operand); slot && scalars[*slot]) {
			value = constant(*scalars[*slot]);
		}
		// The native packed HS ID's low byte is the patch ordinal. The logical
		// interface addresses one patch, as in the frontend's v1 initialization.
		if (value.kind == Kind::PackedControlPoint && operand.sdwa_sel == 0u) return constant(0u);
		return operand.sdwa_sel == 6u ? value : Address {};
	};
	const auto affine = [](uint64_t coefficient, uint64_t offset) {
		return coefficient <= UINT32_MAX && offset <= UINT32_MAX
		           ? Address {Kind::Affine, static_cast<uint32_t>(coefficient),
		                      static_cast<uint32_t>(offset)}
		           : Address {};
	};
	const auto is_constant = [](const Address& value) {
		return value.kind == Kind::Affine && value.coefficient == 0u;
	};
	const auto with_kind = [](Address value, Kind kind) {
		if (value.kind != Kind::Unknown && value.coefficient != 0u) value.kind = kind;
		return value;
	};
	const auto add = [&](Address lhs, Address rhs) {
		if (lhs.kind == Kind::Affine && rhs.kind == Kind::Affine)
			return affine(uint64_t {lhs.coefficient} + rhs.coefficient,
			              uint64_t {lhs.constant} + rhs.constant);
		if (lhs.kind == Kind::Indexed && rhs.kind == Kind::Indexed && lhs.origin == rhs.origin) {
			auto sum   = affine(uint64_t {lhs.coefficient} + rhs.coefficient,
			                    uint64_t {lhs.constant} + rhs.constant);
			sum        = with_kind(sum, Kind::Indexed);
			sum.origin = lhs.origin;
			return sum;
		}
		// An indexed value plus a constant stays indexed in the same unknown index.
		if (is_constant(rhs)) std::swap(lhs, rhs);
		if (is_constant(lhs) && rhs.kind == Kind::Indexed) {
			auto sum   = with_kind(affine(rhs.coefficient, uint64_t {lhs.constant} + rhs.constant),
			                       Kind::Indexed);
			sum.origin = rhs.origin;
			return sum;
		}
		return Address {};
	};
	const auto multiply = [&](Address lhs, Address rhs) {
		if ((is_constant(lhs) && lhs.constant == 0u) || (is_constant(rhs) && rhs.constant == 0u))
			return constant(0u);
		if (is_constant(lhs)) std::swap(lhs, rhs);
		if (!is_constant(rhs)) return Address {};
		// A value the program loaded, scaled: an index of unknown origin.
		if (lhs.kind == Kind::Unknown) {
			if (lhs.origin == 0u) return Address {};
			auto product   = with_kind(affine(rhs.constant, 0u), Kind::Indexed);
			product.origin = lhs.origin;
			return product;
		}
		if (lhs.kind != Kind::Affine && lhs.kind != Kind::Indexed) return Address {};
		auto product   = with_kind(affine(uint64_t {lhs.coefficient} * rhs.constant,
		                                  uint64_t {lhs.constant} * rhs.constant),
		                           lhs.kind);
		product.origin = lhs.origin;
		return product;
	};
	const auto low24 = [&](Address value) {
		if (is_constant(value)) return constant(value.constant & 0xffffffu);
		if (value.kind == Kind::Unknown || value.kind == Kind::Indexed) return value;
		return value.kind == Kind::Affine &&
		               uint64_t {value.coefficient} * (control_points - 1u) + value.constant <=
		                   0xffffffu
		           ? value
		           : Address {};
	};
	const auto mask = [&](Address value, Address bits) -> Address {
		if (is_constant(value)) std::swap(value, bits);
		if (!is_constant(bits)) return {};
		if (is_constant(value)) return constant(value.constant & bits.constant);
		// The packed HS ID: the control point in bits 8-12, the patch ordinal in the low byte.
		if (value.kind == Kind::PackedControlPoint) {
			if ((bits.constant & ~0xffu) == 0u) return constant(0u);
			if (bits.constant == 0x1f00u) return {Kind::Affine, 0x100u, 0u};
		}
		return {};
	};
	const auto same = [](const Address& a, const Address& b) {
		return a.kind == b.kind && a.coefficient == b.coefficient && a.constant == b.constant &&
		       a.origin == b.origin;
	};
	const auto merge = [&](Registers& into, const Registers& other) {
		for (size_t index = 0; index < into.size(); index++) {
			if (!same(into[index], other[index])) into[index] = {};
		}
	};
	const auto& instructions = program.instructions;
	if (instructions.empty()) return std::nullopt;
	const uint32_t exit_pc = instructions.back().pc;
	// Branches inside the program (waterfall loops, skipped blocks): a forward branch's target
	// joins the register state at the branch with the fall-through one, keeping only the values
	// both agree on; a backward branch's target, a loop header, forgets every VGPR the loop body
	// writes. Branches to the end are stage exits and keep the active path's definitions.
	std::unordered_map<uint32_t, std::vector<uint32_t>> loop_writes;
	for (size_t index = 0; index < instructions.size(); index++) {
		const auto& branch = instructions[index];
		if (!IsDirectBranch(branch.opcode) || branch.branch_target == exit_pc ||
		    branch.branch_target > branch.pc)
			continue;
		auto& writes = loop_writes[branch.branch_target];
		for (const auto& inst: instructions) {
			if (inst.pc < branch.branch_target || inst.pc > branch.pc ||
			    inst.dst.kind != OperandKind::Vgpr)
				continue;
			for (uint32_t dword = 0; dword < std::max(inst.data_dwords, 1u); dword++)
				writes.push_back(inst.dst.reg + dword);
			if (inst.opcode == Opcode::V_MOVRELD_B32) {
				for (uint32_t reg = inst.dst.reg; reg < IR::NumVectorRegs; reg++)
					writes.push_back(reg);
			}
		}
	}
	std::unordered_map<uint32_t, std::vector<Registers>> joins;
	bool                                                 reachable = true;
	uint32_t                                             stride    = 0;
	uint32_t                                             origins   = 0;
	for (const auto& inst: instructions) {
		if (const auto found = joins.find(inst.pc); found != joins.end()) {
			for (const auto& state: found->second) {
				if (reachable) {
					merge(registers, state);
				} else {
					registers = state;
					reachable = true;
				}
			}
			joins.erase(found);
		}
		if (const auto found = loop_writes.find(inst.pc); found != loop_writes.end()) {
			for (const auto reg: found->second) {
				if (reg < registers.size()) registers[reg] = {};
			}
		}
		reachable = true;
		if (IsDirectBranch(inst.opcode) && inst.branch_target != exit_pc) {
			if (inst.branch_target > inst.pc) joins[inst.branch_target].push_back(registers);
			reachable = inst.opcode != Opcode::S_BRANCH;
		}
		const bool local_store  = IsLocalStore(inst.opcode);
		const bool buffer_store = inst.opcode == Opcode::BUFFER_STORE_DWORD ||
		                          inst.opcode == Opcode::BUFFER_STORE_DWORDX2 ||
		                          inst.opcode == Opcode::BUFFER_STORE_DWORDX3 ||
		                          inst.opcode == Opcode::BUFFER_STORE_DWORDX4;
		const bool control_store =
		    !local && buffer_store && inst.src2.kind == OperandKind::Sgpr && inst.src2.reg == 2u;
		const bool control_read = !local && IsLocalRead(inst.opcode);
		if ((local && local_store) || control_store || control_read) {
			const auto address = read(inst.src0);
			// A read may name its control point by a loaded index; a store's stride is derived
			// from it, so its index must be the invocation's own.
			if (address.kind != Kind::Affine && !(control_read && address.kind == Kind::Indexed)) {
				LOGF("%s tessellation address is not affine at pc 0x%08x\n", local ? "LS" : "HS",
				     inst.pc);
				return std::nullopt;
			}
			if (address.coefficient != 0u) {
				const auto expected = control_read ? input_stride : stride;
				if ((address.coefficient & 3u) != 0u ||
				    (expected != 0u && expected != address.coefficient)) {
					LOGF("%s tessellation stride %u at pc 0x%08x disagrees with %u\n",
					     local ? "LS" : "HS", address.coefficient, inst.pc, expected);
					return std::nullopt;
				}
				if (!control_read) stride = address.coefficient;
			} else if (control_store && constant_stores != nullptr) {
				const uint64_t begin = uint64_t {address.constant} + inst.offset;
				const uint64_t end   = begin + 4ull * std::max(inst.data_dwords, 1u);
				if (end > UINT32_MAX) return std::nullopt;
				constant_stores->emplace_back(static_cast<uint32_t>(begin),
				                              static_cast<uint32_t>(end));
			}
		}
		if (const auto slot = scalar_slot(inst.dst); slot) {
			// Every register the write may cover: a scalar load's dwords, or a 64-bit pair.
			const auto value = inst.opcode == Opcode::S_MOV_B32 || inst.opcode == Opcode::S_MOVK_I32
			                       ? read(inst.src0)
			                       : Address {};
			const uint32_t covered = std::max(inst.data_dwords, 2u);
			for (uint32_t index = 0; index < covered && *slot + index < scalars.size(); index++)
				scalars[*slot + index].reset();
			if (is_constant(value)) scalars[*slot] = value.constant;
			continue;
		}
		// Store vdata is a source, despite occupying the decoder's dst field.
		if (local_store || buffer_store || inst.dst.kind != OperandKind::Vgpr) continue;
		if (inst.opcode == Opcode::V_MOVRELD_B32) {
			for (uint32_t reg = inst.dst.reg; reg < registers.size(); reg++)
				registers[reg] = {};
			continue;
		}
		const auto lhs   = read(inst.src0);
		const auto rhs   = read(inst.src1);
		const auto third = read(inst.src2);
		Address    value;
		switch (inst.opcode) {
			case Opcode::V_MOV_B32: value = lhs; break;
			case Opcode::V_AND_B32: value = mask(lhs, rhs); break;
			case Opcode::V_BFE_U32:
				if (lhs.kind == Kind::PackedControlPoint && rhs.kind == Kind::Affine &&
				    rhs.coefficient == 0u && third.kind == Kind::Affine &&
				    third.coefficient == 0u) {
					if (rhs.constant == 8u && third.constant == 5u) value = {Kind::Affine, 1u, 0u};
					if (rhs.constant == 0u && third.constant == 8u) value = constant(0u);
				}
				break;
			case Opcode::V_MUL_U32_U24: value = multiply(low24(lhs), low24(rhs)); break;
			case Opcode::V_MUL_LO_U32: value = multiply(lhs, rhs); break;
			case Opcode::V_MAD_U32_U24: value = add(multiply(low24(lhs), low24(rhs)), third); break;
			case Opcode::V_ADD_NC_U32: value = add(lhs, rhs); break;
			case Opcode::V_ADD3_U32: value = add(add(lhs, rhs), third); break;
			case Opcode::V_LSHL_ADD_U32:
				if (rhs.kind == Kind::Affine && rhs.coefficient == 0u && rhs.constant < 32u)
					value = add(multiply(lhs, constant(1u << rhs.constant)), third);
				break;
			case Opcode::V_LSHLREV_B32:
				if (lhs.kind == Kind::Affine && lhs.coefficient == 0u && lhs.constant < 32u)
					value = multiply(rhs, constant(1u << lhs.constant));
				break;
			case Opcode::V_SUB_NC_U32:
				if (lhs.kind == Kind::Affine && rhs.kind == Kind::Affine &&
				    lhs.coefficient >= rhs.coefficient && lhs.constant >= rhs.constant)
					value = affine(lhs.coefficient - rhs.coefficient, lhs.constant - rhs.constant);
				break;
			default: break;
		}
		if (inst.dst.sdwa_sel != 6u || inst.dst.op_sel || inst.dst.omod || inst.dst.clamp ||
		    inst.dst.dpp)
			value = {};
		for (uint32_t index = 0;
		     index < std::max(inst.data_dwords, 1u) && inst.dst.reg + index < registers.size(); index++) {
			registers[inst.dst.reg + index] = {};
			registers[inst.dst.reg + index].origin = ++origins;
		}
		if (value.kind == Kind::Unknown) value.origin = registers.at(inst.dst.reg).origin;
		registers.at(inst.dst.reg) = value;
	}
	if (stride == 0u) {
		LOGF("%s tessellation program has no attribute stride\n", local ? "LS" : "HS");
		return std::nullopt;
	}
	return stride;
}

const IR::Inst* TessellationBufferBase(const IR::Inst& inst) {
	if (IR::BufferAccessOf(inst.GetOpcode()) == IR::BufferAccess::None || inst.NumArgs() <= 3u)
		return nullptr;
	const auto* base = inst.Arg(3).Resolve().TryInstruction();
	return base != nullptr && base->GetOpcode() == IR::ValueOpcode::TessellationBase ? base
	                                                                                 : nullptr;
}

// Diagnostics: a value's expression, a few levels deep.
std::string DescribeValue(IR::Value value, uint32_t depth = 0) {
	value            = value.Resolve();
	const auto* inst = value.TryInstruction();
	if (inst == nullptr) {
		return value.IsImmediate() ? fmt::format("0x{:x}", value.U32()) : std::string("?");
	}
	std::string text(IR::ValueOpcodeName(inst->GetOpcode()));
	if (depth >= 5u) return text + "(..)";
	text += "(";
	for (size_t index = 0; index < inst->NumArgs(); index++) {
		text += (index != 0u ? ", " : "") + DescribeValue(inst->Arg(index), depth + 1u);
	}
	return text + ")";
}

// Whether predicate p implies q: equal, or a conjunction one of whose terms does.
bool ImpliesPredicate(IR::Value p, IR::Value q, uint32_t depth = 0) {
	p = p.Resolve();
	q = q.Resolve();
	if (p == q) return true;
	const auto* inst = p.TryInstruction();
	if (inst == nullptr || depth > 16u || inst->GetOpcode() != IR::ValueOpcode::LogicalAnd)
		return false;
	return ImpliesPredicate(inst->Arg(0), q, depth + 1u) ||
	       ImpliesPredicate(inst->Arg(1), q, depth + 1u);
}

// The address an access guarded by predicate computes on its active lanes: a register the
// guest wrote under EXEC holds Select(exec, written, previous), and any EXEC the access's own
// implies selects the written value. The integer arithmetic over such values (the patch
// ordinal scaled into factor and patch-constant addresses, say) is rebuilt on the result, so
// that constant propagation can fold it.
IR::Value ActiveAddress(IR::Block& block, IR::Block::iterator before, IR::Value value,
                        IR::Value predicate, std::unordered_map<IR::Inst*, IR::Value>& resolved,
                        uint32_t depth = 0) {
	using namespace IR;
	value        = value.Resolve();
	auto* source = value.TryInstruction();
	if (source == nullptr || depth > 64u) return value;
	if (const auto found = resolved.find(source); found != resolved.end()) return found->second;
	const auto opcode = source->GetOpcode();
	if (opcode == ValueOpcode::SelectU32) {
		const auto result =
		    ImpliesPredicate(predicate, source->Arg(0))
		        ? ActiveAddress(block, before, source->Arg(1), predicate, resolved, depth + 1u)
		        : value;
		resolved.emplace(source, result);
		return result;
	}
	switch (opcode) {
		case ValueOpcode::IAdd32:
		case ValueOpcode::ISub32:
		case ValueOpcode::IMul32:
		case ValueOpcode::ShiftLeftLogical32:
		case ValueOpcode::ShiftRightLogical32:
		case ValueOpcode::BitwiseAnd32:
		case ValueOpcode::BitwiseOr32:
		case ValueOpcode::BitFieldUExtract: break;
		default: return value;
	}
	std::array<Value, 3> args {};
	bool                 changed = false;
	const auto           count   = std::min<size_t>(source->NumArgs(), args.size());
	for (size_t index = 0; index < count; index++) {
		args[index] =
		    ActiveAddress(block, before, source->Arg(index), predicate, resolved, depth + 1u);
		changed |= args[index] != source->Arg(index).Resolve();
	}
	// The bits below a shift are zero: the HS's packed ID is InvocationId << 8 (the frontend's v1),
	// so its patch ordinal, the low byte, is 0 in the logical interface.
	const auto shifted_by = [](const Value& operand) -> std::optional<uint32_t> {
		const auto* shift = operand.Resolve().TryInstruction();
		if (shift == nullptr || shift->GetOpcode() != ValueOpcode::ShiftLeftLogical32 ||
		    !shift->Arg(1).Resolve().IsImmediate())
			return std::nullopt;
		return shift->Arg(1).Resolve().U32();
	};
	if (opcode == ValueOpcode::BitwiseAnd32) {
		for (size_t index = 0; index < 2u; index++) {
			const auto shift = shifted_by(args[index]);
			const auto mask  = args[1u - index].Resolve();
			if (shift && *shift < 32u && mask.IsImmediate() && mask.U32() < (1u << *shift)) {
				resolved.emplace(source, Value(0u));
				return Value(0u);
			}
		}
	}
	if (opcode == ValueOpcode::BitFieldUExtract && args[1].Resolve().IsImmediate() &&
	    args[2].Resolve().IsImmediate()) {
		const auto shift = shifted_by(args[0]);
		if (shift && uint64_t {args[1].Resolve().U32()} + args[2].Resolve().U32() <= *shift) {
			resolved.emplace(source, Value(0u));
			return Value(0u);
		}
	}
	if (changed) {
		auto copy = count == 3u ? block.PrependNewInst(before, opcode, {args[0], args[1], args[2]},
		                                               source->Flags<uint64_t>())
		                        : block.PrependNewInst(before, opcode, {args[0], args[1]},
		                                               source->Flags<uint64_t>());
		value     = Value(&*copy);
	}
	resolved.emplace(source, value);
	return value;
}

} // namespace

bool AnalyzeTessellationPrograms(std::span<const uint32_t> local, std::span<const uint32_t> control,
                                 ShaderTessellationInputInfo& info) {
	// VGT_TF_PARAM: the domain (0 isoline, 1 triangle, 2 quad), the partitioning (integer,
	// pow2, fractional odd, fractional even) and the output topology (point, line, triangle cw,
	// triangle ccw). Isolines are not modelled; a pow2 partitioning is drawn as integer.
	if ((info.domain != 1u && info.domain != 2u) || info.partitioning > 3u ||
	    info.output_topology == 1u || info.output_topology > 3u) {
		LOGF("Tessellation configuration not modelled: domain %u, partitioning %u, topology %u\n",
		     info.domain, info.partitioning, info.output_topology);
		return false;
	}
	const auto       local_program = Decoder::DecodeFrontProgram(local);
	Decoder::Program control_program;
	Decoder::DecodeProgram(control, control_program);
	const auto ls_stride =
	    ReflectTessellationStride(local_program, true, info.input_control_points, 0u);
	if (!ls_stride) {
		return false;
	}
	ConstantStores constant_stores;
	const auto     hs_stride = ReflectTessellationStride(
        control_program, false, info.output_control_points, *ls_stride, &constant_stores);
	if (!hs_stride) {
		return false;
	}
	info.ls_stride = *ls_stride;
	info.hs_stride = *hs_stride;
	// The patch constants: what the HS writes at constant addresses past its control points.
	const uint32_t control_end = info.hs_stride * info.output_control_points;
	uint32_t       patch_begin = UINT32_MAX, patch_end = 0;
	for (const auto& [begin, end]: constant_stores) {
		if (begin >= control_end) {
			patch_begin = std::min(patch_begin, begin);
			patch_end   = std::max(patch_end, end);
		}
	}
	info.patch_base = 0;
	info.patch_size = 0;
	if (patch_begin < patch_end) {
		info.patch_base = patch_begin & ~15u;
		info.patch_size = ((patch_end + 15u) & ~15u) - info.patch_base;
	}
	LOGF("Tessellation interface: input_cp=%u output_cp=%u ls_stride=%u hs_stride=%u patch "
	     "0x%x+0x%x domain %u partitioning %u topology %u\n",
	     info.input_control_points, info.output_control_points, info.ls_stride, info.hs_stride,
	     info.patch_base, info.patch_size, info.domain, info.partitioning, info.output_topology);
	return true;
}

bool LowerTessellationMemory(IR::Program& program, const CompileOptions& options) {
	using namespace IR;
	if (options.stage != ShaderType::Local && options.stage != ShaderType::TessellationControl &&
	    options.stage != ShaderType::TessellationEvaluation) {
		return true;
	}
	uint32_t   current_pc  = 0;
	const auto unsupported = [&](const char* what) {
		LOGF("%s tessellation lowering does not model %s (pc 0x%08x)\n",
		     options.stage == ShaderType::Local                 ? "LS"
		     : options.stage == ShaderType::TessellationControl ? "HS"
		                                                        : "TES",
		     what, current_pc);
		return false;
	};
	const auto& tess = options.input_info.vertex->tess;
	// Ring addresses can reuse data VGPRs. Their inactive values are irrelevant to
	// a store guarded by the same EXEC predicate, but must remain intact elsewhere.
	for (auto* block: program.blocks) {
		for (auto it = block->begin(); it != block->end(); ++it) {
			if (TessellationBufferBase(*it) == nullptr) continue;
			std::unordered_map<Inst*, Value> resolved;
			it->SetArg(
			    2, ActiveAddress(*block, it, it->Arg(2), it->Arg(it->NumArgs() - 1u), resolved));
		}
	}
	ConstantPropagationPass(program.blocks, program.wave_size);
	uint32_t reads = 0, writes = 0, factors = 0;
	for (auto* block: program.blocks) {
		for (auto it = block->begin(); it != block->end(); ++it) {
			auto&      inst   = *it;
			const auto shared = SharedAccessOf(inst.GetOpcode());
			const auto buffer = BufferAccessOf(inst.GetOpcode());
			if (shared == SharedAccess::None && buffer == BufferAccess::None) {
				continue;
			}
			const auto&           memory = program.memory_info.at(inst.Flags<MemoryFlags>().index);
			current_pc                   = inst.Flags<MemoryFlags>().pc;
			bool                  write  = false;
			uint32_t              components = 0;
			TessellationAttribute kind;
			Value                 address, predicate;
			if (shared != SharedAccess::None && memory.kind == ResourceKind::Lds) {
				write = shared == SharedAccess::Write;
				if (memory.data_bits != 32u ||
				    (options.stage == ShaderType::Local
				         ? !write
				         : options.stage != ShaderType::TessellationControl ||
				               shared != SharedAccess::Read)) {
					return unsupported("this LDS access");
				}
				kind       = write ? TessellationAttribute::LocalOutput
				                   : TessellationAttribute::ControlInput;
				components = SharedComponentCount(inst.GetOpcode());
				address    = inst.Arg(0);
				predicate  = inst.Arg(inst.NumArgs() - 1u);
			} else if (buffer != BufferAccess::None && memory.kind == ResourceKind::Buffer) {
				const auto* base = TessellationBufferBase(inst);
				if (base == nullptr) {
					continue;
				}
				if (memory.data_bits != 32u || memory.formatted || memory.idxen || !memory.offen ||
				    buffer == BufferAccess::Atomic) {
					return unsupported("this ring buffer access");
				}
				write      = buffer == BufferAccess::Write;
				components = BufferComponentCount(inst.GetOpcode());
				address    = inst.Arg(2).Resolve();
				predicate  = inst.Arg(inst.NumArgs() - 1u);
				const bool control = options.stage == ShaderType::TessellationControl;
				if (base->Arg(0).U32() == 1u) {
					if (!write || !control) {
						return unsupported("a tessellation factor access outside the HS");
					}
					kind = TessellationAttribute::Factor;
					factors += components;
				} else {
					// The HS also reads back what its invocations wrote (control points'
					// outputs, after a barrier below); the TES reads them as its inputs.
					if (write ? !control
					          : !control && options.stage != ShaderType::TessellationEvaluation) {
						return unsupported("a control-point access in the wrong stage");
					}
					kind                       = control ? TessellationAttribute::ControlOutput
					                                     : TessellationAttribute::EvaluationInput;
					const uint32_t control_end = tess.hs_stride * tess.output_control_points;
					if (address.IsImmediate() && address.U32() + memory.offset >= control_end) {
						// Patch constants, past the control points: the region the analysis found
						// the HS writing.
						const uint64_t first = uint64_t {address.U32()} + memory.offset;
						if (tess.patch_size == 0u || first < tess.patch_base ||
						    first + 4u * components >
						        uint64_t {tess.patch_base} + tess.patch_size) {
							return unsupported("a patch-constant access outside the HS's");
						}
						kind = TessellationAttribute::PatchOutput;
					}
				}
			} else {
				continue;
			}
			const auto emit = [&](ValueOpcode opcode, std::initializer_list<Value> args) {
				return Value(&*block->PrependNewInst(it, opcode, args));
			};
			std::array<Value, 4> values;
			for (uint32_t component = 0; component < components; component++) {
				const auto offset = memory.offset + 4u * component;
				const auto byte_address =
				    offset == 0u ? address : emit(ValueOpcode::IAdd32, {address, Value(offset)});
				if (write) {
					Value data = shared != SharedAccess::None ? inst.Arg(component + 1u)
					             : components == 1u
					                 ? inst.Arg(4)
					                 : emit(components == 2u   ? ValueOpcode::CompositeExtractU32x2
					                        : components == 3u ? ValueOpcode::CompositeExtractU32x3
					                                           : ValueOpcode::CompositeExtractU32x4,
					                        {inst.Arg(4), Value(component)});
					emit(ValueOpcode::SetTessellationAttribute,
					     {Value(static_cast<uint32_t>(kind)), byte_address, data, predicate});
					writes++;
				} else {
					values[component] =
					    emit(ValueOpcode::GetTessellationAttribute,
					         {Value(static_cast<uint32_t>(kind)), byte_address, predicate});
					reads++;
				}
			}
			if (!write) {
				Value replacement = values[0];
				if (components == 2u)
					replacement =
					    emit(ValueOpcode::CompositeConstructU32x2, {values[0], values[1]});
				if (components == 3u)
					replacement = emit(ValueOpcode::CompositeConstructU32x3,
					                   {values[0], values[1], values[2]});
				if (components == 4u)
					replacement = emit(ValueOpcode::CompositeConstructU32x4,
					                   {values[0], values[1], values[2], values[3]});
				inst.ReplaceUsesWith(replacement);
			}
			inst.Invalidate();
		}
	}
	// An HS invocation reading what another wrote (the console's wave runs a patch's control
	// points together): a barrier after the writes before such a read, where every invocation
	// still runs. GT7's subdivision-surface HSs have control point 0 read all four outputs back
	// to compute the patch's factors.
	uint32_t barriers = 0;
	if (options.stage == ShaderType::TessellationControl) {
		const auto attribute_of = [](const Inst& inst) {
			return static_cast<TessellationAttribute>(inst.Arg(0).U32());
		};
		Block*          last_block = nullptr;
		Block::iterator last_write;
		for (auto* block: program.blocks) {
			for (auto it = block->begin(); it != block->end(); ++it) {
				const auto opcode = it->GetOpcode();
				if (opcode == ValueOpcode::Barrier) {
					last_block = nullptr;
				} else if (opcode == ValueOpcode::SetTessellationAttribute &&
				           attribute_of(*it) == TessellationAttribute::ControlOutput) {
					last_block = block;
					last_write = it;
				} else if (opcode == ValueOpcode::GetTessellationAttribute &&
				           (attribute_of(*it) == TessellationAttribute::ControlOutput ||
				            attribute_of(*it) == TessellationAttribute::PatchOutput) &&
				           last_block != nullptr) {
					last_block->PrependNewInst(std::next(last_write), ValueOpcode::Barrier, {});
					last_block = nullptr;
					barriers++;
				}
			}
		}
	}
	ConstantPropagationPass(program.blocks, program.wave_size);
	RemoveIdentities(program.blocks);
	EliminateDeadCode(program.blocks);
	if (barriers != 0) {
		LOGF("HS tessellation lowering: %u barrier(s) before control-point outputs are read\n",
		     barriers);
	}
	// The emitter indexes factors and patch constants by constant addresses only.
	const uint32_t factor_count = tess.domain == 2u ? 6u : 4u;
	for (auto* block: program.blocks) {
		for (const auto& inst: *block) {
			if (inst.GetOpcode() != ValueOpcode::GetTessellationAttribute &&
			    inst.GetOpcode() != ValueOpcode::SetTessellationAttribute)
				continue;
			const auto kind = static_cast<TessellationAttribute>(inst.Arg(0).U32());
			if (kind != TessellationAttribute::Factor && kind != TessellationAttribute::PatchOutput)
				continue;
			current_pc = 0;
			if (!inst.Arg(1).IsImmediate()) {
				LOGF("HS tessellation address: %s\n", DescribeValue(inst.Arg(1)).c_str());
				return unsupported(kind == TessellationAttribute::Factor
				                       ? "a tessellation factor at a computed address"
				                       : "a patch constant at a computed address");
			}
			if (kind == TessellationAttribute::Factor && inst.Arg(1).U32() / 4u >= factor_count) {
				return unsupported("a tessellation factor past the domain's");
			}
		}
	}
	LOGF("%s tessellation lowering: reads=%u writes=%u factors=%u\n",
	     options.stage == ShaderType::Local                 ? "LS"
	     : options.stage == ShaderType::TessellationControl ? "HS"
	                                                       : "TES",
	     reads, writes, factors);
	return true;
}

} // namespace Libs::Graphics::ShaderRecompiler
