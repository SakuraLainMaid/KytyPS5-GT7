#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_GPUPHASES_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_GPUPHASES_H_

#include <array>
#include <cstdint>

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

// Diagnostics: where the GPU thread's time goes, by step of a draw and of a dispatch. Each step is
// timed with the processor's time-stamp counter on the GPU thread only (a scope on another thread,
// the draw-prep scanner's among them, does nothing), and every 10 s a "GPU phases" line gives the
// microseconds each step takes per draw or dispatch, with counts (gpuPhases.cpp). The scopes nest:
// a step's time includes the steps inside it. On by default; KYTY_GPU_PHASES=0 turns it off.
//
// Everything a scope needs is inline here, so files built into the unit tests (pageManager.cpp)
// can use one without the report's file.
namespace Libs::Graphics::GpuPhases {

enum class Phase : uint32_t {
	// Whole operations on the GPU thread.
	Process,  // a slice of a guest submission (GuestGpu::Process)
	Command,  // a command run on the GPU thread (a game thread's read of GPU memory, ...)
	Draw,     // a draw (DrawIndex, DrawAuto)
	Dispatch, // a dispatch (DispatchDirect, DispatchIndirect)

	// Steps of a draw.
	DrawPending,   // the deferred operations it runs first (PopPendingOperations)
	DrawConsume,   // checks for a draw that is a copy, resolve or metadata operation instead
	DrawPrograms,  // its shaders: GetGraphicsPrograms (Describe and ProgramGet inside)
	DrawTargets,   // its render targets' images (ResolveRenderColorTarget / DepthTarget)
	DrawTextures,  // PrepareBindings of its stages: textures, samplers, bindless heaps
	DrawFind,      // FindBuffers: the cached buffers of its buffer bindings
	DrawBda,       // PrepareBda: CPU-dirty buffers synchronized for memory-reading shaders
	DrawImages,    // RebindImages and its render targets again: image views
	DrawBuffers,   // RebindBuffers: buffer views, synchronization, the uploaded SRT and data
	DrawVertex,    // vertex and index buffers
	DrawPipeline,  // TryGetGraphicsPipeline
	DrawRendering, // AcquireRenderTargets: attachments, clears, layouts
	DrawCommit,    // CommitBindings: layouts, descriptors, push data
	DrawEmit,      // the rest of ExecutePreparedDraw: dynamic state, the draw, its barriers

	// Steps of a dispatch.
	DispatchPending,
	DispatchProgram,  // GetComputeProgram (ProgramGet inside)
	DispatchClears,   // TryConsumeComputeMetaClear / ImageClear
	DispatchFill,     // ResolveDispatchFill and fills of mapped parts
	DispatchPipeline, // GetComputePipeline
	DispatchTextures, // PrepareBindings
	DispatchFind,     // FindBuffers
	DispatchBda,      // PrepareBda
	DispatchImages,   // RebindImages
	DispatchBuffers,  // RebindBuffers
	DispatchCommit,   // CommitBindings

	// Shared steps, inside the ones above.
	Describe,       // DescribeGraphicsStages (PrepareProgram of each stage)
	ProgramGet,     // ProgramCache::Get of one stage (draw or dispatch)
	ProgramKey,     // its key and lookup
	ProgramScanned, // its take of the draw-prep scanner's result (with the check of its bytes)
	ProgramWalk,    // its own descriptor walk (MaterializeResources), when it had no result
	BufferSync,     // BufferCache::SynchronizeBuffer
	Protect,        // page protection changes (PageManager)
	Flush,          // a command buffer ended and a new one begun (CommandScheduler::Flush)
	BdaWalk,        // PrepareBda's walk of CPU-dirty buffers (at most once a guest submission)
	BdaRegions,     // (a count only) the tracker regions those walks visited
	BdaProtects,    // (a count only) the page protection changes those walks made
	SpecCheck,      // CommitBindings' check of specialization reads against the draw's writes
	Count
};

constexpr size_t PhaseCount = static_cast<size_t>(Phase::Count);

struct Totals {
	std::array<uint64_t, PhaseCount> cycles {};
	std::array<uint64_t, PhaseCount> count {};
};

namespace Detail {
// Set on the GPU thread while the phases are on (Start).
inline thread_local bool t_on = false;
// Written by the GPU thread only.
inline Totals g_totals;
} // namespace Detail

[[nodiscard]] inline uint64_t Now() noexcept {
	return __rdtsc();
}

inline void Add(Phase phase, uint64_t cycles) noexcept {
	const auto index = static_cast<size_t>(phase);
	Detail::g_totals.cycles[index] += cycles;
	Detail::g_totals.count[index]++;
}

// Adds `n` to the count of `phase` without time (a count-only phase), on the GPU thread.
inline void Count(Phase phase, uint64_t n) noexcept {
	if (Detail::t_on) {
		Detail::g_totals.count[static_cast<size_t>(phase)] += n;
	}
}

// The count of `phase` so far (GPU thread).
[[nodiscard]] inline uint64_t CountOf(Phase phase) noexcept {
	return Detail::g_totals.count[static_cast<size_t>(phase)];
}

// Times its block on the GPU thread.
class Scope {
public:
	explicit Scope(Phase phase) noexcept: m_phase(phase), m_start(Detail::t_on ? Now() : 0) {}
	~Scope() { End(); }
	// Ends the timing before the block does.
	void End() noexcept {
		if (m_start != 0) {
			Add(m_phase, Now() - m_start);
			m_start = 0;
		}
	}
	// Drops the timing: the block is not counted.
	void Cancel() noexcept { m_start = 0; }
	// Times the same phase again from here (after an End); a second count.
	void Restart() noexcept {
		End();
		m_start = Detail::t_on ? Now() : 0;
	}
	Scope(const Scope&)            = delete;
	Scope& operator=(const Scope&) = delete;

private:
	Phase    m_phase;
	uint64_t m_start;
};

// GPU thread, at its start: turns the phases on there (KYTY_GPU_PHASES=0 leaves them off).
void Start();
// GPU thread: logs the "GPU phases" line every 10 s; `frames`: the frames the guest has finished.
void ReportIfDue(uint64_t frames);

} // namespace Libs::Graphics::GpuPhases

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_GPUPHASES_H_
