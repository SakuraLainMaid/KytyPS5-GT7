#include "graphics/host_gpu/gpuPhases.h"

#include "common/logging/log.h"
#include "common/perfStats.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cinttypes>
#include <cstdlib>
#include <fmt/format.h>
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

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
	std::array<uint64_t, PhaseCount + 1>                last_protect_pages {};
	std::array<std::array<uint64_t, 3>, PhaseCount + 1> last_protect_modes {};
	std::array<uint64_t, PhaseCount + 1>                last_upload_bytes {};
	std::array<uint64_t, PhaseCount + 1>                last_upload_count {};
	std::array<uint64_t, PhaseCount + 1>                last_upload_copies {};
	// The longest scope of each phase in the window just reported (Totals::longest).
	std::array<uint64_t, PhaseCount> last_longest {};
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

// KYTY_OP_STATS (round 84): the operations by group since the last report (GPU thread only).
struct OpGroup {
	uint64_t extra          = 0;
	uint64_t count          = 0;
	uint64_t cycles         = 0;
	uint64_t bda_cycles     = 0;
	uint64_t protect_cycles = 0;
};
std::unordered_map<uint64_t, OpGroup> g_dispatch_groups;
std::unordered_map<uint64_t, OpGroup> g_draw_groups;

// The groups that took the most cycles, at most `shown` of them, as one line's text.
std::string DescribeGroups(const std::unordered_map<uint64_t, OpGroup>& groups, bool dispatch,
                           double cycles_per_us, uint64_t frames, size_t shown) {
	uint64_t                                         total_cycles = 0;
	uint64_t                                         total_count  = 0;
	std::vector<std::pair<uint64_t, const OpGroup*>> order;
	order.reserve(groups.size());
	for (const auto& [key, group]: groups) {
		total_cycles += group.cycles;
		total_count += group.count;
		order.emplace_back(key, &group);
	}
	const auto take = std::min(shown, order.size());
	std::partial_sort(
	    order.begin(), order.begin() + static_cast<ptrdiff_t>(take), order.end(),
	    [](const auto& a, const auto& b) { return a.second->cycles > b.second->cycles; });
	const auto per_frame = [&](double value) {
		return frames != 0 ? value / static_cast<double>(frames) : 0.0;
	};
	std::string text =
	    fmt::format("{} {}, {} {} ({:.1f} a frame), {:.1f} ms a frame; the most:",
	                dispatch ? "dispatches" : "draws", total_count, groups.size(),
	                dispatch ? "shaders" : "targets", per_frame(static_cast<double>(total_count)),
	                per_frame(static_cast<double>(total_cycles) / cycles_per_us / 1000.0));
	for (size_t i = 0; i < take; i++) {
		const auto& [key, group] = order[i];
		const auto  n            = static_cast<double>(group->count);
		const auto  us_each      = static_cast<double>(group->cycles) / cycles_per_us / n;
		std::string what;
		if (dispatch) {
			what = fmt::format("cs {:016x} ({:.0f} groups)", key,
			                   static_cast<double>(group->extra) / n);
		} else {
			const auto extent  = group->extra;
			const auto layers  = extent >> 32u;
			const auto address = key & ~(uint64_t {1} << 63u);
			what = fmt::format("{} 0x{:x} {}x{}", (key >> 63u) != 0 ? "depth" : "color", address,
			                   extent & 0xffffu, (extent >> 16u) & 0xffffu);
			if (layers > 1u) {
				what += fmt::format("x{}", layers);
			}
		}
		text += fmt::format("{} {}: {:.1f} a frame x {:.2f} us = {:.2f} ms a frame ({:.0f}%; bda "
		                    "{:.2f} us, protects {:.2f} us each)",
		                    i == 0 ? "" : ";", what, per_frame(n), us_each,
		                    per_frame(static_cast<double>(group->cycles) / cycles_per_us / 1000.0),
		                    total_cycles != 0 ? 100.0 * static_cast<double>(group->cycles) /
		                                            static_cast<double>(total_cycles)
		                                      : 0.0,
		                    static_cast<double>(group->bda_cycles) / cycles_per_us / n,
		                    static_cast<double>(group->protect_cycles) / cycles_per_us / n);
	}
	return text;
}

bool EnabledFromEnvironment() {
	const char* value = std::getenv("KYTY_GPU_PHASES");
	return value == nullptr || value[0] != '0';
}

} // namespace

bool OpStatsOn() noexcept {
	static const bool on = [] {
		const char* value = std::getenv("KYTY_OP_STATS");
		return value != nullptr && value[0] == '1';
	}();
	return on;
}

void NoteOp(bool dispatch, uint64_t key, uint64_t extra, uint64_t cycles, uint64_t bda_cycles,
            uint64_t protect_cycles) noexcept {
	auto& group = (dispatch ? g_dispatch_groups : g_draw_groups)[key];
	group.extra = dispatch ? group.extra + extra : extra;
	group.count++;
	group.cycles += cycles;
	group.bda_cycles += bda_cycles;
	group.protect_cycles += protect_cycles;
}

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
	if (OpStatsOn()) {
		LOGF("GPU phases: with it, \"Dispatch shaders\" and \"Draw targets\" lines: the GPU "
		     "thread's time by dispatch shader and by draw target (KYTY_OP_STATS)\n");
	}
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
	std::array<uint64_t, PhaseCount + 1>                protect_pages {};
	std::array<std::array<uint64_t, 3>, PhaseCount + 1> protect_modes {};
	for (size_t i = 0; i <= PhaseCount; i++) {
		protect_cycles[i] = Detail::g_protect_cycles[i] - g_reporter.last_protect_cycles[i];
		protect_count[i]  = Detail::g_protect_count[i] - g_reporter.last_protect_count[i];
		protect_pages[i]  = Detail::g_protect_pages[i] - g_reporter.last_protect_pages[i];
		for (size_t m = 0; m < 3; m++) {
			protect_modes[i][m] =
			    Detail::g_protect_modes[i][m] - g_reporter.last_protect_modes[i][m];
		}
	}
	g_reporter.last_protect_cycles = Detail::g_protect_cycles;
	g_reporter.last_protect_count  = Detail::g_protect_count;
	g_reporter.last_protect_pages  = Detail::g_protect_pages;
	g_reporter.last_protect_modes  = Detail::g_protect_modes;
	std::array<uint64_t, PhaseCount + 1> upload_bytes {};
	std::array<uint64_t, PhaseCount + 1> upload_count {};
	std::array<uint64_t, PhaseCount + 1> upload_copies {};
	for (size_t i = 0; i <= PhaseCount; i++) {
		upload_bytes[i]  = Detail::g_upload_bytes[i] - g_reporter.last_upload_bytes[i];
		upload_count[i]  = Detail::g_upload_count[i] - g_reporter.last_upload_count[i];
		upload_copies[i] = Detail::g_upload_copies[i] - g_reporter.last_upload_copies[i];
	}
	g_reporter.last_upload_bytes  = Detail::g_upload_bytes;
	g_reporter.last_upload_count  = Detail::g_upload_count;
	g_reporter.last_upload_copies = Detail::g_upload_copies;
	g_reporter.last_longest       = totals.longest;
	Detail::g_totals.longest.fill(0);
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
	if (const auto step = Common::PerfStats::WindowConfiguration(); !step.empty()) {
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
			// Round 80: the pages a change covered, and what the changes made the pages.
			const auto changes = static_cast<double>(protect_count[i]);
			by_step += fmt::format(
			    " ({:.1f} pages; {:.0f}% to no access, {:.0f}% read-only, {:.0f}% writable)",
			    static_cast<double>(protect_pages[i]) / changes,
			    100.0 * static_cast<double>(protect_modes[i][0]) / changes,
			    100.0 * static_cast<double>(protect_modes[i][1]) / changes,
			    100.0 * static_cast<double>(protect_modes[i][2]) / changes);
		}
		if (!by_step.empty()) {
			line += " | protects by step: " + by_step;
		}
	}
	{
		// Round 81: the guest bytes uploaded to the GPU by the step they were uploaded in, the
		// most first (CPU writes made since a range was last uploaded; a frame's share with
		// the frames below).
		std::array<size_t, PhaseCount + 1> order {};
		for (size_t i = 0; i < order.size(); i++) {
			order[i] = i;
		}
		std::sort(order.begin(), order.end(),
		          [&](size_t a, size_t b) { return upload_bytes[a] > upload_bytes[b]; });
		std::string by_step;
		uint64_t    total = 0;
		for (const auto bytes: upload_bytes) {
			total += bytes;
		}
		for (size_t k = 0, shown = 0; k < order.size() && shown < 8; k++) {
			const auto i = order[k];
			if (upload_count[i] == 0) {
				continue;
			}
			shown++;
			by_step += fmt::format("{}{} {} x {:.1f} KB ({:.1f} copies)",
			                       by_step.empty() ? "" : ", ", StepName(i), upload_count[i],
			                       static_cast<double>(upload_bytes[i]) / 1024.0 /
			                           static_cast<double>(upload_count[i]),
			                       static_cast<double>(upload_copies[i]) /
			                           static_cast<double>(upload_count[i]));
		}
		if (!by_step.empty()) {
			line +=
			    fmt::format(" | buffer uploads {:.1f} MB", static_cast<double>(total) / 1048576.0);
			if (new_frames != 0) {
				line += fmt::format(" ({:.2f} MB a frame)", static_cast<double>(total) / 1048576.0 /
				                                                static_cast<double>(new_frames));
			}
			line += " by step: " + by_step;
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
	{
		// Round 86: the GPU thread's shader and pipeline work, when there was some.
		const auto longest_ms = [&](Phase phase) {
			return static_cast<double>(g_reporter.last_longest[static_cast<size_t>(phase)]) /
			       cycles_per_us / 1000.0;
		};
		const auto work = [&](const char* name, Phase phase) {
			return fmt::format("{} {} in {:.0f} ms (longest {:.1f} ms)", name, count(phase),
			                   us(phase) / 1000.0, longest_ms(phase));
		};
		if (count(Phase::ShaderDisk) + count(Phase::ShaderCompile) + count(Phase::PipelineWait) +
		        count(Phase::PipelineCompile) + count(Phase::TranslationSkip) +
		        count(Phase::PipelineSkip) !=
		    0) {
			LOGF(
			    "Shader work (%.1f s): on the GPU thread %s, %s, %s, %s; draws skipped while their "
			    "shaders translated %" PRIu64 ", while their pipeline compiled %" PRIu64 "\n",
			    seconds, work("disk cache reads", Phase::ShaderDisk).c_str(),
			    work("translations and compiles", Phase::ShaderCompile).c_str(),
			    work("pipeline waits", Phase::PipelineWait).c_str(),
			    work("pipeline compiles", Phase::PipelineCompile).c_str(),
			    count(Phase::TranslationSkip), count(Phase::PipelineSkip));
		}
	}
	if (OpStatsOn()) {
		LOGF("Dispatch shaders (%.1f s): %s\n", seconds,
		     DescribeGroups(g_dispatch_groups, true, cycles_per_us, new_frames, 16).c_str());
		LOGF("Draw targets (%.1f s): %s\n", seconds,
		     DescribeGroups(g_draw_groups, false, cycles_per_us, new_frames, 16).c_str());
		g_dispatch_groups.clear();
		g_draw_groups.clear();
	}
}

} // namespace Libs::Graphics::GpuPhases
