#include "graphics/host_gpu/renderer/cache/readbackQueue.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/host_gpu/renderer/masterSemaphore.h"

#include <chrono>
#include <cinttypes>

namespace Libs::Graphics {

namespace {

// The staging memory: a game thread's read takes one readback window (KYTY_READBACK_WINDOW_KB,
// 4 MB by default), its GPU-written ranges only, each padded to 64 bytes.
constexpr uint64_t StagingSize = 32ull << 20u;

// A copy that takes longer than this is a broken device, not a slow one.
constexpr uint64_t WaitTimeoutNs = 5'000'000'000ull;

uint64_t NowNs() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                 std::chrono::steady_clock::now().time_since_epoch())
	                                 .count());
}

} // namespace

ReadbackQueue::ReadbackQueue(GraphicContext& graphics, CommandScheduler& scheduler,
                             MasterSemaphore& master)
    : m_graphics(graphics), m_master(master) {
	if (graphics.readback_queue == nullptr ||
	    graphics.readback_family == static_cast<uint32_t>(-1)) {
		return;
	}
	vk::CommandPoolCreateInfo pool_info {};
	pool_info.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer |
	                  vk::CommandPoolCreateFlagBits::eTransient;
	pool_info.queueFamilyIndex = graphics.readback_family;
	RequireVulkanSuccess(graphics.device.createCommandPool(&pool_info, nullptr, &m_pool),
	                     "create the readback command pool");
	vk::CommandBufferAllocateInfo command_info {};
	command_info.commandPool        = m_pool;
	command_info.level              = vk::CommandBufferLevel::ePrimary;
	command_info.commandBufferCount = 1;
	RequireVulkanSuccess(graphics.device.allocateCommandBuffers(&command_info, &m_command),
	                     "allocate the readback command buffer");
	vk::FenceCreateInfo fence_info {};
	RequireVulkanSuccess(graphics.device.createFence(&fence_info, nullptr, &m_fence),
	                     "create the readback fence");
	m_staging = std::make_unique<Buffer>(graphics, scheduler, MemoryUsage::Download, 0,
	                                     vk::BufferUsageFlagBits::eTransferDst, StagingSize);
	SetVulkanObjectNameF(graphics.device, m_staging->Handle(), "Kyty.ReadbackStaging");
	m_queue = graphics.readback_queue;
	LOGF("BufferCache: game threads' reads of bytes the GPU has finished writing are copied on "
	     "the readback queue (%s, %" PRIu64 " MB of staging; KYTY_FAST_READBACK=0 or "
	     "KYTY_BISECT's fastread:off download them behind the renderer's queue)\n",
	     graphics.readback_family == graphics.queue_family ? "the universal family's second"
	                                                       : "the copy engines' family",
	     StagingSize >> 20u);
}

ReadbackQueue::~ReadbackQueue() {
	if (m_graphics.device == nullptr) {
		return;
	}
	if (m_queue != nullptr) {
		// Nothing is in flight between calls (each waits for its copy), but the device may be
		// shutting down after a loss: wait for the queue rather than destroy what it may use.
		(void)m_queue.waitIdle();
	}
	if (m_fence != nullptr) {
		m_graphics.device.destroyFence(m_fence, nullptr);
	}
	if (m_pool != nullptr) {
		m_graphics.device.destroyCommandPool(m_pool, nullptr);
	}
	m_staging.reset();
}

uint64_t ReadbackQueue::Capacity() const noexcept {
	return m_staging != nullptr ? m_staging->Size() : 0;
}

const uint8_t* ReadbackQueue::Copy(vk::Buffer source, std::span<const vk::BufferCopy> copies,
                                   uint64_t total_size, uint64_t after_tick, uint64_t& waited_ns) {
	waited_ns = 0;
	if (m_queue == nullptr || source == nullptr || copies.empty() || total_size == 0 ||
	    total_size > Capacity()) {
		return nullptr;
	}
	for (const auto& copy: copies) {
		if (copy.size == 0 || copy.dstOffset > total_size ||
		    copy.size > total_size - copy.dstOffset) {
			return nullptr;
		}
	}
	const auto begun = NowNs();
	RequireVulkanSuccess(m_command.reset({}), "reset the readback command buffer");
	vk::CommandBufferBeginInfo begin {};
	begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
	RequireVulkanSuccess(m_command.begin(&begin), "begin the readback command buffer");
	// The writers' memory is ordered by the wait on their tick below; the staging memory was last
	// read by the host, after the previous call's fence.
	m_command.copyBuffer(source, m_staging->Handle(), static_cast<uint32_t>(copies.size()),
	                     copies.data());
	vk::BufferMemoryBarrier to_host {};
	to_host.srcAccessMask       = vk::AccessFlagBits::eTransferWrite;
	to_host.dstAccessMask       = vk::AccessFlagBits::eHostRead;
	to_host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	to_host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	to_host.buffer              = m_staging->Handle();
	to_host.offset              = 0;
	to_host.size                = total_size;
	m_command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                          vk::PipelineStageFlagBits::eHost, {}, 0, nullptr, 1, &to_host, 0,
	                          nullptr);
	RequireVulkanSuccess(m_command.end(), "end the readback command buffer");

	// The renderer's queue signals its timeline at each tick's end: waiting on a tick makes the
	// writes of it and of every tick before it visible to this queue's copy (the tick is done
	// already, so the wait itself is free).
	const auto                      timeline   = m_master.Handle();
	const vk::PipelineStageFlags    wait_stage = vk::PipelineStageFlagBits::eTransfer;
	vk::TimelineSemaphoreSubmitInfo timeline_info {};
	timeline_info.waitSemaphoreValueCount = 1;
	timeline_info.pWaitSemaphoreValues    = &after_tick;
	vk::SubmitInfo submit {};
	submit.pNext              = &timeline_info;
	submit.waitSemaphoreCount = 1;
	submit.pWaitSemaphores    = &timeline;
	submit.pWaitDstStageMask  = &wait_stage;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers    = &m_command;
	RequireVulkanSuccess(m_graphics.device.resetFences(1, &m_fence), "reset the readback fence");
	RequireVulkanSuccess(m_queue.submit(1, &submit, m_fence), "submit a readback");
	const auto result = m_graphics.device.waitForFences(1, &m_fence, VK_TRUE, WaitTimeoutNs);
	if (result == vk::Result::eTimeout) {
		EXIT("BufferCache: a readback copy (%" PRIu64 " bytes after tick %" PRIu64
		     ") did not finish in 5 s on the readback queue\n",
		     total_size, after_tick);
	}
	RequireVulkanSuccess(result, "wait for a readback");
	m_staging->Invalidate(0, total_size);
	waited_ns = NowNs() - begun;
	return m_staging->Mapped().data();
}

} // namespace Libs::Graphics
