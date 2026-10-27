#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SPIRVCOVERAGEMASK_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SPIRVCOVERAGEMASK_H_

#include "graphics/host_gpu/vulkanCommon.h"

#include <array>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <span>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics {

// Round 159: PA_SC_AA_MASK, the samples each pixel of a 2x2 quad may cover (16 bits a pixel:
// X0Y0 and X1Y0 in the first register, X0Y1 and X1Y1 in the second). Vulkan's sample mask is the
// same for every pixel, so a draw whose masks differ by pixel position (the ghost cars of time
// trials draw only half their pixels in a checkerboard, issue 30) runs a copy of its pixel shader
// that writes gl_SampleMask from its pixel's position, or, without a pixel shader, a fragment
// shader that only does that. The masks are the copy's constants; the pipeline key holds them.

// The copy of a fragment shader that ANDs its coverage with the AA masks (as given, already
// reduced to the draw's samples); empty when the shader writes gl_SampleMask itself or has a form
// the rewrite does not handle.
[[nodiscard]] std::vector<uint32_t> SpirvWithAaMask(std::span<const uint32_t> spirv,
                                                    uint32_t mask01, uint32_t mask23);
// A fragment shader with no outputs but gl_SampleMask from the AA masks.
[[nodiscard]] std::vector<uint32_t> CoverageOnlyFragmentSpirv(uint32_t mask01, uint32_t mask23);

// KYTY_AA_MASK=0 leaves the AA masks out as before round 159 (as does KYTY_BISECT's aamask:off).
[[nodiscard]] bool AaMaskOn();

// The modules of those copies, made on a thread of their own: a pixel shader's from its
// permutation entry in the shader disk cache (the SPIR-V is not kept in memory: the game's are
// 150,000 words and more), read by the hash of the entry's key. Until a copy is made its draws use
// the shader as it is (and a pipeline without the masks).
class CoverageVariants {
public:
	CoverageVariants(vk::Device device, std::filesystem::path disk_directory);
	~CoverageVariants();
	CoverageVariants(const CoverageVariants&)            = delete;
	CoverageVariants& operator=(const CoverageVariants&) = delete;

	// A pixel shader permutation's id and the XXH3-128 of its disk cache entry's key.
	void NotePixelEntry(uint64_t program_id, std::array<uint64_t, 2> entry_hash);
	// The copy of program `program_id` (0: the coverage-only shader) for these masks, or null
	// while it is being made or when it cannot be. Any thread.
	[[nodiscard]] vk::ShaderModule Find(uint64_t program_id, uint64_t shader_hash, uint32_t mask01,
	                                    uint32_t mask23);

private:
	struct Key {
		uint64_t program_id                   = 0;
		uint32_t mask01                       = 0;
		uint32_t mask23                       = 0;
		bool     operator==(const Key&) const = default;
	};
	struct KeyHash {
		std::size_t operator()(const Key& key) const noexcept;
	};
	enum class State { Queued, Ready, Failed, Retry };
	struct Entry {
		State            state       = State::Queued;
		vk::ShaderModule module      = nullptr;
		uint64_t         retry_at_ms = 0;
		uint32_t         attempts    = 0;
		uint64_t         shader_hash = 0;
	};

	void                WorkerLoop(const std::stop_token& stop);
	[[nodiscard]] Entry Make(const Key& key, uint64_t shader_hash);
	[[nodiscard]] bool  ReadSpirv(std::array<uint64_t, 2> entry_hash, std::vector<uint32_t>& spirv);
	[[nodiscard]] bool  Validate(uint64_t shader_hash, const std::vector<uint32_t>& spirv);

	vk::Device                                            m_device;
	std::filesystem::path                                 m_directory;
	std::mutex                                            m_mutex;
	std::condition_variable_any                           m_wake;
	std::unordered_map<uint64_t, std::array<uint64_t, 2>> m_pixel_entries;
	std::unordered_map<Key, Entry, KeyHash>               m_entries;
	std::deque<Key>                                       m_queue;
	uint32_t                                              m_validated         = 0;
	bool                                                  m_validation_failed = false;
	uint32_t                                              m_logged            = 0;
	std::jthread                                          m_worker;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SPIRVCOVERAGEMASK_H_
