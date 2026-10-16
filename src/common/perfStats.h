#ifndef KYTY_COMMON_PERFSTATS_H_
#define KYTY_COMMON_PERFSTATS_H_

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Diagnostics: every KYTY_PERF seconds (default 2, 0 turns it off) a "Perf:" line with the
// presented frame rate, how the GPU thread spent its time, the draws and dispatches it recorded,
// the time threads spent in tracked-memory faults (GPU readbacks among them), a "Perf threads:"
// line with the busiest threads and the ones that waited longest in faults, and a "Perf GPU:" line
// with how far behind the host GPU runs, what a game thread's access to GPU-written memory waits
// for, the stalls (waits of KYTY_STALL_MS or more for the host GPU, renderer/stallWatch.h), the
// longest swapchain acquire, present and queue lock wait, and the video memory in use. Counting is
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
void AddReadbackSlow(uint64_t pickup_ns, uint64_t record_ns, uint64_t wait_ns, uint32_t steps,
                     uint64_t ticks_behind);
void AddCleanStoreSlow(uint64_t nanoseconds);
void AddDeclinedStoreSlow();
void AddUnsupportedStoreSlow();
void TickSubmittedSlow(uint64_t tick);
void TickCompletedSlow(uint64_t tick);
void AddStallSlow(uint64_t nanoseconds, bool distinct);
void AddHostSlow(uint32_t which, uint64_t nanoseconds);
} // namespace Detail

// Whether the counters count (the reporter runs): for measurements that cost a clock read.
[[nodiscard]] inline bool Active() {
	return Detail::g_enabled.load(std::memory_order_relaxed);
}

inline void AddGpuThread(GpuThread phase, uint64_t nanoseconds) {
	if (Active()) {
		Detail::AddGpuThreadSlow(phase, nanoseconds);
	}
}
inline void AddDraw() {
	if (Active()) {
		Detail::AddDrawSlow();
	}
}
inline void AddDispatch() {
	if (Active()) {
		Detail::AddDispatchSlow();
	}
}
inline void AddPresent() {
	if (Active()) {
		Detail::AddPresentSlow();
	}
}
// A game thread's download of GPU-written memory (`bytes` downloaded).
inline void AddDownload(uint64_t bytes) {
	if (Active()) {
		Detail::AddDownloadSlow(bytes);
	}
}
// A tracked-memory fault the calling thread spent `nanoseconds` in. `emulated`: an access the
// handler completed itself, without lifting the protection (a read of bytes the GPU did not
// write, or a store done in place).
inline void AddFault(uint64_t nanoseconds, bool read, bool emulated) {
	if (Active()) {
		Detail::AddFaultSlow(nanoseconds, read, emulated);
	}
}
// A game thread's access to GPU-written memory that went through the GPU thread
// (BufferCache::ReadMemory): the time until the GPU thread took the request up and the time it
// spent on it (all its steps), the time the access then waited for downloads to land, the steps,
// and how many command buffers the host GPU had not finished when the request was taken up.
inline void AddReadback(uint64_t pickup_ns, uint64_t record_ns, uint64_t wait_ns, uint32_t steps,
                        uint64_t ticks_behind) {
	if (Active()) {
		Detail::AddReadbackSlow(pickup_ns, record_ns, wait_ns, steps, ticks_behind);
	}
}
// A store to a page protected because the GPU wrote to it, done in place (both copies, on the GPU
// thread) instead of downloading the page first: `nanoseconds` in the handler.
inline void AddCleanStore(uint64_t nanoseconds) {
	if (Active()) {
		Detail::AddCleanStoreSlow(nanoseconds);
	}
}
// Such a store taken the old way: declined (the page's budget, or bytes on their way back from the
// GPU), or an instruction the handler does not complete itself.
inline void AddDeclinedStore() {
	if (Active()) {
		Detail::AddDeclinedStoreSlow();
	}
}
inline void AddUnsupportedStore() {
	if (Active()) {
		Detail::AddUnsupportedStoreSlow();
	}
}
// The host GPU's queue: a command buffer submitted with `tick`, and a thread that waited for that
// tick seeing it complete (the priority-operation thread).
inline void TickSubmitted(uint64_t tick) {
	if (Active()) {
		Detail::TickSubmittedSlow(tick);
	}
}
inline void TickCompleted(uint64_t tick) {
	if (Active()) {
		Detail::TickCompletedSlow(tick);
	}
}
// A wait for the host GPU of KYTY_STALL_MS or more (renderer/stallWatch.h); `distinct` when it
// began after the last one counted ended (other threads waiting through the same stall are not
// counted again, only their waits for the longest).
inline void AddStall(uint64_t nanoseconds, bool distinct) {
	if (Active()) {
		Detail::AddStallSlow(nanoseconds, distinct);
	}
}
// Host-side sections that can hold the GPU's queue up; the line gives the longest of each.
enum class Host : uint32_t { Acquire, Present, QueueLock, Count };
inline void AddHost(Host which, uint64_t nanoseconds) {
	if (Active()) {
		Detail::AddHostSlow(static_cast<uint32_t>(which), nanoseconds);
	}
}
// Text appended to the Perf GPU line (the renderer's video memory), called on the reporter's
// thread; null for none.
using ExtraReporter = std::string (*)();
void SetExtraReporter(ExtraReporter reporter);

// Names the calling thread for the thread report, and for the OS (debuggers and Process Explorer)
// where it can.
void NameThread(const char* name);

// The OS id of the calling thread; the name NameThread gave a thread ("tid <id>" when none); every
// named thread (OS id, name).
[[nodiscard]] uint32_t                                      CurrentOsThreadId();
[[nodiscard]] std::string                                   ThreadNameOf(uint32_t tid);
[[nodiscard]] std::vector<std::pair<uint32_t, std::string>> NamedThreads();

// The configuration the lines name (KYTY_BISECT's step), empty for none.
void SetConfiguration(std::string_view name);

// Starts the reporter (the first call; later ones do nothing), and the sampler when KYTY_SAMPLE
// asks for it (common/sampler.h).
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
