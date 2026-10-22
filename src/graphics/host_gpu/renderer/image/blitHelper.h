#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BLITHELPER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BLITHELPER_H_

#include "common/common.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <compare>
#include <vector>

namespace Libs::Graphics {

class CommandScheduler;
class Image;
struct GraphicContext;

class BlitHelper final {
public:
	inline static constexpr auto ColorToMsDepthLayout =
	    vk::ImageLayout::eDepthStencilAttachmentOptimal;

	BlitHelper(GraphicContext& graphics, CommandScheduler& scheduler);
	~BlitHelper();
	KYTY_CLASS_NO_COPY(BlitHelper);

	void ReinterpretColorAsMsDepth(Image& source, Image& destination);
	// Round 122: the first sample of a multisample depth image into a single-sample depth image
	// of its size (a D32 one), for texture reads that see it as single-sample.
	void CopyMsDepthSample0(Image& source, Image& destination);

private:
	struct PipelineKey {
		uint32_t   samples = 1;
		vk::Format format  = vk::Format::eUndefined;
		// The fragment shader: false, a colour image's texels as depth samples; true, a
		// multisample depth image's first sample.
		bool ms_depth_sample0 = false;

		auto operator<=>(const PipelineKey&) const = default;
	};

	struct Pipeline {
		PipelineKey  key;
		vk::Pipeline handle = nullptr;
	};

	[[nodiscard]] vk::Pipeline GetPipeline(PipelineKey key);

	GraphicContext&         m_graphics;
	CommandScheduler&       m_scheduler;
	vk::DescriptorSetLayout m_descriptor_layout = nullptr;
	vk::PipelineLayout      m_pipeline_layout   = nullptr;
	vk::ShaderModule        m_vertex_shader     = nullptr;
	vk::ShaderModule        m_fragment_shader   = nullptr;
	vk::ShaderModule        m_sample0_shader    = nullptr;
	std::vector<Pipeline>   m_pipelines;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BLITHELPER_H_
