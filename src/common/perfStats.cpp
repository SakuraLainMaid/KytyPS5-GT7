#include "common/perfStats.h"

#include "common/common.h"
#include "common/cpuAffinity.h"
#include "common/logging/log.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <fmt/format.h>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // IWYU pragma: keep
#include <tlhelp32.h>
#else
#include <dirent.h>
#include <fstream>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace Common::PerfStats {

namespace {

uint32_t IntervalSeconds() {
	static const uint32_t seconds = [] {
		const char* value = std::getenv("KYTY_PERF");
		return value != nullptr ? static_cast<uint32_t>(std::strtoul(value, nullptr, 10)) : 2u;
	}();
	return seconds;
}

uint32_t OsThreadId() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	return static_cast<uint32_t>(GetCurrentThreadId());
#else
	return static_cast<uint32_t>(syscall(SYS_gettid));
#endif
}

struct Counters {
	std::array<std::atomic<uint64_t>, static_cast<size_t>(GpuThread::Count)> gpu {};

	std::atomic<uint64_t> draws {0};
	std::atomic<uint64_t> dispatches {0};
	std::atomic<uint64_t> presents {0};
	std::atomic<uint64_t> downloads {0};
	std::atomic<uint64_t> download_bytes {0};
	std::atomic<uint64_t> faults {0};
	std::atomic<uint64_t> fault_ns {0};
	std::atomic<uint64_t> fault_reads {0};
	std::atomic<uint64_t> fault_emulated {0};
	std::atomic<uint64_t> long_faults {0}; // 1 ms or more
	std::atomic<uint64_t> longest_fault_ns {0};
};
Counters g_counters;

// A thread's time in faults; slots are never freed (a thread that ends keeps its last values).
struct FaultSlot {
	uint32_t              tid = 0;
	std::atomic<uint64_t> ns {0};
	uint64_t              reported_ns = 0; // reporter thread
};
std::mutex                              g_slots_mutex;
std::vector<std::unique_ptr<FaultSlot>> g_slots;
thread_local FaultSlot*                 t_slot = nullptr;

std::mutex                                g_names_mutex;
std::unordered_map<uint32_t, std::string> g_names;

std::mutex  g_configuration_mutex;
std::string g_configuration;

std::string ThreadName(uint32_t tid) {
	{
		std::scoped_lock lock(g_names_mutex);
		if (const auto it = g_names.find(tid); it != g_names.end()) {
			return it->second;
		}
	}
	return fmt::format("tid {}", tid);
}

// Each thread's CPU time (user and kernel) so far, in nanoseconds.
std::unordered_map<uint32_t, uint64_t> ThreadCpuTimes() {
	std::unordered_map<uint32_t, uint64_t> times;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (snapshot == INVALID_HANDLE_VALUE) {
		return times;
	}
	const auto    process = GetCurrentProcessId();
	THREADENTRY32 entry {};
	entry.dwSize = sizeof(entry);
	for (BOOL more = Thread32First(snapshot, &entry); more != FALSE;
	     more      = Thread32Next(snapshot, &entry)) {
		if (entry.th32OwnerProcessID != process) {
			continue;
		}
		HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
		if (thread == nullptr) {
			continue;
		}
		FILETIME created {}, exited {}, kernel {}, user {};
		if (GetThreadTimes(thread, &created, &exited, &kernel, &user) != FALSE) {
			const auto value = [](const FILETIME& time) {
				return (static_cast<uint64_t>(time.dwHighDateTime) << 32u) | time.dwLowDateTime;
			};
			times[entry.th32ThreadID] = (value(kernel) + value(user)) * 100u;
		}
		CloseHandle(thread);
	}
	CloseHandle(snapshot);
#else
	DIR* directory = opendir("/proc/self/task");
	if (directory == nullptr) {
		return times;
	}
	const auto ticks = static_cast<uint64_t>(std::max(sysconf(_SC_CLK_TCK), 1l));
	while (const auto* entry = readdir(directory)) {
		if (entry->d_name[0] < '0' || entry->d_name[0] > '9') {
			continue;
		}
		std::ifstream file(std::string("/proc/self/task/") + entry->d_name + "/stat");
		std::string   line;
		if (!std::getline(file, line)) {
			continue;
		}
		// The fields after the command name (which may hold spaces) are counted from its ')'.
		const auto close = line.rfind(')');
		if (close == std::string::npos) {
			continue;
		}
		std::vector<std::string> fields;
		size_t                   at = close + 2;
		while (at < line.size()) {
			const auto end = line.find(' ', at);
			fields.push_back(line.substr(at, end == std::string::npos ? end : end - at));
			if (end == std::string::npos) {
				break;
			}
			at = end + 1;
		}
		if (fields.size() < 13) {
			continue;
		}
		// utime and stime: fields 14 and 15 of the line, 12 and 13 after the state.
		const auto cpu = std::strtoull(fields[11].c_str(), nullptr, 10) +
		                 std::strtoull(fields[12].c_str(), nullptr, 10);
		times[static_cast<uint32_t>(std::strtoul(entry->d_name, nullptr, 10))] =
		    cpu * 1'000'000'000ull / ticks;
	}
	closedir(directory);
#endif
	return times;
}

std::string Percent(uint64_t part, uint64_t whole) {
	return fmt::format("{:.0f}%",
	                   whole == 0 ? 0.0
	                              : 100.0 * static_cast<double>(part) / static_cast<double>(whole));
}

void Report(double seconds, std::unordered_map<uint32_t, uint64_t>& last_cpu) {
	auto&      c    = g_counters;
	const auto take = [](std::atomic<uint64_t>& value) {
		return value.exchange(0, std::memory_order_relaxed);
	};
	const auto interval = static_cast<uint64_t>(seconds * 1e9);
	const auto idle     = take(c.gpu[static_cast<size_t>(GpuThread::Idle)]);
	const auto blocked  = take(c.gpu[static_cast<size_t>(GpuThread::Blocked)]);
	const auto busy     = take(c.gpu[static_cast<size_t>(GpuThread::Busy)]);
	const auto host_gpu = take(c.gpu[static_cast<size_t>(GpuThread::HostGpuWait)]);
	const auto draws    = take(c.draws);
	const auto dispatch = take(c.dispatches);
	const auto presents = take(c.presents);
	const auto loads    = take(c.downloads);
	const auto bytes    = take(c.download_bytes);
	const auto faults   = take(c.faults);
	const auto fault_ns = take(c.fault_ns);
	const auto reads    = take(c.fault_reads);
	const auto emulated = take(c.fault_emulated);
	const auto longs    = take(c.long_faults);
	const auto longest  = take(c.longest_fault_ns);
	const auto frames   = std::max<uint64_t>(presents, 1);
	const auto ops      = draws + dispatch;

	std::string configuration;
	{
		std::scoped_lock lock(g_configuration_mutex);
		configuration = g_configuration.empty() ? std::string() : " [" + g_configuration + "]";
	}
	Log::WriteUnmuted(fmt::format(
	    "Perf: {:.1f} s, {:.1f} fps{} | GPU thread: busy {} (host GPU {}), idle {}, blocked {} | "
	    "{} draws, {} dispatches a frame, {:.1f} us each | faults {:.0f}/s ({:.0f} reads, "
	    "{:.0f} emulated), {:.0f} ms/s in handlers, {:.0f}/s of 1 ms or more, longest {:.1f} ms "
	    "| downloads {:.0f}/s, {:.1f} MB/s\n",
	    seconds, static_cast<double>(presents) / seconds, configuration, Percent(busy, interval),
	    Percent(host_gpu, interval), Percent(idle, interval), Percent(blocked, interval),
	    draws / frames, dispatch / frames,
	    ops == 0 ? 0.0 : static_cast<double>(busy) / 1000.0 / static_cast<double>(ops),
	    static_cast<double>(faults) / seconds, static_cast<double>(reads) / seconds,
	    static_cast<double>(emulated) / seconds, static_cast<double>(fault_ns) / 1e6 / seconds,
	    static_cast<double>(longs) / seconds, static_cast<double>(longest) / 1e6,
	    static_cast<double>(loads) / seconds, static_cast<double>(bytes) / 1e6 / seconds));

	// The busiest threads, and those that spent longest in faults.
	auto                                       now_cpu = ThreadCpuTimes();
	std::vector<std::pair<uint64_t, uint32_t>> cpu;
	uint64_t                                   total = 0;
	for (const auto& [tid, ns]: now_cpu) {
		const auto it    = last_cpu.find(tid);
		const auto delta = it == last_cpu.end() || ns < it->second ? 0 : ns - it->second;
		total += delta;
		if (delta != 0) {
			cpu.emplace_back(delta, tid);
		}
	}
	last_cpu = std::move(now_cpu);
	std::ranges::sort(cpu, std::greater {});
	std::string line = fmt::format("Perf threads: CPU {:.1f} cores:",
	                               static_cast<double>(total) / static_cast<double>(interval));
	for (size_t i = 0; i < std::min<size_t>(cpu.size(), 10); i++) {
		line += fmt::format("{} {} {}", i == 0 ? "" : ",", ThreadName(cpu[i].second),
		                    Percent(cpu[i].first, interval));
	}
	std::vector<std::pair<uint64_t, uint32_t>> stalled;
	{
		std::scoped_lock lock(g_slots_mutex);
		for (auto& slot: g_slots) {
			const auto ns     = slot->ns.load(std::memory_order_relaxed);
			const auto delta  = ns - slot->reported_ns;
			slot->reported_ns = ns;
			if (delta >= 1'000'000) {
				stalled.emplace_back(delta, slot->tid);
			}
		}
	}
	std::ranges::sort(stalled, std::greater {});
	if (!stalled.empty()) {
		line += " | in faults:";
		for (size_t i = 0; i < std::min<size_t>(stalled.size(), 6); i++) {
			line += fmt::format("{} {} {:.0f} ms", i == 0 ? "" : ",", ThreadName(stalled[i].second),
			                    static_cast<double>(stalled[i].first) / 1e6);
		}
	}
	Log::WriteUnmuted(line + "\n");
}

void Run() {
	const auto interval = std::chrono::seconds(IntervalSeconds());
	auto       last     = std::chrono::steady_clock::now();
	auto       cpu      = ThreadCpuTimes();
	for (;;) {
		std::this_thread::sleep_for(interval);
		const auto now     = std::chrono::steady_clock::now();
		const auto seconds = std::chrono::duration<double>(now - last).count();
		last               = now;
		Report(seconds, cpu);
	}
}

} // namespace

bool Enabled() {
	return IntervalSeconds() != 0;
}

namespace Detail {

void AddGpuThreadSlow(GpuThread phase, uint64_t nanoseconds) {
	g_counters.gpu[static_cast<size_t>(phase)].fetch_add(nanoseconds, std::memory_order_relaxed);
}

void AddDrawSlow() {
	g_counters.draws.fetch_add(1, std::memory_order_relaxed);
}

void AddDispatchSlow() {
	g_counters.dispatches.fetch_add(1, std::memory_order_relaxed);
}

void AddPresentSlow() {
	g_counters.presents.fetch_add(1, std::memory_order_relaxed);
}

void AddDownloadSlow(uint64_t bytes) {
	g_counters.downloads.fetch_add(1, std::memory_order_relaxed);
	g_counters.download_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

void AddFaultSlow(uint64_t nanoseconds, bool read, bool emulated) {
	auto& c = g_counters;
	c.faults.fetch_add(1, std::memory_order_relaxed);
	c.fault_ns.fetch_add(nanoseconds, std::memory_order_relaxed);
	if (read) {
		c.fault_reads.fetch_add(1, std::memory_order_relaxed);
	}
	if (emulated) {
		c.fault_emulated.fetch_add(1, std::memory_order_relaxed);
	}
	if (nanoseconds >= 1'000'000) {
		c.long_faults.fetch_add(1, std::memory_order_relaxed);
	}
	for (auto longest = c.longest_fault_ns.load(std::memory_order_relaxed);
	     nanoseconds > longest && !c.longest_fault_ns.compare_exchange_weak(
	                                  longest, nanoseconds, std::memory_order_relaxed);) {
	}
	if (t_slot == nullptr) {
		auto slot = std::make_unique<FaultSlot>();
		slot->tid = OsThreadId();
		std::scoped_lock lock(g_slots_mutex);
		t_slot = slot.get();
		g_slots.push_back(std::move(slot));
	}
	t_slot->ns.fetch_add(nanoseconds, std::memory_order_relaxed);
}

} // namespace Detail

void NameThread(const char* name) {
	if (name == nullptr || name[0] == '\0') {
		return;
	}
	{
		std::scoped_lock lock(g_names_mutex);
		g_names[OsThreadId()] = name;
	}
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	// Windows 10 1607 and later; looked up so that older systems still start.
	using SetDescription              = HRESULT(WINAPI*)(HANDLE, PCWSTR);
	static const auto set_description = reinterpret_cast<SetDescription>(reinterpret_cast<void*>(
	    GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription")));
	if (set_description != nullptr) {
		wchar_t wide[64] {};
		if (MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, static_cast<int>(std::size(wide))) >
		    0) {
			set_description(GetCurrentThread(), wide);
		}
	}
#endif
}

void SetConfiguration(std::string_view name) {
	std::scoped_lock lock(g_configuration_mutex);
	g_configuration = std::string(name);
}

void Start() {
	static std::once_flag started;
	std::call_once(started, [] {
		if (const char* set = std::getenv("KYTY_CPU_SET"); set != nullptr && set[0] != '\0') {
			std::string description;
			const bool  ok = Common::SetProcessCpuSet(set, &description);
			LOGF("CPU: KYTY_CPU_SET=%s: %s\n", set, description.c_str());
			(void)ok;
		}
		if (!Enabled()) {
			return;
		}
		Detail::g_enabled.store(true, std::memory_order_relaxed);
		LOGF("Perf: a summary every %u s (KYTY_PERF=<seconds>, 0 turns it off)\n",
		     IntervalSeconds());
		std::thread([] {
			NameThread("PerfReport");
			Run();
		}).detach();
	});
}

} // namespace Common::PerfStats
