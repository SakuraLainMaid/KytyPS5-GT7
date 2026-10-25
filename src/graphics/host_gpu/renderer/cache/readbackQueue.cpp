#include "graphics/host_gpu/renderer/cache/readbackQueue.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/host_gpu/renderer/masterSemaphore.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <tuple>

namespace Libs::Graphics {

namespace {

// A slot's staging memory: a game thread's read takes one readback window (KYTY_READBACK_WINDOW_KB,
// 4 MB by default), its GPU-written ranges only, each padded to 64 bytes. (One copy of up to
// 32 MB at a time before round 152.)
constexpr uint64_t SlotSize = 8ull << 20u;

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
	std::array<vk::CommandBuffer, Slots> commands {};
	vk::CommandBufferAllocateInfo        command_info {};
	command_info.commandPool        = m_pool;
	command_info.level              = vk::CommandBufferLevel::ePrimary;
	command_info.commandBufferCount = Slots;
	RequireVulkanSuccess(graphics.device.allocateCommandBuffers(&command_info, commands.data()),
	                     "allocate the readback command buffers");
	for (uint32_t index = 0; index < Slots; index++) {
		m_slots[index].command = commands[index];
		vk::FenceCreateInfo fence_info {};
		RequireVulkanSuccess(
		    graphics.device.createFence(&fence_info, nullptr, &m_slots[index].fence),
		    "create a readback fence");
	}
	m_slot_size = SlotSize;
	m_staging =
	    std::make_unique<Buffer>(graphics, scheduler, MemoryUsage::Download, 0,
	                             vk::BufferUsageFlagBits::eTransferDst, m_slot_size * Slots);
	SetVulkanObjectNameF(graphics.device, m_staging->Handle(), "Kyty.ReadbackStaging");
	m_queue = graphics.readback_queue;
	LOGF("BufferCache: game threads' reads of bytes the GPU has finished writing are copied on "
	     "the readback queue (%s, up to %u copies of %" PRIu64 " MB at once; KYTY_FAST_READBACK=0 "
	     "or KYTY_BISECT's fastread:off download them behind the renderer's queue)\n",
	     graphics.readback_family == graphics.queue_family ? "the universal family's second"
	                                                       : "the copy engines' family",
	     Slots, m_slot_size >> 20u);
}

ReadbackQueue::~ReadbackQueue() {
	if (m_graphics.device == nullptr) {
		return;
	}
	if (m_queue != nullptr) {
		// The device may be shutting down after a loss: wait for the queue rather than destroy
		// what it may use.
		(void)m_queue.waitIdle();
	}
	for (auto& slot: m_slots) {
		if (slot.fence != nullptr) {
			m_graphics.device.destroyFence(slot.fence, nullptr);
		}
	}
	if (m_pool != nullptr) {
		m_graphics.device.destroyCommandPool(m_pool, nullptr);
	}
	m_staging.reset();
}

uint64_t ReadbackQueue::Capacity() const noexcept {
	return m_staging != nullptr ? m_slot_size : 0;
}

bool ReadbackQueue::Pending(const Slot& slot) const noexcept {
	return slot.done.load(std::memory_order_acquire) !=
	       slot.submitted.load(std::memory_order_relaxed);
}

bool ReadbackQueue::Full() const noexcept {
	for (const auto& slot: m_slots) {
		if (!Pending(slot)) {
			return false;
		}
	}
	return true;
}

ReadbackQueue::Ticket ReadbackQueue::Oldest() const noexcept {
	// A copy no thread has claimed is finished by the caller without waiting for another thread
	// (whose progress it would otherwise depend on).
	Ticket   oldest;
	bool     oldest_claimed = true;
	uint64_t sequence       = UINT64_MAX;
	for (uint32_t index = 0; index < Slots; index++) {
		const auto& slot = m_slots[index];
		if (!Pending(slot)) {
			continue;
		}
		const auto generation = slot.submitted.load(std::memory_order_relaxed);
		const bool claimed    = slot.claimed.load(std::memory_order_acquire) == generation;
		if (!oldest.Valid() ||
		    std::tie(claimed, slot.sequence) < std::tie(oldest_claimed, sequence)) {
			oldest         = {index, generation};
			oldest_claimed = claimed;
			sequence       = slot.sequence;
		}
	}
	return oldest;
}

bool ReadbackQueue::AnyPending() const noexcept {
	return m_pending.load(std::memory_order_acquire) != 0;
}

ReadbackQueue::Ticket ReadbackQueue::PendingIn(uint64_t begin, uint64_t end) const {
	for (uint32_t index = 0; index < Slots; index++) {
		const auto& slot = m_slots[index];
		// The bounds first (the GPU thread's own data): the busy guest reads ask this.
		if (slot.job.end <= begin || slot.job.begin >= end || !Pending(slot)) {
			continue;
		}
		// The first copy ending after `begin` (they are sorted and apart, Submit).
		const auto& copies = slot.job.copies;
		const auto  base   = slot.job.buffer_address;
		const auto  first =
		    std::partition_point(copies.begin(), copies.end(), [&](const vk::BufferCopy& copy) {
			    return base + copy.srcOffset + copy.size <= begin;
		    });
		if (first != copies.end() && base + first->srcOffset < end) {
			return {index, slot.submitted.load(std::memory_order_relaxed)};
		}
	}
	return {};
}

void ReadbackQueue::WaitCopiesFrom(vk::Buffer source) {
	for (auto& slot: m_slots) {
		if (!Pending(slot) || slot.job.source != source) {
			continue;
		}
		// Only the wait: the claimant puts the bytes in guest memory (fences may be waited for
		// on several threads at once; only this thread resets them, once the slot is free).
		const auto result = m_graphics.device.waitForFences(1, &slot.fence, VK_TRUE, WaitTimeoutNs);
		if (result == vk::Result::eTimeout) {
			EXIT("BufferCache: a readback copy did not finish in 5 s on the readback queue\n");
		}
		RequireVulkanSuccess(result, "wait for a readback");
	}
}

ReadbackQueue::Ticket ReadbackQueue::Submit(Job&& job, uint64_t after_tick) {
	if (m_queue == nullptr || job.source == nullptr || job.copies.empty() || job.total_size == 0 ||
	    job.total_size > Capacity()) {
		return {};
	}
	for (const auto& copy: job.copies) {
		if (copy.size == 0 || copy.dstOffset > job.total_size ||
		    copy.size > job.total_size - copy.dstOffset) {
			return {};
		}
	}
	// By guest address, for PendingIn (the ranges never overlap).
	const auto by_address = [](const vk::BufferCopy& a, const vk::BufferCopy& b) {
		return a.srcOffset < b.srcOffset;
	};
	if (!std::is_sorted(job.copies.begin(), job.copies.end(), by_address)) {
		std::sort(job.copies.begin(), job.copies.end(), by_address);
	}
	uint32_t index = Slots;
	for (uint32_t candidate = 0; candidate < Slots; candidate++) {
		if (!Pending(m_slots[candidate])) {
			index = candidate;
			break;
		}
	}
	if (index == Slots) {
		return {};
	}
	auto&      slot   = m_slots[index];
	const auto offset = m_slot_size * index;
	// The slot's last copy is done and its bytes are in guest memory: its command buffer, fence
	// and staging memory are free.
	RequireVulkanSuccess(slot.command.reset({}), "reset a readback command buffer");
	vk::CommandBufferBeginInfo begin {};
	begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
	RequireVulkanSuccess(slot.command.begin(&begin), "begin a readback command buffer");
	// The writers' memory is ordered by the wait on their tick below; the slot's staging memory
	// was last read by the host, after its previous fence.
	std::vector<vk::BufferCopy> copies(job.copies);
	for (auto& copy: copies) {
		copy.dstOffset += offset;
	}
	slot.command.copyBuffer(job.source, m_staging->Handle(), static_cast<uint32_t>(copies.size()),
	                        copies.data());
	vk::BufferMemoryBarrier to_host {};
	to_host.srcAccessMask       = vk::AccessFlagBits::eTransferWrite;
	to_host.dstAccessMask       = vk::AccessFlagBits::eHostRead;
	to_host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	to_host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	to_host.buffer              = m_staging->Handle();
	to_host.offset              = offset;
	to_host.size                = job.total_size;
	slot.command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                             vk::PipelineStageFlagBits::eHost, {}, 0, nullptr, 1, &to_host, 0,
	                             nullptr);
	RequireVulkanSuccess(slot.command.end(), "end a readback command buffer");

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
	submit.pCommandBuffers    = &slot.command;
	RequireVulkanSuccess(m_graphics.device.resetFences(1, &slot.fence), "reset a readback fence");
	RequireVulkanSuccess(m_queue.submit(1, &submit, slot.fence), "submit a readback");
	slot.job      = std::move(job);
	slot.sequence = ++m_sequence;
	m_pending.fetch_add(1, std::memory_order_relaxed);
	const auto generation = slot.submitted.load(std::memory_order_relaxed) + 1;
	slot.submitted.store(generation, std::memory_order_release);
	return {index, generation};
}

const uint8_t* ReadbackQueue::Claim(Ticket ticket, const Job*& job, uint64_t& waited_ns) {
	job       = nullptr;
	waited_ns = 0;
	if (!ticket.Valid() || ticket.slot >= Slots) {
		return nullptr;
	}
	auto& slot     = m_slots[ticket.slot];
	auto  expected = ticket.generation - 1;
	if (!slot.claimed.compare_exchange_strong(expected, ticket.generation,
	                                          std::memory_order_acq_rel)) {
		return nullptr;
	}
	const auto begun  = NowNs();
	const auto result = m_graphics.device.waitForFences(1, &slot.fence, VK_TRUE, WaitTimeoutNs);
	if (result == vk::Result::eTimeout) {
		EXIT("BufferCache: a readback copy (%" PRIu64 " bytes) did not finish in 5 s on the "
		     "readback queue\n",
		     slot.job.total_size);
	}
	RequireVulkanSuccess(result, "wait for a readback");
	const auto offset = m_slot_size * ticket.slot;
	m_staging->Invalidate(offset, slot.job.total_size);
	waited_ns = NowNs() - begun;
	job       = &slot.job;
	return m_staging->Mapped().data() + offset;
}

void ReadbackQueue::Done(Ticket ticket) {
	if (!ticket.Valid() || ticket.slot >= Slots) {
		return;
	}
	{
		std::lock_guard lock(m_done_mutex);
		m_slots[ticket.slot].done.store(ticket.generation, std::memory_order_release);
	}
	m_pending.fetch_sub(1, std::memory_order_release);
	m_done_changed.notify_all();
}

void ReadbackQueue::WaitDone(Ticket ticket) {
	if (!ticket.Valid() || ticket.slot >= Slots) {
		return;
	}
	const auto& slot = m_slots[ticket.slot];
	if (slot.done.load(std::memory_order_acquire) >= ticket.generation) {
		return;
	}
	std::unique_lock lock(m_done_mutex);
	m_done_changed.wait(
	    lock, [&] { return slot.done.load(std::memory_order_acquire) >= ticket.generation; });
}

} // namespace Libs::Graphics
