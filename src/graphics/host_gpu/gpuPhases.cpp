#include "graphics/host_gpu/gpuPhases.h"

#include "common/logging/log.h"
#include "common/perfStats.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <fmt/format.h>
#include <initializer_list>
#include <string>
#include <utility>

namespace Libs::Graphics::GpuPhases {

namespace {

constexpr auto ReportInterval = std::chrono::seconds(10);

struct Reporter {
	std::chrono::steady_clock::time_point last_time {};
	uint64_t                              last_tsc    = 0;
	uint64_t                              last_frames = 0;
	uint64_t                              next_check  = 0;
	Totals                                last {};
	std::array<uint64_t, PhaseCount + 1>  last_protect_cycles {};
	std::array<uint64_t, PhaseCount + 1>  last_protect_count {};
};

// The names of the steps page protection changes are charged to (Detail::ChargesProtects).
const char* StepName(size_t index) {
	switch (static_cast<Phase>(index)) {
		case Phase::Process: return "a slice";
		case Phase::Command: return "commands";
		case Phase::Draw: return "draws";
		case Phase::Dispatch: return "dispatches";
		case Phase::BdaWalk: return "bda walks";
		case Phase::DrawPending: return "draw pending";
		case Phase::DrawConsume: return "draw consume";
		case Phase::DrawPrograms: return "draw programs";
		case Phase::DrawTargets: return "draw targets";
		case Phase::DrawTextures: return "draw textures";
		case Phase::DrawFind: return "draw find";
		case Phase::DrawBda: return "draw bda";
		case Phase::DrawImages: return "draw images";
		case Phase::DrawBuffers: return "draw buffers";
		case Phase::DrawVertex: return "draw vertex";
		case Phase::DrawPipeline: return "draw pipeline";
		case Phase::DrawRendering: return "draw rendering";
		case Phase::DrawCommit: return "draw commit";
		case Phase::DrawEmit: return "draw emit";
		case Phase::DispatchPending: return "dispatch pending";
		case Phase::DispatchProgram: return "dispatch program";
		case Phase::DispatchClears: return "dispatch clears";
		case Phase::DispatchFill: return "dispatch fill";
		case Phase::DispatchPipeline: return "dispatch pipeline";
		case Phase::DispatchTextures: return "dispatch textures";
		case Phase::DispatchFind: return "dispatch find";
		case Phase::DispatchBda: return "dispatch bda";
		case Phase::DispatchImages: return "dispatch images";
		case Phase::DispatchBuffers: return "dispatch buffers";
		case Phase::DispatchCommit: return "dispatch commit";
		default: return "elsewhere";
	}
}

Reporter g_reporter;

bool EnabledFromEnvironment() {
	const char* value = std::getenv("KYTY_GPU_PHASES");
	return value == nullptr || value[0] != '0';
}

} // namespace

void Start() {
	if (!EnabledFromEnvironment()) {
		return;
	}
	Detail::t_on          = true;
	g_reporter.last_time  = std::chrono::steady_clock::now();
	g_reporter.last_tsc   = Now();
	g_reporter.next_check = g_reporter.last_tsc;
	LOGF("GPU phases: every 10 s, a \"GPU phases\" line with the GPU thread's time by step of a "
	     "draw and of a dispatch (KYTY_GPU_PHASES=0 turns it off)\n");
}

void ReportIfDue(uint64_t frames) {
	if (!Detail::t_on) {
		return;
	}
	const auto tsc = Now();
	// A clock read only every ~50 million cycles (~10 ms).
	if (tsc < g_reporter.next_check) {
		return;
	}
	g_reporter.next_check = tsc + 50'000'000u;
	const auto now        = std::chrono::steady_clock::now();
	if (now - g_reporter.last_time < ReportInterval) {
		return;
	}
	const double seconds = std::chrono::duration<double>(now - g_reporter.last_time).count();
	const double cycles_per_us =
	    static_cast<double>(tsc - g_reporter.last_tsc) / (seconds * 1'000'000.0);
	const auto& totals = Detail::g_totals;
	Totals      delta;
	for (size_t i = 0; i < PhaseCount; i++) {
		delta.cycles[i] = totals.cycles[i] - g_reporter.last.cycles[i];
		delta.count[i]  = totals.count[i] - g_reporter.last.count[i];
	}
	std::array<uint64_t, PhaseCount + 1> protect_cycles {};
	std::array<uint64_t, PhaseCount + 1> protect_count {};
	for (size_t i = 0; i <= PhaseCount; i++) {
		protect_cycles[i] = Detail::g_protect_cycles[i] - g_reporter.last_protect_cycles[i];
		protect_count[i]  = Detail::g_protect_count[i] - g_reporter.last_protect_count[i];
	}
	g_reporter.last_protect_cycles = Detail::g_protect_cycles;
	g_reporter.last_protect_count  = Detail::g_protect_count;
	g_reporter.last        = totals;
	g_reporter.last_time   = now;
	g_reporter.last_tsc    = tsc;
	const auto new_frames  = frames - g_reporter.last_frames;
	g_reporter.last_frames = frames;
	if (cycles_per_us <= 0.0) {
		return;
	}

	const auto us = [&](Phase phase) {
		return static_cast<double>(delta.cycles[static_cast<size_t>(phase)]) / cycles_per_us;
	};
	const auto count = [&](Phase phase) { return delta.count[static_cast<size_t>(phase)]; };
	// Microseconds of `phase` per operation of `per`.
	const auto each = [&](Phase phase, Phase per) {
		const auto n = count(per);
		return n != 0 ? us(phase) / static_cast<double>(n) : 0.0;
	};

	std::string line = fmt::format("GPU phases ({:.1f} s)", seconds);
	if (const auto step = Common::PerfStats::Configuration(); !step.empty()) {
		line += fmt::format(" [{}]", step);
	}
	line += fmt::format(": {} frames", new_frames);

	const auto steps = [&](Phase                                                total,
	                       std::initializer_list<std::pair<const char*, Phase>> parts) {
		double sum = 0.0;
		for (const auto& [name, phase]: parts) {
			const auto value = each(phase, total);
			sum += value;
			line += fmt::format(" {} {:.2f},", name, value);
		}
		line += fmt::format(" other {:.2f}", each(total, total) - sum);
	};

	line += fmt::format(" | draws {}, {:.2f} us each:", count(Phase::Draw),
	                    each(Phase::Draw, Phase::Draw));
	steps(Phase::Draw, {{"pending", Phase::DrawPending},
	                    {"consume", Phase::DrawConsume},
	                    {"programs", Phase::DrawPrograms},
	                    {"targets", Phase::DrawTargets},
	                    {"textures", Phase::DrawTextures},
	                    {"find", Phase::DrawFind},
	                    {"bda", Phase::DrawBda},
	                    {"images", Phase::DrawImages},
	                    {"buffers", Phase::DrawBuffers},
	                    {"vertex", Phase::DrawVertex},
	                    {"pipeline", Phase::DrawPipeline},
	                    {"rendering", Phase::DrawRendering},
	                    {"commit", Phase::DrawCommit},
	                    {"emit", Phase::DrawEmit}});
	line += fmt::format(" (describe {:.2f})", each(Phase::Describe, Phase::Draw));
	// Round 78: the tessellated ones among them (GT7's car bodies), and their shaders' share.
	line += fmt::format(" | tessellated draws {}, {:.2f} us each (shaders {:.2f})",
	                    count(Phase::TessDraw), each(Phase::TessDraw, Phase::TessDraw),
	                    each(Phase::TessPrograms, Phase::TessDraw));

	line += fmt::format(" | dispatches {}, {:.2f} us each:", count(Phase::Dispatch),
	                    each(Phase::Dispatch, Phase::Dispatch));
	steps(Phase::Dispatch, {{"pending", Phase::DispatchPending},
	                        {"program", Phase::DispatchProgram},
	                        {"clears", Phase::DispatchClears},
	                        {"fill", Phase::DispatchFill},
	                        {"pipeline", Phase::DispatchPipeline},
	                        {"textures", Phase::DispatchTextures},
	                        {"find", Phase::DispatchFind},
	                        {"bda", Phase::DispatchBda},
	                        {"images", Phase::DispatchImages},
	                        {"buffers", Phase::DispatchBuffers},
	                        {"commit", Phase::DispatchCommit}});

	const auto counted = [&](const char* name, Phase phase) {
		return fmt::format("{} {} x {:.2f} us", name, count(phase), each(phase, phase));
	};
	line += fmt::format(
	    " | program gets {} x {:.2f} us: key {:.2f}, {}, {}", count(Phase::ProgramGet),
	    each(Phase::ProgramGet, Phase::ProgramGet), each(Phase::ProgramKey, Phase::ProgramGet),
	    counted("scanned", Phase::ProgramScanned), counted("walks", Phase::ProgramWalk));
	line += fmt::format(" | {}, {}, {}, {}, {}", counted("draw bda", Phase::DrawBda),
	                    counted("dispatch bda", Phase::DispatchBda),
	                    counted("buffer syncs", Phase::BufferSync),
	                    counted("protects", Phase::Protect), counted("flushes", Phase::Flush));
	const auto walks = count(Phase::BdaWalk);
	const auto per_walk = [&](Phase phase) {
		return walks != 0 ? static_cast<double>(count(phase)) / static_cast<double>(walks) : 0.0;
	};
	line += fmt::format(" | {} ({:.1f} regions, {:.1f} protects each), {}",
	                    counted("bda walks", Phase::BdaWalk), per_walk(Phase::BdaRegions),
	                    per_walk(Phase::BdaProtects), counted("spec checks", Phase::SpecCheck));
	{
		// The protection changes by the step they were made in, the costliest first (round 72).
		std::array<size_t, PhaseCount + 1> order {};
		for (size_t i = 0; i < order.size(); i++) {
			order[i] = i;
		}
		std::sort(order.begin(), order.end(),
		          [&](size_t a, size_t b) { return protect_cycles[a] > protect_cycles[b]; });
		std::string by_step;
		for (size_t k = 0; k < order.size() && k < 8; k++) {
			const auto i = order[k];
			if (protect_count[i] == 0) {
				break;
			}
			by_step += fmt::format("{}{} {} x {:.2f} us", by_step.empty() ? "" : ", ", StepName(i),
			                       protect_count[i],
			                       static_cast<double>(protect_cycles[i]) / cycles_per_us /
			                           static_cast<double>(protect_count[i]));
		}
		if (!by_step.empty()) {
			line += " | protects by step: " + by_step;
		}
	}

	if (new_frames != 0) {
		const auto frame = [&](Phase phase) {
			return us(phase) / 1000.0 / static_cast<double>(new_frames);
		};
		line +=
		    fmt::format(" | a frame: processing {:.1f} ms (draws {:.1f}, dispatches {:.1f}, "
		                "bda {:.1f}), commands {:.1f} ms",
		                frame(Phase::Process), frame(Phase::Draw), frame(Phase::Dispatch),
		                frame(Phase::DrawBda) + frame(Phase::DispatchBda), frame(Phase::Command));
	}
	LOGF("%s\n", line.c_str());
}

} // namespace Libs::Graphics::GpuPhases
