﻿#pragma once

#include <vk_types.h>
#include <vulkan/vulkan_core.h>

class PipelineBuilder {
//> pipeline
public:
    std::vector<VkPipelineShaderStageCreateInfo> _shaderStages;
   
    VkPipelineInputAssemblyStateCreateInfo _inputAssembly;
    VkPipelineRasterizationStateCreateInfo _rasterizer;
    VkPipelineColorBlendAttachmentState _colorBlendAttachment;
    VkPipelineMultisampleStateCreateInfo _multisampling;
    VkPipelineLayout _pipelineLayout;
    VkPipelineDepthStencilStateCreateInfo _depthStencil;
    VkPipelineRenderingCreateInfo _renderInfo;
    VkFormat _colorAttachmentformat;
	VkFormat _depthFormat;

	PipelineBuilder(){ clear(); }

    PipelineBuilder& clear();

    VkPipeline build_pipeline(VkDevice device);
	void initDefault();
    PipelineBuilder& set_shaders(VkShaderModule vertexShader, VkShaderModule fragmentShader, VkShaderModule geometryShader = nullptr);
    PipelineBuilder& set_input_topology(VkPrimitiveTopology topology);
    PipelineBuilder& set_polygon_mode(VkPolygonMode mode, float lineWidth = 1.0);
    PipelineBuilder& set_cull_mode(VkCullModeFlags cullMode, VkFrontFace frontFace);
    PipelineBuilder& set_multisampling_none();
    PipelineBuilder& disable_blending();
    PipelineBuilder& enable_blending_additive();
    PipelineBuilder& enable_blending_alphablend();
    PipelineBuilder& set_conservative(float overestimate);

    PipelineBuilder& set_color_attachment_format(VkFormat format);
    PipelineBuilder& set_depth_format(VkFormat format);
    PipelineBuilder& disable_depthtest();
    PipelineBuilder& disable_color_output();
    PipelineBuilder& enable_depthtest(bool depthWriteEnable,VkCompareOp op);
};

namespace vkutil {
bool load_shader_module(const std::string& filePath, VkDevice device, VkShaderModule* outShaderModule);
}
