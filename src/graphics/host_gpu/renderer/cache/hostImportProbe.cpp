#include "graphics/host_gpu/renderer/cache/hostImportProbe.h"

#include "common/logging/log.h"
#include "common/threads.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "kernel/memory.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // IWYU pragma: keep
// windows.h defines MemoryBarrier as a macro, which hides vk::MemoryBarrier.
#undef MemoryBarrier
#else
#include <sys/mman.h>
#endif

namespace Libs::Graphics::HostImportProbe {

namespace {

constexpr uint64_t ProbeBytes    = 64ull << 20u;
constexpr uint64_t SmallestGuest = 16ull << 20u;
constexpr uint64_t CompareBytes  = 1ull << 20u;
constexpr uint32_t WriteValue    = 0x5a5a5a5au;
constexpr uint64_t WriteBytes    = 64ull << 10u;

constexpr auto HandleType = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT;

std::string Flags(vk::MemoryPropertyFlags flags) {
	std::string text;
	const auto  add = [&](vk::MemoryPropertyFlagBits bit, const char* name) {
        if (flags & bit) {
            text += text.empty() ? name : std::string("|") + name;
        }
	};
	add(vk::MemoryPropertyFlagBits::eDeviceLocal, "device-local");
	add(vk::MemoryPropertyFlagBits::eHostVisible, "host-visible");
	add(vk::MemoryPropertyFlagBits::eHostCoherent, "host-coherent");
	add(vk::MemoryPropertyFlagBits::eHostCached, "host-cached");
	return text.empty() ? std::string("none") : text;
}

uint32_t FindType(const GraphicContext& graphics, uint32_t bits, vk::MemoryPropertyFlags want,
                  vk::MemoryPropertyFlags avoid = {}) {
	const auto& memory = graphics.physical_device_memory_properties;
	for (uint32_t i = 0; i < memory.memoryTypeCount; i++) {
		const auto flags = memory.memoryTypes[i].propertyFlags;
		if ((bits & (1u << i)) != 0 && (flags & want) == want && !(flags & avoid)) {
			return i;
		}
	}
	return UINT32_MAX;
}

// A buffer of the probe's own, in memory of the kind asked for (mapped when host-visible).
struct OwnBuffer {
	vk::Buffer       buffer = nullptr;
	vk::DeviceMemory memory = nullptr;
	void*            mapped = nullptr;
};

bool CreateOwn(GraphicContext& graphics, uint64_t size, vk::MemoryPropertyFlags want,
               OwnBuffer& out) {
	vk::BufferCreateInfo info {};
	info.size  = size;
	info.usage = vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst;
	if (graphics.device.createBuffer(&info, nullptr, &out.buffer) != vk::Result::eSuccess) {
		return false;
	}
	vk::MemoryRequirements requirements {};
	graphics.device.getBufferMemoryRequirements(out.buffer, &requirements);
	const auto type = FindType(graphics, requirements.memoryTypeBits, want);
	if (type == UINT32_MAX) {
		return false;
	}
	vk::MemoryAllocateInfo allocate {};
	allocate.allocationSize  = requirements.size;
	allocate.memoryTypeIndex = type;
	if (graphics.device.allocateMemory(&allocate, nullptr, &out.memory) != vk::Result::eSuccess) {
		return false;
	}
	if (graphics.device.bindBufferMemory(out.buffer, out.memory, 0) != vk::Result::eSuccess) {
		return false;
	}
	if (want & vk::MemoryPropertyFlagBits::eHostVisible) {
		if (graphics.device.mapMemory(out.memory, 0, size, {}, &out.mapped) !=
		    vk::Result::eSuccess) {
			return false;
		}
	}
	return true;
}

void DestroyOwn(GraphicContext& graphics, OwnBuffer& own) {
	if (own.mapped != nullptr) {
		graphics.device.unmapMemory(own.memory);
	}
	if (own.buffer) {
		graphics.device.destroyBuffer(own.buffer, nullptr);
	}
	if (own.memory) {
		graphics.device.freeMemory(own.memory, nullptr);
	}
	own = {};
}

// Records, submits on the renderer's queue (under its lock) and waits; the milliseconds from the
// submission to the end of the wait, or a negative value on failure.
class Submitter {
public:
	explicit Submitter(GraphicContext& graphics): m_graphics(graphics) {
		vk::CommandPoolCreateInfo pool {};
		pool.flags            = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
		pool.queueFamilyIndex = graphics.queue_family;
		if (graphics.device.createCommandPool(&pool, nullptr, &m_pool) != vk::Result::eSuccess) {
			return;
		}
		vk::CommandBufferAllocateInfo allocate {};
		allocate.commandPool        = m_pool;
		allocate.level              = vk::CommandBufferLevel::ePrimary;
		allocate.commandBufferCount = 1;
		if (graphics.device.allocateCommandBuffers(&allocate, &m_command) != vk::Result::eSuccess) {
			return;
		}
		vk::FenceCreateInfo fence {};
		m_ok = graphics.device.createFence(&fence, nullptr, &m_fence) == vk::Result::eSuccess;
	}
	~Submitter() {
		if (m_fence) {
			m_graphics.device.destroyFence(m_fence, nullptr);
		}
		if (m_pool) {
			m_graphics.device.destroyCommandPool(m_pool, nullptr);
		}
	}
	Submitter(const Submitter&)            = delete;
	Submitter& operator=(const Submitter&) = delete;

	[[nodiscard]] bool Ok() const { return m_ok; }

	template <typename Record>
	double Run(Record&& record) {
		if (!m_ok) {
			return -1.0;
		}
		(void)m_command.reset({});
		vk::CommandBufferBeginInfo begin {};
		begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
		if (m_command.begin(&begin) != vk::Result::eSuccess) {
			return -1.0;
		}
		record(m_command);
		// The transfers' writes made visible to the host before the fence.
		vk::MemoryBarrier barrier {};
		barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
		barrier.dstAccessMask = vk::AccessFlagBits::eHostRead;
		m_command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
		                          vk::PipelineStageFlagBits::eHost, {}, 1, &barrier, 0, nullptr, 0,
		                          nullptr);
		if (m_command.end() != vk::Result::eSuccess) {
			return -1.0;
		}
		if (m_graphics.device.resetFences(1, &m_fence) != vk::Result::eSuccess) {
			return -1.0;
		}
		vk::SubmitInfo submit {};
		submit.commandBufferCount = 1;
		submit.pCommandBuffers    = &m_command;
		const auto start          = std::chrono::steady_clock::now();
		{
			Common::LockGuard lock(m_graphics.queue_mutex);
			if (m_graphics.queue.submit(1, &submit, m_fence) != vk::Result::eSuccess) {
				return -1.0;
			}
		}
		if (m_graphics.device.waitForFences(1, &m_fence, VK_TRUE, 5'000'000'000ull) !=
		    vk::Result::eSuccess) {
			return -1.0;
		}
		return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
		    .count();
	}

private:
	GraphicContext&   m_graphics;
	vk::CommandPool   m_pool    = nullptr;
	vk::CommandBuffer m_command = nullptr;
	vk::Fence         m_fence   = nullptr;
	bool              m_ok      = false;
};

double MegabytesPerSecond(uint64_t bytes, double ms) {
	return ms > 0.0 ? static_cast<double>(bytes) / (1024.0 * 1024.0) / (ms / 1000.0) : 0.0;
}

// The best of three timed copies.
template <typename Record>
double BestOfThree(Submitter& submitter, Record&& record) {
	double best = -1.0;
	for (int i = 0; i < 3; i++) {
		const auto ms = submitter.Run(record);
		if (ms < 0.0) {
			return -1.0;
		}
		best = best < 0.0 ? ms : std::min(best, ms);
	}
	return best;
}

// Imports [pointer, pointer + bytes) and copies it on the GPU. `writable`: the probe's own memory,
// which the GPU may write too (the game's is only read).
void ProbeImport(GraphicContext& graphics, Submitter& submitter, const char* name, uint8_t* pointer,
                 uint64_t bytes, bool writable, OwnBuffer& device_copy, OwnBuffer& readback,
                 double device_ms) {
	const auto                         address = reinterpret_cast<uint64_t>(pointer);
	vk::MemoryHostPointerPropertiesEXT properties {};
	const auto                         query =
	    graphics.device.getMemoryHostPointerPropertiesEXT(HandleType, pointer, &properties);
	if (query != vk::Result::eSuccess || properties.memoryTypeBits == 0) {
		LOGF("Host import probe: %s at 0x%016" PRIx64 " (%" PRIu64 " MB): the driver offers no "
		     "memory type for it (%s, types 0x%x)\n",
		     name, address, bytes >> 20u, vk::to_string(query).c_str(), properties.memoryTypeBits);
		return;
	}
	const auto type = FindType(graphics, properties.memoryTypeBits, {});
	const auto type_flags =
	    graphics.physical_device_memory_properties.memoryTypes[type].propertyFlags;

	vk::ImportMemoryHostPointerInfoEXT import {};
	import.handleType   = HandleType;
	import.pHostPointer = pointer;
	vk::MemoryAllocateInfo allocate {};
	allocate.allocationSize  = bytes;
	allocate.memoryTypeIndex = type;
	allocate.pNext           = &import;
	vk::DeviceMemory memory {};
	const auto       imported = graphics.device.allocateMemory(&allocate, nullptr, &memory);
	if (imported != vk::Result::eSuccess) {
		LOGF("Host import probe: %s at 0x%016" PRIx64 " (%" PRIu64 " MB): the import failed (%s; "
		     "types 0x%x, type %u %s)\n",
		     name, address, bytes >> 20u, vk::to_string(imported).c_str(),
		     properties.memoryTypeBits, type, Flags(type_flags).c_str());
		return;
	}
	vk::ExternalMemoryBufferCreateInfo external {};
	external.handleTypes = HandleType;
	vk::BufferCreateInfo info {};
	info.size  = bytes;
	info.usage = vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
	             vk::BufferUsageFlagBits::eStorageBuffer;
	info.pNext = &external;
	vk::Buffer buffer {};
	bool bound = graphics.device.createBuffer(&info, nullptr, &buffer) == vk::Result::eSuccess;
	if (bound) {
		vk::MemoryRequirements requirements {};
		graphics.device.getBufferMemoryRequirements(buffer, &requirements);
		bound = (requirements.memoryTypeBits & (1u << type)) != 0 && requirements.size <= bytes &&
		        graphics.device.bindBufferMemory(buffer, memory, 0) == vk::Result::eSuccess;
	}
	if (!bound) {
		LOGF("Host import probe: %s: imported, but no buffer could use it\n", name);
	} else {
		// The GPU reading it: into the card's memory, timed.
		const vk::BufferCopy whole {0, 0, bytes};
		const auto           read_ms = BestOfThree(submitter, [&](vk::CommandBuffer command) {
            command.copyBuffer(buffer, device_copy.buffer, 1, &whole);
        });
		// The bytes it read: the first MB back to the CPU, compared with the memory itself.
		const vk::BufferCopy first {0, 0, std::min(bytes, CompareBytes)};
		const auto           back_ms = submitter.Run([&](vk::CommandBuffer command) {
            command.copyBuffer(device_copy.buffer, readback.buffer, 1, &first);
        });
		const bool  same = back_ms >= 0.0 && std::memcmp(readback.mapped, pointer, first.size) == 0;
		std::string written  = "not tried (the game's memory is only read)";
		double      write_ms = -1.0;
		if (writable) {
			// The GPU writing it: the card's copy back over it, timed, then a fill the CPU must
			// see.
			write_ms           = BestOfThree(submitter, [&](vk::CommandBuffer command) {
                command.copyBuffer(device_copy.buffer, buffer, 1, &whole);
            });
			const auto fill_ms = submitter.Run([&](vk::CommandBuffer command) {
				command.fillBuffer(buffer, 0, WriteBytes, WriteValue);
			});
			bool       seen    = fill_ms >= 0.0;
			for (uint64_t i = 0; seen && i < WriteBytes; i += 4) {
				uint32_t word = 0;
				std::memcpy(&word, pointer + i, sizeof(word));
				seen = word == WriteValue;
			}
			written =
			    seen ? "the CPU sees the GPU's writes" : "the CPU does NOT see the GPU's writes";
		}
		LOGF("Host import probe: %s at 0x%016" PRIx64 " (%" PRIu64 " MB, type %u %s): the GPU "
		     "reads it at %.0f MB/s (%.2f ms; its own memory %.0f MB/s), %s; writes it at %.0f "
		     "MB/s; %s\n",
		     name, address, bytes >> 20u, type, Flags(type_flags).c_str(),
		     MegabytesPerSecond(bytes, read_ms), read_ms, MegabytesPerSecond(bytes, device_ms),
		     read_ms < 0.0 ? "the copy failed"
		     : same        ? "the bytes match"
		                   : "the bytes DIFFER",
		     MegabytesPerSecond(bytes, write_ms), written.c_str());
	}
	if (buffer) {
		graphics.device.destroyBuffer(buffer, nullptr);
	}
	graphics.device.freeMemory(memory, nullptr);
}

uint8_t* AllocateOwn(uint64_t bytes) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	return static_cast<uint8_t*>(
	    VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
#else
	void* memory = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	return memory == MAP_FAILED ? nullptr : static_cast<uint8_t*>(memory);
#endif
}

void FreeOwn(uint8_t* memory, uint64_t bytes) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	(void)bytes;
	VirtualFree(memory, 0, MEM_RELEASE);
#else
	munmap(memory, bytes);
#endif
}

void Run(GraphicContext& graphics, const uint8_t* guest, uint64_t guest_address,
         uint64_t guest_bytes) {
	const auto alignment = std::max<uint64_t>(graphics.imported_host_pointer_alignment, 4096u);
	LOGF("Host import probe: VK_EXT_external_memory_host, pointers aligned to %" PRIu64
	     " bytes; a game buffer at 0x%016" PRIx64 " (%" PRIu64 " MB) to try\n",
	     alignment, guest_address, guest_bytes >> 20u);
	Submitter submitter(graphics);
	OwnBuffer device_copy;
	OwnBuffer readback;
	if (!submitter.Ok() ||
	    !CreateOwn(graphics, ProbeBytes, vk::MemoryPropertyFlagBits::eDeviceLocal, device_copy) ||
	    !CreateOwn(graphics, CompareBytes,
	               vk::MemoryPropertyFlagBits::eHostVisible |
	                   vk::MemoryPropertyFlagBits::eHostCoherent,
	               readback)) {
		LOGF("Host import probe: could not make its own buffers\n");
		DestroyOwn(graphics, device_copy);
		DestroyOwn(graphics, readback);
		return;
	}
	// The card's own memory, for comparison: a copy within it.
	OwnBuffer device_second;
	double    device_ms = -1.0;
	if (CreateOwn(graphics, ProbeBytes, vk::MemoryPropertyFlagBits::eDeviceLocal, device_second)) {
		const vk::BufferCopy whole {0, 0, ProbeBytes};
		device_ms = BestOfThree(submitter, [&](vk::CommandBuffer command) {
			command.copyBuffer(device_second.buffer, device_copy.buffer, 1, &whole);
		});
	}
	DestroyOwn(graphics, device_second);

	if (auto* own = AllocateOwn(ProbeBytes); own != nullptr) {
		for (uint64_t i = 0; i < ProbeBytes; i += 4) {
			const auto word = static_cast<uint32_t>(i * 2654435761u);
			std::memcpy(own + i, &word, sizeof(word));
		}
		ProbeImport(graphics, submitter, "its own memory", own, ProbeBytes, true, device_copy,
		            readback, device_ms);
		FreeOwn(own, ProbeBytes);
	} else {
		LOGF("Host import probe: could not allocate its own memory\n");
	}

	// The game's: whole aligned pages inside the buffer's memory, read only.
	const auto begin = (reinterpret_cast<uint64_t>(guest) + alignment - 1) & ~(alignment - 1);
	const auto end   = (reinterpret_cast<uint64_t>(guest) + guest_bytes) & ~(alignment - 1);
	if (end > begin) {
		ProbeImport(graphics, submitter, "the game's memory", reinterpret_cast<uint8_t*>(begin),
		            std::min(end - begin, ProbeBytes), false, device_copy, readback, device_ms);
	}
	DestroyOwn(graphics, device_copy);
	DestroyOwn(graphics, readback);
	LOGF("Host import probe: done\n");
}

} // namespace

bool Requested() {
	static const bool requested = [] {
		const char* value = std::getenv("KYTY_HOST_IMPORT_PROBE");
		return value != nullptr && value[0] == '1';
	}();
	return requested;
}

void MaybeRun(GraphicContext& graphics, uint64_t vaddr, uint64_t size) {
	if (!Requested() || !graphics.external_memory_host_enabled || size < SmallestGuest) {
		return;
	}
	static std::atomic<bool> done {false};
	if (done.load(std::memory_order_relaxed)) {
		return;
	}
	const auto  bytes = std::min(size, ProbeBytes + graphics.imported_host_pointer_alignment);
	const auto* guest = LibKernel::Memory::FindGpuCleanBacking(vaddr, bytes);
	if (guest == nullptr || done.exchange(true)) {
		return;
	}
	Run(graphics, guest, vaddr, bytes);
}

} // namespace Libs::Graphics::HostImportProbe
