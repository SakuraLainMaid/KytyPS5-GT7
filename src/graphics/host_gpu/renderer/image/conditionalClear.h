#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_CONDITIONALCLEAR_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_CONDITIONALCLEAR_H_

#include "common/common.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <array>
#include <compare>
#include <span>
#include <vector>

namespace Libs::Graphics {

class CommandBuffer;
class CommandScheduler;
struct GraphicContext;

// A render target's fast clear decided on the GPU (TextureCache::MaterializeColorClear). The guest
// clears a target by writing one clear code over its color metadata; the host reads the metadata
// back to find the code and clears the image. When the GPU wrote the metadata (a guest compute
// pass), that readback drained the GPU, twice a frame in GT7's races. Here a one-workgroup pass
// checks the metadata where the GPU wrote it, and a draw whose vertex count that pass chose
// clears the target with the matching candidate's value, or draws nothing.
class ConditionalColorClear final {
public:
	enum class Numeric { Float, Uint, Sint };

	struct Candidate {
		uint8_t                 code = 0;
		std::array<uint32_t, 4> bits {};
	};
	static constexpr uint32_t MaxCandidates = 5;

	ConditionalColorClear(GraphicContext& graphics, CommandScheduler& scheduler);
	~ConditionalColorClear();
	KYTY_CLASS_NO_COPY(ConditionalColorClear);

	// The check of `words` dwords at `metadata` (device address), then the clear of `view` (one
	// layer of `format`, color-attachment layout, outside any render pass) when they hold one of
	// the candidates' codes throughout. Records into the scheduler's current command buffer.
	void Record(CommandBuffer& command, vk::DeviceAddress metadata, uint32_t words,
	            std::span<const Candidate> candidates, vk::ImageView view, vk::Format format,
	            vk::Extent2D extent);

	[[nodiscard]] static Numeric NumericOf(vk::Format format);

private:
	// The ring entry the check and the draw share (color_clear_check.comp, Entry).
	struct Entry {
		uint32_t                                   draw[4];
		uint32_t                                   color[4];
		uint32_t                                   candidate_count;
		uint32_t                                   codes[7];
		std::array<std::array<uint32_t, 4>, MaxCandidates> colors;
	};
	static_assert(sizeof(Entry) == 144);
	static constexpr uint32_t EntryStride = 256;
	static constexpr uint32_t Entries     = 1024;

	struct PipelineKey {
		vk::Format format = vk::Format::eUndefined;
		auto       operator<=>(const PipelineKey&) const = default;
	};
	[[nodiscard]] vk::Pipeline GetDrawPipeline(vk::Format format);

	GraphicContext&                               m_graphics;
	Buffer                                        m_entries;
	uint32_t                                      m_next = 0;
	vk::PipelineLayout                            m_check_layout   = nullptr;
	vk::Pipeline                                  m_check_pipeline = nullptr;
	vk::PipelineLayout                            m_draw_layout    = nullptr;
	vk::ShaderModule                              m_vertex_shader  = nullptr;
	std::array<vk::ShaderModule, 3>               m_fragment_shaders {};
	std::vector<std::pair<vk::Format, vk::Pipeline>> m_draw_pipelines;
};

// KYTY_GPU_COLOR_CLEAR=0: render-target fast clears are found by reading the metadata back, as
// before.
[[nodiscard]] bool GpuColorClearEnabled();

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_CONDITIONALCLEAR_H_
