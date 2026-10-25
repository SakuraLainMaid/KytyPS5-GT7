#include "common/cpuAffinity.h"

#include "common/common.h"
#include "common/logging/log.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fmt/format.h>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // IWYU pragma: keep
#elif !defined(__APPLE__)
#include <fstream>
#include <pthread.h>
#include <sched.h>
#include <string>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace Common {

static bool Enabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_PCORE_AFFINITY");
		return value == nullptr || value[0] != '0';
	}();
	return enabled;
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS

void PreferPerformanceCores(const char* name) {
	if (!Enabled()) {
		return;
	}
	ULONG length = 0;
	GetSystemCpuSetInformation(nullptr, 0, &length, GetCurrentProcess(), 0);
	if (length == 0) {
		return;
	}
	std::vector<uint8_t> buffer(length);
	auto* first = reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data());
	if (!GetSystemCpuSetInformation(first, length, &length, GetCurrentProcess(), 0)) {
		return;
	}
	struct Set {
		ULONG id;
		BYTE  efficiency;
	};
	std::vector<Set> sets;
	BYTE             lowest = 0xff, highest = 0;
	for (ULONG offset = 0; offset < length;) {
		const auto* info = reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data() + offset);
		if (info->Size == 0) {
			break;
		}
		if (info->Type == CpuSetInformation) {
			sets.push_back({info->CpuSet.Id, info->CpuSet.EfficiencyClass});
			lowest  = std::min(lowest, info->CpuSet.EfficiencyClass);
			highest = std::max(highest, info->CpuSet.EfficiencyClass);
		}
		offset += info->Size;
	}
	if (sets.empty() || lowest == highest) {
		return; // All cores alike.
	}
	std::vector<ULONG> fast;
	for (const auto& set: sets) {
		if (set.efficiency == highest) {
			fast.push_back(set.id);
		}
	}
	const bool ok =
	    SetThreadSelectedCpuSets(GetCurrentThread(), fast.data(), static_cast<ULONG>(fast.size())) != 0;
	LOGF("CPU: %s prefers the %zu fastest of %zu logical processors (efficiency class %u)%s\n", name,
	     fast.size(), sets.size(), static_cast<unsigned>(highest), ok ? "" : " - failed");
}

namespace {

struct GpuSideThread {
	HANDLE      handle = nullptr;
	std::string name;
	// Its own selection when it registered (PreferPerformanceCores'), restored by ClearSplit.
	std::vector<ULONG> own_sets;
};

std::mutex                 g_gpu_side_mutex;
std::vector<GpuSideThread> g_gpu_side_threads;
// Whether "gpuvcache" or "gpufrequency" set CPU sets that a later choice has to clear, and the
// sets they gave the GPU-side threads (a thread registered later gets them too).
bool               g_split_applied = false;
std::vector<ULONG> g_split_gpu_ids;
// "gpucore" (round 84): the GPU thread's own set, one logical processor of a core no other
// thread of the process is placed on.
std::vector<ULONG> g_split_gpu_thread_ids;

// The thread the GPU thread's own set goes to ("gpucore").
bool IsGpuThread(const std::string& name) {
	return name == "Thread_Gpu";
}

// The logical processors of each core of processor group 0 that lies wholly in `mask`.
std::vector<DWORD_PTR> CoresIn(DWORD_PTR mask) {
	std::vector<DWORD_PTR> cores;
	DWORD                  length = 0;
	GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
	std::vector<uint8_t> buffer(length);
	if (length == 0 ||
	    GetLogicalProcessorInformationEx(
	        RelationProcessorCore,
	        reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()),
	        &length) == FALSE) {
		return cores;
	}
	for (DWORD offset = 0; offset < length;) {
		const auto* info =
		    reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data() + offset);
		if (info->Size == 0) {
			break;
		}
		if (info->Relationship == RelationProcessorCore && info->Processor.GroupCount >= 1 &&
		    info->Processor.GroupMask[0].Group == 0) {
			const auto core = info->Processor.GroupMask[0].Mask;
			if (core != 0 && (core & ~mask) == 0) {
				cores.push_back(core);
			}
		}
		offset += info->Size;
	}
	return cores;
}

// The process's affinity before the emulator changed it ("start" gives it back), taken at the
// first look.
DWORD_PTR g_start_mask = 0;
void      NoteStartMask(DWORD_PTR process_mask) {
    if (g_start_mask == 0) {
        g_start_mask = process_mask;
    }
}

// The CPU set ids of the logical processors of group 0 in `mask`.
std::vector<ULONG> CpuSetIds(DWORD_PTR mask) {
	std::vector<ULONG> ids;
	ULONG              length = 0;
	GetSystemCpuSetInformation(nullptr, 0, &length, GetCurrentProcess(), 0);
	if (length == 0) {
		return ids;
	}
	std::vector<uint8_t> buffer(length);
	if (!GetSystemCpuSetInformation(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data()),
	                                length, &length, GetCurrentProcess(), 0)) {
		return ids;
	}
	for (ULONG offset = 0; offset < length;) {
		const auto* info = reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data() + offset);
		if (info->Size == 0) {
			break;
		}
		if (info->Type == CpuSetInformation && info->CpuSet.Group == 0 &&
		    info->CpuSet.LogicalProcessorIndex < sizeof(DWORD_PTR) * 8u &&
		    (mask & (DWORD_PTR {1} << info->CpuSet.LogicalProcessorIndex)) != 0) {
			ids.push_back(info->CpuSet.Id);
		}
		offset += info->Size;
	}
	return ids;
}

// The L3 caches of processor group 0 (the process's, with up to 64 logical processors) and the
// processors of `system_mask` sharing each.
struct Cache {
	DWORD_PTR mask = 0;
	DWORD     size = 0;
};
std::vector<Cache> L3Caches(DWORD_PTR system_mask) {
	std::vector<Cache> caches;
	DWORD              length = 0;
	GetLogicalProcessorInformationEx(RelationCache, nullptr, &length);
	std::vector<uint8_t> buffer(length);
	if (length != 0 &&
	    GetLogicalProcessorInformationEx(
	        RelationCache,
	        reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()),
	        &length) != FALSE) {
		for (DWORD offset = 0; offset < length;) {
			const auto* info =
			    reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data() + offset);
			if (info->Size == 0) {
				break;
			}
			if (info->Relationship == RelationCache && info->Cache.Level == 3 &&
			    info->Cache.GroupMask.Group == 0 &&
			    (info->Cache.GroupMask.Mask & system_mask) != 0) {
				caches.push_back({info->Cache.GroupMask.Mask & system_mask, info->Cache.CacheSize});
			}
			offset += info->Size;
		}
	}
	return caches;
}

// Clears what a split placement set: the process's default CPU sets and the registered threads'.
void ClearSplit() {
	if (!g_split_applied) {
		return;
	}
	SetProcessDefaultCpuSets(GetCurrentProcess(), nullptr, 0);
	for (const auto& thread: g_gpu_side_threads) {
		SetThreadSelectedCpuSets(thread.handle,
		                         thread.own_sets.empty() ? nullptr : thread.own_sets.data(),
		                         static_cast<ULONG>(thread.own_sets.size()));
	}
	g_split_applied = false;
	g_split_gpu_ids.clear();
	g_split_gpu_thread_ids.clear();
}

} // namespace

void RegisterGpuSideThread(const char* name) {
	HANDLE handle = OpenThread(THREAD_SET_LIMITED_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION,
	                           FALSE, GetCurrentThreadId());
	if (handle == nullptr) {
		return;
	}
	GpuSideThread thread {handle, name != nullptr ? std::string(name) : std::string(), {}};
	ULONG         count = 0;
	if (GetThreadSelectedCpuSets(handle, nullptr, 0, &count) == FALSE && count != 0) {
		thread.own_sets.resize(count);
		if (GetThreadSelectedCpuSets(handle, thread.own_sets.data(), count, &count) == FALSE) {
			thread.own_sets.clear();
		}
		thread.own_sets.resize(std::min<size_t>(count, thread.own_sets.size()));
	}
	std::lock_guard lock(g_gpu_side_mutex);
	// Registered after a split was applied: placed with the others (the GPU thread on its own
	// core with "gpucore").
	const auto& ids = !g_split_gpu_thread_ids.empty() && IsGpuThread(thread.name)
	                      ? g_split_gpu_thread_ids
	                      : g_split_gpu_ids;
	if (g_split_applied && !ids.empty()) {
		SetThreadSelectedCpuSets(handle, ids.data(), static_cast<ULONG>(ids.size()));
	}
	g_gpu_side_threads.push_back(std::move(thread));
}

std::string StartCpuSet(std::string* reason) {
	const auto because = [&](std::string text) {
		if (reason != nullptr) {
			*reason = std::move(text);
		}
	};
	if (const char* value = std::getenv("KYTY_CPU_SET"); value != nullptr && value[0] != '\0') {
		if (std::string_view(value) == "off") {
			because("KYTY_CPU_SET=off");
			return {};
		}
		because("KYTY_CPU_SET");
		return value;
	}
	DWORD_PTR process_mask = 0, system_mask = 0;
	if (GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask) == FALSE) {
		because("the process's processors are unknown");
		return {};
	}
	{
		std::lock_guard lock(g_gpu_side_mutex);
		NoteStartMask(process_mask);
	}
	// An affinity given before the start (Process Lasso, start /affinity, a launcher) is the
	// user's choice: the placement would undo it.
	const auto count = [](DWORD_PTR mask) {
		uint32_t n = 0;
		for (; mask != 0; mask &= mask - 1) {
			n++;
		}
		return n;
	};
	if (process_mask != system_mask) {
		because(fmt::format("the process was started on {} of the {} processors: left as it is",
		                    count(process_mask), count(system_mask)));
		return {};
	}
	const auto caches = L3Caches(system_mask);
	if (caches.size() < 2) {
		because(fmt::format("the processors share {} L3 cache(s)", caches.size()));
		return {};
	}
	const auto [smallest, largest] = std::ranges::minmax_element(
	    caches, [](const Cache& a, const Cache& b) { return a.size < b.size; });
	if (smallest->size == largest->size) {
		because(fmt::format("their {} L3 caches are alike ({} MB each)", caches.size(),
		                    largest->size >> 20u));
		return {};
	}
	// A 3D V-Cache die has 96 MB of L3 (the other die of the same chip 32 MB). Chips whose
	// clusters differ otherwise (a mobile chip's compact cores with 8 MB beside 16 MB) are left
	// alone: their small cluster is the slower one, not the one to keep the game's threads off.
	constexpr DWORD VCacheBytes = 64u << 20u;
	if (largest->size < VCacheBytes) {
		because(fmt::format("their L3 caches differ ({} MB and {} MB), but none is a 3D V-Cache "
		                    "die's",
		                    largest->size >> 20u, smallest->size >> 20u));
		return {};
	}
	because(fmt::format("their L3 caches differ ({} MB and {} MB)", largest->size >> 20u,
	                    smallest->size >> 20u));
	return "gpuvcache";
}

bool SetProcessCpuSet(const char* which, std::string* description) {
	const auto describe = [&](std::string text) {
		if (description != nullptr) {
			*description = std::move(text);
		}
	};
	std::lock_guard gpu_side_lock(g_gpu_side_mutex);
	DWORD_PTR process_mask = 0, system_mask = 0;
	if (which == nullptr ||
	    GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask) == FALSE) {
		describe("the process's processors are unknown");
		return false;
	}
	NoteStartMask(process_mask);
	const auto caches = L3Caches(system_mask);
	const auto count = [](DWORD_PTR mask) {
		uint32_t n = 0;
		for (; mask != 0; mask &= mask - 1) {
			n++;
		}
		return n;
	};
	const std::string name(which);
	DWORD_PTR         mask = 0;
	std::string       what;
	if (name == "gpuvcache" || name == "gpufrequency" || name == "gpucore") {
		if (caches.size() < 2) {
			describe(
			    fmt::format("not changed: the processors share {} L3 cache(s)", caches.size()));
			return false;
		}
		const auto largest = std::ranges::max_element(
		    caches, [](const Cache& a, const Cache& b) { return a.size < b.size; });
		const DWORD_PTR vcache   = largest->mask;
		const DWORD_PTR others   = system_mask & ~vcache;
		const DWORD_PTR gpu_side = name == "gpufrequency" ? others : vcache;
		const DWORD_PTR rest     = name == "gpufrequency" ? vcache : others;
		// "gpucore" (round 84): as "gpuvcache", but the GPU thread alone on the last core of the
		// V-Cache die (one of its logical processors; the core's other one, its SMT sibling, is
		// left to no thread of the process), the other GPU-side threads on the die's other cores.
		// The GPU thread is the emulator's bottleneck; a sibling's work slows a core's other
		// thread by up to a third.
		DWORD_PTR gpu        = gpu_side;
		DWORD_PTR gpu_thread = 0;
		if (name == "gpucore") {
			const auto cores = CoresIn(gpu_side);
			if (cores.size() < 2) {
				describe(fmt::format("not changed: {} whole core(s) share the largest L3 cache",
				                     cores.size()));
				return false;
			}
			const auto core = cores.back();
			gpu_thread      = core & (~core + 1u); // its lowest logical processor
			gpu             = gpu_side & ~core;
		}
		const auto gpu_ids        = CpuSetIds(gpu);
		const auto rest_ids       = CpuSetIds(rest);
		const auto gpu_thread_ids = gpu_thread != 0 ? CpuSetIds(gpu_thread) : std::vector<ULONG> {};
		if (gpu_ids.empty() || rest_ids.empty() || (gpu_thread != 0 && gpu_thread_ids.empty())) {
			describe("not changed: no CPU sets");
			return false;
		}
		// A whole-process mask from an earlier step would leave the sets nothing to choose from.
		if (SetProcessAffinityMask(GetCurrentProcess(), system_mask) == FALSE ||
		    SetProcessDefaultCpuSets(GetCurrentProcess(), rest_ids.data(),
		                             static_cast<ULONG>(rest_ids.size())) == FALSE) {
			describe("not changed: the process's CPU sets were refused");
			return false;
		}
		g_split_applied        = true;
		g_split_gpu_ids        = gpu_ids;
		g_split_gpu_thread_ids = gpu_thread_ids;
		std::string threads;
		size_t      placed = 0;
		for (const auto& thread: g_gpu_side_threads) {
			const auto& ids =
			    !gpu_thread_ids.empty() && IsGpuThread(thread.name) ? gpu_thread_ids : gpu_ids;
			if (SetThreadSelectedCpuSets(thread.handle, ids.data(),
			                             static_cast<ULONG>(ids.size())) != FALSE) {
				placed++;
				threads += (threads.empty() ? "" : ", ") + thread.name;
			}
		}
		describe(fmt::format(
		    "{} of {} GPU-side threads ({}; any registered later as well) prefer the {} "
		    "processors {} (mask 0x{:x}){}, the other threads the {} others (mask 0x{:x})",
		    placed, g_gpu_side_threads.size(), threads, count(gpu),
		    name != "gpufrequency"
		        ? fmt::format("sharing the largest L3 cache ({} MB)", largest->size >> 20u)
		        : std::string("outside the largest L3 cache"),
		    gpu,
		    gpu_thread != 0
		        ? fmt::format(" and the GPU thread alone on processor 0x{:x} of the core 0x{:x} "
		                      "no other thread uses",
		                      gpu_thread, gpu_side & ~gpu)
		        : std::string(),
		    count(rest), rest));
		return true;
	}
	ClearSplit();
	if (name == "all") {
		mask = system_mask;
		what = "every processor";
	} else if (name == "start") {
		// Back to what the process started with (a KYTY_BISECT step's end).
		mask = g_start_mask != 0 ? g_start_mask : system_mask;
		what = "the processors it started on";
	} else if (name == "vcache" || name == "frequency") {
		if (caches.size() < 2) {
			describe(
			    fmt::format("not changed: the processors share {} L3 cache(s)", caches.size()));
			return false;
		}
		const auto largest = std::ranges::max_element(
		    caches, [](const Cache& a, const Cache& b) { return a.size < b.size; });
		for (const auto& cache: caches) {
			const bool chosen = &cache == &*largest;
			if (chosen == (name == "vcache")) {
				mask |= cache.mask;
			}
		}
		what = name == "vcache" ? fmt::format("the processors sharing the largest L3 cache ({} MB)",
		                                      largest->size >> 20u)
		                        : std::string("the processors outside the largest L3 cache");
	} else {
		describe("not changed: KYTY_CPU_SET takes vcache, frequency, all, gpuvcache, gpufrequency, "
		         "gpucore or off");
		return false;
	}
	if (mask == 0 || SetProcessAffinityMask(GetCurrentProcess(), mask) == FALSE) {
		describe(fmt::format("not changed: the mask 0x{:x} was refused", mask));
		return false;
	}
	describe(fmt::format("the process runs on {}: {} of {} logical processors (mask 0x{:x})", what,
	                     count(mask), count(system_mask), mask));
	return true;
}

#elif !defined(__APPLE__)

void RegisterGpuSideThread(const char* /*name*/) {}

void PreferPerformanceCores(const char* name) {
	if (!Enabled()) {
		return;
	}
	const long count = sysconf(_SC_NPROCESSORS_CONF);
	if (count <= 0) {
		return;
	}
	std::vector<unsigned long> max_khz(static_cast<size_t>(count), 0);
	unsigned long              lowest = ~0ul, highest = 0;
	for (long cpu = 0; cpu < count; cpu++) {
		std::ifstream file("/sys/devices/system/cpu/cpu" + std::to_string(cpu) +
		                   "/cpufreq/cpuinfo_max_freq");
		unsigned long khz = 0;
		if (!(file >> khz) || khz == 0) {
			return; // Unknown: leave the scheduler alone.
		}
		max_khz[static_cast<size_t>(cpu)] = khz;
		lowest                            = std::min(lowest, khz);
		highest                           = std::max(highest, khz);
	}
	// Hybrid only: efficiency cores top out well below the performance cores (Raptor Lake 4.1
	// against 5.2 GHz), while one CPU's preferred cores differ by a few percent at most.
	if (lowest * 100ul >= highest * 85ul) {
		return;
	}
	cpu_set_t set;
	CPU_ZERO(&set);
	size_t fast = 0;
	for (long cpu = 0; cpu < count; cpu++) {
		if (max_khz[static_cast<size_t>(cpu)] * 100ul >= highest * 85ul) {
			CPU_SET(static_cast<int>(cpu), &set);
			fast++;
		}
	}
	const bool ok = pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
	LOGF("CPU: %s runs on the %zu fastest of %ld logical processors (up to %lu MHz)%s\n", name, fast,
	     count, highest / 1000ul, ok ? "" : " - failed");
}

bool SetProcessCpuSet(const char* /*which*/, std::string* description) {
	if (description != nullptr) {
		*description = "not changed: only on Windows";
	}
	return false;
}

std::string StartCpuSet(std::string* reason) {
	if (const char* value = std::getenv("KYTY_CPU_SET");
	    value != nullptr && value[0] != '\0' && std::string_view(value) != "off") {
		if (reason != nullptr) {
			*reason = "KYTY_CPU_SET";
		}
		return value;
	}
	if (reason != nullptr) {
		*reason = "only on Windows";
	}
	return {};
}

#else

void PreferPerformanceCores(const char* /*name*/) {}

void RegisterGpuSideThread(const char* /*name*/) {}

bool SetProcessCpuSet(const char* /*which*/, std::string* description) {
	if (description != nullptr) {
		*description = "not changed: only on Windows";
	}
	return false;
}

std::string StartCpuSet(std::string* reason) {
	if (const char* value = std::getenv("KYTY_CPU_SET");
	    value != nullptr && value[0] != '\0' && std::string_view(value) != "off") {
		if (reason != nullptr) {
			*reason = "KYTY_CPU_SET";
		}
		return value;
	}
	if (reason != nullptr) {
		*reason = "only on Windows";
	}
	return {};
}

#endif

void LowerThreadPriority() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#elif !defined(__APPLE__)
	(void)setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 5);
#endif
}

ThreadPriorityFloor::ThreadPriorityFloor() noexcept {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	const int current = GetThreadPriority(GetCurrentThread());
	if (current != THREAD_PRIORITY_ERROR_RETURN && current < THREAD_PRIORITY_NORMAL &&
	    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL) != 0) {
		m_previous = current;
		m_raised   = true;
	}
#endif
}

ThreadPriorityFloor::~ThreadPriorityFloor() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (m_raised) {
		SetThreadPriority(GetCurrentThread(), m_previous);
	}
#endif
}

} // namespace Common
