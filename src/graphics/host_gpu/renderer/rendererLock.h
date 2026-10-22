#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERERLOCK_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERERLOCK_H_

#include "common/common.h"
#include "common/perfStats.h"
#include "common/threads.h"

#include <chrono>
#include <cstdint>

namespace Libs::Graphics {

// Round 128: the renderer's lock (RenderContext::GetMutex) as the GPU thread takes it for a draw
// or a dispatch. A race profile had the GPU thread waiting for it in 5 % of its samples, and the
// presenter is the other thread that takes it: how long the GPU thread found it held goes to the
// "Perf locks:" line ("renderer").
class RendererLock {
public:
	explicit RendererLock(Common::Mutex& mutex): m_mutex(mutex) {
		if (!m_mutex.TryLock()) {
			const auto begin = std::chrono::steady_clock::now();
			m_mutex.Lock();
			Common::PerfStats::AddLockWait(
			    Common::PerfStats::Lock::Renderer,
			    static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
			                              std::chrono::steady_clock::now() - begin)
			                              .count()));
		}
	}
	~RendererLock() { m_mutex.Unlock(); }

	KYTY_CLASS_NO_COPY(RendererLock);

private:
	Common::Mutex& m_mutex;
};

// Round 128: how long the presenter holds the renderer's lock, from its construction to its end
// (made just after the lock is taken): the "Perf locks:" line's "renderer held by the
// presenter".
class RendererHoldTimer {
public:
	RendererHoldTimer() = default;
	~RendererHoldTimer() {
		Common::PerfStats::AddLockWait(
		    Common::PerfStats::Lock::RendererHeldByPresenter,
		    static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
		                              std::chrono::steady_clock::now() - m_begin)
		                              .count()));
	}

	KYTY_CLASS_NO_COPY(RendererHoldTimer);

private:
	std::chrono::steady_clock::time_point m_begin = std::chrono::steady_clock::now();
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERERLOCK_H_
