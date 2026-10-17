#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_FAULTPAGES_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_FAULTPAGES_H_

#include <cstddef>
#include <cstdint>

// Which pages the game's faults hit, frame by frame, while KYTY_SAMPLE's statistics run (round
// 74): every 10 s a "Fault pages:" line says how many pages a frame write-fault, how they spread
// over 64 KB blocks, how many fault frame after frame, why a fault did not release its whole
// block, and what the hot pages (Bisect::HotPages) did.
namespace Libs::Graphics::FaultPages {

enum class Outcome : uint8_t {
	Released,      // a write fault that released its whole block
	NotRecent,     // a write fault whose block had not faulted in the last 100 ms
	AddressWrites, // a write fault while shaders store through addresses (no block is released)
	BlockUnmapped, // a write fault in a block that is not all GPU-mapped
	ImageInBlock,  // a write fault in a block an image lies in
	GpuBytes,      // a write fault in a block that holds GPU-written bytes
	BlocksOff,     // a write fault with block release off
	Read,          // a read fault (on memory the GPU wrote)
	Count
};

// GPU thread: starts the reporter once KYTY_SAMPLE's statistics run (until then Note does nothing).
void StartIfActive();
// Any thread (a fault handler): the fault at vaddr, in the frame after `frame` presented ones.
void Note(uint64_t vaddr, uint64_t frame, Outcome outcome) noexcept;

// The hot pages: `pages` became hot (a fault handler); a refresh made `pages` CPU-dirty again, of
// `hot` hot pages (GPU thread); every page stopped being hot (GPU thread).
void NoteHotMarked(size_t pages) noexcept;
void NoteHotRefresh(size_t pages, size_t hot) noexcept;
void NoteHotCleared() noexcept;

// Before guest memory goes: stops (joins) the reporter.
void Stop();

} // namespace Libs::Graphics::FaultPages

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_FAULTPAGES_H_
