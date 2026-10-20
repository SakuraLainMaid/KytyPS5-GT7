#include "graphics/host_gpu/renderer/frameGraph.h"

#include "common/logging/log.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/frameGraphQueries.h"
#include "kernel/memory.h"

#include <algorithm>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fmt/format.h>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <xxhash.h>

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
std::set<uint64_t>           g_grid_checks;
// Round 91: the guest ranges hashed in this capture (BytesHash), with the hash and the operation
// that first read them: re-hashed after the frame, and a range that changes between two of the
// frame's reads is marked where it is read again.
struct HashedRange {
	uint64_t hash  = 0;
	uint32_t first = 0; // the first operation that read these bytes
	uint32_t last  = 0; // the last one that did
};
std::map<std::pair<uint64_t, uint64_t>, HashedRange> g_hashed;
constexpr size_t                                     MaxHashed = 65536;
// The addresses LogBytes logged in this capture: once each, and again when their bytes changed.
std::set<uint64_t> g_bytes_logged;
// Round 91: captures left to begin right after this one (KYTY_FG_FRAMES).
uint32_t g_frames_left = 0;
// Verification: the ranges claimed in this capture and the bytes they download; the counts of
// the comparisons that landed (written as they land, from the scheduler's completion work).
std::set<std::pair<uint64_t, uint64_t>> g_verify_claimed;
uint64_t                                g_verify_claimed_bytes = 0;
uint64_t                                g_verify_texture_bytes = 0;
uint64_t                                g_verify_layer_bytes   = 0;
uint64_t                                g_verify_over_budget   = 0;
std::atomic<uint64_t>                   g_verify_images {0};
std::atomic<uint64_t>                   g_verify_image_bytes {0};
std::atomic<uint64_t>                   g_verify_images_differ {0};
std::atomic<uint64_t>                   g_verify_buffers {0};
std::atomic<uint64_t>                   g_verify_buffer_bytes {0};
std::atomic<uint64_t>                   g_verify_buffers_differ {0};

uint64_t VerifyBudgetBytes() {
	static const uint64_t bytes = [] {
		const char* value = std::getenv("KYTY_FG_VERIFY_MB");
		return (value != nullptr ? std::strtoull(value, nullptr, 10) : 3072ull) << 20u;
	}();
	return bytes;
}

uint64_t VerifyTexturesBudgetBytes() {
	static const uint64_t bytes = [] {
		const char* value = std::getenv("KYTY_FG_VERIFY_TEXTURES_MB");
		return (value != nullptr ? std::strtoull(value, nullptr, 10) : 1536ull) << 20u;
	}();
	return bytes;
}

uint64_t VerifyLayersBudgetBytes() {
	static const uint64_t bytes = [] {
		const char* value = std::getenv("KYTY_FG_VERIFY_LAYERS_MB");
		return (value != nullptr ? std::strtoull(value, nullptr, 10) : 2048ull) << 20u;
	}();
	return bytes;
}

void ResetVerify() {
	g_verify_claimed.clear();
	g_verify_claimed_bytes = 0;
	g_verify_texture_bytes = 0;
	g_verify_layer_bytes   = 0;
	g_verify_over_budget   = 0;
	g_verify_images.store(0, std::memory_order_relaxed);
	g_verify_image_bytes.store(0, std::memory_order_relaxed);
	g_verify_images_differ.store(0, std::memory_order_relaxed);
	g_verify_buffers.store(0, std::memory_order_relaxed);
	g_verify_buffer_bytes.store(0, std::memory_order_relaxed);
	g_verify_buffers_differ.store(0, std::memory_order_relaxed);
}

// After the frame's comparisons landed.
void LogVerify(BufferCache& cache) {
	if (!VerifyEnabled() || g_verify_claimed.empty()) {
		return;
	}
	cache.WaitForDiagnostics();
	LOGF("FG 0 verify: %" PRIu64 " textures (%" PRIu64
	     " MB) compared with their guest bytes, %" PRIu64 " stale; %" PRIu64 " buffers (%" PRIu64
	     " MB), %" PRIu64 " stale; %" PRIu64 " ranges past the budgets (%" PRIu64
	     " MB of buffers, KYTY_FG_VERIFY_MB; KYTY_FG_VERIFY_TEXTURES_MB, KYTY_FG_VERIFY_LAYERS_MB) "
	     "not compared\n",
	     g_verify_images.load(std::memory_order_relaxed),
	     g_verify_image_bytes.load(std::memory_order_relaxed) >> 20u,
	     g_verify_images_differ.load(std::memory_order_relaxed),
	     g_verify_buffers.load(std::memory_order_relaxed),
	     g_verify_buffer_bytes.load(std::memory_order_relaxed) >> 20u,
	     g_verify_buffers_differ.load(std::memory_order_relaxed), g_verify_over_budget,
	     VerifyBudgetBytes() >> 20u);
}

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
	for (size_t row = 0; row < words.size() && lines < MaxBufferLines; row += 16) {
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

// Round 91: KYTY_FG_FRAMES=<n> (1 to 8, default 1): a capture logs n frames in a row, each its own
// capture: a frame's data against the next one's.
uint32_t CaptureFrames() {
	static const uint32_t frames = [] {
		const char* value = std::getenv("KYTY_FG_FRAMES");
		const auto  n     = value != nullptr ? std::strtoul(value, nullptr, 10) : 1ul;
		return static_cast<uint32_t>(std::clamp<unsigned long>(n, 1ul, 8ul));
	}();
	return frames;
}

// Round 91: the ranges the frame's draws hashed, hashed again now: the ones that changed after
// they were read (the CPU wrote them later in the frame, or after it).
void LogRehash(BufferCache& cache) {
	if (g_hashed.empty()) {
		return;
	}
	thread_local std::unique_ptr<uint8_t[]> scratch;
	if (scratch == nullptr) {
		scratch.reset(new uint8_t[HashBytes]);
	}
	// At most 1 GB read again (the GPU thread does it while the game waits for the frame).
	constexpr uint64_t Budget  = 1ull << 30u;
	uint64_t           read    = 0;
	uint64_t           changed = 0;
	uint64_t           gpu     = 0;
	uint64_t           gone    = 0;
	uint64_t           skipped = 0;
	uint32_t           lines   = 0;
	for (const auto& [range, hashed]: g_hashed) {
		const auto& [address, size] = range;
		if (read + size > Budget) {
			skipped++;
			continue;
		}
		read += size;
		if (cache.HasGpuDirtyBytes(address, size)) {
			gpu++;
			continue;
		}
		if (!LibKernel::Memory::TryReadBacking(address, scratch.get(), size)) {
			gone++;
			continue;
		}
		if (XXH3_64bits(scratch.get(), size) == hashed.hash) {
			continue;
		}
		changed++;
		if (lines++ < 4000) {
			LOGF("FG 0 rehash 0x%012" PRIx64 "+0x%" PRIx64 " read at ops %u to %u: changed since\n",
			     address, size, hashed.first, hashed.last);
		}
	}
	LOGF("FG 0 rehash: %zu ranges the frame read, %" PRIu64 " changed since (%" PRIu64
	     " GPU-written now, %" PRIu64 " unreadable, %" PRIu64 " past the 1 GB budget)\n",
	     g_hashed.size(), changed, gpu, gone, skipped);
}

// Round 105: the GPU's copies of the chosen dispatches' buffers as each dispatch found them
// (BufferCache::SnapshotForDiagnostics): "FG 0 snapshot ..." then "FG 0 snap 0x<address> +0x<off>:
// <words>" lines, 16 words a line.
void LogSnapshots(BufferCache& cache) {
	const auto snapshots = cache.TakeDiagnosticSnapshots();
	if (snapshots.empty()) {
		return;
	}
	LOGF("FG 0 snapshots: %zu buffers as the dispatches of the KYTY_FG_DUMP_SHADERS shaders found "
	     "them\n",
	     snapshots.size());
	uint64_t lines = 0;
	for (const auto& snapshot: snapshots) {
		LOGF("FG 0 snapshot op %u %s 0x%012" PRIx64 "+0x%zx: %s\n", snapshot.op,
		     snapshot.label.c_str(), snapshot.address, snapshot.words.size() * 4u,
		     snapshot.words.empty() ? "not readable"
		     : snapshot.from_gpu    ? "the GPU's copy before the dispatch ran"
		                            : "guest memory (no cached buffer holds it all)");
		for (size_t row = 0; row < snapshot.words.size() && lines < MaxBufferLines; row += 16) {
			char   line[16 * 9 + 1] {};
			size_t used = 0;
			for (size_t k = row; k < std::min(snapshot.words.size(), row + 16); k++) {
				used += static_cast<size_t>(
				    std::snprintf(line + used, sizeof(line) - used, " %08x", snapshot.words[k]));
			}
			LOGF("FG 0 snap 0x%012" PRIx64 " +0x%03zx:%s\n", snapshot.address, row * 4u, line);
			lines++;
		}
	}
}

void LogBuffers(BufferCache& cache) {
	std::vector<std::pair<uint64_t, uint64_t>> ranges(g_buffers.begin(), g_buffers.end());
	// The GPU's writes first (one wait for all of them).
	cache.DownloadRangesForDiagnostics(ranges);
	LOGF("FG 0 buffers: %zu, the first %" PRIu64
	     " bytes of each (more of those KYTY_FG_DUMP_SHADERS "
	     "shaders bind)\n",
	     ranges.size(), KeepBytes);
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

uint64_t GpuThreadFrames() {
	return g_frame_ends;
}

void OnFrameBoundary(BufferCache& cache) {
	g_frame_ends++;
	bool continued = false;
	if (g_active.load(std::memory_order_relaxed)) {
		g_logging.store(false, std::memory_order_relaxed);
		g_active.store(false, std::memory_order_relaxed);
		LOGF("FG 0 end of frame: capture %u, %u operations%s\n", g_capture, g_op,
		     g_truncated ? " (lines truncated)" : "");
		// Before the buffers' downloads, which clear the marks of what the GPU wrote.
		LogRehash(cache);
		LogSnapshots(cache);
		LogBuffers(cache);
		LogReadbacks(cache);
		LogVerify(cache);
		if (FrameGraphQueries::Pending()) {
			cache.WaitForDiagnostics();
			FrameGraphQueries::LogResults();
		}
		LOGF("FG 0 end capture %u (%zu shaders dumped)\n", g_capture, g_shaders.size());
		g_buffers.clear();
		g_readbacks.clear();
		g_hashed.clear();
		g_bytes_logged.clear();
		// The next frame may be captured too (KYTY_FG_FRAMES: at once).
		if (g_frames_left != 0) {
			g_frames_left--;
			continued = true;
		}
	}
	const auto& frames    = ScheduledFrames();
	const bool  scheduled = std::find(frames.begin(), frames.end(), g_frame_ends) != frames.end();
	const bool  requested = g_requested.exchange(false, std::memory_order_relaxed);
	if (!continued && !requested && !scheduled) {
		return;
	}
	if (!continued) {
		// A new capture (F9, or a scheduled one): it and the frames after it.
		g_frames_left = CaptureFrames() - 1u;
	}
	g_capture++;
	g_op           = 0;
	g_lines        = 0;
	g_truncated    = false;
	g_show_buffers = false;
	g_buffers.clear();
	g_readbacks.clear();
	g_shaders.clear();
	g_grid_checks.clear();
	g_hashed.clear();
	g_bytes_logged.clear();
	ResetVerify();
	LOGF("FG 0 begin capture %u after frame end %" PRIu64 "\n", g_capture, g_frame_ends);
	if (VerifyEnabled()) {
		LOGF("FG 0 verify on: textures and buffers the GPU did not write are compared with their "
		     "guest bytes, up to %" PRIu64 " MB (KYTY_FG_VERIFY=0 turns it off)\n",
		     VerifyBudgetBytes() >> 20u);
	}
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

uint32_t CaptureNumber() {
	return g_capture;
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

std::string BytesHash(BufferCache& cache, uint64_t address, uint64_t size) {
	size = std::min(size, HashBytes);
	if (address == 0 || size == 0) {
		return "-";
	}
	size = LibKernel::Memory::TryClampRangeSize(address, size);
	// One buffer for the capture's thousands of hashes, not filled first.
	static thread_local std::unique_ptr<uint8_t[]> scratch;
	if (scratch == nullptr) {
		scratch.reset(new uint8_t[HashBytes]);
	}
	if (size == 0 || !LibKernel::Memory::TryReadBacking(address, scratch.get(), size)) {
		return "-";
	}
	const auto  hash = XXH3_64bits(scratch.get(), size);
	const bool  gpu  = cache.HasGpuDirtyBytes(address, size);
	std::string changed;
	if (Logging() && !gpu) {
		const auto key = std::make_pair(address, size);
		if (const auto found = g_hashed.find(key); found != g_hashed.end()) {
			if (found->second.hash != hash) {
				// Changed after op `last` read it (by the CPU, or a download of the GPU's writes).
				changed       = fmt::format(" (changed since op {})", found->second.last);
				found->second = {hash, g_op, g_op};
				g_bytes_logged.erase(address);
			} else {
				found->second.last = g_op;
			}
		} else if (g_hashed.size() < MaxHashed) {
			g_hashed.emplace(key, HashedRange {hash, g_op, g_op});
		}
	}
	return fmt::format("{:016x}{}{}", hash, gpu ? " [G]" : "", changed);
}

bool DumpsShaderMemory(uint64_t shader_hash) {
	static const std::vector<uint64_t> hashes = [] {
		std::vector<uint64_t> parsed;
		const char*           text = std::getenv("KYTY_FG_DUMP_SHADERS");
		while (text != nullptr && *text != '\0') {
			char*      end   = nullptr;
			const auto value = std::strtoull(text, &end, 16);
			if (end == text) {
				break;
			}
			parsed.push_back(value);
			text = *end == ',' ? end + 1 : end;
		}
		return parsed;
	}();
	return Logging() && !hashes.empty() &&
	       std::find(hashes.begin(), hashes.end(), shader_hash) != hashes.end();
}

void LogBytes(const char* label, uint64_t address, uint64_t size) {
	if (!Logging() || address == 0 || size == 0 || !g_bytes_logged.insert(address).second) {
		return;
	}
	size = std::min<uint64_t>(LibKernel::Memory::TryClampRangeSize(address, size), 4096u) &
	       ~uint64_t {3};
	std::vector<uint32_t> words(size / 4u);
	if (words.empty() || !LibKernel::Memory::TryReadBacking(address, words.data(), size)) {
		return;
	}
	for (size_t row = 0; row < words.size(); row += 16) {
		std::string line;
		for (size_t k = row; k < std::min(words.size(), row + 16); k++) {
			line += fmt::format(" {:08x}", words[k]);
		}
		Log("  %s +0x%03zx:%s", label, row * 4u, line.c_str());
	}
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

uint64_t ChosenBufferBytes() {
	static const uint64_t limit = [] {
		const char* value = std::getenv("KYTY_FG_DUMP_BYTES");
		const auto  bytes = value != nullptr ? std::strtoull(value, nullptr, 0) : 65536ull;
		return std::clamp<uint64_t>(bytes, KeepBytes, 1ull << 20u);
	}();
	return limit;
}

void NoteChosenBuffer(uint64_t address, uint64_t size) {
	const uint64_t limit = ChosenBufferBytes();
	if (!Logging() || address == 0 || size == 0) {
		return;
	}
	const auto bytes = std::min(size, limit) & ~uint64_t {3};
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

bool VerifyEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_FG_VERIFY");
		return value == nullptr || value[0] != '0';
	}();
	return enabled;
}

bool ClaimVerify(uint64_t address, uint64_t size) {
	if (!Logging() || !VerifyEnabled() || address == 0 || size == 0 || size > MaxVerifyBytes) {
		return false;
	}
	if (g_verify_claimed.contains({address, size})) {
		return false;
	}
	if (g_verify_claimed_bytes + size > VerifyBudgetBytes()) {
		g_verify_over_budget++;
		return false;
	}
	g_verify_claimed.emplace(address, size);
	g_verify_claimed_bytes += size;
	return true;
}

// A claim against one of the budgets.
static bool ClaimVerifyWithin(uint64_t address, uint64_t size, uint64_t& used, uint64_t budget) {
	if (!Logging() || !VerifyEnabled() || address == 0 || size == 0 || size > MaxVerifyBytes) {
		return false;
	}
	if (g_verify_claimed.contains({address, size})) {
		return false;
	}
	if (used + size > budget) {
		g_verify_over_budget++;
		return false;
	}
	g_verify_claimed.emplace(address, size);
	used += size;
	return true;
}

bool ClaimVerifyTexture(uint64_t address, uint64_t size) {
	return ClaimVerifyWithin(address, size, g_verify_texture_bytes, VerifyTexturesBudgetBytes());
}

bool ClaimVerifyLayers(uint64_t address, uint64_t size) {
	return ClaimVerifyWithin(address, size, g_verify_layer_bytes, VerifyLayersBudgetBytes());
}

void CountVerify(bool image, uint64_t bytes, bool differs) {
	auto& count = image ? g_verify_images : g_verify_buffers;
	auto& total = image ? g_verify_image_bytes : g_verify_buffer_bytes;
	auto& stale = image ? g_verify_images_differ : g_verify_buffers_differ;
	count.fetch_add(1, std::memory_order_relaxed);
	total.fetch_add(bytes, std::memory_order_relaxed);
	if (differs) {
		stale.fetch_add(1, std::memory_order_relaxed);
	}
}

bool ClaimGridCheck(uint64_t address) {
	if (!Logging() || address == 0 || g_grid_checks.size() >= MaxGridChecks) {
		return false;
	}
	return g_grid_checks.insert(address).second;
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
		// Code outside the memory the guest maps (the SDK modules' own shaders): read where the
		// translator reads it (ShaderMapUserData hashed it there), as guest addresses are the
		// process's own.
		std::memcpy(code.data(), reinterpret_cast<const void*>(code_address), bytes);
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
