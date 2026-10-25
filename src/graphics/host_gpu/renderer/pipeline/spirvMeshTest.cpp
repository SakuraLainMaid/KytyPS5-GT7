#include "graphics/host_gpu/renderer/pipeline/spirvMeshTest.h"

#include "common/logging/log.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <spirv/unified1/spirv.hpp>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace Libs::Graphics {
namespace {

struct MeshTestName {
	std::string_view name;
	uint32_t         flag;
};

constexpr std::array<MeshTestName, 5> MeshTestNameList {{
    {"empty", MeshTestEmpty},
    {"noguest", MeshTestNoGuest},
    {"nolayer", MeshTestNoLayer},
    {"nocull", MeshTestNoCull},
    {"fixedcount", MeshTestFixedCount},
}};

struct Instruction {
	uint32_t offset;
	uint32_t words;
	spv::Op  op;
};

// The first interface word of an OpEntryPoint: the one after its name, whose last word holds
// the terminating nul.
uint32_t EntryPointInterfaceStart(std::span<const uint32_t> spirv, const Instruction& entry) {
	for (uint32_t word = 3; word < entry.words; word++) {
		const uint32_t value = spirv[entry.offset + word];
		if ((value & 0xffu) == 0 || (value & 0xff00u) == 0 || (value & 0xff0000u) == 0 ||
		    (value & 0xff000000u) == 0) {
			return word + 1;
		}
	}
	return entry.words;
}

bool IsAccessChain(spv::Op op) {
	return op == spv::OpAccessChain || op == spv::OpInBoundsAccessChain ||
	       op == spv::OpPtrAccessChain || op == spv::OpInBoundsPtrAccessChain;
}

std::atomic<uint32_t> g_mesh_host_flags {0};

} // namespace

void SetMeshHost(uint32_t vendor_id) {
	constexpr uint32_t     AmdVendor = 0x1002u;
	const char*            value     = std::getenv("KYTY_MESH_LAYER");
	const std::string_view setting(value != nullptr ? value : "");
	const bool drop = setting == "drop" || (setting != "keep" && vendor_id == AmdVendor);
	g_mesh_host_flags.store(drop ? static_cast<uint32_t>(MeshTestNoLayer) : 0u,
	                        std::memory_order_relaxed);
	const char* why =
	    setting == "drop"   ? "KYTY_MESH_LAYER=drop"
	    : setting == "keep" ? "KYTY_MESH_LAYER=keep"
	    : drop ? "an AMD GPU, whose driver crashed on it (issue 21); KYTY_MESH_LAYER=keep "
	             "keeps it"
	           : "KYTY_MESH_LAYER=drop leaves it out";
	LOGF("Mesh shaders: their Layer output is %s (%s)\n", drop ? "left out" : "kept", why);
}

uint32_t MeshHostFlags() {
	return g_mesh_host_flags.load(std::memory_order_relaxed);
}

uint32_t MeshTestSetting() {
	static const uint32_t flags = [] {
		const char* value = std::getenv("KYTY_MESH_TEST");
		if (value == nullptr || *value == '\0') {
			return 0u;
		}
		uint32_t         result = 0;
		std::string_view list(value);
		while (!list.empty()) {
			const auto comma = list.find(',');
			const auto name  = list.substr(0, comma);
			bool       known = false;
			for (const auto& entry: MeshTestNameList) {
				if (entry.name == name) {
					result |= entry.flag;
					known = true;
				}
			}
			if (!known && !name.empty()) {
				LOGF(
				    "KYTY_MESH_TEST: \"%.*s\" is not a test name (empty, noguest, nolayer, nocull, "
				    "fixedcount)\n",
				    static_cast<int>(name.size()), name.data());
			}
			list = comma == std::string_view::npos ? std::string_view() : list.substr(comma + 1);
		}
		LOGF("Mesh shaders: modules made from test copies (KYTY_MESH_TEST=%s): %s\n", value,
		     result != 0 ? MeshTestNames(result).c_str() : "none of the names is known, so off");
		return result;
	}();
	return flags;
}

std::string MeshTestNames(uint32_t flags) {
	std::string names;
	for (const auto& entry: MeshTestNameList) {
		if ((flags & entry.flag) != 0) {
			if (!names.empty()) {
				names += ',';
			}
			names += entry.name;
		}
	}
	return names;
}

std::vector<uint32_t> SpirvMeshTest(std::span<const uint32_t> spirv, uint32_t flags) {
	constexpr uint32_t HeaderWords = 5;
	if (flags == 0 || spirv.size() < HeaderWords || spirv[0] != spv::MagicNumber) {
		return {};
	}
	std::vector<Instruction> code;
	for (size_t offset = HeaderWords; offset < spirv.size();) {
		const uint32_t words = spirv[offset] >> 16u;
		if (words == 0 || offset + words > spirv.size()) {
			return {};
		}
		code.push_back(
		    {static_cast<uint32_t>(offset), words, static_cast<spv::Op>(spirv[offset] & 0xffffu)});
		offset += words;
	}

	// The mesh entry point, its declared output counts, the outputs to take out and a 32-bit
	// unsigned integer type for the counts.
	const Instruction*           entry             = nullptr;
	uint32_t                     main_id           = 0;
	uint32_t                     output_vertices   = 0;
	uint32_t                     output_primitives = 0;
	uint32_t                     uint_type         = 0;
	std::unordered_set<uint32_t> removed_variables;
	for (const auto& inst: code) {
		const auto* w = &spirv[inst.offset];
		switch (inst.op) {
			case spv::OpEntryPoint:
				if (inst.words >= 4 && w[1] == spv::ExecutionModelMeshEXT && entry == nullptr) {
					entry   = &inst;
					main_id = w[2];
				}
				break;
			case spv::OpExecutionMode:
				if (inst.words >= 4 && main_id != 0 && w[1] == main_id) {
					if (w[2] == spv::ExecutionModeOutputVertices) {
						output_vertices = w[3];
					} else if (w[2] == spv::ExecutionModeOutputPrimitivesEXT) {
						output_primitives = w[3];
					}
				}
				break;
			case spv::OpDecorate:
				if (inst.words >= 4 && w[2] == spv::DecorationBuiltIn &&
				    (((flags & MeshTestNoLayer) != 0 && w[3] == spv::BuiltInLayer) ||
				     ((flags & MeshTestNoCull) != 0 && w[3] == spv::BuiltInCullPrimitiveEXT))) {
					removed_variables.insert(w[1]);
				}
				break;
			case spv::OpTypeInt:
				if (inst.words >= 4 && w[2] == 32 && w[3] == 0 && uint_type == 0) {
					uint_type = w[1];
				}
				break;
			default: break;
		}
	}
	if (entry == nullptr) {
		return {};
	}
	std::unordered_map<uint32_t, uint32_t> uint_constants; // value -> id
	size_t                                 main_begin = code.size();
	size_t                                 main_end   = code.size();
	for (size_t index = 0; index < code.size(); index++) {
		const auto& inst = code[index];
		const auto* w    = &spirv[inst.offset];
		if (inst.op == spv::OpConstant && inst.words == 4 && w[1] == uint_type && uint_type != 0) {
			uint_constants.try_emplace(w[3], w[2]);
		} else if (inst.op == spv::OpFunction && inst.words >= 5 && w[2] == main_id) {
			main_begin = index;
		} else if (inst.op == spv::OpFunctionEnd && main_begin < code.size() &&
		           main_end == code.size()) {
			main_end = index;
		}
	}
	if (main_end == code.size()) {
		return {};
	}
	const auto constant = [&](uint32_t value) {
		const auto found = uint_constants.find(value);
		return found != uint_constants.end() ? found->second : 0u;
	};

	// The pointers into the outputs taken out, and the calls the entry point makes.
	std::unordered_set<uint32_t> removed_ids = removed_variables;
	for (size_t index = 0; index < code.size(); index++) {
		const auto& inst = code[index];
		const auto* w    = &spirv[inst.offset];
		if (IsAccessChain(inst.op) && inst.words >= 4 && removed_ids.contains(w[3])) {
			removed_ids.insert(w[2]);
		} else if ((flags & MeshTestNoGuest) != 0 && inst.op == spv::OpFunctionCall &&
		           inst.words >= 4 && index > main_begin && index < main_end) {
			removed_ids.insert(w[2]);
		}
	}

	const bool empty      = (flags & MeshTestEmpty) != 0;
	uint32_t   bound      = spirv[3];
	const auto zero       = constant(0);
	const auto fixed_vtx  = constant(output_vertices);
	const auto fixed_prim = constant(output_primitives);
	if (empty && zero == 0) {
		return {};
	}
	uint32_t main_type          = 0;
	uint32_t main_function_type = 0;
	bool     changed            = false;

	std::vector<uint32_t> result;
	result.reserve(spirv.size() + 16);
	result.insert(result.end(), spirv.begin(), spirv.begin() + HeaderWords);
	for (size_t index = 0; index < code.size(); index++) {
		const auto& inst = code[index];
		const auto* w    = &spirv[inst.offset];
		const auto  copy = [&] { result.insert(result.end(), w, w + inst.words); };
		switch (inst.op) {
			case spv::OpName:
			case spv::OpDecorate:
			case spv::OpDecorateId:
			case spv::OpDecorateString:
				if (inst.words >= 2 && removed_ids.contains(w[1])) {
					changed = true;
					continue;
				}
				break;
			case spv::OpVariable:
				if (inst.words >= 3 && removed_ids.contains(w[2])) {
					changed = true;
					continue;
				}
				break;
			case spv::OpAccessChain:
			case spv::OpInBoundsAccessChain:
			case spv::OpPtrAccessChain:
			case spv::OpInBoundsPtrAccessChain:
			case spv::OpFunctionCall:
				if (inst.words >= 3 && removed_ids.contains(w[2])) {
					changed = true;
					continue;
				}
				break;
			case spv::OpStore:
				if (inst.words >= 3 && removed_ids.contains(w[1])) {
					changed = true;
					continue;
				}
				break;
			case spv::OpEntryPoint:
				if (&inst == entry && !removed_variables.empty()) {
					const auto start = EntryPointInterfaceStart(spirv, inst);
					const auto first = result.size();
					result.insert(result.end(), w, w + start);
					for (uint32_t word = start; word < inst.words; word++) {
						if (!removed_variables.contains(w[word])) {
							result.push_back(w[word]);
						}
					}
					const auto words = static_cast<uint32_t>(result.size() - first);
					result[first]    = (words << 16u) | static_cast<uint32_t>(spv::OpEntryPoint);
					changed          = changed || words != inst.words;
					continue;
				}
				break;
			case spv::OpFunction:
				if (empty && index == main_begin) {
					// The guest's entry point stays as a function nothing calls; the new one
					// takes its id, which the entry point and execution modes name.
					main_type          = w[1];
					main_function_type = w[4];
					result.insert(result.end(), {w[0], w[1], bound++, w[3], w[4]});
					changed = true;
					continue;
				}
				break;
			case spv::OpSetMeshOutputsEXT:
				if ((flags & MeshTestFixedCount) != 0 && index > main_begin && index < main_end &&
				    inst.words == 3 && fixed_vtx != 0 && fixed_prim != 0) {
					result.insert(result.end(), {w[0], fixed_vtx, fixed_prim});
					changed = true;
					continue;
				}
				break;
			default: break;
		}
		copy();
	}
	if (empty) {
		const uint32_t label = bound++;
		result.insert(result.end(),
		              {(5u << 16u) | static_cast<uint32_t>(spv::OpFunction), main_type, main_id,
		               static_cast<uint32_t>(spv::FunctionControlMaskNone), main_function_type,
		               (2u << 16u) | static_cast<uint32_t>(spv::OpLabel), label,
		               (3u << 16u) | static_cast<uint32_t>(spv::OpSetMeshOutputsEXT), zero, zero,
		               (1u << 16u) | static_cast<uint32_t>(spv::OpReturn),
		               (1u << 16u) | static_cast<uint32_t>(spv::OpFunctionEnd)});
	}
	if (!changed) {
		return {};
	}
	result[3] = bound;
	return result;
}

} // namespace Libs::Graphics
