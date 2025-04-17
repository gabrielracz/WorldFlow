#include <vk_pipelines.h>

#include "../include/defines.hpp"

#include "vk_initializers.h"
#include <fstream>
#include <iostream>

//> pipe_clear
PipelineBuilder& PipelineBuilder::clear()
{
    // clear all of the structs we need back to 0 with their correct stype

    _inputAssembly = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };

    _rasterizer = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };

    _colorBlendAttachment = {};

    _multisampling = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };

    _pipelineLayout = {};

    _depthStencil = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };

    _renderInfo = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };

    _shaderStages.clear();
    return *this;
}
//< pipe_clear

//> build_pipeline_1
VkPipeline PipelineBuilder::build_pipeline(VkDevice device)
{
    // make viewport state from our stored viewport and scissor.
    // at the moment we wont support multiple viewports or scissors
    VkPipelineViewportStateCreateInfo viewportState = {};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.pNext = nullptr;

    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    // setup dummy color blending. We arent using transparent objects yet
    // the blending is just "no blend", but we do write to the color attachment
    VkPipelineColorBlendStateCreateInfo colorBlending = {};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.pNext = nullptr;

    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.logicOp = VK_LOGIC_OP_COPY;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &_colorBlendAttachment;

    // completely clear VertexInputStateCreateInfo, as we have no need for it
    VkPipelineVertexInputStateCreateInfo _vertexInputInfo = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };

    //< build_pipeline_1

    //> build_pipeline_2
    // build the actual pipeline
    // we now use all of the info structs we have been writing into into this one
    // to create the pipeline
    VkGraphicsPipelineCreateInfo pipelineInfo = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    // connect the renderInfo to the pNext extension mechanism
    pipelineInfo.pNext = &_renderInfo;

    pipelineInfo.stageCount = (uint32_t)_shaderStages.size();
    pipelineInfo.pStages = _shaderStages.data();
    pipelineInfo.pVertexInputState = &_vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &_inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &_rasterizer;
    pipelineInfo.pMultisampleState = &_multisampling;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDepthStencilState = &_depthStencil;
    pipelineInfo.layout = _pipelineLayout;

    //< build_pipeline_2
    //> build_pipeline_3
    VkDynamicState state[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };

    VkPipelineDynamicStateCreateInfo dynamicInfo = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynamicInfo.pDynamicStates = &state[0];
    dynamicInfo.dynamicStateCount = 2;

    pipelineInfo.pDynamicState = &dynamicInfo;
    //< build_pipeline_3
    //> build_pipeline_4
    // its easy to error out on create graphics pipeline, so we handle it a bit
    // better than the common VK_CHECK case
    VkPipeline newPipeline;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo,
            nullptr, &newPipeline)
        != VK_SUCCESS) {
        // fmt::println("failed to create pipeline");
        std::cerr << "failed to create pipeline" << std::endl;
        return VK_NULL_HANDLE; // failed to create graphics pipeline
    } else {
        return newPipeline;
    }
    //< build_pipeline_4
}
//> set_shaders
PipelineBuilder& PipelineBuilder::set_shaders(VkShaderModule vertexShader, VkShaderModule fragmentShader, VkShaderModule geometryShader)
{
    _shaderStages.clear();

    _shaderStages.push_back(
        vkinit::pipeline_shader_stage_create_info(VK_SHADER_STAGE_VERTEX_BIT, vertexShader));

    if(geometryShader) {
        _shaderStages.push_back(
            vkinit::pipeline_shader_stage_create_info(VK_SHADER_STAGE_GEOMETRY_BIT, geometryShader));
    }

    _shaderStages.push_back(
        vkinit::pipeline_shader_stage_create_info(VK_SHADER_STAGE_FRAGMENT_BIT, fragmentShader));
    
    return *this;
}
//< set_shaders
//> set_topo
PipelineBuilder& PipelineBuilder::set_input_topology(VkPrimitiveTopology topology)
{
    _inputAssembly.topology = topology;
    // we are not going to use primitive restart on the entire tutorial so leave
    // it on false
    _inputAssembly.primitiveRestartEnable = VK_FALSE;
    return *this;
}
//< set_topo

//> set_poly
PipelineBuilder& PipelineBuilder::set_polygon_mode(VkPolygonMode mode, float lineWidth)
{
    _rasterizer.polygonMode = mode;
    _rasterizer.lineWidth = lineWidth;
    return *this;
}
//< set_poly

PipelineBuilder& PipelineBuilder::set_cull_mode(VkCullModeFlags cullMode, VkFrontFace frontFace)
{
    _rasterizer.cullMode = cullMode;
    _rasterizer.frontFace = frontFace;

    return *this;
}

PipelineBuilder& PipelineBuilder::set_conservative(float overestimate)
{
    if(overestimate == 0.0) {return *this;}

    VkPipelineRasterizationConservativeStateCreateInfoEXT conservative_rasterization_info{};
    conservative_rasterization_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT;
    // conservative_rasterization_info.conservativeRasterizationMode = (overestimate > 0.0) ? VK_CONSERVATIVE_RASTERIZATION_MODE_OVERESTIMATE_EXT : VK_CONSERVATIVE_RASTERIZATION_MODE_UNDERESTIMATE_EXT;
    // conservative_rasterization_info.extraPrimitiveOverestimationSize = std::fabs(overestimate);

    conservative_rasterization_info.conservativeRasterizationMode = VK_CONSERVATIVE_RASTERIZATION_MODE_OVERESTIMATE_EXT;
    conservative_rasterization_info.extraPrimitiveOverestimationSize = 0.75f;

    // Link the conservative rasterization info to the rasterization state create info
    this->_rasterizer.pNext = &conservative_rasterization_info;
    return *this;
}

//> set_multisample
PipelineBuilder& PipelineBuilder::set_multisampling_none()
{
    _multisampling.sampleShadingEnable = VK_FALSE;
    // multisampling defaulted to no multisampling (1 sample per pixel)
    _multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    _multisampling.minSampleShading = 1.0f;
    _multisampling.pSampleMask = nullptr;
    // no alpha to coverage either
    _multisampling.alphaToCoverageEnable = VK_FALSE;
    _multisampling.alphaToOneEnable = VK_FALSE;
    return *this;
}
//< set_multisample
PipelineBuilder& PipelineBuilder::disable_color_output()
{
    _colorBlendAttachment.colorWriteMask = 0;
    _colorBlendAttachment.blendEnable = VK_FALSE;
    return *this;
}

//> set_noblend
PipelineBuilder& PipelineBuilder::disable_blending()
{
    // default write mask
    _colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    // no blending
    _colorBlendAttachment.blendEnable = VK_FALSE;
    return *this;
}
//< set_noblend

//> alphablend
PipelineBuilder& PipelineBuilder::enable_blending_additive()
{
    _colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    _colorBlendAttachment.blendEnable = VK_TRUE;
    _colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    _colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    _colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    _colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    _colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    _colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    return *this;
}

PipelineBuilder& PipelineBuilder::enable_blending_alphablend()
{
    _colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    _colorBlendAttachment.blendEnable = VK_TRUE;
    _colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    _colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    _colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    _colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    _colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    _colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    return *this;
}
//< alphablend

//> set_formats
PipelineBuilder& PipelineBuilder::set_color_attachment_format(VkFormat format)
{
    _colorAttachmentformat = format;
    // connect the format to the renderInfo  structure
    _renderInfo.colorAttachmentCount = 1;
    _renderInfo.pColorAttachmentFormats = &_colorAttachmentformat;
    return *this;
}

PipelineBuilder& PipelineBuilder::set_depth_format(VkFormat format)
{
    _renderInfo.depthAttachmentFormat = format;
    return *this;
}
//< set_formats

//> depth_disable
PipelineBuilder& PipelineBuilder::disable_depthtest()
{
    _depthStencil.depthTestEnable = VK_FALSE;
    _depthStencil.depthWriteEnable = VK_FALSE;
    _depthStencil.depthCompareOp = VK_COMPARE_OP_NEVER;
    _depthStencil.depthBoundsTestEnable = VK_FALSE;
    _depthStencil.stencilTestEnable = VK_FALSE;
    _depthStencil.front = {};
    _depthStencil.back = {};
    _depthStencil.minDepthBounds = -10.f;
    _depthStencil.maxDepthBounds = 10.f;
    return *this;
}
//< depth_disable

//> depth_enable
PipelineBuilder& PipelineBuilder::enable_depthtest(bool depthWriteEnable, VkCompareOp op)
{
    _depthStencil.depthTestEnable = VK_TRUE;
    _depthStencil.depthWriteEnable = depthWriteEnable;
    _depthStencil.depthCompareOp = op;
    _depthStencil.depthBoundsTestEnable = VK_FALSE;
    _depthStencil.stencilTestEnable = VK_FALSE;
    _depthStencil.front = {};
    _depthStencil.back = {};
    _depthStencil.minDepthBounds = 0.f;
    _depthStencil.maxDepthBounds = 1.f;
    return *this;
}
//< depth_enable

//> load_shader
bool vkutil::load_shader_module(const std::string& filePath,
    VkDevice device,
    VkShaderModule* outShaderModule)
{
    // open the file. With cursor at the end
    std::ifstream file(filePath, std::ios::ate | std::ios::binary);

    if (!file.is_open()) {
        return false;
    }

    // find what the size of the file is by looking up the location of the cursor
    // because the cursor is at the end, it gives the size directly in bytes
    size_t fileSize = (size_t)file.tellg();

    // spirv expects the buffer to be on uint32, so make sure to reserve a int
    // vector big enough for the entire file
    std::vector<uint32_t> buffer(fileSize / sizeof(uint32_t));

    // put file cursor at beginning
    file.seekg(0);

    // load the entire file into the buffer
    file.read((char*)buffer.data(), fileSize);

    // now that the file is loaded into the buffer, we can close it
    file.close();

    // create a new shader module, using the buffer we loaded
    VkShaderModuleCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.pNext = nullptr;

    // codeSize has to be in bytes, so multply the ints in the buffer by size of
    // int to know the real size of the buffer
    createInfo.codeSize = buffer.size() * sizeof(uint32_t);
    createInfo.pCode = buffer.data();

    // check that the creation goes well.
    VkShaderModule shaderModule;
    VK_ASSERT(vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule));
    *outShaderModule = shaderModule;
    return true;
}
//< load_shader