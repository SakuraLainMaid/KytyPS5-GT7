#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_READBACKQUEUE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_READBACKQUEUE_H_

#include "common/common.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

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
// already done.
// Round 152: the GPU thread records and submits a copy (Submit, GPU thread only) and goes on; the
// copy's wait, about 0.1 ms, was a tenth of its time in races. A thread that needs the bytes
// (the reading game thread, as a rule) waits for the copy and puts them in guest memory (Claim,
// then Done), in one of Slots copies that can be on their way at once.
class ReadbackQueue final {
public:
	static constexpr uint32_t Slots = 8;

	// A copy on its way: its slot, and which use of the slot it is (0: none).
	struct Ticket {
		uint32_t           slot       = 0;
		uint64_t           generation = 0;
		[[nodiscard]] bool Valid() const noexcept { return generation != 0; }
	};
	// What a copy takes: from `source`, whose first byte is guest memory's `buffer_address`; each
	// copy's dstOffset counts from 0 in the slot's staging memory, within `total_size`. `begin`
	// and `end` bound the guest bytes of the copies.
	struct Job {
		vk::Buffer                  source         = nullptr;
		uint64_t                    buffer_address = 0;
		uint64_t                    total_size     = 0;
		uint64_t                    begin          = 0;
		uint64_t                    end            = 0;
		std::vector<vk::BufferCopy> copies;
	};

	ReadbackQueue(GraphicContext& graphics, CommandScheduler& scheduler, MasterSemaphore& master);
	~ReadbackQueue();
	KYTY_CLASS_NO_COPY(ReadbackQueue);

	[[nodiscard]] bool Available() const noexcept { return m_queue != nullptr; }
	// The most bytes one copy takes (a slot's staging memory).
	[[nodiscard]] uint64_t Capacity() const noexcept;

	// GPU thread. Records the job's copies and submits them after the renderer's tick
	// `after_tick` (one the GPU has finished, and every write the copies need is in it or before
	// it). An invalid ticket when the copy could not be made (the caller downloads the old way),
	// or when every slot is in use (Full; finish the oldest, Oldest, first).
	[[nodiscard]] Ticket Submit(Job&& job, uint64_t after_tick);
	[[nodiscard]] bool   Full() const noexcept;
	// The oldest copy no thread has started on yet, else the oldest.
	[[nodiscard]] Ticket Oldest() const noexcept;
	// GPU thread: a copy not yet in guest memory with bytes in [begin, end), or an invalid
	// ticket. AnyPending (any thread): whether there is one at all.
	[[nodiscard]] Ticket PendingIn(uint64_t begin, uint64_t end) const;
	[[nodiscard]] bool   AnyPending() const noexcept;
	// GPU thread: waits for the copies that read `source`, before it is destroyed.
	void WaitCopiesFrom(vk::Buffer source);

	// Any thread: the first to ask waits for the ticket's copy and gets its job and its bytes
	// (Claim; `waited_ns`: how long it waited), puts them in guest memory and calls Done. The
	// others get nullptr from Claim and wait in WaitDone. A ticket done already is neither.
	[[nodiscard]] const uint8_t* Claim(Ticket ticket, const Job*& job, uint64_t& waited_ns);
	void                         Done(Ticket ticket);
	void                         WaitDone(Ticket ticket);

private:
	struct Slot {
		vk::CommandBuffer command = nullptr;
		vk::Fence         fence   = nullptr;
		Job               job;
		// The uses submitted, claimed and done; the slot is free when all three agree.
		std::atomic<uint64_t> submitted {0};
		std::atomic<uint64_t> claimed {0};
		std::atomic<uint64_t> done {0};
		uint64_t              sequence = 0; // the submissions' order (GPU thread)
	};
	[[nodiscard]] bool Pending(const Slot& slot) const noexcept;

	GraphicContext&         m_graphics;
	MasterSemaphore&        m_master;
	vk::Queue               m_queue = nullptr;
	vk::CommandPool         m_pool  = nullptr;
	std::array<Slot, Slots> m_slots;
	std::unique_ptr<Buffer> m_staging;
	uint64_t                m_slot_size = 0;
	uint64_t                m_sequence  = 0;
	std::mutex              m_done_mutex;
	std::condition_variable m_done_changed;
	std::atomic<uint32_t>   m_pending {0}; // copies submitted and not yet done
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_READBACKQUEUE_H_
