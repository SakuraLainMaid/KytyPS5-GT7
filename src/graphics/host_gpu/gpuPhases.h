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
	TessDraw,       // a tessellated draw, whole (inside Draw; round 78)
	TessPrograms,   // its shaders (inside DrawPrograms)
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
// The step a page protection change is charged to: the innermost open scope of a whole
// operation (a slice, a command, a draw, a dispatch, a BDA walk) or of a draw's or a dispatch's
// steps; Count outside them. And the protection changes by that step (GPU thread only).
inline thread_local Phase                                          t_step = Phase::Count;
inline std::array<uint64_t, static_cast<size_t>(Phase::Count) + 1> g_protect_cycles {};
inline std::array<uint64_t, static_cast<size_t>(Phase::Count) + 1> g_protect_count {};
// Round 80: the pages those changes covered, and what they set the pages to (no access,
// read-only, read-write), by step.
inline std::array<uint64_t, static_cast<size_t>(Phase::Count) + 1> g_protect_pages {};
inline std::array<std::array<uint64_t, 3>, static_cast<size_t>(Phase::Count) + 1>
                             g_protect_modes {};
// Uploads of guest bytes to the GPU by the step they were made in (round 81): bytes, uploads and
// the copies they were made of.
inline std::array<uint64_t, static_cast<size_t>(Phase::Count) + 1> g_upload_bytes {};
inline std::array<uint64_t, static_cast<size_t>(Phase::Count) + 1> g_upload_count {};
inline std::array<uint64_t, static_cast<size_t>(Phase::Count) + 1> g_upload_copies {};
[[nodiscard]] constexpr bool ChargesProtects(Phase phase) noexcept {
	return phase == Phase::Process || phase == Phase::Command || phase == Phase::Draw ||
	       phase == Phase::Dispatch || phase == Phase::BdaWalk ||
	       (phase >= Phase::DrawPending && phase <= Phase::DispatchCommit);
}
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

// A page protection change of `bytes` to `mode` (0 no access, 1 read-only, 2 read-write), charged
// to the step a Protect scope would charge it to, on the GPU thread (round 80).
inline void NoteProtect(uint64_t bytes, uint32_t mode) noexcept {
	if (!Detail::t_on) {
		return;
	}
	const auto step = static_cast<size_t>(Detail::t_step);
	Detail::g_protect_pages[step] += (bytes + 4095u) >> 12u;
	Detail::g_protect_modes[step][mode < 2u ? mode : 2u]++;
}

// An upload of `bytes` of guest memory in `copies` copies, charged to the step as a page
// protection change is, on the GPU thread (round 81).
inline void NoteUpload(uint64_t bytes, uint64_t copies) noexcept {
	if (!Detail::t_on) {
		return;
	}
	const auto step = static_cast<size_t>(Detail::t_step);
	Detail::g_upload_bytes[step] += bytes;
	Detail::g_upload_count[step]++;
	Detail::g_upload_copies[step] += copies;
}

// The count of `phase` so far (GPU thread).
[[nodiscard]] inline uint64_t CountOf(Phase phase) noexcept {
	return Detail::g_totals.count[static_cast<size_t>(phase)];
}

// Times its block on the GPU thread.
class Scope {
public:
	explicit Scope(Phase phase) noexcept: m_phase(phase), m_start(Detail::t_on ? Now() : 0) {
		if (m_start != 0) {
			m_step = Detail::t_step;
			if (Detail::ChargesProtects(phase)) {
				Detail::t_step = phase;
				m_set_step     = true;
			}
		}
	}
	~Scope() { End(); }
	// Ends the timing before the block does (and its charge of protection changes).
	void End() noexcept {
		if (m_start != 0) {
			const auto cycles = Now() - m_start;
			Add(m_phase, cycles);
			if (m_phase == Phase::Protect) {
				Detail::g_protect_cycles[static_cast<size_t>(m_step)] += cycles;
				Detail::g_protect_count[static_cast<size_t>(m_step)]++;
			}
			m_start = 0;
		}
		RestoreStep();
	}
	// Drops the timing: the block is not counted.
	void Cancel() noexcept {
		m_start = 0;
		RestoreStep();
	}
	// Times the same phase again from here (after an End); a second count.
	void Restart() noexcept {
		End();
		m_start = Detail::t_on ? Now() : 0;
		if (m_start != 0) {
			m_step = Detail::t_step;
			if (Detail::ChargesProtects(m_phase)) {
				Detail::t_step = m_phase;
				m_set_step     = true;
			}
		}
	}
	Scope(const Scope&)            = delete;
	Scope& operator=(const Scope&) = delete;

private:
	void RestoreStep() noexcept {
		if (m_set_step) {
			Detail::t_step = m_step;
			m_set_step     = false;
		}
	}

	Phase    m_phase;
	uint64_t m_start;
	Phase    m_step     = Phase::Count; // t_step when it began
	bool     m_set_step = false;        // it set t_step (to restore when it ends)
};

// GPU thread, at its start: turns the phases on there (KYTY_GPU_PHASES=0 leaves them off).
void Start();
// GPU thread: logs the "GPU phases" line every 10 s; `frames`: the frames the guest has finished.
void ReportIfDue(uint64_t frames);

} // namespace Libs::Graphics::GpuPhases

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_GPUPHASES_H_
