#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DESCRIPTORS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DESCRIPTORS_H_

#include "common/assert.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "graphics/host_gpu/renderer/image/image.h"
#include "graphics/host_gpu/renderer/pipeline/bindlessTable.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/shaderBindings.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

namespace Libs::Graphics {

struct ShaderStageRuntime;

struct TextureBinding {
	ImageId                    image_id;
	vk::ImageView              image_view = nullptr;
	TextureCache::ImageDesc    desc;
	vk::ImageLayout            layout = vk::ImageLayout::eUndefined;
	std::vector<vk::ImageView> mip_views;
	// The texture memo (RenderExecutor::ResolveTexture): the view a binding of the same
	// descriptor was given before, for RebindImages to use again once the image is found
	// unchanged; and, for a binding just looked up in the texture cache, the image set's
	// generation of that lookup (TextureCache::ImageSetGeneration), so that RebindImages can keep
	// it (0: it may not be kept).
	vk::ImageView memo_view       = nullptr;
	uint64_t      memo_generation = 0;
};

struct PreparedBindings {
	struct BufferSource {
		uint64_t address = 0;
		uint64_t size    = 0;
		BufferId id;
	};

	// The draw owns the immutable compiled-program/runtime-snapshot association through commit.
	const ShaderStageRuntime* runtime = nullptr;
	// Keep the resolved guest range through cache preparation; only the host buffer ID may
	// become stale and need resolving again when bindings are rebound.
	std::vector<BufferSource>             buffer_sources;
	std::vector<vk::DescriptorBufferInfo> buffers;
	std::vector<TextureBinding>           images;
	std::vector<vk::Sampler>              samplers;
	vk::DescriptorBufferInfo              gds {nullptr, 0, VK_WHOLE_SIZE};
	vk::DescriptorBufferInfo              flattened_srt;
	vk::DescriptorBufferInfo              shader_data_buffer;
	vk::DescriptorBufferInfo              shared_memory;
	std::vector<uint32_t>                 shader_data;
	// Bindless: flattened-SRT words to patch (offset, region, entries) and the heaps whose
	// resolved images must be readable for this draw (stable: the table keeps heaps in a deque).
	std::vector<std::array<uint32_t, 3>>  bindless_patches;
	std::vector<BindlessTable::Heap*>     bindless_heaps;
};

// A shader that reads guest memory through its page table (DMA): the memory its user-data
// pointers point at is made resident before it runs (BufferCache::MakeResident), the first
// KYTY_DMA_PREFAULT_KB (default 4; 0 turns this off) from each. The light-probe bake reads its
// per-probe constants that way, a new page each probe; those it read as zeros drew whole cubes of
// NaN, the black lighting (2026-10-14).
void PrefaultDmaPointers(BufferCache& buffers, const ShaderStageRuntime& stage);
// KYTY_DMA_FAULT_REPORT=1 (diagnostics): the page faults are collected after every DMA dispatch
// that dispatch recovery does not run again, so the "GPU page faults" line names that dispatch
// alone, with its user data; draws are named per collection, as without it.
[[nodiscard]] bool DmaFaultReport();

[[nodiscard]] vk::DescriptorType
NativeDescriptorType(ShaderRecompiler::IR::DescriptorBindingKind kind);
[[nodiscard]] uint32_t
NativeDescriptorCount(const ShaderRecompiler::IR::DescriptorBinding& binding);
[[nodiscard]] vk::DescriptorImageInfo MakeImageInfo(const TextureBinding& texture,
                                                    uint32_t              element = 0);

template <typename T>
[[nodiscard]] T DecodeNativeDescriptor(const ShaderRecompiler::IR::DescriptorValue& value) {
	static_assert(std::is_trivially_copyable_v<T>);
	static_assert(sizeof(T) % sizeof(uint32_t) == 0);
	T result {};
	EXIT_IF(value.dword_count < sizeof(result) / sizeof(uint32_t));
	std::memcpy(&result, value.dwords.data(), sizeof(result));
	return result;
}

[[nodiscard]] bool IsSupportedDepthTextureEncoding(const ShaderTextureResource& descriptor,
                                                   bool r128 = false);
void ValidateStorageTexture(const ShaderRecompiler::IR::ImageResource& resource,
                            const ShaderTextureResource& descriptor, uint64_t size);

// Diagnostics (KYTY_WATCH_SHADER): logs again, a few frames later, the user-data buffers a
// watched draw read, to tell data written after the draw from data never written.
void LogWatchedRereads(BufferCache& cache, uint64_t presented_frames);

// Diagnostics: KYTY_WATCH_SHADER=<hash> with KYTY_WATCH_SHADER_FRAME=<n>: whether a use of the
// shader falls in the n-th presented frame after its first use (each caller counts up to 64).
bool InWatchedShaderFrame(uint64_t hash, uint64_t presented_frame, std::atomic<uint32_t>& logged);
// The presented frame of the watched shader's first use, or UINT64_MAX.
uint64_t WatchedShaderFirstFrame();

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DESCRIPTORS_H_
