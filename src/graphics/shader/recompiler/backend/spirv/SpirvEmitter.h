#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SPIRVEMITTER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SPIRVEMITTER_H_

#include "common/common.h"
#include "common/stringUtils.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <vector>

namespace Libs::Graphics::ShaderRecompiler::Spirv {

std::vector<uint32_t> EmitProgram(const IR::Program& program,
                                  ShaderStageInputInfo input_info);

// OpName is emitted with graphics debug dumps, or always once this is set (tests find variables
// and functions by name).
void SetEmitNames(bool enabled);

// Optional image features of the device, set once by the device layer.
struct HostImageFeatures {
	// shaderResourceMinLod: the MinLod image operand, used for IMAGE_SAMPLE*_CL.
	bool min_lod = false;
};

void              SetHostImageFeatures(const HostImageFeatures& features);
HostImageFeatures GetHostImageFeatures();

// Optional floating-point controls of the device, set once by the device layer.
struct HostFloatFeatures {
	// shaderDenormFlushToZeroFloat32, settable apart from the other widths: a compute shader whose
	// float mode flushes 32-bit denormals (FLOAT_MODE bits 4 and 5 clear) declares
	// DenormFlushToZero for 32 bits, and its legacy MAD/MAC need no explicit flushes.
	bool denorm_flush_f32 = false;
};

void              SetHostFloatFeatures(const HostFloatFeatures& features);
HostFloatFeatures GetHostFloatFeatures();

// S_MEMREALTIME counts at 100 MHz. The emitter reads the device clock (OpReadClockKHR, device
// scope) and shifts it right by this many bits to approach that rate. The device layer sets it
// from the device's clock rate: 3 for a 1 GHz clock (NVIDIA, the default), 0 for a 100 MHz one
// (AMD).
void     SetDeviceClockShift(uint32_t shift);
uint32_t GetDeviceClockShift();

// A coherent (GLC) buffer load is followed by a device-scope acquire barrier, so that a load
// polling for another workgroup's write sees it. Without one, AMD's drivers keep serving the
// cached value (BufferWorkgroupPublication, ISSUES.md #16); NVIDIA reloads anyway. The device
// layer enables it on every vendor but NVIDIA; KYTY_COHERENT_LOAD_ACQUIRE=0/1 overrides that.
void SetCoherentLoadAcquire(bool enabled);
bool GetCoherentLoadAcquire();
// Round 116: how a coherent buffer load is made. Barrier: a load and the acquire barrier above.
// Acquire: one atomic load at device scope with acquire semantics (the load reads the device's
// copy, and later loads are ordered after it), cheaper than the barrier. Relaxed: one atomic load
// at device scope, unordered. Plain: a volatile load. KYTY_COHERENT_LOAD_MODE=barrier, acquire,
// relaxed or plain picks one; without it, Barrier when the acquire barrier is on
// (KYTY_COHERENT_LOAD_ACQUIRE or the device's default), else Plain. On an RTX 5090 a plain load
// kept reading what other workgroups had replaced (ray tracing mode's car interiors blotchy, the
// wheels glowing white), while an RTX 3090 reloads it anyway.
enum class CoherentLoadMode : uint32_t { Plain = 0, Barrier = 1, Acquire = 2, Relaxed = 3 };
CoherentLoadMode          GetCoherentLoadMode();
[[nodiscard]] const char* CoherentLoadModeName(CoherentLoadMode mode);

// Why a mesh program cannot run in passes (ShaderMeshInputInfo::passes), or nullptr.
[[nodiscard]] const char* MeshPassesUnsupported(const IR::Program& program);

} // namespace Libs::Graphics::ShaderRecompiler::Spirv

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SPIRVEMITTER_H_ */
