#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_OCCLUSIONQUERIES_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_OCCLUSIONQUERIES_H_

#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>

// Round 119: the guest's occlusion counters (PIXEL_PIPE_STAT_DUMP), counted by the host GPU.
// While DB_COUNT_CONTROL counts (ZPASS_INCREMENT_DISABLE clear, and PERFECT_ZPASS_COUNTS or a
// ZPASS_ENABLE set), each draw counts the samples that pass its depth and stencil tests with a
// host occlusion query. A dump writes the 16 depth blocks' counters at its address (a begin/end
// pair a block, 16 bytes apart; bit 63 marks a result as ready): the samples counted since the
// start in block 0, zero in the others, so a query's end minus its begin is what its draws
// passed. The write waits for the GPU to finish the work recorded before the dump (the deferred
// operations, as KYTY_LABELS_AT_COMPLETION's labels). Before, every dump wrote a counter one
// sample higher than the last in every block: everything counted as visible, and a game's
// aircraft warning lights glowed through the buildings in front of them.
// KYTY_OCCLUSION_QUERIES=0: those synthetic counters, as before.
namespace Libs::Graphics {
class CommandRecorder;
class RenderContext;
} // namespace Libs::Graphics

namespace Libs::Graphics::OcclusionQueries {

[[nodiscard]] bool Enabled();

// GPU thread, before a draw begins its rendering: a query slot for it when DB_COUNT_CONTROL counts
// (may end the rendering open, to reset a block of queries); -1 otherwise.
[[nodiscard]] int32_t PrepareDraw(RenderContext& context, const CommandRecorder& recorder,
                                  uint32_t db_count_control);
// GPU thread, in the draw's rendering: the query around its commands.
void BeginDraw(const CommandRecorder& recorder, int32_t slot);
void EndDraw(const CommandRecorder& recorder, int32_t slot);

// GPU thread: a draw not recorded (its shaders or pipeline still compiling) while DB_COUNT_CONTROL
// counts. It counts as visible: with 0 samples the game culls the object as occluded and never
// draws it again, so it stays missing after its pipeline is ready.
void SkippedDraw(RenderContext& context, uint32_t db_count_control);

// GPU thread: a PIXEL_PIPE_STAT_DUMP to `address` (8-byte aligned). False until a draw has
// counted samples (DB_COUNT_CONTROL never counted): the caller writes the synthetic counters
// then, so a game that counts some other way keeps its lights.
[[nodiscard]] bool Dump(RenderContext& context, uint64_t address);

void Shutdown();

} // namespace Libs::Graphics::OcclusionQueries

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_OCCLUSIONQUERIES_H_
