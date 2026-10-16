#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_CONDITIONALCLEAR_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_CONDITIONALCLEAR_H_

#include "common/common.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <array>
#include <atomic>
#include <chrono>
#include <compare>
#include <deque>
#include <span>
#include <unordered_map>
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

	// What a check is of, for the report of its outcome.
	struct Note {
		uint64_t target    = 0; // the image's guest address
		uint64_t metadata  = 0; // the checked slice's metadata
		uint32_t layer     = 0;
		uint32_t width     = 0;
		uint32_t height    = 0;
		uint32_t samples   = 1;
		uint32_t fg_op     = 0; // the frame graph's operation it went with (F9), or 0
		bool     dcc       = false;
		bool     unwritten = false; // nothing wrote the metadata since the last check (a recheck)
	};

	ConditionalColorClear(GraphicContext& graphics, CommandScheduler& scheduler);
	~ConditionalColorClear();
	KYTY_CLASS_NO_COPY(ConditionalColorClear);

	// The check of `words` dwords at `metadata` (device address), then the clear of `view` (one
	// layer of `format`, color-attachment layout, outside any render pass) when they hold one of
	// the candidates' codes throughout. Records into the scheduler's current command buffer, and
	// returns the check's number. Its outcome (whether it cleared) is copied where the host reads
	// it once the command buffer is done, and reported: a "TextureCache: GPU fast-clear checks"
	// line every 10 s, an "FG" line for a check in an F9 capture, and the first rechecks of
	// unwritten metadata that cleared.
	uint32_t Record(CommandBuffer& command, vk::DeviceAddress metadata, uint32_t words,
	                std::span<const Candidate> candidates, vk::ImageView view, vk::Format format,
	                uint32_t samples, vk::Extent2D extent, const Note& note);
	// A check left out because nothing wrote its metadata since the last one.
	static void NoteSkipped();

	[[nodiscard]] static Numeric NumericOf(vk::Format format);

private:
	// The outcomes of the checks recorded and not read yet, oldest first.
	struct Pending {
		uint32_t slot   = 0;
		uint32_t number = 0;
		uint64_t tick   = 0;
		Note     note;
	};
	struct TargetCount {
		uint64_t checks            = 0;
		uint64_t cleared           = 0;
		uint64_t unwritten_checks  = 0;
		uint64_t unwritten_cleared = 0;
		uint32_t width             = 0;
		uint32_t height            = 0;
		uint32_t samples           = 1;
		bool     dcc               = false;
	};
	// Reads the outcomes whose command buffers are done (GPU thread, as checks are recorded).
	void Collect();
	void Report(double seconds);

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
		vk::Format format  = vk::Format::eUndefined;
		uint32_t   samples = 1;
		auto       operator<=>(const PipelineKey&) const = default;
	};
	[[nodiscard]] vk::Pipeline GetDrawPipeline(PipelineKey key);

	GraphicContext&                               m_graphics;
	Buffer                                        m_entries;
	uint32_t                                      m_next = 0;
	vk::PipelineLayout                            m_check_layout   = nullptr;
	vk::Pipeline                                  m_check_pipeline = nullptr;
	vk::PipelineLayout                            m_draw_layout    = nullptr;
	vk::ShaderModule                              m_vertex_shader  = nullptr;
	std::array<vk::ShaderModule, 3>               m_fragment_shaders {};
	std::vector<std::pair<PipelineKey, vk::Pipeline>> m_draw_pipelines;

	static constexpr uint32_t OutcomeSlots = 16384;
	CommandScheduler&         m_scheduler;
	Buffer                    m_outcomes; // a dword a check: its draw's vertex count
	uint32_t                  m_next_outcome = 0;
	uint32_t                  m_checks       = 0;
	std::deque<Pending>       m_pending;
	std::unordered_map<uint64_t, TargetCount> m_targets;          // since the last report
	uint64_t                                  m_unknown      = 0; // outcomes overwritten unread
	uint32_t                                  m_detail_lines = 0;
	std::chrono::steady_clock::time_point     m_last_report  = std::chrono::steady_clock::now();
	static std::atomic<uint64_t>              s_skipped;
};

// KYTY_GPU_COLOR_CLEAR=0: render-target fast clears are found by reading the metadata back, as
// before.
[[nodiscard]] bool GpuColorClearEnabled();

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_CONDITIONALCLEAR_H_
