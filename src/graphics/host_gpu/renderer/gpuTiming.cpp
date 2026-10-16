#include "graphics/host_gpu/renderer/gpuTiming.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandRecorder.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cinttypes>
#include <cstdlib>
#include <fmt/format.h>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics::GpuTiming {

namespace {

constexpr uint32_t BlockQueries = 2048;
constexpr uint32_t BlockCount   = 32;

struct Pair {
	uint32_t before = 0;
	uint32_t after  = 0;
	uint64_t key    = 0;
	uint32_t kind   = 0;
};

struct Block {
	uint64_t          tick = 0;
	uint32_t          used = 0;
	int64_t           open = -1; // query index of a Before without its After yet
	std::vector<Pair> pairs;
	std::atomic<bool> in_flight {false};
};

struct Stat {
	double   ms    = 0.0;
	uint64_t count = 0;
	uint32_t kind  = 0;
};

struct State {
	vk::Device                           device;
	vk::QueryPool                        pool;
	double                               period_ns = 1.0;
	std::array<Block, BlockCount>        blocks;
	uint32_t                             next_block = 0;
	Block*                               current    = nullptr;
	std::mutex                           stats_mutex;
	std::unordered_map<uint64_t, Stat>   stats; // key 0: GPU time between timed commands
	double                               total_ms = 0.0;
};

State& GetState() {
	static State state;
	return state;
}

// Watch mode (KYTY_GPU_WATCH=1, or KYTY_BISECT's gpuwatch): every command is timed, and a command
// buffer that takes KYTY_GPU_WATCH_MS (default 30) or more is logged with its costliest commands.
std::atomic<bool> g_watch_step {false};

bool WatchFromEnvironment() {
	static const bool on = [] {
		const char* value = std::getenv("KYTY_GPU_WATCH");
		return value != nullptr && value[0] == '1';
	}();
	return on;
}

bool Watching() {
	return WatchFromEnvironment() || g_watch_step.load(std::memory_order_relaxed);
}

double WatchMs() {
	static const double ms = [] {
		const char* value  = std::getenv("KYTY_GPU_WATCH_MS");
		const auto  parsed = value != nullptr ? std::strtod(value, nullptr) : 30.0;
		return parsed > 0.0 ? parsed : 30.0;
	}();
	return ms;
}

// A slow command buffer in watch mode: its commands by GPU time, the largest gap between two.
void ReportSlowBlock(const Block& block, const std::vector<uint64_t>& values, double period_ns,
                     double span_ms) {
	static uint64_t reported = 0;
	static auto     last     = std::chrono::steady_clock::time_point {};
	const auto      now      = std::chrono::steady_clock::now();
	if (reported >= 64 && now - last < std::chrono::seconds(5)) {
		return;
	}
	reported++;
	last = now;
	struct Timed {
		double   ms   = 0.0;
		uint64_t key  = 0;
		uint32_t kind = 0;
	};
	std::vector<Timed> timed;
	double             gap_ms       = 0.0;
	uint64_t           gap_after    = 0;
	uint64_t           previous     = 0;
	uint64_t           previous_key = 0;
	for (const auto& pair: block.pairs) {
		const auto before = values[pair.before];
		const auto after  = values[pair.after];
		if (after >= before) {
			timed.push_back(
			    {static_cast<double>(after - before) * period_ns / 1e6, pair.key, pair.kind});
		}
		if (previous != 0 && before >= previous) {
			const auto gap = static_cast<double>(before - previous) * period_ns / 1e6;
			if (gap > gap_ms) {
				gap_ms    = gap;
				gap_after = previous_key;
			}
		}
		previous     = after;
		previous_key = pair.key;
	}
	std::ranges::sort(timed, [](const Timed& a, const Timed& b) { return a.ms > b.ms; });
	std::string costliest;
	for (size_t i = 0; i < timed.size() && i < 6; i++) {
		costliest +=
		    fmt::format("{} {} {:016x} {:.2f} ms", i == 0 ? "" : ",",
		                timed[i].kind == Draw ? "draw" : "dispatch", timed[i].key, timed[i].ms);
	}
	LOGF("GpuTiming slow tick %" PRIu64 ": %.1f ms from the first command to the last, %zu "
	     "commands timed; costliest:%s; largest gap %.2f ms (after %016" PRIx64 ")\n",
	     block.tick, span_ms, timed.size(), costliest.c_str(), gap_ms, gap_after);
}

struct Window {
	uint64_t first = 0;
	uint64_t count = 0;
	// first counts from the first use of the watched shader (WatchedShaderFirstFrame).
	bool relative = false;
};

const Window& GetWindow() {
	static const Window window = [] {
		Window result;
		const char* value = std::getenv("KYTY_GPU_TIMING");
		if (value != nullptr) {
			result.relative = *value == '+';
			char* end       = nullptr;
			result.first    = std::strtoull(result.relative ? value + 1 : value, &end, 10);
			result.count = end != nullptr && *end == ':' ? std::strtoull(end + 1, nullptr, 10) : 1;
		}
		return result;
	}();
	return window;
}

void ReadBlock(State& state, Block& block) {
	if (block.used != 0) {
		std::vector<uint64_t> values(block.used);
		const auto result = state.device.getQueryPoolResults(
		    state.pool, static_cast<uint32_t>(&block - state.blocks.data()) * BlockQueries,
		    block.used, values.size() * sizeof(uint64_t), values.data(), sizeof(uint64_t),
		    vk::QueryResultFlagBits::e64);
		if (result == vk::Result::eSuccess) {
			std::lock_guard lock(state.stats_mutex);
			uint64_t        previous_after = 0;
			for (const auto& pair: block.pairs) {
				const auto before = values[pair.before];
				const auto after  = values[pair.after];
				if (after >= before) {
					auto& stat = state.stats[pair.key];
					stat.ms += static_cast<double>(after - before) * state.period_ns / 1e6;
					stat.count++;
					stat.kind = pair.kind;
				}
				if (previous_after != 0 && before >= previous_after) {
					auto& gap = state.stats[0];
					gap.ms += static_cast<double>(before - previous_after) * state.period_ns / 1e6;
					gap.count++;
				}
				previous_after = after;
			}
			if (!block.pairs.empty()) {
				const auto first = values[block.pairs.front().before];
				const auto last  = values[block.pairs.back().after];
				if (last >= first) {
					const auto span = static_cast<double>(last - first) * state.period_ns / 1e6;
					state.total_ms += span;
					if (span >= WatchMs() && Watching()) {
						ReportSlowBlock(block, values, state.period_ns, span);
					}
				}
			}
		}
	}
	block.pairs.clear();
	block.used = 0;
	block.open = -1;
	block.in_flight.store(false, std::memory_order_release);
}

// The block for the recording in progress, or null when it has none (BeginBlockSlow gives a
// command buffer its block when it begins, outside any render pass: resetting the queries ended
// the draw's rendering when the first draw of a command buffer did it here, 2026-10-16).
Block* CurrentBlock(RenderContext& context, CommandBuffer& /*buffer*/) {
	auto&      state = GetState();
	const auto tick  = context.GetCommandScheduler().CurrentTick();
	if (state.current != nullptr && state.current->tick == tick &&
	    state.current->used + 2 <= BlockQueries) {
		return state.current;
	}
	return nullptr;
}

} // namespace

namespace Detail {

void BeginBlockSlow(RenderContext& context, CommandScheduler& scheduler,
                    const CommandRecorder& recorder, uint64_t tick) {
	if (&context.GetCommandScheduler() != &scheduler) {
		return; // the presenter's command buffers are not timed
	}
	auto& state   = GetState();
	state.current = nullptr;
	if (!state.pool) {
		const auto& graphics = context.GetGraphics();
		state.device         = graphics.device;
		state.period_ns      = graphics.physical_device_properties.limits.timestampPeriod;
		vk::QueryPoolCreateInfo info {};
		info.queryType  = vk::QueryType::eTimestamp;
		info.queryCount = BlockQueries * BlockCount;
		if (state.device.createQueryPool(&info, nullptr, &state.pool) != vk::Result::eSuccess) {
			state.pool = nullptr;
			g_active.store(false);
			return;
		}
	}
	auto& block = state.blocks[state.next_block % BlockCount];
	if (block.in_flight.load(std::memory_order_acquire)) {
		return; // every block waits for its command buffer: this one is not timed
	}
	state.next_block++;
	block.tick = tick;
	block.used = 0;
	block.open = -1;
	block.pairs.clear();
	block.in_flight.store(true, std::memory_order_release);
	recorder.resetQueryPool(state.pool,
	                        static_cast<uint32_t>(&block - state.blocks.data()) * BlockQueries,
	                        BlockQueries);
	// Runs once this tick has executed, so every query of the block is written.
	scheduler.DeferPriorityOperation([&state, &block] { ReadBlock(state, block); });
	state.current = &block;
}

void BeforeSlow(RenderContext& context, CommandBuffer& buffer) {
	auto* block = CurrentBlock(context, buffer);
	if (block == nullptr) {
		return;
	}
	auto&      state = GetState();
	const auto base  = static_cast<uint32_t>(block - state.blocks.data()) * BlockQueries;
	buffer.Recorder().writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, state.pool,
	                                 base + block->used);
	block->open = block->used++;
}

void AfterSlow(RenderContext& context, CommandBuffer& buffer, uint64_t key, uint32_t kind) {
	auto& state = GetState();
	auto* block = state.current;
	if (block == nullptr || block->open < 0 ||
	    block->tick != context.GetCommandScheduler().CurrentTick() ||
	    block->used >= BlockQueries) {
		return;
	}
	const auto base = static_cast<uint32_t>(block - state.blocks.data()) * BlockQueries;
	buffer.Recorder().writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, state.pool,
	                                 base + block->used);
	block->pairs.push_back({static_cast<uint32_t>(block->open), block->used, key, kind});
	block->used++;
	block->open = -1;
}

} // namespace Detail

namespace {
// RequestFrames: how many frames to time from the next present on, and the window it set (the
// frames after the presents first .. first + count - 1, as KYTY_GPU_TIMING's).
std::atomic<uint32_t> g_requested_frames {0};
uint64_t              g_requested_first = UINT64_MAX; // presenter thread
uint64_t              g_requested_count = 0;
bool                  g_frame_lines     = false; // presenter thread: the frame is in a window
} // namespace

void RequestFrames(uint32_t frames) {
	g_requested_frames.store(frames, std::memory_order_release);
}

void SetWatch(bool on) {
	const bool was = g_watch_step.exchange(on, std::memory_order_acq_rel);
	if (on && !was) {
		Detail::g_active.store(true, std::memory_order_relaxed);
		LOGF("GpuTiming: watching: every draw and dispatch is timed on the GPU; command buffers of "
		     "%.0f ms or more are logged (\"GpuTiming slow\")\n",
		     WatchMs());
	}
}

void Present(uint64_t frame) {
	const auto& window = GetWindow();
	if (const auto requested = g_requested_frames.exchange(0, std::memory_order_acq_rel);
	    requested != 0) {
		g_requested_first = frame;
		g_requested_count = requested;
	}
	const bool requested_window = g_requested_count != 0;
	const bool watching         = Watching();
	if (window.count == 0 && !requested_window && !watching) {
		Detail::g_active.store(false, std::memory_order_relaxed);
		return;
	}
	auto& state = GetState();
	if (watching && !g_frame_lines) {
		// Watch mode alone: no frame lines, only the slow command buffers (ReadBlock).
		std::lock_guard lock(state.stats_mutex);
		state.stats.clear();
		state.total_ms = 0.0;
	} else if (Detail::g_active.load(std::memory_order_relaxed)) {
		std::vector<std::pair<uint64_t, Stat>> sorted;
		double                                 timed = 0.0;
		double                                 total = 0.0;
		{
			std::lock_guard lock(state.stats_mutex);
			sorted.assign(state.stats.begin(), state.stats.end());
			state.stats.clear();
			total          = state.total_ms;
			state.total_ms = 0.0;
		}
		std::sort(sorted.begin(), sorted.end(),
		          [](const auto& a, const auto& b) { return a.second.ms > b.second.ms; });
		for (const auto& [key, stat]: sorted) {
			if (key != 0) {
				timed += stat.ms;
			}
		}
		LOGF("GpuTiming frame %" PRIu64 ": span=%.1fms commands=%.1fms\n", frame, total, timed);
		for (size_t i = 0; i < sorted.size() && i < 20; i++) {
			const auto& [key, stat] = sorted[i];
			LOGF("GpuTiming   %s 0x%016" PRIx64 " %7.2fms x%" PRIu64 "\n",
			     key == 0 ? "between " : (stat.kind == Draw ? "draw    " : "dispatch"), key,
			     stat.ms, stat.count);
		}
	}
	auto first = window.first;
	if (window.relative) {
		const auto watched = WatchedShaderFirstFrame();
		first              = watched != UINT64_MAX ? watched + window.first : UINT64_MAX;
	}
	bool active =
	    window.count != 0 && first != UINT64_MAX && frame >= first && frame - first < window.count;
	if (requested_window) {
		if (frame >= g_requested_first && frame - g_requested_first < g_requested_count) {
			active = true;
		} else if (frame >= g_requested_first) {
			g_requested_count = 0;
		}
	}
	// The frame lines are for the timing windows; watch mode times without them.
	g_frame_lines = active;
	Detail::g_active.store(active || watching, std::memory_order_relaxed);
}

} // namespace Libs::Graphics::GpuTiming
