/*
Copyright (C) 2026, Q2RTX contributors.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

/*
	AI upscaler ("upscaler") implementation overview
	=================================================

	Marshals the TAA output image to an on-device TFLite model (QuickSRNetSmall)
	running on the Hexagon NPU via LiteRT's QNN dispatch backend, and unpacks the
	result back into a screen-resolution image for the final blit.

	Unlike FSR, the model doesn't run in-queue on the GPU: it runs on a separate
	piece of hardware (the NPU) driven by LiteRT on the CPU. To avoid a GPU -> CPU
	-> NPU copy, the pack/unpack compute shaders write/read directly into a pair
	of VkBuffers whose memory is shared with LiteRT's input/output tensors through
	Linux dma_buf file descriptors (VK_EXT_external_memory_dma_buf). The only
	synchronization needed is a single fence wait per frame between the pack
	dispatch and the (blocking) NPU inference call, since the dma_buf's kernel-level
	implicit fencing takes care of the rest.

	Q2RTX cvars
	-----------
	* flt_upscaler_enable - 0 = disable, 1 = enable the NPU upscaler.
*/

#include "shared/shared.h"
#include "vkpt.h"
#include "vk_util.h"
#include "system/system.h"

#include <string.h>

cvar_t *cvar_flt_upscaler_enable = NULL;

void vkpt_upscaler_init_cvars(void)
{
	cvar_flt_upscaler_enable = Cvar_Get("flt_upscaler_enable", "0", CVAR_ARCHIVE);
}

#ifdef USE_LITE_RT

#include "litert/c/litert_common.h"
#include "litert/c/litert_environment.h"
#include "litert/c/litert_environment_options.h"
#include "litert/c/litert_model.h"
#include "litert/c/litert_options.h"
#include "litert/c/litert_compiled_model.h"
#include "litert/c/litert_tensor_buffer.h"
#include "litert/c/litert_tensor_buffer_requirements.h"
#include "litert/c/litert_layout.h"

#define UPSCALER_MODEL_PATH "models/quicksrnetsmall-w8a8.tflite"
#define UPSCALER_SIGNATURE_INDEX 0
#define UPSCALER_INPUT_INDEX 0
#define UPSCALER_OUTPUT_INDEX 0

struct
{
	// LiteRT handles
	LiteRtEnvironment    env;
	LiteRtModel          model;
	LiteRtOptions        options;
	LiteRtCompiledModel  compiled_model;
	LiteRtTensorBuffer   input_tensor_buffer;
	LiteRtTensorBuffer   output_tensor_buffer;
	LiteRtRankedTensorType input_type;
	LiteRtRankedTensorType output_type;
	bool                 model_loaded;

	// Vulkan-side dma_buf resources backing the LiteRT tensor buffers
	VkBuffer             input_buffer;
	VkDeviceMemory       input_buffer_mem;
	VkDeviceSize         input_buffer_size;
	int                  input_buffer_fd;

	VkBuffer             output_buffer;
	VkDeviceMemory       output_buffer_mem;
	VkDeviceSize         output_buffer_size;
	int                  output_buffer_fd;

	// Pack/unpack compute pipelines
	VkPipeline           pipeline_pack;
	VkPipeline           pipeline_unpack;
	VkPipelineLayout     pipeline_layout;
	VkDescriptorSetLayout descriptor_set_layout;
	VkDescriptorPool     descriptor_pool;
	VkDescriptorSet      descriptor_set;

	// Dedicated fence for the pack-dispatch -> NPU inference handoff
	VkFence              fence;
} upscaler;

static bool create_litert_environment(void)
{
	char dispatch_dir[MAX_OSPATH];
	if (Q_concat(dispatch_dir, sizeof(dispatch_dir), sys_libdir->string) >= sizeof(dispatch_dir)) {
		Com_EPrintf("upscaler: dispatch library directory path too long\n");
		return false;
	}

	LiteRtEnvOption env_options[] = {
		{
			.tag = kLiteRtEnvOptionTagDispatchLibraryDir,
			.value = {
				.type = kLiteRtAnyTypeString,
				.str_value = dispatch_dir,
			},
		},
	};

	if (LiteRtCreateEnvironment(LENGTH(env_options), env_options, &upscaler.env) != kLiteRtStatusOk) {
		Com_EPrintf("upscaler: failed to create LiteRT environment\n");
		return false;
	}

	return true;
}

static bool load_model_and_query_tensors(void)
{
	char model_path[MAX_OSPATH];
	if (Q_concat(model_path, sizeof(model_path), fs_gamedir, PATH_SEP_STRING, UPSCALER_MODEL_PATH) >= sizeof(model_path)) {
		Com_EPrintf("upscaler: model path too long\n");
		return false;
	}

	if (LiteRtCreateModelFromFile(upscaler.env, model_path, &upscaler.model) != kLiteRtStatusOk) {
		Com_Printf("upscaler: could not load %s, NPU upscaler unavailable\n", model_path);
		return false;
	}

	LiteRtSignature signature;
	if (LiteRtGetModelSignature(upscaler.model, UPSCALER_SIGNATURE_INDEX, &signature) != kLiteRtStatusOk) {
		Com_EPrintf("upscaler: failed to get model signature\n");
		return false;
	}

	LiteRtTensor input_tensor, output_tensor;
	if (LiteRtGetSignatureInputTensorByIndex(signature, UPSCALER_INPUT_INDEX, &input_tensor) != kLiteRtStatusOk ||
		LiteRtGetSignatureOutputTensorByIndex(signature, UPSCALER_OUTPUT_INDEX, &output_tensor) != kLiteRtStatusOk) {
		Com_EPrintf("upscaler: failed to get model input/output tensors\n");
		return false;
	}

	if (LiteRtGetRankedTensorType(input_tensor, &upscaler.input_type) != kLiteRtStatusOk ||
		LiteRtGetRankedTensorType(output_tensor, &upscaler.output_type) != kLiteRtStatusOk) {
		Com_EPrintf("upscaler: failed to query model tensor shapes\n");
		return false;
	}

	return true;
}

static bool create_compiled_model(void)
{
	if (LiteRtCreateOptions(&upscaler.options) != kLiteRtStatusOk) {
		Com_EPrintf("upscaler: failed to create LiteRT options\n");
		return false;
	}

	LiteRtSetOptionsHardwareAccelerators(upscaler.options, kLiteRtHwAcceleratorNpu);

	if (LiteRtCreateCompiledModel(upscaler.env, upscaler.model, upscaler.options, &upscaler.compiled_model) != kLiteRtStatusOk) {
		Com_EPrintf("upscaler: failed to compile model for the NPU\n");
		return false;
	}

	bool fully_accelerated = false;
	if (LiteRtCompiledModelIsFullyAccelerated(upscaler.compiled_model, &fully_accelerated) == kLiteRtStatusOk && !fully_accelerated) {
		Com_WPrintf("upscaler: model is not fully accelerated on the NPU, some ops will fall back\n");
	}

	return true;
}

static size_t ranked_tensor_type_byte_size(const LiteRtRankedTensorType *type)
{
	size_t num_elements = 0;
	if (LiteRtGetNumLayoutElements(&type->layout, &num_elements) != kLiteRtStatusOk)
		return 0;

	size_t element_size;
	switch (type->element_type) {
	case kLiteRtElementTypeFloat32: element_size = 4; break;
	case kLiteRtElementTypeFloat16: element_size = 2; break;
	case kLiteRtElementTypeInt8:
	case kLiteRtElementTypeUInt8:   element_size = 1; break;
	default:                        element_size = 4; break;
	}

	return num_elements * element_size;
}

static bool create_tensor_buffers(void)
{
	upscaler.input_buffer_size = ranked_tensor_type_byte_size(&upscaler.input_type);
	upscaler.output_buffer_size = ranked_tensor_type_byte_size(&upscaler.output_type);

	if (upscaler.input_buffer_size == 0 || upscaler.output_buffer_size == 0) {
		Com_EPrintf("upscaler: could not determine tensor buffer sizes\n");
		return false;
	}

	if (create_buffer_dma_buf(upscaler.input_buffer_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
			&upscaler.input_buffer, &upscaler.input_buffer_mem) != VK_SUCCESS) {
		Com_EPrintf("upscaler: failed to create input dma_buf buffer\n");
		return false;
	}
	ATTACH_LABEL_VARIABLE_NAME(upscaler.input_buffer, BUFFER, "upscaler input tensor");

	if (get_memory_fd(upscaler.input_buffer_mem, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, &upscaler.input_buffer_fd) != VK_SUCCESS) {
		Com_EPrintf("upscaler: failed to export input buffer as dma_buf fd\n");
		return false;
	}

	// No deallocator: the fd is owned by the Vulkan buffer above, LiteRT only borrows it.
	if (LiteRtCreateTensorBufferFromDmaBufBuffer(&upscaler.input_type, NULL, upscaler.input_buffer_fd,
			upscaler.input_buffer_size, 0, NULL, &upscaler.input_tensor_buffer) != kLiteRtStatusOk) {
		Com_EPrintf("upscaler: failed to wrap input dma_buf fd as a LiteRT tensor buffer\n");
		return false;
	}

	if (create_buffer_dma_buf(upscaler.output_buffer_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
			&upscaler.output_buffer, &upscaler.output_buffer_mem) != VK_SUCCESS) {
		Com_EPrintf("upscaler: failed to create output dma_buf buffer\n");
		return false;
	}
	ATTACH_LABEL_VARIABLE_NAME(upscaler.output_buffer, BUFFER, "upscaler output tensor");

	if (get_memory_fd(upscaler.output_buffer_mem, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, &upscaler.output_buffer_fd) != VK_SUCCESS) {
		Com_EPrintf("upscaler: failed to export output buffer as dma_buf fd\n");
		return false;
	}

	if (LiteRtCreateTensorBufferFromDmaBufBuffer(&upscaler.output_type, NULL, upscaler.output_buffer_fd,
			upscaler.output_buffer_size, 0, NULL, &upscaler.output_tensor_buffer) != kLiteRtStatusOk) {
		Com_EPrintf("upscaler: failed to wrap output dma_buf fd as a LiteRT tensor buffer\n");
		return false;
	}

	return true;
}

static void destroy_litert(void)
{
	if (upscaler.input_tensor_buffer) {
		LiteRtDestroyTensorBuffer(upscaler.input_tensor_buffer);
		upscaler.input_tensor_buffer = NULL;
	}
	if (upscaler.output_tensor_buffer) {
		LiteRtDestroyTensorBuffer(upscaler.output_tensor_buffer);
		upscaler.output_tensor_buffer = NULL;
	}
	if (upscaler.compiled_model) {
		LiteRtDestroyCompiledModel(upscaler.compiled_model);
		upscaler.compiled_model = NULL;
	}
	if (upscaler.options) {
		LiteRtDestroyOptions(upscaler.options);
		upscaler.options = NULL;
	}
	if (upscaler.model) {
		LiteRtDestroyModel(upscaler.model);
		upscaler.model = NULL;
	}
	if (upscaler.env) {
		LiteRtDestroyEnvironment(upscaler.env);
		upscaler.env = NULL;
	}
}

VkResult vkpt_upscaler_initialize(void)
{
	memset(&upscaler, 0, sizeof(upscaler));
	upscaler.input_buffer_fd = -1;
	upscaler.output_buffer_fd = -1;

	if (!qvk.supports_dma_buf) {
		Com_Printf("upscaler: VK_EXT_external_memory_dma_buf not available, NPU upscaler disabled\n");
		return VK_SUCCESS;
	}

	if (!create_litert_environment())
		goto fail;

	if (!load_model_and_query_tensors())
		goto fail;

	if (!create_compiled_model())
		goto fail;

	if (!create_tensor_buffers())
		goto fail;

	VkFenceCreateInfo fence_info = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	_VK(vkCreateFence(qvk.device, &fence_info, NULL, &upscaler.fence));

	upscaler.model_loaded = true;
	return VK_SUCCESS;

fail:
	destroy_litert();
	return VK_SUCCESS;
}

VkResult vkpt_upscaler_destroy(void)
{
	if (upscaler.fence) {
		vkDestroyFence(qvk.device, upscaler.fence, NULL);
		upscaler.fence = NULL;
	}

	destroy_litert();

	if (upscaler.input_buffer) {
		vkDestroyBuffer(qvk.device, upscaler.input_buffer, NULL);
		upscaler.input_buffer = NULL;
	}
	if (upscaler.input_buffer_mem) {
		vkFreeMemory(qvk.device, upscaler.input_buffer_mem, NULL);
		upscaler.input_buffer_mem = NULL;
	}
	if (upscaler.output_buffer) {
		vkDestroyBuffer(qvk.device, upscaler.output_buffer, NULL);
		upscaler.output_buffer = NULL;
	}
	if (upscaler.output_buffer_mem) {
		vkFreeMemory(qvk.device, upscaler.output_buffer_mem, NULL);
		upscaler.output_buffer_mem = NULL;
	}

	upscaler.model_loaded = false;

	return VK_SUCCESS;
}

static void create_pipeline_layout(void)
{
	VkDescriptorSetLayoutBinding bindings[2] = { 0 };
	bindings[0].binding = 0;
	bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[0].descriptorCount = 1;
	bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

	bindings[1].binding = 1;
	bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[1].descriptorCount = 1;
	bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

	const VkDescriptorSetLayoutCreateInfo set_layout_create_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = LENGTH(bindings),
		.pBindings = bindings,
	};

	_VK(vkCreateDescriptorSetLayout(qvk.device, &set_layout_create_info, NULL, &upscaler.descriptor_set_layout));

	VkDescriptorSetLayout desc_set_layouts[] = {
		upscaler.descriptor_set_layout,
		qvk.desc_set_layout_ubo,
		qvk.desc_set_layout_textures,
	};

	CREATE_PIPELINE_LAYOUT(qvk.device, &upscaler.pipeline_layout,
		.setLayoutCount = LENGTH(desc_set_layouts),
		.pSetLayouts    = desc_set_layouts,
	);
	ATTACH_LABEL_VARIABLE(upscaler.pipeline_layout, PIPELINE_LAYOUT);
}

static void create_pipelines(void)
{
	const VkComputePipelineCreateInfo pipeline_infos[2] = {
		{
			.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			.stage = SHADER_STAGE(QVK_MOD_UPSCALER_PACK_COMP, VK_SHADER_STAGE_COMPUTE_BIT),
			.layout = upscaler.pipeline_layout,
		},
		{
			.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			.stage = SHADER_STAGE(QVK_MOD_UPSCALER_UNPACK_COMP, VK_SHADER_STAGE_COMPUTE_BIT),
			.layout = upscaler.pipeline_layout,
		},
	};

	VkPipeline pipelines[2];
	_VK(vkCreateComputePipelines(qvk.device, VK_NULL_HANDLE, LENGTH(pipeline_infos), pipeline_infos, NULL, pipelines));
	upscaler.pipeline_pack = pipelines[0];
	upscaler.pipeline_unpack = pipelines[1];
}

static void create_descriptor_set(void)
{
	const VkDescriptorPoolSize pool_sizes[] = {
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 },
	};

	const VkDescriptorPoolCreateInfo pool_create_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 1,
		.poolSizeCount = LENGTH(pool_sizes),
		.pPoolSizes = pool_sizes,
	};

	_VK(vkCreateDescriptorPool(qvk.device, &pool_create_info, NULL, &upscaler.descriptor_pool));

	const VkDescriptorSetAllocateInfo allocate_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = upscaler.descriptor_pool,
		.descriptorSetCount = 1,
		.pSetLayouts = &upscaler.descriptor_set_layout,
	};

	_VK(vkAllocateDescriptorSets(qvk.device, &allocate_info, &upscaler.descriptor_set));
}

static void update_descriptor_set(void)
{
	if (!upscaler.input_buffer || !upscaler.output_buffer)
		return;

	VkDescriptorBufferInfo input_info = {
		.buffer = upscaler.input_buffer,
		.offset = 0,
		.range  = upscaler.input_buffer_size,
	};

	VkDescriptorBufferInfo output_info = {
		.buffer = upscaler.output_buffer,
		.offset = 0,
		.range  = upscaler.output_buffer_size,
	};

	VkWriteDescriptorSet writes[2] = { 0 };
	writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[0].dstSet = upscaler.descriptor_set;
	writes[0].dstBinding = 0;
	writes[0].descriptorCount = 1;
	writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[0].pBufferInfo = &input_info;

	writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[1].dstSet = upscaler.descriptor_set;
	writes[1].dstBinding = 1;
	writes[1].descriptorCount = 1;
	writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[1].pBufferInfo = &output_info;

	vkUpdateDescriptorSets(qvk.device, LENGTH(writes), writes, 0, NULL);
}

VkResult vkpt_upscaler_create_pipelines(void)
{
	if (!upscaler.model_loaded)
		return VK_SUCCESS;

	create_pipeline_layout();
	create_pipelines();
	create_descriptor_set();
	update_descriptor_set();

	return VK_SUCCESS;
}

VkResult vkpt_upscaler_destroy_pipelines(void)
{
	if (upscaler.pipeline_pack) {
		vkDestroyPipeline(qvk.device, upscaler.pipeline_pack, NULL);
		upscaler.pipeline_pack = NULL;
	}
	if (upscaler.pipeline_unpack) {
		vkDestroyPipeline(qvk.device, upscaler.pipeline_unpack, NULL);
		upscaler.pipeline_unpack = NULL;
	}
	if (upscaler.pipeline_layout) {
		vkDestroyPipelineLayout(qvk.device, upscaler.pipeline_layout, NULL);
		upscaler.pipeline_layout = NULL;
	}
	if (upscaler.descriptor_pool) {
		vkDestroyDescriptorPool(qvk.device, upscaler.descriptor_pool, NULL);
		upscaler.descriptor_pool = NULL;
	}
	if (upscaler.descriptor_set_layout) {
		vkDestroyDescriptorSetLayout(qvk.device, upscaler.descriptor_set_layout, NULL);
		upscaler.descriptor_set_layout = NULL;
	}

	return VK_SUCCESS;
}

bool vkpt_upscaler_is_enabled(void)
{
	if (!upscaler.model_loaded)
		return false;

	if (cvar_flt_upscaler_enable->integer == 0)
		return false;

	return true;
}

#define BARRIER_COMPUTE_IMG(cmd_buf, img) \
	do { \
		VkImageSubresourceRange subresource_range = { \
			.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT, \
			.baseMipLevel   = 0, \
			.levelCount     = 1, \
			.baseArrayLayer = 0, \
			.layerCount     = 1 \
		}; \
		IMAGE_BARRIER(cmd_buf, \
				.image            = img, \
				.subresourceRange = subresource_range, \
				.srcAccessMask    = VK_ACCESS_SHADER_WRITE_BIT, \
				.dstAccessMask    = VK_ACCESS_SHADER_READ_BIT, \
				.oldLayout        = VK_IMAGE_LAYOUT_GENERAL, \
				.newLayout        = VK_IMAGE_LAYOUT_GENERAL, \
		); \
	} while(0)

// Note: the incoming cmd_buf (the caller's shared post-processing command buffer,
// e.g. main.c's post_cmd_buf) is intentionally not used here. Unlike FSR, this pass
// needs to submit and block on a fence mid-frame (see below), which the shared
// buffer's owner does not expect -- it keeps recording into and submitting that
// buffer itself later in the frame. So pack/unpack each get their own dedicated
// command buffer and submission.
VkResult vkpt_upscaler_do(VkCommandBuffer cmd_buf)
{
	VkDescriptorSet desc_sets[] = {
		upscaler.descriptor_set,
		qvk.desc_set_ubo,
		qvk_get_current_desc_set_textures(),
	};

	VkCommandBuffer pack_cmd_buf = vkpt_begin_command_buffer(&qvk.cmd_buffers_graphics);

	BEGIN_PERF_MARKER(pack_cmd_buf, PROFILER_UPSCALER);
	BEGIN_PERF_MARKER(pack_cmd_buf, PROFILER_UPSCALER_PACK);

	// Pack: read VKPT_IMG_TAA_OUTPUT, resample/quantize into the input tensor buffer.
	vkCmdBindPipeline(pack_cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, upscaler.pipeline_pack);
	vkCmdBindDescriptorSets(pack_cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE,
		upscaler.pipeline_layout, 0, LENGTH(desc_sets), desc_sets, 0, 0);

	uint32_t in_w = upscaler.input_type.layout.dimensions[2];
	uint32_t in_h = upscaler.input_type.layout.dimensions[1];
	vkCmdDispatch(pack_cmd_buf, (in_w + 15) / 16, (in_h + 15) / 16, 1);

	VkBufferMemoryBarrier pack_barrier = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_HOST_READ_BIT,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.buffer = upscaler.input_buffer,
		.size = upscaler.input_buffer_size,
	};
	vkCmdPipelineBarrier(pack_cmd_buf, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
		0, 0, NULL, 1, &pack_barrier, 0, NULL);

	END_PERF_MARKER(pack_cmd_buf, PROFILER_UPSCALER_PACK);

	// Submit and wait: the NPU inference call below reads the input buffer
	// through the dma_buf fd, so the pack dispatch must have completed on the GPU.
	_VK(vkResetFences(qvk.device, 1, &upscaler.fence));
	vkpt_submit_command_buffer(pack_cmd_buf, qvk.queue_graphics, ALL_GPUS,
		0, NULL, NULL, NULL, 0, NULL, NULL, upscaler.fence);
	_VK(vkWaitForFences(qvk.device, 1, &upscaler.fence, VK_TRUE, UINT64_MAX));

	LiteRtStatus run_status = LiteRtRunCompiledModel(upscaler.compiled_model, UPSCALER_SIGNATURE_INDEX,
		1, &upscaler.input_tensor_buffer, 1, &upscaler.output_tensor_buffer);
	if (run_status != kLiteRtStatusOk)
		Com_EPrintf("upscaler: NPU inference failed (%d)\n", run_status);

	VkCommandBuffer unpack_cmd_buf = vkpt_begin_command_buffer(&qvk.cmd_buffers_graphics);

	BEGIN_PERF_MARKER(unpack_cmd_buf, PROFILER_UPSCALER_UNPACK);

	vkCmdBindPipeline(unpack_cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, upscaler.pipeline_unpack);
	vkCmdBindDescriptorSets(unpack_cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE,
		upscaler.pipeline_layout, 0, LENGTH(desc_sets), desc_sets, 0, 0);

	VkBufferMemoryBarrier unpack_barrier = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.buffer = upscaler.output_buffer,
		.size = upscaler.output_buffer_size,
	};
	vkCmdPipelineBarrier(unpack_cmd_buf, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		0, 0, NULL, 1, &unpack_barrier, 0, NULL);

	VkExtent2D dispatch_size = qvk.extent_unscaled;
	vkCmdDispatch(unpack_cmd_buf, (dispatch_size.width + 15) / 16, (dispatch_size.height + 15) / 16, 1);
	BARRIER_COMPUTE_IMG(unpack_cmd_buf, qvk.images[VKPT_IMG_UPSCALE_OUTPUT]);

	END_PERF_MARKER(unpack_cmd_buf, PROFILER_UPSCALER_UNPACK);
	END_PERF_MARKER(unpack_cmd_buf, PROFILER_UPSCALER);

	vkpt_submit_command_buffer_simple(unpack_cmd_buf, qvk.queue_graphics, true);

	return VK_SUCCESS;
}

VkResult vkpt_upscaler_final_blit(VkCommandBuffer cmd_buf, bool warp)
{
	return vkpt_final_blit(cmd_buf, VKPT_IMG_UPSCALE_OUTPUT, qvk.extent_unscaled, false, warp);
}

#else // !USE_LITE_RT

VkResult vkpt_upscaler_initialize(void)
{
	return VK_SUCCESS;
}

VkResult vkpt_upscaler_destroy(void)
{
	return VK_SUCCESS;
}

VkResult vkpt_upscaler_create_pipelines(void)
{
	return VK_SUCCESS;
}

VkResult vkpt_upscaler_destroy_pipelines(void)
{
	return VK_SUCCESS;
}

bool vkpt_upscaler_is_enabled(void)
{
	return false;
}

VkResult vkpt_upscaler_do(VkCommandBuffer cmd_buf)
{
	return VK_SUCCESS;
}

VkResult vkpt_upscaler_final_blit(VkCommandBuffer cmd_buf, bool warp)
{
	return VK_SUCCESS;
}

#endif // USE_LITE_RT

// vim: shiftwidth=4 noexpandtab tabstop=4 cindent
