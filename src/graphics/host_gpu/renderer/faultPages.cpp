#include "graphics/host_gpu/renderer/faultPages.h"

#include "common/logging/log.h"
#include "common/perfStats.h"
#include "graphics/host_gpu/regionManager.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <condition_variable>
#include <fmt/format.h>
#include <iterator>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace Libs::Graphics::FaultPages {
namespace {

// One entry a fault: the page (bits 63..32), the frame's low 24 bits (31..8), the outcome (7..0).
// The newest RingSize faults are what a report sees (about 4 s of a race's write faults).
constexpr size_t   RingSize   = size_t {1} << 18u;
constexpr uint64_t FrameMask  = (uint64_t {1} << 24u) - 1u;
constexpr uint64_t PageShift  = 12;
constexpr uint64_t BlockPages = 16; // 64 KB
// A report looks back at most this many frames (10 s at 60 fps).
constexpr uint64_t MaxAgeFrames = 600;

// Made once and never freed: a fault handler may still write it while the emulator exits.
std::atomic<uint64_t>* g_ring = nullptr;
std::atomic<uint64_t>  g_ring_pos {0};
std::atomic<uint64_t>  g_last_frame {0};
std::atomic<uint64_t>  g_hot_marked {0};
std::atomic<uint64_t>  g_hot_refreshes {0};
std::atomic<uint64_t>  g_hot_redirtied {0};
std::atomic<uint64_t>  g_hot_now {0};
std::atomic<uint64_t>  g_hot_clears {0};

struct Decoded {
	uint64_t frame;
	uint64_t page;
	Outcome  outcome;
};

std::string Percent(uint64_t part, uint64_t whole) {
	return whole == 0 ? std::string("0%")
	                  : fmt::format("{:.0f}%",
	                                100.0 * static_cast<double>(part) / static_cast<double>(whole));
}

// The pages of each frame (sorted by frame, then page): per-frame figures and the frame-to-frame
// repeat.
struct FrameFigures {
	uint64_t                faults       = 0;
	uint64_t                pages        = 0;
	uint64_t                blocks       = 0;
	uint64_t                again        = 0; // pages that faulted the frame before too
	uint64_t                again_of     = 0; // pages of frames whose frame before is in the window
	uint64_t                steady_pages = 0; // pages that fault in 3/4 of the frames or more
	uint64_t                steady_hits  = 0; // their faults
	std::array<uint64_t, 4> block_pages {};   // blocks with 1, 2-3, 4-7, 8-16 pages in a frame
};

FrameFigures Figure(std::vector<Decoded>& entries, uint64_t first_frame, uint64_t frames) {
	FrameFigures figures;
	std::sort(entries.begin(), entries.end(), [](const Decoded& a, const Decoded& b) {
		return a.frame != b.frame ? a.frame < b.frame : a.page < b.page;
	});
	figures.faults = entries.size();
	std::vector<uint64_t> previous_pages;
	std::vector<uint64_t> current_pages;
	std::vector<uint64_t> all_pages; // one per (frame, page)
	uint64_t              previous_frame = UINT64_MAX;
	const auto            close_frame    = [&](uint64_t frame) {
        if (frame != UINT64_MAX && previous_frame != UINT64_MAX && frame == previous_frame + 1u) {
            std::vector<uint64_t> common;
            std::set_intersection(current_pages.begin(), current_pages.end(),
			                                    previous_pages.begin(), previous_pages.end(),
			                                    std::back_inserter(common));
            figures.again += common.size();
            figures.again_of += current_pages.size();
        }
	};
	for (size_t i = 0; i < entries.size();) {
		const auto frame = entries[i].frame;
		current_pages.clear();
		size_t j = i;
		for (; j < entries.size() && entries[j].frame == frame; j++) {
			if (current_pages.empty() || current_pages.back() != entries[j].page) {
				current_pages.push_back(entries[j].page);
			}
		}
		figures.pages += current_pages.size();
		// The pages a block has in this frame.
		for (size_t k = 0; k < current_pages.size();) {
			const auto block = current_pages[k] / BlockPages;
			size_t     l     = k;
			while (l < current_pages.size() && current_pages[l] / BlockPages == block) {
				l++;
			}
			const auto count = l - k;
			figures.blocks++;
			figures.block_pages[count == 1 ? 0 : count <= 3 ? 1 : count <= 7 ? 2 : 3]++;
			k = l;
		}
		close_frame(frame);
		all_pages.insert(all_pages.end(), current_pages.begin(), current_pages.end());
		previous_pages.swap(current_pages);
		previous_frame = frame;
		i              = j;
	}
	// Pages that fault in most frames, and the faults they take.
	if (frames >= 4) {
		std::sort(all_pages.begin(), all_pages.end());
		std::vector<uint64_t> steady;
		for (size_t k = 0; k < all_pages.size();) {
			size_t l = k;
			while (l < all_pages.size() && all_pages[l] == all_pages[k]) {
				l++;
			}
			if ((l - k) * 4u >= frames * 3u) {
				steady.push_back(all_pages[k]);
			}
			k = l;
		}
		figures.steady_pages = steady.size();
		for (const auto& entry: entries) {
			figures.steady_hits +=
			    std::binary_search(steady.begin(), steady.end(), entry.page) ? 1u : 0u;
		}
	}
	(void)first_frame;
	return figures;
}

class Reporter final {
public:
	Reporter(): m_thread([this](const std::stop_token& stop) { Run(stop); }) {}

private:
	void Run(const std::stop_token& stop) {
		uint64_t   last_pos         = 0;
		uint64_t   last_frame       = g_last_frame.load(std::memory_order_relaxed);
		uint64_t   last_submissions = g_guest_submission_seq.load(std::memory_order_relaxed);
		std::mutex mutex;
		for (;;) {
			{
				std::unique_lock lock(mutex);
				(void)m_wake.wait_for(lock, stop, std::chrono::seconds(10), [] { return false; });
			}
			if (stop.stop_requested()) {
				return;
			}
			const auto pos          = g_ring_pos.load(std::memory_order_acquire);
			const auto frame_now    = g_last_frame.load(std::memory_order_relaxed);
			const auto submissions  = g_guest_submission_seq.load(std::memory_order_relaxed);
			const auto count        = std::min<uint64_t>(pos - last_pos, RingSize);
			const auto frames_done  = frame_now - last_frame;
			const auto subs_done    = submissions - last_submissions;
			const bool first_report = last_frame == 0;
			last_pos                = pos;
			last_frame              = frame_now;
			last_submissions        = submissions;
			// The first report's rates would count every frame since the start.
			if (count == 0 || first_report) {
				continue;
			}
			std::vector<Decoded> writes;
			std::vector<Decoded> reads;
			writes.reserve(count);
			std::array<uint64_t, static_cast<size_t>(Outcome::Count)> outcomes {};
			uint64_t                                                  oldest = UINT64_MAX;
			uint64_t                                                  newest = 0;
			std::vector<Decoded>                                      all;
			all.reserve(count);
			for (uint64_t i = pos - count; i < pos; i++) {
				const auto entry   = g_ring[i % RingSize].load(std::memory_order_relaxed);
				const auto frame24 = (entry >> 8u) & FrameMask;
				const auto age     = (frame_now - frame24) & FrameMask;
				const auto outcome = static_cast<Outcome>(entry & 0xffu);
				// A slot claimed but not written yet, or one from the ring's lap before: older
				// than any frame this report covers (or ahead of the newest, wrapped).
				if (outcome >= Outcome::Count || entry == 0 || age > MaxAgeFrames ||
				    age > frame_now) {
					continue;
				}
				const auto frame = frame_now - age;
				all.push_back({frame, entry >> 32u, outcome});
				oldest = std::min(oldest, frame);
				newest = std::max(newest, frame);
			}
			// Whole frames only: the oldest may have faults before the ring's start, the newest
			// is still going.
			if (all.empty() || newest < oldest + 2u) {
				continue;
			}
			const auto first  = oldest + 1u;
			const auto last   = newest - 1u;
			const auto frames = last - first + 1u;
			for (const auto& entry: all) {
				if (entry.frame < first || entry.frame > last) {
					continue;
				}
				if (entry.outcome == Outcome::Read) {
					reads.push_back(entry);
				} else {
					writes.push_back(entry);
				}
				outcomes[static_cast<size_t>(entry.outcome)]++;
			}
			const auto w         = Figure(writes, first, frames);
			const auto r         = Figure(reads, first, frames);
			const auto per_frame = [frames](uint64_t value) {
				return fmt::format("{:.0f}",
				                   static_cast<double>(value) / static_cast<double>(frames));
			};
			const auto outcome = [&](Outcome which) {
				return Percent(outcomes[static_cast<size_t>(which)], w.faults);
			};
			const auto refreshes = g_hot_refreshes.exchange(0, std::memory_order_relaxed);
			const auto redirtied = g_hot_redirtied.exchange(0, std::memory_order_relaxed);
			const auto marked    = g_hot_marked.exchange(0, std::memory_order_relaxed);
			const auto clears    = g_hot_clears.exchange(0, std::memory_order_relaxed);
			const auto per_done  = [frames_done](uint64_t value) {
                return frames_done == 0
				            ? std::string("0")
				            : fmt::format("{:.1f}", static_cast<double>(value) /
				                                        static_cast<double>(frames_done));
			};
			auto config = Common::PerfStats::Configuration();
			if (config.empty()) {
				config = "normal";
			}
			LOGF(
			    "Fault pages [%s] (the last %" PRIu64
			    " frames): writes %s a frame on %s pages in %s "
			    "64 KB blocks (a block's pages in a frame: 1 %s, 2-3 %s, 4-7 %s, 8-16 %s) | %s of "
			    "a frame's pages faulted the frame before too; %" PRIu64
			    " pages fault in 3/4 of the frames or more (%s of the faults) | what the write "
			    "faults did: released a block %s, first in their block in 100 ms %s, address "
			    "writes %s, an image in the block %s, GPU-written bytes in the block %s, block not "
			    "mapped %s, blocks off %s | reads %s a frame on %s pages (%s the frame before too) "
			    "| %s submissions a frame | hot pages: %" PRIu64 " now, %" PRIu64
			    " made hot, %s refreshes a frame making %s pages CPU-dirty again a frame, %" PRIu64
			    " clears\n",
			    config.c_str(), frames, per_frame(w.faults).c_str(), per_frame(w.pages).c_str(),
			    per_frame(w.blocks).c_str(), Percent(w.block_pages[0], w.blocks).c_str(),
			    Percent(w.block_pages[1], w.blocks).c_str(),
			    Percent(w.block_pages[2], w.blocks).c_str(),
			    Percent(w.block_pages[3], w.blocks).c_str(), Percent(w.again, w.again_of).c_str(),
			    w.steady_pages, Percent(w.steady_hits, w.faults).c_str(),
			    outcome(Outcome::Released).c_str(), outcome(Outcome::NotRecent).c_str(),
			    outcome(Outcome::AddressWrites).c_str(), outcome(Outcome::ImageInBlock).c_str(),
			    outcome(Outcome::GpuBytes).c_str(), outcome(Outcome::BlockUnmapped).c_str(),
			    outcome(Outcome::BlocksOff).c_str(), per_frame(r.faults).c_str(),
			    per_frame(r.pages).c_str(), Percent(r.again, r.again_of).c_str(),
			    per_done(subs_done).c_str(), g_hot_now.load(std::memory_order_relaxed), marked,
			    per_done(refreshes).c_str(), per_done(redirtied).c_str(), clears);
		}
	}

	std::condition_variable_any m_wake;
	std::jthread                m_thread; // last: it starts with the others in place
};

std::mutex g_reporter_mutex;
// Deleted (joined) by Stop only: a reporter still running at exit is left to the process's end.
Reporter*         g_reporter = nullptr;
std::atomic<bool> g_started {false};
bool              g_stopped = false;

} // namespace

void StartIfActive() {
	if (g_started.load(std::memory_order_acquire) || !Common::PerfStats::Active()) {
		return;
	}
	std::lock_guard lock(g_reporter_mutex);
	if (g_stopped || g_reporter != nullptr) {
		return;
	}
	if (g_ring == nullptr) {
		g_ring = new std::atomic<uint64_t>[RingSize] {};
	}
	g_started.store(true, std::memory_order_release);
	g_reporter = new Reporter();
}

void Note(uint64_t vaddr, uint64_t frame, Outcome outcome) noexcept {
	if (!g_started.load(std::memory_order_acquire)) {
		return;
	}
	// The newest frame a fault saw (a slow handler may come with an older one).
	auto last = g_last_frame.load(std::memory_order_relaxed);
	while (frame > last &&
	       !g_last_frame.compare_exchange_weak(last, frame, std::memory_order_relaxed)) {
	}
	const auto entry = ((vaddr >> PageShift) << 32u) | ((frame & FrameMask) << 8u) |
	                   static_cast<uint64_t>(outcome);
	const auto index = g_ring_pos.fetch_add(1, std::memory_order_relaxed);
	g_ring[index % RingSize].store(entry, std::memory_order_relaxed);
}

void NoteHotMarked(size_t pages) noexcept {
	g_hot_marked.fetch_add(pages, std::memory_order_relaxed);
}

void NoteHotRefresh(size_t pages, size_t hot) noexcept {
	g_hot_refreshes.fetch_add(1, std::memory_order_relaxed);
	g_hot_redirtied.fetch_add(pages, std::memory_order_relaxed);
	g_hot_now.store(hot, std::memory_order_relaxed);
}

void NoteHotCleared() noexcept {
	g_hot_clears.fetch_add(1, std::memory_order_relaxed);
	g_hot_now.store(0, std::memory_order_relaxed);
}

void Stop() {
	Reporter* reporter = nullptr;
	{
		std::lock_guard lock(g_reporter_mutex);
		g_stopped = true;
		g_started.store(false, std::memory_order_release);
		reporter   = g_reporter;
		g_reporter = nullptr;
	}
	delete reporter;
}

} // namespace Libs::Graphics::FaultPages
