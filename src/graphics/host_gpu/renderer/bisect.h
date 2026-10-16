#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BISECT_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BISECT_H_

#include <cstdint>
#include <string>

// Diagnostics: KYTY_BISECT="<step>;<step>;...". Each F8 press saves a picture of the frame as it
// is (image-dumps/bisect_<press>_<step>.jpg, next to the log) and then turns on the next step,
// only that one; after the last step it goes back to normal. So one scene can be compared with
// each part of the frame changed in turn: hold still, press F8, wait a second, press again, and
// so on. The step that is on is named at the top of the screen, and each press logs a "Bisect:"
// line. KYTY_BISECT_START=<n> turns step n on from the start, for what the game makes while it
// loads. A step is one action or several joined with '+', after an optional "<name>=":
//   skip:<hash>  the draws and dispatches with a shader of that hash (16 hex digits, or the first
//                8 or more of them) are not run
//   lit:big      depth comparisons against depth images 4096 or more texels wide (the top-down
//                shadow maps of a whole track) pass, as if nothing cast a shadow there
//   lit:all      every depth comparison passes: no shadow map darkens anything
//   mips:cs      a compute shader's sampler with unnormalized coordinates gets the T#'s whole view,
//                as before 2026-10-15 (round 56), instead of its first mip. Graphics stages keep
//                the whole view anyway: mips:whole is the same as mips:cs, and mips:graphics
//                changes nothing
// and, for A/B tests of performance with the Perf lines (common/perfStats.h), which name the step:
//   cpu:vcache   the process runs on the processors sharing the largest L3 cache (cpu:frequency
//                the others, cpu:all every one); without it, as at the start (KYTY_CPU_SET)
//   readback:<n> a game thread's fault on GPU-written memory downloads a window of n KB around
//                it (KYTY_READBACK_WINDOW_KB, default 4096; 0 the faulting pages only)
//   log:off      LOGF writes nothing
//   writes:off   a store to a page protected because the GPU wrote to it downloads the page first,
//                as before 2026-10-16 (round 61), instead of being done in place
//                (KYTY_NO_CLEAN_WRITE_EMULATION=1 for the whole run)
//   gputime:<n>  the next n frames are timed on the host GPU (GpuTiming, "GpuTiming frame" lines),
//                once, when the step turns on
// and, for the stalls (renderer/stallWatch.h):
//   nancheck:off the NaN check (KYTY_NAN_CHECK) checks and scrubs nothing
//   present:fence the presenter waits on the CPU for the swapchain image it acquires, and its
//                command buffer waits for nothing on the GPU (KYTY_PRESENT_ACQUIRE=fence for the
//                whole run), instead of making the GPU queue wait for the image
//   gc:off       neither the buffer cache nor the texture cache collects anything
//   gpuwatch     every draw and dispatch is timed on the host GPU, and a command buffer that
//                takes KYTY_GPU_WATCH_MS (default 30) or more is logged with its costliest ones
//                ("GpuTiming slow" lines; KYTY_GPU_WATCH=1 for the whole run)
namespace Libs::Graphics::Bisect {

// The GPU thread, at its start: applies the start step's CPU set and log setting.
void Start();

// F8 (window thread): asks for a picture of the frame as it is and for the next step after it;
// returns the line to show for that step.
std::string Advance();

// Presenter thread: the file name (no extension) of the picture Advance asked for, or empty.
// Once the picture is recorded (or there was no frame to take), ApplyPending turns on the step.
[[nodiscard]] std::string TakePictureRequest();
void                      ApplyPending();

// GPU thread: what the step that is on asks for.
[[nodiscard]] bool Active();
[[nodiscard]] bool SkipsShader(uint64_t hash);
[[nodiscard]] bool PassesDepthCompare(bool big_depth_image);
// Whether a compute shader's sampler with unnormalized coordinates keeps the T#'s whole view.
[[nodiscard]] bool WholeUnnormalizedViews();
// The readback window the step that is on asks for, in KB, or -1.
[[nodiscard]] int64_t ReadbackWindowKB();
// Any thread: whether the step that is on turns off stores done in place (writes:off).
[[nodiscard]] bool CleanWritesOff();
// Any thread: nancheck:off, present:fence, gc:off.
[[nodiscard]] bool NanCheckOff();
[[nodiscard]] bool PresentFence();
[[nodiscard]] bool GcOff();

} // namespace Libs::Graphics::Bisect

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BISECT_H_
