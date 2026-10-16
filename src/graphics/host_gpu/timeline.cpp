#include "graphics/host_gpu/timeline.h"

#include "common/logging/log.h"
#include "common/perfStats.h"

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdlib>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Libs::Graphics::Timeline {

namespace {

struct Window {
	uint64_t first = 0;
	uint64_t count = 0;
};

const Window& GetWindow() {
	static const Window window = [] {
		Window result;
		const char* value = std::getenv("KYTY_TIMELINE");
		if (value != nullptr) {
			char* end    = nullptr;
			result.first = std::strtoull(value, &end, 10);
			result.count = end != nullptr && *end == ':' ? std::strtoull(end + 1, nullptr, 10) : 1;
		}
		return result;
	}();
	return window;
}

uint64_t NowMicros() {
	static const auto start = std::chrono::steady_clock::now();
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
	                                 std::chrono::steady_clock::now() - start)
	                                 .count());
}

// F6's request, taken up at the next presented frame.
std::atomic<uint32_t> g_requested {0};

uint32_t ThreadId() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	return static_cast<uint32_t>(GetCurrentThreadId());
#else
	return 0;
#endif
}

} // namespace

namespace Detail {

void MarkSlow(const char* event, uint64_t a, uint64_t b) {
	LOGF("TL %" PRIu64 " %u %s 0x%" PRIx64 " 0x%" PRIx64 "\n", NowMicros(), ThreadId(), event, a,
	     b);
}

} // namespace Detail

void RequestCapture(uint32_t frames) {
	g_requested.store(frames, std::memory_order_relaxed);
}

void Present(uint64_t frame) {
	// Called with the presenter's lock held (Presenter::Present), one call at a time.
	static uint64_t key_first = 0;
	static uint64_t key_count = 0;
	if (const auto requested = g_requested.exchange(0, std::memory_order_relaxed); requested != 0) {
		key_first = frame + 1;
		key_count = requested;
		LOGF("TL capture: presented frames %" PRIu64 " to %" PRIu64 " (F6)\n", key_first,
		     key_first + key_count - 1);
		for (const auto& [tid, name]: Common::PerfStats::NamedThreads()) {
			LOGF("TL thread %u %s\n", tid, name.c_str());
		}
	}
	const auto& window = GetWindow();
	const bool  in_window =
	    window.count != 0 && frame >= window.first && frame - window.first < window.count;
	const bool in_key = key_count != 0 && frame >= key_first && frame - key_first < key_count;
	const bool active = in_window || in_key;
	if (active || Detail::g_active.load(std::memory_order_relaxed)) {
		Detail::MarkSlow("present", frame, 0);
	}
	Detail::g_active.store(active, std::memory_order_relaxed);
}

} // namespace Libs::Graphics::Timeline
