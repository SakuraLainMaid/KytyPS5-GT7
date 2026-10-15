#ifndef KYTY_COMMON_PERFSTATS_H_
#define KYTY_COMMON_PERFSTATS_H_

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

// Diagnostics: every KYTY_PERF seconds (default 2, 0 turns it off) a "Perf:" line with the
// presented frame rate, how the GPU thread spent its time, the draws and dispatches it recorded,
// the time threads spent in tracked-memory faults (GPU readbacks among them), and a "Perf
// threads:" line with the busiest threads and the ones that waited longest in faults. Counting is
// a relaxed atomic add; the reporter thread does the rest.
namespace Common::PerfStats {

[[nodiscard]] bool Enabled();

// The GPU thread's time: waiting for work (the guest has submitted nothing), waiting on a blocked
// queue, and processing; HostGpuWait is the part of processing it spent waiting for the host GPU.
enum class GpuThread : uint32_t { Idle, Blocked, Busy, HostGpuWait, Count };

namespace Detail {
inline std::atomic<bool> g_enabled {false};

void AddGpuThreadSlow(GpuThread phase, uint64_t nanoseconds);
void AddDrawSlow();
void AddDispatchSlow();
void AddPresentSlow();
void AddDownloadSlow(uint64_t bytes);
void AddFaultSlow(uint64_t nanoseconds, bool read, bool emulated);
} // namespace Detail

inline void AddGpuThread(GpuThread phase, uint64_t nanoseconds) {
	if (Detail::g_enabled.load(std::memory_order_relaxed)) {
		Detail::AddGpuThreadSlow(phase, nanoseconds);
	}
}
inline void AddDraw() {
	if (Detail::g_enabled.load(std::memory_order_relaxed)) {
		Detail::AddDrawSlow();
	}
}
inline void AddDispatch() {
	if (Detail::g_enabled.load(std::memory_order_relaxed)) {
		Detail::AddDispatchSlow();
	}
}
inline void AddPresent() {
	if (Detail::g_enabled.load(std::memory_order_relaxed)) {
		Detail::AddPresentSlow();
	}
}
// A game thread's download of GPU-written memory (`bytes` downloaded).
inline void AddDownload(uint64_t bytes) {
	if (Detail::g_enabled.load(std::memory_order_relaxed)) {
		Detail::AddDownloadSlow(bytes);
	}
}
// A tracked-memory fault the calling thread spent `nanoseconds` in. `emulated`: a read the handler
// completed itself, without lifting the protection.
inline void AddFault(uint64_t nanoseconds, bool read, bool emulated) {
	if (Detail::g_enabled.load(std::memory_order_relaxed)) {
		Detail::AddFaultSlow(nanoseconds, read, emulated);
	}
}

// Names the calling thread for the thread report, and for the OS (debuggers and Process Explorer)
// where it can.
void NameThread(const char* name);

// The configuration the lines name (KYTY_BISECT's step), empty for none.
void SetConfiguration(std::string_view name);

// Starts the reporter (the first call; later ones do nothing).
void Start();

} // namespace Common::PerfStats

namespace Log {
// Diagnostics: while muted, LOGF writes nothing (KYTY_BISECT's log:off, which measures what the
// log costs). The Perf lines are written anyway.
void SetMuted(bool muted);
// Writes a line even while muted.
void WriteUnmuted(std::string_view text);
} // namespace Log

#endif // KYTY_COMMON_PERFSTATS_H_
