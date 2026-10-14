#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_FAULTMANAGER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_FAULTMANAGER_H_

#include "common/abi.h"
#include "graphics/host_gpu/renderer/cache/bufferCachePages.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace Libs::Graphics {

class BufferCache;

struct ShaderFaultReport {
	uint64_t         page_count = 0;
	ShaderTrapRecord trap;
};

class FaultManager {
	static constexpr size_t MaxPendingFaults = 8;

public:
	FaultManager(GraphicContext& graphics, CommandScheduler& scheduler, BufferCache& buffer_cache);
	~FaultManager();
	KYTY_CLASS_NO_COPY(FaultManager);

	[[nodiscard]] Buffer*           GetFaultBuffer() noexcept;
	void                            ProcessFaultBuffer();
	[[nodiscard]] ShaderFaultReport CollectFaults();
	// Page faults reported while no dispatch transaction was collecting (for example by a
	// dispatch that ran without recovery). A change re-arms dispatch recovery.
	[[nodiscard]] uint64_t UnattributedFaults() const noexcept {
		return m_unattributed_faults.load(std::memory_order_acquire);
	}
	// Diagnostics: a DMA shader recorded since the faults were last collected; a collection that
	// finds faults no dispatch transaction claims names them ("GPU page faults: ...").
	// `label`, when not empty, describes this use of it (KYTY_DMA_FAULT_REPORT).
	void NoteDmaShader(uint64_t hash, std::string label);

private:
	void                                   ProcessFaultBufferImpl(ShaderFaultReport* report);
	GraphicContext&                        m_graphics;
	CommandScheduler&                      m_scheduler;
	BufferCache&                           m_buffer_cache;
	size_t                                 m_download_area_size;
	Buffer                                 m_fault_buffer;
	Buffer                                 m_download_buffer;
	std::array<uint64_t, MaxPendingFaults> m_fault_areas {};
	uint32_t                               m_current_area                  = 0;
	bool                                   m_initialized                   = false;
	vk::DescriptorSetLayout                m_fault_process_desc_layout     = nullptr;
	vk::Pipeline                           m_fault_process_pipeline        = nullptr;
	vk::PipelineLayout                     m_fault_process_pipeline_layout = nullptr;
	std::atomic<uint64_t>                  m_unattributed_faults {0};
	std::vector<std::string>               m_recent_dma;
	std::unordered_set<uint64_t>           m_recent_dma_hashes;
	size_t                                 m_recent_dma_more = 0;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_FAULTMANAGER_H_
