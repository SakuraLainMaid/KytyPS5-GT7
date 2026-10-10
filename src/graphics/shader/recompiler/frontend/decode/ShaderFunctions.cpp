#include "graphics/shader/recompiler/frontend/decode/ShaderFunctions.h"

#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"
#include "graphics/shader/shaderBindings.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <string_view>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <unordered_map>

namespace Libs::Graphics::ShaderRecompiler::Decoder {
namespace {

constexpr uint32_t MaxFunctionWords  = 4096;
constexpr uint32_t MaxExpandedWords  = 131072;
constexpr uint64_t MaxCallTableBytes = 16u * 1024u * 1024u;
constexpr size_t   MaxCallTargets    = 256;
// Specialized variants carry their own descriptors, and the translation time grows faster than
// linearly with them (GT7: 27 variants ~1.4 s, 54 ~8.6 s); KYTY_CALL_VARIANTS overrides.
constexpr size_t   DefaultMaxSpecializedVariants = 32;
size_t MaxSpecializedVariants() {
	static const size_t value = [] {
		// KYTY_CALL_VARIANTS=0: no specialized variants at all (every one takes the miss path).
		const char* text = std::getenv("KYTY_CALL_VARIANTS");
		if (text == nullptr || text[0] == '\0') return DefaultMaxSpecializedVariants;
		return std::min<size_t>(std::strtoul(text, nullptr, 10), MaxCallTargets);
	}();
	return value;
}
// A table may hold more targets than one expansion inlines: the expansion then takes the
// reported ones (ReportShaderCallMiss) and traps on the rest.
constexpr size_t   MaxTableTargets   = 16384;
// Words kept free of callee code for the main program, the selection trees and branch islands.
constexpr uint32_t ExpansionSlackWords = 8192;

bool IsCall(uint32_t word) {
	return (word & 0xff80ff00u) == 0xbe802100u && ((word >> 16u) & 127u) != 125u;
}

uint32_t DestinationWords(const Instruction& inst, uint32_t wave_size) {
	const auto name = magic_enum::enum_name(inst.opcode);
	if (name.starts_with("V_CMP")) return wave_size / 32u;
	const bool pair = name.find("64") != std::string_view::npos;
	return std::max(inst.data_dwords, pair ? 2u : 1u);
}

bool Writes(const Instruction& inst, uint32_t reg, uint32_t wave_size) {
	if (IsCall(inst.raw[0])) {
		const auto dst = (inst.raw[0] >> 16u) & 127u;
		return reg == dst || reg == dst + 1u;
	}
	return (inst.dst.kind == OperandKind::Sgpr && reg >= inst.dst.reg &&
	        reg < inst.dst.reg + DestinationWords(inst, wave_size)) ||
	       (inst.dst2.kind == OperandKind::Sgpr && reg >= inst.dst2.reg &&
	        reg < inst.dst2.reg + wave_size / 32u);
}

struct Function {
	std::vector<uint32_t>    code;
	std::vector<Instruction> instructions;
};

bool ReadsScc(const Instruction& inst) {
	switch (inst.opcode) {
		case Opcode::S_CBRANCH_SCC0:
		case Opcode::S_CBRANCH_SCC1:
		case Opcode::S_CSELECT_B32:
		case Opcode::S_CSELECT_B64:
		case Opcode::S_ADDC_U32:
		case Opcode::S_SUBB_U32:
		case Opcode::S_CMOV_B64: return true;
		default: return false;
	}
}

// Conservative: true only for scalar ALU operations that always set SCC.
bool WritesScc(const Instruction& inst) {
	const auto name = magic_enum::enum_name(inst.opcode);
	if (name.starts_with("S_CMP") || name.starts_with("S_BITCMP")) return true;
	static constexpr std::string_view prefixes[] = {
	    "S_AND_B", "S_OR_B", "S_XOR_B", "S_NAND_B", "S_NOR_B", "S_XNOR_B", "S_ANDN2_B",
	    "S_ORN2_B", "S_LSHL_B", "S_LSHR_B", "S_ASHR_I", "S_BFE_", "S_NOT_B", "S_BCNT",
	    "S_MIN_", "S_MAX_", "S_ABS_I32", "S_ABSDIFF", "S_ADD_U32", "S_ADD_I32", "S_SUB_U32",
	    "S_SUB_I32", "S_ADDK_I32", "S_LSHL1_ADD", "S_LSHL2_ADD", "S_LSHL3_ADD", "S_LSHL4_ADD"};
	for (const auto prefix: prefixes)
		if (name.starts_with(prefix)) return true;
	return false;
}

enum class SccUse { Dead, Live, PassThrough };

// How code from `start` uses the SCC it is entered with: read on some path (Live), overwritten
// on every path first (Dead), or carried to a S_SETPC_B64 return (PassThrough). Bounded and
// conservative: an unknown path counts as a read.
SccUse ScanScc(const std::vector<Instruction>& insts, size_t start) {
	const auto index_of = [&](uint32_t pc) -> std::optional<size_t> {
		const auto found = std::lower_bound(insts.begin(), insts.end(), pc,
		                                    [](const Instruction& inst, uint32_t value) {
			                                    return inst.pc < value;
		                                    });
		if (found == insts.end() || found->pc != pc) return {};
		return static_cast<size_t>(found - insts.begin());
	};
	std::vector<size_t> pending {start};
	std::set<size_t>    seen;
	bool                passes = false;
	size_t              steps  = 0;
	while (!pending.empty()) {
		size_t at = pending.back();
		pending.pop_back();
		for (;;) {
			if (at >= insts.size() || ++steps > 8192) return SccUse::Live;
			if (!seen.insert(at).second) break;
			const auto& inst = insts[at];
			if (ReadsScc(inst) || IsCall(inst.raw[0])) return SccUse::Live;
			if (WritesScc(inst) || inst.opcode == Opcode::S_ENDPGM) break;
			if (inst.opcode == Opcode::S_SETPC_B64) {
				passes = true;
				break;
			}
			if (IsDirectBranch(inst.opcode)) {
				const auto target = index_of(inst.branch_target);
				if (!target) return SccUse::Live;
				if (inst.opcode == Opcode::S_BRANCH) {
					at = *target;
					continue;
				}
				pending.push_back(*target);
			}
			++at;
		}
	}
	return passes ? SccUse::PassThrough : SccUse::Dead;
}

bool ReadFunction(uint64_t address, uint32_t return_reg, const ShaderCodeReader& read,
                  Function& function, std::string& reason, uint32_t wave_size) {
	uint32_t next            = 0;
	uint32_t furthest_target = 0;
	bool     returned        = false;
	while (next + MaxInstructionRawWords <= MaxFunctionWords) {
		while (function.code.size() < next + MaxInstructionRawWords) {
			std::array<uint32_t, 64> chunk {};
			if (!read(address + function.code.size() * 4u, chunk)) {
				reason = "shader function code is not mapped";
				return false;
			}
			function.code.insert(function.code.end(), chunk.begin(), chunk.end());
		}
		if (GetInstructionFamily(function.code[next]) == Family::Unknown) {
			reason = "shader function has an unknown instruction encoding";
			return false;
		}
		Instruction inst;
		DecodeInstruction(function.code, next, inst);
		if (IsCall(inst.raw[0]) || inst.opcode == Opcode::S_ENDPGM ||
		    inst.opcode == Opcode::UNSUPPORTED || Writes(inst, return_reg, wave_size) ||
		    Writes(inst, return_reg + 1u, wave_size)) {
			reason = "shader function needs nested calls or unsupported instructions";
			return false;
		}
		function.instructions.push_back(inst);
		next += inst.word_count;
		if (IsDirectBranch(inst.opcode)) {
			furthest_target = std::max(furthest_target, inst.branch_target);
		}
		if (inst.opcode == Opcode::S_SETPC_B64) {
			if (inst.src0.kind != OperandKind::Sgpr || inst.src0.reg != return_reg) {
				reason = "shader function returns through a different scalar pair";
				return false;
			}
			returned = true;
			if (next * 4u > furthest_target) break;
		}
	}
	if (!returned || function.instructions.back().opcode != Opcode::S_SETPC_B64) {
		reason = "shader function exceeds the bounded code range";
		return false;
	}
	function.code.resize(next);
	std::set<uint32_t> starts;
	for (const auto& inst: function.instructions)
		starts.insert(inst.pc);
	for (const auto& inst: function.instructions) {
		if (IsDirectBranch(inst.opcode) && !starts.contains(inst.branch_target)) {
			reason = "shader function branches outside its instruction range";
			return false;
		}
	}
	return true;
}

struct CallTarget {
	uint64_t address;
	Function function;
};

struct Call {
	uint32_t                source_reg;
	uint32_t                return_reg;
	bool                    dynamic;
	std::vector<CallTarget> targets;
	// Table calls: the inlined variants, each with the index of its function in `targets`.
	bool                                                specialized = false;
	std::vector<std::pair<ShaderCallVariant, size_t>> variants;
};

bool IsScalarLoad(Opcode opcode) {
	return opcode == Opcode::S_LOAD_DWORD || opcode == Opcode::S_LOAD_DWORDX2 ||
	       opcode == Opcode::S_LOAD_DWORDX4 || opcode == Opcode::S_LOAD_DWORDX8 ||
	       opcode == Opcode::S_LOAD_DWORDX16;
}

bool IsScalarBufferLoad(Opcode opcode) {
	return opcode == Opcode::S_BUFFER_LOAD_DWORD || opcode == Opcode::S_BUFFER_LOAD_DWORDX2 ||
	       opcode == Opcode::S_BUFFER_LOAD_DWORDX4 || opcode == Opcode::S_BUFFER_LOAD_DWORDX8 ||
	       opcode == Opcode::S_BUFFER_LOAD_DWORDX16;
}

std::optional<uint32_t> Immediate(const Operand& operand) {
	if (operand.kind == OperandKind::Null) return 0;
	if (operand.kind == OperandKind::LiteralConstant ||
	    operand.kind == OperandKind::IntegerInlineConstant)
		return operand.value;
	return {};
}

std::optional<uint32_t> ScalarIndex(const Operand& operand) {
	switch (operand.kind) {
		case OperandKind::Sgpr: return operand.reg;
		case OperandKind::VccLo: return 106;
		case OperandKind::VccHi: return 107;
		case OperandKind::M0: return 124;
		case OperandKind::ExecLo: return 126;
		case OperandKind::ExecHi: return 127;
		default: return {};
	}
}

void EmitMove(std::vector<uint32_t>& code, uint32_t reg, uint32_t value) {
	code.push_back(0xbe8003ffu | (reg << 16u)); // S_MOV_B32 s[reg], literal
	code.push_back(value);
}

bool EmitOriginalPc(std::vector<uint32_t>& code, const Instruction& inst, uint64_t base,
                    std::string& reason) {
	if (inst.dst.kind == OperandKind::Null) {
		code.push_back(0xbf800000u); // S_NOP retains an instruction boundary.
		return true;
	}
	const auto reg = (inst.raw[0] >> 16u) & 127u;
	if (!((inst.dst.kind == OperandKind::Sgpr && inst.dst.reg <= 105u) ||
	      inst.dst.kind == OperandKind::VccLo || inst.dst.kind == OperandKind::M0 ||
	      inst.dst.kind == OperandKind::ExecLo)) {
		reason = "shader GETPC destination is not a supported scalar pair";
		return false;
	}
	// Expansion changes instruction positions, but GETPC must still address the
	// original shader's constants/code. MOV does not change SCC, matching GETPC.
	const auto pc = base + inst.pc + 4u;
	EmitMove(code, reg, static_cast<uint32_t>(pc));
	EmitMove(code, reg + 1u, static_cast<uint32_t>(pc >> 32u));
	return true;
}

using Branches = std::vector<std::pair<uint32_t, uint32_t>>;

// marks: further word positions to keep pointing at the same instructions.
bool RelocateBranches(std::vector<uint32_t>& code, Branches& branches, std::string& reason,
                      std::vector<uint32_t>* marks = nullptr) {
	// Keep relocation symbolic until every function has been inserted. A long
	// edge uses guarded S_BRANCH islands; ordinary fallthrough skips the island.
	// Unlike GETPC/ADD/SETPC stubs, these do not clobber guest SGPRs or SCC.
	for (size_t index = 0; index < branches.size();) {
		const auto [at, target] = branches[index];
		const auto delta        = int64_t(target) - int64_t(at) - 1;
		if (delta >= std::numeric_limits<int16_t>::min() &&
		    delta <= std::numeric_limits<int16_t>::max()) {
			++index;
			continue;
		}
		if (code.size() + 2 > MaxExpandedWords || branches.size() > MaxExpandedWords) {
			reason = "expanded shader branch islands exceed the bounded code size";
			return false;
		}
		const auto desired   = delta > 0 ? at + 16384u : at - 16384u;
		uint32_t   insertion = 0;
		for (uint32_t word = 0; word <= desired && word < code.size();) {
			insertion = word;
			Instruction inst;
			DecodeInstruction(code, word, inst);
			if (inst.word_count == 0 || inst.word_count > code.size() - word) {
				reason = "expanded shader has an invalid branch-island boundary";
				return false;
			}
			word += inst.word_count;
		}
		if (insertion <= std::min(at, target) || insertion >= std::max(at, target)) {
			reason = "cannot place a branch island between expanded instructions";
			return false;
		}
		code.insert(code.begin() + insertion, {0xbf820000u, 0xbf820000u});
		for (auto& [source, destination]: branches) {
			if (source >= insertion) source += 2;
			if (destination >= insertion) destination += 2;
		}
		if (marks != nullptr) {
			for (auto& mark: *marks)
				if (mark >= insertion) mark += 2;
		}
		const auto onward      = branches[index].second;
		branches[index].second = insertion + 1u;
		branches.emplace_back(insertion, insertion + 2u); // Fallthrough bypass.
		branches.emplace_back(insertion + 1u, onward);    // Long edge continuation.
		// Insertion may lengthen an earlier edge beyond its range, so revisit it.
		index = 0;
	}
	for (const auto [at, target]: branches) {
		const auto delta = int64_t(target) - int64_t(at) - 1;
		code[at]         = (code[at] & 0xffff0000u) | (static_cast<uint32_t>(delta) & 0xffffu);
	}
	return true;
}

// The decoded shader: everything the expansion needs that depends only on its code.
struct ShaderAnalysis {
	std::vector<uint32_t> code;
	Program               program;
	std::set<uint32_t>    labels;
	bool                  has_calls = false;
	bool                  has_setpc = false;
	// ResolveCalls' reaching-definition queries depend on the code alone, so their graph and
	// answers are kept with it (one expander per pipeline cache, used from one thread).
	mutable std::vector<std::vector<size_t>>                               successors;
	mutable std::map<std::pair<uint32_t, size_t>, std::optional<size_t>> reaching;
};

void AnalyzeShader(std::span<const uint32_t> code, ShaderAnalysis& analysis) {
	analysis.code.assign(code.begin(), code.end());
	if (std::none_of(code.begin(), code.end(), IsCall)) return;
	DecodeProgram(analysis.code, analysis.program);
	for (const auto& inst: analysis.program.instructions) {
		analysis.has_calls |= IsCall(inst.raw[0]);
		if (IsDirectBranch(inst.opcode)) analysis.labels.insert(inst.branch_target);
		analysis.has_setpc |= inst.opcode == Opcode::S_SETPC_B64;
	}
}

// One call instruction and every address it can reach.
struct CallSite {
	using Variant = ShaderCallVariant;
	uint32_t              pc         = 0;
	uint32_t              source_reg = 0;
	uint32_t              return_reg = 0;
	bool                  dynamic    = false;
	std::vector<uint64_t> addresses; // ascending
	// Registers the targets were resolved through across control flow (a descriptor loaded
	// before the call's loop): a callee that wrote one would change them, so none may.
	std::vector<uint32_t> pinned; // ascending
	// A table call inlines variants: a target, and when `specialized`, the two table words the
	// call passes in the pair after the target's (GT7's material record pointer), which the
	// inlined body then sees as constants. Unspecialized variants have zero data.
	bool                 specialized = false;
	std::vector<Variant> variants;  // ascending, the candidates
	std::vector<Variant> preferred; // the reported variants among them (ascending), first
	size_t               table_targets = 0; // the variants the table holds (for the log only)

	// The table size (and a table call's distinct targets, which only the log uses) are left
	// out: a table that grows (GT7 streams materials in) while offering the same variants keeps
	// its expansion.
	bool operator==(const CallSite& other) const {
		return pc == other.pc && source_reg == other.source_reg && return_reg == other.return_reg &&
		       dynamic == other.dynamic && (dynamic || addresses == other.addresses) &&
		       pinned == other.pinned && specialized == other.specialized &&
		       variants == other.variants && preferred == other.preferred;
	}
};

// Resolves each call's targets from the user data and the memory holding call tables. Which
// registers it consults depends on the code alone; their values reach the result only through
// the targets.
bool ResolveCalls(const ShaderAnalysis& analysis, uint64_t base,
                  std::span<const uint32_t> user_data, const ShaderCodeReader& read,
                  uint32_t wave_size, std::vector<CallSite>& sites,
                  std::set<uint32_t>& dependencies, std::string& reason) {
	const auto& program = analysis.program;
	const auto& labels  = analysis.labels;
	// Instruction-level control flow, built on first use: fallthrough and direct branch edges.
	if (analysis.successors.empty() && !program.instructions.empty()) {
		std::unordered_map<uint32_t, size_t> index_of_pc;
		for (size_t i = 0; i < program.instructions.size(); i++)
			index_of_pc.emplace(program.instructions[i].pc, i);
		analysis.successors.resize(program.instructions.size());
		for (size_t i = 0; i < program.instructions.size(); i++) {
			const auto& inst = program.instructions[i];
			auto&       next = analysis.successors[i];
			if (inst.opcode == Opcode::S_ENDPGM) continue;
			if (IsDirectBranch(inst.opcode)) {
				if (const auto target = index_of_pc.find(inst.branch_target);
				    target != index_of_pc.end())
					next.push_back(target->second);
				if (inst.opcode == Opcode::S_BRANCH) continue;
			}
			if (i + 1 < program.instructions.size()) next.push_back(i + 1);
		}
	}
	// Whether `to` is reachable: from the successors of `from`, or from the entry while never
	// passing `avoid` (from == SIZE_MAX).
	const auto reaches = [&](size_t from, size_t to, size_t avoid) {
		std::vector<bool>   seen(program.instructions.size());
		std::vector<size_t> work;
		if (from == SIZE_MAX) {
			if (avoid == 0) return false;
			work.push_back(0);
		} else {
			work = analysis.successors[from];
		}
		while (!work.empty()) {
			const auto at = work.back();
			work.pop_back();
			if (at == avoid || seen[at]) continue;
			if (at == to) return true;
			seen[at] = true;
			work.insert(work.end(), analysis.successors[at].begin(), analysis.successors[at].end());
		}
		return false;
	};
	// The one write of reg whose value reaches the block starting at `first` on every path: the
	// only writer that can reach it, and one every path from the entry passes. A call inside a
	// loop reads a table descriptor loaded before the loop this way. Indirect branches (S_SETPC)
	// make reachability unknown, so they disable this.
	const auto reaching_definition = [&](uint32_t reg, size_t first) -> std::optional<size_t> {
		if (analysis.has_setpc) return {};
		const auto key = std::pair {reg, first};
		if (const auto known = analysis.reaching.find(key); known != analysis.reaching.end())
			return known->second;
		std::optional<size_t> definition;
		bool                  ambiguous = false;
		for (size_t i = 0; i < program.instructions.size() && !ambiguous; i++) {
			if (!Writes(program.instructions[i], reg, wave_size) || !reaches(i, first, SIZE_MAX))
				continue;
			ambiguous  = definition.has_value();
			definition = i;
		}
		if (ambiguous || (definition && reaches(SIZE_MAX, first, *definition))) definition.reset();
		analysis.reaching.emplace(key, definition);
		return definition;
	};
	const auto block_start = [&](size_t at) {
		while (at > 0 && !labels.contains(program.instructions[at].pc) &&
		       !IsDirectBranch(program.instructions[at - 1].opcode))
			--at;
		return at;
	};
	std::set<uint32_t> pinned; // the call site's, collected while resolving it
	const auto resolve = [&](auto&& self, uint32_t reg, size_t before, size_t first, uint32_t depth,
	                         bool load_memory = false) -> std::optional<uint32_t> {
		if (reg >= 128 || depth > 32) return {};
		const bool initial =
		    reg < user_data.size() &&
		    std::none_of(
		        program.instructions.begin(), program.instructions.begin() + before,
		        [&](const auto& inst) { return Writes(inst, reg, wave_size); });
		if (initial) {
			dependencies.insert(reg);
			return user_data[reg];
		}
		for (size_t i = before; i > first;) {
			const auto& inst = program.instructions[--i];
			if (IsDirectBranch(inst.opcode) || IsCall(inst.raw[0])) return {};
			if (Writes(inst, reg, wave_size)) {
				if (inst.opcode == Opcode::S_GETPC_B64 && inst.dst.kind == OperandKind::Sgpr) {
					const auto pc = base + inst.pc + 4u;
					return static_cast<uint32_t>(pc >> ((reg - inst.dst.reg) * 32u));
				}
				if (load_memory && IsScalarLoad(inst.opcode) &&
				    inst.src0.kind == OperandKind::Sgpr) {
					const auto low    = self(self, inst.src0.reg, i, first, depth + 1, true);
					const auto high   = self(self, inst.src0.reg + 1u, i, first, depth + 1, true);
					const auto offset = inst.src1.kind == OperandKind::Sgpr
					                        ? self(self, inst.src1.reg, i, first, depth + 1, true)
					                        : Immediate(inst.src1);
					if (!low || !high || !offset) return {};
					const uint64_t          address     = (uint64_t {*high} << 32u) | *low;
					const uint32_t          byte_offset = *offset + inst.offset;
					std::array<uint32_t, 1> value {};
					if (!read(address + byte_offset + (reg - inst.dst.reg) * 4u, value)) return {};
					return value[0];
				}
				if ((inst.opcode != Opcode::S_MOV_B32 && inst.opcode != Opcode::S_MOV_B64) ||
				    inst.dst.kind != OperandKind::Sgpr)
					return {};
				if (inst.src0.kind == OperandKind::Sgpr) {
					return self(self, inst.src0.reg + reg - inst.dst.reg, i, first, depth + 1,
					            load_memory);
				}
				if (inst.opcode == Opcode::S_MOV_B32 &&
				    (inst.src0.kind == OperandKind::LiteralConstant ||
				     inst.src0.kind == OperandKind::IntegerInlineConstant))
					return inst.src0.value;
				return {};
			}
			if (labels.contains(inst.pc) && i != first) return {};
		}
		if (first != 0) {
			if (const auto definition = reaching_definition(reg, first)) {
				pinned.insert(reg);
				return self(self, reg, *definition + 1u, block_start(*definition), depth + 1,
				            load_memory);
			}
		}
		return {};
	};
	// Follow only straight-line scalar copies back to one buffer load. The
	// runtime load and its data arguments are retained in the expanded program.
	const auto origin = [&](auto&& self, uint32_t reg, size_t before, size_t first,
	                        uint32_t depth) -> std::optional<std::pair<size_t, uint32_t>> {
		if (reg >= 105u || depth > 32) return {};
		for (size_t i = before; i > first;) {
			const auto& inst = program.instructions[--i];
			if (IsDirectBranch(inst.opcode) || IsCall(inst.raw[0])) return {};
			if (Writes(inst, reg, wave_size)) {
				if (IsScalarBufferLoad(inst.opcode)) return std::pair {i, reg - inst.dst.reg};
				if ((inst.opcode == Opcode::S_MOV_B32 || inst.opcode == Opcode::S_MOV_B64) &&
				    inst.src0.kind == OperandKind::Sgpr)
					return self(self, inst.src0.reg + reg - inst.dst.reg, i, first, depth + 1);
				return {};
			}
			if (labels.contains(inst.pc)) return {};
		}
		return {};
	};
	for (size_t i = 0; i < program.instructions.size(); i++) {
		const auto& inst = program.instructions[i];
		if (!IsCall(inst.raw[0])) continue;
		const auto src   = inst.raw[0] & 255u;
		const auto dst   = (inst.raw[0] >> 16u) & 127u;
		size_t     first = i;
		while (first > 0 && !labels.contains(program.instructions[first].pc) &&
		       !IsDirectBranch(program.instructions[first - 1].opcode))
			--first;
		pinned.clear();
		const auto low  = resolve(resolve, src, i, first, 0);
		const auto high = resolve(resolve, src + 1u, i, first, 0);
		if (src >= 105u || dst >= 105u) {
			reason = "shader call uses a special scalar register pair";
			return false;
		}
		const bool                      dynamic = !low || !high;
		std::set<uint64_t>              addresses;
		std::set<CallSite::Variant>     variants;
		bool                            specialized = false;
		if (!dynamic) {
			addresses.insert((uint64_t {*high} << 32u) | *low);
		} else {
			const auto lo = origin(origin, src, i, first, 0);
			const auto hi = origin(origin, src + 1u, i, first, 0);
			if (!lo || !hi || lo->first != hi->first || hi->second != lo->second + 1u) {
				reason = "shader call target is not a scalar buffer table load";
				return false;
			}
			const auto& load = program.instructions[lo->first];
			if (load.src0.kind != OperandKind::Sgpr) {
				reason = "shader call table descriptor is not a scalar register tuple";
				return false;
			}
			ShaderBufferResource descriptor;
			for (uint32_t word = 0; word < 4; ++word) {
				const auto value =
				    resolve(resolve, load.src0.reg + word, lo->first, first, 0, true);
				if (!value) {
					reason = "shader call table descriptor is not statically readable";
					return false;
				}
				descriptor.fields[word] = *value;
			}
			// Prove the dynamic byte offset's alignment from its last definition.
			// This bounds all possible target slots without assuming a runtime index.
			uint32_t   shift      = 0;
			const auto offset_reg = ScalarIndex(load.src1);
			for (size_t at = lo->first; at > first;) {
				const auto& def = program.instructions[--at];
				if (IsDirectBranch(def.opcode) || IsCall(def.raw[0])) break;
				const auto dst_reg  = ScalarIndex(def.dst);
				const auto dst2_reg = ScalarIndex(def.dst2);
				if (offset_reg && dst_reg && *offset_reg >= *dst_reg &&
				    *offset_reg < *dst_reg + DestinationWords(def, wave_size)) {
					if (def.opcode == Opcode::S_LSHL_B32 && *offset_reg == *dst_reg) {
						if (const auto amount = Immediate(def.src1)) shift = *amount & 31u;
					}
					break;
				}
				if (offset_reg && dst2_reg && *offset_reg >= *dst2_reg &&
				    *offset_reg < *dst2_reg + wave_size / 32u)
					break;
				if (labels.contains(def.pc)) break;
			}
			const uint64_t size = descriptor.GetSize();
			if (shift < 3u || shift > 16u || size == 0 || size > MaxCallTableBytes ||
			    descriptor.Base48() == 0 || descriptor.Type() != 0 || descriptor.SwizzleEnabled() ||
			    descriptor.AddTid() || (descriptor.Base48() & 3u) != 0 || (load.offset & 3u) != 0) {
				reason = "shader call table needs unsupported bounds or addressing";
				return false;
			}
			std::vector<uint32_t> table(size / 4u);
			if (!read(descriptor.Base48(), table)) {
				reason = "shader call table is not fully mapped";
				return false;
			}
			const uint64_t step       = uint64_t {1} << shift;
			const uint64_t first_slot = (load.offset % step) + lo->second * 4u;
			// The pair after the target's, when it holds the next two words of the same table
			// record at the call: the variants are specialized on them.
			const auto d0 = src + 3u < 106u ? origin(origin, src + 2u, i, first, 0)
			                                : std::optional<std::pair<size_t, uint32_t>> {};
			const auto d1 = src + 3u < 106u ? origin(origin, src + 3u, i, first, 0)
			                                : std::optional<std::pair<size_t, uint32_t>> {};
			specialized = d0 && d1 && d0->first == lo->first && d1->first == lo->first &&
			              d0->second == lo->second + 2u && d1->second == lo->second + 3u &&
			              step >= 16u;
			for (uint64_t offset = first_slot; offset + (specialized ? 16u : 8u) <= size;
			     offset += step) {
				const uint64_t address =
				    table[offset / 4u] | (uint64_t {table[offset / 4u + 1u]} << 32u);
				if (address == 0) continue; // A runtime null target takes the explicit trap path.
				addresses.insert(address);
				variants.insert({address,
				                 specialized ? table[offset / 4u + 2u] : 0u,
				                 specialized ? table[offset / 4u + 3u] : 0u});
				if (variants.size() > MaxTableTargets) {
					reason = "shader call table exceeds the bounded target count";
					return false;
				}
			}
		}
		CallSite site {inst.pc, src, dst, dynamic, {addresses.begin(), addresses.end()},
		               {pinned.begin(), pinned.end()}};
		site.specialized   = specialized;
		site.variants      = {variants.begin(), variants.end()};
		site.table_targets = variants.size();
		sites.push_back(std::move(site));
	}
	return true;
}

// Where an inlined table target's body sits in the expanded code (words).
struct BodyRange {
	uint32_t begin  = 0;
	uint32_t end    = 0;
	uint64_t target = 0;
};

// What an expansion left out of its table calls (for the log).
struct ExpansionStats {
	size_t table_targets = 0; // targets the tables hold
	size_t candidates    = 0; // targets offered to the expansion
	size_t inlined       = 0;
	size_t over_budget   = 0; // left out: the expanded code would exceed its bound
	size_t unreadable    = 0; // left out: the callee is not expandable (reason below)
	size_t dependency    = 0; // left out: the callee writes a call-target dependency
	size_t excluded      = 0; // left out: its body failed to translate before
	size_t callee_words  = 0;
	std::string first_unreadable;

	[[nodiscard]] bool Partial() const {
		return inlined < table_targets || over_budget != 0 || unreadable != 0 || dependency != 0 ||
		       excluded != 0;
	}
};

// Reads the callees and emits the expanded program: a function of the code, the resolved
// targets, the consulted registers and the callee code.
//
// A table call (dynamic) inlines its preferred targets first, then the rest, while the expanded
// code stays in bound; a target left out, or not expandable, takes the miss trap at run time
// instead of failing the whole shader. Each inlined target's body is emitted once: the two SCC
// selection trees enter it through a stub that restores SCC.
bool BuildExpansion(const ShaderAnalysis& analysis, uint64_t base,
                    const std::vector<CallSite>& sites, const std::set<uint32_t>& dependencies,
                    const ShaderCodeReader& read, uint32_t wave_size,
                    std::vector<uint32_t>& expanded, std::string& reason,
                    ExpansionStats* stats = nullptr, std::vector<BodyRange>* bodies = nullptr,
                    const std::set<uint64_t>* excluded = nullptr) {
	const auto&              program = analysis.program;
	ExpansionStats           local_stats;
	auto&                    st = stats != nullptr ? *stats : local_stats;
	std::map<uint32_t, Call> calls;
	const uint32_t           budget =
        MaxExpandedWords > analysis.code.size() + ExpansionSlackWords
	                  ? MaxExpandedWords - static_cast<uint32_t>(analysis.code.size()) -
	                        ExpansionSlackWords
	                  : 0u;
	uint64_t used = 0;
	const auto writes_dependency = [&](const Function& function) {
		for (const auto& inst: function.instructions)
			for (const auto reg: dependencies)
				if (Writes(inst, reg, wave_size)) return true;
		return false;
	};
	for (const auto& site: sites) {
		Call call {site.source_reg, site.return_reg, site.dynamic, {}};
		if (!site.dynamic) {
			for (const auto address: site.addresses) {
				CallTarget target {address, {}};
				if ((address & 3u) != 0 || !ReadFunction(address, site.return_reg, read,
				                                         target.function, reason, wave_size)) {
					if (reason.empty()) reason = "shader call target is not DWORD aligned";
					return false;
				}
				used += target.function.code.size() + 4u;
				call.targets.push_back(std::move(target));
			}
			calls.emplace(site.pc, std::move(call));
			continue;
		}
		st.table_targets += site.table_targets;
		st.candidates += site.variants.size();
		call.specialized = site.specialized;
		std::vector<CallSite::Variant> order = site.preferred;
		for (const auto& variant: site.variants)
			if (!std::binary_search(site.preferred.begin(), site.preferred.end(), variant))
				order.push_back(variant);
		std::map<uint64_t, std::optional<size_t>> functions; // target -> index in call.targets
		const size_t limit = site.specialized ? MaxSpecializedVariants() : MaxCallTargets;
		for (const auto& variant: order) {
			if (call.variants.size() >= limit) {
				st.over_budget++;
				continue;
			}
			if (excluded != nullptr && excluded->contains(variant.target)) {
				st.excluded++;
				continue;
			}
			auto found = functions.find(variant.target);
			if (found == functions.end()) {
				CallTarget  target {variant.target, {}};
				std::string why;
				if ((variant.target & 3u) != 0) {
					why = "shader call target is not DWORD aligned";
				} else if (!ReadFunction(variant.target, site.return_reg, read, target.function,
				                         why, wave_size)) {
					if (why.empty()) why = "shader function is not expandable";
				} else if (writes_dependency(target.function)) {
					why = "shader function modifies a call-target dependency";
				}
				std::optional<size_t> index;
				if (why.empty()) {
					index = call.targets.size();
					call.targets.push_back(std::move(target));
				} else if (st.unreadable++ == 0) {
					st.first_unreadable = why;
				}
				found = functions.emplace(variant.target, index).first;
			}
			if (!found->second) continue;
			const auto& function = call.targets[*found->second].function;
			// The body, the return-address and data moves, two SCC stubs, and the compares
			// in each selection tree.
			const uint64_t cost = function.code.size() + 8u + 3u + 2u * 9u;
			if (used + cost > budget) {
				st.over_budget++;
				continue;
			}
			used += cost;
			st.callee_words += function.code.size();
			call.variants.emplace_back(variant, *found->second);
		}
		st.inlined += call.variants.size();
		calls.emplace(site.pc, std::move(call));
	}
	if (used > budget) {
		reason = "expanded shader exceeds the bounded code size";
		return false;
	}
	// A table target left out takes the miss path, which returns as if the callee did nothing.
	// The material waterfalls do not depend on what their callee does, but a ray traversal loop
	// does (its hit functions end the traversal): a partial expansion of one hung the GPU. Such
	// a shader needs every target, or it gives up as before.
	const auto has_bvh = [](const std::vector<Instruction>& instructions) {
		return std::any_of(instructions.begin(), instructions.end(), [](const auto& inst) {
			return inst.opcode == Opcode::IMAGE_BVH_INTERSECT_RAY;
		});
	};
	if (st.Partial() && has_bvh(program.instructions)) {
		reason = "ray tracing shader with table call targets left out";
		return false;
	}
	const bool has_backedge =
	    std::any_of(program.instructions.begin(), program.instructions.end(), [](const auto& inst) {
		    return IsDirectBranch(inst.opcode) && inst.branch_target <= inst.pc;
	    });
	if (has_backedge) {
		for (const auto reg: dependencies) {
			if (std::any_of(program.instructions.begin(), program.instructions.end(),
			                [&](const auto& inst) { return Writes(inst, reg, wave_size); })) {
				reason = "shader loop may overwrite a call-target dependency";
				return false;
			}
		}
	}
	// A callee must not invalidate the user-data assumptions used to resolve later calls.
	// (Table targets that would were left out above.)
	for (const auto& [pc, call]: calls) {
		for (const auto& target: call.targets) {
			if (writes_dependency(target.function)) {
				reason = "shader function modifies a call-target dependency";
				return false;
			}
		}
	}
	std::vector<uint32_t>                      result;
	std::map<uint32_t, uint32_t>               main_pc;
	std::vector<std::pair<uint32_t, uint32_t>> main_branches;
	Branches                                   relocations;
	for (const auto& inst: program.instructions) {
		main_pc[inst.pc] = static_cast<uint32_t>(result.size());
		const auto found = calls.find(inst.pc);
		if (found == calls.end()) {
			if (inst.opcode == Opcode::S_ENDPGM) {
				result.push_back(0xbf810000u);
			} else if (inst.opcode == Opcode::S_GETPC_B64) {
				if (!EmitOriginalPc(result, inst, base, reason)) return false;
			} else {
				if (IsDirectBranch(inst.opcode))
					main_branches.emplace_back(result.size(), inst.branch_target);
				result.insert(result.end(), inst.raw, inst.raw + inst.word_count);
			}
		} else {
			const auto&           call      = found->second;
			const uint64_t        return_pc = base + inst.pc + inst.word_count * 4u;
			std::vector<uint32_t> returns;
			auto                  emit_target = [&](const CallTarget& target) -> bool {
				EmitMove(result, call.return_reg, static_cast<uint32_t>(return_pc));
				EmitMove(result, call.return_reg + 1u, static_cast<uint32_t>(return_pc >> 32u));
				std::map<uint32_t, uint32_t>               local_pc;
				std::vector<std::pair<uint32_t, uint32_t>> branches;
				for (const auto& body: target.function.instructions) {
					const auto at     = static_cast<uint32_t>(result.size());
					local_pc[body.pc] = at;
					if (body.opcode == Opcode::S_SETPC_B64) {
						returns.push_back(at);
						result.push_back(0xbf820000u); // S_BRANCH to the caller's continuation.
					} else if (body.opcode == Opcode::S_GETPC_B64) {
						if (!EmitOriginalPc(result, body, target.address, reason)) return false;
					} else {
						if (IsDirectBranch(body.opcode))
							branches.emplace_back(at, body.branch_target);
						result.insert(result.end(), body.raw, body.raw + body.word_count);
					}
				}
				for (const auto& [at, target]: branches) {
					relocations.emplace_back(at, local_pc.at(target));
				}
				return true;
			};
			if (call.dynamic) {
				// SWAPPC preserves SCC, and the selection compares overwrite it: one selection
				// tree per incoming SCC value, without borrowing a guest SGPR. Each tree's hit
				// branches to a stub that restores its SCC value and enters the variant's body,
				// emitted once. The miss trap names the target pair, so the GPU can report the
				// variant (the target and the pair after it).
				const uint32_t miss_trap =
				    0xbf920000u | ShaderCallMissTrapImmediate(call.source_reg);
				// A miss reports the variant and returns as if the callee did nothing. It must not
				// end the wave: inside the caller's waterfall loop that hung the GPU (the loop
				// never retired the missing lanes), and the call's other lanes still need it.
				const auto miss = [&]() {
					result.push_back(miss_trap);
					returns.push_back(static_cast<uint32_t>(result.size()));
					result.push_back(0xbf820000u); // S_BRANCH to the caller's continuation.
				};
				std::map<uint32_t, std::vector<size_t>> groups;
				for (size_t k = 0; k < call.variants.size(); ++k)
					groups[static_cast<uint32_t>(call.variants[k].first.target >> 32u)].push_back(k);
				std::array<std::vector<std::pair<uint32_t, size_t>>, 2> hits; // (branch, variant)
				// One tree suffices when no inlined body reads the SCC the call passed in, and
				// none carries it back to a continuation that reads it.
				bool dual = false, passes = false;
				for (const auto& entry: call.variants) {
					const auto use = ScanScc(call.targets[entry.second].function.instructions, 0);
					dual |= use == SccUse::Live;
					passes |= use == SccUse::PassThrough;
				}
				if (!dual && passes) {
					const auto at = static_cast<size_t>(&inst - program.instructions.data());
					dual = ScanScc(program.instructions, at + 1u) == SccUse::Live;
				}
				const uint32_t trees = dual ? 2u : 1u;
				const auto compare = [&](uint32_t reg, uint32_t value) {
					result.push_back(0xbf06ff00u | reg); // S_CMP_EQ_U32 s[reg], literal
					result.push_back(value);
				};
				const auto select_scc = static_cast<uint32_t>(result.size());
				if (dual) result.push_back(0xbf850000u); // S_CBRANCH_SCC1 to the SCC=1 tree.
				for (uint32_t scc = 0; scc < trees; ++scc) {
					if (scc != 0) relocations.emplace_back(select_scc, result.size());
					for (const auto& [high, members]: groups) {
						compare(call.source_reg + 1u, high);
						const auto high_miss = static_cast<uint32_t>(result.size());
						result.push_back(0xbf840000u); // S_CBRANCH_SCC0 to the next group.
						for (const auto k: members) {
							const auto& variant = call.variants[k].first;
							compare(call.source_reg, static_cast<uint32_t>(variant.target));
							if (!dual) {
								// An if/else chain with each body in place: the structurizer
								// keeps it linear (bodies placed after the chain made it
								// quadratic in the variants).
								std::vector<uint32_t> misses;
								misses.push_back(static_cast<uint32_t>(result.size()));
								result.push_back(0xbf840000u); // S_CBRANCH_SCC0 to the next.
								if (call.specialized) {
									compare(call.source_reg + 2u, variant.data0);
									misses.push_back(static_cast<uint32_t>(result.size()));
									result.push_back(0xbf840000u);
									compare(call.source_reg + 3u, variant.data1);
									misses.push_back(static_cast<uint32_t>(result.size()));
									result.push_back(0xbf840000u);
									// The values the compares proved, as constants the resource
									// tracking can follow (S_MOV leaves SCC alone).
									EmitMove(result, call.source_reg + 2u, variant.data0);
									EmitMove(result, call.source_reg + 3u, variant.data1);
								}
								const auto begin = static_cast<uint32_t>(result.size());
								if (!emit_target(call.targets[call.variants[k].second]))
									return false;
								if (bodies != nullptr)
									bodies->push_back({begin, static_cast<uint32_t>(result.size()),
									                   variant.target});
								for (const auto at: misses)
									relocations.emplace_back(at, result.size());
							} else if (call.specialized) {
								std::array<uint32_t, 2> misses {};
								misses[0] = static_cast<uint32_t>(result.size());
								result.push_back(0xbf840000u); // S_CBRANCH_SCC0 to the next.
								compare(call.source_reg + 2u, variant.data0);
								misses[1] = static_cast<uint32_t>(result.size());
								result.push_back(0xbf840000u);
								compare(call.source_reg + 3u, variant.data1);
								hits[scc].emplace_back(static_cast<uint32_t>(result.size()), k);
								result.push_back(0xbf850000u); // S_CBRANCH_SCC1 to the stub.
								for (const auto at: misses)
									relocations.emplace_back(at, result.size());
							} else {
								hits[scc].emplace_back(static_cast<uint32_t>(result.size()), k);
								result.push_back(0xbf850000u); // S_CBRANCH_SCC1 to the stub.
							}
						}
						miss();
						relocations.emplace_back(high_miss, result.size());
					}
					miss();
				}
				std::vector<std::array<uint32_t, 2>> stubs(call.variants.size());
				for (size_t k = 0; dual && k < call.variants.size(); ++k) {
					const auto& [variant, function] = call.variants[k];
					if (dual) {
						stubs[k][0] = static_cast<uint32_t>(result.size());
						result.push_back(0xbf068180u); // S_CMP_EQ_U32 0, 1: SCC = 0.
						const auto enter = static_cast<uint32_t>(result.size());
						result.push_back(0xbf820000u); // S_BRANCH to the body.
						stubs[k][1] = static_cast<uint32_t>(result.size());
						result.push_back(0xbf068080u); // S_CMP_EQ_U32 0, 0: SCC = 1.
						relocations.emplace_back(enter, result.size());
					} else {
						stubs[k][0] = static_cast<uint32_t>(result.size());
					}
					if (call.specialized) {
						// The values the compares proved, as constants the resource tracking
						// can follow (S_MOV leaves SCC alone).
						EmitMove(result, call.source_reg + 2u, variant.data0);
						EmitMove(result, call.source_reg + 3u, variant.data1);
					}
					const auto begin = static_cast<uint32_t>(result.size());
					if (!emit_target(call.targets[function])) return false;
					if (bodies != nullptr)
						bodies->push_back(
						    {begin, static_cast<uint32_t>(result.size()), variant.target});
				}
				for (uint32_t scc = 0; scc < trees; ++scc)
					for (const auto& [at, k]: hits[scc])
						relocations.emplace_back(at, stubs[k][scc]);
			} else {
				for (const auto& target: call.targets)
					if (!emit_target(target)) return false;
			}
			for (const auto at: returns)
				relocations.emplace_back(at, result.size());
		}
		if (result.size() > MaxExpandedWords) {
			reason = "expanded shader exceeds the bounded code size";
			return false;
		}
	}
	for (const auto& [at, target]: main_branches) {
		const auto found = main_pc.find(target);
		if (found == main_pc.end()) {
			reason = "shader branches outside its instruction range";
			return false;
		}
		relocations.emplace_back(at, found->second);
	}
	std::vector<uint32_t> marks;
	if (bodies != nullptr) {
		for (const auto& body: *bodies) {
			marks.push_back(body.begin);
			marks.push_back(body.end);
		}
	}
	if (!RelocateBranches(result, relocations, reason, &marks)) return false;
	if (bodies != nullptr) {
		for (size_t i = 0; i < bodies->size(); ++i) {
			(*bodies)[i].begin = marks[i * 2u];
			(*bodies)[i].end   = marks[i * 2u + 1u];
		}
	}
	expanded = std::move(result);
	return true;
}

// KYTY_CALL_LEARN=1: a specialized table over the bound inlines the variants the GPU reported.
// Off by default: every such table seen so far (GT7's race and license materials) takes new
// records every frame, so a learned variant only matches when a later frame reuses the record's
// address for another material, and those runs hung the GPU or faulted (round 13 logs: each
// device loss followed a fresh learned target; a run without any lasted four races).
bool LearnSpecializedVariants() {
	static const bool value = [] {
		const char* text = std::getenv("KYTY_CALL_LEARN");
		return text != nullptr && text[0] == '1';
	}();
	return value;
}

// Bounds the table calls: a table over MaxCallTargets offers only its reported variants (none,
// for a specialized one, unless LearnSpecializedVariants). An unspecialized site matches reports
// by target alone.
void SelectTableTargets(std::vector<CallSite>& sites,
                        const std::vector<ShaderCallVariant>* reported) {
	for (auto& site: sites) {
		if (!site.dynamic) continue;
		site.preferred.clear();
		const bool over_bound = site.variants.size() > MaxCallTargets;
		if (reported != nullptr && (!over_bound || !site.specialized || LearnSpecializedVariants())) {
			if (site.specialized) {
				std::set_intersection(site.variants.begin(), site.variants.end(),
				                      reported->begin(), reported->end(),
				                      std::back_inserter(site.preferred));
			} else {
				for (const auto& variant: *reported) {
					const ShaderCallVariant plain {variant.target, 0u, 0u};
					if (std::binary_search(site.variants.begin(), site.variants.end(), plain) &&
					    (site.preferred.empty() || site.preferred.back() != plain)) {
						site.preferred.push_back(plain);
					}
				}
			}
		}
		if (over_bound) {
			site.variants = site.preferred;
		}
	}
}

std::string DescribeStats(const ExpansionStats& st, size_t words) {
	char line[512];
	std::snprintf(line, sizeof(line),
	              "table targets=%zu offered=%zu inlined=%zu (callee words %zu, expanded words "
	              "%zu) over budget=%zu not expandable=%zu%s%s writes dependency=%zu excluded=%zu",
	              st.table_targets, st.candidates, st.inlined, st.callee_words, words,
	              st.over_budget, st.unreadable, st.first_unreadable.empty() ? "" : " first: ",
	              st.first_unreadable.c_str(), st.dependency, st.excluded);
	return line;
}

// A specialized target counts as churning once this many distinct data values were reported for
// it, over at least ChurnBatches batches (a scene that shows many materials at once reports
// them in one or two). KYTY_CALL_CHURN=<values> changes the bound; 0 never drops a target.
constexpr uint32_t DefaultChurnValues = 12;
constexpr uint32_t ChurnBatches       = 6;
uint32_t ChurnValues() {
	static const uint32_t value = [] {
		const char* text = std::getenv("KYTY_CALL_CHURN");
		if (text == nullptr || text[0] == '\0') return DefaultChurnValues;
		return static_cast<uint32_t>(std::strtoul(text, nullptr, 10));
	}();
	return value;
}

struct CallMissRegistry {
	struct Target {
		uint32_t values     = 0; // distinct data values reported
		uint32_t batches    = 0; // batches they arrived in
		uint64_t last_batch = std::numeric_limits<uint64_t>::max();
		bool     churning   = false;
	};
	struct Shader {
		std::set<ShaderCallVariant>                           pending;
		std::shared_ptr<const std::vector<ShaderCallVariant>> absorbed;
		std::chrono::steady_clock::time_point        first_pending {};
		std::map<uint64_t, Target>                   targets;
		uint64_t                                     batch = 0; // absorptions so far
		bool                                         dirty = false; // a target started churning
	};
	static constexpr size_t                    MaxTargetsPerShader = 8192;
	static constexpr std::chrono::milliseconds BatchWindow {300};
	std::mutex                                 mutex;
	std::unordered_map<uint64_t, Shader>       shaders;

	static CallMissRegistry& Get() {
		static CallMissRegistry registry;
		return registry;
	}
};

using Reads = std::vector<std::pair<uint64_t, std::vector<uint32_t>>>;

// Merges contiguous and overlapping reads into ranges (overlapping reads saw the same memory),
// so that validating them takes a few reads instead of one per 64-word chunk.
void MergeReads(Reads& reads) {
	std::sort(reads.begin(), reads.end(),
	          [](const auto& a, const auto& b) { return a.first < b.first; });
	Reads merged;
	for (auto& [address, words]: reads) {
		if (!merged.empty()) {
			auto&      last = merged.back();
			const auto end  = last.first + last.second.size() * sizeof(uint32_t);
			if (address <= end) {
				const auto skip = (end - address) / sizeof(uint32_t);
				if (skip < words.size()) {
					last.second.insert(last.second.end(),
					                   words.begin() + static_cast<std::ptrdiff_t>(skip),
					                   words.end());
				}
				continue;
			}
		}
		merged.emplace_back(address, std::move(words));
	}
	reads = std::move(merged);
}

} // namespace

bool InlineShaderFunctions(std::span<const uint32_t> code, uint64_t base,
                           std::span<const uint32_t> user_data, const ShaderCodeReader& read,
                           std::vector<uint32_t>& expanded, std::string& reason,
                           uint32_t wave_size, std::vector<uint32_t>* consulted_user_data) {
	expanded.clear();
	reason.clear();
	if (consulted_user_data != nullptr) {
		consulted_user_data->clear();
	}
	if (wave_size != 32u && wave_size != 64u) {
		reason = "shader calls require a known wave size";
		return false;
	}
	ShaderAnalysis analysis;
	AnalyzeShader(code, analysis);
	if (!analysis.has_calls) return true;
	if (analysis.has_setpc) {
		reason = "shader calls combined with indirect branches are unsupported";
		return false;
	}
	std::vector<CallSite> sites;
	std::set<uint32_t>    dependencies;
	bool ok = ResolveCalls(analysis, base, user_data, read, wave_size, sites, dependencies, reason);
	if (ok) {
		SelectTableTargets(sites, nullptr);
		ok = BuildExpansion(analysis, base, sites, dependencies, read, wave_size, expanded, reason);
	}
	if (consulted_user_data != nullptr) {
		consulted_user_data->assign(dependencies.begin(), dependencies.end());
	}
	return ok;
}

struct ShaderFunctionExpander::Impl {
	struct Expansion {
		uint64_t              base            = 0;
		uint32_t              wave_size       = 0;
		size_t                user_data_count = 0;
		std::vector<CallSite> sites;
		Reads                 reads;  // the callee code, as read
		Reads                 failed; // reads that failed: (address, requested words)
		bool                  ok = false;
		std::string           reason;
		std::vector<uint32_t> code;
		std::vector<BodyRange> bodies;
		uint64_t               exclusion_generation = 0;
	};
	struct Shader {
		ShaderAnalysis         analysis;
		std::vector<Expansion> expansions; // most recent last
		uint64_t               builds = 0;
	};
	// GT7 dispatches one material shader over several tables a frame.
	static constexpr size_t MaxExpansions = 32;

	std::unordered_map<uint64_t, Shader> shaders; // by code address
	std::vector<uint32_t>                scratch;
	// Table targets whose bodies failed to translate (ExcludeFailedCallTarget), for every shader:
	// GT7's shaders share their material functions.
	std::set<uint64_t> excluded;
	uint64_t           exclusion_generation = 0;

	bool StillReads(const Expansion& expansion, const ShaderCodeReader& read) {
		for (const auto& [address, words]: expansion.reads) {
			scratch.resize(words.size());
			if (!read(address, scratch) || scratch != words) return false;
		}
		for (const auto& [address, words]: expansion.failed) {
			scratch.resize(words.size());
			if (read(address, scratch)) return false;
		}
		return true;
	}
};

ShaderFunctionExpander::ShaderFunctionExpander(): m_impl(std::make_unique<Impl>()) {}
ShaderFunctionExpander::~ShaderFunctionExpander() = default;

bool ReportShaderCallMiss(uint64_t shader_hash, const ShaderCallVariant& target, bool* churned) {
	if (churned != nullptr) *churned = false;
	auto&           registry = CallMissRegistry::Get();
	std::lock_guard lock(registry.mutex);
	auto&           shader = registry.shaders[shader_hash];
	const bool      specialized = target.data0 != 0u || target.data1 != 0u;
	auto*           stats = specialized ? &shader.targets[target.target] : nullptr;
	if (stats != nullptr && stats->churning) return false;
	if (shader.absorbed != nullptr &&
	    std::binary_search(shader.absorbed->begin(), shader.absorbed->end(), target)) {
		return false;
	}
	const size_t known = shader.pending.size() + (shader.absorbed ? shader.absorbed->size() : 0u);
	if (known >= CallMissRegistry::MaxTargetsPerShader || shader.pending.contains(target)) {
		return false;
	}
	if (stats != nullptr) {
		stats->values++;
		if (stats->last_batch != shader.batch) {
			stats->last_batch = shader.batch;
			stats->batches++;
		}
		const auto bound = ChurnValues();
		if (bound != 0u && stats->values > bound && stats->batches >= ChurnBatches) {
			// Its learned variants are dropped at the next expansion (ShaderCallMissTargets).
			stats->churning = true;
			shader.dirty    = true;
			std::erase_if(shader.pending,
			              [&](const auto& variant) { return variant.target == target.target; });
			if (churned != nullptr) *churned = true;
			return true;
		}
	}
	if (shader.pending.empty()) shader.first_pending = std::chrono::steady_clock::now();
	shader.pending.insert(target);
	return true;
}

std::shared_ptr<const std::vector<ShaderCallVariant>> ShaderCallMissTargets(uint64_t shader_hash) {
	auto&           registry = CallMissRegistry::Get();
	std::lock_guard lock(registry.mutex);
	const auto      found = registry.shaders.find(shader_hash);
	if (found == registry.shaders.end()) return nullptr;
	auto& shader = found->second;
	if (shader.dirty ||
	    (!shader.pending.empty() &&
	     std::chrono::steady_clock::now() - shader.first_pending >= CallMissRegistry::BatchWindow)) {
		const auto churning = [&](const ShaderCallVariant& variant) {
			if (variant.data0 == 0u && variant.data1 == 0u) return false;
			const auto stats = shader.targets.find(variant.target);
			return stats != shader.targets.end() && stats->second.churning;
		};
		std::vector<ShaderCallVariant> merged;
		if (shader.absorbed != nullptr) merged = *shader.absorbed;
		merged.insert(merged.end(), shader.pending.begin(), shader.pending.end());
		std::erase_if(merged, churning);
		std::sort(merged.begin(), merged.end());
		shader.absorbed = std::make_shared<const std::vector<ShaderCallVariant>>(std::move(merged));
		shader.pending.clear();
		shader.dirty = false;
		shader.batch++;
	}
	return shader.absorbed;
}

uint64_t ShaderFunctionExpander::ExcludeFailedCallTarget(std::span<const uint32_t> expanded,
                                                         uint32_t pc) {
	const uint32_t word = pc / 4u;
	for (const auto& [address, shader]: m_impl->shaders) {
		(void)address;
		for (const auto& expansion: shader.expansions) {
			if (expansion.code.size() != expanded.size() ||
			    !std::equal(expanded.begin(), expanded.end(), expansion.code.begin()))
				continue;
			for (const auto& body: expansion.bodies) {
				if (word >= body.begin && word < body.end) {
					if (m_impl->excluded.insert(body.target).second) m_impl->exclusion_generation++;
					return body.target;
				}
			}
			return 0;
		}
	}
	return 0;
}

bool ShaderFunctionExpander::Expand(std::span<const uint32_t> code, uint64_t base,
                                    std::span<const uint32_t> user_data,
                                    const ShaderCodeReader& read, std::vector<uint32_t>& expanded,
                                    std::string& reason, uint32_t wave_size) {
	return Expand(code, base, user_data, read, expanded, reason, wave_size, 0, nullptr);
}

bool ShaderFunctionExpander::Expand(std::span<const uint32_t> code, uint64_t base,
                                    std::span<const uint32_t> user_data,
                                    const ShaderCodeReader& read, std::vector<uint32_t>& expanded,
                                    std::string& reason, uint32_t wave_size, uint64_t shader_hash,
                                    std::string* summary) {
	expanded.clear();
	reason.clear();
	if (summary != nullptr) summary->clear();
	if (wave_size != 32u && wave_size != 64u) {
		reason = "shader calls require a known wave size";
		return false;
	}
	auto& shader = m_impl->shaders[reinterpret_cast<uint64_t>(code.data())];
	if (shader.analysis.code.size() != code.size() ||
	    !std::equal(code.begin(), code.end(), shader.analysis.code.begin())) {
		shader = {};
		AnalyzeShader(code, shader.analysis);
	}
	const auto& analysis = shader.analysis;
	if (!analysis.has_calls) return true;
	if (analysis.has_setpc) {
		reason = "shader calls combined with indirect branches are unsupported";
		return false;
	}
	std::vector<CallSite> sites;
	std::set<uint32_t>    dependencies;
	if (!ResolveCalls(analysis, base, user_data, read, wave_size, sites, dependencies, reason)) {
		return false;
	}
	{
		const auto reported = shader_hash != 0 ? ShaderCallMissTargets(shader_hash) : nullptr;
		SelectTableTargets(sites, reported.get());
	}
	for (const auto& expansion: shader.expansions) {
		if (expansion.base == base && expansion.wave_size == wave_size &&
		    expansion.user_data_count == user_data.size() && expansion.sites == sites &&
		    expansion.exclusion_generation == m_impl->exclusion_generation &&
		    m_impl->StillReads(expansion, read)) {
			expanded = expansion.code;
			reason   = expansion.reason;
			return expansion.ok;
		}
	}
	Impl::Expansion expansion;
	expansion.base            = base;
	expansion.wave_size       = wave_size;
	expansion.user_data_count = user_data.size();
	expansion.sites           = std::move(sites);
	const ShaderCodeReader recording = [&](uint64_t address, std::span<uint32_t> words) {
		const bool ok = read(address, words);
		(ok ? expansion.reads : expansion.failed)
		    .emplace_back(address, std::vector<uint32_t>(words.begin(), words.end()));
		return ok;
	};
	ExpansionStats stats;
	expansion.exclusion_generation = m_impl->exclusion_generation;
	expansion.ok = BuildExpansion(analysis, base, expansion.sites, dependencies, recording,
	                              wave_size, expansion.code, expansion.reason, &stats,
	                              &expansion.bodies, &m_impl->excluded);
	const auto builds = ++shader.builds;
	if (summary != nullptr && expansion.ok && stats.Partial()) {
		*summary = DescribeStats(stats, expansion.code.size());
	}
	if (summary != nullptr && builds >= 16 && (builds & (builds - 1)) == 0) {
		// Each new expansion is a new program to translate and compile.
		*summary += (summary->empty() ? "" : "; ") + std::to_string(builds) +
		            " expansions built for this shader (call tables changing?)";
	}
	MergeReads(expansion.reads);
	expanded = expansion.code;
	reason   = expansion.reason;
	if (shader.expansions.size() >= Impl::MaxExpansions) {
		shader.expansions.erase(shader.expansions.begin());
	}
	shader.expansions.push_back(std::move(expansion));
	return shader.expansions.back().ok;
}

} // namespace Libs::Graphics::ShaderRecompiler::Decoder
