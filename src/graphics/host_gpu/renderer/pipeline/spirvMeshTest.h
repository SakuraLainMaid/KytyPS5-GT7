#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SPIRVMESHTEST_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SPIRVMESHTEST_H_

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Libs::Graphics {

// Round 148: KYTY_MESH_TEST="<name>[,<name>...]", a test for AMD's Windows driver, whose shader
// compiler crashed making every mesh-shader pipeline (issues 12 and 21). Each mesh shader's module
// is made from a copy with one part taken out, to find the part the driver fails on:
//   empty       the copy's entry point only sets 0 vertices and 0 primitives: nothing is drawn
//   noguest     the entry point does not call the translated guest code (it writes what it has)
//   nolayer     no Layer output
//   nocull      no CullPrimitiveEXT output: primitives the guest left out are drawn
//   fixedcount  the output counts are the declared maximums, not the ones the guest set
// The copies change what is drawn; they test whether the driver compiles them. The shader cache
// is unchanged.
enum MeshTestFlag : uint32_t {
	MeshTestEmpty      = 1u << 0u,
	MeshTestNoGuest    = 1u << 1u,
	MeshTestNoLayer    = 1u << 2u,
	MeshTestNoCull     = 1u << 3u,
	MeshTestFixedCount = 1u << 4u,
};

// KYTY_MESH_TEST's flags, read once (and logged); 0 when it is not set.
[[nodiscard]] uint32_t    MeshTestSetting();
[[nodiscard]] std::string MeshTestNames(uint32_t flags);

// Round 151: AMD's Windows driver crashed making every pipeline whose mesh shader writes the
// per-primitive Layer output (an RX 6950 XT, drivers 26.5.1 and 26.10.1; issue 21). Of
// KYTY_MESH_TEST's copies only those without the Layer output compiled, and with them the game's
// text drew. So on an AMD GPU the mesh shaders' modules leave Layer out: a draw into a target of
// one layer draws into it the same. KYTY_MESH_LAYER=keep keeps it there, =drop leaves it out on
// any GPU. Called once with the device's vendor before any shader is compiled.
void SetMeshHost(uint32_t vendor_id);
// The parts every mesh shader's module leaves out on this host (MeshTestNoLayer or none).
[[nodiscard]] uint32_t MeshHostFlags();

// The copy of a mesh shader's SPIR-V with the flags' parts taken out; empty when there is
// nothing to take out or the module is not one it understands.
[[nodiscard]] std::vector<uint32_t> SpirvMeshTest(std::span<const uint32_t> spirv, uint32_t flags);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SPIRVMESHTEST_H_
