#include "graphics/host_gpu/renderer/bisect.h"

#include "common/logging/log.h"

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
	bool              lit_big  = false;
	bool              lit_all  = false;
	bool              whole_cs = false;
	std::string       name; // for the screen and the picture's file name
	std::string       text; // name and actions, for the log
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

// Whether this step's first use of each action was logged (bit 0 skip, 1 lit, 2 mips).
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

// g_pending_mutex held.
void ApplyLocked() {
	if (!g_pending.has_value()) {
		return;
	}
	g_logged.store(0, std::memory_order_release);
	g_current.store(*g_pending, std::memory_order_release);
	g_pending.reset();
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
	LOGF("Bisect: F8 press %u: a picture of %s (%s.jpg), then %s\n", g_presses,
	     now == 0 ? "normal" : fmt::format("step {}", now).c_str(), g_picture.c_str(),
	     next == 0 ? "back to normal" : fmt::format("step {} of {}: {}", next, steps.size(),
	                                               steps[next - 1].text)
	                                      .c_str());
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
