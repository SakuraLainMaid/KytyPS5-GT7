#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SHADERRESOURCEBARRIER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SHADERRESOURCEBARRIER_H_

#include "graphics/host_gpu/graphicContext.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"

#include <source_location>

namespace Libs::Graphics {

struct ShaderStageRuntime;
class CommandRecorder;

vk::ShaderStageFlagBits NativeShaderStage(ShaderType stage);
vk::PipelineStageFlags  ShaderPipelineStages(vk::ShaderStageFlags stages);
vk::MemoryBarrier       MakeShaderAccessDependency();
vk::MemoryBarrier       MakeShaderWriteHazardDependency();
vk::MemoryBarrier       MakeShaderWriteDependency();
vk::BufferMemoryBarrier MakeGdsDependency(vk::Buffer buffer);
bool HasShaderBufferWrites(const ShaderStageRuntime& runtime);
// The site is the caller's, for the barrier counts by place (RecordedCommands).
void ShaderAccessBarrier(const CommandRecorder& vk_buffer, vk::PipelineStageFlags source_stages,
                         const std::source_location& site = std::source_location::current());
void ShaderWriteHazardBarrier(const CommandRecorder&      vk_buffer,
                              vk::PipelineStageFlags      destination_stages,
                              const std::source_location& site = std::source_location::current());
void ShaderWriteBarrier(const CommandRecorder& vk_buffer, vk::PipelineStageFlags source_stages,
                        const std::source_location& site = std::source_location::current());

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SHADERRESOURCEBARRIER_H_
