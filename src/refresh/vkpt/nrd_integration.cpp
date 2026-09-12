/* Optional per-frame NRD bridge. */
#ifdef CONFIG_VKPT_NRD
// These have to precede vkpt.h: shared.h defines min/max and PLANE_X/Y/Z as
// object-like macros, which break both libstdc++ (std::numeric_limits::min())
// and NRI's own enum members (nri::PlaneBits::PLANE_Y).
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>

#include "NRI.h"
#include "Extensions/NRIHelper.h"
// NRIWrapperVK.h refers to AccelerationStructureBits, which lives in the ray
// tracing extension header and is not pulled in by NRI.h.
#include "Extensions/NRIRayTracing.h"
#include "Extensions/NRIWrapperVK.h"
#include "NRD.h"
#include "NRDIntegration.hpp"
#endif

// The engine headers are C: without this the Com_*/Cvar_* calls below get C++
// mangling and fail to link.
extern "C" {
#include "vkpt.h"
}
#include "shader/constants.h"
#include "nrd_math.h"

#ifdef CONFIG_VKPT_NRD

namespace {
// Identifiers of the denoisers inside the single NRD instance. Direct diffuse
// uses REBLUR, indirect diffuse uses RELAX, and specular keeps its cvar-selected
// REBLUR/RELAX mode.
enum : nrd::Identifier {
	NRD_DENOISER_DIRECT_DIFFUSE = 0,
	NRD_DENOISER_INDIRECT_DIFFUSE = 1,
	NRD_DENOISER_SPECULAR = 2
};

nrd::Integration *g_integration = nullptr;
bool g_available = false;
// Latched at instance creation: the settings struct passed to
// SetDenoiserSettings() must match the denoiser type chosen back then, and the
// prepare/composite shaders must pack specular the same way. Changing the cvar
// recreates the instance instead of switching mid-frame.
bool g_spec_relax = false;
bool g_spec_sh = false;
float g_smoothed_frame_time = 1.0f / 60.0f;
char g_error[256] = "";
uint64_t g_last_frame = 0;
uint16_t g_resource_size_prev[2] = {};
float g_camera_jitter_prev[2] = {};
// Tracks the shared NRD history state for all three denoisers.
bool g_frame_history_valid = false;

void disable(const char *message) noexcept
{
	std::snprintf(g_error, sizeof(g_error), "%s", message ? message : "unknown error");
	try {
		Com_WPrintf("NRD unavailable: %s\n", g_error);
	} catch (...) {
		// Failure reporting must not mask the original NRD failure.
	}
	g_available = false;
}

// Mirrors the flt_nrd_spec_denoiser cvar declared by the UBO cvar list, which is
// what nrd_prepare.comp / nrd_composite.comp branch on. Read here rather than
// from the UBO because the instance is created before the first frame fills it.
static bool spec_denoiser_is_relax()
{
	const cvar_t *cv = Cvar_Get("flt_nrd_spec_denoiser", "1", 0);
	return cv && cv->value >= 0.5f;
}

static nrd::Resource make_resource(unsigned image, VkFormat format)
{
	nrd::Resource resource = {};
	// VkImage is a pointer on 64-bit targets, while NRI takes the raw handle.
	resource.vk.image = reinterpret_cast<VKNonDispatchableHandle>(qvk.images[image]);
	resource.vk.format = static_cast<VKEnum>(format);
	// q2rtx keeps these images in GENERAL and accesses them from compute.
	resource.state = {nri::AccessBits::SHADER_RESOURCE_STORAGE,
		nri::Layout::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER};
	return resource;
}

static void set_common_resources(nrd::ResourceSnapshot &snapshot)
{
	// Q2RTX keeps these images in GENERAL between dispatches. Ask NRI to
	// restore that application-owned state before returning to the frame graph.
	snapshot.restoreInitialState = true;
	snapshot.SetResource(nrd::ResourceType::IN_MV,
		make_resource(VKPT_IMG_NRD_MV, VK_FORMAT_R16G16_SFLOAT));
	snapshot.SetResource(nrd::ResourceType::IN_NORMAL_ROUGHNESS,
		make_resource(VKPT_IMG_NRD_NORMAL_ROUGHNESS, VK_FORMAT_R16G16B16A16_SFLOAT));
	snapshot.SetResource(nrd::ResourceType::IN_VIEWZ,
		make_resource(VKPT_IMG_NRD_VIEWZ, VK_FORMAT_R32_SFLOAT));
    if (vkpt_refdef.uniform_buffer.flt_nrd_spec_confidence >= 0.5f) {
        snapshot.SetResource(nrd::ResourceType::IN_DIFF_CONFIDENCE,
            make_resource(VKPT_IMG_NRD_DIFF_CONFIDENCE, VK_FORMAT_R16_SFLOAT));
        snapshot.SetResource(nrd::ResourceType::IN_SPEC_CONFIDENCE,
            make_resource(VKPT_IMG_NRD_SPEC_CONFIDENCE, VK_FORMAT_R16_SFLOAT));
    }
}

static void set_diffuse_resources(nrd::ResourceSnapshot &snapshot, bool direct)
{
	snapshot.SetResource(nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST,
		make_resource(direct ? VKPT_IMG_NRD_DIRECT_DIFF_RADIANCE_HITDIST_IN
			: VKPT_IMG_NRD_INDIRECT_DIFF_RADIANCE_HITDIST_IN,
			VK_FORMAT_R16G16B16A16_SFLOAT));
	snapshot.SetResource(nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST,
		make_resource(direct ? VKPT_IMG_NRD_DIRECT_DIFF_RADIANCE_HITDIST_OUT
			: VKPT_IMG_NRD_INDIRECT_DIFF_RADIANCE_HITDIST_OUT,
			VK_FORMAT_R16G16B16A16_SFLOAT));
}

static void set_specular_resources(nrd::ResourceSnapshot &snapshot)
{
	snapshot.SetResource(g_spec_sh ? nrd::ResourceType::IN_SPEC_SH0 : nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST,
		make_resource(VKPT_IMG_NRD_SPEC_RADIANCE_HITDIST_IN, VK_FORMAT_R16G16B16A16_SFLOAT));
	snapshot.SetResource(g_spec_sh ? nrd::ResourceType::OUT_SPEC_SH0 : nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST,
		make_resource(VKPT_IMG_NRD_SPEC_RADIANCE_HITDIST_OUT, VK_FORMAT_R16G16B16A16_SFLOAT));
    if (g_spec_sh) {
        snapshot.SetResource(nrd::ResourceType::IN_SPEC_SH1,
            make_resource(VKPT_IMG_NRD_SPEC_SH1_IN, VK_FORMAT_R16G16B16A16_SFLOAT));
        snapshot.SetResource(nrd::ResourceType::OUT_SPEC_SH1,
            make_resource(VKPT_IMG_NRD_SPEC_SH1_OUT, VK_FORMAT_R16G16B16A16_SFLOAT));
    }
}

static bool configure_frame(float frame_time, bool reset_history)
{
	const QVKUniformBuffer_t &ubo = vkpt_refdef.uniform_buffer;
	nrd::CommonSettings common = {};
	vkpt_nrd_projection(common.viewToClipMatrix, &ubo.P[0][0]);
	vkpt_nrd_projection(common.viewToClipMatrixPrev, &ubo.P_prev[0][0]);
	std::memcpy(common.worldToViewMatrix, ubo.V, sizeof(common.worldToViewMatrix));
	std::memcpy(common.worldToViewMatrixPrev, ubo.V_prev, sizeof(common.worldToViewMatrixPrev));
	common.motionVectorScale[0] = 1.0f;
	common.motionVectorScale[1] = 1.0f;
	common.motionVectorScale[2] = 0.0f;
	// primary_rays.rgen projects the same surface into two non-jittered
	// matrices. The sampled position includes jitter, but their difference
	// already excludes projection jitter. Keep the default zero MV bias.
	common.cameraJitter[0] = ubo.sub_pixel_jitter[0];
	common.cameraJitter[1] = ubo.sub_pixel_jitter[1];
	common.cameraJitterPrev[0] = g_camera_jitter_prev[0];
	common.cameraJitterPrev[1] = g_camera_jitter_prev[1];
	common.resourceSize[0] = static_cast<uint16_t>(qvk.extent_screen_images.width);
	common.resourceSize[1] = static_cast<uint16_t>(qvk.extent_screen_images.height);
	common.resourceSizePrev[0] = g_frame_history_valid ? g_resource_size_prev[0] : common.resourceSize[0];
	common.resourceSizePrev[1] = g_frame_history_valid ? g_resource_size_prev[1] : common.resourceSize[1];
	// The images are allocated at screen-image size, while the current render
	// rectangle is carried by the per-frame UBO (and can be dynamically scaled).
	common.rectSize[0] = static_cast<uint16_t>(ubo.width);
	common.rectSize[1] = static_cast<uint16_t>(ubo.height);
	common.rectSizePrev[0] = static_cast<uint16_t>(ubo.prev_width);
	common.rectSizePrev[1] = static_cast<uint16_t>(ubo.prev_height);
	if (ubo.flt_nrd_debug >= 0.5f) {
		static uint16_t dbg_rect[2], dbg_res[2];
		if (dbg_rect[0] != common.rectSize[0] || dbg_rect[1] != common.rectSize[1] ||
			dbg_res[0] != common.resourceSize[0] || dbg_res[1] != common.resourceSize[1]) {
			dbg_rect[0] = common.rectSize[0]; dbg_rect[1] = common.rectSize[1];
			dbg_res[0] = common.resourceSize[0]; dbg_res[1] = common.resourceSize[1];
			Com_Printf("NRD: rect %ux%u prev %ux%u resource %ux%u\n",
				common.rectSize[0], common.rectSize[1], common.rectSizePrev[0], common.rectSizePrev[1],
				common.resourceSize[0], common.resourceSize[1]);
		}
	}
	common.timeDeltaBetweenFrames = frame_time > 0.0f ? frame_time * 1000.0f : 0.0f;
	// NRD treats viewZ >= denoisingRange as sky and leaves those pixels
	// unwritten. nrd_prepare clamps misses to PRIMARY_RAY_T_MAX, so keep real
	// surfaces at that distance inside the range.
	common.denoisingRange = 2.0f * (float)PRIMARY_RAY_T_MAX;
	common.frameIndex = static_cast<uint32_t>(qvk.frame_counter);
	const bool resource_size_changed = g_frame_history_valid &&
		(common.resourceSize[0] != common.resourceSizePrev[0] ||
		 common.resourceSize[1] != common.resourceSizePrev[1]);
	const bool discontinuity = qvk.frame_counter == 0 || qvk.frame_counter != g_last_frame + 1;
	// An explicitly invalidated frame must clear buffer contents as well as
	// accumulation weights. RESTART alone can retain garbage/NaNs, whereas
	// disabling/re-enabling NRD already takes the full-clear path below.
	const bool clear_history = reset_history || !g_frame_history_valid || resource_size_changed || discontinuity;
	common.accumulationMode = clear_history
		? nrd::AccumulationMode::CLEAR_AND_RESTART : nrd::AccumulationMode::CONTINUE;
	if (clear_history && ubo.flt_nrd_debug != 0.0f)
		Com_Printf("NRD: history clear frame %u invalidated %d initialized %d resource_changed %d discontinuity %d\n",
			common.frameIndex, int(reset_history), int(g_frame_history_valid), int(resource_size_changed), int(discontinuity));
	common.isMotionVectorInWorldSpace = false;
    common.isHistoryConfidenceAvailable = ubo.flt_nrd_spec_confidence >= 0.5f;

	if (g_integration->SetCommonSettings(common) != nrd::Result::SUCCESS)
		return false;

	nrd::ReblurSettings direct_reblur = {};
	// Keep these synchronized with reblur_hit_distance_normalization() in nrd_prepare.comp.
	direct_reblur.hitDistanceParameters.A = NRD_REBLUR_HIT_DISTANCE_A;
	direct_reblur.hitDistanceParameters.B = NRD_REBLUR_HIT_DISTANCE_B;
	direct_reblur.hitDistanceParameters.C = NRD_REBLUR_HIT_DISTANCE_C;
	direct_reblur.checkerboardMode = nrd::CheckerboardMode::OFF;
	bool success = g_integration->SetDenoiserSettings(NRD_DENOISER_DIRECT_DIFFUSE, &direct_reblur)
		== nrd::Result::SUCCESS;

	// RELAX has no hit distance normalization: it consumes the raw hit distance
	// written by nrd_prepare.comp. The checkerboard is already resolved there.
	nrd::RelaxSettings indirect_relax = {};
	indirect_relax.checkerboardMode = nrd::CheckerboardMode::OFF;
	// First-bounce lobe selection leaves holes in each signal. Reconstruct
	// in-lobe distances before filtering, including half-resolution GI rows.
	indirect_relax.hitDistanceReconstructionMode = nrd::HitDistanceReconstructionMode::AREA_5X5;
	indirect_relax.enableAntiFirefly = true;
	success = success && g_integration->SetDenoiserSettings(NRD_DENOISER_INDIRECT_DIFFUSE,
		&indirect_relax) == nrd::Result::SUCCESS;

    // Specular has its own response settings; diffuse remains on SDK defaults.
    if (std::isfinite(frame_time) && frame_time > 0.0f)
        g_smoothed_frame_time = g_smoothed_frame_time * 0.9f + frame_time * 0.1f;
    float seconds = ubo.flt_nrd_spec_history_seconds;
    if (!std::isfinite(seconds)) seconds = 1.0f;
    const uint32_t history = common.isHistoryConfidenceAvailable
        ? static_cast<uint32_t>(Q_clipf(seconds / g_smoothed_frame_time + 0.5f, 4.0f, 60.0f)) : 30;
    const uint32_t fast_history = history > 20 ? history / 5 : 4;
    float lobe_scale = ubo.flt_nrd_spec_lobe_scale;
    if (!std::isfinite(lobe_scale)) lobe_scale = 1.5f;
    lobe_scale = g_spec_sh ? Q_clipf(lobe_scale, 0.1f, 4.0f) : 1.0f;
    if (g_spec_relax) {
        nrd::RelaxSettings specular_relax = indirect_relax;
        specular_relax.specularMaxAccumulatedFrameNum = history;
        specular_relax.specularMaxFastAccumulatedFrameNum = fast_history < history ? fast_history : history - 1;
        specular_relax.historyFixFrameNum = specular_relax.specularMaxFastAccumulatedFrameNum > 3 ? 3 : 1;
        specular_relax.lobeAngleFraction *= lobe_scale;
        if (ubo.flt_nrd_spec_antilag < 0.5f) {
            specular_relax.antilagSettings.accelerationAmount = 0.0f;
            specular_relax.antilagSettings.resetAmount = 0.0f;
        }
        success = success && g_integration->SetDenoiserSettings(NRD_DENOISER_SPECULAR,
            &specular_relax) == nrd::Result::SUCCESS;
    } else {
        nrd::ReblurSettings specular_reblur = direct_reblur;
        specular_reblur.hitDistanceReconstructionMode = nrd::HitDistanceReconstructionMode::AREA_5X5;
        specular_reblur.maxAccumulatedFrameNum = history;
        specular_reblur.maxFastAccumulatedFrameNum = fast_history < history ? fast_history : history - 1;
        specular_reblur.historyFixFrameNum = specular_reblur.maxFastAccumulatedFrameNum > 3 ? 3 : 1;
        specular_reblur.lobeAngleFraction *= lobe_scale;
        // 4.18 has no enable flag. ComputeAntilag tends to 1 as sensitivity
        // increases. Keep it finite to avoid inf/NaN in the shader.
        if (ubo.flt_nrd_spec_antilag < 0.5f)
            specular_reblur.antilagSettings.luminanceSensitivity = 1e6f;
        success = success && g_integration->SetDenoiserSettings(NRD_DENOISER_SPECULAR,
            &specular_reblur) == nrd::Result::SUCCESS;
    }

	if (success) {
		g_resource_size_prev[0] = common.resourceSize[0];
		g_resource_size_prev[1] = common.resourceSize[1];
		g_camera_jitter_prev[0] = common.cameraJitter[0];
		g_camera_jitter_prev[1] = common.cameraJitter[1];
		g_frame_history_valid = true;
	}
	return success;
}

VkResult recreate_impl()
{
	g_available = false;
	g_error[0] = '\0';
	if (qvk.device_count != 1) {
		disable("NRD requires exactly one Vulkan device");
		return VK_SUCCESS;
	}

	if (qvk.queue_idx_graphics < 0 || qvk.queue_graphics == VK_NULL_HANDLE) {
		disable("graphics queue is unavailable");
		return VK_SUCCESS;
	}

	// NRI refuses to adopt a device without synchronization2 (and aborts the
	// process while doing so), and dereferences vkCmdPushDescriptorSet without
	// checking, so verify both are enabled before handing the device over.
	if (!qvk.supports_nrd_device_extensions) {
		disable("the Vulkan device lacks synchronization2, push descriptors, partially bound descriptors, or formatless storage image access");
		return VK_SUCCESS;
	}

	nri::QueueFamilyVKDesc queue_family = {};
	queue_family.familyIndex = static_cast<uint32_t>(qvk.queue_idx_graphics);
	queue_family.queueNum = 1;
	queue_family.queueType = nri::QueueType::GRAPHICS;

	nri::DeviceCreationVKDesc device_desc = {};
	device_desc.vkInstance = qvk.instance;
	device_desc.vkPhysicalDevice = qvk.physical_device;
	device_desc.vkDevice = qvk.device;
	device_desc.queueFamilies = &queue_family;
	device_desc.queueFamilyNum = 1;
	device_desc.minorVersion = 2;
	// Without this NRI assumes a bare 1.2 device and rejects it for missing
	// synchronization2. Only the extensions NRD itself needs are declared: NRI
	// eagerly resolves the entry points of every extension it is told about, and
	// the ray tracing ones q2rtx enables would make it look for functions from
	// extensions (such as ray_tracing_maintenance1) that are not enabled here.
	static const char *const nrd_device_extensions[] = {
		VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
		VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME
	};
	device_desc.vkExtensions.deviceExtensions = nrd_device_extensions;
	device_desc.vkExtensions.deviceExtensionNum = LENGTH(nrd_device_extensions);
	device_desc.vkBindingOffsets.sRegister = 0;
	device_desc.vkBindingOffsets.tRegister = 20;
	device_desc.vkBindingOffsets.bRegister = 2;
	device_desc.vkBindingOffsets.uRegister = 3;

	// One instance, three denoisers: NRD explicitly supports mixing families, and
	// the transient memory pool is shared between direct diffuse, indirect
	// diffuse, and specular.
	g_spec_relax = spec_denoiser_is_relax();
    g_spec_sh = Cvar_Get("flt_nrd_spec_sh", "1", 0)->value >= 0.5f;

	nrd::DenoiserDesc denoisers[3] = {};
	denoisers[0].identifier = NRD_DENOISER_DIRECT_DIFFUSE;
	denoisers[0].denoiser = nrd::Denoiser::REBLUR_DIFFUSE;
	denoisers[1].identifier = NRD_DENOISER_INDIRECT_DIFFUSE;
	denoisers[1].denoiser = nrd::Denoiser::RELAX_DIFFUSE;
	denoisers[2].identifier = NRD_DENOISER_SPECULAR;
	denoisers[2].denoiser = g_spec_relax
        ? (g_spec_sh ? nrd::Denoiser::RELAX_SPECULAR_SH : nrd::Denoiser::RELAX_SPECULAR)
        : (g_spec_sh ? nrd::Denoiser::REBLUR_SPECULAR_SH : nrd::Denoiser::REBLUR_SPECULAR);
	nrd::InstanceCreationDesc instance_desc = {};
	instance_desc.denoisers = denoisers;
	instance_desc.denoisersNum = LENGTH(denoisers);

	nrd::IntegrationCreationDesc integration_desc = {};
	std::snprintf(integration_desc.name, sizeof(integration_desc.name), "q2rtx-reblur-relax-%s",
		g_spec_relax ? "relax" : "reblur");
	integration_desc.resourceWidth = static_cast<uint16_t>(qvk.extent_screen_images.width);
	integration_desc.resourceHeight = static_cast<uint16_t>(qvk.extent_screen_images.height);
	integration_desc.queuedFrameNum = MAX_FRAMES_IN_FLIGHT;
	integration_desc.autoWaitForIdle = false;

	if (!g_integration) {
		g_integration = new (std::nothrow) nrd::Integration();
		if (!g_integration) {
			disable("could not allocate NRD integration");
			return VK_SUCCESS;
		}
	}
	const nrd::Result result = g_integration->RecreateVK(integration_desc, instance_desc, device_desc);
	if (result != nrd::Result::SUCCESS) {
		char message[256];
		std::snprintf(message, sizeof(message), "NRD integration creation failed (result %u)",
			static_cast<unsigned>(result));
		disable(message);
		return VK_SUCCESS;
	}

	g_available = true;
	g_last_frame = 0;
	// Recreate starts fresh histories for all three denoisers.
	g_frame_history_valid = false;
	g_camera_jitter_prev[0] = 0.0f;
	g_camera_jitter_prev[1] = 0.0f;
	Com_Printf("NRD integration enabled: REBLUR direct diffuse + RELAX indirect diffuse + %s specular (%ux%u)\n",
		g_spec_relax ? "RELAX" : "REBLUR",
		qvk.extent_screen_images.width, qvk.extent_screen_images.height);
	return VK_SUCCESS;
}
} // namespace

extern "C" VkResult vkpt_nrd_initialize(void)
{
	try { return recreate_impl(); }
	catch (...) { disable("exception while creating NRD integration"); return VK_SUCCESS; }
}

extern "C" VkResult vkpt_nrd_recreate(void)
{
	return vkpt_nrd_initialize();
}

extern "C" VkResult vkpt_nrd_destroy(void)
{
	try {
		g_available = false;
		g_last_frame = 0;
		// Destroy invalidates the histories of all three denoisers.
		g_frame_history_valid = false;
		if (g_integration) {
			g_integration->Destroy();
			delete g_integration;
			g_integration = nullptr;
		}
	} catch (...) {
		disable("exception while destroying NRD integration");
	}
	return VK_SUCCESS;
}

extern "C" bool vkpt_nrd_available(void) { return g_available; }
extern "C" bool vkpt_nrd_history_valid(void)
{
    return g_available && g_frame_history_valid && qvk.frame_counter != 0 &&
        qvk.frame_counter == g_last_frame + 1 &&
        qvk.extent_screen_images.width == g_resource_size_prev[0] &&
        qvk.extent_screen_images.height == g_resource_size_prev[1];
}
extern "C" const char *vkpt_nrd_get_error(void) { return g_error[0] ? g_error : nullptr; }

extern "C" VkResult vkpt_nrd_denoise(VkCommandBuffer command_buffer, float frame_time,
	bool reset_history)
{
	if (!g_available || !g_integration || command_buffer == VK_NULL_HANDLE)
		return VK_NOT_READY;

	try {
		g_integration->NewFrame();
		if (!configure_frame(frame_time, reset_history)) {
			disable("NRD per-frame settings failed");
			return VK_ERROR_INITIALIZATION_FAILED;
		}

		nri::CommandBufferVKDesc command_desc = {};
		command_desc.vkCommandBuffer = command_buffer;
		command_desc.queueType = nri::QueueType::GRAPHICS;

		// Run direct diffuse first with only its diffuse resources bound.
		nrd::ResourceSnapshot direct_resources;
		set_common_resources(direct_resources);
		set_diffuse_resources(direct_resources, true);
		const nrd::Identifier direct_denoisers[] = { NRD_DENOISER_DIRECT_DIFFUSE };
		g_integration->DenoiseVK(direct_denoisers, LENGTH(direct_denoisers), command_desc,
			direct_resources);

		// Then run indirect diffuse and specular together over the diffuse and
		// existing specular resources.
		nrd::ResourceSnapshot indirect_resources;
		set_common_resources(indirect_resources);
		set_diffuse_resources(indirect_resources, false);
		set_specular_resources(indirect_resources);
		const nrd::Identifier indirect_denoisers[] = {
			NRD_DENOISER_INDIRECT_DIFFUSE, NRD_DENOISER_SPECULAR
		};
		g_integration->DenoiseVK(indirect_denoisers, LENGTH(indirect_denoisers), command_desc,
			indirect_resources);
		g_last_frame = qvk.frame_counter;
		return VK_SUCCESS;
	} catch (...) {
		disable("exception while recording NRD denoise work");
		return VK_ERROR_INITIALIZATION_FAILED;
	}
}

#endif /* CONFIG_VKPT_NRD */
