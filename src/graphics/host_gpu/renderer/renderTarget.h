#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERTARGET_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERTARGET_H_

#include "graphics/host_gpu/vulkanCommon.h"

#include <array>
#include <cstdint>

namespace Libs::Graphics {

static constexpr uint32_t RENDER_COLOR_ATTACHMENTS_MAX = 8;

struct RenderAttachment {
	vk::ImageView           image_view    = nullptr;
	vk::ImageLayout         image_layout  = vk::ImageLayout::eUndefined;
	std::array<uint32_t, 4> clear_value   = {};
	bool                    is_clear      = false;
	bool                    has_depth     = false;
	bool                    depth_clear   = false;
	bool                    has_stencil   = false;
	bool                    stencil_clear = false;
	// EQAA fallback: the attachment is a temporary multisampled image resolved onto this view.
	vk::ImageView           resolve_image_view   = nullptr;
	vk::ImageLayout         resolve_image_layout = vk::ImageLayout::eUndefined;
	vk::ResolveModeFlagBits resolve_mode         = vk::ResolveModeFlagBits::eNone;

	bool operator==(const RenderAttachment&) const = default;
};

struct RenderState {
	std::array<RenderAttachment, RENDER_COLOR_ATTACHMENTS_MAX> color_attachments;
	RenderAttachment                                           depth_stencil_attachment;
	uint32_t                                                   width                 = 0;
	uint32_t                                                   height                = 0;
	uint32_t                                                   num_layers            = 1;
	uint32_t                                                   num_color_attachments = 0;

	bool operator==(const RenderState&) const = default;
};

[[nodiscard]] inline constexpr uint32_t render_sample_count(uint32_t encoded_samples) {
	return encoded_samples <= 3 ? 1u << encoded_samples : 0;
}

[[nodiscard]] inline constexpr vk::SampleCountFlagBits vulkan_sample_count(uint32_t samples) {
	switch (samples) {
		case 1: return vk::SampleCountFlagBits::e1;
		case 2: return vk::SampleCountFlagBits::e2;
		case 4: return vk::SampleCountFlagBits::e4;
		case 8: return vk::SampleCountFlagBits::e8;
		default: return {};
	}
}

enum class TargetViewType : uint8_t { Image2D, Image2DArray, Unsupported };

struct TargetViewInfo {
	TargetViewType type         = TargetViewType::Unsupported;
	uint32_t       base_layer   = 0;
	uint32_t       layer_count  = 0;
	uint32_t       image_layers = 0;
};

inline constexpr TargetViewInfo ResolveTargetViewInfo(uint32_t base_layer, uint32_t last_layer,
                                                      uint32_t draw_layer_offset = 0) {
	if (base_layer > last_layer || draw_layer_offset != 0) {
		return {};
	}
	return {base_layer == last_layer ? TargetViewType::Image2D : TargetViewType::Image2DArray,
	        base_layer, last_layer - base_layer + 1u, last_layer + 1u};
}

// PS5 EQAA without host mixed-sample support (a capture layer hides
// VK_NV_framebuffer_mixed_samples): 1-sample colour over 2..4-sample depth renders the colour at
// the depth sample count and resolves it. pipelineCache.cpp and renderDraw.cpp must agree.
[[nodiscard]] inline constexpr bool
EmulatesColorAtDepthSamples(bool accepts_mixed, uint32_t color_samples, uint32_t depth_samples) {
	return !accepts_mixed && color_samples == 1 && depth_samples > 1 && depth_samples <= 4;
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERTARGET_H_
