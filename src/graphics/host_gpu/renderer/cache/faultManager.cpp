#include "graphics/host_gpu/renderer/cache/faultManager.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "gpu_tiler_shaders/fault_buffer_process_spv.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/commandRecorder.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/frontend/decode/ShaderFunctions.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cinttypes>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>

namespace Libs::Graphics {

namespace {

constexpr size_t MaxPageFaults       = 1024;
constexpr size_t PageFaultAreaSize   = MaxPageFaults * sizeof(uint64_t);
constexpr size_t PageFaultBitsetSize = BufferCache::CACHING_NUMPAGES / 8;

// The table-call targets the shaders' expansions trapped on: each joins its shader's next
// expansion (ShaderFunctionExpander).
void ReportCallMisses(const uint8_t* slots) {
	static std::atomic<uint32_t> logged {0};
	static std::atomic<uint64_t> total {0};
	for (uint32_t index = 0; index < kShaderCallMissSlots; ++index) {
		ShaderCallMissSlot slot;
		std::memcpy(&slot, slots + index * sizeof(slot), sizeof(slot));
		if (slot.key == 0) continue;
		const auto hash   = (uint64_t {slot.shader_hash_high} << 32u) | slot.shader_hash_low;
		const auto target = (uint64_t {slot.target_high} << 32u) | slot.target_low;
		if (target == 0) continue;
		bool churned = false;
		if (ShaderRecompiler::Decoder::ReportShaderCallMiss(hash, {target, slot.data0, slot.data1},
		                                                    &churned)) {
			if (churned) {
				LOGF("Shader call target: hash=%016" PRIx64 " target=0x%016" PRIx64
				     " takes new data every frame: its variants are dropped, the miss path "
				     "handles it from now on\n",
				     hash, target);
				continue;
			}
			const auto count = total.fetch_add(1) + 1;
			if (logged.fetch_add(1) < 64 || (count & (count - 1)) == 0) {
				LOGF("Shader call miss: hash=%016" PRIx64 " target=0x%016" PRIx64
				     " data=0x%08x%08x (%" PRIu64 " reported)\n",
				     hash, target, slot.data1, slot.data0, count);
			}
		}
	}
}

// Diagnostics (KYTY_PROBE): the registers the probed shaders recorded (ShaderProbeRecord). The
// first 16 fault-buffer reads that saw a probe log every record, later ones one every 3 seconds,
// so a run shows the values both at the start and as scenes change.
void ReportProbes(const uint8_t* area) {
	uint32_t hits[kShaderProbePoints] {};
	std::memcpy(hits, area + offsetof(ShaderTrapArea, probe_count), sizeof(hits));
	if (std::ranges::all_of(hits, [](uint32_t count) { return count == 0; })) return;
	static std::mutex                            mutex;
	const std::lock_guard                        lock(mutex);
	static uint64_t                              submission = 0;
	static std::chrono::steady_clock::time_point last;
	const auto                                   now = std::chrono::steady_clock::now();
	if (submission++ >= 16 && now - last < std::chrono::seconds(3)) return;
	last = now;
	if (submission == 1) {
		const char* setting = std::getenv("KYTY_PROBE");
		LOGF("Shader probe: KYTY_PROBE=%s\n", setting != nullptr ? setting : "");
	}
	static_assert(kShaderProbePoints == 8);
	LOGF("Shader probe: read %" PRIu64 ", lanes at each point: %u %u %u %u %u %u %u %u\n",
	     submission, hits[0], hits[1], hits[2], hits[3], hits[4], hits[5], hits[6], hits[7]);
	for (uint32_t index = 0; index < kShaderProbeSlots; ++index) {
		ShaderProbeRecord record;
		std::memcpy(&record, area + offsetof(ShaderTrapArea, probes) + index * sizeof(record),
		            sizeof(record));
		if (record.shader_hash_low == 0 && record.shader_hash_high == 0) continue;
		char       text[kShaderProbeValues * 40] {};
		size_t     used   = 0;
		const auto values = std::min(record.count, kShaderProbeValues);
		for (uint32_t value = 0; value < values && used < sizeof(text); value++) {
			const auto bits = record.values[value];
			const auto written =
			    std::snprintf(text + used, sizeof(text) - used, " [%u]=0x%08x(%g)", value, bits,
			                  static_cast<double>(std::bit_cast<float>(bits)));
			if (written <= 0) break;
			used += static_cast<size_t>(written);
		}
		LOGF("Shader probe: point %u hash=%08x%08x pc=0x%x hit=%u%s\n",
		     index / kShaderProbeSlotsPerPoint, record.shader_hash_high, record.shader_hash_low,
		     record.pc, record.hit, text);
	}
}

size_t DownloadAreaSize(const GraphicContext& graphics) {
	const auto& limits = graphics.physical_device_properties.limits;
	const auto  alignment =
	    std::max(limits.nonCoherentAtomSize, limits.minStorageBufferOffsetAlignment);
	return (PageFaultAreaSize + sizeof(ShaderTrapArea) + alignment - 1) & ~(alignment - 1);
}

} // namespace

FaultManager::FaultManager(GraphicContext& graphics, CommandScheduler& scheduler,
                           BufferCache& buffer_cache)
    : m_graphics(graphics), m_scheduler(scheduler), m_buffer_cache(buffer_cache),
      m_download_area_size(DownloadAreaSize(graphics)),
      m_fault_buffer(graphics, scheduler, MemoryUsage::DeviceLocal, 0, AllFlags,
                     PageFaultBitsetSize + sizeof(ShaderTrapArea)),
      m_download_buffer(graphics, scheduler, MemoryUsage::Download, 0, AllFlags,
                        MaxPendingFaults * m_download_area_size) {
	SetVulkanObjectNameF(m_graphics.device, m_fault_buffer.Handle(), "Fault Buffer");

	const vk::DescriptorSetLayoutBinding bindings[] {
	    {0, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute, nullptr},
	    {1, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute, nullptr},
	};
	vk::DescriptorSetLayoutCreateInfo layout_info {};
	layout_info.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
	layout_info.bindingCount = std::size(bindings);
	layout_info.pBindings    = bindings;
	RequireVulkanSuccess(m_graphics.device.createDescriptorSetLayout(&layout_info, nullptr,
	                                                                 &m_fault_process_desc_layout),
	                     "create fault-buffer descriptor layout");

	const auto module = CompileSPV(FAULT_BUFFER_PROCESS_SPV, m_graphics.device);

	vk::PipelineLayoutCreateInfo pipeline_layout_info {};
	pipeline_layout_info.setLayoutCount = 1;
	pipeline_layout_info.pSetLayouts    = &m_fault_process_desc_layout;
	RequireVulkanSuccess(m_graphics.device.createPipelineLayout(&pipeline_layout_info, nullptr,
	                                                            &m_fault_process_pipeline_layout),
	                     "create fault-buffer pipeline layout");

	vk::PipelineShaderStageCreateInfo stage {};
	stage.stage  = vk::ShaderStageFlagBits::eCompute;
	stage.module = module;
	stage.pName  = "main";
	vk::ComputePipelineCreateInfo pipeline_info {};
	pipeline_info.stage  = stage;
	pipeline_info.layout = m_fault_process_pipeline_layout;
	const auto result    = m_graphics.device.createComputePipelines(
	    nullptr, 1, &pipeline_info, nullptr, &m_fault_process_pipeline);
	m_graphics.device.destroyShaderModule(module, nullptr);
	RequireVulkanSuccess(result, "create fault-buffer pipeline");
	SetVulkanObjectNameF(m_graphics.device, m_fault_process_pipeline, "Fault Buffer Parser");
}

FaultManager::~FaultManager() {
	m_graphics.device.destroyPipeline(m_fault_process_pipeline, nullptr);
	m_graphics.device.destroyPipelineLayout(m_fault_process_pipeline_layout, nullptr);
	m_graphics.device.destroyDescriptorSetLayout(m_fault_process_desc_layout, nullptr);
}

Buffer* FaultManager::GetFaultBuffer() noexcept {
	// Construction happens before the scheduler is active. Initialize on the GPU
	// thread at first binding, before any shader can read or claim the record.
	if (!m_initialized) {
		m_fault_buffer.Fill(0, m_fault_buffer.Size(), 0);
		m_initialized = true;
	}
	return &m_fault_buffer;
}

void FaultManager::ProcessFaultBuffer() {
	ProcessFaultBufferImpl(nullptr);
}

ShaderFaultReport FaultManager::CollectFaults() {
	ShaderFaultReport report;
	ProcessFaultBufferImpl(&report);
	// The report is owned by this stack frame; its deferred callback must finish
	// before returning. Newly populated pages are uploaded in the next recording.
	m_scheduler.Finish();
	return report;
}

void FaultManager::ProcessFaultBufferImpl(ShaderFaultReport* report) {
	KYTY_PROFILER_FUNCTION();
	(void)GetFaultBuffer();
	if (const auto wait_tick = m_fault_areas[m_current_area]; wait_tick != 0) {
		m_scheduler.Wait(wait_tick);
		m_scheduler.PopPendingOperations();
	}

	const auto offset = m_current_area * m_download_area_size;
	auto*      mapped = m_download_buffer.Mapped().data() + offset;
	std::memset(mapped, 0, m_download_area_size);
	m_download_buffer.Flush(offset, m_download_area_size);

	vk::BufferMemoryBarrier2 pre_barrier {};
	pre_barrier.srcStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
	pre_barrier.srcAccessMask = vk::AccessFlagBits2::eShaderWrite;
	pre_barrier.dstStageMask =
	    vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eTransfer;
	pre_barrier.dstAccessMask = vk::AccessFlagBits2::eShaderRead |
	                            vk::AccessFlagBits2::eShaderWrite |
	                            vk::AccessFlagBits2::eTransferRead;
	pre_barrier.buffer        = m_fault_buffer.Handle();
	pre_barrier.offset        = 0;
	pre_barrier.size          = m_fault_buffer.Size();
	auto post_barrier         = pre_barrier;
	post_barrier.srcStageMask =
	    vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eTransfer;
	post_barrier.srcAccessMask =
	    vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eTransferRead;
	post_barrier.dstStageMask = vk::PipelineStageFlagBits2::eAllCommands;
	post_barrier.dstAccessMask =
	    vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;

	const vk::DescriptorBufferInfo infos[] {
	    {m_fault_buffer.Handle(), 0, PageFaultBitsetSize},
	    {m_download_buffer.Handle(), offset, PageFaultAreaSize},
	};
	std::array<vk::WriteDescriptorSet, 2> writes {};
	for (uint32_t index = 0; index < writes.size(); ++index) {
		writes[index].dstBinding      = index;
		writes[index].descriptorCount = 1;
		writes[index].descriptorType  = vk::DescriptorType::eStorageBuffer;
		writes[index].pBufferInfo     = &infos[index];
	}

	m_scheduler.EndRendering();
	auto               command = m_scheduler.Current().Recorder();
	vk::DependencyInfo dependency {};
	dependency.dependencyFlags          = vk::DependencyFlagBits::eByRegion;
	dependency.bufferMemoryBarrierCount = 1;
	dependency.pBufferMemoryBarriers    = &pre_barrier;
	command.pipelineBarrier2(dependency);
	command.bindPipeline(vk::PipelineBindPoint::eCompute, m_fault_process_pipeline);
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, m_fault_process_pipeline_layout,
	                             0, writes);
	const auto num_threads    = BufferCache::CACHING_NUMPAGES / 32;
	const auto num_workgroups = (num_threads + 63) / 64;
	command.dispatch(static_cast<uint32_t>(num_workgroups), 1, 1);
	const vk::BufferCopy trap_copy {PageFaultBitsetSize, offset + PageFaultAreaSize,
	                                sizeof(ShaderTrapArea)};
	command.copyBuffer(m_fault_buffer.Handle(), m_download_buffer.Handle(), trap_copy);
	dependency.pBufferMemoryBarriers = &post_barrier;
	command.pipelineBarrier2(dependency);
	vk::BufferMemoryBarrier2 download_barrier {};
	download_barrier.srcStageMask =
	    vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eTransfer;
	download_barrier.srcAccessMask =
	    vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eTransferWrite;
	download_barrier.dstStageMask    = vk::PipelineStageFlagBits2::eHost;
	download_barrier.dstAccessMask   = vk::AccessFlagBits2::eHostRead;
	download_barrier.buffer          = m_download_buffer.Handle();
	download_barrier.offset          = offset;
	download_barrier.size            = m_download_area_size;
	dependency.pBufferMemoryBarriers = &download_barrier;
	command.pipelineBarrier2(dependency);

	// Preserve the record in readback, then release it for the next attempt. Fill
	// supplies the transfer-read -> transfer-write -> shader-access dependencies.
	m_fault_buffer.Fill(PageFaultBitsetSize, sizeof(ShaderTrapArea), 0);
	const auto area = m_current_area;
	m_scheduler.DeferOperation([this, mapped, offset, area, report] {
		m_download_buffer.Invalidate(offset, m_download_area_size);
		ShaderTrapRecord trap;
		std::memcpy(&trap, mapped + PageFaultAreaSize, sizeof(trap));
		ReportCallMisses(mapped + PageFaultAreaSize + offsetof(ShaderTrapArea, misses));
		ReportProbes(mapped + PageFaultAreaSize);
		if (trap.claimed != 0 && report == nullptr) {
			const auto  hash    = (uint64_t {trap.shader_hash_high} << 32) | trap.shader_hash_low;
			const auto* pending = std::bit_cast<const uint64_t*>(mapped);
			EXIT("GPU shader trap: hash=0x%016" PRIx64
			     " pc=0x%08x code=0x%02x pending_pages=%" PRIu64 " first_page=%016" PRIx64 "\n",
			     hash, trap.pc, trap.code, pending[0], pending[1]);
		}
		RangeSet    fault_ranges;
		const auto* faults = std::bit_cast<const uint64_t*>(mapped);
		uint32_t    ray_faults = 0;
		std::memcpy(&ray_faults, mapped + PageFaultAreaSize + offsetof(ShaderTrapArea, ray_faults),
		            sizeof(ray_faults));
		if (report != nullptr) {
			report->page_count = faults[0];
			report->trap       = trap;
		} else if (faults[0] > ray_faults) {
			m_unattributed_faults.fetch_add(1, std::memory_order_release);
		} else if (faults[0] != 0) {
			// Counted, the faults of rays that reach BVH memory for the first time (a new scene)
			// would re-arm every dispatch recovery trusts, each frame until the BVH is mapped.
			static std::atomic<uint32_t> logged {0};
			if (logged.fetch_add(1, std::memory_order_relaxed) < 16) {
				LOGF("Ray tracing page faults: %" PRIu64 " pages mapped for the rays, dispatch "
				     "recovery not re-armed\n",
				     faults[0]);
			}
		}
		// The parser's counter can exceed its bounded output array. Never consume
		// unrecorded addresses as pages (or read beyond the mapped readback area).
		const auto count = std::min<uint64_t>(faults[0], MaxPageFaults - 1);
		if (faults[0] > count) {
			LOGF("GPU page-fault report truncated: %" PRIu64 " entries, %" PRIu64 " recorded\n",
			     faults[0], count);
		}
		for (uint32_t index = 1; index <= count; ++index) {
			const auto address = BufferCache::GuestAddress(faults[index]);
			fault_ranges.Add(address, BufferCache::CACHING_PAGESIZE);
			LOGF("Accessed non-GPU cached memory at 0x%016" PRIx64 "\n", address);
		}
		fault_ranges.ForEach([this](uint64_t start, uint64_t end) {
			EXIT_IF(end - start > std::numeric_limits<uint32_t>::max());
			(void)m_buffer_cache.FindBuffer(start, end - start);
		});
		if (report != nullptr) {
			fault_ranges.ForEach([this](uint64_t start, uint64_t end) {
				m_buffer_cache.SynchronizeBuffersInRange(start, end - start);
			});
		}
		m_fault_areas[area] = 0;
	});

	m_fault_areas[m_current_area++] = m_scheduler.CurrentTick();
	m_current_area %= MaxPendingFaults;
}

} // namespace Libs::Graphics
