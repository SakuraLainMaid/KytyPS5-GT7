#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_IMAGEDUMP_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_IMAGEDUMP_H_

#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>
#include <string>
#include <vector>

// Diagnostics: KYTY_DUMP_IMAGES=<item>[,<item>...]. During an F9 capture (the frame graph), the
// textures a draw or dispatch binds that match an item are written to image-dumps/ (next to the
// log) as PNG files, as the GPU holds them at that point of the frame (before the draw or
// dispatch): once a capture for each content (an image the GPU writes again is written again), up
// to KYTY_DUMP_IMAGES_MAX (default 40) a capture. Items: 0x<address> (the image at that guest
// address), <width>x<height> or <width>x<height>x<layers> (of mip 0), cubes (arrays of six or more
// layers, a multiple of six), depth (depth formats), all.
//
// A single image is shown at most KYTY_DUMP_IMAGES_SIZE (default 1024) texels on its longer side,
// from the mip closest above that and every n-th row and column; the layers of an array (and the
// slices of a volume) are tiled side by side, six to a row block for cubes, each at most half that.
// Float colors are scaled so that the 99th percentile of the brightest channel is white, with a
// 2.2 gamma; unorm and sRGB bytes are shown as they are, snorm from -1..1, integers against their
// largest value, depth from its smallest to its largest value. Texels that are not finite are
// magenta. The log gets a line for each file ("FG dump ...") with the scale and the counts.
namespace Libs::Graphics::ImageDump {

[[nodiscard]] bool Enabled();
[[nodiscard]] bool Wanted(uint64_t address, uint32_t width, uint32_t height, uint32_t layers,
                          bool depth);
// GPU thread, in a capture: whether to dump this image's content now (the first time this
// capture, within the count).
[[nodiscard]] bool Claim(uint32_t capture, uint64_t address, uint64_t content);
// A single image's longer side; an array's tiles get half of it (at least 64).
[[nodiscard]] uint32_t MaxSide();
// One image's readback, KYTY_DUMP_IMAGES_MB (default 256).
[[nodiscard]] uint64_t MaxBytes();

// The bytes a texel of the aspect a dump copies takes (the depth aspect of a depth format), or 0
// for a format the dump does not decode (block-compressed ones among them).
[[nodiscard]] uint32_t TexelBytes(vk::Format format, bool& depth);

// The readback: `tiles` tiles (layers or slices) `tile_pitch` bytes apart, each `rows` rows of
// `row_texels` texels `row_pitch` bytes apart; every `step`-th texel of a row is shown.
struct Layout {
	vk::Format format      = vk::Format::eUndefined;
	uint32_t   row_texels  = 0;
	uint32_t   rows        = 0;
	uint32_t   tiles       = 0;
	uint32_t   step        = 1;
	uint64_t   row_pitch   = 0;
	uint64_t   tile_pitch  = 0;
	uint32_t   texel_bytes = 0;
	bool       depth       = false;
	bool       cubes       = false;
};

// Any thread: decodes `bytes` and writes image-dumps/<name>.png on a thread of its own.
void Write(std::string name, std::string description, const Layout& layout,
           std::vector<uint8_t> bytes);

} // namespace Libs::Graphics::ImageDump

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_IMAGEDUMP_H_
