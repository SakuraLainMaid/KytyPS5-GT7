#include "common/sampler.h"

#include "common/common.h"
#include "common/hostSymbols.h"
#include "common/logging/log.h"
#include "common/perfStats.h"
#include "common/x64MemoryOperand.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // IWYU pragma: keep
#endif

namespace Common::Sampler {

namespace {

std::vector<std::string> Patterns() {
	std::vector<std::string> patterns;
	const char*              value = std::getenv("KYTY_SAMPLE");
	if (value == nullptr || value[0] == '\0' || std::string_view(value) == "0") {
		return patterns;
	}
	if (std::string_view(value) == "1") {
		// Every job thread (round 88): the game's loading moves between them, and a thread
		// busy at 100% went unseen while Job#0 and Job#1 waited.
		return {"Thread_Gpu", "Job#*", "GpuRecord", "Thread_DrawPrep", "GpuPriority"};
	}
	std::string_view rest(value);
	while (!rest.empty()) {
		const auto comma = rest.find(',');
		auto       name  = rest.substr(0, comma);
		while (!name.empty() && name.front() == ' ') {
			name.remove_prefix(1);
		}
		while (!name.empty() && name.back() == ' ') {
			name.remove_suffix(1);
		}
		if (!name.empty()) {
			patterns.emplace_back(name);
		}
		rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);
	}
	return patterns;
}

[[maybe_unused]] bool Matches(const std::string& name, const std::vector<std::string>& patterns) {
	return std::ranges::any_of(patterns, [&](const std::string& pattern) {
		if (!pattern.empty() && pattern.back() == '*') {
			return name.starts_with(std::string_view(pattern).substr(0, pattern.size() - 1));
		}
		return name == pattern;
	});
}

[[maybe_unused]] uint32_t EnvNumber(const char* name, uint32_t fallback, uint32_t low,
                                    uint32_t high) {
	const char* value = std::getenv(name);
	if (value == nullptr || value[0] == '\0') {
		return fallback;
	}
	return std::clamp(static_cast<uint32_t>(std::strtoul(value, nullptr, 10)), low, high);
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

constexpr size_t MaxFrames  = 48;
constexpr size_t PathFrames = 6; // a "paths" entry: the leaf and five frames below it
constexpr size_t ChainDepth = 5; // a "waits" entry: the leaf and four emulator frames
// The stack copied at each sample, and zeroed room after it (what a sample did not copy is zeroed
// too): the unwinder reads a frame's saved registers above its stack pointer, so a frame that
// starts in the copy and runs past its end reads zeros instead of memory past the buffer (a frame
// larger than the room would not fit a thread's stack).
constexpr size_t StackCopyBytes = 64 * 1024;
constexpr size_t UnwindPadBytes = 1024 * 1024;

// The game's code (its modules and the instruction trampolines after them are mapped from
// 0x8_0000_0000 up, below the emulator's own heaps): the samples there are reported with their
// exact addresses, callers and code bytes ("Sample <thread> guest"), to be disassembled offline.
constexpr uint64_t GuestCodeBegin = 0x8'0000'0000ull;
constexpr uint64_t GuestCodeEnd   = 0x10'0000'0000ull;

[[nodiscard]] bool IsGuestCode(uint64_t address) {
	return address >= GuestCodeBegin && address < GuestCodeEnd;
}

// Up to PathFrames addresses, zero-padded.
struct PathKey {
	std::array<uint64_t, PathFrames> frames {};
	[[nodiscard]] bool               operator==(const PathKey&) const = default;
};

struct PathKeyHash {
	size_t operator()(const PathKey& key) const noexcept {
		uint64_t hash = 0xcbf29ce484222325ull;
		for (const auto frame: key.frames) {
			hash = (hash ^ frame) * 0x100000001b3ull;
			hash ^= hash >> 29u;
		}
		return static_cast<size_t>(hash);
	}
};

struct Target {
	uint32_t    tid = 0;
	std::string name;
	HANDLE      handle   = nullptr;
	uint64_t    samples  = 0;
	uint64_t    in_image = 0; // samples whose leaf is in the emulator's own code
	uint64_t    frames   = 0; // frames walked, all samples
	uint64_t    deep     = 0; // samples walked four frames or more
	// Leaf addresses, and the emulator's addresses on the stack (once a sample), by samples.
	std::unordered_map<uint64_t, uint32_t> self;
	std::unordered_map<uint64_t, uint32_t> total;
	// The leaf and the frames below it; the leaf and the emulator's frames below it.
	std::unordered_map<PathKey, uint32_t, PathKeyHash> paths;
	std::unordered_map<PathKey, uint32_t, PathKeyHash> chains;
	// Samples whose leaf is in the game's code: by the leaf's 256-byte bucket, its callers (the
	// return address of the frame-pointer chain's first frame).
	std::unordered_map<uint64_t, std::unordered_map<uint64_t, uint32_t>> guest_callers;
	// Round 107: samples in the game's code by the instruction they stopped at (past the red zone
	// trampolines' stack adjustments) and the address its memory operand names (GuestOperand).
	std::map<std::pair<uint64_t, uint64_t>, uint32_t> guest_loads;
};

// Round 107: the memory operand of the instruction a guest sample stopped at, and that
// instruction's address. A sample often stops on a red zone trampoline's "lea rsp, [rsp -/+ 0x80]"
// or on the jump into or out of one; those are stepped over (with the stack pointer they change),
// up to four of them.
bool GuestOperand(const CONTEXT& context, uint64_t* pc_out, uint64_t* address) {
	X64::Registers regs {context.Rax, context.Rcx, context.Rdx, context.Rbx,
	                     context.Rsp, context.Rbp, context.Rsi, context.Rdi,
	                     context.R8,  context.R9,  context.R10, context.R11,
	                     context.R12, context.R13, context.R14, context.R15};
	static constexpr std::array<uint8_t, 5> RedZoneDown {0x48, 0x8d, 0x64, 0x24, 0x80};
	static constexpr std::array<uint8_t, 8> RedZoneUp {0x48, 0x8d, 0xa4, 0x24,
	                                                   0x80, 0x00, 0x00, 0x00};
	uint64_t                                pc = context.Rip;
	for (int step = 0; step < 5; step++) {
		std::array<uint8_t, 16> code {};
		SIZE_T                  read = 0;
		if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(pc), code.data(),
		                      code.size(), &read) == FALSE ||
		    read != code.size()) {
			return false;
		}
		if (std::memcmp(code.data(), RedZoneDown.data(), RedZoneDown.size()) == 0) {
			regs[4] -= 0x80;
			pc += RedZoneDown.size();
			continue;
		}
		if (std::memcmp(code.data(), RedZoneUp.data(), RedZoneUp.size()) == 0) {
			regs[4] += 0x80;
			pc += RedZoneUp.size();
			continue;
		}
		if (code[0] == 0xe9) {
			int32_t rel = 0;
			std::memcpy(&rel, &code[1], sizeof(rel));
			pc = pc + 5u + static_cast<uint64_t>(static_cast<int64_t>(rel));
			continue;
		}
		*pc_out = pc;
		return X64::MemoryOperandAddress(code, regs, address);
	}
	return false;
}

struct Image {
	uint64_t           begin = 0;
	uint64_t           end   = 0;
	[[nodiscard]] bool Contains(uint64_t address) const {
		return address >= begin && address < end;
	}
};

Image EmulatorImage() {
	Image       image;
	const auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
	if (base == nullptr) {
		return image;
	}
	const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
		return image;
	}
	const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE) {
		return image;
	}
	image.begin = reinterpret_cast<uint64_t>(base);
	image.end   = image.begin + nt->OptionalHeader.SizeOfImage;
	return image;
}

// One sample's registers and the top of its stack. Copied while the thread is suspended: nothing
// is allocated then (the thread may hold the heap's lock), and the stack it describes cannot
// change underneath. The walk runs on the copy after the thread has gone on.
struct Capture {
	CONTEXT                                              context {};
	std::array<uint8_t, StackCopyBytes + UnwindPadBytes> stack {};
	size_t                                               copied = 0;
};

bool CaptureThread(HANDLE thread, Capture& capture) {
	if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
		return false;
	}
	capture.context              = {};
	capture.context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
	const bool ok                = GetThreadContext(thread, &capture.context) != FALSE;
	capture.copied               = 0;
	if (ok) {
		// The stack's committed pages from the stack pointer up (one region up to the stack's
		// base), at most StackCopyBytes; halved until a read succeeds if that is unknown.
		size_t                   size = StackCopyBytes;
		MEMORY_BASIC_INFORMATION info {};
		if (VirtualQuery(reinterpret_cast<const void*>(capture.context.Rsp), &info,
		                 sizeof(info)) != 0 &&
		    info.State == MEM_COMMIT) {
			const auto end = reinterpret_cast<uint64_t>(info.BaseAddress) + info.RegionSize;
			if (end > capture.context.Rsp) {
				size = static_cast<size_t>(std::min<uint64_t>(size, end - capture.context.Rsp));
			}
		}
		for (; size >= 64 && capture.copied == 0; size /= 2) {
			SIZE_T read = 0;
			if (ReadProcessMemory(GetCurrentProcess(),
			                      reinterpret_cast<const void*>(capture.context.Rsp),
			                      capture.stack.data(), size, &read) != FALSE) {
				capture.copied = read;
			}
		}
	}
	ResumeThread(thread);
	// Nothing of an earlier sample (another thread's, maybe) past this copy.
	if (capture.copied < StackCopyBytes) {
		std::memset(capture.stack.data() + capture.copied, 0, StackCopyBytes - capture.copied);
	}
	return ok;
}

// The register a function's unwind data names as its frame register (null: none), from its
// UNWIND_INFO header (the frame register is the low nibble of its fourth byte; 0 is none).
DWORD64* FrameRegister(CONTEXT& context, DWORD64 image_base, const RUNTIME_FUNCTION& function) {
	const auto* info = reinterpret_cast<const uint8_t*>(image_base + function.UnwindData);
	switch (info[3] & 0x0fu) {
		case 1: return &context.Rcx;
		case 2: return &context.Rdx;
		case 3: return &context.Rbx;
		case 4: return &context.Rsp;
		case 5: return &context.Rbp;
		case 6: return &context.Rsi;
		case 7: return &context.Rdi;
		case 8: return &context.R8;
		case 9: return &context.R9;
		case 10: return &context.R10;
		case 11: return &context.R11;
		case 12: return &context.R12;
		case 13: return &context.R13;
		case 14: return &context.R14;
		case 15: return &context.R15;
		default: return nullptr;
	}
}

// The leaf, then the return addresses of the frame-pointer chain within the copied stack: for a
// leaf in the game's code, whose frames have no unwind data (the chain stops where it leaves the
// copy or stops rising).
size_t WalkFramePointers(const Capture& capture, std::array<uint64_t, MaxFrames>& frames) {
	size_t count    = 0;
	frames[count++] = capture.context.Rip;
	const auto base = capture.context.Rsp;
	auto       rbp  = capture.context.Rbp;
	while (count < MaxFrames && rbp >= base && rbp - base + 16 <= capture.copied &&
	       (rbp & 7u) == 0) {
		uint64_t   next = 0;
		uint64_t   ret  = 0;
		const auto at   = static_cast<size_t>(rbp - base);
		std::memcpy(&next, capture.stack.data() + at, sizeof(next));
		std::memcpy(&ret, capture.stack.data() + at + 8, sizeof(ret));
		if (ret == 0) {
			break;
		}
		frames[count++] = ret;
		if (next <= rbp) {
			break;
		}
		rbp = next;
	}
	return count;
}

// The leaf, then the return addresses the Windows unwind data gives (RtlVirtualUnwind), on the
// copy: every register that points into the copied stack is moved into the copy before each
// step, so the unwinder reads the copy, not the thread's live stack. The walk stops at the game's
// code (no unwind data), at a frame outside the copy, at a frame whose frame register points
// outside it (one that runs past the copy's end), at a frame below the leaf without unwind data,
// or after MaxFrames.
size_t UnwindFrames(const Capture& capture, std::array<uint64_t, MaxFrames>& frames) {
	size_t count    = 0;
	frames[count++] = capture.context.Rip;
	if (capture.copied < 64) {
		return count;
	}
	CONTEXT        context  = capture.context;
	const uint64_t original = capture.context.Rsp;
	const uint64_t copy     = reinterpret_cast<uint64_t>(capture.stack.data());
	const uint64_t copied   = capture.copied;
	const auto     relocate = [&] {
        for (DWORD64* reg: {&context.Rax, &context.Rcx, &context.Rdx, &context.Rbx, &context.Rsp,
                            &context.Rbp, &context.Rsi, &context.Rdi, &context.R8, &context.R9,
                            &context.R10, &context.R11, &context.R12, &context.R13, &context.R14,
                            &context.R15}) {
            if (*reg >= original && *reg < original + copied) {
                *reg = *reg - original + copy;
            }
        }
	};
	relocate();
	while (count < MaxFrames) {
		const uint64_t pc = context.Rip;
		if (pc == 0 || IsGuestCode(pc)) {
			break;
		}
		if (context.Rsp < copy || context.Rsp + 8 > copy + copied) {
			break;
		}
		DWORD64    image_base = 0;
		const auto function   = RtlLookupFunctionEntry(pc, &image_base, nullptr);
		if (function == nullptr) {
			// A leaf function needs no unwind data: its return address is at the stack pointer.
			// Below the leaf, a frame without it is code the walk cannot follow.
			if (count != 1) {
				break;
			}
			uint64_t ret = 0;
			std::memcpy(&ret, reinterpret_cast<const void*>(context.Rsp), sizeof(ret));
			context.Rip = ret;
			context.Rsp += 8;
		} else {
			// A frame register outside the copy: the unwinder would read the live stack there.
			if (const auto* frame = FrameRegister(context, image_base, *function);
			    frame != nullptr && (*frame < copy || *frame >= copy + copied)) {
				break;
			}
			PVOID   handler_data = nullptr;
			DWORD64 establisher  = 0;
			RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, pc, function, &context, &handler_data,
			                 &establisher, nullptr);
			// A machine frame (an exception's) gives the interrupted stack pointer as it was.
			relocate();
		}
		if (context.Rip == 0) {
			break;
		}
		frames[count++] = context.Rip;
	}
	return count;
}

// A leaf where a thread waits in the kernel: a lock, a condition variable, an event, a fence
// (the display driver's wait), a sleep or a yield.
[[nodiscard]] bool IsWaitName(std::string_view name) {
	if (const auto bang = name.find('!'); bang != std::string_view::npos) {
		name.remove_prefix(bang + 1);
	}
	if (name.find("GdiDdDDIWait") != std::string_view::npos ||
	    name.find("RemoveIoCompletion") != std::string_view::npos) {
		return true;
	}
	if (!name.starts_with("Nt") && !name.starts_with("Zw")) {
		return false;
	}
	return name.find("Wait") != std::string_view::npos ||
	       name.find("Delay") != std::string_view::npos ||
	       name.find("Yield") != std::string_view::npos;
}

// Names in paths: without the namespaces every emulator name repeats, at most 70 characters.
[[nodiscard]] std::string Shorten(std::string name) {
	for (const std::string_view prefix: {"Libs::Graphics::ShaderRecompiler::IR::",
	                                     "Libs::Graphics::", "Libs::LibKernel::Memory::",
	                                     "Libs::LibKernel::", "Libs::", "Common::"}) {
		for (size_t at = name.find(prefix); at != std::string::npos; at = name.find(prefix, at)) {
			name.erase(at, prefix.size());
		}
	}
	if (name.size() > 70) {
		name.resize(70);
	}
	return name;
}

class Sampler {
public:
	Sampler(std::vector<std::string> patterns, uint32_t hz, uint32_t report_seconds)
	    : m_patterns(std::move(patterns)), m_hz(hz), m_report_seconds(report_seconds),
	      m_image(EmulatorImage()) {}

	void Run() {
		HANDLE timer = CreateWaitableTimerExW(
		    nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
		const auto period_100ns = static_cast<LONGLONG>(10'000'000 / m_hz);
		auto       capture      = std::make_unique<Capture>();
		auto       last_scan    = std::chrono::steady_clock::now() - std::chrono::seconds(10);
		auto       last_report  = std::chrono::steady_clock::now();
		for (;;) {
			if (timer != nullptr) {
				LARGE_INTEGER due {};
				due.QuadPart = -period_100ns;
				if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) != FALSE) {
					WaitForSingleObject(timer, INFINITE);
				} else {
					Sleep(1);
				}
			} else {
				Sleep(1);
			}
			const auto now = std::chrono::steady_clock::now();
			if (now - last_scan >= std::chrono::seconds(1)) {
				Scan();
				last_scan = now;
			}
			for (auto& target: m_targets) {
				if (target->handle != nullptr) {
					Sample(*target, *capture);
				}
			}
			if (now - last_report >= std::chrono::seconds(m_report_seconds)) {
				Report(std::chrono::duration<double>(now - last_report).count());
				last_report = std::chrono::steady_clock::now();
			}
		}
	}

private:
	// New threads with the names asked for; ended ones are let go.
	void Scan() {
		const auto self = GetCurrentThreadId();
		for (const auto& [tid, name]: PerfStats::NamedThreads()) {
			if (tid == self || !Matches(name, m_patterns) ||
			    std::ranges::any_of(m_targets, [tid](const auto& t) { return t->tid == tid; })) {
				continue;
			}
			HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
			                               THREAD_QUERY_LIMITED_INFORMATION,
			                           FALSE, tid);
			if (handle == nullptr) {
				continue;
			}
			auto target    = std::make_unique<Target>();
			target->tid    = tid;
			target->name   = name;
			target->handle = handle;
			Log::WriteUnmuted(fmt::format("Sample: sampling {} (tid {})\n", name, tid));
			m_targets.push_back(std::move(target));
		}
		for (auto& target: m_targets) {
			DWORD code = 0;
			if (target->handle != nullptr &&
			    (GetExitCodeThread(target->handle, &code) == FALSE || code != STILL_ACTIVE)) {
				CloseHandle(target->handle);
				target->handle = nullptr;
			}
		}
	}

	void Sample(Target& target, Capture& capture) {
		if (!CaptureThread(target.handle, capture)) {
			return;
		}
		std::array<uint64_t, MaxFrames> frames {};
		const bool                      guest_leaf = IsGuestCode(capture.context.Rip);
		const auto count = guest_leaf ? WalkFramePointers(capture, frames)
		                              : UnwindFrames(capture, frames);
		target.samples++;
		target.frames += count;
		target.deep += count >= 4 ? 1u : 0u;
		if (m_image.Contains(frames[0])) {
			target.in_image++;
		}
		target.self[frames[0]]++;
		if (guest_leaf) {
			target.guest_callers[frames[0] & ~uint64_t {0xff}][count > 1 ? frames[1] : 0]++;
			uint64_t insn    = 0;
			uint64_t address = 0;
			if (GuestOperand(capture.context, &insn, &address)) {
				const auto key = std::make_pair(insn, address);
				if (const auto it = target.guest_loads.find(key); it != target.guest_loads.end()) {
					it->second++;
				} else if (target.guest_loads.size() < 8192) {
					target.guest_loads.emplace(key, 1u);
				}
			}
			return;
		}
		for (size_t i = 0; i < count; i++) {
			// The emulator's own frames, once a sample (recursion counts once).
			if (!m_image.Contains(frames[i]) ||
			    std::find(frames.begin(), frames.begin() + static_cast<std::ptrdiff_t>(i),
			              frames[i]) != frames.begin() + static_cast<std::ptrdiff_t>(i)) {
				continue;
			}
			target.total[frames[i]]++;
		}
		PathKey path;
		for (size_t i = 0; i < std::min(count, PathFrames); i++) {
			path.frames[i] = frames[i];
		}
		target.paths[path]++;
		PathKey chain;
		chain.frames[0] = frames[0];
		size_t depth    = 1;
		for (size_t i = 1; i < count && depth < ChainDepth; i++) {
			if (m_image.Contains(frames[i])) {
				chain.frames[depth++] = frames[i];
			}
		}
		target.chains[chain]++;
	}

	const std::string& Name(uint64_t address) {
		if (const auto it = m_names.find(address); it != m_names.end()) {
			return it->second;
		}
		auto name = HostSymbol(address);
		if (name.empty()) {
			name = fmt::format("0x{:x}", address & ~uint64_t {0xff});
		} else if (name.size() > 90) {
			name.resize(90);
		}
		return m_names.emplace(address, std::move(name)).first->second;
	}

	// Round 131: the inline frames at a return address and the function they are in, with their
	// lines (Common::HostInlineChain), looked up at the call instruction (the byte before).
	const std::string& Chain(uint64_t return_address) {
		if (const auto it = m_chains.find(return_address); it != m_chains.end()) {
			return it->second;
		}
		auto chain = HostInlineChain(return_address - 1);
		if (chain.size() > 400) {
			chain.resize(400);
		}
		return m_chains.emplace(return_address, std::move(chain)).first->second;
	}

	const std::string& Line(uint64_t address) {
		if (const auto it = m_lines.find(address); it != m_lines.end()) {
			return it->second;
		}
		auto line = HostLine(address);
		if (line.empty()) {
			line = fmt::format("+0x{:x}", address - m_image.begin);
		}
		return m_lines.emplace(address, std::move(line)).first->second;
	}

	[[nodiscard]] static double Share(uint64_t count, uint64_t samples) {
		return 100.0 * static_cast<double>(count) / static_cast<double>(std::max<uint64_t>(samples, 1));
	}

	// The entries by count, largest first.
	template <typename Key>
	static std::vector<std::pair<uint64_t, Key>> Sorted(
	    const std::unordered_map<Key, uint64_t>& counts) {
		std::vector<std::pair<uint64_t, Key>> sorted;
		sorted.reserve(counts.size());
		for (const auto& [key, count]: counts) {
			sorted.emplace_back(count, key);
		}
		std::ranges::sort(sorted, [](const auto& a, const auto& b) { return a.first > b.first; });
		return sorted;
	}

	std::string Top(const std::unordered_map<uint64_t, uint32_t>& counts, uint64_t samples,
	                size_t limit = 30) {
		std::unordered_map<std::string, uint64_t> by_name;
		for (const auto& [address, count]: counts) {
			by_name[Name(address)] += count;
		}
		const auto  sorted = Sorted(by_name);
		std::string out;
		for (size_t i = 0; i < std::min<size_t>(sorted.size(), limit); i++) {
			out += fmt::format("{}{:.1f}% {}", i == 0 ? " " : "; ", Share(sorted[i].first, samples),
			                   sorted[i].second);
		}
		return out;
	}

	// "a < b < c": the names of a path's frames, a frame repeating the one above it (recursion)
	// left out.
	std::string Joined(const PathKey& key) {
		std::string out;
		std::string previous;
		for (const auto frame: key.frames) {
			if (frame == 0) {
				break;
			}
			auto name = Shorten(Name(frame));
			if (name == previous) {
				continue;
			}
			out += out.empty() ? "" : " < ";
			out += name;
			previous = std::move(name);
		}
		return out;
	}

	std::string TopPaths(const Target& target, size_t limit) {
		std::unordered_map<std::string, uint64_t> by_text;
		for (const auto& [key, count]: target.paths) {
			by_text[Joined(key)] += count;
		}
		const auto  sorted = Sorted(by_text);
		std::string out;
		for (size_t i = 0; i < std::min<size_t>(sorted.size(), limit); i++) {
			out += fmt::format("{}{:.1f}% {}", i == 0 ? " " : " | ",
			                   Share(sorted[i].first, target.samples), sorted[i].second);
		}
		return out;
	}

	// The waits by the emulator's frames below them, as a share of all samples, with the share of
	// all samples that waited at all.
	std::string TopWaits(const Target& target, size_t limit, uint64_t& waited) {
		std::unordered_map<std::string, uint64_t> by_text;
		waited = 0;
		for (const auto& [key, count]: target.chains) {
			if (m_image.Contains(key.frames[0]) || !IsWaitName(Name(key.frames[0]))) {
				continue;
			}
			waited += count;
			// Round 131: where in the first emulator frame it waits, inline frames included (a
			// lock taken in an inlined function shows only its caller otherwise).
			auto text = Joined(key);
			if (key.frames[1] != 0) {
				if (const auto& chain = Chain(key.frames[1]); !chain.empty()) {
					text += " [at " + chain + "]";
				}
			}
			by_text[text] += count;
		}
		const auto  sorted = Sorted(by_text);
		std::string out;
		for (size_t i = 0; i < std::min<size_t>(sorted.size(), limit); i++) {
			out += fmt::format("{}{:.1f}% {}", i == 0 ? " " : " | ",
			                   Share(sorted[i].first, target.samples), sorted[i].second);
		}
		return out;
	}

	// The busiest functions of the emulator's own code, each with its busiest source lines.
	std::string TopLines(const Target& target, size_t functions, size_t lines) {
		std::unordered_map<std::string, uint64_t>                         by_function;
		std::unordered_map<std::string, std::vector<std::pair<uint64_t, uint32_t>>> addresses;
		for (const auto& [address, count]: target.self) {
			if (!m_image.Contains(address)) {
				continue;
			}
			const auto& name = Name(address);
			by_function[name] += count;
			addresses[name].emplace_back(address, count);
		}
		const auto  sorted = Sorted(by_function);
		std::string out;
		for (size_t i = 0; i < std::min<size_t>(sorted.size(), functions); i++) {
			std::unordered_map<std::string, uint64_t> by_line;
			for (const auto& [address, count]: addresses[sorted[i].second]) {
				by_line[Line(address)] += count;
			}
			const auto  ranked = Sorted(by_line);
			std::string text;
			for (size_t j = 0; j < std::min<size_t>(ranked.size(), lines); j++) {
				text += fmt::format("{}{} {:.1f}%", j == 0 ? "" : ", ", ranked[j].second,
				                    Share(ranked[j].first, target.samples));
			}
			out += fmt::format("{}{} {:.1f}%: {}", i == 0 ? " " : " | ", Shorten(sorted[i].second),
			                   Share(sorted[i].first, target.samples), text);
		}
		return out;
	}

	// "48 8b 50 38 ..." for [address, address + size), or "?" where it cannot be read.
	static std::string CodeBytes(uint64_t address, size_t size) {
		std::array<uint8_t, 128> bytes {};
		size        = std::min(size, bytes.size());
		SIZE_T read = 0;
		if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
		                      bytes.data(), size, &read) == FALSE ||
		    read != size) {
			return "?";
		}
		std::string out;
		for (size_t i = 0; i < size; i++) {
			out += fmt::format("{}{:02x}", i == 0 ? "" : " ", bytes[i]);
		}
		return out;
	}

	// The game code a thread's samples fell in most: for each of the top buckets, its share, the
	// exact addresses sampled most, the callers, and the code bytes around the hottest address and
	// before the top caller's return address (its call).
	void ReportGuest(const Target& target) {
		if (target.guest_callers.empty()) {
			return;
		}
		std::unordered_map<uint64_t, uint64_t> buckets;
		for (const auto& [address, count]: target.self) {
			if (IsGuestCode(address)) {
				buckets[address & ~uint64_t {0xff}] += count;
			}
		}
		std::vector<std::pair<uint64_t, uint64_t>> sorted(buckets.begin(), buckets.end());
		std::ranges::sort(sorted, [](const auto& a, const auto& b) { return a.second > b.second; });
		const auto share = [&](uint64_t count) { return Share(count, target.samples); };
		for (size_t rank = 0; rank < std::min<size_t>(sorted.size(), 6); rank++) {
			const auto [bucket, count] = sorted[rank];
			if (share(count) < 1.0) {
				break;
			}
			std::vector<std::pair<uint64_t, uint32_t>> exact;
			for (const auto& [address, hits]: target.self) {
				if ((address & ~uint64_t {0xff}) == bucket) {
					exact.emplace_back(address, hits);
				}
			}
			std::ranges::sort(exact,
			                  [](const auto& a, const auto& b) { return a.second > b.second; });
			std::string addresses;
			for (size_t i = 0; i < std::min<size_t>(exact.size(), 4); i++) {
				addresses += fmt::format("{}0x{:x} {:.1f}%", i == 0 ? "" : ", ", exact[i].first,
				                         share(exact[i].second));
			}
			std::string callers;
			uint64_t    top_caller = 0;
			if (const auto it = target.guest_callers.find(bucket);
			    it != target.guest_callers.end()) {
				std::vector<std::pair<uint64_t, uint32_t>> by(it->second.begin(), it->second.end());
				std::ranges::sort(by,
				                  [](const auto& a, const auto& b) { return a.second > b.second; });
				for (size_t i = 0; i < std::min<size_t>(by.size(), 3); i++) {
					if (top_caller == 0 && by[i].first != 0) {
						top_caller = by[i].first;
					}
					callers += fmt::format("{}{} {:.1f}%", i == 0 ? "" : ", ",
					                       by[i].first == 0 ? std::string("(none)")
					                                        : fmt::format("0x{:x}", by[i].first),
					                       share(by[i].second));
				}
			}
			const auto hottest = exact.empty() ? bucket : exact.front().first;
			Log::WriteUnmuted(fmt::format(
			    "Sample {} guest {}: {:.1f}% at 0x{:x}: {} | called from {} | code 0x{:x}: {} | "
			    "call 0x{:x}: {}\n",
			    target.name, rank + 1, share(count), bucket, addresses, callers, hottest - 32,
			    CodeBytes(hottest - 32, 64), top_caller - 16,
			    top_caller != 0 ? CodeBytes(top_caller - 16, 16) : std::string("?")));
		}
		ReportGuestLoads(target);
	}

	// Round 107: the memory the game's code was reading or writing where its samples stopped, by
	// share: the instruction (its first bytes) and its operand's address, with what the address
	// holds now ("Sample <thread> loads"). A thread spinning on a flag shows it at the top.
	void ReportGuestLoads(const Target& target) {
		if (target.guest_loads.empty()) {
			return;
		}
		std::vector<std::pair<std::pair<uint64_t, uint64_t>, uint32_t>> sorted(
		    target.guest_loads.begin(), target.guest_loads.end());
		std::ranges::sort(sorted, [](const auto& a, const auto& b) { return a.second > b.second; });
		std::string text;
		for (size_t i = 0; i < std::min<size_t>(sorted.size(), 8); i++) {
			const auto& [key, count] = sorted[i];
			const auto share         = Share(count, target.samples);
			if (share < 0.5) {
				break;
			}
			uint64_t   value = 0;
			SIZE_T     read  = 0;
			const bool readable =
			    ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(key.second),
			                      &value, sizeof(value), &read) != FALSE &&
			    read == sizeof(value);
			text +=
			    fmt::format("{} {:.1f}% at 0x{:x} ({}) -> 0x{:x} holding {}", i == 0 ? "" : " |",
			                share, key.first, CodeBytes(key.first, 8), key.second,
			                readable ? fmt::format("0x{:016x}", value) : std::string("?"));
		}
		if (!text.empty()) {
			Log::WriteUnmuted(fmt::format("Sample {} loads:{}\n", target.name, text));
		}
	}

	void Report(double seconds) {
		for (auto& target: m_targets) {
			if (target->samples == 0) {
				continue;
			}
			ReportGuest(*target);
			Log::WriteUnmuted(fmt::format(
			    "Sample {} (tid {}): {} samples in {:.0f} s, {:.0f}% in the emulator's code | "
			    "self:{}\n",
			    target->name, target->tid, target->samples, seconds,
			    Share(target->in_image, target->samples), Top(target->self, target->samples, 50)));
			Log::WriteUnmuted(fmt::format("Sample {} (tid {}) total:{}\n", target->name,
			                              target->tid, Top(target->total, target->samples, 70)));
			Log::WriteUnmuted(fmt::format(
			    "Sample {} (tid {}) depth: {:.1f} frames a sample, {:.0f}% walked 4 or more\n",
			    target->name, target->tid,
			    static_cast<double>(target->frames) / static_cast<double>(target->samples),
			    Share(target->deep, target->samples)));
			uint64_t   waited = 0;
			const auto waits  = TopWaits(*target, 16, waited);
			Log::WriteUnmuted(fmt::format("Sample {} (tid {}) waits: {:.1f}% of its samples |{}\n",
			                              target->name, target->tid,
			                              Share(waited, target->samples), waits));
			Log::WriteUnmuted(fmt::format("Sample {} (tid {}) paths:{}\n", target->name,
			                              target->tid, TopPaths(*target, 40)));
			Log::WriteUnmuted(fmt::format("Sample {} (tid {}) lines:{}\n", target->name,
			                              target->tid, TopLines(*target, 24, 6)));
			target->samples  = 0;
			target->in_image = 0;
			target->frames   = 0;
			target->deep     = 0;
			target->self.clear();
			target->total.clear();
			target->paths.clear();
			target->chains.clear();
			target->guest_callers.clear();
			target->guest_loads.clear();
		}
		// The names stay: most addresses come back in the next report.
		if (m_names.size() > 400'000) {
			m_names.clear();
		}
		if (m_lines.size() > 400'000) {
			m_lines.clear();
		}
		if (m_chains.size() > 100'000) {
			m_chains.clear();
		}
	}

	std::vector<std::string>                  m_patterns;
	uint32_t                                  m_hz;
	uint32_t                                  m_report_seconds;
	Image                                     m_image;
	std::vector<std::unique_ptr<Target>>      m_targets;
	std::unordered_map<uint64_t, std::string> m_names;
	std::unordered_map<uint64_t, std::string> m_lines;
	std::unordered_map<uint64_t, std::string> m_chains;
};

#endif

} // namespace

void Start() {
	static std::once_flag started;
	std::call_once(started, [] {
		auto patterns = Patterns();
		if (patterns.empty()) {
			return;
		}
		std::string list;
		for (const auto& pattern: patterns) {
			list += (list.empty() ? "" : ", ") + pattern;
		}
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		const auto hz      = EnvNumber("KYTY_SAMPLE_HZ", 1000, 10, 5000);
		const auto seconds = EnvNumber("KYTY_SAMPLE_REPORT", 10, 1, 3600);
		Log::WriteUnmuted(fmt::format("Sample: {} times a second: {}; a report every {} s "
		                              "(KYTY_SAMPLE, KYTY_SAMPLE_HZ, KYTY_SAMPLE_REPORT); stacks "
		                              "walked with the unwind data\n",
		                              hz, list, seconds));
		std::thread([patterns = std::move(patterns), hz, seconds]() mutable {
			PerfStats::NameThread("Sampler");
			Sampler sampler(std::move(patterns), hz, seconds);
			sampler.Run();
		}).detach();
#else
		Log::WriteUnmuted(fmt::format("Sample: KYTY_SAMPLE ({}) only samples on Windows\n", list));
#endif
	});
}

} // namespace Common::Sampler
