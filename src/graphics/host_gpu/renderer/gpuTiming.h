#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUTIMING_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUTIMING_H_

#include <atomic>
#include <cstdint>

// Diagnostics: KYTY_GPU_TIMING=<first frame>:<frames> times every draw and dispatch on the host
// GPU (timestamps before and after, which the per-command barriers make exact) and logs, per
// presented frame in that window, the GPU time of the costliest shaders and of everything
// between commands. Off by default. KYTY_GPU_TIMING=+<n>:<frames> starts the window n frames
// after the first use of the KYTY_WATCH_SHADER shader (the first one it names).
namespace Libs::Graphics {

class CommandBuffer;
class CommandRecorder;
class CommandScheduler;
class RenderContext;

namespace GpuTiming {

namespace Detail {
inline std::atomic<bool> g_active {false};
void                     BeginBlockSlow(RenderContext& context, CommandScheduler& scheduler,
                                        const CommandRecorder& recorder, uint64_t tick);
void BeforeSlow(RenderContext& context, CommandBuffer& buffer);
void AfterSlow(RenderContext& context, CommandBuffer& buffer, uint64_t key, uint32_t kind);
} // namespace Detail

// A scheduler's command buffer for `tick` has begun (outside any render pass): while timing, the
// renderer's gets the queries its commands are timed with, reset here.
inline void BeginCommandBuffer(RenderContext& context, CommandScheduler& scheduler,
                               const CommandRecorder& recorder, uint64_t tick) {
	if (Detail::g_active.load(std::memory_order_relaxed)) {
		Detail::BeginBlockSlow(context, scheduler, recorder, tick);
	}
}

enum Kind : uint32_t { Draw = 1, Dispatch = 2 };

// What a draw was, for the "GpuTiming slow" lines: which draws of a shader take the GPU tens of
// milliseconds when the rest of them take microseconds.
struct DrawDetail {
	enum Flag : uint32_t {
		Indexed      = 1u << 0u,
		GpuArguments = 1u << 1u, // counts read on the GPU (indirect)
		Mesh         = 1u << 2u, // through a mesh shader
		Tessellated  = 1u << 3u,
	};
	uint64_t vs            = 0; // the first vertex stage (a tessellated draw's LS)
	uint64_t hs            = 0; // a tessellated draw's HS
	uint32_t count         = 0; // vertices or indices
	uint32_t instances     = 0;
	uint32_t width         = 0; // the render area
	uint32_t height        = 0;
	uint32_t layers        = 0;
	uint32_t color_samples = 0; // 0 without color targets
	uint32_t depth_samples = 0; // 0 without a depth target
	uint32_t patch_points  = 0; // a tessellated draw's input control points
	float    tess_min      = 0.0f; // the factor range it is made for (0: no clamp)
	float    tess_max      = 0.0f;
	uint32_t flags         = 0;
};

namespace Detail {
void AfterDrawSlow(RenderContext& context, CommandBuffer& buffer, uint64_t key,
                   const DrawDetail& detail);
} // namespace Detail

inline void Before(RenderContext& context, CommandBuffer& buffer) {
	if (Detail::g_active.load(std::memory_order_relaxed)) {
		Detail::BeforeSlow(context, buffer);
	}
}

inline void After(RenderContext& context, CommandBuffer& buffer, uint64_t key, uint32_t kind) {
	if (Detail::g_active.load(std::memory_order_relaxed)) {
		Detail::AfterSlow(context, buffer, key, kind);
	}
}

// A draw's After with what it was; `detail` is called only while timing.
template <typename DescribeDraw>
inline void AfterDraw(RenderContext& context, CommandBuffer& buffer, uint64_t key,
                      DescribeDraw&& detail) {
	if (Detail::g_active.load(std::memory_order_relaxed)) {
		Detail::AfterDrawSlow(context, buffer, key, detail());
	}
}

// Called once per presented frame.
void Present(uint64_t frame);

// Any thread: times the `frames` frames presented after the next one (KYTY_BISECT's gputime:<n>),
// besides KYTY_GPU_TIMING's window.
void RequestFrames(uint32_t frames);

// Any thread: watch mode on or off (KYTY_BISECT's gpuwatch; KYTY_GPU_WATCH=1 keeps it on): every
// draw and dispatch is timed, without the frame lines, and a command buffer whose commands span
// KYTY_GPU_WATCH_MS (default 30) or more is logged with its costliest ones ("GpuTiming slow").
void SetWatch(bool on);

// Watch mode (round 65): the shaders of the draws of 10 ms or more in the "GpuTiming slow" lines
// are kept (up to 32), and as such a pixel shader draws again, its light grids (R32G32_UINT array
// textures) are compared with their guest bytes now and then (ClaimGridProbe: each once in 3 s,
// 8 in 10 s), with their most lights a cell per layer ("GpuTiming: ps ... drew slowly" and
// "FG 0 grid" lines): GT7's light loops run as many times a pixel as a cell's count says, and
// counts of tens of thousands would make the reflection cube map's draws that take 30-80 ms where
// the same shader's others take 0.1 ms. GPU thread.
[[nodiscard]] bool ProbesGridsOf(uint64_t pixel_shader);
[[nodiscard]] bool ClaimGridProbe(uint64_t address);

} // namespace GpuTiming
} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUTIMING_H_
