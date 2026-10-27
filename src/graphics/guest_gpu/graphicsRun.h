#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRUN_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRUN_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/threads.h"
#include "common/uniqueFunction.h"
#include "graphics/guest_gpu/command_processor/commandProcessor.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <semaphore>
#include <span>
#include <thread>
#include <unordered_map>

namespace Libs::Graphics {

class RenderContext;
class DrawPrepScanner;

class GuestGpu final {
public:
	explicit GuestGpu(RenderContext& renderer);
	~GuestGpu();
	KYTY_CLASS_NO_COPY(GuestGpu);

	void               Shutdown();
	[[nodiscard]] bool IsStopping();
	void               SendCommand(Common::UniqueFunction<void>&& command);
	// Like SendCommand, but false instead of exiting once the GPU no longer accepts commands.
	bool               TrySendCommand(Common::UniqueFunction<void>&& command);
	void               SendCommandSync(Common::UniqueFunction<void>&& command);
	// Like SendCommandSync, but false instead of exiting once the GPU no longer accepts commands.
	bool TrySendCommandSync(Common::UniqueFunction<void>&& command);

	// End-of-pipe labels (RELEASE_MEM without an interrupt) become visible to the CPU once the
	// host GPU has executed the work recorded before them, as on hardware, instead of when the
	// packet is recorded. Until then the GPU thread's own label checks (WAIT_REG_MEM,
	// COND_INDIRECT_BUFFER) see the recorded value through ReadLabel.
	// KYTY_LABELS_AT_COMPLETION=1 enables it.
	[[nodiscard]] static bool LabelsAtCompletion();
	// Round 135: whether the PM4 cache flushes, RELEASE_MEM and suspend points record the full
	// barrier of before (KYTY_PM4_BARRIERS=global, or KYTY_BISECT's barriers:global), and draws
	// that write buffers go without the write-hazard barrier the change gave them.
	[[nodiscard]] static bool GlobalPm4Barriers();
	// Round 166: the guest's own synchronization orders the GPU work: a full barrier at each of
	// its partial flushes, cache actions, ACQUIRE_MEM, waits on memory and suspend points, and at
	// the start of each queue's slice, instead of one before every writing dispatch and one after
	// every dispatch. The default since round 167; KYTY_PM4_BARRIERS=renderer (or =global), or
	// KYTY_BISECT's barriers:renderer (barriers:global), turns it off.
	[[nodiscard]] static bool GuestPm4Barriers();
	// A barrier at the guest's sync points (global or guest).
	[[nodiscard]] static bool Pm4SyncBarriers() {
		return GlobalPm4Barriers() || GuestPm4Barriers();
	}
	// Whether any label write has been deferred (so ReadLabel must consult the pending ones).
	[[nodiscard]] static bool LabelsDeferred() noexcept;
	// GPU thread.
	void DeferLabelWrite(uint64_t address, uint64_t value, uint32_t size);
	template <typename T>
	[[nodiscard]] T ReadLabel(const volatile T* address) const;

	// Submitted command memory is borrowed and must remain valid until GPU execution completes.
	void Submit(std::span<const uint32_t> draw_commands,
	            std::span<const uint32_t> constant_commands);
	void SubmitCompute(uint32_t queue, std::span<const uint32_t> commands);
	void SubmitFlipPreparation(uint64_t request_id);
	// Insert an ordered graphics drain; only a previous suspend point can block the caller.
	void SuspendPoint();
	// Wait for guest command processing, including all compute queues (not native GPU completion).
	void              WaitForIdle();
	[[nodiscard]] int GetFrameNum() const;

	[[nodiscard]] static bool IsGpuThread() noexcept;

	// Round 108: compute queue submissions enqueued so far (any thread). A graphics stream the GPU
	// thread runs stops between packets once one more arrived (ComputeYieldMicroseconds), so
	// compute work the game sends to run beside its graphics, and often waits for, is not held
	// behind the rest of a long graphics submission.
	[[nodiscard]] uint64_t ComputeEnqueued() const noexcept {
		return m_compute_enqueued.load(std::memory_order_relaxed);
	}
	// How long a graphics stream runs before it may stop, in microseconds (KYTY_COMPUTE_YIELD,
	// 500 if unset); 0 when KYTY_COMPUTE_YIELD=0 or KYTY_BISECT's yield:off turns it off.
	[[nodiscard]] static uint32_t ComputeYieldMicroseconds();

private:
	static constexpr uint32_t ComputePipeCount     = 7;
	static constexpr uint32_t QueuesPerComputePipe = 8;
	static constexpr uint32_t ComputeQueueCount    = ComputePipeCount * QueuesPerComputePipe;
	static constexpr uint32_t ComputeQueueBase     = 0x20;
	static constexpr uint32_t QueueCount           = 1 + ComputeQueueCount;

	enum class SubmissionType { Graphics, Compute, FlipPreparation, SuspendPoint };

	struct Submission {
		SubmissionType            type     = SubmissionType::Graphics;
		uint32_t                  queue_id = 0;
		std::span<const uint32_t> commands;
		std::span<const uint32_t> constant_commands;
		Pm4Execution              command_execution;
		Pm4Execution              constant_execution;
		bool                      started           = false;
		bool                      command_complete  = false;
		bool                      constant_complete = false;
		bool                      blocked           = false;
		bool                      yielded           = false; // stopped for compute (round 108)
		bool                      progressed        = false; // the last slice ran packets
		uint64_t                  flip_request_id   = 0;
		uint64_t                  prep_seq          = 0; // draw prep: graphics submission number
	};

	void              Enqueue(Submission submission);
	void              ProcessCommands();
	bool              Process(Submission& submission);
	static void       ThreadRun(void* data);
	CommandProcessor& GetProcessor(uint32_t queue_id);

	RenderContext&                                 m_renderer;
	Common::Mutex                                  m_queue_mutex;
	std::mutex                                     m_shutdown_mutex;
	Common::CondVar                                m_work_available;
	Common::CondVar                                m_idle;
	std::array<std::deque<Submission>, QueueCount> m_queues;
	std::deque<Common::UniqueFunction<void>>       m_commands;
	std::atomic_uint32_t                           m_pending_commands {0};
	std::atomic<uint64_t>                          m_compute_enqueued {0};
	uint32_t                                       m_next_queue        = 0;
	uint32_t                                       m_submission_count  = 0;
	bool                                           m_processing        = false;
	bool                                           m_accepting         = true;
	bool                                           m_stopping          = false;
	bool                                           m_shutdown_complete = false;
	// Completion callbacks can outlive GuestGpu during renderer shutdown.
	std::shared_ptr<std::binary_semaphore> m_suspend_point_ready =
	    std::make_shared<std::binary_semaphore>(1);

	struct PendingLabel {
		uint64_t value    = 0;
		uint32_t size     = 0;
		uint64_t sequence = 0;
	};
	// Label writes recorded but not yet visible to the CPU. GPU thread only.
	std::unordered_map<uint64_t, PendingLabel> m_pending_labels;
	uint64_t                                   m_label_sequence = 0;

	std::unique_ptr<CommandProcessor>                                m_gfx_cp;
	std::array<std::unique_ptr<CommandProcessor>, ComputeQueueCount> m_compute_cp;
	// KYTY_DRAW_PREP: the scanner, and the number of the last graphics submission queued
	// (under m_queue_mutex).
	std::unique_ptr<DrawPrepScanner> m_draw_prep;
	// Round 170: the compute queues' scanner (null: they go to m_draw_prep, as before).
	std::unique_ptr<DrawPrepScanner> m_draw_prep_compute;
	uint64_t                         m_prep_seq = 0;

	uint64_t        m_submit_id = 0;
	std::atomic_int m_done_num  = 0;
	std::jthread    m_thread;

	friend class CommandProcessor;
};
} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRUN_H_ */
