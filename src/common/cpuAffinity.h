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
// Round 79: also "gpuvcache" (the threads RegisterGpuSideThread named prefer the processors
// sharing the largest L3 cache, and every other thread the rest: Windows CPU sets) and
// "gpufrequency" (the other way round). Round 84: "gpucore", as "gpuvcache" but with the GPU
// thread alone on one logical processor of the V-Cache die's last core, whose other logical
// processor no thread of the process is given.
bool SetProcessCpuSet(const char* which, std::string* description);

// The placement the run starts with: KYTY_CPU_SET when it is set; otherwise, round 80,
// "gpuvcache" on a CPU with a 3D V-Cache die beside another (a Ryzen X3D with two dies: the
// GPU-side threads on the V-Cache die, +1.5 fps in round 79's races), unless the process was
// started with an affinity of its own; else none (empty). `reason` says why. KYTY_CPU_SET=off
// leaves the threads' placement alone, as before round 80; KYTY_CPU_SET=all runs every thread on
// every processor.
[[nodiscard]] std::string StartCpuSet(std::string* reason = nullptr);

// The calling thread feeds the host GPU (the GPU thread, draw preparation, Vulkan recording):
// SetProcessCpuSet's "gpuvcache" and "gpufrequency" place it apart from the other threads.
void RegisterGpuSideThread(const char* name);

// Round 117: the calling thread runs below normal priority, so that the game's threads go first
// (background work such as the driver's compiles of big shaders). Windows:
// THREAD_PRIORITY_BELOW_NORMAL; Linux: a nice value of 5; a no-op elsewhere.
void LowerThreadPriority();

// Round 152: while it lives, the calling thread runs at normal priority at least, so that threads
// waiting for what it is doing (the GPU thread, for a game thread's copy into guest memory) are
// not held up by a low priority the game gave it. Windows (a guest priority of 733 or more runs at
// THREAD_PRIORITY_LOWEST); a no-op elsewhere.
class ThreadPriorityFloor final {
public:
	ThreadPriorityFloor() noexcept;
	~ThreadPriorityFloor();
	ThreadPriorityFloor(const ThreadPriorityFloor&)            = delete;
	ThreadPriorityFloor& operator=(const ThreadPriorityFloor&) = delete;

private:
	[[maybe_unused]] int  m_previous = 0;
	[[maybe_unused]] bool m_raised   = false;
};

} // namespace Common

#endif // EMULATOR_SRC_COMMON_CPUAFFINITY_H_
