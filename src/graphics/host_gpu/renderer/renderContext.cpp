#include "graphics/host_gpu/renderer/renderContext.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/perfStats.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/regionDefinitions.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/presentation/videoOut.h"
#include "kernel/memory.h"
#include "libs/errno.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

namespace Libs::Graphics {

RenderContext::RenderContext(GraphicContext& graphics)
    : m_graphics(graphics), m_render_executor(*this), m_command_scheduler(*this, graphics),
      m_descriptor_heap(graphics, m_command_scheduler.GetMasterSemaphore()),
      m_pipeline_cache(graphics), m_sampler_cache(graphics),
      m_buffer_cache(graphics, m_command_scheduler, m_page_manager, m_texture_cache),
      m_texture_cache(graphics, m_command_scheduler, m_page_manager, m_buffer_cache),
      m_bindless_table(graphics, m_command_scheduler, m_page_manager) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
	m_texture_cache.on_bindless_unregister = [this](ImageId id) {
		m_bindless_table.OnImageUnregistered(id);
	};
	m_texture_cache.on_image_release = [this](const Image& image) {
		if (!m_bindless_table.Enabled()) {
			return false;
		}
		std::vector<vk::ImageView> views;
		views.reserve(image.views.size());
		for (const auto& cached: image.views) {
			views.push_back(cached.view);
		}
		return m_bindless_table.ReleaseViews(views);
	};
	// vkQueueSubmit, and the queue lock that present also holds, runs on a queue thread instead
	// of the GPU thread. Frames reach the presenter only after their flip tick completed, and the
	// presenter's own scheduler stays synchronous. KYTY_ASYNC_SUBMIT=0 submits on the GPU thread.
	// KYTY_RECORD_THREAD=0: no recording thread (CommandScheduler::EnableRecordingThread), which
	// submits too.
	const char* record_thread = std::getenv("KYTY_RECORD_THREAD");
	const char* async_submit  = std::getenv("KYTY_ASYNC_SUBMIT");
	if (record_thread == nullptr || std::strcmp(record_thread, "0") != 0) {
		m_command_scheduler.EnableRecordingThread();
	} else if (async_submit == nullptr || std::strcmp(async_submit, "0") != 0) {
		m_command_scheduler.EnableAsyncSubmit();
	}
	LOGF("Recording thread: %s, async queue submit: %s\n",
	     m_command_scheduler.Threaded() ? "on" : "off",
	     m_command_scheduler.AsyncSubmit() ? "on" : "off");
}

RenderContext::~RenderContext() {
	ShutdownGpu();
	m_command_scheduler.Shutdown();
}

void WarmUpLocalMemory(GraphicContext& graphics, CommandScheduler& scheduler);

void RenderContext::InitializeGpu(VideoOut::VideoOutDriver* video_out) {
	EXIT_IF(m_gpu != nullptr);
	WarmUpLocalMemory(m_graphics, m_command_scheduler);
	m_video_out = video_out;
	m_gpu       = std::make_unique<GuestGpu>(*this);
}

void RenderContext::ShutdownGpu() {
	if (m_gpu != nullptr) {
		m_gpu->Shutdown();
		m_gpu.reset();
	}
	if (m_video_out != nullptr) {
		if (m_command_scheduler.Active()) {
			m_command_scheduler.Finish();
		}
		m_command_scheduler.DrainPriorityOperations();
		m_video_out = nullptr;
	}
}

GuestGpu& RenderContext::GetGpu() const {
	EXIT_IF(m_gpu == nullptr);
	return *m_gpu;
}

VideoOut::VideoOutDriver& RenderContext::GetVideoOut() const {
	EXIT_IF(m_video_out == nullptr);
	return *m_video_out;
}

bool RenderContext::HandleFault(PageFaultAccess access, uint64_t fault_vaddr) noexcept {
	// The host reports the faulting byte, not the instruction's access width. Both caches
	// resolve its page; guessing a width can cross the end of a valid guest mapping.
	constexpr uint64_t fault_size = 1;
	if (!IsMapped(fault_vaddr, fault_size)) {
		return false;
	}
	if (access == PageFaultAccess::Write) {
		m_buffer_cache.InvalidateMemory(fault_vaddr, fault_size);
		m_texture_cache.InvalidateMemory(fault_vaddr, fault_size);
		m_bindless_table.OnCpuWrite(fault_vaddr, fault_size);
	} else {
		m_buffer_cache.ReadMemory(fault_vaddr, fault_size);
	}
	return true;
}

bool RenderContext::CanServeCleanRead(uint64_t fault_vaddr, uint64_t vaddr,
                                      uint64_t size) const noexcept {
	return IsMapped(vaddr, size) && m_page_manager.IsReadWatched(fault_vaddr) &&
	       m_buffer_cache.IsCleanForConcurrentRead(vaddr, size);
}

namespace {

// Stores done in place: each is a round trip to the GPU thread that leaves the page protected. A
// page the CPU writes over many times (a structure filled field by field) costs more that way than
// one download, after which it is writable: after PageBudget of them within WindowMs, the page's
// next store takes the download. A store that spends the budget of BulkPages pages within a second
// is copying in bulk (a memcpy into GPU-written memory): it takes the download for BulkMs.
constexpr uint32_t PageBudget = 8;
constexpr uint64_t WindowMs   = 100;
constexpr uint32_t BulkPages  = 2;
constexpr uint64_t BulkMs     = 2000;

bool TakeStoreBudget(uint64_t pc, uint64_t page) {
	struct PageEntry {
		uint64_t page     = UINT64_MAX;
		uint64_t since_ms = 0;
		uint32_t count    = 0;
	};
	struct SiteEntry {
		uint64_t pc            = 0;
		uint64_t since_ms      = 0;
		uint32_t spent         = 0; // pages whose budget it spent since since_ms
		uint64_t bulk_until_ms = 0;
	};
	static std::array<PageEntry, 1024> pages {};
	static std::array<SiteEntry, 256>  sites {};
	static std::mutex                  mutex;
	const auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
	                                           std::chrono::steady_clock::now().time_since_epoch())
	                                           .count());
	std::scoped_lock lock(mutex);
	auto&            site = sites[(pc ^ (pc >> 8u)) % sites.size()];
	if (site.pc != pc) {
		site = {pc, now, 0, 0};
	}
	if (now < site.bulk_until_ms) {
		return false;
	}
	auto& entry = pages[(page / TRACKER_PAGE_SIZE) % pages.size()];
	if (entry.page != page || now - entry.since_ms >= WindowMs) {
		entry = {page, now, 0};
	}
	if (entry.count >= PageBudget) {
		// This store takes the download, which leaves the page writable.
		if (now - site.since_ms >= 1000) {
			site.since_ms = now;
			site.spent    = 0;
		}
		if (++site.spent >= BulkPages) {
			site.bulk_until_ms = now + BulkMs;
			site.spent         = 0;
		}
		return false;
	}
	entry.count++;
	return true;
}

} // namespace

bool RenderContext::IsGpuWrittenPage(uint64_t fault_vaddr) noexcept {
	if (!IsMapped(fault_vaddr, 1)) {
		return false;
	}
	const auto page = fault_vaddr & ~(TRACKER_PAGE_SIZE - 1);
	return m_buffer_cache.IsRegionGpuModified(page, TRACKER_PAGE_SIZE);
}

bool RenderContext::TryWriteFaulting(uint64_t pc, uint64_t fault_vaddr, uint64_t vaddr,
                                     const void* data, uint64_t size) noexcept {
	if (m_gpu == nullptr || GuestGpu::IsGpuThread() || size == 0 || !IsMapped(vaddr, size)) {
		return false;
	}
	// A page with no GPU-written bytes left (they were downloaded with another page) only has its
	// protection lifted on the normal path, without a download: the stores after this one are then
	// free, where done in place each would fault again.
	const auto page = fault_vaddr & ~(TRACKER_PAGE_SIZE - 1);
	if (m_buffer_cache.IsCleanForConcurrentRead(page, TRACKER_PAGE_SIZE)) {
		return false;
	}
	if (!TakeStoreBudget(pc, page)) {
		Common::PerfStats::AddDeclinedStore();
		return false;
	}
	bool written = false;
	if (!m_gpu->TrySendCommandSync([this, vaddr, data, size, &written] {
		    written = m_buffer_cache.WriteClean(vaddr, data, size);
	    })) {
		return false;
	}
	if (!written) {
		Common::PerfStats::AddDeclinedStore();
		return false;
	}
	m_bindless_table.OnCpuWrite(vaddr, size);
	return true;
}

bool RenderContext::InvalidateMemory(uint64_t vaddr, uint64_t size) {
	if (!IsMapped(vaddr, size)) {
		return false;
	}
	m_buffer_cache.InvalidateMemory(vaddr, size);
	m_texture_cache.InvalidateMemory(vaddr, size);
	m_bindless_table.OnCpuWrite(vaddr, size);
	return true;
}

bool RenderContext::IsMapped(uint64_t vaddr, uint64_t size) const noexcept {
	if (!GuestRange {vaddr, size}.Valid()) {
		return false;
	}
	std::shared_lock lock(m_mapped_ranges_mutex);
	return m_mapped_ranges.Contains(vaddr, size);
}

void RenderContext::MapMemory(uint64_t vaddr, uint64_t size) {
	std::lock_guard lock(m_mapped_ranges_mutex);
	m_mapped_ranges.Add(vaddr, size);
}

void RenderContext::UnmapMemory(uint64_t vaddr, uint64_t size) {
	if (CommandScheduler::InDeferredOperation()) {
		EXIT("unsupported memory unmap from an asynchronous GPU completion, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	// The kernel unmaps every free range it is about to (re)map, most of which were never GPU
	// mapped. Buffers, images and GPU-dirty pages are only tracked inside mapped ranges (faults
	// and invalidations outside them are ignored), so such an unmap has nothing to invalidate:
	// skip the GPU-thread round trip and the full drain it forces. KYTY_UNMAP_SKIP_UNMAPPED=0
	// sends every unmap to the GPU thread again.
	static const bool skip_unmapped = [] {
		const auto* value = std::getenv("KYTY_UNMAP_SKIP_UNMAPPED");
		return value == nullptr || std::strcmp(value, "0") != 0;
	}();
	if (skip_unmapped && GuestRange {vaddr, size}.Valid()) {
		std::shared_lock lock(m_mapped_ranges_mutex);
		if (!m_mapped_ranges.Intersects(vaddr, size)) {
			return;
		}
	}
	const auto unmap = [this, vaddr, size] {
		if (m_command_scheduler.Active()) {
			const auto tick = m_command_scheduler.CurrentTick();
			m_command_scheduler.Finish();
			m_command_scheduler.WaitPriorityOperations(tick);
		}
		m_buffer_cache.InvalidateMemory(vaddr, size);
		m_buffer_cache.ForgetUnmapped(vaddr, size);
		m_texture_cache.UnmapMemory(vaddr, size);
		m_bindless_table.UnwatchRange(vaddr, size);
		std::lock_guard lock(m_mapped_ranges_mutex);
		m_mapped_ranges.Subtract(vaddr, size);
	};
	// Shutdown still owns the GPU while queued rendering drains, but its command lane no
	// longer accepts external work. Use the guest GPU's state for the teardown route.
	if (m_gpu == nullptr || m_gpu->IsStopping()) {
		unmap();
		return;
	}
	m_gpu->SendCommandSync(unmap);
}

void RenderContext::PrepareBda() {
	if (!m_bda_logged) {
		Log::WriteToConsoleAndLog("GPU: using buffer device address (BDA) shader memory access.\n");
		m_bda_logged = true;
	}
	// Every cached buffer is synchronized so a global-memory shader sees the CPU's writes.
	// That walk touched every buffer per dispatch (a quarter of the GPU thread in the
	// world); it only has to repeat once something became CPU-dirty or a buffer appeared.
	// The walk runs at most once per guest submission: what the game wrote before submitting is
	// visible to all its commands, as on the console, and the GPU thread runs behind the game
	// anyway. Per draw, the walk and the re-protection it triggers took ~38 % of the GPU thread
	// on the title menu (21 -> 32 fps).
	const auto epoch = g_cpu_dirty_epoch.load(std::memory_order_acquire);
	if (epoch != m_bda_synced_epoch) {
		const auto submission = g_guest_submission_seq.load(std::memory_order_relaxed);
		if (submission == m_bda_synced_submission) {
			m_fault_process_pending = true;
			return;
		}
		m_bda_synced_submission = submission;
	}
	if (epoch != m_bda_synced_epoch) {
		// The guest writes somewhere nearly all the time, so the epoch moves between most
		// dispatches; walk only the regions holding CPU-dirty pages, not every buffer.
		std::shared_lock lock(m_mapped_ranges_mutex);
		m_mapped_ranges.ForEach([this](uint64_t start, uint64_t end) {
			m_buffer_cache.SynchronizeCpuDirtyBuffersInRange(start, end - start);
		});
		m_bda_synced_epoch = epoch;
	}
	m_fault_process_pending = true;
}

// Diagnostics: KYTY_DUMP_GUEST=<frame>:<address>+<size>[,<address>+<size>...] (frame decimal,
// the rest hex) writes guest memory, with the GPU's writes downloaded first, to
// dump_<frame>_<address>.bin once that many frames have been presented. A frame written +<n>
// counts from the first use of the KYTY_WATCH_SHADER shader.
static void DumpGuestMemory(GraphicContext& graphics, BufferCache& cache) {
	struct Request {
		uint64_t                                   frame    = 0;
		bool                                       relative = false;
		std::vector<std::pair<uint64_t, uint64_t>> ranges;
	};
	static const Request request = [] {
		Request     result;
		const char* value = std::getenv("KYTY_DUMP_GUEST");
		if (value == nullptr) {
			return result;
		}
		char* end       = nullptr;
		result.relative = *value == '+';
		result.frame    = std::strtoull(result.relative ? value + 1 : value, &end, 10);
		value        = end != nullptr && *end == ':' ? end + 1 : nullptr;
		while (value != nullptr && *value != '\0') {
			const auto address = std::strtoull(value, &end, 16);
			uint64_t   size    = 4;
			if (end != nullptr && *end == '+') {
				size = std::strtoull(end + 1, &end, 16);
			}
			result.ranges.emplace_back(address, size);
			value = end != nullptr && *end == ',' ? end + 1 : nullptr;
		}
		return result;
	}();
	static bool done = false;
	if (done || request.ranges.empty()) {
		return;
	}
	const auto first = WatchedShaderFirstFrame();
	if (request.relative && first == UINT64_MAX) {
		return;
	}
	const auto frame = request.relative ? first + request.frame : request.frame;
	if (graphics.presented_frames.load(std::memory_order_relaxed) < frame) {
		return;
	}
	done = true;
	for (const auto& [address, size]: request.ranges) {
		cache.DownloadRangeForDiagnostics(address, size);
		std::vector<uint8_t> bytes(size);
		const bool read = LibKernel::Memory::TryReadBacking(address, bytes.data(), size);
		const auto name = "dump_" + std::to_string(frame) + "_" +
		                  std::to_string(address) + ".bin";
		if (read) {
			if (auto* file = std::fopen(name.c_str(), "wb"); file != nullptr) {
				std::fwrite(bytes.data(), 1, bytes.size(), file);
				std::fclose(file);
			}
		}
		LOGF("DumpGuest: 0x%016" PRIx64 "+0x%" PRIx64 " -> %s (%s)\n", address, size, name.c_str(),
		     read ? "written" : "not readable");
	}
}

void RenderContext::RunGarbageCollector() {
	DumpGuestMemory(GetGraphics(), m_buffer_cache);
	LogWatchedRereads(m_buffer_cache, GetGraphics().presented_frames.load(std::memory_order_relaxed));
	if (m_fault_process_pending) {
		m_fault_process_pending = false;
		m_buffer_cache.ProcessFaultBuffer();
	}
	m_texture_cache.ProcessDownloadImages();
	m_texture_cache.RunGarbageCollector();
	m_buffer_cache.RunGarbageCollector();
}

void RenderContext::AddInterruptEq(LibKernel::EventQueue::KernelEqueue eq, int event_id) {
	Common::LockGuard lock(m_interrupt_mutex);

	auto it = std::find_if(
	    m_interrupt_eqs.begin(), m_interrupt_eqs.end(),
	    [eq, event_id](const auto& entry) { return entry.eq == eq && entry.event_id == event_id; });
	if (it != m_interrupt_eqs.end()) {
		return;
	}

	m_interrupt_eqs.push_back({eq, event_id});
}

void RenderContext::DeleteInterruptEq(LibKernel::EventQueue::KernelEqueue eq, int event_id) {
	Common::LockGuard lock(m_interrupt_mutex);

	auto it = std::find_if(
	    m_interrupt_eqs.begin(), m_interrupt_eqs.end(),
	    [eq, event_id](const auto& entry) { return entry.eq == eq && entry.event_id == event_id; });
	if (it == m_interrupt_eqs.end()) {
		return;
	}

	m_interrupt_eqs.erase(it);
}

void RenderContext::TriggerInterrupt(int event_id, uint32_t context_id) {
	std::vector<InterruptEqRegistration> registrations;
	{
		Common::LockGuard lock(m_interrupt_mutex);
		for (const auto& registration: m_interrupt_eqs) {
			if (registration.event_id == event_id) {
				registrations.push_back(registration);
			}
		}
	}

	for (const auto& registration: registrations) {
		const auto result = LibKernel::EventQueue::KernelTriggerEvent(
		    registration.eq, static_cast<uintptr_t>(registration.event_id),
		    LibKernel::EventQueue::KERNEL_EVFILT_GRAPHICS,
		    reinterpret_cast<void*>(static_cast<uintptr_t>(context_id)));
		if (result == LibKernel::KERNEL_ERROR_EBADF || result == LibKernel::KERNEL_ERROR_ENOENT) {
			DeleteInterruptEq(registration.eq, registration.event_id);
			continue;
		}
		EXIT_NOT_IMPLEMENTED(result != OK);
	}
}

} // namespace Libs::Graphics
