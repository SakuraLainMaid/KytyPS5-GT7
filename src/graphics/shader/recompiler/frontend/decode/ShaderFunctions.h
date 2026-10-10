#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERFUNCTIONS_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERFUNCTIONS_H_

#include <compare>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::Decoder {

using ShaderCodeReader = std::function<bool(uint64_t, std::span<uint32_t>)>;

// The s_trap code the expansion emits for a call target outside the captured set. Guest
// s_trap instructions are no-ops, as on retail hardware; only this code reports a trap.
constexpr uint32_t ShaderCallMissTrapCode = 0x7fu;

// The expansion's miss trap names the scalar pair holding the missed target in the upper byte
// of its immediate: simm16 = ShaderCallMissTrapCode | ((target_reg + 1) << 8). Zero there means
// the trap does not name one. The trap reports s[target_reg .. target_reg + 3]: the target and
// the pair after it (a specialized variant's data).
constexpr uint32_t ShaderCallMissTrapImmediate(uint32_t target_reg) {
	return ShaderCallMissTrapCode | (((target_reg + 1u) & 0xffu) << 8u);
}

// Call-miss feedback. A table can hold more targets than one expansion can inline (GT7's material
// tables hold hundreds), so a table call over the bound inlines the targets the GPU reported
// missing (ReportShaderCallMiss, from the fault buffer) and traps on the rest. Reports gather for
// a short while before the next expansion takes them, to batch the recompiles.
// Returns true the first time the target is reported for the shader.
// A table call target, with the two table words the call passes along when the expansion
// specializes on them (zero otherwise).
struct ShaderCallVariant {
	uint64_t target = 0;
	uint32_t data0  = 0;
	uint32_t data1  = 0;
	auto     operator<=>(const ShaderCallVariant&) const = default;
};
bool ReportShaderCallMiss(uint64_t shader_hash, const ShaderCallVariant& variant);
// The variants reported for the shader so far (ascending), or null.
std::shared_ptr<const std::vector<ShaderCallVariant>> ShaderCallMissTargets(uint64_t shader_hash);

// Expands scalar calls through immutable pointers or bounded scalar-buffer tables.
// Table calls retain runtime selection and trap on targets outside the captured set.
// Empty output means no calls; unresolved calls never discard guest instructions.
// consulted_user_data, when given, receives the user-data registers the result depends on;
// together with the code and the memory read through `read`, they determine the result.
bool InlineShaderFunctions(std::span<const uint32_t> code, uint64_t base,
                           std::span<const uint32_t> user_data, const ShaderCodeReader& read,
                           std::vector<uint32_t>& expanded, std::string& reason,
                           uint32_t wave_size = 64,
                           std::vector<uint32_t>* consulted_user_data = nullptr);

// InlineShaderFunctions for a caller that expands the same shaders over and over (the pipeline
// cache, once per dispatch). The decoded shader is kept while its code is unchanged, and an
// expansion while the call targets resolve to the same addresses and the callee code reads the
// same; only the target resolution runs every time. The results equal InlineShaderFunctions'.
class ShaderFunctionExpander {
public:
	ShaderFunctionExpander();
	~ShaderFunctionExpander();
	ShaderFunctionExpander(const ShaderFunctionExpander&)            = delete;
	ShaderFunctionExpander& operator=(const ShaderFunctionExpander&) = delete;

	bool Expand(std::span<const uint32_t> code, uint64_t base, std::span<const uint32_t> user_data,
	            const ShaderCodeReader& read, std::vector<uint32_t>& expanded, std::string& reason,
	            uint32_t wave_size);
	// With the shader's hash: table calls over the bound inline the targets reported missing for
	// it (ShaderCallMissTargets). summary, when given, receives a line about a new expansion that
	// leaves table targets out (empty otherwise).
	bool Expand(std::span<const uint32_t> code, uint64_t base, std::span<const uint32_t> user_data,
	            const ShaderCodeReader& read, std::vector<uint32_t>& expanded, std::string& reason,
	            uint32_t wave_size, uint64_t shader_hash, std::string* summary);

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace Libs::Graphics::ShaderRecompiler::Decoder

#endif
