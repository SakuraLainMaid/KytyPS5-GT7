#include "common/sampler.h"

#include "common/common.h"
#include "common/hostSymbols.h"
#include "common/logging/log.h"
#include "common/perfStats.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // IWYU pragma: keep
#endif

namespace Common::Sampler {

namespace {

std::vector<std::string> Patterns() {
	std::vector<std::string> patterns;
	const char*              value = std::getenv("KYTY_SAMPLE");
	if (value == nullptr || value[0] == '\0' || std::string_view(value) == "0") {
		return patterns;
	}
	if (std::string_view(value) == "1") {
		return {"Thread_Gpu", "Job#0", "GpuRecord", "Thread_DrawPrep"};
	}
	std::string_view rest(value);
	while (!rest.empty()) {
		const auto comma = rest.find(',');
		auto       name  = rest.substr(0, comma);
		while (!name.empty() && name.front() == ' ') {
			name.remove_prefix(1);
		}
		while (!name.empty() && name.back() == ' ') {
			name.remove_suffix(1);
		}
		if (!name.empty()) {
			patterns.emplace_back(name);
		}
		rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);
	}
	return patterns;
}

[[maybe_unused]] bool Matches(const std::string& name, const std::vector<std::string>& patterns) {
	return std::ranges::any_of(patterns, [&](const std::string& pattern) {
		if (!pattern.empty() && pattern.back() == '*') {
			return name.starts_with(std::string_view(pattern).substr(0, pattern.size() - 1));
		}
		return name == pattern;
	});
}

[[maybe_unused]] uint32_t EnvNumber(const char* name, uint32_t fallback, uint32_t low,
                                    uint32_t high) {
	const char* value = std::getenv(name);
	if (value == nullptr || value[0] == '\0') {
		return fallback;
	}
	return std::clamp(static_cast<uint32_t>(std::strtoul(value, nullptr, 10)), low, high);
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

constexpr size_t MaxFrames      = 24;
constexpr size_t StackCopyBytes = 16384;

struct Target {
	uint32_t    tid = 0;
	std::string name;
	HANDLE      handle   = nullptr;
	uint64_t    samples  = 0;
	uint64_t    in_image = 0; // samples whose leaf is in the emulator's own code
	// Leaf addresses, and the emulator's addresses on the stack (once a sample), by samples.
	std::unordered_map<uint64_t, uint32_t> self;
	std::unordered_map<uint64_t, uint32_t> total;
};

struct Image {
	uint64_t           begin = 0;
	uint64_t           end   = 0;
	[[nodiscard]] bool Contains(uint64_t address) const {
		return address >= begin && address < end;
	}
};

Image EmulatorImage() {
	Image       image;
	const auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
	if (base == nullptr) {
		return image;
	}
	const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
		return image;
	}
	const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE) {
		return image;
	}
	image.begin = reinterpret_cast<uint64_t>(base);
	image.end   = image.begin + nt->OptionalHeader.SizeOfImage;
	return image;
}

// One sample's registers and the top of its stack. Copied while the thread is suspended: nothing
// is allocated then (the thread may hold the heap's lock), and the stack it describes cannot
// change underneath.
struct Capture {
	CONTEXT                             context {};
	std::array<uint8_t, StackCopyBytes> stack {};
	size_t                              copied = 0;
};

bool CaptureThread(HANDLE thread, Capture& capture) {
	if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
		return false;
	}
	capture.context              = {};
	capture.context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
	const bool ok                = GetThreadContext(thread, &capture.context) != FALSE;
	capture.copied               = 0;
	if (ok) {
		// Near the top of a stack the copy's size reaches past its end: halve until it fits.
		for (size_t size = StackCopyBytes; size >= 512 && capture.copied == 0; size /= 2) {
			SIZE_T read = 0;
			if (ReadProcessMemory(GetCurrentProcess(),
			                      reinterpret_cast<const void*>(capture.context.Rsp),
			                      capture.stack.data(), size, &read) != FALSE) {
				capture.copied = read;
			}
		}
	}
	ResumeThread(thread);
	return ok;
}

// The leaf, then the return addresses of the frame-pointer chain within the copied stack (the
// emulator keeps frame pointers, /Oy-; the chain stops where it leaves the copy or stops rising).
size_t WalkFrames(const Capture& capture, std::array<uint64_t, MaxFrames>& frames) {
	size_t count    = 0;
	frames[count++] = capture.context.Rip;
	const auto base = capture.context.Rsp;
	auto       rbp  = capture.context.Rbp;
	while (count < MaxFrames && rbp >= base && rbp - base + 16 <= capture.copied &&
	       (rbp & 7u) == 0) {
		uint64_t   next = 0;
		uint64_t   ret  = 0;
		const auto at   = static_cast<size_t>(rbp - base);
		std::memcpy(&next, capture.stack.data() + at, sizeof(next));
		std::memcpy(&ret, capture.stack.data() + at + 8, sizeof(ret));
		if (ret == 0) {
			break;
		}
		frames[count++] = ret;
		if (next <= rbp) {
			break;
		}
		rbp = next;
	}
	return count;
}

class Sampler {
public:
	Sampler(std::vector<std::string> patterns, uint32_t hz, uint32_t report_seconds)
	    : m_patterns(std::move(patterns)), m_hz(hz), m_report_seconds(report_seconds),
	      m_image(EmulatorImage()) {}

	void Run() {
		HANDLE timer = CreateWaitableTimerExW(
		    nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
		const auto period_100ns = static_cast<LONGLONG>(10'000'000 / m_hz);
		auto       capture      = std::make_unique<Capture>();
		auto       last_scan    = std::chrono::steady_clock::now() - std::chrono::seconds(10);
		auto       last_report  = std::chrono::steady_clock::now();
		for (;;) {
			if (timer != nullptr) {
				LARGE_INTEGER due {};
				due.QuadPart = -period_100ns;
				if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) != FALSE) {
					WaitForSingleObject(timer, INFINITE);
				} else {
					Sleep(1);
				}
			} else {
				Sleep(1);
			}
			const auto now = std::chrono::steady_clock::now();
			if (now - last_scan >= std::chrono::seconds(1)) {
				Scan();
				last_scan = now;
			}
			for (auto& target: m_targets) {
				if (target->handle != nullptr) {
					Sample(*target, *capture);
				}
			}
			if (now - last_report >= std::chrono::seconds(m_report_seconds)) {
				Report(std::chrono::duration<double>(now - last_report).count());
				last_report = now;
			}
		}
	}

private:
	// New threads with the names asked for; ended ones are let go.
	void Scan() {
		const auto self = GetCurrentThreadId();
		for (const auto& [tid, name]: PerfStats::NamedThreads()) {
			if (tid == self || !Matches(name, m_patterns) ||
			    std::ranges::any_of(m_targets, [tid](const auto& t) { return t->tid == tid; })) {
				continue;
			}
			HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
			                               THREAD_QUERY_LIMITED_INFORMATION,
			                           FALSE, tid);
			if (handle == nullptr) {
				continue;
			}
			auto target    = std::make_unique<Target>();
			target->tid    = tid;
			target->name   = name;
			target->handle = handle;
			Log::WriteUnmuted(fmt::format("Sample: sampling {} (tid {})\n", name, tid));
			m_targets.push_back(std::move(target));
		}
		for (auto& target: m_targets) {
			DWORD code = 0;
			if (target->handle != nullptr &&
			    (GetExitCodeThread(target->handle, &code) == FALSE || code != STILL_ACTIVE)) {
				CloseHandle(target->handle);
				target->handle = nullptr;
			}
		}
	}

	void Sample(Target& target, Capture& capture) {
		if (!CaptureThread(target.handle, capture)) {
			return;
		}
		std::array<uint64_t, MaxFrames> frames {};
		const auto                      count = WalkFrames(capture, frames);
		target.samples++;
		if (m_image.Contains(frames[0])) {
			target.in_image++;
		}
		target.self[frames[0]]++;
		for (size_t i = 0; i < count; i++) {
			// The emulator's own frames: the chain through the game's code is not reliable.
			if (!m_image.Contains(frames[i]) ||
			    std::find(frames.begin(), frames.begin() + static_cast<std::ptrdiff_t>(i),
			              frames[i]) != frames.begin() + static_cast<std::ptrdiff_t>(i)) {
				continue;
			}
			target.total[frames[i]]++;
		}
	}

	const std::string& Name(uint64_t address) {
		if (const auto it = m_names.find(address); it != m_names.end()) {
			return it->second;
		}
		auto name = HostSymbol(address);
		if (name.empty()) {
			name = fmt::format("0x{:x}", address & ~uint64_t {0xff});
		} else if (name.size() > 90) {
			name.resize(90);
		}
		return m_names.emplace(address, std::move(name)).first->second;
	}

	std::string Top(const std::unordered_map<uint64_t, uint32_t>& counts, uint64_t samples) {
		std::unordered_map<std::string, uint64_t> by_name;
		for (const auto& [address, count]: counts) {
			by_name[Name(address)] += count;
		}
		std::vector<std::pair<uint64_t, std::string>> sorted;
		sorted.reserve(by_name.size());
		for (auto& [name, count]: by_name) {
			sorted.emplace_back(count, name);
		}
		std::ranges::sort(sorted, std::greater {});
		std::string out;
		for (size_t i = 0; i < std::min<size_t>(sorted.size(), 24); i++) {
			out += fmt::format("{}{:.1f}% {}", i == 0 ? " " : "; ",
			                   100.0 * static_cast<double>(sorted[i].first) /
			                       static_cast<double>(std::max<uint64_t>(samples, 1)),
			                   sorted[i].second);
		}
		return out;
	}

	void Report(double seconds) {
		for (auto& target: m_targets) {
			if (target->samples == 0) {
				continue;
			}
			Log::WriteUnmuted(fmt::format(
			    "Sample {} (tid {}): {} samples in {:.0f} s, {:.0f}% in the emulator's code | "
			    "self:{}\n",
			    target->name, target->tid, target->samples, seconds,
			    100.0 * static_cast<double>(target->in_image) /
			        static_cast<double>(target->samples),
			    Top(target->self, target->samples)));
			Log::WriteUnmuted(fmt::format("Sample {} (tid {}) total:{}\n", target->name,
			                              target->tid, Top(target->total, target->samples)));
			target->samples  = 0;
			target->in_image = 0;
			target->self.clear();
			target->total.clear();
		}
		// The names stay: most addresses come back in the next report.
		if (m_names.size() > 400'000) {
			m_names.clear();
		}
	}

	std::vector<std::string>                  m_patterns;
	uint32_t                                  m_hz;
	uint32_t                                  m_report_seconds;
	Image                                     m_image;
	std::vector<std::unique_ptr<Target>>      m_targets;
	std::unordered_map<uint64_t, std::string> m_names;
};

#endif

} // namespace

void Start() {
	static std::once_flag started;
	std::call_once(started, [] {
		auto patterns = Patterns();
		if (patterns.empty()) {
			return;
		}
		std::string list;
		for (const auto& pattern: patterns) {
			list += (list.empty() ? "" : ", ") + pattern;
		}
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		const auto hz      = EnvNumber("KYTY_SAMPLE_HZ", 1000, 10, 5000);
		const auto seconds = EnvNumber("KYTY_SAMPLE_REPORT", 10, 1, 3600);
		Log::WriteUnmuted(fmt::format("Sample: {} times a second: {}; a report every {} s "
		                              "(KYTY_SAMPLE, KYTY_SAMPLE_HZ, KYTY_SAMPLE_REPORT)\n",
		                              hz, list, seconds));
		std::thread([patterns = std::move(patterns), hz, seconds]() mutable {
			PerfStats::NameThread("Sampler");
			Sampler sampler(std::move(patterns), hz, seconds);
			sampler.Run();
		}).detach();
#else
		Log::WriteUnmuted(fmt::format("Sample: KYTY_SAMPLE ({}) only samples on Windows\n", list));
#endif
	});
}

} // namespace Common::Sampler
