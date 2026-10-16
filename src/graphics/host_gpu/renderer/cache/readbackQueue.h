#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_READBACKQUEUE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_READBACKQUEUE_H_

#include "common/common.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>
#include <memory>
#include <span>

namespace Libs::Graphics {

class Buffer;
class CommandScheduler;
class MasterSemaphore;
struct GraphicContext;

// Game threads' reads of bytes the GPU has finished writing (BufferCache::FastReadback). Their
// download used to go at the end of the renderer's queue, behind every command buffer still on
// the GPU: 2-3 ms a read in GT7, about 440 reads a second (round 63), though the bytes had been
// written a frame or more before. Here it goes on a queue of its own
// (GraphicContext::readback_queue: a copy engine's, which runs beside the graphics engine, or the
// universal family's second queue), which waits only for the tick of the bytes' last writer,
// already done, and the GPU thread waits for it there and then. Only the GPU thread uses it.
class ReadbackQueue final {
public:
	ReadbackQueue(GraphicContext& graphics, CommandScheduler& scheduler, MasterSemaphore& master);
	~ReadbackQueue();
	KYTY_CLASS_NO_COPY(ReadbackQueue);

	[[nodiscard]] bool Available() const noexcept { return m_queue != nullptr; }
	// The most bytes one copy takes (its staging memory).
	[[nodiscard]] uint64_t Capacity() const noexcept;

	// Copies `copies` out of `source` (their dstOffset counts from 0 in the staging memory, each
	// within `total_size`) after the renderer's tick `after_tick` (one the GPU has finished, and
	// every write the copies need is in it or before it), and waits for them. Returns the
	// staging memory, valid until the next call, or nullptr when the copy could not be made (the
	// caller downloads the old way). `waited_ns`: how long the wait took.
	[[nodiscard]] const uint8_t* Copy(vk::Buffer source, std::span<const vk::BufferCopy> copies,
	                                  uint64_t total_size, uint64_t after_tick,
	                                  uint64_t& waited_ns);

private:
	GraphicContext&         m_graphics;
	MasterSemaphore&        m_master;
	vk::Queue               m_queue   = nullptr;
	vk::CommandPool         m_pool    = nullptr;
	vk::CommandBuffer       m_command = nullptr;
	vk::Fence               m_fence   = nullptr;
	std::unique_ptr<Buffer> m_staging;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_READBACKQUEUE_H_
