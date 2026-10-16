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
// and, for what holds the frame rate (round 63):
//   tess:off     tessellated draws (patch primitives) are not run
//   tess:max:<f> the tessellator draws factors up to f at most: a larger VGT_HOS_MAX_TESS_LEVEL
//                (or none) is lowered to f, so the tessellation shaders are made again for it on
//                their next draws
//   flush:<n>    a command buffer goes to the GPU after n draws (KYTY_DRAW_FLUSH_INTERVAL for the
//                whole run, default 16)
//   readahead    what game threads read back is downloaded as soon as the GPU writes it again,
//                so their reads find it in memory (KYTY_READAHEAD=1 for the whole run)
// and (round 64):
//   cmask:off    a multisampled color target's CMASK fast clears are left out, as before round
//                63b (KYTY_MSAA_CMASK_CLEARS=0 for the whole run)
//   fastread:off a game thread's read of bytes the GPU has finished writing waits for a download
//                behind the GPU's queue, as before round 64, instead of one on the readback queue
//                (KYTY_FAST_READBACK=0 for the whole run)
//   fastread:any the fast readback copies bytes even while a command that stores through shader
//                addresses is unfinished (it may miss what that command stores there: a test of
//                what waiting for those commands costs)
// and (round 65):
//   recheck      a render target's fast clear is checked on the GPU again at the first draw of
//                every command buffer, as before round 65, even when nothing wrote its metadata
//                since the last check (KYTY_GPU_CLEAR_RECHECK=1 for the whole run)
//   locks:spin   the texture cache's and the memory tracker's locks spin as before round 65
//                (a test-and-set loop) instead of waiting politely (KYTY_LOCKS_SPIN=1 for the
//                whole run)
//   htile:depth  a depth target's HTile fill clears its depth alone, as before round 65, even
//                when the fill's value clears its stencil too (KYTY_HTILE_STENCIL_CLEARS=0 for
//                the whole run)
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
// Any thread: tess:off; tess:max's factor (0 for none); flush:'s draws (0 for the usual);
// readahead.
[[nodiscard]] bool     TessOff();
[[nodiscard]] float    TessMaxLevel();
[[nodiscard]] uint32_t FlushInterval();
[[nodiscard]] bool     Readahead();
// Any thread: cmask:off; fastread:off; fastread:any.
[[nodiscard]] bool MsaaCmaskOff();
[[nodiscard]] bool FastReadbackOff();
[[nodiscard]] bool FastReadbackAnyAddress();
// Any thread: recheck; locks:spin; htile:depth.
[[nodiscard]] bool GpuClearRecheck();
[[nodiscard]] bool LocksSpin();
[[nodiscard]] bool HtileDepthOnly();

} // namespace Libs::Graphics::Bisect

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BISECT_H_
