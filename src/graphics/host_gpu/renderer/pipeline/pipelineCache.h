#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINECACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINECACHE_H_

#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "graphics/host_gpu/renderer/renderTarget.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/shader.h"
#include "graphics/shader/shaderCompiler.h"

#include <array>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics {

struct GraphicContext;
struct ShaderParams;
struct RenderColorInfo;
struct RenderDepthInfo;
class CommandBuffer;

namespace HW {
class Context;
class Shader;
class UserConfig;
struct ComputeShaderInfo;
} // namespace HW

namespace DrawPrep {
struct PacketResult;
} // namespace DrawPrep

#pragma pack(push, 1)

struct PipelineStaticParameters {
	bool                       negative_one_to_one      = false;
	bool                       depth_clip_enable        = true;
	vk::PrimitiveTopology      topology                 = vk::PrimitiveTopology::ePointList;
	bool                       primitive_restart_enable = false;
	uint32_t                   samples                  = 1;
	bool                       sample_shading_enable    = false;
	bool                       depth_bounds_test_enable = false;
	float                      depth_min_bounds         = 0.0f;
	float                      depth_max_bounds         = 0.0f;
	uint32_t                   color_mask[RENDER_COLOR_ATTACHMENTS_MAX]           = {};
	bool                       cull_front                                         = false;
	bool                       cull_back                                          = false;
	bool                       face                                               = false;
	bool                       provoking_vtx_last                                 = false;
	vk::PolygonMode            polygon_mode                                       = vk::PolygonMode::eFill;
	uint8_t                    color_srcblend[RENDER_COLOR_ATTACHMENTS_MAX]       = {};
	uint8_t                    color_comb_fcn[RENDER_COLOR_ATTACHMENTS_MAX]       = {};
	uint8_t                    color_destblend[RENDER_COLOR_ATTACHMENTS_MAX]      = {};
	uint8_t                    alpha_srcblend[RENDER_COLOR_ATTACHMENTS_MAX]       = {};
	uint8_t                    alpha_comb_fcn[RENDER_COLOR_ATTACHMENTS_MAX]       = {};
	uint8_t                    alpha_destblend[RENDER_COLOR_ATTACHMENTS_MAX]      = {};
	bool                       separate_alpha_blend[RENDER_COLOR_ATTACHMENTS_MAX] = {};
	bool                       blend_enable[RENDER_COLOR_ATTACHMENTS_MAX]         = {};

	bool operator==(const PipelineStaticParameters& other) const noexcept;
};

#pragma pack(pop)

static_assert(std::is_trivially_copyable_v<PipelineStaticParameters>);
static_assert(std::is_standard_layout_v<PipelineStaticParameters>);
static_assert(alignof(PipelineStaticParameters) == 1);
static_assert(sizeof(PipelineStaticParameters) == 125);

struct PipelineRenderingState {
	std::array<vk::Format, RENDER_COLOR_ATTACHMENTS_MAX> color_formats {};
	vk::Format                                           depth_format   = vk::Format::eUndefined;
	vk::Format                                           stencil_format = vk::Format::eUndefined;
	uint32_t                                             color_count    = 0;
	// Sample count of the color attachments when it differs from the rasterization count
	// (PipelineStaticParameters::samples); 0 when every attachment matches rasterization.
	uint32_t                                             color_samples  = 0;
	uint32_t                                             depth_samples  = 0;
	// The bound layer count when a vertex-side stage exports the render target array index
	// (ShaderRecompiler::IR::LayerLimitSpecId), else 0.
	uint32_t                                             layer_limit    = 0;

	bool operator==(const PipelineRenderingState&) const = default;
};

struct PipelineVertexInputState {
	struct Binding {
		uint32_t stride                           = 0;
		bool     instance                         = false;
		bool     operator==(const Binding&) const = default;
	};
	struct Attribute {
		uint32_t offset                             = 0;
		uint8_t  binding                            = 0;
		bool     operator==(const Attribute&) const = default;
	};

	std::array<Binding, ShaderVertexInputInfo::RES_MAX>   bindings {};
	std::array<Attribute, ShaderVertexInputInfo::RES_MAX> attributes {};
	uint8_t                                               binding_count   = 0;
	uint8_t                                               attribute_count = 0;

	bool operator==(const PipelineVertexInputState&) const = default;
};

struct ShaderProgram {
	uint64_t         id     = 0;
	vk::ShaderModule module = nullptr;
	uint32_t         spirv_words = 0;
	uint64_t         hash        = 0;
	// A pixel shader that reads its sample index, as one that runs once per pixel with the index
	// reading 0 (SpirvWithoutSampleId); null otherwise. For NV mixed-sample draws.
	vk::ShaderModule pixel_rate_module = nullptr;
	// A hull shader that samples at an explicit LOD, as one that turns its coordinates into
	// normalized ones at that LOD (SpirvNormalizedExplicitLod), with an id of its own; null
	// otherwise. A draw whose samplers there all have unnormalized coordinates runs it (with them
	// made normalized), and sets normalized_samplers on its copy of the program.
	vk::ShaderModule normalized_module   = nullptr;
	uint64_t         normalized_id       = 0;
	bool             normalized_samplers = false;

	explicit operator bool() const { return id != 0 && module != nullptr; }
};

// The SPIR-V of a pixel shader that reads the SampleId built-in, with that input gone and its
// reads giving 0; empty when the shader has no SampleId input or uses it in a way this does not
// rewrite. A SampleId input makes the fragment shader run once per sample, and NV mixed samples
// (fewer color samples than raster samples, PS5 EQAA) forbids per-sample shading unless coverage
// reduction truncates (VUID-VkPipelineMultisampleStateCreateInfo-rasterizationSamples-01415).
[[nodiscard]] std::vector<uint32_t> SpirvWithoutSampleId(std::span<const uint32_t> spirv);
// KYTY_EQAA_PER_SAMPLE=1 (for testing): NV mixed-sample draws keep pixel shaders that read their
// sample index as they are, per-sample shading included, which NVIDIA does not allow there.
[[nodiscard]] bool EqaaPerSampleShading();

// The SPIR-V of a stage before the rasterizer with every float add, subtract, multiply, divide
// and dot product decorated NoContraction, and its Position output Invariant; empty when there
// is nothing to add. The guest GPU rounds a multiply before the add that follows it unless the
// shader asks for a fused one, and a driver free to fuse them may do so in one shader and not in
// another computing the same position: a depth prepass and the pass that tests EQUAL against it
// then disagree in the last bits.
[[nodiscard]] std::vector<uint32_t> SpirvExactFloat(std::span<const uint32_t> spirv);
// The SPIR-V of a stage whose explicit-LOD samples (OpImageSampleExplicitLod with a Lod and
// nothing but offsets) take their coordinates divided by the size of the mip the LOD names, the
// nearest within the image (an array layer as it is); empty when it has none it can rewrite. Run
// with a normalized sampler, it samples as the guest GPU does with FORCE_UNNORMALIZED: at
// coordinates in that mip's texels. Vulkan allows unnormalized coordinates only on a view of one
// mip at LOD 0, and GT7's hull shaders cull hidden patches against a depth pyramid sampled this
// way at a level each picks from the patch's size on screen.
[[nodiscard]] std::vector<uint32_t> SpirvNormalizedExplicitLod(std::span<const uint32_t> spirv);
// KYTY_UNNORMALIZED_FIX (default on; =0, or KYTY_BISECT's unorm:old, off): hull shaders get the
// SpirvNormalizedExplicitLod copy, used by draws whose samplers there are all unnormalized.
[[nodiscard]] bool UnnormalizedSamplingFix();
// Whether every sampler a stage samples through has unnormalized coordinates (this draw's).
[[nodiscard]] bool StageSamplersAllUnnormalized(const ShaderStageRuntime& stage);
// KYTY_EXACT_FLOAT=1 (round 75, a test): every vertex, geometry and tessellation shader's module is
// made from SpirvExactFloat's copy. The shader cache is unchanged; the driver compiles again.
[[nodiscard]] bool ExactFloatPositions();

// Everything vkCreateGraphicsPipelines reads for one graphics pipeline, owned in one place so the
// driver compile can run on a worker thread (the structures point into each other: it stays
// where it was allocated).
struct GraphicsPipelineBuild {
	GraphicsPipelineBuild() = default;
	~GraphicsPipelineBuild();
	KYTY_CLASS_NO_COPY(GraphicsPipelineBuild);

	vk::Device                                                            device;
	vk::ShaderModule                                                      tess_control = nullptr;
	vk::ShaderModule                                                      tess_eval    = nullptr;
	std::array<vk::PipelineShaderStageCreateInfo, 4>                      stages {};
	std::array<vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo, 4>  subgroup_sizes {};
	std::array<vk::VertexInputAttributeDescription, ShaderVertexInputInfo::RES_MAX> input_attr {};
	std::array<vk::VertexInputBindingDescription, ShaderVertexInputInfo::RES_MAX>   input_desc {};
	vk::PipelineVertexInputStateCreateInfo                                vertex_input_info {};
	vk::PipelineInputAssemblyStateCreateInfo                              input_assembly {};
	vk::PipelineViewportDepthClipControlCreateInfoEXT                     depth_clip_control {};
	vk::PipelineViewportStateCreateInfo                                   viewport_state {};
	vk::PipelineRasterizationDepthClipStateCreateInfoEXT                  clip_ext {};
	vk::PipelineRasterizationStateCreateInfo                              rasterizer {};
	vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT            provoking_vertex {};
	vk::PipelineMultisampleStateCreateInfo                                multisampling {};
	std::array<vk::PipelineColorBlendAttachmentState, RENDER_COLOR_ATTACHMENTS_MAX> color_blend {};
	std::array<vk::Bool32, RENDER_COLOR_ATTACHMENTS_MAX>                  color_write_enable {};
	vk::PipelineColorWriteCreateInfoEXT                                   color_write {};
	vk::PipelineColorBlendStateCreateInfo                                 color_blending {};
	vk::PipelineDepthStencilStateCreateInfo                               depth_stencil {};
	std::vector<vk::DynamicState>                                         dynamic_states;
	vk::PipelineDynamicStateCreateInfo                                    dynamic_state {};
	vk::PipelineTessellationStateCreateInfo                               tessellation {};
	PipelineRenderingState                                                rendering;
	vk::PipelineRenderingCreateInfo                                       rendering_info {};
	std::array<vk::SampleCountFlagBits, RENDER_COLOR_ATTACHMENTS_MAX>     color_attachment_samples {};
	vk::AttachmentSampleCountInfoAMD                                      attachment_sample_count {};
	vk::GraphicsPipelineCreateInfo                                        pipeline_info {};
	vk::SpecializationMapEntry                                            layer_limit_entry {};
	uint32_t                                                              layer_limit_value = 0;
	vk::SpecializationInfo                                                layer_limit_spec {};
	uint64_t                                                              vs_hash = 0;
	uint64_t                                                              ps_hash = 0;
};

// A graphics pipeline a worker thread compiles (see PipelineCache::TryGetGraphicsPipeline).
struct PendingGraphicsPipeline;
class PipelineCompiler;

// The owning renderer serializes access, including saves while the GPU is running.
// Whether every color target the draw writes is at most KYTY_SMALL_TARGET_SYNC pixels wide and
// high (default 1024; 0 = never): such draws are never skipped while their shaders translate or
// their pipeline compiles (pipelineCache.cpp).
bool SmallColorTargetDraw(const HW::Context& context);

class PipelineCache {
public:
	explicit PipelineCache(GraphicContext& graphics);
	~PipelineCache();
	KYTY_CLASS_NO_COPY(PipelineCache);
	void Save();

	struct Pipeline {
		vk::PipelineLayout      pipeline_layout       = nullptr;
		vk::Pipeline            pipeline              = nullptr;
		vk::DescriptorSetLayout descriptor_set_layout = nullptr;
		bool                    uses_push_descriptors = false;
		// Samples bindless images: descriptor set 1 is the bindless table.
		bool                    uses_bindless         = false;
		// Set while a worker compiles `pipeline`; draws skip it until the compile finishes.
		std::shared_ptr<PendingGraphicsPipeline> pending;
	};

	struct GraphicsPrograms {
		std::array<ShaderProgram, 3> vertex;
		ShaderProgram pixel;

		[[nodiscard]] uint32_t VertexStageCount() const { return vertex[1] ? 3u : 1u; }
	};

	GraphicsPrograms
	GetGraphicsPrograms(const HW::VertexShaderInfo& vertex_regs,
	                    const HW::PixelShaderInfo& pixel_regs, const HW::ShaderRegisters& sh,
	                    const HW::Context& context, const HW::UserConfig& user_config,
	                    std::span<const Prospero::ColorComponentMapping, 8> target_export_mapping,
	                    bool pixel_active, std::array<ShaderVertexInputInfo, 3>& vertex_info,
	                    ShaderPixelInputInfo& pixel_info);
	// Draw prep (KYTY_DRAW_PREP, drawPrep.h): the SRT refreshes GetGraphicsPrograms or
	// GetComputeProgram would run for a draw's or dispatch's registers, in two steps so that the
	// walks can run on several threads (round 73): the stage inputs from the registers (the
	// scanner thread, which follows them), then the walks (any of its workers), into `out`.
	struct DrawPrepInputs {
		bool                   compute = false;
		bool                   pixel   = false;
		ShaderParams           pixel_params;
		ShaderPixelInputInfo   pixel_info;
		ShaderParams           vertex_params;
		ShaderVertexInputInfo  vertex_info;
		ShaderParams           compute_params;
		ShaderComputeInputInfo compute_info;
	};
	bool DrawPrepDescribeGraphics(const HW::VertexShaderInfo& vertex_regs,
	                              const HW::PixelShaderInfo&  pixel_regs,
	                              const HW::ShaderRegisters& sh, const HW::Context& context,
	                              const HW::UserConfig&                               user_config,
	                              std::span<const Prospero::ColorComponentMapping, 8> mapping,
	                              bool pixel_active, DrawPrepInputs& inputs);
	bool DrawPrepDescribeCompute(const HW::ComputeShaderInfo& regs, const HW::ShaderRegisters& sh,
	                             const ShaderComputeInputInfo& input_info, DrawPrepInputs& inputs);
	bool DrawPrepWalk(const DrawPrepInputs& inputs, DrawPrep::PacketResult& out);
	ShaderProgram GetComputeProgram(const HW::ComputeShaderInfo& regs,
	                                const HW::ShaderRegisters&   sh,
	                                ShaderComputeInputInfo&      input_info);
	// Whether the shader at this address is on the skip list (--skip-shaders): its dispatches can
	// be dropped before anything is prepared for them.
	[[nodiscard]] static bool IsSkipListed(uint64_t shader_addr);

	Pipeline& GetGraphicsPipeline(std::span<const RenderColorInfo>       colors,
	                              const RenderDepthInfo&                 depth,
	                              std::span<const ShaderVertexInputInfo> vertex_info,
	                              CommandBuffer& command, const ShaderPixelInputInfo* ps_input_info,
	                              vk::PrimitiveTopology topology, bool primitive_restart_enable,
	                              const GraphicsPrograms& programs);
	// As GetGraphicsPipeline; with may_defer, a pipeline the driver does not compile within a
	// short wait is compiled on a worker thread, and nullptr says to skip this draw.
	Pipeline* TryGetGraphicsPipeline(std::span<const RenderColorInfo>       colors,
	                                 const RenderDepthInfo&                 depth,
	                                 std::span<const ShaderVertexInputInfo> vertex_info,
	                                 CommandBuffer&                         command,
	                                 const ShaderPixelInputInfo*            ps_input_info,
	                                 vk::PrimitiveTopology topology, bool primitive_restart_enable,
	                                 const GraphicsPrograms& programs, bool may_defer);
	Pipeline& GetComputePipeline(const ShaderComputeInputInfo& input_info,
	                             const ShaderProgram&          compute_program);

private:
	struct ProgramCache;

	// GetGraphicsPrograms' stage inputs from the registers (shared with DrawPrepDescribeGraphics).
	bool DescribeGraphicsStages(const HW::VertexShaderInfo& vertex_regs,
	                            const HW::PixelShaderInfo& pixel_regs, const HW::ShaderRegisters& sh,
	                            const HW::Context& context, const HW::UserConfig& user_config,
	                            std::span<const Prospero::ColorComponentMapping, 8> target_export_mapping,
	                            bool pixel_active, std::array<ShaderVertexInputInfo, 3>& vertex_info,
	                            ShaderPixelInputInfo& pixel_info,
	                            std::array<ShaderParams, 3>& vertex_params, ShaderParams& pixel_params);

	struct GraphicsPipelineKey {
		PipelineRenderingState   rendering;
		std::array<uint64_t, 3>  vertex_shader_ids {};
		uint64_t                 ps_shader_id = 0;
		PipelineVertexInputState vertex_input;
		PipelineStaticParameters static_params;

		bool operator==(const GraphicsPipelineKey& other) const {
			return rendering == other.rendering && vertex_shader_ids == other.vertex_shader_ids &&
			       ps_shader_id == other.ps_shader_id && vertex_input == other.vertex_input &&
			       static_params == other.static_params;
		}
	};

	struct PipelineKeyHash {
		static void Mix(std::size_t& hash, std::size_t value) {
			hash ^= value + static_cast<std::size_t>(0x9e3779b97f4a7c15ull) + (hash << 6u) +
			        (hash >> 2u);
		}
	};

	struct GraphicsPipelineKeyHash {
		std::size_t operator()(const GraphicsPipelineKey& key) const;
	};

	GraphicContext&               m_graphics;
	std::unique_ptr<ProgramCache> m_program_cache;
	vk::PipelineCache             m_driver_cache = nullptr;
	std::filesystem::path         m_driver_cache_path;
	std::unordered_map<GraphicsPipelineKey, std::unique_ptr<Pipeline>, GraphicsPipelineKeyHash>
	                                                        m_graphics_pipelines;
	std::unordered_map<uint64_t, std::unique_ptr<Pipeline>> m_compute_pipelines;
	std::unique_ptr<PipelineCompiler>                       m_compiler;
	uint64_t                                                m_deferred_draws = 0;

	void InitializeDriverCache();
	// Takes a finished background compile into the pipeline; false while it still runs.
	bool FinishPending(Pipeline& pipeline);
};

// KYTY_DYNAMIC_RASTER_STATE (default on): cull mode and front face (and, except on macOS, the
// depth-bounds test enable and bounds) are dynamic state of every renderer graphics pipeline,
// recorded per draw by SetGraphicsDynamicParams, and leave the pipeline key; key fields that
// cannot change the pipeline are zeroed. =0 bakes them into each pipeline as before.
[[nodiscard]] bool PipelineDynamicRasterStateEnabled();

void LogPipelineTrace(const char* phase, uint64_t vertex_program_id, uint64_t pixel_program_id);
// Creates the pipeline's layouts and fills the create info; CreateGraphicsPipeline compiles it.
std::unique_ptr<GraphicsPipelineBuild>
PrepareGraphicsPipeline(GraphicContext& graphics, PipelineCache::Pipeline& pipeline,
                        const PipelineRenderingState&          rendering,
                        const PipelineVertexInputState&        vertex_input,
                        std::span<const ShaderVertexInputInfo> vertex_info,
                        const ShaderPixelInputInfo*            ps_input_info,
                        const PipelineCache::GraphicsPrograms& programs,
                        const PipelineStaticParameters&        static_params);
// False for an embedded-fetch attribute whose components the shader only replaces by constants:
// it gets no native vertex attribute, and its buffer is not bound for it.
[[nodiscard]] bool VertexAttributeFetched(const ShaderVertexInputInfo& info, int attribute);
// Any thread.
vk::Result CreateGraphicsPipeline(const GraphicsPipelineBuild& build, vk::PipelineCache driver_cache,
                                  vk::Pipeline* pipeline);
void CreatePipelineInternal(GraphicContext& graphics, PipelineCache::Pipeline& pipeline,
                            const PipelineRenderingState&          rendering,
                            const PipelineVertexInputState&        vertex_input,
                            std::span<const ShaderVertexInputInfo> vertex_info,
                            const ShaderPixelInputInfo*            ps_input_info,
                            const PipelineCache::GraphicsPrograms& programs,
                            const PipelineStaticParameters&        static_params,
                            vk::PipelineCache                      driver_cache);
void CreatePipelineInternal(GraphicContext& graphics, PipelineCache::Pipeline& pipeline,
                            const ShaderComputeInputInfo& input_info,
                            vk::ShaderModule compute_module, vk::PipelineCache driver_cache);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINECACHE_H_
