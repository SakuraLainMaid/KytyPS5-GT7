#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_TIMELINE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_TIMELINE_H_

#include <atomic>
#include <cstdint>

// Diagnostics: KYTY_TIMELINE=<first frame>:<frames> logs "TL <microseconds> <thread> <event>"
// lines for guest submissions, GPU-thread processing, host GPU submissions and completions,
// drains, presents, the game's event-queue waits and flips, and game-thread reads of GPU-written
// memory, for that window of presented frames. F6 (RequestCapture) logs the next frames the same
// way; each capture first names the threads ("TL thread <id> <name>"). Off by default.
namespace Libs::Graphics::Timeline {

namespace Detail {
inline std::atomic<bool> g_active {false};
void MarkSlow(const char* event, uint64_t a, uint64_t b);
} // namespace Detail

[[nodiscard]] inline bool Active() {
	return Detail::g_active.load(std::memory_order_relaxed);
}

inline void Mark(const char* event, uint64_t a = 0, uint64_t b = 0) {
	if (Active()) {
		Detail::MarkSlow(event, a, b);
	}
}

// Called once per presented frame.
void Present(uint64_t frame);

// F6: the next `frames` presented frames are logged.
void RequestCapture(uint32_t frames);

} // namespace Libs::Graphics::Timeline

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_TIMELINE_H_
