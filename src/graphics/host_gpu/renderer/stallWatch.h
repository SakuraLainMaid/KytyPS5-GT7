#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_STALLWATCH_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_STALLWATCH_H_

#include "graphics/host_gpu/vulkanCommon.h"

#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// Diagnostics (KYTY_STALL_WATCH, default on; 0 turns it off): every command buffer of the
// renderer's and the presenter's schedulers carries a host GPU timestamp at its start and its end,
// and the CPU side keeps when it was recorded, handed over and submitted, and what it held (draws,
// dispatches, their first shaders, transfers). When a thread waits KYTY_STALL_MS (default 100) or
// more for the host GPU -- the priority-operation thread, the scheduler's own thread, or a game
// thread's readback -- a "Stall" report lays out the command buffers the GPU ran around that wait,
// in the GPU's order with the idle time before each, and the slow CPU sections of that time
// (swapchain acquire and present, the queue lock, vkQueueSubmit, the recording thread, priority
// callbacks, the caches' collectors). Reports are at least 2 s apart for the first 8, 5 s apart up
// to the 40th, then 30 s apart; the Perf GPU line counts every stall.
namespace Libs::Graphics {

struct GraphicContext;

namespace StallWatch {

[[nodiscard]] bool     Enabled();
[[nodiscard]] uint64_t NowNs();

// The presenter, at each present: refreshes (twice a second) the video memory figures the Perf GPU
// line ends with.
void UpdateMemoryText(const GraphicContext& graphics);

enum class Section : uint8_t {
	Acquire,      // vkAcquireNextImageKHR (and its fence wait with present:fence)
	Present,      // the queue lock and vkQueuePresentKHR
	QueueLock,    // waiting for the queue lock to submit
	QueueSubmit,  // vkQueueSubmit itself
	Record,       // the recording thread running one chunk of commands
	Callback,     // a priority operation's callback
	OwnerWait,    // the scheduler's own thread waiting for the host GPU
	PriorityWait, // the GPU thread waiting for priority operations
	BufferGc,     // the buffer cache's collector
	TextureGc,    // the texture cache's collector
	Count
};

namespace Detail {
void NoteSectionSlow(Section section, uint64_t begin_ns, uint64_t end_ns, uint64_t arg);
} // namespace Detail

// A CPU section that may hold the host GPU's queue or the submissions up; kept when it took 2 ms
// or more.
inline void NoteSection(Section section, uint64_t begin_ns, uint64_t end_ns, uint64_t arg = 0) {
	if (end_ns > begin_ns && end_ns - begin_ns >= 2'000'000u) {
		Detail::NoteSectionSlow(section, begin_ns, end_ns, arg);
	}
}

// What a command buffer moved besides its draws and dispatches.
enum class Transfer : uint8_t {
	BufferUpload,
	BufferDownload,
	BufferCopy,
	ImageUpload,
	ImageDownload,
	ImageCopy,
	Detile,
	Tile,
	NanScan,
	Other,
	Count
};

// One scheduler's command buffers.
class Timeline {
public:
	Timeline(GraphicContext& graphics, std::string name);
	~Timeline();
	Timeline(const Timeline&)            = delete;
	Timeline& operator=(const Timeline&) = delete;

	// Before the first command buffer.
	void                             SetName(std::string name);
	[[nodiscard]] const std::string& Name() const noexcept { return m_name; }

	// The thread that allocates the ticks (the owner): a command buffer for `tick` begins, what it
	// holds, and its hand-over for submission.
	void Open(uint64_t tick);
	void NoteDraw(uint64_t shader, bool indirect);
	void NoteDispatch(uint64_t shader, bool indirect, uint64_t groups);
	void NoteTransfer(Transfer kind, uint64_t bytes);
	void Close(uint64_t tick);

	// The thread that records the Vulkan commands: right after vkBeginCommandBuffer, and right
	// before vkEndCommandBuffer.
	void WriteBegin(vk::CommandBuffer command, uint64_t tick) const;
	void WriteEnd(vk::CommandBuffer command, uint64_t tick) const;
	// The thread that submits, once vkQueueSubmit has returned.
	void Queued(uint64_t tick);

	// Any thread that waited `waited_ns` for `tick`; `known`: the last tick seen done when the
	// wait began (0 when not known).
	void OnWait(uint64_t tick, uint64_t known, uint64_t waited_ns);

	// Tests: GPU times (ns) of a timeline's command buffer instead of its timestamp queries.
	using GpuTimes = bool (*)(const Timeline& timeline, uint64_t tick, uint64_t& begin,
	                          uint64_t& end);
	static void SetGpuTimesForTests(GpuTimes times);

	// Round 135, the reporter thread (the Perf GPU line): the host GPU's work on this timeline
	// since the last call, from its command buffers' timestamps: how much of that time it ran one
	// of them, their GPU time, the gaps between them, and how long they took from their hand-over
	// to their submission (the recording thread). Empty when there is nothing to tell.
	[[nodiscard]] std::string BusyText();

private:
	static constexpr uint64_t Slots      = 8192;
	static constexpr size_t   MaxShaders = 4;

	struct Content {
		uint32_t                                                   draws          = 0;
		uint32_t                                                   dispatches     = 0;
		uint32_t                                                   indirect       = 0;
		uint32_t                                                   other_shaders  = 0;
		uint64_t                                                   largest_groups = 0;
		uint64_t                                                   largest_shader = 0;
		std::array<uint64_t, MaxShaders>                           shaders {};
		std::array<uint32_t, MaxShaders>                           counts {};
		std::array<char, MaxShaders>                               kinds {};
		std::array<uint64_t, static_cast<size_t>(Transfer::Count)> bytes {};
		std::array<uint32_t, static_cast<size_t>(Transfer::Count)> transfers {};
	};
	struct Record {
		uint64_t tick      = 0;
		uint64_t opened_ns = 0;
		uint64_t closed_ns = 0;
		uint64_t queued_ns = 0;
		Content  content;
	};
	struct Row {
		const Timeline* timeline = nullptr;
		Record          record;
		bool            has_gpu   = false;
		uint64_t        gpu_begin = 0;
		uint64_t        gpu_end   = 0;
	};

	void AddShader(uint64_t shader, char kind);
	// The records of ticks [first, last] that are still in the ring, with their GPU times.
	void Collect(uint64_t first, uint64_t last, std::vector<Row>& rows) const;
	void Report(uint64_t number, uint64_t tick, uint64_t known, uint64_t waited_ns) const;
	[[nodiscard]] std::string Describe(const Content& content) const;

	GraphicContext&     m_graphics;
	std::string         m_name;
	vk::QueryPool       m_pool      = nullptr;
	double              m_period_ns = 1.0;
	mutable std::mutex  m_mutex; // m_records, m_last_closed, m_busy_*
	std::vector<Record> m_records;
	uint64_t            m_last_closed = 0;
	// BusyText: the last tick it counted, and that command buffer's GPU end.
	uint64_t m_busy_tick = 0;
	uint64_t m_busy_end  = 0;
	// The owner's command buffer in progress.
	Content  m_open;
	uint64_t m_open_tick = 0;
	uint64_t m_open_ns   = 0;
};

} // namespace StallWatch
} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_STALLWATCH_H_
