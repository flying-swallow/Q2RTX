#include "vkpt.h"

enum {
	NRD_PIPELINE_PREPARE,
    NRD_PIPELINE_CONFIDENCE_FILTER,
	NRD_PIPELINE_COMPOSITE,
	NRD_NUM_PIPELINES
};

static VkPipeline nrd_pipelines[NRD_NUM_PIPELINES];
static VkPipelineLayout nrd_pipeline_layout;

VkResult vkpt_nrd_pipeline_initialize(void)
{
	VkDescriptorSetLayout descriptor_set_layouts[] = {
		qvk.desc_set_layout_ubo,
		qvk.desc_set_layout_textures,
		qvk.desc_set_layout_vertex_buffer
	};

    VkPushConstantRange push_range = { VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(int) };
	CREATE_PIPELINE_LAYOUT(qvk.device, &nrd_pipeline_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_range,
		.setLayoutCount = LENGTH(descriptor_set_layouts),
		.pSetLayouts = descriptor_set_layouts);
	return VK_SUCCESS;
}

VkResult vkpt_nrd_pipeline_destroy(void)
{
	if (nrd_pipeline_layout != VK_NULL_HANDLE)
		vkDestroyPipelineLayout(qvk.device, nrd_pipeline_layout, NULL);
	nrd_pipeline_layout = VK_NULL_HANDLE;
	return VK_SUCCESS;
}

VkResult vkpt_nrd_create_pipelines(void)
{
	VkComputePipelineCreateInfo create_info[NRD_NUM_PIPELINES] = {
		{
			.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			.stage = SHADER_STAGE(QVK_MOD_NRD_PREPARE_COMP, VK_SHADER_STAGE_COMPUTE_BIT),
			.layout = nrd_pipeline_layout
		},
		{
			.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = SHADER_STAGE(QVK_MOD_NRD_CONFIDENCE_FILTER_COMP, VK_SHADER_STAGE_COMPUTE_BIT),
            .layout = nrd_pipeline_layout
        },
        {
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			.stage = SHADER_STAGE(QVK_MOD_NRD_COMPOSITE_COMP, VK_SHADER_STAGE_COMPUTE_BIT),
			.layout = nrd_pipeline_layout
		}
	};

	_VK(vkCreateComputePipelines(qvk.device, VK_NULL_HANDLE, NRD_NUM_PIPELINES,
		create_info, NULL, nrd_pipelines));
	return VK_SUCCESS;
}

VkResult vkpt_nrd_destroy_pipelines(void)
{
	for (int i = 0; i < NRD_NUM_PIPELINES; i++) {
		if (nrd_pipelines[i] != VK_NULL_HANDLE)
			vkDestroyPipeline(qvk.device, nrd_pipelines[i], NULL);
		nrd_pipelines[i] = VK_NULL_HANDLE;
	}
	return VK_SUCCESS;
}

// NRD images remain in GENERAL.  The write destination is included because
// the prepare pass both consumes and produces resources in the same set.
#define NRD_BARRIER_COMPUTE(cmd_buf, image_index) \
	do { \
		IMAGE_BARRIER(cmd_buf, \
			.image = qvk.images[image_index], \
			.subresourceRange = (VkImageSubresourceRange){ \
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, \
				.levelCount = 1, .baseArrayLayer = 0, .layerCount = 1 }, \
			.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, \
			.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, \
			.oldLayout = VK_IMAGE_LAYOUT_GENERAL, .newLayout = VK_IMAGE_LAYOUT_GENERAL); \
	} while (0)

static void nrd_bind_and_dispatch(VkCommandBuffer cmd_buf, int pipeline)
{
	VkDescriptorSet descriptor_sets[] = {
		qvk.desc_set_ubo,
		qvk_get_current_desc_set_textures(),
		qvk.desc_set_vertex_buffer
	};

	vkCmdBindPipeline(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE,
		nrd_pipelines[pipeline]);
	vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE,
		nrd_pipeline_layout, 0, LENGTH(descriptor_sets), descriptor_sets, 0, NULL);
    uint32_t width = qvk.extent_render.width;
    uint32_t height = qvk.extent_render.height;
    // Confidence inputs use the entire texture as normalized screen space.
    if (pipeline == NRD_PIPELINE_PREPARE) {
        width = qvk.extent_screen_images.width;
        height = qvk.extent_screen_images.height;
    }
    if (pipeline == NRD_PIPELINE_CONFIDENCE_FILTER) {
        width = (width + 4) / 5;
        height = (height + 4) / 5;
    }
    vkCmdDispatch(cmd_buf, (width + 15) / 16, (height + 15) / 16, 1);
}

VkResult vkpt_nrd_filter_confidence(VkCommandBuffer cmd_buf)
{
    for (int step = 1; step <= 5; step++) {
        NRD_BARRIER_COMPUTE(cmd_buf, VKPT_IMG_NRD_CONFIDENCE_GRADIENT_0);
        NRD_BARRIER_COMPUTE(cmd_buf, VKPT_IMG_NRD_CONFIDENCE_GRADIENT_1);
        vkCmdPushConstants(cmd_buf, nrd_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(step), &step);
        nrd_bind_and_dispatch(cmd_buf, NRD_PIPELINE_CONFIDENCE_FILTER);
    }
    NRD_BARRIER_COMPUTE(cmd_buf, VKPT_IMG_NRD_CONFIDENCE_GRADIENT_1);
    return VK_SUCCESS;
}

VkResult vkpt_nrd_prepare(VkCommandBuffer cmd_buf)
{
	// These inputs are not ping-ponged and remain at fixed physical indices.
	int raw_fixed_inputs[] = {
		VKPT_IMG_PT_MOTION, VKPT_IMG_PT_TRANSPARENT,
		VKPT_IMG_PT_SHADING_POSITION, VKPT_IMG_PT_VIEW_DIRECTION,
		VKPT_IMG_PT_THROUGHPUT, VKPT_IMG_PT_COLOR_LF_SH,
		VKPT_IMG_PT_COLOR_LF_COCG, VKPT_IMG_PT_COLOR_HF,
		VKPT_IMG_PT_COLOR_SPEC, VKPT_IMG_PT_NRD_HITDIST, VKPT_IMG_PT_SPEC_MOMENT
	};
	for (unsigned i = 0; i < LENGTH(raw_fixed_inputs); i++)
		NRD_BARRIER_COMPUTE(cmd_buf, raw_fixed_inputs[i]);

	// These checkerboard inputs are bound to physical A/B according to the
	// current frame's descriptor-set parity.
	int raw_ping_ponged_inputs[] = {
		VKPT_IMG_PT_VIEW_DEPTH_A, VKPT_IMG_PT_NORMAL_A,
		VKPT_IMG_PT_BASE_COLOR_A, VKPT_IMG_PT_METALLIC_A
	};
	int frame_index = qvk.frame_counter & 1;
	for (unsigned i = 0; i < LENGTH(raw_ping_ponged_inputs); i++)
		NRD_BARRIER_COMPUTE(cmd_buf, raw_ping_ponged_inputs[i] + frame_index);

	nrd_bind_and_dispatch(cmd_buf, NRD_PIPELINE_PREPARE);

	int nrd_inputs[] = {
		VKPT_IMG_NRD_MV, VKPT_IMG_NRD_NORMAL_ROUGHNESS, VKPT_IMG_NRD_VIEWZ,
		VKPT_IMG_NRD_DIRECT_DIFF_RADIANCE_HITDIST_IN,
		VKPT_IMG_NRD_INDIRECT_DIFF_RADIANCE_HITDIST_IN,
		VKPT_IMG_NRD_SPEC_RADIANCE_HITDIST_IN, VKPT_IMG_NRD_SPEC_SH1_IN,
        VKPT_IMG_NRD_SPEC_SH1_OUT, VKPT_IMG_NRD_SPEC_CONFIDENCE, VKPT_IMG_NRD_DIFF_CONFIDENCE
	};
	for (unsigned i = 0; i < LENGTH(nrd_inputs); i++)
		NRD_BARRIER_COMPUTE(cmd_buf, nrd_inputs[i]);
	return VK_SUCCESS;
}

VkResult vkpt_nrd_composite(VkCommandBuffer cmd_buf)
{
	int nrd_resources[] = {
		VKPT_IMG_NRD_MV, VKPT_IMG_NRD_NORMAL_ROUGHNESS, VKPT_IMG_NRD_VIEWZ,
		VKPT_IMG_NRD_DIRECT_DIFF_RADIANCE_HITDIST_IN,
		VKPT_IMG_NRD_INDIRECT_DIFF_RADIANCE_HITDIST_IN,
		VKPT_IMG_NRD_SPEC_RADIANCE_HITDIST_IN,
		VKPT_IMG_NRD_DIRECT_DIFF_RADIANCE_HITDIST_OUT,
		VKPT_IMG_NRD_INDIRECT_DIFF_RADIANCE_HITDIST_OUT,
		VKPT_IMG_NRD_SPEC_RADIANCE_HITDIST_OUT, VKPT_IMG_NRD_SPEC_SH1_OUT, VKPT_IMG_NRD_SPEC_CONFIDENCE
	};
	for (unsigned i = 0; i < LENGTH(nrd_resources); i++)
		NRD_BARRIER_COMPUTE(cmd_buf, nrd_resources[i]);

	nrd_bind_and_dispatch(cmd_buf, NRD_PIPELINE_COMPOSITE);
	NRD_BARRIER_COMPUTE(cmd_buf, VKPT_IMG_ASVGF_COLOR);
	return VK_SUCCESS;
}
