#include "graphics/host_gpu/renderer/frameGraphQueries.h"

#include "common/logging/log.h"
#include "graphics/host_gpu/renderer/commandRecorder.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/frameGraph.h"

#include <algorithm>
#include <cinttypes>
#include <cstdlib>
#include <vector>

namespace Libs::Graphics::FrameGraphQueries {

namespace {

constexpr uint32_t PoolQueries = 65536;
constexpr uint32_t Unused      = ~0u;

bool Enabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_FG_SAMPLES");
		return value == nullptr || value[0] != '0';
	}();
	return enabled;
}

// GPU thread only.
struct State {
	vk::Device            device = nullptr;
	vk::QueryPool         pool   = nullptr;
	bool                  failed = false;
	bool                  reset  = false; // the pool was reset for this capture
	uint32_t              used   = 0;     // queries begun in it
	bool                  ready  = false; // PrepareDraw left a query for the next BeginDraw
	uint64_t              missed = 0;     // draws without a query
	std::vector<uint32_t> ops    = std::vector<uint32_t>(PoolQueries, Unused);
	std::vector<uint8_t>  kinds  = std::vector<uint8_t>(PoolQueries, 0); // Kind of each
};

State& GetState() {
	static State state;
	return state;
}

} // namespace

void PrepareDraw(vk::Device device, CommandScheduler& scheduler, const CommandRecorder& recorder) {
	auto& state = GetState();
	state.ready = false;
	if (!Enabled() || !FrameGraph::Logging() || state.failed) {
		return;
	}
	if (!state.pool) {
		vk::QueryPoolCreateInfo info {};
		info.queryType  = vk::QueryType::eOcclusion;
		info.queryCount = PoolQueries;
		if (device.createQueryPool(&info, nullptr, &state.pool) != vk::Result::eSuccess) {
			state.pool   = nullptr;
			state.failed = true;
			LOGF("FG 0 samples: no query pool, the draws' samples are not counted\n");
			return;
		}
		state.device = device;
	}
	if (!state.reset) {
		// Once a capture, outside rendering (this draw begins its own next): the queries of
		// later command buffers follow the reset in submission order.
		scheduler.EndRendering();
		recorder.resetQueryPool(state.pool, 0, PoolQueries);
		state.reset = true;
	}
	if (state.used >= PoolQueries) {
		state.missed++;
		return;
	}
	state.ready = true;
}

int32_t BeginDraw(const CommandRecorder& recorder) {
	auto& state = GetState();
	if (!state.ready) {
		return -1;
	}
	state.ready      = false;
	const auto query = state.used++;
	state.ops[query]   = FrameGraph::CurrentOp();
	state.kinds[query] = static_cast<uint8_t>(Kind::Draw);
	recorder.beginQuery(state.pool, query);
	return static_cast<int32_t>(query);
}

int32_t BeginExtra(const CommandRecorder& recorder, Kind kind) {
	auto& state = GetState();
	if (!state.pool || !state.reset || !FrameGraph::Logging() || state.used >= PoolQueries) {
		return -1;
	}
	const auto query   = state.used++;
	state.ops[query]   = FrameGraph::CurrentOp();
	state.kinds[query] = static_cast<uint8_t>(kind);
	recorder.beginQuery(state.pool, query);
	return static_cast<int32_t>(query);
}

void EndDraw(const CommandRecorder& recorder, int32_t query) {
	if (query >= 0) {
		recorder.endQuery(GetState().pool, static_cast<uint32_t>(query));
	}
}

bool Pending() {
	const auto& state = GetState();
	return state.reset || state.missed != 0;
}

void LogResults() {
	auto& state = GetState();
	if (state.used != 0) {
		std::vector<uint64_t> values(static_cast<size_t>(state.used) * 2u, 0);
		// Every query read was begun after the reset, in command buffers that have finished.
		const auto result = state.device.getQueryPoolResults(
		    state.pool, 0, state.used, values.size() * sizeof(uint64_t), values.data(),
		    2u * sizeof(uint64_t),
		    vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWithAvailability);
		uint64_t counted = 0;
		uint64_t none    = 0;
		if (result == vk::Result::eSuccess || result == vk::Result::eNotReady) {
			for (uint32_t q = 0; q < state.used; q++) {
				if (state.ops[q] == Unused || values[q * 2u + 1u] == 0) {
					continue;
				}
				const auto kind  = static_cast<Kind>(state.kinds[q]);
				const auto count = values[q * 2u];
				if (kind == Kind::Draw) {
					counted++;
					none += count == 0 ? 1u : 0u;
					LOGF("FG %u   samples passed: %" PRIu64 "\n", state.ops[q], count);
				} else if (kind == Kind::Close) {
					LOGF("FG %u   samples passed as close or closer (16384 units): %" PRIu64 "\n",
					     state.ops[q], count);
				} else {
					LOGF("FG %u   samples drawn without a depth test: %" PRIu64 "\n", state.ops[q],
					     count);
				}
			}
		}
		LOGF("FG 0 samples: %" PRIu64 " draws counted, %" PRIu64 " of them passed none; %" PRIu64
		     " draws without a query\n",
		     counted, none, state.missed);
		std::fill(state.ops.begin(), state.ops.begin() + state.used, Unused);
	} else if (state.missed != 0) {
		LOGF("FG 0 samples: %" PRIu64 " draws without a query\n", state.missed);
	}
	state.reset  = false;
	state.used   = 0;
	state.ready  = false;
	state.missed = 0;
}

void Shutdown() {
	auto& state = GetState();
	if (state.pool) {
		state.device.destroyQueryPool(state.pool, nullptr);
		state.pool = nullptr;
	}
	state.failed = true;
}

} // namespace Libs::Graphics::FrameGraphQueries
