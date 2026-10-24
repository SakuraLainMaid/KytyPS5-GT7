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
	// of its size (a D32 one), for texture reads that see it as single-sample. Round 124:
	// recorded through the command recorder (no drain of the recording thread).
	void CopyMsDepthSample0(Image& source, Image& destination);
	// Round 124: the first sample of a multisample depth/stencil image's stencil into a
	// single-sample R8_UINT colour image of its size, for texture reads of the stencil that see
	// it as single-sample.
	void CopyMsStencilSample0(Image& source, Image& destination);
	// A single-sample colour image into every sample of a multisample colour image of its size and
	// format: the EQAA fallback's temporary starts from the guest target's contents.
	void ReplicateColorToMs(Image& source, Image& destination);

private:
	// The fragment shader a pipeline runs, and what it writes.
	enum class PipelineKind : uint32_t {
		ColorToMsDepth,   // a colour image's texels as depth samples (depth attachment)
		MsDepthSample0,   // a multisample depth image's first sample (depth attachment)
		MsStencilSample0, // a multisample stencil's first sample (R8_UINT colour attachment)
		ColorToMs,        // a colour image's texels in every sample (colour attachment)
	};
	struct PipelineKey {
		uint32_t     samples = 1;
		vk::Format   format  = vk::Format::eUndefined;
		PipelineKind kind    = PipelineKind::ColorToMsDepth;

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
	vk::ShaderModule        m_stencil0_shader   = nullptr;
	vk::ShaderModule        m_color_ms_shader   = nullptr;
	std::vector<Pipeline>   m_pipelines;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BLITHELPER_H_
