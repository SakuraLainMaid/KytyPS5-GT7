#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_FRAMEGRAPH_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_FRAMEGRAPH_H_

#include "common/common.h"

#include <atomic>
#include <cstdint>
#include <string>

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
// GPU thread: the guest frames it has ended (suspend points it has processed).
[[nodiscard]] uint64_t GpuThreadFrames();

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
// The capture running (or the last one): counted from 1.
[[nodiscard]] uint32_t CaptureNumber();
void                   Log(const char* format, ...) KYTY_FORMAT_PRINTF(1, 2);

// Round 90: a hash of guest bytes as they are now, their first HashBytes ("-" when unreadable),
// with " [G]" when the GPU wrote some of them (guest memory then lacks what it wrote): what a draw
// reads, to compare two draws over the same data.
inline constexpr uint64_t HashBytes = 256ull << 10u;
[[nodiscard]] std::string BytesHash(BufferCache& cache, uint64_t address, uint64_t size);

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

// A light grid (an R32G32_UINT array texture) a shader reads: whether to compare it with its guest
// bytes (TextureCache::CompareWithGuest), the first time each address is bound in a capture, up to
// MaxGridChecks per capture.
inline constexpr size_t MaxGridChecks = 12;
[[nodiscard]] bool      ClaimGridCheck(uint64_t address);

// Verification (KYTY_FG_VERIFY, default 1; 0 turns it off): the first time a capture binds a
// texture or a buffer that the GPU did not write, the GPU's copy of it (as the commands recorded so
// far leave it) is compared with its guest bytes, which the CPU wrote and which are the truth for
// it; a difference is a stale copy ("FG <op> stale ..."). An array texture of six or more layers,
// written on the GPU or not, also gets a line of its layers' contents ("FG <op> layers ...").
// Up to KYTY_FG_VERIFY_MB (default 3072) of buffer downloads per capture,
// KYTY_FG_VERIFY_TEXTURES_MB (1536) of textures and KYTY_FG_VERIFY_LAYERS_MB (2048) of GPU-written
// arrays, each up to MaxVerifyBytes.
inline constexpr uint64_t MaxVerifyBytes = 256ull << 20u;
[[nodiscard]] bool        VerifyEnabled();
// Whether to verify this buffer range now: the first time in the capture, within the budget.
[[nodiscard]] bool ClaimVerify(uint64_t address, uint64_t size);
// The same for a texture the CPU wrote, within a budget of its own, KYTY_FG_VERIFY_TEXTURES_MB
// (default 1536): every one is downloaded whole, and its copy is held until the GPU is done.
[[nodiscard]] bool ClaimVerifyTexture(uint64_t address, uint64_t size);
// The same for an array the GPU wrote (its layers' contents are shown), within a budget of its
// own, KYTY_FG_VERIFY_LAYERS_MB (default 2048): the textures the CPU wrote do not use it up.
[[nodiscard]] bool ClaimVerifyLayers(uint64_t address, uint64_t size);
// Any thread, as a comparison lands: counted for the capture's "FG 0 verify" summary.
void CountVerify(bool image, uint64_t bytes, bool differs);

// A shader the frame runs: its guest code is dumped once per capture.
void NoteShader(const char* stage, uint64_t hash, uint64_t code_address, const uint32_t* user_data,
                uint32_t user_data_count);

} // namespace Libs::Graphics::FrameGraph

namespace Libs::Graphics {
// The size of the guest code registered for the shader at this address (0 when unknown).
[[nodiscard]] uint32_t ShaderCodeSizeBytes(uint64_t shader_addr);
} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_FRAMEGRAPH_H_
