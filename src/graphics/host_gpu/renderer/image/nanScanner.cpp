#include "graphics/host_gpu/renderer/image/nanScanner.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "gpu_tiler_shaders/nan_scan_spv.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandRecorder.h"

#include <algorithm>
#include <span>

namespace Libs::Graphics {

namespace {

struct PushConstants {
	uint32_t words_low;
	uint32_t words_high;
	uint32_t counts_low;
	uint32_t counts_high;
	uint32_t words_per_layer;
	uint32_t layers;
	uint32_t packing;
};

constexpr uint32_t LocalSize = 256;

} // namespace

NanScanner::NanScanner(GraphicContext& graphics): m_graphics(graphics) {
	// A diagnostic: a failure leaves the scanner unusable (Valid() is false) instead of stopping
	// the emulator.
	const vk::PushConstantRange  push_range {vk::ShaderStageFlagBits::eCompute, 0,
                                            sizeof(PushConstants)};
	vk::PipelineLayoutCreateInfo pipeline_layout_info {};
	pipeline_layout_info.pushConstantRangeCount = 1;
	pipeline_layout_info.pPushConstantRanges    = &push_range;
	auto result =
	    graphics.device.createPipelineLayout(&pipeline_layout_info, nullptr, &m_pipeline_layout);
	if (result != vk::Result::eSuccess) {
		LOGF("NanScanner: create pipeline layout failed: %s\n", vk::to_string(result).c_str());
		m_pipeline_layout = nullptr;
		return;
	}

	const std::span<const uint32_t> code {NAN_SCAN_SPV};
	vk::ShaderModuleCreateInfo      module_info {};
	module_info.codeSize    = code.size_bytes();
	module_info.pCode       = code.data();
	vk::ShaderModule module = nullptr;
	result                  = graphics.device.createShaderModule(&module_info, nullptr, &module);
	if (result != vk::Result::eSuccess) {
		LOGF("NanScanner: create shader module failed: %s\n", vk::to_string(result).c_str());
		return;
	}
	vk::PipelineShaderStageCreateInfo stage {};
	stage.stage  = vk::ShaderStageFlagBits::eCompute;
	stage.module = module;
	stage.pName  = "main";
	vk::ComputePipelineCreateInfo pipeline_info {};
	pipeline_info.stage  = stage;
	pipeline_info.layout = m_pipeline_layout;
	result =
	    graphics.device.createComputePipelines(nullptr, 1, &pipeline_info, nullptr, &m_pipeline);
	graphics.device.destroyShaderModule(module, nullptr);
	if (result != vk::Result::eSuccess) {
		LOGF("NanScanner: create pipeline failed: %s\n", vk::to_string(result).c_str());
		m_pipeline = nullptr;
	}
}

NanScanner::~NanScanner() {
	if (m_pipeline) {
		m_graphics.device.destroyPipeline(m_pipeline, nullptr);
	}
	if (m_pipeline_layout) {
		m_graphics.device.destroyPipelineLayout(m_pipeline_layout, nullptr);
	}
}

void NanScanner::Scan(const CommandRecorder& command, vk::DeviceAddress words,
                      vk::DeviceAddress counts, uint32_t words_per_layer, uint32_t layers,
                      Packing packing) const {
	const uint64_t total = uint64_t {words_per_layer} * layers;
	if (!Valid() || total == 0 || total > UINT32_MAX) {
		return;
	}
	const PushConstants push {static_cast<uint32_t>(words),
	                          static_cast<uint32_t>(words >> 32u),
	                          static_cast<uint32_t>(counts),
	                          static_cast<uint32_t>(counts >> 32u),
	                          words_per_layer,
	                          layers,
	                          static_cast<uint32_t>(packing)};
	const auto          max_groups =
	    m_graphics.physical_device_properties.limits.maxComputeWorkGroupCount[0];
	const uint64_t groups = std::clamp<uint64_t>((total + LocalSize - 1u) / LocalSize, 1u,
	                                             std::min<uint64_t>(max_groups, 65535u));
	command.bindPipeline(vk::PipelineBindPoint::eCompute, m_pipeline);
	command.pushConstants(m_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
	                      &push);
	command.dispatch(static_cast<uint32_t>(groups), 1, 1);
}

} // namespace Libs::Graphics
