#include "common/lastWords.h"

#include "common/common.h"

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS

#include "common/hostSymbols.h"
#include "common/logging/log.h"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fmt/format.h>
#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // IWYU pragma: keep

namespace Common::LastWords {

namespace {

constexpr uint32_t MaxFrames         = 48;
constexpr DWORD    ReportWaitMs      = 5000;
constexpr uint32_t CppExceptionCode  = 0xE06D7363u;
constexpr uint32_t StackOverflowCode = 0xC00000FDu;

// What the failing thread hands the reporter: only copies, no allocation or formatting (its
// stack may be all but used up).
struct Report {
	const char* what            = "";
	bool        final           = false;
	uint32_t    code            = 0;
	uint64_t    address         = 0;
	DWORD       thread_id       = 0;
	uint64_t    parameters[4]   = {};
	uint32_t    parameter_count = 0;
	bool        has_context     = false;
};

Report g_report;
// The failing thread's registers (the fault's, or where it reported), walked by the reporter.
alignas(16) CONTEXT g_context;
HANDLE                       g_request     = nullptr;
HANDLE                       g_done        = nullptr;
DWORD                        g_reporter_id = 0;
std::atomic<bool>            g_installed {false};
std::atomic<bool>            g_busy {false};
std::atomic<void (*)()>      g_final_action {nullptr};
std::atomic<bool>            g_final_made {false};
std::atomic<uint32_t>        g_first_chance_reports {0};
LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter = nullptr;

bool Readable(uint64_t address, size_t size) {
	if (address == 0 || size == 0) {
		return false;
	}
	uint64_t at  = address;
	uint64_t end = address + size;
	while (at < end) {
		MEMORY_BASIC_INFORMATION info {};
		if (VirtualQuery(reinterpret_cast<const void*>(at), &info, sizeof(info)) == 0 ||
		    info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0 ||
		    (info.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
		                     PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0) {
			return false;
		}
		at = reinterpret_cast<uint64_t>(info.BaseAddress) + info.RegionSize;
	}
	return true;
}

std::string ReadString(uint64_t address, size_t max) {
	std::string out;
	for (size_t i = 0; i < max; i++) {
		if ((i == 0 || ((address + i) & 0xfffu) == 0) && !Readable(address + i, 1)) {
			break;
		}
		const char c = *reinterpret_cast<const char*>(address + i);
		if (c == '\0') {
			break;
		}
		out += c;
	}
	return out;
}

const char* CodeName(uint32_t code) {
	switch (code) {
		case 0xC0000005u: return "an access violation";
		case 0xC0000006u:
			return "an in-page error: memory backed by a file or the page file could not be read";
		case 0xC0000017u: return "no memory";
		case 0xC000001Du: return "an illegal instruction";
		case 0xC000008Cu: return "an array bounds error";
		case 0xC0000094u: return "an integer division by zero";
		case 0xC0000095u: return "an integer overflow";
		case 0xC0000096u: return "a privileged instruction";
		case StackOverflowCode: return "a stack overflow";
		case 0xC0000374u: return "heap corruption";
		case 0xC0000409u: return "a stack buffer overrun or a fail-fast";
		case 0xC0000420u: return "an assertion failure";
		case 0x80000003u: return "a breakpoint";
		case CppExceptionCode: return "a C++ exception";
		default: return "an exception";
	}
}

// "module.dll+0x1234 Symbol+0x10 [file.cpp:12]" for an address of the host's code.
std::string Where(uint64_t address) {
	std::string out    = fmt::format("0x{:016x}", address);
	HMODULE     module = nullptr;
	if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
	                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                       reinterpret_cast<LPCWSTR>(address), &module) != FALSE &&
	    module != nullptr) {
		char path[MAX_PATH] {};
		if (GetModuleFileNameA(module, path, MAX_PATH) != 0) {
			const char* name = std::strrchr(path, '\\');
			out += fmt::format(" {}+0x{:x}", name != nullptr ? name + 1 : path,
			                   address - reinterpret_cast<uint64_t>(module));
		}
	} else {
		out += " (in no module: the game's code, or code made at run time)";
	}
	uint64_t start = address;
	if (const auto symbol = HostSymbol(address, &start); !symbol.empty()) {
		out += fmt::format(" {}+0x{:x}", symbol, address - start);
	}
	if (const auto line = HostLine(address); !line.empty()) {
		out += fmt::format(" [{}]", line);
	}
	return out;
}

std::string ThreadName(DWORD thread_id) {
	using GetDescription              = HRESULT(WINAPI*)(HANDLE, PWSTR*);
	static const auto get_description = reinterpret_cast<GetDescription>(reinterpret_cast<void*>(
	    GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription")));
	if (get_description == nullptr) {
		return {};
	}
	HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, thread_id);
	if (thread == nullptr) {
		return {};
	}
	std::string name;
	PWSTR       wide = nullptr;
	if (SUCCEEDED(get_description(thread, &wide)) && wide != nullptr) {
		char utf8[128] {};
		if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, static_cast<int>(sizeof(utf8)), nullptr,
		                        nullptr) > 0) {
			name = utf8;
		}
		LocalFree(wide);
	}
	CloseHandle(thread);
	return name;
}

bool InModule(uint64_t address) {
	HMODULE module = nullptr;
	return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
	                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                          reinterpret_cast<LPCWSTR>(address), &module) != FALSE &&
	       module != nullptr;
}

// The failing thread's return addresses, from its registers. Not CaptureStackBackTrace: that
// stops at the thread's stack bounds, which are cleared while guest code runs. The host's code
// unwinds through its unwind tables (a function without one is a leaf: its return address at
// rsp), the game's through its rbp frames. Each read is checked first.
int WalkStack(CONTEXT context, uint64_t* frames, int max) {
	int count = 0;
	while (count < max) {
		const uint64_t rip = context.Rip;
		if (rip == 0) {
			break;
		}
		frames[count++]    = rip;
		DWORD64 image_base = 0;
		if (auto* entry = RtlLookupFunctionEntry(rip, &image_base, nullptr); entry != nullptr) {
			if (!Readable(context.Rsp, 512)) {
				break;
			}
			PVOID   handler_data = nullptr;
			DWORD64 establisher  = 0;
			RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, rip, entry, &context, &handler_data,
			                 &establisher, nullptr);
		} else if (InModule(rip)) {
			if (!Readable(context.Rsp, 8)) {
				break;
			}
			context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
			context.Rsp += 8;
		} else {
			if ((context.Rbp & 7u) != 0 || !Readable(context.Rbp, 16)) {
				break;
			}
			const auto* frame = reinterpret_cast<const DWORD64*>(context.Rbp);
			context.Rsp       = context.Rbp + 16;
			context.Rip       = frame[1];
			context.Rbp       = frame[0];
		}
	}
	return count;
}

// ".?AVbad_alloc@std@@" as "std::bad_alloc"; other forms (templates) as they are.
std::string TypeName(const std::string& decorated) {
	if (decorated.size() < 6 ||
	    (decorated.compare(0, 4, ".?AV") != 0 && decorated.compare(0, 4, ".?AU") != 0) ||
	    decorated.find("?$") != std::string::npos ||
	    decorated.compare(decorated.size() - 2, 2, "@@") != 0) {
		return decorated;
	}
	std::string body = decorated.substr(4, decorated.size() - 6);
	std::string out;
	size_t      end = body.size();
	while (true) {
		const auto at   = body.rfind('@', end == 0 ? 0 : end - 1);
		const auto from = at == std::string::npos ? 0 : at + 1;
		out += body.substr(from, end - from);
		if (at == std::string::npos || at == 0) {
			break;
		}
		out += "::";
		end = at;
	}
	return out;
}

// The thrown type of an MSVC C++ exception (and what() of a std::exception), from its
// exception record: the object, the ThrowInfo and the image base its offsets count from.
std::string CppException(const Report& report) {
	if (report.parameter_count < 4) {
		return {};
	}
	const uint64_t object = report.parameters[1];
	const uint64_t info   = report.parameters[2];
	const uint64_t base   = report.parameters[3];
	if (!Readable(info, 16)) {
		return "  (the thrown type is not known)\n";
	}
	const uint64_t types =
	    base + static_cast<uint32_t>(*reinterpret_cast<const int32_t*>(info + 12));
	if (!Readable(types, 4)) {
		return "  (the thrown type is not known)\n";
	}
	const int32_t count = *reinterpret_cast<const int32_t*>(types);
	std::string   names;
	std::string   what;
	for (int32_t i = 0; i < std::min<int32_t>(count, 8); i++) {
		const uint64_t entry = types + 4u + 4u * static_cast<uint32_t>(i);
		if (!Readable(entry, 4)) {
			break;
		}
		const uint64_t type =
		    base + static_cast<uint32_t>(*reinterpret_cast<const int32_t*>(entry));
		if (!Readable(type, 28)) {
			break;
		}
		const uint64_t descriptor =
		    base + static_cast<uint32_t>(*reinterpret_cast<const int32_t*>(type + 4));
		const int32_t offset    = *reinterpret_cast<const int32_t*>(type + 8);
		const auto    decorated = ReadString(descriptor + 16, 160);
		if (decorated.empty()) {
			break;
		}
		names += (names.empty() ? "" : ", ") + TypeName(decorated);
		// std::exception: its virtual table, then the message.
		if (decorated == ".?AVexception@std@@" && object != 0) {
			const uint64_t message = object + static_cast<int64_t>(offset) + 8u;
			if (Readable(message, 8)) {
				what = ReadString(*reinterpret_cast<const uint64_t*>(message), 240);
			}
		}
	}
	std::string out = names.empty() ? std::string() : fmt::format("  thrown: {}\n", names);
	if (!what.empty()) {
		out += fmt::format("  what(): {}\n", what);
	}
	return out;
}

DWORD WINAPI ReporterMain(LPVOID /*parameter*/) {
	for (;;) {
		if (WaitForSingleObject(g_request, INFINITE) != WAIT_OBJECT_0) {
			return 0;
		}
		const auto& report = g_report;
		const auto  name   = ThreadName(report.thread_id);
		std::string text   = fmt::format("\nLast words (round 125): {} on thread {}{}", report.what,
		                                 report.thread_id, name.empty() ? "" : " (" + name + ")");
		if (report.code != 0) {
			text += fmt::format(", exception 0x{:08x}, {}", report.code, CodeName(report.code));
		}
		if (report.address != 0) {
			text += fmt::format(", at {}", Where(report.address));
		}
		text += "\n";
		if (report.code == 0xC0000005u && report.parameter_count >= 2) {
			text += fmt::format("  {} of 0x{:016x}\n",
			                    report.parameters[0] == 0   ? "a read"
			                    : report.parameters[0] == 1 ? "a write"
			                    : report.parameters[0] == 8 ? "an instruction fetch"
			                                                : "an access",
			                    report.parameters[1]);
		}
		if (report.code == CppExceptionCode) {
			text += CppException(report);
		}
		if (report.has_context) {
			uint64_t  frames[MaxFrames] {};
			const int count = WalkStack(g_context, frames, static_cast<int>(MaxFrames));
			text += "--- Stack of that thread ---\n";
			for (int i = 0; i < count; i++) {
				text += fmt::format("  #{:<2} {}\n", i, Where(frames[i]));
			}
		}
		text += report.final ? "The process ends now (Windows ends it after this report).\n"
		                     : "First chance: if nothing handles it, the process ends now.\n";
		Log::WriteFatal(text);
		SetEvent(g_done);
	}
}

// The report, written by the reporter thread; the failing thread waits for it at most 5 s. One
// final report (the end of the process): the later ends (abort() after std::terminate) only wait.
void Speak(const char* what, bool final, uint32_t code, uint64_t address,
           const uint64_t* parameters, uint32_t parameter_count, const CONTEXT* context) {
	if (!g_installed.load(std::memory_order_acquire) || GetCurrentThreadId() == g_reporter_id ||
	    g_final_made.load(std::memory_order_acquire)) {
		return;
	}
	bool expected = false;
	if (!g_busy.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
		// Another report is being written: wait for it, then go on.
		for (int i = 0; i < 50 && g_busy.load(std::memory_order_acquire); i++) {
			Sleep(100);
		}
		return;
	}
	if (final) {
		g_final_made.store(true, std::memory_order_release);
		// Before the report: whatever must not outlive the process in the outside world (a force
		// on a racing wheel).
		if (auto* action = g_final_action.exchange(nullptr, std::memory_order_acq_rel)) {
			action();
		}
	}
	auto& report           = g_report;
	report.what            = what;
	report.final           = final;
	report.code            = code;
	report.address         = address;
	report.thread_id       = GetCurrentThreadId();
	report.parameter_count = std::min<uint32_t>(parameter_count, 4);
	for (uint32_t i = 0; i < report.parameter_count; i++) {
		report.parameters[i] = parameters[i];
	}
	// The fault's registers; else this thread's here (its callers are what failed).
	if (context != nullptr) {
		std::memcpy(&g_context, context, sizeof(CONTEXT));
	} else {
		RtlCaptureContext(&g_context);
	}
	report.has_context = true;
	ResetEvent(g_done);
	SetEvent(g_request);
	WaitForSingleObject(g_done, ReportWaitMs);
	g_busy.store(false, std::memory_order_release);
}

LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* pointers) {
	if (pointers != nullptr && pointers->ExceptionRecord != nullptr) {
		const auto* record = pointers->ExceptionRecord;
		uint64_t    parameters[4] {};
		const auto  count = std::min<DWORD>(record->NumberParameters, 4);
		for (DWORD i = 0; i < count; i++) {
			parameters[i] = record->ExceptionInformation[i];
		}
		Speak("an exception nothing handled", true, record->ExceptionCode,
		      reinterpret_cast<uint64_t>(record->ExceptionAddress), parameters, count,
		      pointers->ContextRecord);
	}
	return g_previous_filter != nullptr ? g_previous_filter(pointers) : EXCEPTION_CONTINUE_SEARCH;
}

[[noreturn]] void OnTerminate() {
	Speak("std::terminate (an exception nothing caught, or one leaving a function that may not "
	      "throw)",
	      true, 0, 0, nullptr, 0, nullptr);
	std::abort();
}

void OnAbort(int /*signal*/) {
	// abort() goes on to end the process when this returns.
	Speak("abort()", true, 0, 0, nullptr, 0, nullptr);
}

void OnInvalidParameter(const wchar_t* /*expression*/, const wchar_t* /*function*/,
                        const wchar_t* /*file*/, unsigned int /*line*/, uintptr_t /*reserved*/) {
	Speak("an invalid argument to a C runtime function", true, 0, 0, nullptr, 0, nullptr);
	// As the C runtime's own handler: the process ends.
	RaiseFailFastException(nullptr, nullptr, 0);
}

void OnPureCall() {
	Speak("a pure virtual function call", true, 0, 0, nullptr, 0, nullptr);
	RaiseFailFastException(nullptr, nullptr, 0);
}

} // namespace

void SetFinalAction(void (*action)()) {
	g_final_action.store(action, std::memory_order_release);
}

void Install() {
	if (g_installed.load(std::memory_order_acquire)) {
		return;
	}
	g_request = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	g_done    = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (g_request == nullptr || g_done == nullptr) {
		return;
	}
	HANDLE thread = CreateThread(nullptr, 1u << 20u, ReporterMain, nullptr, 0, &g_reporter_id);
	if (thread == nullptr) {
		return;
	}
	CloseHandle(thread);
	g_previous_filter = SetUnhandledExceptionFilter(UnhandledFilter);
	std::set_terminate(OnTerminate);
	std::signal(SIGABRT, OnAbort);
	_set_invalid_parameter_handler(OnInvalidParameter);
	_set_purecall_handler(OnPureCall);
	g_installed.store(true, std::memory_order_release);
}

void ReportFirstChance(uint32_t code, uint64_t address, const uint64_t* parameters,
                       uint32_t parameter_count, const void* context) {
	if (!g_installed.load(std::memory_order_acquire)) {
		return;
	}
	const auto* registers = static_cast<const CONTEXT*>(context);
	// While guest code runs, the thread's stack bounds are cleared (RunOnGuestStack): Windows
	// finds no handler for an exception there, and ends the process without the unhandled-
	// exception filter.
	const auto* tib         = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
	const bool  guest_stack = tib != nullptr && tib->StackBase == nullptr;
	// Exceptions the emulator never raises on purpose, on its own stacks: reported at once (a
	// stack overflow may leave no stack for the unhandled-exception filter).
	const bool fatal = code == StackOverflowCode || code == 0xC0000094u || code == 0xC0000095u ||
	                   code == 0xC0000096u || code == 0xC000008Cu || code == 0xC0000006u;
	// The first 4 (a library that handles its own may raise more).
	if ((guest_stack || fatal) &&
	    g_first_chance_reports.fetch_add(1, std::memory_order_relaxed) < 4) {
		Speak(guest_stack ? "an exception on a guest stack (Windows finds no handler there)"
		                  : "an exception that ends the process unless something handles it",
		      false, code, address, parameters, parameter_count, registers);
	}
}

} // namespace Common::LastWords

#else

namespace Common::LastWords {

void SetFinalAction(void (* /*action*/)()) {}

void Install() {}

void ReportFirstChance(uint32_t /*code*/, uint64_t /*address*/, const uint64_t* /*parameters*/,
                       uint32_t /*parameter_count*/, const void* /*context*/) {}

} // namespace Common::LastWords

#endif
