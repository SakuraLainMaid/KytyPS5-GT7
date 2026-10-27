#include "graphics/host_gpu/renderer/cache/streamBuffer.h"

#include "common/alignment.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/host_gpu/gpuPhases.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/bisect.h"
#include "graphics/host_gpu/renderer/cache/bufferAddressLog.h"
#include "graphics/host_gpu/renderer/commandRecorder.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <numeric>
#include <vk_mem_alloc.h>

namespace Libs::Graphics {

namespace {

constexpr size_t WATCHES_INITIAL_RESERVE = 0x4000;
constexpr size_t WATCHES_RESERVE_CHUNK   = 0x1000;

[[nodiscard]] VmaAllocationCreateFlags AllocationFlags(MemoryUsage usage) {
	switch (usage) {
		case MemoryUsage::Upload:
		case MemoryUsage::Stream:
			return VMA_ALLOCATION_CREATE_MAPPED_BIT |
			       VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
		case MemoryUsage::Download:
			return VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
		case MemoryUsage::DeviceLocal: return {};
	}
	return {};
}

[[nodiscard]] VmaMemoryUsage AllocationUsage(MemoryUsage usage) {
	switch (usage) {
		case MemoryUsage::DeviceLocal:
		case MemoryUsage::Stream: return VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
		case MemoryUsage::Upload:
		case MemoryUsage::Download: return VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
	}
	return VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
}

[[nodiscard]] bool AlignUp(uint64_t value, uint64_t alignment, uint64_t& result) {
	if (alignment == 0) {
		result = value;
		return true;
	}
	const auto aligned = Common::AlignUp(value, alignment);
	if (aligned < value) {
		return false;
	}
	result = aligned;
	return true;
}

} // namespace

namespace BufferAddressLog {
namespace {

struct Range {
	uint64_t device_address = 0;
	uint64_t size           = 0;
	uint64_t guest_address  = 0;
	uint32_t usage          = 0;
	uint64_t created_tick   = 0;
	uint64_t destroyed_tick = 0;
	uint64_t destroyed_gpu  = 0;
	int64_t  destroyed_ms   = 0;
};

constexpr size_t DESTROYED_RING = 4096;

std::atomic<bool> g_recording {true};

struct State {
	std::mutex                        mutex;
	std::map<uint64_t, Range>         live; // by device address
	std::array<Range, DESTROYED_RING> destroyed {};
	uint64_t                          destroyed_count = 0;
};

State& Get() {
	static State* state = new State; // left alive: buffers may go after static destructors
	return *state;
}

int64_t NowMs() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
	           std::chrono::steady_clock::now().time_since_epoch())
	    .count();
}

const char* UsageName(uint32_t usage) {
	switch (static_cast<MemoryUsage>(usage)) {
		case MemoryUsage::DeviceLocal: return "device-local";
		case MemoryUsage::Upload: return "upload";
		case MemoryUsage::Download: return "download";
		case MemoryUsage::Stream: return "stream";
	}
	return "?";
}

std::string Name(const Range& range, uint64_t address) {
	char text[256];
	std::snprintf(text, sizeof(text),
	              "%s buffer 0x%016" PRIx64 "+0x%" PRIx64 " (offset 0x%" PRIx64
	              "), guest 0x%016" PRIx64 ", made at tick %" PRIu64,
	              UsageName(range.usage), range.device_address, range.size,
	              address - range.device_address, range.guest_address, range.created_tick);
	return text;
}

} // namespace

bool Recording() noexcept {
	return g_recording.load(std::memory_order_acquire);
}

void Stop() noexcept {
	g_recording.store(false, std::memory_order_release);
}

void NoteCreated(uint64_t device_address, uint64_t size, uint64_t guest_address, uint32_t usage,
                 uint64_t tick) {
	auto&           state = Get();
	std::lock_guard lock(state.mutex);
	state.live[device_address] = {device_address, size, guest_address, usage, tick, 0, 0, 0};
}

void NoteDestroyed(uint64_t device_address, uint64_t tick, uint64_t gpu_tick) {
	auto&           state = Get();
	std::lock_guard lock(state.mutex);
	const auto      found = state.live.find(device_address);
	if (found == state.live.end()) {
		return;
	}
	auto range                                                = found->second;
	range.destroyed_tick                                      = tick;
	range.destroyed_gpu                                       = gpu_tick;
	range.destroyed_ms                                        = NowMs();
	state.destroyed[state.destroyed_count++ % DESTROYED_RING] = range;
	state.live.erase(found);
}

std::string Describe(uint64_t address, uint64_t precision) {
	auto&           state = Get();
	std::lock_guard lock(state.mutex);
	// The fault is somewhere in the precision-aligned block holding the address.
	if (precision > 1 && (precision & (precision - 1)) == 0) {
		address &= ~(precision - 1);
	}
	const uint64_t end = address + std::max<uint64_t>(precision, 1);
	std::string    text;
	auto           above = state.live.lower_bound(end);
	auto           below = above;
	bool           live  = false;
	// Live buffers overlapping the reported range.
	while (below != state.live.begin()) {
		--below;
		const auto& range = below->second;
		if (range.device_address + range.size > address) {
			text += "    in the live " + Name(range, address) + "\n";
			live = true;
		} else {
			break;
		}
	}
	const auto now   = NowMs();
	const auto count = std::min<uint64_t>(state.destroyed_count, DESTROYED_RING);
	uint32_t   shown = 0;
	for (uint64_t i = 0; i < count && shown < 4; i++) {
		const auto& range = state.destroyed[(state.destroyed_count - 1 - i) % DESTROYED_RING];
		if (range.device_address < end && range.device_address + range.size > address) {
			char when[160];
			std::snprintf(when, sizeof(when),
			              ", destroyed at tick %" PRIu64 " (the GPU done to %" PRIu64 "), %" PRId64
			              " ms ago",
			              range.destroyed_tick, range.destroyed_gpu, now - range.destroyed_ms);
			text += "    in a destroyed " + Name(range, address) + when + "\n";
			shown++;
		}
	}
	if (!live && shown == 0) {
		char line[160];
		std::snprintf(line, sizeof(line),
		              "    in no buffer with a device address (live %zu, the latest %" PRIu64
		              " destroyed); nearest:\n",
		              state.live.size(), count);
		text += line;
		if (below != state.live.end() && below->second.device_address < address) {
			text += "      below: " + Name(below->second, address) + "\n";
		}
		if (above != state.live.end()) {
			char distance[64];
			std::snprintf(distance, sizeof(distance), " (0x%" PRIx64 " above)",
			              above->second.device_address - address);
			text += "      above: " + Name(above->second, address) + distance + "\n";
		}
	}
	return text;
}

} // namespace BufferAddressLog

Buffer::Buffer(GraphicContext& graphics, CommandScheduler& scheduler, MemoryUsage usage,
               uint64_t cpu_address, vk::BufferUsageFlags flags, uint64_t size, bool host_cached)
    : m_graphics(&graphics), m_scheduler(&scheduler), m_usage(usage), m_cpu_address(cpu_address),
      m_size(size) {
	KYTY_PROFILER_FUNCTION();
	const GpuPhases::Scope create_phase(GpuPhases::Phase::BufferCreate);
	EXIT_IF(graphics.allocator == nullptr || size == 0);

	vk::BufferCreateInfo buffer_info {};
	buffer_info.size        = size;
	buffer_info.usage       = flags;
	// The readback queue of the copy engines' family reads the cache's buffers too (ReadbackQueue):
	// shared with that family, so neither needs an ownership transfer.
	const std::array<uint32_t, 2> families = {graphics.queue_family, graphics.readback_family};
	if (graphics.readback_family != static_cast<uint32_t>(-1) &&
	    graphics.readback_family != graphics.queue_family) {
		buffer_info.sharingMode           = vk::SharingMode::eConcurrent;
		buffer_info.queueFamilyIndexCount = static_cast<uint32_t>(families.size());
		buffer_info.pQueueFamilyIndices   = families.data();
	}

	// Buffers with device addresses (every game buffer) took dedicated memory, a kernel allocation
	// each. Round 92 (from BryanKAdams' BryKytyPS5, 4068f4ac): up to SharedMax they share VMA's
	// memory blocks; the BDA page table holds each page's own device address, so any placement
	// works. KYTY_BUFFERS_DEDICATED=1 (or the step buffers:dedicated) gives each its own again.
	static constexpr uint64_t SharedMax     = 64ull << 20u;
	static const bool         all_dedicated = [] {
        const char* value = std::getenv("KYTY_BUFFERS_DEDICATED");
        return value != nullptr && value[0] == '1';
	}();
	const bool with_bda = bool(flags & vk::BufferUsageFlagBits::eShaderDeviceAddress);
	const VmaAllocationCreateFlags bda_flag =
	    with_bda && (size > SharedMax || all_dedicated || Bisect::BuffersDedicated())
	        ? VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT
	        : 0;
	VmaAllocationCreateInfo allocation_info {};
	allocation_info.flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT | bda_flag |
	                        (host_cached ? VMA_ALLOCATION_CREATE_MAPPED_BIT |
	                                           VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
	                                     : AllocationFlags(usage));
	allocation_info.usage =
	    host_cached ? VMA_MEMORY_USAGE_AUTO_PREFER_HOST : AllocationUsage(usage);
	allocation_info.preferredFlags =
	    usage == MemoryUsage::DeviceLocal ? VkMemoryPropertyFlags {}
	    : host_cached ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT
	                  : VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

	VmaAllocationInfo allocation_result {};
	VkBuffer          native_buffer = VK_NULL_HANDLE;
	const auto        create        = [&](const VmaAllocationCreateInfo& create_info) {
        return static_cast<vk::Result>(vmaCreateBuffer(
            graphics.allocator, static_cast<const VkBufferCreateInfo*>(buffer_info), &create_info,
            &native_buffer, &m_allocation, &allocation_result));
	};
	auto result = create(allocation_info);
	if (result != vk::Result::eSuccess) {
		// The heap it prefers is out of budget: video memory, on a 4-8 GB card, for the game's
		// large buffers. As images do (GraphicContext::CreateImage), take system memory and pay in
		// bandwidth rather than stop; then the preferred heap past its budget, which the driver
		// may still serve by paging. Only when both fail is the session out of memory.
		auto spill           = allocation_info;
		spill.usage          = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
		spill.preferredFlags = 0;
		const auto first     = result;
		result               = create(spill);
		const char* where    = "system memory";
		if (result != vk::Result::eSuccess) {
			auto over  = allocation_info;
			over.flags &= ~static_cast<VmaAllocationCreateFlags>(VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT);
			result     = create(over);
			where      = "the preferred heap past its budget";
		}
		static std::atomic<uint32_t> spills {0};
		if (spills.fetch_add(1, std::memory_order_relaxed) < 32) {
			LOGF("Buffer spilled (%s): %" PRIu64 " MB, usage %d, first attempt %s -> %s\n", where,
			     size >> 20u, static_cast<int>(usage), vk::to_string(first).c_str(),
			     vk::to_string(result).c_str());
		}
	}
	if (result != vk::Result::eSuccess) {
		graphics.LogMemoryBudget();
		EXIT("Out of memory: a %" PRIu64 " MB GPU buffer fits neither in video memory nor in system "
		     "memory (%s). The VMA heap lines above give usage and budget. Close other programs; on "
		     "a card with little video memory lower the resolution; if Windows' commit limit is "
		     "reached, enlarge the page file.\n",
		     size >> 20u, vk::to_string(result).c_str());
	}

	m_buffer = native_buffer;
	if (with_bda) {
		vk::BufferDeviceAddressInfo address_info {};
		address_info.buffer = m_buffer;
		m_device_address    = graphics.device.getBufferAddress(address_info);
		EXIT_IF(m_device_address == 0);
		if (BufferAddressLog::Recording()) {
			BufferAddressLog::NoteCreated(m_device_address, size, cpu_address,
			                              static_cast<uint32_t>(usage), scheduler.CurrentTick());
		}
	}

	VkMemoryPropertyFlags properties = 0;
	vmaGetAllocationMemoryProperties(graphics.allocator, m_allocation, &properties);
	m_coherent   = (properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
	m_properties = properties;
	if (allocation_result.pMappedData != nullptr) {
		m_mapped = {static_cast<uint8_t*>(allocation_result.pMappedData),
		            static_cast<size_t>(size)};
	}
}

Buffer::~Buffer() {
	if (m_buffer != nullptr) {
		if (m_device_address != 0 && BufferAddressLog::Recording()) {
			BufferAddressLog::NoteDestroyed(
			    m_device_address, m_scheduler != nullptr ? m_scheduler->CurrentTick() : 0,
			    m_scheduler != nullptr ? m_scheduler->GetMasterSemaphore().KnownGpuTick() : 0);
		}
		vmaDestroyBuffer(m_graphics->allocator, m_buffer, m_allocation);
	}
}

vk::DeviceAddress Buffer::BufferDeviceAddress() const noexcept {
	EXIT_IF(m_device_address == 0);
	return m_device_address;
}

bool Buffer::IsInBounds(uint64_t address, uint64_t size) const noexcept {
	return address >= m_cpu_address && size <= Size() && address - m_cpu_address <= Size() - size;
}

void Buffer::Flush(uint64_t offset, uint64_t size) {
	EXIT_IF(m_mapped.empty() || offset > Size() || size > Size() - offset);
	if (!IsCoherent() && size != 0) {
		const auto result =
		    vmaFlushAllocation(m_graphics->allocator, m_allocation, offset, size);
		EXIT_NOT_IMPLEMENTED(static_cast<vk::Result>(result) != vk::Result::eSuccess);
	}
}

void Buffer::Invalidate(uint64_t offset, uint64_t size) {
	EXIT_IF(m_usage != MemoryUsage::Download || offset > Size() || size > Size() - offset);
	if (!IsCoherent() && size != 0) {
		const auto result =
		    vmaInvalidateAllocation(m_graphics->allocator, m_allocation, offset, size);
		EXIT_NOT_IMPLEMENTED(static_cast<vk::Result>(result) != vk::Result::eSuccess);
	}
}

vk::BufferMemoryBarrier Buffer::Barrier(uint64_t offset, uint64_t size, vk::AccessFlags source,
                                        vk::AccessFlags destination) const {
	if (Handle() == nullptr || size == 0 || offset > Size() || size > Size() - offset) {
		EXIT("Buffer: invalid DMA barrier, handle=%p offset=0x%016" PRIx64 " size=0x%016" PRIx64
		     " capacity=0x%016" PRIx64 "\n",
		     static_cast<const void*>(Handle()), offset, size, Size());
	}
	vk::BufferMemoryBarrier barrier {};
	barrier.srcAccessMask       = source;
	barrier.dstAccessMask       = destination;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.buffer              = Handle();
	barrier.offset              = offset;
	barrier.size                = size;
	return barrier;
}

void Buffer::CopyFrom(CommandBuffer& command, const Buffer& source, uint64_t source_offset,
                      uint64_t destination_offset, uint64_t size, vk::AccessFlags source_before,
                      vk::AccessFlags destination_before, vk::AccessFlags source_after,
                      vk::AccessFlags destination_after, const std::source_location& site) {
	if (size == 0 || source_offset > source.Size() || size > source.Size() - source_offset ||
	    destination_offset > Size() || size > Size() - destination_offset) {
		EXIT("Buffer: invalid copy range\n");
	}
	if (source.Handle() == Handle() && source_offset < destination_offset + size &&
	    destination_offset < source_offset + size) {
		EXIT("Buffer: overlapping self-copy\n");
	}
	command.EndRendering();
	const vk::BufferMemoryBarrier before[] = {
	    source.Barrier(source_offset, size, source_before, vk::AccessFlagBits::eTransferRead),
	    Barrier(destination_offset, size, destination_before, vk::AccessFlagBits::eTransferWrite),
	};
	const auto host_access  = vk::AccessFlagBits::eHostRead | vk::AccessFlagBits::eHostWrite;
	auto       before_stage = vk::PipelineStageFlags {vk::PipelineStageFlagBits::eAllCommands};
	if (static_cast<bool>((source_before | destination_before) & host_access)) {
		before_stage |= vk::PipelineStageFlagBits::eHost;
	}
	const auto native = command.Recorder();
	native.pipelineBarrier(before_stage, vk::PipelineStageFlagBits::eTransfer,
	                       vk::DependencyFlagBits::eByRegion, 0, nullptr, 2, before, 0, nullptr,
	                       site);
	const vk::BufferCopy copy {source_offset, destination_offset, size};
	native.copyBuffer(source.Handle(), Handle(), 1, &copy);
	const vk::BufferMemoryBarrier after[] = {
	    source.Barrier(source_offset, size, vk::AccessFlagBits::eTransferRead, source_after),
	    Barrier(destination_offset, size, vk::AccessFlagBits::eTransferWrite, destination_after),
	};
	auto after_stage = vk::PipelineStageFlags {vk::PipelineStageFlagBits::eAllCommands};
	if (static_cast<bool>((source_after | destination_after) & host_access)) {
		after_stage |= vk::PipelineStageFlagBits::eHost;
	}
	native.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, after_stage,
	                       vk::DependencyFlagBits::eByRegion, 0, nullptr, 2, after, 0, nullptr,
	                       site);
}

void Buffer::Fill(uint64_t offset, uint64_t size, uint32_t value,
                  const std::source_location& site) {
	if (((offset | size) & 3u) != 0) {
		EXIT("Buffer: fill range must be dword aligned\n");
	}
	auto& command = Scheduler().Current();
	command.EndRendering();
	const auto before =
	    Barrier(offset, size, vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
	            vk::AccessFlagBits::eTransferWrite);
	const auto native = command.Recorder();
	native.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                       vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlagBits::eByRegion,
	                       0, nullptr, 1, &before, 0, nullptr, site);
	native.fillBuffer(Handle(), offset, size, value);
	const auto after = Barrier(offset, size, vk::AccessFlagBits::eTransferWrite,
	                           vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite);
	native.pipelineBarrier(
	    vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eAllCommands,
	    vk::DependencyFlagBits::eByRegion, 0, nullptr, 1, &after, 0, nullptr, site);
}

StreamBuffer::StreamBuffer(GraphicContext& graphics, CommandScheduler& scheduler, MemoryUsage usage,
                           uint64_t size, bool host_cached)
    : Buffer(graphics, scheduler, usage, 0, AllFlags, size, host_cached),
      m_current_watches(WATCHES_INITIAL_RESERVE), m_previous_watches(WATCHES_INITIAL_RESERVE) {}

bool StreamBuffer::NormalizeReservation(bool coherent, uint64_t atom, uint64_t& size,
                                        uint64_t& alignment) {
	if (coherent) {
		return true;
	}
	if (!AlignUp(size, atom, size)) {
		return false;
	}
	const auto divisor = std::gcd(alignment, atom);
	if (alignment != 0 && alignment / divisor > UINT64_MAX / atom) {
		return false;
	}
	alignment = alignment == 0 ? atom : alignment / divisor * atom;
	return true;
}

std::pair<uint8_t*, uint64_t> StreamBuffer::Map(uint64_t size, uint64_t alignment,
                                                bool allow_wait) {
	if (Mapped().empty()) {
		return {nullptr, 0};
	}
	uint64_t   mapped_size = size;
	const auto atom        = Graphics().physical_device_properties.limits.nonCoherentAtomSize;
	if (!NormalizeReservation(IsCoherent(), atom, mapped_size, alignment)) {
		return {nullptr, 0};
	}
	if (mapped_size > Size()) {
		return {nullptr, 0};
	}

	uint64_t aligned_offset = 0;
	if (!AlignUp(m_offset, alignment, aligned_offset)) {
		return {nullptr, 0};
	}

	const bool wrap = aligned_offset > Size() - mapped_size;
	if (wrap) {
		aligned_offset = 0;
	}

	auto wait_cursor = wrap ? size_t {0} : m_wait_cursor;
	auto wait_bound  = wrap ? uint64_t {0} : m_wait_bound;
	auto invalidation_mark =
	    wrap ? std::optional<size_t> {m_current_watch_cursor} : m_invalidation_mark;
	auto& pending_watches = wrap ? m_current_watches : m_previous_watches;
	if (!WaitPendingOperations(pending_watches, invalidation_mark, aligned_offset + mapped_size,
	                           allow_wait, wait_cursor, wait_bound)) {
		return {nullptr, 0};
	}

	if (wrap) {
		m_invalidation_mark    = invalidation_mark;
		m_current_watch_cursor = 0;
		std::swap(m_previous_watches, m_current_watches);
	}
	m_wait_cursor = wait_cursor;
	m_wait_bound  = wait_bound;
	m_offset      = aligned_offset;
	m_mapped_size = mapped_size;
	return {Mapped().data() + m_offset, m_offset};
}

void StreamBuffer::Commit() {
	if (Usage() != MemoryUsage::Download && m_mapped_size != 0) {
		Flush(m_offset, m_mapped_size);
	}

	m_offset += m_mapped_size;
	const auto tick = Scheduler().CurrentTick();
	if (m_current_watch_cursor != 0 && m_current_watches[m_current_watch_cursor - 1].tick == tick) {
		m_current_watches[m_current_watch_cursor - 1].upper_bound = m_offset;
		return;
	}
	if (m_current_watch_cursor + 1 >= m_current_watches.size()) {
		m_current_watches.resize(m_current_watches.size() + WATCHES_RESERVE_CHUNK);
	}
	auto& watch       = m_current_watches[m_current_watch_cursor++];
	watch.upper_bound = m_offset;
	watch.tick        = tick;
}

uint64_t StreamBuffer::Copy(const void* source, uint64_t size, uint64_t alignment) {
	EXIT_IF(source == nullptr);
	const auto [data, offset] = Map(size, alignment);
	EXIT_IF(data == nullptr);
	std::memcpy(data, source, static_cast<size_t>(size));
	Commit();
	return offset;
}

bool StreamBuffer::WaitPendingOperations(const std::vector<Watch>& watches,
                                         std::optional<size_t>     invalidation_mark,
                                         uint64_t requested_upper_bound, bool allow_wait,
                                         size_t& wait_cursor, uint64_t& wait_bound) {
	if (!invalidation_mark.has_value()) {
		return true;
	}
	while (requested_upper_bound > wait_bound && wait_cursor < *invalidation_mark) {
		const auto& watch = watches[wait_cursor];
		if (!Scheduler().IsFree(watch.tick) && !allow_wait) {
			return false;
		}
		// Profiling-only zone: a stream-buffer ring wrap forcing the CPU to wait for the GPU to
		// finish with the memory it's about to reclaim. Scheduler().IsFree() above is the fast,
		// non-blocking path -- only reaching Wait() itself means the GPU genuinely hasn't caught
		// up yet.
		{
			KYTY_PROFILER_BLOCK("StreamBuffer::WaitPendingOperations (ring wrap wait)");
			Scheduler().Wait(watch.tick);
			if (Usage() == MemoryUsage::Download) {
				Scheduler().WaitPriorityOperations(watch.tick);
			}
		}
		wait_bound = watch.upper_bound;
		++wait_cursor;
	}
	return true;
}

} // namespace Libs::Graphics
