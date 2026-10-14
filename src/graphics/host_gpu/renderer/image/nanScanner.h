#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_NANSCANNER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_NANSCANNER_H_

#include "common/common.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>

namespace Libs::Graphics {

class CommandRecorder;
struct GraphicContext;

// Diagnostics: counts, on the GPU, the words of an image's texels that hold a value that is not
// finite (infinity or NaN), one counter per layer (nan_scan.comp). The caller copies a mip of the
// image tightly packed into a buffer with a device address, zeroes the counters, and reads them
// back (TextureCache::ScanNonFiniteGpuLocked); only the counters cross to the host.
class NanScanner {
public:
	// How the float bit fields sit in a word of the texels.
	enum class Packing : uint32_t { B10G11R11 = 0, TwoHalves = 1, Float32 = 2 };

	explicit NanScanner(GraphicContext& graphics);
	~NanScanner();
	KYTY_CLASS_NO_COPY(NanScanner);

	// False when the pipeline could not be created (logged once); Scan then records nothing.
	[[nodiscard]] bool Valid() const { return m_pipeline != nullptr; }

	// Records the count of `words_per_layer * layers` words at `words` into the `layers` counters
	// at `counts` (added to them). Binds a compute pipeline and push constants: call it between a
	// guest dispatch's recording and the next one's bindings.
	void Scan(const CommandRecorder& command, vk::DeviceAddress words, vk::DeviceAddress counts,
	          uint32_t words_per_layer, uint32_t layers, Packing packing) const;

private:
	GraphicContext&    m_graphics;
	vk::PipelineLayout m_pipeline_layout = nullptr;
	vk::Pipeline       m_pipeline        = nullptr;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_NANSCANNER_H_
