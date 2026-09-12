/*
Copyright (C) 2018 Christoph Schied
Copyright (C) 2019, NVIDIA CORPORATION. All rights reserved.

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

#ifndef  _TEXTURES_H_
#define  _TEXTURES_H_

#include "constants.h"

#define IMG_WIDTH  (qvk.extent_screen_images.width)
#define IMG_HEIGHT (qvk.extent_screen_images.height)
#define IMG_WIDTH_MGPU (qvk.extent_screen_images.width / qvk.device_count)
#define IMG_WIDTH_RENDER  (qvk.extent_render.width)
#define IMG_HEIGHT_RENDER (qvk.extent_render.height)
#define IMG_WIDTH_UNSCALED  (qvk.extent_unscaled.width)
#define IMG_HEIGHT_UNSCALED (qvk.extent_unscaled.height)

#define IMG_WIDTH_TAA  (qvk.extent_taa_images.width)
#define IMG_HEIGHT_TAA  (qvk.extent_taa_images.height)

/* These are images that are to be used as render targets and buffers, but not textures. */
#define LIST_IMAGES \
	IMG_DO(PT_MOTION,                  0, R16G16B16A16_SFLOAT, rgba16f, IMG_WIDTH,           IMG_HEIGHT     ) \
	IMG_DO(PT_TRANSPARENT,             1, R16G16B16A16_SFLOAT, rgba16f, IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(ASVGF_COLOR,                2, R16G16B16A16_SFLOAT, rgba16f, IMG_WIDTH,           IMG_HEIGHT     ) \
	IMG_DO(PT_SHADING_POSITION,        3, R32G32B32A32_SFLOAT, rgba32f, IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(FLAT_COLOR,                 4, R16G16B16A16_SFLOAT, rgba16f, IMG_WIDTH,           IMG_HEIGHT     ) \
	IMG_DO(FLAT_MOTION,                5, R16G16B16A16_SFLOAT, rgba16f, IMG_WIDTH,           IMG_HEIGHT     ) \
	IMG_DO(PT_GODRAYS_THROUGHPUT_DIST, 6, R16G16B16A16_SFLOAT, rgba16f, IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(BLOOM_HBLUR,                7, R16G16B16A16_SFLOAT, rgba16f, IMG_WIDTH_TAA / 4,   IMG_HEIGHT_TAA / 4 ) \
	IMG_DO(BLOOM_VBLUR,                8, R16G16B16A16_SFLOAT, rgba16f, IMG_WIDTH_TAA / 4,   IMG_HEIGHT_TAA / 4 ) \
	IMG_DO(TAA_OUTPUT,                 9, R16G16B16A16_SFLOAT, rgba16f, IMG_WIDTH_TAA,       IMG_HEIGHT_TAA ) \
	IMG_DO(PT_VIEW_DIRECTION,         10, R16G16B16A16_SFLOAT, rgba16f, IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_VIEW_DIRECTION2,        11, R16G16B16A16_SFLOAT, rgba16f, IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_THROUGHPUT,              12, R16G16B16A16_SFLOAT, rgba16f, IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_BOUNCE_THROUGHPUT,       13, R16G16B16A16_SFLOAT, rgba16f, IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(HQ_COLOR_INTERLEAVED,       14, R32G32B32A32_SFLOAT, rgba32f, IMG_WIDTH,           IMG_HEIGHT     ) \
	IMG_DO(PT_COLOR_LF_SH,             15, R16G16B16A16_SFLOAT, rgba16f, IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_COLOR_LF_COCG,           16, R16G16_SFLOAT,       rg16f,   IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_COLOR_HF,                17, R32_UINT,            r32ui,   IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_COLOR_SPEC,              18, R32_UINT,            r32ui,   IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_GEO_NORMAL2,             19, R32_UINT,            r32ui,   IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(FSR3_DEPTH,                 20, R32_SFLOAT,          r32f,    IMG_WIDTH,          IMG_HEIGHT) \
	IMG_DO(FSR3_REACTIVE_MASK,        21, R8_UNORM,            r8,      IMG_WIDTH,          IMG_HEIGHT) \
	IMG_DO(FSR3_TRANSPARENCY_MASK,    22, R8_UNORM,            r8,      IMG_WIDTH,          IMG_HEIGHT) \
	IMG_DO(CLEAR,                      23, R8G8B8A8_UNORM,      rgba8,   1,                   1              ) \
	IMG_DO(PT_NRD_HITDIST,             24, R16G16B16A16_SFLOAT, rgba16f, IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(NRD_MV,                     25, R16G16_SFLOAT,       rg16f,   IMG_WIDTH,           IMG_HEIGHT     ) \
	IMG_DO(NRD_NORMAL_ROUGHNESS,       26, R16G16B16A16_SFLOAT, rgba16f, IMG_WIDTH,           IMG_HEIGHT     ) \
	IMG_DO(NRD_VIEWZ,                  27, R32_SFLOAT,          r32f,    IMG_WIDTH,           IMG_HEIGHT     ) \
	IMG_DO(NRD_DIRECT_DIFF_RADIANCE_HITDIST_IN,  28, R16G16B16A16_SFLOAT, rgba16f, IMG_WIDTH, IMG_HEIGHT) \
	IMG_DO(NRD_INDIRECT_DIFF_RADIANCE_HITDIST_IN,29, R16G16B16A16_SFLOAT, rgba16f, IMG_WIDTH, IMG_HEIGHT) \
	IMG_DO(NRD_SPEC_RADIANCE_HITDIST_IN,         30, R16G16B16A16_SFLOAT, rgba16f, IMG_WIDTH, IMG_HEIGHT) \
	IMG_DO(NRD_DIRECT_DIFF_RADIANCE_HITDIST_OUT, 31, R16G16B16A16_SFLOAT, rgba16f, IMG_WIDTH, IMG_HEIGHT) \
	IMG_DO(NRD_INDIRECT_DIFF_RADIANCE_HITDIST_OUT,32,R16G16B16A16_SFLOAT, rgba16f, IMG_WIDTH, IMG_HEIGHT) \
	IMG_DO(NRD_SPEC_RADIANCE_HITDIST_OUT,        33, R16G16B16A16_SFLOAT, rgba16f, IMG_WIDTH, IMG_HEIGHT) \
    LIST_IMAGES_NRD_EXTRA

// Traced moments use FP32 to avoid overflowing before the NRD input rescale.
#define LIST_IMAGES_NRD_EXTRA \
    IMG_DO(PT_SPEC_MOMENT, 34, R32G32B32A32_SFLOAT, rgba32f, IMG_WIDTH_MGPU, IMG_HEIGHT) \
    IMG_DO(PT_SPEC_FIRST_DIRECTION, 35, R16G16B16A16_SFLOAT, rgba16f, IMG_WIDTH_MGPU, IMG_HEIGHT) \
    IMG_DO(NRD_SPEC_SH1_IN, 36, R16G16B16A16_SFLOAT, rgba16f, IMG_WIDTH, IMG_HEIGHT) \
    IMG_DO(NRD_SPEC_SH1_OUT, 37, R16G16B16A16_SFLOAT, rgba16f, IMG_WIDTH, IMG_HEIGHT) \
    IMG_DO(NRD_CONFIDENCE_SAMPLE_0, 38, R32G32B32A32_SFLOAT, rgba32f, (IMG_WIDTH + 4) / 5, (IMG_HEIGHT + 4) / 5) \
    IMG_DO(NRD_CONFIDENCE_SAMPLE_1, 39, R32G32B32A32_SFLOAT, rgba32f, (IMG_WIDTH + 4) / 5, (IMG_HEIGHT + 4) / 5) \
    IMG_DO(NRD_CONFIDENCE_GRADIENT_0, 40, R32G32B32A32_SFLOAT, rgba32f, (IMG_WIDTH + 4) / 5, (IMG_HEIGHT + 4) / 5) \
    IMG_DO(NRD_CONFIDENCE_GRADIENT_1, 41, R32G32B32A32_SFLOAT, rgba32f, (IMG_WIDTH + 4) / 5, (IMG_HEIGHT + 4) / 5) \
    IMG_DO(NRD_SPEC_CONFIDENCE, 42, R16_SFLOAT, r16f, IMG_WIDTH, IMG_HEIGHT) \
    IMG_DO(NRD_DIFF_CONFIDENCE, 43, R16_SFLOAT, r16f, IMG_WIDTH, IMG_HEIGHT)

#define NUM_IMAGES_BASE     44

#define LIST_IMAGES_A_B \
	IMG_DO(PT_VISBUF_PRIM_A,          NUM_IMAGES_BASE + 0,  R32G32_UINT,         rg32ui,  IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_VISBUF_PRIM_B,          NUM_IMAGES_BASE + 1,  R32G32_UINT,         rg32ui,  IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_VISBUF_BARY_A,          NUM_IMAGES_BASE + 2,  R16G16_SFLOAT,       rg16f,   IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_VISBUF_BARY_B,          NUM_IMAGES_BASE + 3,  R16G16_SFLOAT,       rg16f,   IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_CLUSTER_A,              NUM_IMAGES_BASE + 4,  R16_UINT,            r16ui,   IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_CLUSTER_B,              NUM_IMAGES_BASE + 5,  R16_UINT,            r16ui,   IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_BASE_COLOR_A,           NUM_IMAGES_BASE + 6,  R16G16B16A16_SFLOAT, rgba16f, IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_BASE_COLOR_B,           NUM_IMAGES_BASE + 7,  R16G16B16A16_SFLOAT, rgba16f, IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_METALLIC_A,             NUM_IMAGES_BASE + 8,  R8G8_UNORM,          rg8,     IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_METALLIC_B,             NUM_IMAGES_BASE + 9,  R8G8_UNORM,          rg8,     IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_VIEW_DEPTH_A,           NUM_IMAGES_BASE + 10, R16_SFLOAT,          r32f,    IMG_WIDTH,           IMG_HEIGHT     ) \
	IMG_DO(PT_VIEW_DEPTH_B,           NUM_IMAGES_BASE + 11, R16_SFLOAT,          r32f,    IMG_WIDTH,           IMG_HEIGHT     ) \
	IMG_DO(PT_NORMAL_A,               NUM_IMAGES_BASE + 12, R32_UINT,            r32ui,   IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_NORMAL_B,               NUM_IMAGES_BASE + 13, R32_UINT,            r32ui,   IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_GEO_NORMAL_A,           NUM_IMAGES_BASE + 14, R32_UINT,            r32ui,   IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_GEO_NORMAL_B,           NUM_IMAGES_BASE + 15, R32_UINT,            r32ui,   IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(ASVGF_TAA_A,               NUM_IMAGES_BASE + 16, R16G16B16A16_SFLOAT, rgba16f, IMG_WIDTH_TAA,       IMG_HEIGHT_TAA ) \
	IMG_DO(ASVGF_TAA_B,               NUM_IMAGES_BASE + 17, R16G16B16A16_SFLOAT, rgba16f, IMG_WIDTH_TAA,       IMG_HEIGHT_TAA ) \
	IMG_DO(ASVGF_RNG_SEED_A,          NUM_IMAGES_BASE + 18, R32_UINT,            r32ui,   IMG_WIDTH,           IMG_HEIGHT     ) \
	IMG_DO(ASVGF_RNG_SEED_B,          NUM_IMAGES_BASE + 19, R32_UINT,            r32ui,   IMG_WIDTH,           IMG_HEIGHT     ) \

#define LIST_IMAGES_B_A \
	IMG_DO(PT_VISBUF_PRIM_B,          NUM_IMAGES_BASE + 0,  R32G32_UINT,         rg32ui,  IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_VISBUF_PRIM_A,          NUM_IMAGES_BASE + 1,  R32G32_UINT,         rg32ui,  IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_VISBUF_BARY_B,          NUM_IMAGES_BASE + 2,  R16G16_SFLOAT,       rg16f,   IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_VISBUF_BARY_A,          NUM_IMAGES_BASE + 3,  R16G16_SFLOAT,       rg16f,   IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_CLUSTER_B,              NUM_IMAGES_BASE + 4,  R16_UINT,            r16ui,   IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_CLUSTER_A,              NUM_IMAGES_BASE + 5,  R16_UINT,            r16ui,   IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_BASE_COLOR_B,           NUM_IMAGES_BASE + 6,  R16G16B16A16_SFLOAT, rgba16f, IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_BASE_COLOR_A,           NUM_IMAGES_BASE + 7,  R16G16B16A16_SFLOAT, rgba16f, IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_METALLIC_B,             NUM_IMAGES_BASE + 8,  R8G8_UNORM,          rg8,     IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_METALLIC_A,             NUM_IMAGES_BASE + 9,  R8G8_UNORM,          rg8,     IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_VIEW_DEPTH_B,           NUM_IMAGES_BASE + 10, R16_SFLOAT,          r32f,    IMG_WIDTH,           IMG_HEIGHT     ) \
	IMG_DO(PT_VIEW_DEPTH_A,           NUM_IMAGES_BASE + 11, R16_SFLOAT,          r32f,    IMG_WIDTH,           IMG_HEIGHT     ) \
	IMG_DO(PT_NORMAL_B,               NUM_IMAGES_BASE + 12, R32_UINT,            r32ui,   IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_NORMAL_A,               NUM_IMAGES_BASE + 13, R32_UINT,            r32ui,   IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_GEO_NORMAL_B,           NUM_IMAGES_BASE + 14, R32_UINT,            r32ui,   IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(PT_GEO_NORMAL_A,           NUM_IMAGES_BASE + 15, R32_UINT,            r32ui,   IMG_WIDTH_MGPU,      IMG_HEIGHT     ) \
	IMG_DO(ASVGF_TAA_B,               NUM_IMAGES_BASE + 16, R16G16B16A16_SFLOAT, rgba16f, IMG_WIDTH_TAA,       IMG_HEIGHT_TAA ) \
	IMG_DO(ASVGF_TAA_A,               NUM_IMAGES_BASE + 17, R16G16B16A16_SFLOAT, rgba16f, IMG_WIDTH_TAA,       IMG_HEIGHT_TAA ) \
	IMG_DO(ASVGF_RNG_SEED_B,          NUM_IMAGES_BASE + 18, R32_UINT,            r32ui,   IMG_WIDTH,           IMG_HEIGHT     ) \
	IMG_DO(ASVGF_RNG_SEED_A,          NUM_IMAGES_BASE + 19, R32_UINT,            r32ui,   IMG_WIDTH,           IMG_HEIGHT     ) \

#define NUM_IMAGES (NUM_IMAGES_BASE + 20) /* this really sucks but I don't know how to fix it
                                             counting with enum does not work in GLSL */

// todo: make naming consistent!
#define GLOBAL_TEXTURES_TEX_ARR_BINDING_IDX  0
#define BINDING_OFFSET_IMAGES     (1 + GLOBAL_TEXTURES_TEX_ARR_BINDING_IDX)
#define BINDING_OFFSET_TEXTURES   (BINDING_OFFSET_IMAGES + NUM_IMAGES)
#define BINDING_OFFSET_BLUE_NOISE (BINDING_OFFSET_TEXTURES + NUM_IMAGES)
#define BINDING_OFFSET_ENVMAP     (BINDING_OFFSET_BLUE_NOISE + 1)
#define BINDING_OFFSET_PHYSICAL_SKY (BINDING_OFFSET_ENVMAP + 1)
#define BINDING_OFFSET_PHYSICAL_SKY_IMG (BINDING_OFFSET_PHYSICAL_SKY + 1)
#define BINDING_OFFSET_SKY_TRANSMITTANCE (BINDING_OFFSET_PHYSICAL_SKY_IMG + 1)
#define BINDING_OFFSET_SKY_SCATTERING (BINDING_OFFSET_SKY_TRANSMITTANCE + 1)
#define BINDING_OFFSET_SKY_IRRADIANCE (BINDING_OFFSET_SKY_SCATTERING + 1)
#define BINDING_OFFSET_SKY_CLOUDS (BINDING_OFFSET_SKY_IRRADIANCE + 1)
#define BINDING_OFFSET_TERRAIN_ALBEDO (BINDING_OFFSET_SKY_CLOUDS + 1)
#define BINDING_OFFSET_TERRAIN_NORMALS (BINDING_OFFSET_TERRAIN_ALBEDO + 1)
#define BINDING_OFFSET_TERRAIN_DEPTH (BINDING_OFFSET_TERRAIN_NORMALS + 1)
#define BINDING_OFFSET_TERRAIN_SHADOWMAP (BINDING_OFFSET_TERRAIN_DEPTH + 1)


#ifndef VKPT_SHADER
/***************************************************************************/
/* HOST CODE                                                               */
/***************************************************************************/

#if MAX_RIMAGES != NUM_GLOBAL_TEXTURES
#error need to fix the constant here as well
#endif


enum QVK_IMAGES {
#define IMG_DO(_name, ...) \
	VKPT_IMG_##_name,
	LIST_IMAGES
	LIST_IMAGES_A_B
#undef IMG_DO
	NUM_VKPT_IMAGES
};

typedef char compile_time_check_num_images[(NUM_IMAGES == NUM_VKPT_IMAGES)*2-1];

#elif defined(GLOBAL_TEXTURES_DESC_SET_IDX)
/***************************************************************************/
/* SHADER CODE                                                             */
/***************************************************************************/

/* general texture array for world, etc */
layout(
	set = GLOBAL_TEXTURES_DESC_SET_IDX,
	binding = GLOBAL_TEXTURES_TEX_ARR_BINDING_IDX
) uniform sampler2D global_texture_descriptors[];

#define SAMPLER_r16ui   usampler2D
#define SAMPLER_r32ui   usampler2D
#define SAMPLER_rg32ui  usampler2D
#define SAMPLER_r32i    isampler2D
#define SAMPLER_r16f   sampler2D
#define SAMPLER_r32f    sampler2D
#define SAMPLER_rg32f   sampler2D
#define SAMPLER_rg16f   sampler2D
#define SAMPLER_rgba32f sampler2D
#define SAMPLER_rgba16f sampler2D
#define SAMPLER_rgba8   sampler2D
#define SAMPLER_r8      sampler2D
#define SAMPLER_rg8     sampler2D

#define IMAGE_r16ui   uimage2D
#define IMAGE_r32ui   uimage2D
#define IMAGE_rg32ui  uimage2D
#define IMAGE_r32i    iimage2D
#define IMAGE_r16f    image2D
#define IMAGE_r32f    image2D
#define IMAGE_rg32f   image2D
#define IMAGE_rg16f   image2D
#define IMAGE_rgba32f image2D
#define IMAGE_rgba16f image2D
#define IMAGE_rgba8   image2D
#define IMAGE_r8      image2D
#define IMAGE_rg8     image2D

/* framebuffer images */
#define IMG_DO(_name, _binding, _vkformat, _glslformat, _w, _h) \
	layout(set = GLOBAL_TEXTURES_DESC_SET_IDX, binding = BINDING_OFFSET_IMAGES + _binding, _glslformat) \
	uniform IMAGE_##_glslformat IMG_##_name;
LIST_IMAGES
LIST_IMAGES_A_B
#undef IMG_DO

/* framebuffer textures */
#define IMG_DO(_name, _binding, _vkformat, _glslformat, _w, _h) \
	layout(set = GLOBAL_TEXTURES_DESC_SET_IDX, binding = BINDING_OFFSET_TEXTURES + _binding) \
	uniform SAMPLER_##_glslformat TEX_##_name;
LIST_IMAGES
LIST_IMAGES_A_B
#undef IMG_DO

layout(
	set = GLOBAL_TEXTURES_DESC_SET_IDX,
	binding = BINDING_OFFSET_BLUE_NOISE
) uniform sampler2DArray TEX_BLUE_NOISE;

layout(
	set = GLOBAL_TEXTURES_DESC_SET_IDX,
	binding = BINDING_OFFSET_ENVMAP
) uniform samplerCube TEX_ENVMAP;

layout(
    set = GLOBAL_TEXTURES_DESC_SET_IDX,
    binding = BINDING_OFFSET_PHYSICAL_SKY
) uniform samplerCube TEX_PHYSICAL_SKY;

layout(
    set = GLOBAL_TEXTURES_DESC_SET_IDX,
    binding = BINDING_OFFSET_PHYSICAL_SKY_IMG,
    rgba16f
) uniform imageCube IMG_PHYSICAL_SKY;

//#precomputed_sky begin

layout(
	set = GLOBAL_TEXTURES_DESC_SET_IDX,
	binding = BINDING_OFFSET_SKY_TRANSMITTANCE
) uniform sampler2D TEX_SKY_TRANSMITTANCE;

layout(
	set = GLOBAL_TEXTURES_DESC_SET_IDX,
	binding = BINDING_OFFSET_SKY_SCATTERING
) uniform sampler3D TEX_SKY_SCATTERING;

layout(
	set = GLOBAL_TEXTURES_DESC_SET_IDX,
	binding = BINDING_OFFSET_SKY_IRRADIANCE
) uniform sampler2D TEX_SKY_IRRADIANCE;

layout(
	set = GLOBAL_TEXTURES_DESC_SET_IDX,
	binding = BINDING_OFFSET_SKY_CLOUDS
) uniform sampler3D TEX_SKY_CLOUDS;

layout(
	set = GLOBAL_TEXTURES_DESC_SET_IDX,
	binding = BINDING_OFFSET_TERRAIN_ALBEDO
) uniform samplerCube IMG_TERRAIN_ALBEDO;

layout(
	set = GLOBAL_TEXTURES_DESC_SET_IDX,
	binding = BINDING_OFFSET_TERRAIN_NORMALS
) uniform samplerCube IMG_TERRAIN_NORMALS;

layout(
	set = GLOBAL_TEXTURES_DESC_SET_IDX,
	binding = BINDING_OFFSET_TERRAIN_DEPTH
) uniform samplerCube IMG_TERRAIN_DEPTH;

layout(
	set = GLOBAL_TEXTURES_DESC_SET_IDX,
	binding = BINDING_OFFSET_TERRAIN_SHADOWMAP
) uniform sampler2D TEX_TERRAIN_SHADOWMAP;
//#precomputed_sky end

vec4
global_texture(uint idx, vec2 tex_coord)
{
	if(idx >= NUM_GLOBAL_TEXTURES)
		return vec4(1, 0, 1, 0);
	return texture(global_texture_descriptors[nonuniformEXT(idx)], tex_coord);
}

vec4
global_textureLod(uint idx, vec2 tex_coord, float lod)
{
	if(idx >= NUM_GLOBAL_TEXTURES)
		return vec4(1, 1, 0, 0);
	return textureLod(global_texture_descriptors[nonuniformEXT(idx)], tex_coord, lod);
}

vec4
global_textureGrad(uint idx, vec2 tex_coord, vec2 d_x, vec2 d_y)
{
	if(idx >= NUM_GLOBAL_TEXTURES)
		return vec4(1, 1, 0, 0);
	return textureGrad(global_texture_descriptors[nonuniformEXT(idx)], tex_coord, d_x, d_y);
}

ivec2
global_textureSize(uint idx, int level)
{
	if(idx >= NUM_GLOBAL_TEXTURES)
		return ivec2(0);
	return textureSize(global_texture_descriptors[nonuniformEXT(idx)], level);
}


#endif


#endif /*_TEXTURES_H_*/
// vim: shiftwidth=4 noexpandtab tabstop=4 cindent
