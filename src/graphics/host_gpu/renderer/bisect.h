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
//   mips:whole   a sampler with unnormalized coordinates gets the whole view in every stage, as
//                before 2026-10-15 (round 56); otherwise only pixel shaders do. mips:cs does so
//                for compute shaders only, mips:graphics for the other stages only
namespace Libs::Graphics::Bisect {

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
[[nodiscard]] bool WholeUnnormalizedViews(bool compute);

} // namespace Libs::Graphics::Bisect

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BISECT_H_
