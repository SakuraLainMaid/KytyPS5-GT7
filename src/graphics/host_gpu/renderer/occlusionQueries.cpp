#include "graphics/host_gpu/renderer/occlusionQueries.h"

#include "common/logging/log.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/commandRecorder.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/renderContext.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace Libs::Graphics::OcclusionQueries {

namespace {

// A ring of queries, reset a block at a time (outside rendering) when the ring reaches it; a block
// is used again once every query in it was read.
constexpr uint32_t PoolQueries  = 1u << 16u;
constexpr uint32_t BlockQueries = 4096u;
constexpr uint64_t ReadyBit     = uint64_t {1} << 63u;
constexpr uint32_t DepthBlocks  = 16u;

// GPU thread only (the deferred operations come back to it, TrySendCommand).
struct State {
	vk::Device    device  = nullptr;
	vk::QueryPool pool    = nullptr;
	bool          failed  = false;
	bool          precise = false;
	uint64_t      begun   = 0; // queries begun
	uint64_t      read    = 0; // queries whose samples are in `total`
	uint64_t      reset   = 0; // the queries below this one were reset
	uint64_t      total   = 0; // the samples the read queries passed
	// Diagnostics, logged every 5 s at a dump.
	uint64_t                              dumps       = 0;
	uint64_t                              written     = 0;
	uint64_t                              counted     = 0;
	uint64_t                              full        = 0; // draws not counted: the ring was full
	uint64_t                              unavailable = 0; // queries read without a result
	uint64_t                              skipped     = 0; // draws skipped, counted as visible
	std::vector<uint32_t>                 controls;        // DB_COUNT_CONTROL values drawn with
	uint32_t                              last_control   = UINT32_MAX;
	uint64_t                              synthetic      = 0; // dumps before any draw counted
	std::chrono::steady_clock::time_point since          = std::chrono::steady_clock::now();
	uint64_t                              window_dumps   = 0;
	uint64_t                              window_counted = 0;
};

State& GetState() {
	static State state;
	return state;
}

// DB_COUNT_CONTROL: ZPASS_INCREMENT_DISABLE (bit 0) clear, and PERFECT_ZPASS_COUNTS (bit 1) or a
// ZPASS_ENABLE (bits 8 to 11) set.
bool Counting(uint32_t control) {
	return (control & 1u) == 0u && ((control & 2u) != 0u || ((control >> 8u) & 0xfu) != 0u);
}

// GPU thread: the samples of the queries below `upto`, all of them in work the GPU has finished.
void Read(uint64_t upto) {
	auto& state = GetState();
	if (state.pool == nullptr) {
		state.read = std::max(state.read, upto);
		return;
	}
	std::array<uint64_t, 2u * 512u> values {};
	while (state.read < upto) {
		const auto first = static_cast<uint32_t>(state.read % PoolQueries);
		const auto count = static_cast<uint32_t>(
		    std::min<uint64_t>({upto - state.read, PoolQueries - first, 512u}));
		const auto result = state.device.getQueryPoolResults(
		    state.pool, first, count, count * 2u * sizeof(uint64_t), values.data(),
		    2u * sizeof(uint64_t),
		    vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWithAvailability);
		if (result == vk::Result::eSuccess || result == vk::Result::eNotReady) {
			for (uint32_t i = 0; i < count; i++) {
				if (values[i * 2u + 1u] != 0) {
					state.total += values[i * 2u];
				} else {
					state.unavailable++;
				}
			}
		} else {
			state.unavailable += count;
		}
		state.read += count;
	}
}

// GPU thread: the 16 depth blocks' counters, as a dump writes them (begin/end pairs 16 bytes
// apart); the ready bit set in each.
void WriteCounters(RenderContext& context, uint64_t address, uint64_t total) {
	for (uint32_t block = 0; block < DepthBlocks; block++) {
		const uint64_t value = ReadyBit | (block == 0 ? (total & ~ReadyBit) : 0u);
		const uint64_t at    = address + uint64_t {block} * 16u;
		if (!context.GetBufferCache().WriteClean(at, &value, sizeof(value))) {
			context.PrepareGpuThreadWrite(at, sizeof(value));
			std::memcpy(reinterpret_cast<void*>(at), &value, sizeof(value));
		}
	}
}

// Once the GPU has finished the work recorded so far: the queries below `upto` read and, with an
// address, the counters written there.
void Defer(RenderContext& context, uint64_t upto, uint64_t address) {
	auto* const target = &context;
	context.GetCommandScheduler().DeferPriorityOperation([target, upto, address] {
		(void)target->GetGpu().TrySendCommand([target, upto, address] {
			Read(upto);
			if (address != 0) {
				WriteCounters(*target, address, GetState().total);
				GetState().written++;
			}
		});
	});
}

void NoteControl(uint32_t control) {
	auto& state = GetState();
	if (control == state.last_control) {
		return;
	}
	state.last_control = control;
	auto& controls     = state.controls;
	if (controls.size() < 8 &&
	    std::find(controls.begin(), controls.end(), control) == controls.end()) {
		controls.push_back(control);
	}
}

void LogWindow() {
	auto&      state = GetState();
	const auto now   = std::chrono::steady_clock::now();
	if (now - state.since < std::chrono::seconds(5)) {
		return;
	}
	std::string controls;
	for (const auto control: state.controls) {
		char text[32];
		std::snprintf(text, sizeof(text), "%s0x%x%s", controls.empty() ? "" : ", ", control,
		              Counting(control) ? " (counts)" : "");
		controls += text;
	}
	LOGF("Occlusion queries: %" PRIu64 " dumps and %" PRIu64 " draws counted in the last %.0f s "
	     "(%" PRIu64 " dumps written, %" PRIu64 " draws counted in all; %" PRIu64
	     " not counted: the queries ran out; %" PRIu64 " read without a result; %" PRIu64
	     " skipped while compiling, counted visible; %" PRIu64
	     " dumps synthetic, before any draw counted); the counter at %" PRIu64
	     " samples; DB_COUNT_CONTROL values drawn with: %s; %s queries\n",
	     state.window_dumps, state.window_counted,
	     std::chrono::duration<double>(now - state.since).count(), state.written, state.counted,
	     state.full, state.unavailable, state.skipped, state.synthetic, state.total,
	     controls.empty() ? "none" : controls.c_str(), state.precise ? "precise" : "not precise");
	state.since          = now;
	state.window_dumps   = 0;
	state.window_counted = 0;
}

} // namespace

bool Enabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_OCCLUSION_QUERIES");
		const bool  on    = value == nullptr || value[0] != '0';
		LOGF("Occlusion queries: %s (KYTY_OCCLUSION_QUERIES)\n",
		     on ? "counted by the host GPU" : "synthetic, every query visible");
		return on;
	}();
	return enabled;
}

int32_t PrepareDraw(RenderContext& context, const CommandRecorder& recorder,
                    uint32_t db_count_control) {
	if (!Enabled()) {
		return -1;
	}
	NoteControl(db_count_control);
	if (!Counting(db_count_control)) {
		return -1;
	}
	auto& state = GetState();
	if (state.failed) {
		return -1;
	}
	if (!state.pool) {
		auto&                   graphics = context.GetGraphics();
		vk::QueryPoolCreateInfo info {};
		info.queryType  = vk::QueryType::eOcclusion;
		info.queryCount = PoolQueries;
		if (graphics.device.createQueryPool(&info, nullptr, &state.pool) != vk::Result::eSuccess) {
			state.pool   = nullptr;
			state.failed = true;
			LOGF("Occlusion queries: no query pool; the dumps' counters stay synthetic\n");
			return -1;
		}
		state.device  = graphics.device;
		state.precise = graphics.occlusion_query_precise_enabled;
	}
	if (state.begun - state.read > PoolQueries - BlockQueries) {
		// The ring's next block still holds queries no dump has read.
		state.full++;
		return -1;
	}
	if (state.begun == state.reset) {
		context.GetCommandScheduler().EndRendering();
		recorder.resetQueryPool(state.pool, static_cast<uint32_t>(state.begun % PoolQueries),
		                        BlockQueries);
		state.reset += BlockQueries;
		if (state.begun != 0) {
			// The queries before this block are read even when no dump comes for a while.
			Defer(context, state.begun, 0);
		}
	}
	return static_cast<int32_t>(state.begun % PoolQueries);
}

void BeginDraw(const CommandRecorder& recorder, int32_t slot) {
	if (slot < 0) {
		return;
	}
	auto& state = GetState();
	recorder.beginQuery(state.pool, static_cast<uint32_t>(slot),
	                    state.precise ? vk::QueryControlFlagBits::ePrecise
	                                  : vk::QueryControlFlags {});
	state.begun++;
	state.counted++;
	state.window_counted++;
}

void EndDraw(const CommandRecorder& recorder, int32_t slot) {
	if (slot < 0) {
		return;
	}
	recorder.endQuery(GetState().pool, static_cast<uint32_t>(slot));
}

void SkippedDraw(RenderContext& context, uint32_t db_count_control) {
	if (!Enabled() || !Counting(db_count_control) || GetState().failed) {
		return;
	}
	// ponytail: a fixed count, not the draw's real coverage; enough for "visible" tests, a lens
	// flare sized by samples flickers until the pipeline is ready.
	constexpr uint64_t SkippedSamples = 1024u;
	// Deferred like a dump, so a dump recorded before this draw does not see the samples.
	context.GetCommandScheduler().DeferPriorityOperation([target = &context] {
		(void)target->GetGpu().TrySendCommand([] { GetState().total += SkippedSamples; });
	});
	GetState().skipped++;
}

bool Dump(RenderContext& context, uint64_t address) {
	auto& state = GetState();
	state.dumps++;
	state.window_dumps++;
	if (state.counted == 0 || state.failed) {
		state.synthetic++;
		LogWindow();
		return false;
	}
	Defer(context, state.begun, address);
	LogWindow();
	return true;
}

void Shutdown() {
	auto& state = GetState();
	if (state.pool) {
		state.device.destroyQueryPool(state.pool, nullptr);
		state.pool = nullptr;
	}
	state.failed = true;
}

} // namespace Libs::Graphics::OcclusionQueries
