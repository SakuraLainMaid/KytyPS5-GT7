#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_REGIONMANAGER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_REGIONMANAGER_H_

#include "common/assert.h"
#include "common/perfStats.h"
#include "graphics/host_gpu/pageManager.h"
#include "graphics/host_gpu/regionDefinitions.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <utility>

#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef min
#undef max
#undef MemoryBarrier
#elif defined(__APPLE__)
#include <pthread.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace Libs::Graphics {

// The lock of the texture cache and of each memory-tracker region. Every CPU write fault takes the
// texture cache's (TextureCache::InvalidateMemory), and the GPU thread holds it for each image it
// looks up: in GT7 the game's six job threads spent near half their time in fault handlers, all
// about as long as one another (round 64), as if queued behind the GPU thread. A waiter used to
// spin on test-and-set, writing the lock's cache line in a loop under the holder and taking the
// processor from the holder when they share a core. It now reads the lock until it is free,
// pausing, and yields the processor after a while (KYTY_LOCKS_SPIN=1 or KYTY_BISECT's locks:spin
// for the old way); the waits are counted on the "Perf locks:" line.
class TrackingSpinLock final {
public:
	explicit TrackingSpinLock(
	    Common::PerfStats::Lock kind = Common::PerfStats::Lock::MemoryTracker) noexcept
	    : m_kind(kind) {}
	KYTY_CLASS_NO_COPY(TrackingSpinLock);

	void lock() noexcept {
		const auto thread = CurrentThread();
		if (m_lock.exchange(true, std::memory_order_acquire)) {
			Wait(thread);
		}
		m_owner.store(thread, std::memory_order_relaxed);
	}
	void unlock() noexcept {
		EXIT_NOT_IMPLEMENTED(m_owner.load(std::memory_order_relaxed) != CurrentThread());
		m_owner.store(0, std::memory_order_relaxed);
		m_lock.store(false, std::memory_order_release);
	}

private:
	// Held by another thread (by this one: a recursive lock ends the emulator).
	void Wait(uint32_t thread) noexcept {
		EXIT_NOT_IMPLEMENTED(m_owner.load(std::memory_order_relaxed) == thread);
		const bool timed = Common::PerfStats::Active();
		const auto start =
		    timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point {};
		if (Common::PerfStats::SpinLocks()) {
			while (m_lock.exchange(true, std::memory_order_acquire)) {
				EXIT_NOT_IMPLEMENTED(m_owner.load(std::memory_order_relaxed) == thread);
				std::atomic_signal_fence(std::memory_order_seq_cst);
			}
		} else {
			uint32_t spins = 0;
			do {
				while (m_lock.load(std::memory_order_relaxed)) {
					if (spins < 256) {
						spins++;
						Pause();
					} else {
						std::this_thread::yield();
					}
				}
			} while (m_lock.exchange(true, std::memory_order_acquire));
		}
		if (timed) {
			Common::PerfStats::AddLockWait(
			    m_kind, static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
			                                      std::chrono::steady_clock::now() - start)
			                                      .count()));
		}
	}

	static void Pause() noexcept {
#if defined(_M_X64) || defined(__x86_64__)
		_mm_pause();
#else
		std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
	}

	static uint32_t CurrentThread() noexcept {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		return GetCurrentThreadId();
#elif defined(__APPLE__)
		// mach thread port is a nonzero per-thread id (0 is the "no owner" sentinel).
		return static_cast<uint32_t>(pthread_mach_thread_np(pthread_self()));
#elif defined(__linux__)
		static thread_local const uint32_t tid = static_cast<uint32_t>(::syscall(SYS_gettid));
		return tid;
#else
		EXIT("region tracking thread identity is unsupported on this platform\n");
#endif
	}

	std::atomic<bool>       m_lock {false};
	std::atomic_uint32_t    m_owner {0};
	Common::PerfStats::Lock m_kind;
};

static_assert(std::atomic_uint32_t::is_always_lock_free);

// Bumped whenever pages become CPU-dirty anywhere (a write fault, an untrack, an
// invalidation). A consumer that must see every CPU write, such as the pre-dispatch sync of
// all buffers for global-memory shaders, can skip its walk while this has not moved.
inline std::atomic<uint64_t> g_cpu_dirty_epoch {1};

// Bumped by the GPU thread when it starts a guest submission (its first slice).
inline std::atomic<uint64_t> g_guest_submission_seq {1};

class RegionManager final {
public:
	RegionManager(PageManager& page_manager, uint64_t cpu_addr)
	    : m_page_manager(page_manager), m_cpu_addr(cpu_addr) {
		if (m_cpu_addr % TRACKER_REGION_SIZE != 0) {
			EXIT("invalid region tracking manager construction\n");
		}
		m_cpu_dirty.Fill();
		m_writable.Fill();
		m_readable.Fill();
	}

	KYTY_CLASS_NO_COPY(RegionManager);

	[[nodiscard]] uint64_t GetCpuAddr() const { return m_cpu_addr; }

	// Lock-free "any page in this region may be dirty" summaries. A binding that spans a
	// whole heap covers hundreds of regions; without these every draw locks and walks each
	// one. False is exact (set only after a walk saw no bits); true is conservative.
	template <DirtySource source>
	[[nodiscard]] bool MaybeModified() const noexcept {
		return GetSummary<source>().load(std::memory_order_acquire);
	}
	template <DirtySource source>
	[[nodiscard]] bool IsModified(uint64_t offset, uint64_t size) const {
		const auto [start, end] = GetPageRange(m_cpu_addr + offset, size);
		return GetBits<source>().FirstRangeFrom(start).first < end;
	}

	template <DirtySource source, bool enable>
	void ChangeState(uint64_t vaddr, uint64_t size) {
		const auto [start, end] = GetPageRange(vaddr, size);
		if constexpr (source == DirtySource::Cpu && enable) {
			if (RegionBits(m_gpu_dirty, start, end).Any()) {
				EXIT("CPU dirty state conflicts with GPU dirty state\n");
			}
		}
		if constexpr (source == DirtySource::Gpu && enable) {
			if (RegionBits(m_cpu_dirty, start, end).Any()) {
				EXIT("GPU dirty state conflicts with CPU dirty state\n");
			}
		}
		auto& bits = GetBits<source>();
		if constexpr (enable) {
			bits.SetRange(start, end);
			StoreSummary<source>(true);
			if constexpr (source == DirtySource::Cpu) {
				NoteBdaPending();
				g_cpu_dirty_epoch.fetch_add(1, std::memory_order_release);
			}
		} else {
			bits.UnsetRange(start, end);
			if (bits.None()) {
				StoreSummary<source>(false);
			}
		}
		if constexpr (source == DirtySource::Cpu) {
			UpdateCpuProtection();
		} else {
			if constexpr (enable) {
				// Written by the GPU: no longer kept writable for the CPU (hot pages, below).
				if (m_hot_count != 0 && RegionBits(m_hot, start, end).Any()) {
					m_hot.UnsetRange(start, end);
					m_hot_count = m_hot.Count();
					UpdateCpuProtection();
				}
			}
			UpdateProtection<enable, true>();
		}
	}

	template <DirtySource source, bool clear, typename Func>
	void ForEachModifiedRange(uint64_t vaddr, uint64_t size, Func&& func) {
		const auto [start, end] = GetPageRange(vaddr, size);
		auto&      bits         = GetBits<source>();
		if (bits.FirstRangeFrom(start).first >= end) {
			return;
		}
		RegionBits mask(bits, start, end);
		if constexpr (clear) {
			bits.UnsetRange(start, end);
			if (bits.None()) {
				StoreSummary<source>(false);
			}
			if constexpr (source == DirtySource::Cpu) {
				UpdateCpuProtection();
			} else {
				UpdateProtection<false, true>();
			}
		}
		for (const auto [first, last]: mask) {
			func(m_cpu_addr + first * TRACKER_PAGE_SIZE, (last - first) * TRACKER_PAGE_SIZE);
		}
	}

	// ForEachModifiedRange<DirtySource::Cpu, true> without its protection change (round 80): the
	// CPU-dirty pages of the range become clean, and func gets their runs, but they stay writable
	// until CommitCpuProtection, which the caller makes before it releases the lock (and before
	// it reads the pages' bytes). Several ranges then change the protection in one call.
	template <typename Func>
	void TakeCpuModifiedRanges(uint64_t vaddr, uint64_t size, Func&& func) {
		const auto [start, end] = GetPageRange(vaddr, size);
		auto& bits              = GetBits<DirtySource::Cpu>();
		if (bits.FirstRangeFrom(start).first >= end) {
			return;
		}
		RegionBits mask(bits, start, end);
		bits.UnsetRange(start, end);
		if (bits.None()) {
			StoreSummary<DirtySource::Cpu>(false);
		}
		for (const auto [first, last]: mask) {
			func(m_cpu_addr + first * TRACKER_PAGE_SIZE, (last - first) * TRACKER_PAGE_SIZE);
		}
	}
	void CommitCpuProtection() { UpdateCpuProtection(); }

	// The tracker mirrors the CPU summary in one bit per region (MemoryTracker::IsRangeCpuCleanHint).
	// Set before the region is published.
	void SetCpuSummaryBit(std::atomic<uint64_t>* word, uint64_t bit) noexcept {
		m_cpu_summary_word = word;
		m_cpu_summary_bit  = bit;
	}

	// The tracker's bits of the regions the next BDA walk visits (MemoryTracker::TakeBdaPending):
	// one in a word of regions and one in the word above it. Set before the region is published.
	void SetBdaPendingBits(std::atomic<uint64_t>* word, uint64_t bit,
	                       std::atomic<uint64_t>* top_word, uint64_t top_bit) noexcept {
		m_bda_word     = word;
		m_bda_bit      = bit;
		m_bda_top_word = top_word;
		m_bda_top_bit  = top_bit;
	}

	// Pages of this region became CPU-dirty: the next BDA walk visits it. The region's bit before
	// the word's, so a walk that sees the word's bit finds the region's.
	void NoteBdaPending() noexcept {
		if (m_bda_word != nullptr) {
			m_bda_word->fetch_or(m_bda_bit, std::memory_order_release);
			m_bda_top_word->fetch_or(m_bda_top_bit, std::memory_order_release);
		}
	}

	// Hot pages (KYTY_BISECT's "hotpages"): pages the CPU writes over and over, kept writable
	// instead of write-protected again after each upload, so that the CPU's next write does not
	// fault. Without a fault the tracker cannot see that write, so they are marked CPU-dirty again
	// wholesale (RedirtyHot) at the points where the GPU thread may next read what the CPU wrote
	// since: a new slice of a submission, a wait on memory. A page the GPU writes stops being hot.
	// All with the lock held.
	//
	// The pages of [vaddr, vaddr + size) that hold no GPU-written bytes become hot; how many did.
	size_t MarkHot(uint64_t vaddr, uint64_t size) {
		const auto [start, end] = GetPageRange(vaddr, size);
		RegionBits pages;
		pages.SetRange(start, end);
		pages &= ~m_hot;
		pages &= ~m_gpu_dirty;
		if (pages.None()) {
			return 0;
		}
		m_hot |= pages;
		m_hot_count = m_hot.Count();
		UpdateCpuProtection();
		return pages.Count();
	}
	// The hot pages become CPU-dirty again (they are writable already); how many.
	size_t RedirtyHot() {
		if (m_hot_count == 0) {
			return 0;
		}
		return DirtyHot(m_hot);
	}
	// The hot pages of [vaddr, vaddr + size) become CPU-dirty: something wrote them without a
	// fault (the GPU thread's own stores to guest memory); how many.
	size_t DirtyHotIn(uint64_t vaddr, uint64_t size) {
		if (m_hot_count == 0) {
			return 0;
		}
		const auto [start, end] = GetPageRange(vaddr, size);
		return DirtyHot(RegionBits(m_hot, start, end));
	}
	// The hot pages of [vaddr, vaddr + size) stop being hot, as CPU-dirty pages: the CPU may have
	// written them since their last upload, unseen, so their next upload takes those writes (and
	// write-protects them, so the writes after it fault again). Before a sync that hands the
	// range to the GPU's writes (MemoryTracker::ForEachUploadRange).
	void DropHotIn(uint64_t vaddr, uint64_t size) {
		if (m_hot_count == 0) {
			return;
		}
		const auto [start, end] = GetPageRange(vaddr, size);
		const RegionBits pages(m_hot, start, end);
		if (pages.None()) {
			return;
		}
		(void)DirtyHot(pages);
		m_hot.UnsetRange(start, end);
		m_hot_count = m_hot.Count();
		UpdateCpuProtection();
	}
	// No page is hot any more. They become CPU-dirty first (unseen writes since their last
	// upload are uploaded with them), then write-protected at that upload as any page.
	void ClearHot() {
		if (m_hot_count == 0) {
			return;
		}
		(void)DirtyHot(m_hot);
		m_hot.Clear();
		m_hot_count = 0;
		UpdateCpuProtection();
	}
	[[nodiscard]] size_t HotCount() const noexcept { return m_hot_count; }

	TrackingSpinLock lock;
	// Whether the tracker lists this region as holding hot pages (MemoryTracker's hot mutex).
	bool in_hot_list = false;

private:
	// Every summary change goes through here; the CPU one is mirrored in the tracker's bitmap at
	// the same point, before any page protection changes.
	template <DirtySource source>
	void StoreSummary(bool value) {
		GetSummary<source>().store(value, std::memory_order_release);
		if constexpr (source == DirtySource::Cpu) {
			if (m_cpu_summary_word != nullptr) {
				if (value) {
					m_cpu_summary_word->fetch_or(m_cpu_summary_bit, std::memory_order_acq_rel);
				} else {
					m_cpu_summary_word->fetch_and(~m_cpu_summary_bit, std::memory_order_acq_rel);
				}
			}
		}
	}

	template <bool track, bool is_read>
	void UpdateProtection() {
		const auto protection = is_read ? ~m_gpu_dirty : m_cpu_dirty;
		auto&      previous   = is_read ? m_readable : m_writable;
		auto       mask       = protection ^ previous;
		if (mask.None()) {
			return;
		}
		previous = protection;
		m_page_manager.UpdatePageWatchersForRegion<track, is_read>(m_cpu_addr, mask);
	}

	// `pages` (hot ones) that are neither GPU-written nor CPU-dirty become CPU-dirty; how many.
	size_t DirtyHot(const RegionBits& pages) {
		auto dirty = pages & ~m_gpu_dirty;
		dirty &= ~m_cpu_dirty;
		if (dirty.None()) {
			return 0;
		}
		m_cpu_dirty |= dirty;
		StoreSummary<DirtySource::Cpu>(true);
		NoteBdaPending();
		g_cpu_dirty_epoch.fetch_add(1, std::memory_order_release);
		return dirty.Count();
	}

	// The CPU write protection: a page is writable while CPU-dirty or hot. Pages can go both
	// ways in one call (hot pages cleared while others become dirty).
	void UpdateCpuProtection() {
		auto writable = m_cpu_dirty;
		if (m_hot_count != 0) {
			writable |= m_hot;
		}
		auto now_writable  = writable & ~m_writable;
		auto now_protected = m_writable & ~writable;
		if (now_writable.None() && now_protected.None()) {
			return;
		}
		m_writable = writable;
		if (now_protected.Any()) {
			m_page_manager.UpdatePageWatchersForRegion<true, false>(m_cpu_addr, now_protected);
		}
		if (now_writable.Any()) {
			m_page_manager.UpdatePageWatchersForRegion<false, false>(m_cpu_addr, now_writable);
		}
	}

	template <DirtySource source>
	RegionBits& GetBits() {
		if constexpr (source == DirtySource::Cpu) {
			return m_cpu_dirty;
		} else {
			return m_gpu_dirty;
		}
	}

	template <DirtySource source>
	const RegionBits& GetBits() const {
		if constexpr (source == DirtySource::Cpu) {
			return m_cpu_dirty;
		} else {
			return m_gpu_dirty;
		}
	}

	template <DirtySource source>
	std::atomic<bool>& GetSummary() {
		if constexpr (source == DirtySource::Cpu) {
			return m_cpu_maybe_dirty;
		} else {
			return m_gpu_maybe_dirty;
		}
	}

	template <DirtySource source>
	const std::atomic<bool>& GetSummary() const {
		if constexpr (source == DirtySource::Cpu) {
			return m_cpu_maybe_dirty;
		} else {
			return m_gpu_maybe_dirty;
		}
	}

	[[nodiscard]] std::pair<size_t, size_t> GetPageRange(uint64_t vaddr, uint64_t size) const {
		if (size == 0 || vaddr < m_cpu_addr || vaddr >= m_cpu_addr + TRACKER_REGION_SIZE ||
		    size > m_cpu_addr + TRACKER_REGION_SIZE - vaddr) {
			EXIT("range lies outside its tracking region\n");
		}
		const auto offset = vaddr - m_cpu_addr;
		return {static_cast<size_t>(offset / TRACKER_PAGE_SIZE),
		        static_cast<size_t>((offset + size + TRACKER_PAGE_SIZE - 1) / TRACKER_PAGE_SIZE)};
	}

	PageManager& m_page_manager;
	uint64_t     m_cpu_addr = 0;
	RegionBits   m_cpu_dirty;
	RegionBits   m_gpu_dirty;
	// The constructor fills m_cpu_dirty, so the CPU summary starts true.
	std::atomic<bool> m_cpu_maybe_dirty {true};
	std::atomic<bool> m_gpu_maybe_dirty {false};
	std::atomic<uint64_t>* m_cpu_summary_word = nullptr;
	uint64_t               m_cpu_summary_bit  = 0;
	std::atomic<uint64_t>* m_bda_word         = nullptr;
	uint64_t               m_bda_bit          = 0;
	std::atomic<uint64_t>* m_bda_top_word     = nullptr;
	uint64_t               m_bda_top_bit      = 0;
	RegionBits   m_writable;
	RegionBits   m_readable;
	RegionBits             m_hot;
	size_t                 m_hot_count = 0;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_REGIONMANAGER_H_
