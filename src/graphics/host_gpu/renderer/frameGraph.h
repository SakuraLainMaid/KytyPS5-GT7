#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_FRAMEGRAPH_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_FRAMEGRAPH_H_

#include "common/common.h"

#include <atomic>
#include <cstdint>

namespace Libs::Graphics {
class BufferCache;
} // namespace Libs::Graphics

// Diagnostics: a capture of one whole guest frame as the GPU thread runs it. F9 (or
// KYTY_FRAME_GRAPH=<frame>[,<frame>...], counted in guest frame ends) logs the next frame: every
// draw, dispatch and DMA in order, with its shaders, render targets, textures (and their cache
// states), buffers and user data, and the work the emulator skipped or replaced, then the
// contents of the small buffers the frame's dispatches and full-screen draws read (after the
// frame, with the GPU's writes downloaded). Every line starts "FG ". The shaders' guest code goes
// to shader-dumps/<stage>_<hash>.bin, with a .txt beside it.
namespace Libs::Graphics::FrameGraph {

// Any thread: capture the next frame.
void RequestCapture();
// GPU thread, at each guest frame end (a suspend point): starts and ends captures.
void OnFrameBoundary(BufferCache& cache);

extern std::atomic<bool> g_active;
extern std::atomic<bool> g_logging;
// A capture runs (the GPU thread is in the captured frame).
[[nodiscard]] inline bool Active() {
	return g_active.load(std::memory_order_relaxed);
}
// A capture runs and still logs (it stops at a line limit).
[[nodiscard]] inline bool Logging() {
	return g_logging.load(std::memory_order_relaxed);
}

// The capture's lines. BeginOp starts a numbered operation (a draw, a dispatch, ...); Log adds a
// line to the current one.
uint32_t               BeginOp();
[[nodiscard]] uint32_t CurrentOp();
void                   Log(const char* format, ...) KYTY_FORMAT_PRINTF(1, 2);

// Buffer contents shown after the frame: the first KeepBytes of each.
inline constexpr uint64_t KeepBytes = 1024;
void                      NoteBuffer(uint64_t address, uint64_t size);
// Whether the current operation's buffers are shown (dispatches and full-screen draws).
void               SetShowBuffers(bool show);
[[nodiscard]] bool ShowBuffers();
// A small linear image the GPU writes, the kind games read back on the CPU: its guest memory after
// the frame, up to ReadbackBytes, once the read-backs (TextureCache::ReadsBackForCpu) landed;
// read_back says whether this one is read back (if not, the CPU sees stale bytes).
inline constexpr uint64_t ReadbackBytes = 8192;
void                      NoteReadback(uint64_t address, uint64_t size, bool read_back);

// A shader the frame runs: its guest code is dumped once per capture.
void NoteShader(const char* stage, uint64_t hash, uint64_t code_address, const uint32_t* user_data,
                uint32_t user_data_count);

} // namespace Libs::Graphics::FrameGraph

namespace Libs::Graphics {
// The size of the guest code registered for the shader at this address (0 when unknown).
[[nodiscard]] uint32_t ShaderCodeSizeBytes(uint64_t shader_addr);
} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_FRAMEGRAPH_H_
