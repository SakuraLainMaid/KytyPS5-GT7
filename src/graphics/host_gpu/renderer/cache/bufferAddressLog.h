#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_BUFFERADDRESSLOG_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_BUFFERADDRESSLOG_H_

#include <cstdint>
#include <string>

// Round 111, device-fault diagnostics: the device address ranges of the buffers that have one,
// live and the latest destroyed, so a device fault's address can be named (DumpDeviceFault). A
// GPU that read an unmapped address in a session (issue 3, a license test's retry prompt) left only
// the address.
namespace Libs::Graphics::BufferAddressLog {

// Whether buffers are still noted: Stop() at shutdown, once the scheduler a buffer's destructor
// would ask for its ticks may be gone.
[[nodiscard]] bool Recording() noexcept;
void               Stop() noexcept;

void NoteCreated(uint64_t device_address, uint64_t size, uint64_t guest_address, uint32_t usage,
                 uint64_t tick);
// `tick`: the scheduler's current tick; `gpu_tick`: the last one the GPU had finished.
void NoteDestroyed(uint64_t device_address, uint64_t tick, uint64_t gpu_tick);
// What held [address, address + precision): a live buffer, one destroyed (how long ago, at which
// tick), or none, with the nearest live buffers around it.
[[nodiscard]] std::string Describe(uint64_t address, uint64_t precision);

} // namespace Libs::Graphics::BufferAddressLog

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_BUFFERADDRESSLOG_H_
