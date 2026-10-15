#ifndef EMULATOR_SRC_COMMON_CPUAFFINITY_H_
#define EMULATOR_SRC_COMMON_CPUAFFINITY_H_

#include <string>

namespace Common {

// On a CPU with cores of different speeds (Intel 12th-14th gen and later hybrid designs), keeps
// the calling thread on the fastest cores: the emulator's GPU thread is its bottleneck, and on an
// efficiency core it ran ~1.7x slower per operation (i7-13700F on Linux, 2026-10-06). Windows:
// CPU sets of the highest efficiency class (a scheduler preference). Linux: the cores with the
// highest maximum frequency. A no-op on CPUs whose cores are all alike, on macOS, and with
// KYTY_PCORE_AFFINITY=0. `name` is for the log line.
void PreferPerformanceCores(const char* name);

// Keeps every thread of the process on one set of logical processors: "vcache" (those sharing
// the largest L3 cache: the 3D V-Cache die of a Ryzen X3D, or the first die when they are alike),
// "frequency" (the others) or "all" (every processor the system lets the process use). Windows
// only; returns false and says why in `description` otherwise, or for a name it does not know.
// For KYTY_CPU_SET (the whole run) and KYTY_BISECT's cpu: steps (an A/B test within one).
bool SetProcessCpuSet(const char* which, std::string* description);

} // namespace Common

#endif // EMULATOR_SRC_COMMON_CPUAFFINITY_H_
