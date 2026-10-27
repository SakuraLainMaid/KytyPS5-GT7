#ifndef EMULATOR_SRC_GRAPHICS_GUEST_GPU_DRAWPREPSCANNER_H_
#define EMULATOR_SRC_GRAPHICS_GUEST_GPU_DRAWPREPSCANNER_H_

#include "graphics/host_gpu/renderer/drawPrep.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <vector>

namespace Libs::Graphics {

class RenderContext;
class CommandProcessor;

namespace HW {
class Context;
} // namespace HW

// The draw-prep scanner (host_gpu/renderer/drawPrep.h; KYTY_DRAW_PREP=0 turns it off): reads each
// graphics submission when it is queued, ahead of the GPU thread, follows its register writes on a
// command processor of its own (which never executes anything), and prepares the descriptor walks
// of its draws for the GPU thread to take.
//
// Since round 73 the walks run on workers (KYTY_DRAW_PREP_WORKERS, default 2; 0 walks on the
// scanner thread, as KYTY_BISECT's scan:one does): one thread walked ~7.5 us a stage, slower than
// the GPU thread used them (~13 us a draw of two stages), so the GPU thread overtook it and walked
// a fifth of the stages itself (~5.8 us each). The results are published in packet order.
class DrawPrepScanner final {
public:
	// `name`: for the log ("graphics", "compute": round 170 scans the compute queues on a
	// scanner of their own, so they wait behind no graphics submission).
	DrawPrepScanner(RenderContext& renderer, const char* name);
	~DrawPrepScanner();
	DrawPrepScanner(const DrawPrepScanner&)            = delete;
	DrawPrepScanner& operator=(const DrawPrepScanner&) = delete;

	// Under the GPU queue lock, in queue order: a submission's command stream (graphics queue 0,
	// compute queues 1..), and the register reset the GPU thread does at a suspend point.
	void EnqueueSubmission(uint32_t queue, uint64_t seq, std::span<const uint32_t> commands);
	void EnqueueReset(uint32_t queue);

private:
	struct Item {
		uint32_t                  queue = 0;
		uint64_t                  seq   = 0; // 0: a register reset
		std::span<const uint32_t> commands;
	};
	// The register copy of one GPU queue (each queue has its own command processor).
	struct QueueState {
		std::unique_ptr<CommandProcessor> cp;
		std::unique_ptr<HW::Context>      saved_ctx; // the context-state push
		bool                              tainted = false;
		bool                              pushed  = false;
	};
	QueueState& State(uint32_t queue);

	// A draw's or dispatch's walks, for a worker: its place in the order of publication.
	struct Job {
		uint64_t                      index = 0;
		DrawPrep::Key                 key;
		PipelineCache::DrawPrepInputs inputs;
	};

	void Run();
	void WorkerRun();
	// Scanner thread: the walks of a packet whose inputs were described, to a worker (or here).
	void Dispatch(const DrawPrep::Key& key, const PipelineCache::DrawPrepInputs& inputs);
	void Walk(Job& job);
	// Publishes the result of job `index` (none: nothing prepared) once those before it are.
	void Finish(uint64_t index, std::unique_ptr<DrawPrep::PacketResult> result);
	void Scan(const Item& item);
	// `packet` is in the scanner's copy of the stream, `guest_packet` where the GPU thread reads it
	// (the key both sides match).
	void ScanDraw(uint64_t seq, uint32_t ordinal, const uint32_t* guest_packet);
	void ScanDispatch(uint64_t seq, uint32_t ordinal, const uint32_t* packet,
	                  const uint32_t* guest_packet, uint32_t opcode);
	bool ApplyRegisterIndirect(uint32_t header, const uint32_t* body);
	void ApplyContextState(uint32_t operation);

	RenderContext&                         m_renderer;
	std::array<std::unique_ptr<QueueState>, 64> m_queues;
	QueueState*                            m_state = nullptr; // of the item being scanned
	uint32_t                               m_queue = 0;

	std::mutex              m_mutex;
	std::condition_variable m_work;
	std::deque<Item>        m_items;
	bool                    m_stop = false;

	// The workers' queue, and the results that wait for earlier ones to be published.
	std::mutex                                                m_job_mutex;
	std::condition_variable                                   m_job_ready;
	std::condition_variable                                   m_job_space;
	std::deque<Job>                                           m_jobs;
	bool                                                      m_job_stop   = false;
	uint64_t                                                  m_next_index = 0; // scanner
	std::mutex                                                m_order_mutex;
	std::map<uint64_t, std::unique_ptr<DrawPrep::PacketResult>> m_done; // null: none
	uint64_t                                                  m_next_publish = 0;
	std::atomic<bool>                                         m_stopping {false};
	std::vector<std::thread>                                  m_workers;
	std::thread                                               m_thread;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_GUEST_GPU_DRAWPREPSCANNER_H_
