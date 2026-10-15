#include "common/cpuAffinity.h"

#include "common/common.h"
#include "common/logging/log.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fmt/format.h>
#include <string>
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

bool SetProcessCpuSet(const char* which, std::string* description) {
	const auto describe = [&](std::string text) {
		if (description != nullptr) {
			*description = std::move(text);
		}
	};
	DWORD_PTR process_mask = 0, system_mask = 0;
	if (which == nullptr ||
	    GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask) == FALSE) {
		describe("the process's processors are unknown");
		return false;
	}
	// The L3 caches of processor group 0 (the process's, with up to 64 logical processors) and
	// the processors sharing each.
	struct Cache {
		DWORD_PTR mask = 0;
		DWORD     size = 0;
	};
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
	if (name == "all") {
		mask = system_mask;
		what = "every processor";
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
		describe("not changed: KYTY_CPU_SET takes vcache, frequency or all");
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

#else

void PreferPerformanceCores(const char* /*name*/) {}

bool SetProcessCpuSet(const char* /*which*/, std::string* description) {
	if (description != nullptr) {
		*description = "not changed: only on Windows";
	}
	return false;
}

#endif

} // namespace Common
