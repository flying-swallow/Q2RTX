/* C-linkage Q2RTX-facing FSR3 module. The Vulkan callbacks and shader blobs
 * live in fsr3_backend_vk.cpp; this file owns the SDK context and history. */
#include "vkpt.h"

#include "../../../extern/FidelityFX-SDK/Kits/FidelityFX/api/include/ffx_api_types.h"
/* The public FSR3 header currently includes C++-only backend internals, so
 * retain the C ABI declarations here. These layouts/signatures mirror
 * upscalers/fsr3/include/ffx_fsr3upscaler.h and api/internal/ffx_interface.h. */
typedef enum FfxApiSurfaceFormat FfxApiSurfaceFormat;
typedef enum FfxApiResourceUsage FfxApiResourceUsage;
typedef enum FfxApiResourceState FfxApiResourceState;
typedef enum FfxApiResourceType FfxApiResourceType;
typedef struct FfxApiDimensions2D FfxApiDimensions2D;
typedef struct FfxApiFloatCoords2D FfxApiFloatCoords2D;
typedef struct FfxApiResourceDescription FfxApiResourceDescription;
typedef struct FfxApiResource FfxApiResource;
typedef void *FfxDevice;
typedef void *FfxCommandList;
typedef int32_t FfxErrorCode;
typedef int32_t FfxApiMsgType;
typedef struct FfxResourceInternal { int32_t internalIndex; } FfxResourceInternal;
typedef struct FfxResourceInitData { int32_t type; size_t size; union { const void *buffer; unsigned char value; }; } FfxResourceInitData;
typedef struct FfxResourceHeapPlacementInfo { int32_t heapType; bool usePlacementHeap; void *placementHeap; uint64_t placementHeapOffset; } FfxResourceHeapPlacementInfo;
typedef struct FfxCreateResourceDescription { FfxResourceHeapPlacementInfo heapInfo; FfxApiResourceDescription resourceDescription; FfxApiResourceState initialState; const wchar_t *name; uint32_t id; FfxResourceInitData initData; } FfxCreateResourceDescription;
typedef struct FfxInterface { void *callbacks[24]; void *scratchBuffer; size_t scratchBufferSize; FfxDevice device; } FfxInterface;
typedef struct FfxFsr3UpscalerContext { uint32_t data[256 * 1024]; } FfxFsr3UpscalerContext;
typedef void (*FfxFsr3UpscalerMessage)(FfxApiMsgType, const wchar_t *);
typedef struct FfxFsr3UpscalerContextDescription { uint32_t flags; FfxApiDimensions2D maxRenderSize, maxUpscaleSize; FfxFsr3UpscalerMessage fpMessage; FfxInterface backendInterface; } FfxFsr3UpscalerContextDescription;
typedef struct FfxFsr3UpscalerSharedResourceDescriptions { FfxCreateResourceDescription reconstructedPrevNearestDepth, dilatedDepth, dilatedMotionVectors; } FfxFsr3UpscalerSharedResourceDescriptions;
typedef struct FfxFsr3UpscalerDispatchDescription { FfxCommandList commandList; FfxApiResource color, depth, motionVectors, exposure, reactive, transparencyAndComposition; FfxApiResource dilatedDepth, dilatedMotionVectors, reconstructedPrevNearestDepth, output; FfxApiFloatCoords2D jitterOffset, motionVectorScale; FfxApiDimensions2D renderSize, upscaleSize; bool enableSharpening; float sharpness, frameTimeDelta, preExposure; bool reset; float cameraNear, cameraFar, cameraFovAngleVertical, viewSpaceToMetersFactor; uint32_t flags; } FfxFsr3UpscalerDispatchDescription;

extern FfxErrorCode ffxFsr3UpscalerContextCreate(FfxFsr3UpscalerContext *, const FfxFsr3UpscalerContextDescription *);
extern FfxErrorCode ffxFsr3UpscalerGetSharedResourceDescriptions(FfxFsr3UpscalerContext *, FfxFsr3UpscalerSharedResourceDescriptions *);
/* fsr3_backend_vk.h; declared here because that header pulls in the SDK's C++-only interface header. */
extern void fsr3_vkpt_begin_frame(FfxInterface *backend, uint32_t frame_index);
extern FfxErrorCode ffxFsr3UpscalerContextDispatch(FfxFsr3UpscalerContext *, const FfxFsr3UpscalerDispatchDescription *);
extern FfxErrorCode ffxFsr3UpscalerContextDestroy(FfxFsr3UpscalerContext *);
extern int32_t ffxFsr3UpscalerGetJitterPhaseCount(int32_t renderWidth, int32_t displayWidth);
extern FfxErrorCode ffxFsr3UpscalerGetJitterOffset(float *, float *, int32_t index, int32_t phaseCount);

enum {
	FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE = (1 << 0),
	FFX_FSR3UPSCALER_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS = (1 << 1),
	FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE = (1 << 5),
	FFX_FSR3UPSCALER_ENABLE_DYNAMIC_RESOLUTION = (1 << 6),
	FFX_FSR3UPSCALER_ENABLE_DEBUG_CHECKING = (1 << 8)
};
#define FFX_OK 0

/* The SDK message helpers are only diagnostics. Keep the renderer portable
 * without pulling the SDK's platform-specific debugger implementation into
 * the client target. */
void ffxPrintMessage(uint32_t type, const wchar_t *message) { (void)type; (void)message; }
void ffxSetPrintMessageCallback(void (*callback)(uint32_t, const wchar_t *), uint32_t level)
{ (void)callback; (void)level; }

extern bool fsr3_vkpt_is_available(void);
extern FfxErrorCode fsr3_vkpt_create_interface(FfxInterface *outInterface);
extern void fsr3_vkpt_destroy_interface(FfxInterface *interface);
extern FfxApiSurfaceFormat fsr3_vkpt_format_to_ffx(VkFormat format);
extern FfxApiResource fsr3_vkpt_wrap_image(VkImage, const FfxApiResourceDescription *, FfxApiResourceState);
/* The frame-graph worker may provide a dedicated FSR3 output image. Until
 * then, keep this C module buildable against the current image table. */
static FfxInterface fsr3_backend;

static FfxFsr3UpscalerContext fsr3_context;
static FfxResourceInternal fsr3_shared[3];
static bool fsr3_initialized, fsr3_context_valid, fsr3_history_invalid;
static VkExtent2D fsr3_max_render_size, fsr3_output_size;

cvar_t *cvar_flt_fsr_enable, *cvar_flt_fsr_sharpness;

static FfxApiResourceDescription image_desc(VkFormat format, VkExtent2D size, FfxApiResourceUsage usage)
{
	FfxApiResourceDescription d = {0};
	d.type = FFX_API_RESOURCE_TYPE_TEXTURE2D; d.format = fsr3_vkpt_format_to_ffx(format);
	d.width = size.width; d.height = size.height; d.depth = 1; d.mipCount = 1; d.usage = usage;
	return d;
}

// Resource dimensions describe the VkImage allocation, not the active render
// rectangle. FSR uses them to normalize texture coordinates during upscaling.
static VkExtent2D image_extent(int image)
{
    switch (image) {
#define IMG_DO(name, binding, format, glslformat, width, height) \
    case VKPT_IMG_##name: return (VkExtent2D){width, height};
        LIST_IMAGES
        LIST_IMAGES_A_B
#undef IMG_DO
    default: return (VkExtent2D){0, 0};
    }
}

static FfxApiResource wrap_image(int image, VkFormat format,
	FfxApiResourceState state, FfxApiResourceUsage usage)
{
	FfxApiResourceDescription d = image_desc(format, image_extent(image), usage);
	return fsr3_vkpt_wrap_image(qvk.images[image], &d, state);
}

static cvar_t *cvar_flt_fsr_debug = NULL;

static void fsr3_message(FfxApiMsgType type, const wchar_t *message)
{
	(void)type;
	if (!message) return;
	char narrow[256];
	size_t n = wcstombs(narrow, message, sizeof(narrow) - 1);
	narrow[n == (size_t)-1 ? 0 : n] = 0;
	Com_WPrintf("FSR3 SDK: %s\n", narrow);
}

void fsr3_vkpt_log(const char *message)
{
	Com_WPrintf("%s\n", message ? message : "(null)");
}

/* True when the user asked for FSR3, whether or not the context exists yet.
 * Image allocation sizes must not depend on the context, which is destroyed
 * before the extents are re-evaluated during a swapchain recreate. */
bool vkpt_fsr3_is_requested(void)
{
	return cvar_flt_fsr_enable && cvar_flt_fsr_enable->integer;
}

static void release_shared(void)
{
	if (!fsr3_context_valid) return;
	for (unsigned i = 0; i < LENGTH(fsr3_shared); ++i) {
		if (fsr3_shared[i].internalIndex >= 0)
		((FfxErrorCode (*)(FfxInterface *, FfxResourceInternal, uint32_t))fsr3_backend.callbacks[11])(&fsr3_backend, fsr3_shared[i], 0);
		fsr3_shared[i].internalIndex = -1;
	}
}

static VkResult create_context(void)
{
	FfxFsr3UpscalerContextDescription d = {0};
	d.flags = FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE |
		FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE |
		FFX_FSR3UPSCALER_ENABLE_DYNAMIC_RESOLUTION;
	/* Do not set DISPLAY_RESOLUTION_MOTION_VECTORS: Q2RTX supplies render-
	 * resolution motion vectors. Debug checking is deliberately validation-only. */
	if (qvk.enable_validation)
		d.flags |= FFX_FSR3UPSCALER_ENABLE_DEBUG_CHECKING;
	d.maxRenderSize = (FfxApiDimensions2D){qvk.extent_screen_images.width, qvk.extent_screen_images.height};
	d.maxUpscaleSize = (FfxApiDimensions2D){qvk.extent_unscaled.width, qvk.extent_unscaled.height};
	d.fpMessage = fsr3_message; d.backendInterface = fsr3_backend;
	FfxErrorCode ffx_result = ffxFsr3UpscalerContextCreate(&fsr3_context, &d);
	if (ffx_result != FFX_OK) {
		Com_WPrintf("FSR3: ffxFsr3UpscalerContextCreate failed (FfxErrorCode 0x%08x), max render %ux%u upscale %ux%u\n",
			(unsigned)ffx_result, d.maxRenderSize.width, d.maxRenderSize.height, d.maxUpscaleSize.width, d.maxUpscaleSize.height);
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	fsr3_context_valid = true;
	FfxFsr3UpscalerSharedResourceDescriptions s = {0};
	ffx_result = ffxFsr3UpscalerGetSharedResourceDescriptions(&fsr3_context, &s);
	if (ffx_result != FFX_OK) {
		Com_WPrintf("FSR3: ffxFsr3UpscalerGetSharedResourceDescriptions failed (FfxErrorCode 0x%08x)\n", (unsigned)ffx_result);
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	const FfxCreateResourceDescription *desc[3] = {&s.reconstructedPrevNearestDepth, &s.dilatedDepth, &s.dilatedMotionVectors};
	for (unsigned i = 0; i < 3; ++i) {
		fsr3_shared[i].internalIndex = -1;
		ffx_result = ((FfxErrorCode (*)(FfxInterface *, const FfxCreateResourceDescription *, uint32_t, FfxResourceInternal *))fsr3_backend.callbacks[5])(&fsr3_backend, desc[i], 0, &fsr3_shared[i]);
		if (ffx_result != FFX_OK) {
			Com_WPrintf("FSR3: shared resource %u creation failed (FfxErrorCode 0x%08x)\n", i, (unsigned)ffx_result);
			return VK_ERROR_OUT_OF_DEVICE_MEMORY;
		}
	}
	fsr3_max_render_size = qvk.extent_screen_images; fsr3_output_size = qvk.extent_unscaled;
	fsr3_history_invalid = true;
	return VK_SUCCESS;
}

void vkpt_fsr3_init_cvars(void)
{
	cvar_flt_fsr_enable = Cvar_Get("flt_fsr_enable", "0", CVAR_ARCHIVE);
	/* Preserve existing configs; FSR3 combines upscale and sharpening itself. */
	cvar_flt_fsr_sharpness = Cvar_Get("flt_fsr_sharpness", "0.2", CVAR_ARCHIVE);
	cvar_flt_fsr_debug = Cvar_Get("flt_fsr_debug", "0", 0);
}

VkResult vkpt_fsr3_initialize(void)
{
	if (!fsr3_vkpt_is_available()) return VK_SUCCESS;
	/* The FSR3 shaders declare formatless storage images, which need the same
	 * device features the NRD path enables. */
	if (!qvk.supports_nrd_device_extensions) {
		Com_WPrintf("FSR3: device lacks formatless storage image support; upscaling disabled\n");
		return VK_SUCCESS;
	}
	if (fsr3_vkpt_create_interface(&fsr3_backend) != FFX_OK) return VK_SUCCESS;
	fsr3_initialized = true;
	VkResult context_result = create_context();
	if (context_result != VK_SUCCESS) {
		Com_WPrintf("FSR3: context creation failed (VkResult %d); upscaling disabled\n", (int)context_result);
		if (fsr3_context_valid) {
			vkDeviceWaitIdle(qvk.device);
			release_shared();
			ffxFsr3UpscalerContextDestroy(&fsr3_context);
			fsr3_context_valid = false;
		}
		fsr3_vkpt_destroy_interface(&fsr3_backend); fsr3_initialized = false;
	}
	return VK_SUCCESS;
}

VkResult vkpt_fsr3_recreate(void)
{
	VkResult result = vkpt_fsr3_destroy();
	if (result != VK_SUCCESS) return result;
	return vkpt_fsr3_initialize();
}

VkResult vkpt_fsr3_destroy(void)
{
	if (fsr3_context_valid) {
		/* The SDK context owns resources and pipelines used by queued dispatches. */
		vkDeviceWaitIdle(qvk.device);
		release_shared();
		ffxFsr3UpscalerContextDestroy(&fsr3_context);
		fsr3_context_valid = false;
	}
	if (fsr3_initialized) { fsr3_vkpt_destroy_interface(&fsr3_backend); fsr3_initialized = false; }
	return VK_SUCCESS;
}

/* FSR3 creates pipelines as part of context creation. */
bool vkpt_fsr3_is_enabled(void) { return cvar_flt_fsr_enable && cvar_flt_fsr_enable->integer && fsr3_context_valid; }
void vkpt_fsr3_reset(void) { fsr3_history_invalid = true; }

static VkResult vkpt_fsr3_do(VkCommandBuffer cmd_buf, float frame_time)
{
	if (!vkpt_fsr3_is_enabled()) return VK_SUCCESS;
	/* DRS changes the active rectangle within the existing allocation. Only
	 * growth beyond the context capacity requires recreating its resources. */
	if (fsr3_max_render_size.width < qvk.extent_render.width || fsr3_max_render_size.height < qvk.extent_render.height ||
		fsr3_max_render_size.width != qvk.extent_screen_images.width || fsr3_max_render_size.height != qvk.extent_screen_images.height ||
		fsr3_output_size.width != qvk.extent_unscaled.width || fsr3_output_size.height != qvk.extent_unscaled.height) return VK_ERROR_OUT_OF_DATE_KHR;
	fsr3_vkpt_begin_frame(&fsr3_backend, (uint32_t)(qvk.frame_counter % 4));
	/* checkerboard_interleave writes the FSR3 adapter images immediately before
	 * this dispatch. Make those writes visible even though the SDK owns the
	 * subsequent resource transitions for its internal images. */
	VkMemoryBarrier input_barrier = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
	};
	vkCmdPipelineBarrier(cmd_buf, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &input_barrier, 0, NULL, 0, NULL);
	FfxFsr3UpscalerDispatchDescription d = {0};
	d.commandList = (FfxCommandList)cmd_buf;
	d.color = wrap_image(VKPT_IMG_FLAT_COLOR, VK_FORMAT_R16G16B16A16_SFLOAT, FFX_API_RESOURCE_STATE_COMPUTE_READ, FFX_API_RESOURCE_USAGE_READ_ONLY);
	d.depth = wrap_image(VKPT_IMG_FSR3_DEPTH, VK_FORMAT_R32_SFLOAT, FFX_API_RESOURCE_STATE_COMPUTE_READ, FFX_API_RESOURCE_USAGE_READ_ONLY | FFX_API_RESOURCE_USAGE_DEPTHTARGET);
	d.motionVectors = wrap_image(VKPT_IMG_FLAT_MOTION, VK_FORMAT_R16G16B16A16_SFLOAT, FFX_API_RESOURCE_STATE_COMPUTE_READ, FFX_API_RESOURCE_USAGE_READ_ONLY);
	d.reactive = wrap_image(VKPT_IMG_FSR3_REACTIVE_MASK, VK_FORMAT_R8_UNORM, FFX_API_RESOURCE_STATE_COMPUTE_READ, FFX_API_RESOURCE_USAGE_READ_ONLY);
	d.transparencyAndComposition = wrap_image(VKPT_IMG_FSR3_TRANSPARENCY_MASK, VK_FORMAT_R8_UNORM, FFX_API_RESOURCE_STATE_COMPUTE_READ, FFX_API_RESOURCE_USAGE_READ_ONLY);
	d.output = wrap_image(VKPT_IMG_TAA_OUTPUT, VK_FORMAT_R16G16B16A16_SFLOAT, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, FFX_API_RESOURCE_USAGE_UAV);
	d.dilatedDepth = ((FfxApiResource (*)(FfxInterface *, FfxResourceInternal))fsr3_backend.callbacks[7])(&fsr3_backend, fsr3_shared[1]);
	d.dilatedMotionVectors = ((FfxApiResource (*)(FfxInterface *, FfxResourceInternal))fsr3_backend.callbacks[7])(&fsr3_backend, fsr3_shared[2]);
	d.reconstructedPrevNearestDepth = ((FfxApiResource (*)(FfxInterface *, FfxResourceInternal))fsr3_backend.callbacks[7])(&fsr3_backend, fsr3_shared[0]);
	d.renderSize = (FfxApiDimensions2D){qvk.extent_render.width, qvk.extent_render.height};
	d.upscaleSize = (FfxApiDimensions2D){qvk.extent_unscaled.width, qvk.extent_unscaled.height};
	d.enableSharpening = true;
	/* Keep the existing cvar's name and meaning, but use FSR3's range and
	 * direction: zero adds no sharpening and one adds the maximum. */
	d.sharpness = cvar_flt_fsr_sharpness->value;
	if (d.sharpness < 0.0f) d.sharpness = 0.0f;
	if (d.sharpness > 1.0f) d.sharpness = 1.0f;
	d.frameTimeDelta = frame_time > 0.0f ? frame_time * 1000.0f : 16.6667f;
	d.preExposure = 1.0f;
	/* FSR3 receives the Q2RTX-unit clip planes; only its physical-distance
	 * conversion factor below changes units. */
	d.cameraNear = vkpt_refdef.z_near; d.cameraFar = vkpt_refdef.z_far;
	d.cameraFovAngleVertical = vkpt_refdef.fd->fov_y * (float)M_PI / 180.0f;
	d.viewSpaceToMetersFactor = METERS_PER_WORLD_UNIT;
	if (!vkpt_fsr3_get_jitter(&d.jitterOffset.x, &d.jitterOffset.y)) return VK_ERROR_DEVICE_LOST;
	d.motionVectorScale.x = (float)qvk.extent_render.width;
	d.motionVectorScale.y = (float)qvk.extent_render.height;
	d.reset = fsr3_history_invalid; fsr3_history_invalid = false;
	/* flt_fsr_debug 1 draws the SDK's debug view into the output instead of the upscaled image. */
	d.flags = (cvar_flt_fsr_debug && cvar_flt_fsr_debug->integer) ? 1u : 0u;
	if (cvar_flt_fsr_debug && cvar_flt_fsr_debug->integer >= 2) {
		static int reported = 0;
		if (reported++ < 3)
			Com_Printf("FSR3 dispatch: render %ux%u upscale %ux%u jitter %.3f %.3f mvscale %.0f %.0f near %.2f far %.2f fov %.3f dt %.2f reset %d\n",
				d.renderSize.width, d.renderSize.height, d.upscaleSize.width, d.upscaleSize.height,
				d.jitterOffset.x, d.jitterOffset.y, d.motionVectorScale.x, d.motionVectorScale.y,
				d.cameraNear, d.cameraFar, d.cameraFovAngleVertical, d.frameTimeDelta, (int)d.reset);
	}
	return ffxFsr3UpscalerContextDispatch(&fsr3_context, &d) == FFX_OK ? VK_SUCCESS : VK_ERROR_DEVICE_LOST;
}

bool vkpt_fsr3_get_jitter(float *x, float *y)
{
	if (!fsr3_context_valid) return false;
	int32_t phase_count = ffxFsr3UpscalerGetJitterPhaseCount((int32_t)qvk.extent_render.width,
		(int32_t)qvk.extent_unscaled.width);
	if (phase_count <= 0) return false;
	return ffxFsr3UpscalerGetJitterOffset(x, y,
		(int32_t)(qvk.frame_counter % (uint64_t)phase_count), phase_count) == FFX_OK;
}
VkResult vkpt_fsr3_dispatch(VkCommandBuffer cmd_buf, float frame_time, bool reset)
{
	if (reset) vkpt_fsr3_reset();
	/* Keep the adapter inputs and output explicit: FSR3 consumes only its
	 * dedicated adapter resources. */
	if (!vkpt_fsr3_is_enabled()) return VK_SUCCESS;
	return vkpt_fsr3_do(cmd_buf, frame_time);
}
