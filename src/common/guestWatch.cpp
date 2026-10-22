#include "common/guestWatch.h"

#include "common/common.h"
#include "common/logging/log.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <fmt/format.h>
#include <mutex>
#include <string>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // IWYU pragma: keep
// clang-format off
#include <tlhelp32.h> // IWYU pragma: keep
// clang-format on
#endif

namespace Common::GuestWatch {

namespace {

std::atomic<uint64_t> g_base {0};
std::atomic<uint64_t> g_size {0};

[[nodiscard]] [[maybe_unused]] uint64_t Relative(uint64_t address) {
	const auto base = g_base.load(std::memory_order_relaxed);
	const auto size = g_size.load(std::memory_order_relaxed);
	return address >= base && address < base + size ? address - base : address;
}

} // namespace

void SetProgram(uint64_t base, uint64_t size) {
	g_base.store(base, std::memory_order_relaxed);
	g_size.store(size, std::memory_order_relaxed);
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS

namespace {

constexpr uint32_t MaxWatches   = 4;
constexpr uint32_t MaxLines     = 48;
constexpr uint32_t MaxCodeDumps = 8;

std::mutex                                      g_mutex; // arming
std::array<std::atomic<uint64_t>, MaxWatches>   g_watches {};
std::atomic<uint32_t>                           g_count {0};
std::atomic<uint64_t>                           g_hits {0};
std::atomic<uint32_t>                           g_lines {0};
std::array<std::atomic<uint64_t>, MaxCodeDumps> g_code_pcs {};

constexpr const char* RegisterNames[16] = {"rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp",
                                           "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};

[[nodiscard]] uint64_t Register(const CONTEXT& context, int index) {
	const uint64_t values[16] = {context.Rax, context.Rbx, context.Rcx, context.Rdx,
	                             context.Rsi, context.Rdi, context.Rbp, context.Rsp,
	                             context.R8,  context.R9,  context.R10, context.R11,
	                             context.R12, context.R13, context.R14, context.R15};
	return values[index];
}

[[nodiscard]] bool Readable(uint64_t address, uint64_t size) {
	if (address < 0x10000 || size == 0) {
		return false;
	}
	for (uint64_t page = address & ~uint64_t {0xfff}; page < address + size; page += 0x1000) {
		MEMORY_BASIC_INFORMATION info {};
		if (VirtualQuery(reinterpret_cast<const void*>(page), &info, sizeof(info)) == 0 ||
		    info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0 ||
		    (info.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
		                     PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY)) ==
		        0) {
			return false;
		}
	}
	return true;
}

// The first hit at this code address: whether its code bytes are still to be logged.
[[nodiscard]] bool ClaimCodeDump(uint64_t pc) {
	for (auto& slot: g_code_pcs) {
		uint64_t expected = 0;
		if (slot.load(std::memory_order_relaxed) == pc) {
			return false;
		}
		if (slot.compare_exchange_strong(expected, pc, std::memory_order_relaxed)) {
			return true;
		}
		if (expected == pc) {
			return false;
		}
	}
	return false;
}

LONG WINAPI Handler(PEXCEPTION_POINTERS pointers) {
	if (pointers->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) {
		return EXCEPTION_CONTINUE_SEARCH;
	}
	auto&      context = *pointers->ContextRecord;
	const auto count   = g_count.load(std::memory_order_acquire);
	uint32_t   which   = MaxWatches;
	for (uint32_t i = 0; i < count; i++) {
		if ((context.Dr6 & (uint64_t {1} << i)) != 0) {
			which = i;
			break;
		}
	}
	if (which == MaxWatches) {
		return EXCEPTION_CONTINUE_SEARCH;
	}
	context.Dr6 &= ~uint64_t {0xf};
	const auto hit = g_hits.fetch_add(1, std::memory_order_relaxed) + 1;
	if (g_lines.load(std::memory_order_relaxed) >= MaxLines) {
		if (hit % 1000 == 0) {
			Log::Write(fmt::format("GuestWatch: {} hits so far\n", hit));
		}
		return EXCEPTION_CONTINUE_EXECUTION;
	}
	g_lines.fetch_add(1, std::memory_order_relaxed);
	const auto  watch = g_watches[which].load(std::memory_order_relaxed);
	const auto  pc    = static_cast<uint64_t>(context.Rip);
	std::string line  = fmt::format(
        "GuestWatch: hit {} on watch {} (0x{:x}) = {:016x} by code at 0x{:x} (host 0x{:x}), thread "
	     "{}:",
        hit, which, watch, Readable(watch, 8) ? *reinterpret_cast<const uint64_t*>(watch) : 0,
        Relative(pc), pc, GetCurrentThreadId());
	for (int r = 0; r < 16; r++) {
		line += fmt::format(" {}={:x}", RegisterNames[r], Register(context, r));
	}
	// The main program's return addresses on the stack, the nearest first.
	const auto base  = g_base.load(std::memory_order_relaxed);
	const auto size  = g_size.load(std::memory_order_relaxed);
	uint32_t   found = 0;
	line += " | stack:";
	for (uint64_t at = context.Rsp; at < context.Rsp + 0x1000 && found < 16; at += 8) {
		if ((at & 0xfff) == 0 && !Readable(at, 8)) {
			break;
		}
		const auto value = *reinterpret_cast<const uint64_t*>(at);
		if (size != 0 && value >= base && value < base + size) {
			line += fmt::format(" +0x{:x}:0x{:x}", at - context.Rsp, value - base);
			found++;
		}
	}
	line += '\n';
	if (ClaimCodeDump(pc)) {
		const uint64_t from = pc - 0x180;
		if (Readable(from, 0x200)) {
			// One line a 32 bytes, each naming its code address, so a search for "GuestWatch" keeps
			// them all.
			const auto* bytes = reinterpret_cast<const uint8_t*>(from);
			for (uint32_t row = 0; row < 0x200; row += 32) {
				line += fmt::format("GuestWatch code 0x{:x} +0x{:03x}:", Relative(from), row);
				for (uint32_t i = row; i < row + 32; i++) {
					line += fmt::format(" {:02x}", bytes[i]);
				}
				line += '\n';
			}
		}
	}
	Log::Write(line);
	return EXCEPTION_CONTINUE_EXECUTION;
}

std::once_flag g_handler_once;

// DR7: Li enable, RWi = 01 (writes), LENi = 10 (8 bytes), for each watch.
[[nodiscard]] uint64_t DebugControl(uint32_t count) {
	uint64_t dr7 = 0;
	for (uint32_t i = 0; i < count; i++) {
		dr7 |= (uint64_t {1} << (2u * i)) | (uint64_t {0x9} << (16u + 4u * i));
	}
	return dr7;
}

} // namespace

uint32_t Arm(const uint64_t* addresses, uint32_t count, const char* reason) {
	std::scoped_lock lock(g_mutex);
	std::call_once(g_handler_once, [] { AddVectoredExceptionHandler(1, Handler); });
	auto watches = g_count.load(std::memory_order_relaxed);
	for (uint32_t i = 0; i < count && watches < MaxWatches; i++) {
		const auto address = addresses[i] & ~uint64_t {7};
		bool       known   = false;
		for (uint32_t w = 0; w < watches; w++) {
			known = known || g_watches[w].load(std::memory_order_relaxed) == address;
		}
		if (!known && address != 0) {
			g_watches[watches++].store(address, std::memory_order_relaxed);
		}
	}
	g_count.store(watches, std::memory_order_release);
	const auto  dr7   = DebugControl(watches);
	const DWORD self  = GetCurrentThreadId();
	uint32_t    armed = 0;
	HANDLE      snap  = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (snap != INVALID_HANDLE_VALUE) {
		THREADENTRY32 entry {};
		entry.dwSize = sizeof(entry);
		for (BOOL ok = Thread32First(snap, &entry); ok != FALSE; ok = Thread32Next(snap, &entry)) {
			if (entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == self) {
				continue;
			}
			HANDLE thread =
			    OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE,
			               entry.th32ThreadID);
			if (thread == nullptr) {
				continue;
			}
			if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
				CONTEXT context {};
				context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
				if (GetThreadContext(thread, &context) != FALSE) {
					context.Dr0 = g_watches[0].load(std::memory_order_relaxed);
					context.Dr1 = g_watches[1].load(std::memory_order_relaxed);
					context.Dr2 = g_watches[2].load(std::memory_order_relaxed);
					context.Dr3 = g_watches[3].load(std::memory_order_relaxed);
					context.Dr7 = (context.Dr7 & ~uint64_t {0xffff00ff}) | dr7;
					if (SetThreadContext(thread, &context) != FALSE) {
						armed++;
					}
				}
				ResumeThread(thread);
			}
			CloseHandle(thread);
		}
		CloseHandle(snap);
	}
	std::string list;
	for (uint32_t w = 0; w < watches; w++) {
		list += fmt::format("{}0x{:x}", w == 0 ? "" : ", ", g_watches[w].load());
	}
	Log::WriteToConsoleAndLog(fmt::format("GuestWatch: watching writes to {} on {} threads ({})\n",
	                                      list.empty() ? "nothing" : list, armed, reason));
	return armed;
}

#else

uint32_t Arm(const uint64_t* /*addresses*/, uint32_t /*count*/, const char* reason) {
	Log::WriteToConsoleAndLog(
	    fmt::format("GuestWatch: write watches need Windows; not armed ({})\n", reason));
	return 0;
}

#endif

} // namespace Common::GuestWatch
