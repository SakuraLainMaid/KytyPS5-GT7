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
// A block's pipeline statistics queries: one per timed command (watch mode, when the device
// has pipelineStatisticsQuery and KYTY_GPU_WATCH_STATS=1 asks for them).
constexpr uint32_t BlockStats = BlockQueries / 2;

// What each statistics query counts, in the order Vulkan writes them (by bit). Not the input
// assembly or vertex shader counts: a mesh shader draw must not run inside a query of those.
constexpr vk::QueryPipelineStatisticFlags StatFlags =
    vk::QueryPipelineStatisticFlagBits::eClippingInvocations |
    vk::QueryPipelineStatisticFlagBits::eClippingPrimitives |
    vk::QueryPipelineStatisticFlagBits::eFragmentShaderInvocations |
    vk::QueryPipelineStatisticFlagBits::eComputeShaderInvocations;
constexpr uint32_t StatCount = 4;

struct Stats {
	uint64_t clip_in   = 0; // primitives that reached clipping
	uint64_t clip_out  = 0; // primitives clipping put out
	uint64_t fragments = 0; // fragment shader invocations
	uint64_t threads   = 0; // compute shader invocations
};

struct Pair {
	uint32_t   before     = 0;
	uint32_t   after      = 0;
	uint64_t   key        = 0;
	uint32_t   kind       = 0;
	bool       has_detail = false;
	DrawDetail detail;
	int32_t    stats_query = -1; // its statistics query in the block, or -1
	bool       has_stats   = false;
	Stats      stats;
	uint32_t gap = 0; // RecordedCommands marks of what was recorded since the timed command before
};

// Round 165: the GPU time between two timed commands, by the most telling thing recorded there.
enum GapClass : uint32_t {
	GapImageCopies,
	GapBufferCopies,
	GapClears,
	GapFills,
	GapLayoutChanges,
	GapRenderPasses,
	GapBarriers,
	GapNothing,
	GapClassCount
};
constexpr std::array<const char*, GapClassCount> GapClassNames {
    "image copies",   "buffer copies",       "clears",   "fills",
    "layout changes", "render pass changes", "barriers", "nothing recorded"};

GapClass ClassifyGap(uint32_t gap) {
	using namespace RecordedCommands;
	if ((gap & GapImageCopy) != 0) {
		return GapImageCopies;
	}
	if ((gap & GapCopy) != 0) {
		return GapBufferCopies;
	}
	if ((gap & GapClear) != 0) {
		return GapClears;
	}
	if ((gap & GapFill) != 0) {
		return GapFills;
	}
	if ((gap & GapLayout) != 0) {
		return GapLayoutChanges;
	}
	if ((gap & GapRenderPass) != 0) {
		return GapRenderPasses;
	}
	if ((gap & GapBarrier) != 0) {
		return GapBarriers;
	}
	return GapNothing;
}

std::string Count(uint64_t value) {
	if (value >= 10'000'000) {
		return fmt::format("{:.0f}M", static_cast<double>(value) / 1e6);
	}
	if (value >= 1'000'000) {
		return fmt::format("{:.1f}M", static_cast<double>(value) / 1e6);
	}
	if (value >= 10'000) {
		return fmt::format("{:.0f}K", static_cast<double>(value) / 1e3);
	}
	return fmt::format("{}", value);
}

// "{1.2M fragments, 128 primitives clipped to 256}" and the like.
std::string DescribeStats(const Stats& stats, uint32_t kind) {
	if (kind == Dispatch) {
		return fmt::format("{{{} threads}}", Count(stats.threads));
	}
	return fmt::format("{{{} fragments, {} primitives clipped to {}}}", Count(stats.fragments),
	                   Count(stats.clip_in), Count(stats.clip_out));
}

// "[VS ..., 1234 indices x1; 3840x2160, color 1x, depth 4x]" and the like.
std::string Describe(const DrawDetail& detail) {
	std::string text;
	if ((detail.flags & DrawDetail::Tessellated) != 0) {
		text = fmt::format("LS {:016x} HS {:016x}, {} control points, factors ", detail.vs,
		                   detail.hs, detail.patch_points);
		text += detail.tess_max > 0.0f
		            ? fmt::format("{:g}-{:g}", static_cast<double>(detail.tess_min),
		                          static_cast<double>(detail.tess_max))
		            : std::string("unclamped");
	} else {
		text = fmt::format("{} {:016x}", (detail.flags & DrawDetail::Mesh) != 0 ? "mesh" : "VS",
		                   detail.vs);
	}
	if ((detail.flags & DrawDetail::GpuArguments) != 0) {
		text += ", counts on the GPU";
	} else {
		text += fmt::format(", {} {} x{}", detail.count,
		                    (detail.flags & DrawDetail::Indexed) != 0 ? "indices" : "vertices",
		                    detail.instances);
	}
	text += fmt::format("; {}x{}", detail.width, detail.height);
	if (detail.layers > 1) {
		text += fmt::format("x{}", detail.layers);
	}
	if (detail.color_samples != 0) {
		text += fmt::format(", color {}x", detail.color_samples);
	}
	if (detail.depth_samples != 0) {
		text += fmt::format(", depth {}x", detail.depth_samples);
	}
	if (const auto& grid = detail.grid; grid.address != 0) {
		text += fmt::format("; grid 0x{:x} {}x{}x{} view {}+{}", grid.address, grid.width,
		                    grid.height, grid.layers, grid.base, grid.count);
		text += grid.read ? fmt::format(" most {}/{}/{} lights, {} cells over 256", grid.most_low,
		                                grid.most_mid, grid.most_top, grid.over)
		                  : std::string(" (unread)");
	}
	if (detail.grid.words_read) {
		text += fmt::format("; cb 528={:08x} 53c={:08x} 6a4={:08x}", detail.grid.words[0],
		                    detail.grid.words[1], detail.grid.words[2]);
	}
	return "[" + text + "]";
}

struct Block {
	uint64_t          tick = 0;
	uint32_t          used = 0;
	int64_t           open = -1; // query index of a Before without its After yet
	uint32_t          open_gap   = 0;  // the marks that Before took
	uint32_t          stats_used = 0;
	int32_t           stats_open = -1; // statistics query begun by that Before
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
	vk::QueryPool                        stats_pool; // null without pipeline statistics
	double                               period_ns = 1.0;
	std::array<Block, BlockCount>        blocks;
	uint32_t                             next_block = 0;
	Block*                               current    = nullptr;
	std::mutex                           stats_mutex;
	std::unordered_map<uint64_t, Stat>   stats; // key 0: GPU time between timed commands
	std::array<Stat, GapClassCount>      gap_stats {}; // that time by what filled the gap
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

// KYTY_GPU_WATCH_STATS=1: watch mode with a pipeline statistics query around every draw and
// dispatch (the fragments and primitives of the slow ones). Off by default since round 65: with
// them on, round 64's runs had the host GPU 8-17 ms behind instead of 2-6.
bool WatchStats() {
	static const bool on = [] {
		const char* value = std::getenv("KYTY_GPU_WATCH_STATS");
		return value != nullptr && value[0] == '1';
	}();
	return on;
}

double WatchMs() {
	static const double ms = [] {
		const char* value  = std::getenv("KYTY_GPU_WATCH_MS");
		const auto  parsed = value != nullptr ? std::strtod(value, nullptr) : 30.0;
		return parsed > 0.0 ? parsed : 30.0;
	}();
	return ms;
}

// The shaders of the slowest draws watch mode found (ProbesGridsOf), up to MaxSlowShaders.
constexpr size_t      MaxSlowShaders = 32;
std::mutex            g_slow_mutex;
std::vector<uint64_t> g_slow_shaders; // under g_slow_mutex
std::atomic<uint32_t> g_slow_count {0};

void NoteSlowShader(uint64_t key) {
	std::lock_guard lock(g_slow_mutex);
	if (g_slow_shaders.size() >= MaxSlowShaders ||
	    std::ranges::find(g_slow_shaders, key) != g_slow_shaders.end()) {
		return;
	}
	g_slow_shaders.push_back(key);
	g_slow_count.store(static_cast<uint32_t>(g_slow_shaders.size()), std::memory_order_release);
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
		double      ms     = 0.0;
		uint64_t    key    = 0;
		uint32_t    kind   = 0;
		const Pair* pair   = nullptr;
		size_t      index  = 0; // its place among the command buffer's timed commands
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
			timed.push_back({static_cast<double>(after - before) * period_ns / 1e6, pair.key,
			                 pair.kind, &pair, static_cast<size_t>(&pair - block.pairs.data())});
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
	// A draw of 10 ms or more: its shader's light grids are looked at as it draws again.
	for (size_t i = 0; i < timed.size() && i < 3; i++) {
		if (timed[i].kind == Draw && timed[i].ms >= 10.0) {
			NoteSlowShader(timed[i].key);
		}
	}
	std::string costliest;
	for (size_t i = 0; i < timed.size() && i < 6; i++) {
		costliest +=
		    fmt::format("{} {} {:016x} {:.2f} ms", i == 0 ? "" : ",",
		                timed[i].kind == Draw ? "draw" : "dispatch", timed[i].key, timed[i].ms);
		// What the three costliest draws were, and where they came in the command buffer.
		if (i < 3 && timed[i].pair != nullptr && timed[i].pair->has_detail) {
			costliest += fmt::format(" (#{} of {}) {}", timed[i].index + 1, block.pairs.size(),
			                         Describe(timed[i].pair->detail));
		}
		// And what they made the GPU do (pipeline statistics, round 64).
		if (i < 3 && timed[i].pair != nullptr && timed[i].pair->has_stats) {
			costliest += " " + DescribeStats(timed[i].pair->stats, timed[i].kind);
		}
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
	if (block.stats_used != 0 && state.stats_pool) {
		std::vector<uint64_t> counts(static_cast<size_t>(block.stats_used) * StatCount);
		const auto            result = state.device.getQueryPoolResults(
            state.stats_pool, static_cast<uint32_t>(&block - state.blocks.data()) * BlockStats,
            block.stats_used, counts.size() * sizeof(uint64_t), counts.data(),
            StatCount * sizeof(uint64_t), vk::QueryResultFlagBits::e64);
		if (result == vk::Result::eSuccess) {
			for (auto& pair: block.pairs) {
				if (pair.stats_query < 0 ||
				    static_cast<uint32_t>(pair.stats_query) >= block.stats_used) {
					continue;
				}
				const auto* c  = counts.data() + static_cast<size_t>(pair.stats_query) * StatCount;
				pair.stats     = {c[0], c[1], c[2], c[3]};
				pair.has_stats = true;
			}
		}
	}
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
					const auto ms =
					    static_cast<double>(before - previous_after) * state.period_ns / 1e6;
					auto& gap = state.stats[0];
					gap.ms += ms;
					gap.count++;
					auto& by_kind = state.gap_stats[ClassifyGap(pair.gap)];
					by_kind.ms += ms;
					by_kind.count++;
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
	block.used       = 0;
	block.open       = -1;
	block.stats_used = 0;
	block.stats_open = -1;
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
	// What a command buffer records before its first timed command is that gap's.
	RecordedCommands::g_gap.store(0, std::memory_order_relaxed);
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
		if (graphics.pipeline_statistics_enabled && WatchStats()) {
			vk::QueryPoolCreateInfo stats_info {};
			stats_info.queryType          = vk::QueryType::ePipelineStatistics;
			stats_info.queryCount         = BlockStats * BlockCount;
			stats_info.pipelineStatistics = StatFlags;
			if (state.device.createQueryPool(&stats_info, nullptr, &state.stats_pool) !=
			    vk::Result::eSuccess) {
				state.stats_pool = nullptr;
			}
			LOGF("GpuTiming: watch mode counts fragments and primitives of each timed draw: %s\n",
			     state.stats_pool ? "yes" : "no (the query pool was refused)");
		} else if (graphics.pipeline_statistics_enabled) {
			LOGF("GpuTiming: watch mode times commands without pipeline statistics "
			     "(KYTY_GPU_WATCH_STATS=1 counts the fragments and primitives of each)\n");
		}
	}
	auto& block = state.blocks[state.next_block % BlockCount];
	if (block.in_flight.load(std::memory_order_acquire)) {
		return; // every block waits for its command buffer: this one is not timed
	}
	state.next_block++;
	block.tick       = tick;
	block.used       = 0;
	block.open       = -1;
	block.stats_used = 0;
	block.stats_open = -1;
	block.pairs.clear();
	block.in_flight.store(true, std::memory_order_release);
	recorder.resetQueryPool(state.pool,
	                        static_cast<uint32_t>(&block - state.blocks.data()) * BlockQueries,
	                        BlockQueries);
	if (state.stats_pool) {
		recorder.resetQueryPool(state.stats_pool,
		                        static_cast<uint32_t>(&block - state.blocks.data()) * BlockStats,
		                        BlockStats);
	}
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
	block->open_gap  = RecordedCommands::g_gap.exchange(0, std::memory_order_relaxed);
	buffer.Recorder().writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, state.pool,
	                                 base + block->used);
	block->open = block->used++;
	// Its statistics, in watch mode (one query each, ended by its After in the same render pass).
	block->stats_open = -1;
	if (state.stats_pool && Watching() && block->stats_used < BlockStats) {
		const auto stats_base = static_cast<uint32_t>(block - state.blocks.data()) * BlockStats;
		buffer.Recorder().beginQuery(state.stats_pool, stats_base + block->stats_used);
		block->stats_open = static_cast<int32_t>(block->stats_used++);
	}
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
	if (block->stats_open >= 0) {
		const auto stats_base = static_cast<uint32_t>(block - state.blocks.data()) * BlockStats;
		buffer.Recorder().endQuery(state.stats_pool,
		                           stats_base + static_cast<uint32_t>(block->stats_open));
	}
	buffer.Recorder().writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, state.pool,
	                                 base + block->used);
	Pair pair;
	pair.before      = static_cast<uint32_t>(block->open);
	pair.after       = block->used;
	pair.key         = key;
	pair.kind        = kind;
	pair.stats_query = block->stats_open;
	pair.gap         = block->open_gap;
	block->pairs.push_back(pair);
	RecordedCommands::g_gap.store(0, std::memory_order_relaxed);
	block->used++;
	block->open       = -1;
	block->stats_open = -1;
}

void AfterDrawSlow(RenderContext& context, CommandBuffer& buffer, uint64_t key,
                   const DrawDetail& detail) {
	auto&      state  = GetState();
	const auto before = state.current != nullptr ? state.current->pairs.size() : 0;
	AfterSlow(context, buffer, key, Draw);
	if (state.current != nullptr && state.current->pairs.size() == before + 1) {
		state.current->pairs.back().has_detail = true;
		state.current->pairs.back().detail     = detail;
	}
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

bool ProbesGridsOf(uint64_t pixel_shader) {
	if (g_slow_count.load(std::memory_order_acquire) == 0 || !Watching()) {
		return false;
	}
	std::lock_guard lock(g_slow_mutex);
	return std::ranges::find(g_slow_shaders, pixel_shader) != g_slow_shaders.end();
}

bool ClaimGridProbe(uint64_t address) {
	// GPU thread only: each grid once in 3 s, 8 probes in 10 s at most.
	using Clock = std::chrono::steady_clock;
	static std::unordered_map<uint64_t, Clock::time_point> last;
	static std::vector<Clock::time_point>                  recent;
	const auto                                             now = Clock::now();
	std::erase_if(recent,
	              [&](Clock::time_point at) { return now - at >= std::chrono::seconds(10); });
	if (recent.size() >= 8) {
		return false;
	}
	if (last.size() >= 4096) {
		last.clear();
	}
	auto& previous = last[address];
	if (previous != Clock::time_point {} && now - previous < std::chrono::seconds(3)) {
		return false;
	}
	previous = now;
	recent.push_back(now);
	return true;
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
		std::array<Stat, GapClassCount>        gaps {};
		{
			std::lock_guard lock(state.stats_mutex);
			sorted.assign(state.stats.begin(), state.stats.end());
			state.stats.clear();
			total          = state.total_ms;
			state.total_ms = 0.0;
			gaps           = state.gap_stats;
			state.gap_stats.fill({});
		}
		std::sort(sorted.begin(), sorted.end(),
		          [](const auto& a, const auto& b) { return a.second.ms > b.second.ms; });
		for (const auto& [key, stat]: sorted) {
			if (key != 0) {
				timed += stat.ms;
			}
		}
		LOGF("GpuTiming frame %" PRIu64 ": span=%.1fms commands=%.1fms\n", frame, total, timed);
		{
			std::string by_kind;
			for (uint32_t i = 0; i < GapClassCount; i++) {
				if (gaps[i].count != 0) {
					by_kind += fmt::format("{}{} {:.2f}ms x{}", by_kind.empty() ? "" : ", ",
					                       GapClassNames[i], gaps[i].ms, gaps[i].count);
				}
			}
			if (!by_kind.empty()) {
				LOGF("GpuTiming   between, by what was recorded there: %s\n", by_kind.c_str());
			}
		}
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
