#include "graphics/host_gpu/renderer/image/conditionalClear.h"

#include "common/assert.h"
#include "gpu_blit_shaders/color_clear_check_spv.h"
#include "gpu_blit_shaders/gpu_blit_fs_triangle_spv.h"
#include "gpu_blit_shaders/gpu_conditional_clear_float_spv.h"
#include "gpu_blit_shaders/gpu_conditional_clear_sint_spv.h"
#include "gpu_blit_shaders/gpu_conditional_clear_uint_spv.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandRecorder.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderTarget.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <vulkan/vulkan_format_traits.hpp>

namespace Libs::Graphics {

namespace {

struct CheckPush {
	uint32_t metadata_low;
	uint32_t metadata_high;
	uint32_t entry_low;
	uint32_t entry_high;
	uint32_t words;
};

struct DrawPush {
	uint32_t entry_low;
	uint32_t entry_high;
};

} // namespace

bool GpuColorClearEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_GPU_COLOR_CLEAR");
		return value == nullptr || value[0] != '0';
	}();
	return enabled;
}

ConditionalColorClear::Numeric ConditionalColorClear::NumericOf(vk::Format format) {
	const std::string_view numeric = vk::componentNumericFormat(format, 0);
	if (numeric == "UINT") return Numeric::Uint;
	if (numeric == "SINT") return Numeric::Sint;
	return Numeric::Float;
}

ConditionalColorClear::ConditionalColorClear(GraphicContext& graphics, CommandScheduler& scheduler)
    : m_graphics(graphics),
      m_entries(graphics, scheduler, MemoryUsage::DeviceLocal, 0,
                vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eIndirectBuffer |
                    vk::BufferUsageFlagBits::eTransferDst |
                    vk::BufferUsageFlagBits::eShaderDeviceAddress,
                uint64_t {Entries} * EntryStride) {
	SetVulkanObjectNameF(graphics.device, m_entries.Handle(), "Conditional Color Clears");
	EXIT_IF(!m_entries.HasDeviceAddress());

	const vk::PushConstantRange  check_range {vk::ShaderStageFlagBits::eCompute, 0,
                                             sizeof(CheckPush)};
	vk::PipelineLayoutCreateInfo check_layout {};
	check_layout.pushConstantRangeCount = 1;
	check_layout.pPushConstantRanges    = &check_range;
	RequireVulkanSuccess(graphics.device.createPipelineLayout(&check_layout, nullptr, &m_check_layout),
	                     "create color-clear check layout");
	const auto check_module = CompileSPV(COLOR_CLEAR_CHECK_SPV, graphics.device);
	vk::ComputePipelineCreateInfo check_info {};
	check_info.stage.stage  = vk::ShaderStageFlagBits::eCompute;
	check_info.stage.module = check_module;
	check_info.stage.pName  = "main";
	check_info.layout       = m_check_layout;
	const auto result = graphics.device.createComputePipelines(nullptr, 1, &check_info, nullptr,
	                                                           &m_check_pipeline);
	graphics.device.destroyShaderModule(check_module, nullptr);
	RequireVulkanSuccess(result, "create color-clear check pipeline");

	const vk::PushConstantRange  draw_range {vk::ShaderStageFlagBits::eFragment, 0,
                                            sizeof(DrawPush)};
	vk::PipelineLayoutCreateInfo draw_layout {};
	draw_layout.pushConstantRangeCount = 1;
	draw_layout.pPushConstantRanges    = &draw_range;
	RequireVulkanSuccess(graphics.device.createPipelineLayout(&draw_layout, nullptr, &m_draw_layout),
	                     "create conditional-clear layout");
	m_vertex_shader       = CompileSPV(GPU_BLIT_FS_TRIANGLE_SPV, graphics.device);
	m_fragment_shaders[0] = CompileSPV(GPU_CONDITIONAL_CLEAR_FLOAT_SPV, graphics.device);
	m_fragment_shaders[1] = CompileSPV(GPU_CONDITIONAL_CLEAR_UINT_SPV, graphics.device);
	m_fragment_shaders[2] = CompileSPV(GPU_CONDITIONAL_CLEAR_SINT_SPV, graphics.device);
}

ConditionalColorClear::~ConditionalColorClear() {
	for (const auto& [key, pipeline]: m_draw_pipelines) {
		(void)key;
		m_graphics.device.destroyPipeline(pipeline, nullptr);
	}
	for (const auto module: m_fragment_shaders) {
		if (module != nullptr) m_graphics.device.destroyShaderModule(module, nullptr);
	}
	if (m_vertex_shader != nullptr) m_graphics.device.destroyShaderModule(m_vertex_shader, nullptr);
	m_graphics.device.destroyPipelineLayout(m_draw_layout, nullptr);
	m_graphics.device.destroyPipeline(m_check_pipeline, nullptr);
	m_graphics.device.destroyPipelineLayout(m_check_layout, nullptr);
}

vk::Pipeline ConditionalColorClear::GetDrawPipeline(PipelineKey key) {
	const auto cached = std::ranges::find(m_draw_pipelines, key,
	                                      &std::pair<PipelineKey, vk::Pipeline>::first);
	auto       format  = key.format;
	const auto samples = vulkan_sample_count(key.samples);
	EXIT_IF(samples == vk::SampleCountFlagBits {});
	if (cached != m_draw_pipelines.end()) {
		return cached->second;
	}
	std::array<vk::PipelineShaderStageCreateInfo, 2> stages {};
	stages[0].stage  = vk::ShaderStageFlagBits::eVertex;
	stages[0].module = m_vertex_shader;
	stages[0].pName  = "main";
	stages[1].stage  = vk::ShaderStageFlagBits::eFragment;
	stages[1].module = m_fragment_shaders[static_cast<size_t>(NumericOf(format))];
	stages[1].pName  = "main";

	vk::PipelineVertexInputStateCreateInfo   vertex_input {};
	vk::PipelineInputAssemblyStateCreateInfo input_assembly {};
	input_assembly.topology = vk::PrimitiveTopology::eTriangleList;
	vk::PipelineViewportStateCreateInfo viewport {}; // counts are dynamic (WithCount)
	vk::PipelineRasterizationStateCreateInfo rasterization {};
	rasterization.lineWidth = 1.0f;
	rasterization.cullMode  = vk::CullModeFlagBits::eNone;
	vk::PipelineMultisampleStateCreateInfo multisample {};
	// Each pixel's shading writes all of its samples: the clear covers them all.
	multisample.rasterizationSamples = samples;
	vk::PipelineDepthStencilStateCreateInfo depth {};
	vk::PipelineColorBlendAttachmentState blend {};
	blend.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
	                       vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
	vk::PipelineColorBlendStateCreateInfo color_blend {};
	color_blend.attachmentCount = 1;
	color_blend.pAttachments    = &blend;
	const std::array dynamic_states {vk::DynamicState::eViewportWithCount,
	                                 vk::DynamicState::eScissorWithCount};
	vk::PipelineDynamicStateCreateInfo dynamic {};
	dynamic.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
	dynamic.pDynamicStates    = dynamic_states.data();
	vk::PipelineRenderingCreateInfo rendering {};
	rendering.colorAttachmentCount    = 1;
	rendering.pColorAttachmentFormats = &format;

	vk::GraphicsPipelineCreateInfo create {};
	create.pNext               = &rendering;
	create.stageCount          = static_cast<uint32_t>(stages.size());
	create.pStages             = stages.data();
	create.pVertexInputState   = &vertex_input;
	create.pInputAssemblyState = &input_assembly;
	create.pViewportState      = &viewport;
	create.pRasterizationState = &rasterization;
	create.pMultisampleState   = &multisample;
	create.pDepthStencilState  = &depth;
	create.pColorBlendState    = &color_blend;
	create.pDynamicState       = &dynamic;
	create.layout              = m_draw_layout;
	vk::Pipeline pipeline = nullptr;
	RequireVulkanSuccess(
	    m_graphics.device.createGraphicsPipelines(nullptr, 1, &create, nullptr, &pipeline),
	    "create conditional-clear pipeline");
	m_draw_pipelines.emplace_back(key, pipeline);
	return pipeline;
}

void ConditionalColorClear::Record(CommandBuffer& command, vk::DeviceAddress metadata,
                                   uint32_t words, std::span<const Candidate> candidates,
                                   vk::ImageView view, vk::Format format, uint32_t samples,
                                   vk::Extent2D extent) {
	EXIT_IF(command.IsInvalid() || metadata == 0 || words == 0 || view == nullptr ||
	        candidates.empty() || candidates.size() > MaxCandidates);
	command.EndRendering();
	const auto recorder = command.Recorder();

	// An entry is rewritten only after Entries later checks, each ordered behind the draws before.
	const auto     slot   = m_next;
	m_next                = (m_next + 1u) % Entries;
	const auto     offset = vk::DeviceSize {slot} * EntryStride;
	const uint64_t entry  = m_entries.BufferDeviceAddress() + offset;

	Entry values {};
	values.candidate_count = static_cast<uint32_t>(candidates.size());
	for (size_t i = 0; i < candidates.size(); i++) {
		values.codes[i]  = candidates[i].code;
		values.colors[i] = candidates[i].bits;
	}
	constexpr auto CandidatesOffset = offsetof(Entry, candidate_count);
	// The entry's last indirect read and the metadata's writers (copies, shaders) come before.
	vk::MemoryBarrier before {};
	before.srcAccessMask = vk::AccessFlagBits::eShaderWrite | vk::AccessFlagBits::eTransferWrite |
	                       vk::AccessFlagBits::eIndirectCommandRead |
	                       vk::AccessFlagBits::eShaderRead;
	before.dstAccessMask = vk::AccessFlagBits::eTransferWrite;
	recorder.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                         vk::PipelineStageFlagBits::eTransfer, {}, 1, &before, 0, nullptr, 0,
	                         nullptr);
	recorder.updateBuffer(m_entries.Handle(), offset + CandidatesOffset,
	                      sizeof(Entry) - CandidatesOffset,
	                      reinterpret_cast<const uint8_t*>(&values) + CandidatesOffset);
	vk::MemoryBarrier to_check {};
	to_check.srcAccessMask = vk::AccessFlagBits::eShaderWrite | vk::AccessFlagBits::eTransferWrite;
	to_check.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
	recorder.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                         vk::PipelineStageFlagBits::eComputeShader, {}, 1, &to_check, 0,
	                         nullptr, 0, nullptr);

	const CheckPush check {static_cast<uint32_t>(metadata), static_cast<uint32_t>(metadata >> 32u),
	                       static_cast<uint32_t>(entry), static_cast<uint32_t>(entry >> 32u),
	                       words};
	recorder.bindPipeline(vk::PipelineBindPoint::eCompute, m_check_pipeline);
	recorder.pushConstants(m_check_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(check),
	                       &check);
	recorder.dispatch(1, 1, 1);

	// The draw count and color, and the rewritten keys, for every later reader.
	vk::MemoryBarrier after {};
	after.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
	after.dstAccessMask = vk::AccessFlagBits::eIndirectCommandRead |
	                      vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite |
	                      vk::AccessFlagBits::eTransferRead | vk::AccessFlagBits::eTransferWrite;
	recorder.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
	                         vk::PipelineStageFlagBits::eAllCommands, {}, 1, &after, 0, nullptr, 0,
	                         nullptr);

	vk::RenderingAttachmentInfo attachment {};
	attachment.imageView   = view;
	attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
	attachment.loadOp      = vk::AttachmentLoadOp::eLoad;
	attachment.storeOp     = vk::AttachmentStoreOp::eStore;
	vk::RenderingInfo rendering {};
	rendering.renderArea.extent    = extent;
	rendering.layerCount           = 1;
	rendering.colorAttachmentCount = 1;
	rendering.pColorAttachments    = &attachment;
	recorder.beginRendering(rendering);
	recorder.bindPipeline(vk::PipelineBindPoint::eGraphics, GetDrawPipeline({format, samples}));
	const vk::Viewport viewport {0.0f, 0.0f, static_cast<float>(extent.width),
	                             static_cast<float>(extent.height), 0.0f, 1.0f};
	const vk::Rect2D   scissor {{0, 0}, extent};
	recorder.setViewportWithCount(1, &viewport);
	recorder.setScissorWithCount(1, &scissor);
	const DrawPush draw {static_cast<uint32_t>(entry), static_cast<uint32_t>(entry >> 32u)};
	recorder.pushConstants(m_draw_layout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(draw),
	                       &draw);
	recorder.drawIndirect(m_entries.Handle(), offset, 1, 16u);
	recorder.endRendering();
}

} // namespace Libs::Graphics
