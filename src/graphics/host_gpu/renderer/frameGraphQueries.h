#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_FRAMEGRAPHQUERIES_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_FRAMEGRAPHQUERIES_H_

#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>

// Diagnostics (round 92): while an F9 capture logs, every draw counts the samples that passed its
// depth and stencil tests with an occlusion query, and after the frame each draw's count is logged
// as a line of its operation ("FG <op>   samples passed: <n>"). A colour pass that tests depth for
// EQUAL against its depth prepass and passes nothing did not land where the prepass drew; one that
// passes about as many as its prepass drew did, and what it wrote is its own (the queries are not
// precise ones: a driver may count fewer, never none of many). KYTY_FG_SAMPLES=0 turns it off.
// The queries (65,536) are reset once a capture, outside rendering: a captured frame ends its
// rendering once more for it.
namespace Libs::Graphics {
class CommandRecorder;
class CommandScheduler;
} // namespace Libs::Graphics

namespace Libs::Graphics::FrameGraphQueries {

// GPU thread, before a draw begins its rendering on the renderer's scheduler: while logging, makes
// sure a query is ready for it (may end the rendering open to reset some).
void PrepareDraw(vk::Device device, CommandScheduler& scheduler, const CommandRecorder& recorder);
// GPU thread: a query around a draw's commands, in its rendering, after PrepareDraw; -1: none.
[[nodiscard]] int32_t BeginDraw(const CommandRecorder& recorder);
// Round 93: what a query counts. A draw that tests depth for EQUAL without writing it is drawn
// again after itself in a capture, colour and stencil writes off: once testing "as close or
// closer" with a bias of 16384 units of its depth precision and 4 of its slope towards the camera
// (Close), once without a depth test (Anywhere). Against the draw's own count they tell a pass
// that lands a little apart from its depth prepass from one that something nearer hides. Round
// 107: once more "as close or closer" without the bias (Front): of the samples a little apart, it
// passes those in front of the prepass's depth, not those behind it.
enum class Kind : uint8_t { Draw, Close, Anywhere, Front };
// GPU thread: another query after the draw's own ended, in the same rendering; -1: none.
[[nodiscard]] int32_t BeginExtra(const CommandRecorder& recorder, Kind kind);
void                  EndDraw(const CommandRecorder& recorder, int32_t query);
// GPU thread, after the frame: queries to read (wait for the GPU, then LogResults).
[[nodiscard]] bool Pending();
// GPU thread, after the frame, once its command buffers finished: the counts, then forget them.
void LogResults();
// GPU thread, before the device goes: the query pool.
void Shutdown();

} // namespace Libs::Graphics::FrameGraphQueries

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_FRAMEGRAPHQUERIES_H_
