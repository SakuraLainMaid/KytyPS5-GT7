#include "common/hostSymbols.h"

#include "common/assert.h"
#include "common/logging/log.h"

#include <cstdint>
#include <fmt/format.h>
#include <mutex>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
// windows.h first
#include <dbghelp.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <utility>
#include <vector>
#endif

// Host symbols: kept apart from the fatal-error handlers in assert.cpp, so that code which only
// names host addresses (the sampler, PerfStats) does not pull those handlers into test
// programs that define their own.

namespace Common {

#ifdef _WIN32
// dbghelp is not thread-safe: two threads symbolising at once crashed inside SymFromAddr.
static std::mutex g_symbol_mutex;

// The handle dbghelp keys this process's symbols by: a duplicate of the process handle, so that
// another user of dbghelp in the process (a driver, an overlay) that initialized it for the
// pseudo handle does not collide with this one.
static HANDLE SymbolProcess() {
	static const HANDLE process = [] {
		HANDLE duplicate = nullptr;
		if (DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(),
		                    &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS) == FALSE) {
			return GetCurrentProcess();
		}
		return duplicate;
	}();
	return process;
}

// g_symbol_mutex held. Symbols come from the PDB next to the executable or one directory up (the
// install layout keeps the PDB in the build directory). What dbghelp found is logged once.
static bool InitializeSymbols(HANDLE process) {
	static const bool initialized = [process] {
		char exe_path[MAX_PATH] {};
		GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
		std::string search = exe_path;
		if (const auto slash = search.find_last_of("\\/"); slash != std::string::npos) {
			search.resize(slash);
			std::string parent = search;
			if (const auto up = parent.find_last_of("\\/"); up != std::string::npos) {
				parent.resize(up);
				search += ";" + parent;
			}
		}
		SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
		const bool  ok      = SymInitialize(process, search.c_str(), TRUE) != FALSE;
		const auto  error   = ok ? 0u : static_cast<unsigned>(GetLastError());
		const auto* version = ImagehlpApiVersion();
		std::string state;
		if (ok) {
			// Loads the executable's symbols (deferred until first use) to say what they are.
			alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256] {};
			auto*                     symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
			symbol->SizeOfStruct             = sizeof(SYMBOL_INFO);
			symbol->MaxNameLen               = 256;
			DWORD64           displacement   = 0;
			const auto        here           = reinterpret_cast<DWORD64>(&SymbolProcess);
			const bool        named = SymFromAddr(process, here, &displacement, symbol) != FALSE;
			IMAGEHLP_MODULE64 module {};
			module.SizeOfStruct = sizeof(module);
			if (SymGetModuleInfo64(process, here, &module) != FALSE) {
				state = fmt::format("the executable's symbols are of type {} ({}), PDB \"{}\"{}",
				                    static_cast<int>(module.SymType),
				                    module.SymType == SymPdb      ? "a PDB"
				                    : module.SymType == SymExport ? "exports only"
				                    : module.SymType == SymNone   ? "none"
				                                                  : "other",
				                    module.LoadedPdbName, named ? "" : "; a test lookup failed");
			} else {
				state = fmt::format("no module information for the executable (error {})",
				                    static_cast<unsigned>(GetLastError()));
			}
		} else {
			state = fmt::format("SymInitialize failed (error {}); names come from the map file and "
			                    "the system modules' export tables",
			                    error);
		}
		LOGF("Symbols: dbghelp %u.%u.%u: %s\n", version != nullptr ? version->MajorVersion : 0u,
		     version != nullptr ? version->MinorVersion : 0u,
		     version != nullptr ? version->Revision : 0u, state.c_str());
		return ok;
	}();
	return initialized;
}

// Names without dbghelp: the linker map next to the executable (kyty_emulator_*.map, written with
// /lldmap: "RVA size align " then the output section, the input section or, indented 16, a
// demangled symbol) for the emulator's own code, and the export tables of the system's modules.
namespace {

struct MapNames {
	bool                                       loaded = false;
	uint64_t                                   base   = 0;
	uint64_t                                   end    = 0;
	std::vector<std::pair<uint32_t, uint32_t>> entries; // RVA, offset of the name in text
	std::string                                text;
};

// "public: void __cdecl A::B(int) const" -> "A::B": the access, the return type, the calling
// convention and the parameters are left out, so that the samples of a function add up.
std::string ShortName(std::string name) {
	if (const auto convention = name.find("__cdecl "); convention != std::string::npos) {
		name.erase(0, convention + 8);
	}
	for (const char* prefix: {"public: ", "private: ", "protected: ", "static ", "virtual "}) {
		if (name.starts_with(prefix)) {
			name.erase(0, std::char_traits<char>::length(prefix));
		}
	}
	for (bool stripped = true; stripped;) {
		stripped = false;
		for (const char* suffix: {" const", " volatile", " &&", " &"}) {
			const auto length = std::char_traits<char>::length(suffix);
			if (name.size() > length && name.ends_with(suffix)) {
				name.resize(name.size() - length);
				stripped = true;
			}
		}
	}
	if (!name.empty() && name.back() == ')') {
		int depth = 0;
		for (size_t i = name.size(); i-- > 0;) {
			if (name[i] == ')') {
				depth++;
			} else if (name[i] == '(' && --depth == 0) {
				if (i > 0) {
					name.resize(i);
				}
				break;
			}
		}
	}
	return name;
}

uint64_t ImageSize(const uint8_t* base) {
	const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
		return 0;
	}
	const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
	return nt->Signature == IMAGE_NT_SIGNATURE ? nt->OptionalHeader.SizeOfImage : 0;
}

// g_symbol_mutex held.
MapNames& LoadedMap() {
	static MapNames names;
	if (names.loaded) {
		return names;
	}
	names.loaded       = true;
	const auto* module = reinterpret_cast<const uint8_t*>(GetModuleHandleA(nullptr));
	names.base         = reinterpret_cast<uint64_t>(module);
	names.end          = names.base + ImageSize(module);
	char exe_path[MAX_PATH] {};
	GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
	std::error_code       error;
	std::filesystem::path exe(exe_path);
	std::filesystem::path map;
	// The newest kyty_emulator*.map next to the executable, or one directory up.
	std::filesystem::file_time_type newest {};
	for (const auto& directory: {exe.parent_path(), exe.parent_path().parent_path()}) {
		for (const auto& entry: std::filesystem::directory_iterator(directory, error)) {
			const auto name = entry.path().filename().string();
			if (entry.path().extension() == ".map" && name.starts_with(exe.stem().string())) {
				const auto time = entry.last_write_time(error);
				if (map.empty() || time > newest) {
					map    = entry.path();
					newest = time;
				}
			}
		}
		if (!map.empty()) {
			break;
		}
	}
	if (map.empty()) {
		LOGF("Symbols: no %s*.map next to the executable\n", exe.stem().string().c_str());
		return names;
	}
	std::ifstream file(map);
	std::string   line;
	bool          in_text = false;
	while (std::getline(file, line)) {
		// "%08x %08x %5d " and then the rest.
		if (line.size() <= 24 || std::isxdigit(static_cast<unsigned char>(line[0])) == 0 ||
		    line[8] != ' ' || line[17] != ' ') {
			continue;
		}
		const auto rest   = std::string_view(line).substr(24);
		const auto indent = rest.find_first_not_of(' ');
		if (indent == std::string_view::npos) {
			continue;
		}
		if (indent == 0) {
			in_text = rest.starts_with(".text");
			continue;
		}
		if (!in_text || indent < 16) {
			continue;
		}
		const auto size  = std::strtoull(line.substr(9, 8).c_str(), nullptr, 16);
		const auto align = std::strtoull(line.substr(18, 5).c_str(), nullptr, 10);
		auto       name  = rest.substr(indent);
		if (size != 0 || align != 0 || name.empty() || name.front() == '.') {
			continue;
		}
		const auto rva =
		    static_cast<uint32_t>(std::strtoul(line.substr(0, 8).c_str(), nullptr, 16));
		names.entries.emplace_back(rva, static_cast<uint32_t>(names.text.size()));
		names.text += ShortName(std::string(name));
		names.text.push_back('\0');
	}
	std::ranges::stable_sort(names.entries, {}, &std::pair<uint32_t, uint32_t>::first);
	LOGF("Symbols: %zu functions from %s\n", names.entries.size(), map.string().c_str());
	return names;
}

// g_symbol_mutex held.
std::string MapSymbol(uint64_t address, uint64_t* start) {
	auto& names = LoadedMap();
	if (address < names.base || address >= names.end || names.entries.empty()) {
		return {};
	}
	const auto rva = static_cast<uint32_t>(address - names.base);
	auto       it =
	    std::ranges::upper_bound(names.entries, rva, {}, &std::pair<uint32_t, uint32_t>::first);
	if (it == names.entries.begin()) {
		return {};
	}
	--it;
	if (start != nullptr) {
		*start = names.base + it->first;
	}
	return std::string(names.text.c_str() + it->second);
}

// The export at or below `address` of the module that holds it ("ntdll!NtWaitForSingleObject"),
// or the module's name alone.
std::string ExportSymbol(uint64_t address, uint64_t* start) {
	HMODULE module = nullptr;
	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
	                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                       reinterpret_cast<LPCSTR>(address), &module) == FALSE ||
	    module == nullptr) {
		return {};
	}
	char path[MAX_PATH] {};
	GetModuleFileNameA(module, path, MAX_PATH);
	auto        module_name = std::filesystem::path(path).stem().string();
	const auto* base        = reinterpret_cast<const uint8_t*>(module);
	const auto  rva         = address - reinterpret_cast<uint64_t>(module);
	const auto* dos         = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
		return module_name;
	}
	const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE ||
	    nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT) {
		return module_name;
	}
	const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
	if (directory.VirtualAddress == 0 || directory.Size == 0) {
		return module_name;
	}
	const auto* exports =
	    reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + directory.VirtualAddress);
	const auto* functions = reinterpret_cast<const DWORD*>(base + exports->AddressOfFunctions);
	const auto* names     = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
	const auto* ordinals  = reinterpret_cast<const WORD*>(base + exports->AddressOfNameOrdinals);
	DWORD       best      = 0;
	const char* best_name = nullptr;
	for (DWORD i = 0; i < exports->NumberOfNames; i++) {
		const auto ordinal = ordinals[i];
		if (ordinal >= exports->NumberOfFunctions) {
			continue;
		}
		const auto function = functions[ordinal];
		// A forwarder names another module's export instead of code.
		if (function >= directory.VirtualAddress &&
		    function < directory.VirtualAddress + directory.Size) {
			continue;
		}
		if (function <= rva && function >= best) {
			best      = function;
			best_name = reinterpret_cast<const char*>(base + names[i]);
		}
	}
	if (best_name == nullptr) {
		return module_name;
	}
	if (start != nullptr) {
		*start = reinterpret_cast<uint64_t>(module) + best;
	}
	return module_name + "!" + best_name;
}

// g_symbol_mutex held: the map, then the export tables.
std::string FallbackSymbol(uint64_t address, uint64_t* start) {
	if (auto name = MapSymbol(address, start); !name.empty()) {
		return name;
	}
	return ExportSymbol(address, start);
}

} // namespace
#endif

std::string HostSymbol(uint64_t address, uint64_t* start) {
#ifdef _WIN32
	const auto       process = SymbolProcess();
	std::scoped_lock symbol_lock(g_symbol_mutex);
	if (InitializeSymbols(process)) {
		alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512] {};
		auto*                     symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
		symbol->SizeOfStruct             = sizeof(SYMBOL_INFO);
		symbol->MaxNameLen               = 512;
		DWORD64 displacement             = 0;
		if (SymFromAddr(process, address, &displacement, symbol) != FALSE) {
			if (start != nullptr) {
				*start = address - displacement;
			}
			return std::string(symbol->Name);
		}
	}
	return FallbackSymbol(address, start);
#else
	(void)address;
	(void)start;
	return {};
#endif
}

std::string HostLine(uint64_t address) {
#ifdef _WIN32
	const auto       process = SymbolProcess();
	std::scoped_lock symbol_lock(g_symbol_mutex);
	if (!InitializeSymbols(process)) {
		return {};
	}
	IMAGEHLP_LINE64 line {};
	line.SizeOfStruct  = sizeof(line);
	DWORD displacement = 0;
	if (SymGetLineFromAddr64(process, address, &displacement, &line) == FALSE ||
	    line.FileName == nullptr) {
		return {};
	}
	std::string_view file(line.FileName);
	if (const auto slash = file.find_last_of("\\/"); slash != std::string_view::npos) {
		file.remove_prefix(slash + 1);
	}
	return fmt::format("{}:{}", file, line.LineNumber);
#else
	(void)address;
	return {};
#endif
}

// Round 131: the functions inlined at `address`, innermost first, each with its line, then the
// function they were inlined into with its line: "Mutex::Lock (threads.cpp:267) < RendererLock
// (rendererLock.h:23) < RenderExecutor::DispatchDirectPart (renderCompute.cpp:1554)". The inline
// frames come from dbghelp's inline-trace calls (looked up by name, as the MinGW headers lack
// them); without them, the function and its line alone.
std::string HostInlineChain(uint64_t address) {
#ifdef _WIN32
	using AddrIncludeInlineTrace = DWORD(WINAPI*)(HANDLE, DWORD64);
	using QueryInlineTrace =
	    BOOL(WINAPI*)(HANDLE, DWORD64, DWORD, DWORD64, DWORD64, LPDWORD, LPDWORD);
	using FromInlineContext = BOOL(WINAPI*)(HANDLE, DWORD64, ULONG, PDWORD64, PSYMBOL_INFO);
	using GetLineFromInlineContext =
	    BOOL(WINAPI*)(HANDLE, DWORD64, ULONG, DWORD64, PDWORD, PIMAGEHLP_LINE64);
	struct Calls {
		AddrIncludeInlineTrace   include = nullptr;
		QueryInlineTrace         query   = nullptr;
		FromInlineContext        symbol  = nullptr;
		GetLineFromInlineContext line    = nullptr;
	};
	static const Calls calls = [] {
		Calls      found;
		const auto module = GetModuleHandleA("dbghelp.dll");
		if (module != nullptr) {
			found.include = reinterpret_cast<AddrIncludeInlineTrace>(
			    reinterpret_cast<void*>(GetProcAddress(module, "SymAddrIncludeInlineTrace")));
			found.query = reinterpret_cast<QueryInlineTrace>(
			    reinterpret_cast<void*>(GetProcAddress(module, "SymQueryInlineTrace")));
			found.symbol = reinterpret_cast<FromInlineContext>(
			    reinterpret_cast<void*>(GetProcAddress(module, "SymFromInlineContext")));
			found.line = reinterpret_cast<GetLineFromInlineContext>(
			    reinterpret_cast<void*>(GetProcAddress(module, "SymGetLineFromInlineContext")));
		}
		return found;
	}();
	const auto file_line = [](const IMAGEHLP_LINE64& line) {
		std::string_view file(line.FileName);
		if (const auto slash = file.find_last_of("\\/"); slash != std::string_view::npos) {
			file.remove_prefix(slash + 1);
		}
		return fmt::format("{}:{}", file, line.LineNumber);
	};
	std::string chain;
	{
		const auto       process = SymbolProcess();
		std::scoped_lock symbol_lock(g_symbol_mutex);
		if (!InitializeSymbols(process)) {
			return {};
		}
		const DWORD inlined = calls.include != nullptr && calls.query != nullptr &&
		                              calls.symbol != nullptr && calls.line != nullptr
		                          ? calls.include(process, address)
		                          : 0u;
		DWORD       context = 0;
		DWORD       index   = 0;
		if (inlined != 0 &&
		    calls.query(process, address, 0, address, address, &context, &index) != FALSE) {
			for (DWORD i = 0; i < inlined && i < 8; i++, context++) {
				alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256] {};
				auto*                     symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
				symbol->SizeOfStruct             = sizeof(SYMBOL_INFO);
				symbol->MaxNameLen               = 256;
				DWORD64 displacement             = 0;
				if (calls.symbol(process, address, context, &displacement, symbol) == FALSE) {
					break;
				}
				chain += chain.empty() ? "" : " < ";
				chain += symbol->Name;
				IMAGEHLP_LINE64 line {};
				line.SizeOfStruct = sizeof(line);
				DWORD line_offset = 0;
				if (calls.line(process, address, context, 0, &line_offset, &line) != FALSE &&
				    line.FileName != nullptr) {
					chain += " (" + file_line(line) + ")";
				}
			}
		}
	}
	// The function the inline frames are in (HostSymbol and HostLine take the lock themselves).
	auto outer = HostSymbol(address);
	if (outer.empty()) {
		return chain;
	}
	if (const auto line = HostLine(address); !line.empty()) {
		outer += " (" + line + ")";
	}
	return chain.empty() ? outer : chain + " < " + outer;
#else
	(void)address;
	return {};
#endif
}

// The host call stack of the failing thread, so a fatal report names its caller instead of
// only the file that noticed.
std::string HostBacktrace() {
#ifdef _WIN32
	static constexpr ULONG MaxFrames = 48;
	void*                  frames[MaxFrames];
	const auto             count = CaptureStackBackTrace(1, MaxFrames, frames, nullptr);
	if (count == 0) {
		return {};
	}

	const auto       process = SymbolProcess();
	std::scoped_lock symbol_lock(g_symbol_mutex);
	const bool       initialized = InitializeSymbols(process);

	std::string out = "--- Host stack ---\n";
	for (ULONG i = 0; i < count; i++) {
		const auto address = reinterpret_cast<DWORD64>(frames[i]);
		out += fmt::format("  #{:<2} 0x{:016x}", i, address);
		bool named = false;
		if (initialized) {
			alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512] {};
			auto*                     symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
			symbol->SizeOfStruct             = sizeof(SYMBOL_INFO);
			symbol->MaxNameLen               = 512;
			DWORD64 displacement             = 0;
			if (SymFromAddr(process, address, &displacement, symbol) != FALSE) {
				out += fmt::format(" {}+0x{:x}", symbol->Name, displacement);
				named = true;
			}
			IMAGEHLP_LINE64 line {};
			line.SizeOfStruct = sizeof(line);
			DWORD line_displacement = 0;
			if (SymGetLineFromAddr64(process, address, &line_displacement, &line) != FALSE) {
				out += fmt::format(" [{}:{}]", line.FileName, line.LineNumber);
			}
		}
		if (!named) {
			uint64_t start = address;
			if (const auto name = FallbackSymbol(address, &start); !name.empty()) {
				out += fmt::format(" {}+0x{:x}", name, address - start);
			}
		}
		out += "\n";
	}
	return out;
#else
	return {};
#endif
}

} // namespace Common
