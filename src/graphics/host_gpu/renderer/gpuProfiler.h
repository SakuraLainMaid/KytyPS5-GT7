#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUPROFILER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUPROFILER_H_

#include "common/profiler.h"

#include <cstddef>
#include <cstdint>

// The host GPU on Tracy's timeline (--profile; KYTY_PROFILE_GPU=0 turns it off): one GPU zone
// per host draw and dispatch, named by its shader, from timestamps written through the recording
// thread (no drain) and read back once their tick has executed. Only while a Tracy server is
// connected. Needs hostQueryReset and calibrated timestamps, which --profile enables when the
// device has them.
namespace Libs::Graphics {

class CommandBuffer;
class CommandScheduler;
class RenderContext;

namespace GpuProfiler {

namespace Detail {
int64_t BeginSlow(RenderContext& context, CommandBuffer& buffer, const char* name, size_t size,
                  uint32_t color);
void    EndSlow(CommandBuffer& buffer, int64_t token);
} // namespace Detail

// Opens a zone before a command; returns its token (negative: none opened).
inline int64_t Begin(RenderContext& context, CommandBuffer& buffer, const char* name, size_t size,
                     uint32_t color) {
	if (!Profiler::g_counters) {
		return -1;
	}
	return Detail::BeginSlow(context, buffer, name, size, color);
}

// Closes the zone after the command.
inline void End(CommandBuffer& buffer, int64_t token) {
	if (token >= 0) {
		Detail::EndSlow(buffer, token);
	}
}

// A zone around the transfer work recorded in its scope (a download, an upload, an image copy),
// so the GPU's time outside the shaders shows on the timeline. The scope must not submit: both
// timestamps go into the command buffer current at its start. Only the renderer's scheduler.
class TransferZone {
public:
	TransferZone(CommandScheduler& scheduler, const char* name) {
		if (Profiler::g_counters) {
			Open(scheduler, name);
		}
	}
	~TransferZone() {
		if (m_token >= 0) {
			Detail::EndSlow(*m_buffer, m_token);
		}
	}
	TransferZone(const TransferZone&)            = delete;
	TransferZone& operator=(const TransferZone&) = delete;

private:
	void           Open(CommandScheduler& scheduler, const char* name);
	CommandBuffer* m_buffer = nullptr;
	int64_t        m_token  = -1;
};

} // namespace GpuProfiler
} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUPROFILER_H_
