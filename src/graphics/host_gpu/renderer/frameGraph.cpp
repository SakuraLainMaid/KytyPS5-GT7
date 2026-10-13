#include "graphics/host_gpu/renderer/frameGraph.h"

#include "common/logging/log.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "kernel/memory.h"

#include <algorithm>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Libs::Graphics::FrameGraph {

std::atomic<bool> g_active {false};
std::atomic<bool> g_logging {false};

namespace {

// Everything but the request is the GPU thread's.
std::atomic<bool>            g_requested {false};
uint64_t                     g_frame_ends   = 0;
uint32_t                     g_capture      = 0;
uint32_t                     g_op           = 0;
bool                         g_show_buffers = false;
uint64_t                     g_lines        = 0;
bool                         g_truncated    = false;
std::map<uint64_t, uint64_t>                  g_buffers;   // guest address -> bytes kept
std::map<uint64_t, std::pair<uint64_t, bool>> g_readbacks; // small linear images: bytes, read back
std::set<uint64_t>           g_shaders;

constexpr uint64_t MaxLines       = 600000;
constexpr size_t   MaxBuffers     = 8192;
constexpr size_t   MaxReadbacks   = 64;
constexpr uint64_t MaxBufferLines = 120000;

// KYTY_FRAME_GRAPH=<frame>[,<frame>...]: the frame ends (counted from the start) after which a
// capture starts, besides F9.
const std::vector<uint64_t>& ScheduledFrames() {
	static const std::vector<uint64_t> frames = [] {
		std::vector<uint64_t> result;
		const char*           value = std::getenv("KYTY_FRAME_GRAPH");
		while (value != nullptr && *value != '\0') {
			char*      end   = nullptr;
			const auto frame = std::strtoull(value, &end, 10);
			if (end == value) {
				break;
			}
			result.push_back(frame);
			value = end != nullptr && *end == ',' ? end + 1 : nullptr;
		}
		return result;
	}();
	return frames;
}

// One region's words, 16 a line: "FG 0 buf 0x<address> +0x<offset>: <words>".
bool LogWords(uint64_t address, uint64_t bytes, std::vector<uint32_t>& words, uint64_t& lines) {
	words.assign((bytes + 3u) / 4u, 0);
	if (words.empty() || !LibKernel::Memory::TryReadBacking(address, words.data(), bytes)) {
		LOGF("FG 0 buf 0x%012" PRIx64 "+0x%" PRIx64 ": not readable\n", address, bytes);
		return false;
	}
	for (size_t row = 0; row < words.size(); row += 16) {
		char   line[16 * 9 + 1] {};
		size_t used = 0;
		for (size_t k = row; k < std::min(words.size(), row + 16); k++) {
			used += static_cast<size_t>(
			    std::snprintf(line + used, sizeof(line) - used, " %08x", words[k]));
		}
		LOGF("FG 0 buf 0x%012" PRIx64 " +0x%03zx:%s\n", address, row * 4u, line);
		lines++;
	}
	return true;
}

// The images read back for the CPU, as guest memory holds them once those read-backs landed.
void LogReadbacks(BufferCache& cache) {
	if (g_readbacks.empty()) {
		return;
	}
	cache.WaitForDiagnostics();
	LOGF("FG 0 readback images: %zu, the first %" PRIu64 " bytes of each\n", g_readbacks.size(),
	     ReadbackBytes);
	std::vector<uint32_t> words;
	uint64_t              lines = 0;
	for (const auto& [address, kept]: g_readbacks) {
		const auto& [bytes, read_back] = kept;
		LOGF("FG 0 readback 0x%012" PRIx64 "+0x%" PRIx64 " %s\n", address, bytes,
		     read_back ? "read back" : "not read back (the CPU sees what was there)");
		(void)LogWords(address, bytes, words, lines);
	}
}

void LogBuffers(BufferCache& cache) {
	std::vector<std::pair<uint64_t, uint64_t>> ranges(g_buffers.begin(), g_buffers.end());
	// The GPU's writes first (one wait for all of them).
	cache.DownloadRangesForDiagnostics(ranges);
	LOGF("FG 0 buffers: %zu, the first %" PRIu64 " bytes of each\n", ranges.size(), KeepBytes);
	std::vector<uint32_t> words;
	uint64_t              lines = 0;
	for (const auto& [address, bytes]: ranges) {
		if (lines >= MaxBufferLines) {
			LOGF("FG 0 (the buffer contents stop here: %" PRIu64 " lines)\n", MaxBufferLines);
			break;
		}
		(void)LogWords(address, bytes, words, lines);
	}
}

} // namespace

void RequestCapture() {
	if (!g_requested.exchange(true, std::memory_order_relaxed)) {
		LOGF("FG 0 capture requested: the next whole frame is logged\n");
	}
}

void OnFrameBoundary(BufferCache& cache) {
	g_frame_ends++;
	if (g_active.load(std::memory_order_relaxed)) {
		g_logging.store(false, std::memory_order_relaxed);
		g_active.store(false, std::memory_order_relaxed);
		LOGF("FG 0 end of frame: capture %u, %u operations%s\n", g_capture, g_op,
		     g_truncated ? " (lines truncated)" : "");
		LogBuffers(cache);
		LogReadbacks(cache);
		LOGF("FG 0 end capture %u (%zu shaders dumped)\n", g_capture, g_shaders.size());
		g_buffers.clear();
		g_readbacks.clear();
		// The next frame may be captured too.
	}
	const auto& frames    = ScheduledFrames();
	const bool  scheduled = std::find(frames.begin(), frames.end(), g_frame_ends) != frames.end();
	if (!g_requested.exchange(false, std::memory_order_relaxed) && !scheduled) {
		return;
	}
	g_capture++;
	g_op           = 0;
	g_lines        = 0;
	g_truncated    = false;
	g_show_buffers = false;
	g_buffers.clear();
	g_readbacks.clear();
	g_shaders.clear();
	LOGF("FG 0 begin capture %u after frame end %" PRIu64 "\n", g_capture, g_frame_ends);
	g_active.store(true, std::memory_order_relaxed);
	g_logging.store(true, std::memory_order_relaxed);
}

uint32_t BeginOp() {
	g_show_buffers = false;
	return ++g_op;
}

uint32_t CurrentOp() {
	return g_op;
}

void Log(const char* format, ...) {
	if (!Logging()) {
		return;
	}
	if (++g_lines > MaxLines) {
		g_truncated = true;
		g_logging.store(false, std::memory_order_relaxed);
		LOGF("FG %u (the capture stops logging here: %" PRIu64 " lines)\n", g_op, MaxLines);
		return;
	}
	char    text[1024];
	va_list args;
	va_start(args, format);
	std::vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	LOGF("FG %u %s\n", g_op, text);
}

void NoteBuffer(uint64_t address, uint64_t size) {
	if (!Logging() || !g_show_buffers || address == 0 || size == 0) {
		return;
	}
	const auto bytes = std::min(size, KeepBytes) & ~uint64_t {3};
	if (bytes == 0) {
		return;
	}
	if (const auto found = g_buffers.find(address); found != g_buffers.end()) {
		found->second = std::max(found->second, bytes);
		return;
	}
	if (g_buffers.size() < MaxBuffers) {
		g_buffers.emplace(address, bytes);
	}
}

void NoteReadback(uint64_t address, uint64_t size, bool read_back) {
	if (!Logging() || address == 0 || size == 0) {
		return;
	}
	const auto bytes = std::min(size, ReadbackBytes) & ~uint64_t {3};
	if (bytes != 0 && (g_readbacks.size() < MaxReadbacks || g_readbacks.contains(address))) {
		auto& kept  = g_readbacks[address];
		kept.first  = std::max(kept.first, bytes);
		kept.second = kept.second || read_back;
	}
}

void SetShowBuffers(bool show) {
	g_show_buffers = show;
}

bool ShowBuffers() {
	return g_show_buffers;
}

void NoteShader(const char* stage, uint64_t hash, uint64_t code_address, const uint32_t* user_data,
                uint32_t user_data_count) {
	if (!Logging() || hash == 0 || !g_shaders.insert(hash).second) {
		return;
	}
	const auto bytes = code_address != 0 ? ShaderCodeSizeBytes(code_address) : 0u;
	Log("shader %s %016" PRIx64 " at 0x%012" PRIx64 ", %u bytes", stage, hash, code_address, bytes);
	if (bytes == 0) {
		return;
	}
	std::vector<uint32_t> code(bytes / 4u);
	if (!LibKernel::Memory::TryReadBacking(code_address, code.data(), bytes)) {
		return;
	}
	std::error_code created;
	std::filesystem::create_directories("shader-dumps", created);
	char path[96];
	std::snprintf(path, sizeof(path), "shader-dumps/%s_%016" PRIx64 ".bin", stage, hash);
	if (FILE* file = std::fopen(path, "wb"); file != nullptr) {
		std::fwrite(code.data(), sizeof(uint32_t), code.size(), file);
		std::fclose(file);
	}
	std::snprintf(path, sizeof(path), "shader-dumps/%s_%016" PRIx64 ".txt", stage, hash);
	if (FILE* file = std::fopen(path, "w"); file != nullptr) {
		std::fprintf(file, "stage=%s hash=%016" PRIx64 " address=0x%012" PRIx64 " bytes=%u\n",
		             stage, hash, code_address, bytes);
		std::fprintf(file, "user_data=");
		for (uint32_t i = 0; i < user_data_count && user_data != nullptr; i++) {
			std::fprintf(file, "%08x ", user_data[i]);
		}
		std::fprintf(file, "\n");
		std::fclose(file);
	}
}

} // namespace Libs::Graphics::FrameGraph
