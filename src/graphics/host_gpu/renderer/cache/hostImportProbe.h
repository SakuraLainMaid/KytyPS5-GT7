#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_HOSTIMPORTPROBE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_HOSTIMPORTPROBE_H_

#include <cstdint>

namespace Libs::Graphics {
struct GraphicContext;
}

// KYTY_HOST_IMPORT_PROBE=1 (round 116): whether the graphics card can read and write the game's
// memory where it lies, in system memory (VK_EXT_external_memory_host), instead of through copies
// in its own memory, and how fast. The game's CPU writes to the memory its GPU buffers lie in (a
// few tens of MB a frame) are what the write faults, the buffer synchronizations, the uploads and
// the walks for memory-reading shaders exist for; buffers the card reads in place would need none
// of them. The probe imports 64 MB of memory of the emulator's own and up to 64 MB of a game
// buffer's memory, copies them on the GPU (timed, against a copy in the card's memory), compares
// the bytes, and checks that the CPU sees the GPU's writes into its own memory (never the game's).
// Results: the "Host import probe:" lines. Nothing else changes.
namespace Libs::Graphics::HostImportProbe {

// Whether KYTY_HOST_IMPORT_PROBE=1 asks for the probe (the device layer then enables
// VK_EXT_external_memory_host when the device has it).
[[nodiscard]] bool Requested();

// GPU thread, at a new game buffer: the first one of 16 MB or more whose memory one guest mapping
// holds is probed, once.
void MaybeRun(GraphicContext& graphics, uint64_t vaddr, uint64_t size);

} // namespace Libs::Graphics::HostImportProbe

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_HOSTIMPORTPROBE_H_
