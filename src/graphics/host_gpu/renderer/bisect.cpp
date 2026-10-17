#include "graphics/host_gpu/renderer/bisect.h"

#include "common/cpuAffinity.h"
#include "common/logging/log.h"
#include "common/perfStats.h"
#include "graphics/host_gpu/renderer/gpuTiming.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdlib>
#include <fmt/format.h>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

namespace Libs::Graphics::Bisect {
namespace {

struct Skip {
	uint64_t    prefix = 0;
	uint32_t    shift  = 0; // the low bits of the hash a shorter prefix leaves out
	std::string text;
};

struct Step {
	std::vector<Skip> skips;
	bool              lit_big     = false;
	bool              lit_all     = false;
	bool              whole_cs    = false;
	bool              log_off     = false;
	bool              writes_off  = false;
	bool              nan_off     = false;
	bool              fence       = false;
	bool              gc_off      = false;
	bool              gpu_watch   = false;
	bool              tess_off    = false;
	bool              readahead   = false;
	bool              cmask_off    = false;
	bool              fastread_off = false;
	bool              fastread_any = false;
	bool              recheck      = false;
	bool              locks_spin   = false;
	bool              htile_depth  = false;
	bool              pattern_off    = false;
	bool              faultblock_off = false;
	bool              pop_wait       = false;
	bool              texmemo_off    = false;
	bool              rebind_twice    = false;
	bool              drawprep_off    = false;
	bool              gpuwrites_fault = false;
	bool              bdawalk_all     = false;
	bool              checkpoints_on  = false;
	bool              speccheck_all   = false;
	bool              drawprep_taint  = false;
	bool              bdaspan_off     = false;
	bool              scanplan_shared = false;
	bool              views_old       = false;
	bool              grids_off       = false;
	float             tess_max    = 0.0f; // tess:max:<f>; 0: none
	uint32_t          flush       = 0;    // flush:<n>; 0: the usual
	int32_t           dflush          = -1;   // dflush:<n>; -1: the usual
	uint32_t          gpu_frames  = 0;    // gputime:<n>
	int64_t           readback_kb = -1; // -1: the usual window
	std::string       cpu_set;          // empty: as at the start
	std::string       name;             // for the screen and the picture's file name
	std::string       text;             // name and actions, for the log
};

std::string_view Trim(std::string_view text) {
	while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
		text.remove_prefix(1);
	}
	while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
		text.remove_suffix(1);
	}
	return text;
}

std::vector<std::string_view> Split(std::string_view text, char separator) {
	std::vector<std::string_view> parts;
	size_t                        start = 0;
	while (start <= text.size()) {
		const auto end = (std::min)(text.find(separator, start), text.size());
		if (const auto part = Trim(text.substr(start, end - start)); !part.empty()) {
			parts.push_back(part);
		}
		start = end + 1;
	}
	return parts;
}

bool ParseAction(std::string_view action, Step& step) {
	if (action == "lit:big") {
		step.lit_big = true;
		return true;
	}
	if (action == "lit:all") {
		step.lit_all = true;
		return true;
	}
	if (action == "mips:whole" || action == "mips:cs") {
		step.whole_cs = true;
		return true;
	}
	if (action == "log:off") {
		step.log_off = true;
		return true;
	}
	if (action == "writes:off") {
		step.writes_off = true;
		return true;
	}
	if (action == "nancheck:off") {
		step.nan_off = true;
		return true;
	}
	if (action == "present:fence") {
		step.fence = true;
		return true;
	}
	if (action == "gc:off") {
		step.gc_off = true;
		return true;
	}
	if (action == "gpuwatch") {
		step.gpu_watch = true;
		return true;
	}
	if (action == "tess:off") {
		step.tess_off = true;
		return true;
	}
	if (action == "readahead") {
		step.readahead = true;
		return true;
	}
	if (action == "cmask:off") {
		step.cmask_off = true;
		return true;
	}
	if (action == "fastread:off") {
		step.fastread_off = true;
		return true;
	}
	if (action == "fastread:any") {
		step.fastread_any = true;
		return true;
	}
	if (action == "recheck") {
		step.recheck = true;
		return true;
	}
	if (action == "locks:spin") {
		step.locks_spin = true;
		return true;
	}
	if (action == "htile:depth") {
		step.htile_depth = true;
		return true;
	}
	if (action == "patternfill:off") {
		step.pattern_off = true;
		return true;
	}
	if (action == "faultblock:off") {
		step.faultblock_off = true;
		return true;
	}
	if (action == "popwait") {
		step.pop_wait = true;
		return true;
	}
	if (action == "texmemo:off") {
		step.texmemo_off = true;
		return true;
	}
	if (action == "rebind2") {
		step.rebind_twice = true;
		return true;
	}
	if (action == "drawprep:off") {
		step.drawprep_off = true;
		return true;
	}
	if (action == "gpuwrites:fault") {
		step.gpuwrites_fault = true;
		return true;
	}
	if (action == "bdawalk:all") {
		step.bdawalk_all = true;
		return true;
	}
	if (action == "checkpoints:on") {
		step.checkpoints_on = true;
		return true;
	}
	if (action == "speccheck:all") {
		step.speccheck_all = true;
		return true;
	}
	if (action == "drawprep:taint") {
		step.drawprep_taint = true;
		return true;
	}
	if (action == "bdaspan:off") {
		step.bdaspan_off = true;
		return true;
	}
	if (action == "scanplan:shared") {
		step.scanplan_shared = true;
		return true;
	}
	if (action == "views:old") {
		step.views_old = true;
		return true;
	}
	if (action == "grids:off") {
		step.grids_off = true;
		return true;
	}
	if (action.starts_with("tess:max:")) {
		const auto text = std::string(action.substr(9));
		if (text.empty() || text.size() > 8 ||
		    text.find_first_not_of("0123456789.") != std::string::npos) {
			return false;
		}
		const auto factor = std::strtof(text.c_str(), nullptr);
		// The host tessellates factors from 1 to 64.
		if (!(factor >= 1.0f && factor <= 64.0f)) {
			return false;
		}
		step.tess_max = factor;
		return true;
	}
	if (action.starts_with("flush:")) {
		const auto draws = action.substr(6);
		if (draws.empty() || draws.size() > 5 ||
		    draws.find_first_not_of("0123456789") != std::string_view::npos) {
			return false;
		}
		step.flush = static_cast<uint32_t>(std::strtoul(std::string(draws).c_str(), nullptr, 10));
		return step.flush != 0;
	}
	if (action.starts_with("dflush:")) {
		const auto dispatches = action.substr(7);
		if (dispatches.empty() || dispatches.size() > 5 ||
		    dispatches.find_first_not_of("0123456789") != std::string_view::npos) {
			return false;
		}
		step.dflush =
		    static_cast<int32_t>(std::strtoul(std::string(dispatches).c_str(), nullptr, 10));
		return true;
	}
	if (action.starts_with("gputime:")) {
		const auto frames = action.substr(8);
		if (frames.empty() || frames.size() > 4 ||
		    frames.find_first_not_of("0123456789") != std::string_view::npos) {
			return false;
		}
		step.gpu_frames =
		    static_cast<uint32_t>(std::strtoul(std::string(frames).c_str(), nullptr, 10));
		return step.gpu_frames != 0;
	}
	if (action == "cpu:vcache" || action == "cpu:frequency" || action == "cpu:all") {
		step.cpu_set = std::string(action.substr(4));
		return true;
	}
	if (action.starts_with("readback:")) {
		const auto kb = action.substr(9);
		if (kb.empty() || kb.find_first_not_of("0123456789") != std::string_view::npos) {
			return false;
		}
		step.readback_kb =
		    static_cast<int64_t>(std::strtoull(std::string(kb).c_str(), nullptr, 10));
		return true;
	}
	if (action == "mips:graphics") {
		LOGF("Bisect: mips:graphics changes nothing: graphics stages keep the whole view "
		     "anyway\n");
		return true;
	}
	if (action.starts_with("skip:")) {
		auto hex = action.substr(5);
		if (hex.starts_with("0x") || hex.starts_with("0X")) {
			hex.remove_prefix(2);
		}
		if (hex.size() < 8 || hex.size() > 16 ||
		    hex.find_first_not_of("0123456789abcdefABCDEF") != std::string_view::npos) {
			return false;
		}
		Skip skip;
		skip.prefix = std::strtoull(std::string(hex).c_str(), nullptr, 16);
		skip.shift  = static_cast<uint32_t>(4u * (16u - hex.size()));
		skip.text   = std::string(hex);
		step.skips.push_back(std::move(skip));
		return true;
	}
	return false;
}

// 0: none on (normal); n: Steps()[n - 1].
std::atomic<uint32_t> g_current {0};

const std::vector<Step>& Steps() {
	static const std::vector<Step> steps = [] {
		std::vector<Step> result;
		const char*       value = std::getenv("KYTY_BISECT");
		if (value == nullptr) {
			return result;
		}
		for (auto text: Split(value, ';')) {
			Step step;
			bool understood = true;
			// "<name>=<actions>": the name is what the screen shows.
			std::string_view name = text;
			if (const auto equals = text.find('='); equals != std::string_view::npos) {
				name = Trim(text.substr(0, equals));
				text = Trim(text.substr(equals + 1));
			}
			for (const auto action: Split(text, '+')) {
				if (!ParseAction(action, step)) {
					LOGF("Bisect: KYTY_BISECT action \"%.*s\" is not understood; its step is left "
					     "out\n",
					     static_cast<int>(action.size()), action.data());
					understood = false;
				}
			}
			if (!understood) {
				continue;
			}
			step.name = std::string(name.empty() ? text : name);
			step.text = name == text || name.empty() ? std::string(text)
			                                         : fmt::format("{} ({})", name, text);
			result.push_back(std::move(step));
		}
		std::string list;
		for (size_t i = 0; i < result.size(); i++) {
			list += fmt::format("{}{}: {}", i == 0 ? "" : ", ", i + 1, result[i].text);
		}
		LOGF("Bisect: KYTY_BISECT has %zu steps; each F8 saves a picture, then turns on the next "
		     "one: %s\n",
		     result.size(), list.c_str());
		// KYTY_BISECT_START=<n>: step n is on from the start (for what is made while loading).
		if (const char* start = std::getenv("KYTY_BISECT_START"); start != nullptr) {
			const auto n = std::strtoul(start, nullptr, 10);
			if (n >= 1 && n <= result.size()) {
				g_current.store(static_cast<uint32_t>(n), std::memory_order_release);
				LOGF("Bisect: KYTY_BISECT_START: step %lu is on from the start: %s\n", n,
				     result[n - 1].text.c_str());
			}
		}
		return result;
	}();
	return steps;
}

// Whether this step's first use of each action was logged (bit 0 skip, 1 lit, 2 mips, 3 tess:off).
std::atomic<uint32_t> g_logged {0};
// Advance's picture and the step that follows it.
std::mutex              g_pending_mutex;
std::string             g_picture;
std::optional<uint32_t> g_pending;
uint32_t                g_presses = 0;

const Step* Current() {
	const auto& steps   = Steps();
	const auto  current = g_current.load(std::memory_order_acquire);
	return current == 0 || current > steps.size() ? nullptr : &steps[current - 1];
}

bool FirstUse(uint32_t bit) {
	return (g_logged.fetch_or(bit, std::memory_order_acq_rel) & bit) == 0;
}

std::string StartCpuSet() {
	const char* value = std::getenv("KYTY_CPU_SET");
	return value != nullptr && value[0] != '\0' ? std::string(value) : std::string("all");
}

// What the step that is on asks of the process (the CPU set, the log) and the name the Perf lines
// give it. Once at the start, then at every change.
void ApplyEffects() {
	const auto& steps   = Steps();
	const auto  current = g_current.load(std::memory_order_acquire);
	const Step* step    = current == 0 || current > steps.size() ? nullptr : &steps[current - 1];
	Common::PerfStats::SetConfiguration(step != nullptr ? step->name : std::string("normal"));
	Log::SetMuted(step != nullptr && step->log_off);
	if (step != nullptr && step->gpu_frames != 0) {
		GpuTiming::RequestFrames(step->gpu_frames);
		Log::WriteUnmuted(fmt::format("Bisect: gputime: timing the next {} frames on the GPU\n",
		                              step->gpu_frames));
	}
	GpuTiming::SetWatch(step != nullptr && step->gpu_watch);
	Common::PerfStats::SetSpinLocks(Common::PerfStats::SpinLocksFromEnvironment() ||
	                                (step != nullptr && step->locks_spin));
	static std::string applied = StartCpuSet();
	const auto wanted = step != nullptr && !step->cpu_set.empty() ? step->cpu_set : StartCpuSet();
	if (wanted != applied) {
		std::string description;
		Common::SetProcessCpuSet(wanted.c_str(), &description);
		Log::WriteUnmuted(fmt::format("Bisect: cpu:{}: {}\n", wanted, description));
		applied = wanted;
	}
}

// g_pending_mutex held.
void ApplyLocked() {
	if (!g_pending.has_value()) {
		return;
	}
	g_logged.store(0, std::memory_order_release);
	g_current.store(*g_pending, std::memory_order_release);
	g_pending.reset();
	ApplyEffects();
}

std::string FileName(std::string_view name) {
	std::string out;
	for (const char c: name) {
		const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
		                  c == '-';
		if (keep) {
			out += c;
		} else if (!out.empty() && out.back() != '_') {
			out += '_';
		}
		if (out.size() >= 40) {
			break;
		}
	}
	while (!out.empty() && out.back() == '_') {
		out.pop_back();
	}
	return out.empty() ? "step" : out;
}

} // namespace

std::string Advance() {
	const auto& steps = Steps();
	if (steps.empty()) {
		static std::atomic<bool> told {false};
		if (!told.exchange(true)) {
			LOGF("Bisect: F8 does nothing: KYTY_BISECT is not set\n");
		}
		return {};
	}
	std::scoped_lock lock(g_pending_mutex);
	// A press before the last one's picture was taken: that step goes on now, unpictured.
	ApplyLocked();
	const auto now  = g_current.load(std::memory_order_acquire);
	const auto next = (now + 1u) % static_cast<uint32_t>(steps.size() + 1u);
	g_presses++;
	g_picture = fmt::format("bisect_{:02}_{}", g_presses,
	                        now == 0 ? std::string("normal") : FileName(steps[now - 1].name));
	g_pending = next;
	// Written even under log:off, which the press may be leaving.
	Log::WriteUnmuted(fmt::format(
	    "Bisect: F8 press {}: a picture of {} ({}.jpg), then {}\n", g_presses,
	    now == 0 ? std::string("normal") : fmt::format("step {}", now), g_picture,
	    next == 0 ? std::string("back to normal")
	              : fmt::format("step {} of {}: {}", next, steps.size(), steps[next - 1].text)));
	if (next == 0) {
		return fmt::format("Bisect: normal (all {} steps off)", steps.size());
	}
	return fmt::format("Bisect step {} of {}: {}", next, steps.size(), steps[next - 1].name);
}

std::string TakePictureRequest() {
	std::scoped_lock lock(g_pending_mutex);
	std::string      picture;
	picture.swap(g_picture);
	return picture;
}

void ApplyPending() {
	std::scoped_lock lock(g_pending_mutex);
	ApplyLocked();
}

bool Active() {
	return Current() != nullptr;
}

bool SkipsShader(uint64_t hash) {
	const auto* step = Current();
	if (step == nullptr) {
		return false;
	}
	for (const auto& skip: step->skips) {
		if ((hash >> skip.shift) == skip.prefix) {
			if (FirstUse(1u)) {
				LOGF("Bisect: skipping shader %016" PRIx64 " (skip:%s), the first this step\n", hash,
				     skip.text.c_str());
			}
			return true;
		}
	}
	return false;
}

bool PassesDepthCompare(bool big_depth_image) {
	const auto* step = Current();
	if (step == nullptr || !(step->lit_all || (step->lit_big && big_depth_image))) {
		return false;
	}
	if (FirstUse(2u)) {
		LOGF("Bisect: a depth comparison%s passes (%s), the first this step\n",
		     big_depth_image ? " against a big depth image" : "",
		     step->lit_all ? "lit:all" : "lit:big");
	}
	return true;
}

void Start() {
	if (Steps().empty()) {
		return;
	}
	std::scoped_lock lock(g_pending_mutex);
	ApplyEffects();
}

int64_t ReadbackWindowKB() {
	const auto* step = Current();
	return step != nullptr ? step->readback_kb : -1;
}

bool CleanWritesOff() {
	const auto* step = Current();
	return step != nullptr && step->writes_off;
}

bool NanCheckOff() {
	const auto* step = Current();
	return step != nullptr && step->nan_off;
}

bool PresentFence() {
	const auto* step = Current();
	return step != nullptr && step->fence;
}

bool GcOff() {
	const auto* step = Current();
	return step != nullptr && step->gc_off;
}

bool TessOff() {
	const auto* step = Current();
	if (step == nullptr || !step->tess_off) {
		return false;
	}
	if (FirstUse(8u)) {
		LOGF("Bisect: tess:off: a tessellated draw is left out, the first this step\n");
	}
	return true;
}

float TessMaxLevel() {
	const auto* step = Current();
	return step != nullptr ? step->tess_max : 0.0f;
}

uint32_t FlushInterval() {
	const auto* step = Current();
	return step != nullptr ? step->flush : 0u;
}

bool Readahead() {
	const auto* step = Current();
	return step != nullptr && step->readahead;
}

bool MsaaCmaskOff() {
	const auto* step = Current();
	return step != nullptr && step->cmask_off;
}

bool FastReadbackOff() {
	const auto* step = Current();
	return step != nullptr && step->fastread_off;
}

bool FastReadbackAnyAddress() {
	const auto* step = Current();
	return step != nullptr && step->fastread_any;
}

bool GpuClearRecheck() {
	const auto* step = Current();
	return step != nullptr && step->recheck;
}

bool LocksSpin() {
	const auto* step = Current();
	return step != nullptr && step->locks_spin;
}

bool HtileDepthOnly() {
	const auto* step = Current();
	return step != nullptr && step->htile_depth;
}

bool PatternFillsOff() {
	const auto* step = Current();
	return step != nullptr && step->pattern_off;
}

bool FaultBlocksOff() {
	const auto* step = Current();
	return step != nullptr && step->faultblock_off;
}

bool PopWait() {
	const auto* step = Current();
	return step != nullptr && step->pop_wait;
}

bool TextureMemoOff() {
	const auto* step = Current();
	return step != nullptr && step->texmemo_off;
}

bool RebindTwice() {
	const auto* step = Current();
	return step != nullptr && step->rebind_twice;
}

bool DrawPrepOff() {
	const auto* step = Current();
	return step != nullptr && step->drawprep_off;
}

bool GpuWritesFault() {
	const auto* step = Current();
	return step != nullptr && step->gpuwrites_fault;
}

bool BdaWalkAll() {
	const auto* step = Current();
	return step != nullptr && step->bdawalk_all;
}

bool CheckpointsOn() {
	const auto* step = Current();
	return step != nullptr && step->checkpoints_on;
}

bool SpecCheckAll() {
	const auto* step = Current();
	return step != nullptr && step->speccheck_all;
}

int32_t DispatchFlushInterval() {
	const auto* step = Current();
	return step != nullptr ? step->dflush : -1;
}

bool DrawPrepTaint() {
	const auto* step = Current();
	return step != nullptr && step->drawprep_taint;
}

bool BdaSpanOff() {
	const auto* step = Current();
	return step != nullptr && step->bdaspan_off;
}

bool ScanPlanShared() {
	const auto* step = Current();
	return step != nullptr && step->scanplan_shared;
}

bool OldViewBase() {
	const auto* step = Current();
	return step != nullptr && step->views_old;
}

bool LightGridsOff() {
	const auto* step = Current();
	return step != nullptr && step->grids_off;
}

bool WholeUnnormalizedViews() {
	const auto* step = Current();
	if (step == nullptr || !step->whole_cs) {
		return false;
	}
	if (FirstUse(4u)) {
		LOGF("Bisect: an unnormalized sampler of a compute shader keeps the whole view, the first "
		     "this step\n");
	}
	return true;
}

} // namespace Libs::Graphics::Bisect
