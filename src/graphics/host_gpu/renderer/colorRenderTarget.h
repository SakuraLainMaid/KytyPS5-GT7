#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COLORRENDERTARGET_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COLORRENDERTARGET_H_

#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "graphics/host_gpu/renderer/renderTarget.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <algorithm>
#include <cstdint>
#include <span>

namespace Libs::Graphics {

struct RenderColorInfo {
	// Discovery keeps guest image information but can remap the view into a larger cache image.
	TextureCache::ImageDesc         desc;
	ImageId                         image_id;
	uint32_t                        target_slot      = 0;
	uint32_t                        guest_mip_level   = 0;
	uint32_t                        guest_array_layer = 0;
	Prospero::ColorComponentMapping export_mapping;

	[[nodiscard]] vk::Extent2D Extent() const {
		return {std::max(desc.info.extent.width >> guest_mip_level, 1u),
		        std::max(desc.info.extent.height >> guest_mip_level, 1u)};
	}
};

// The colour targets can be rendered through temporary multisampled images and resolved onto
// themselves (single-sample, single-layer 2D at layer 0, level 0); `color_samples` is their highest
// count.
[[nodiscard]] inline bool MsColorTargetsResolvable(std::span<const RenderColorInfo> colors,
                                                   uint32_t&                        color_samples) {
	color_samples   = 0;
	bool resolvable = !colors.empty();
	for (const auto& color: colors) {
		const auto& view = color.desc.view_info;
		color_samples    = std::max(color_samples, color.desc.info.samples);
		// Level 0 too: ReplicateColorToMs copies the whole image into a temporary of the view's
		// size.
		if (color.desc.info.samples != 1 || view.type != vk::ImageViewType::e2D ||
		    view.base_layer != 0 || view.base_level != 0) {
			resolvable = false;
		}
	}
	return resolvable;
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COLORRENDERTARGET_H_
