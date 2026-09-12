#include "fsr3_vkpt_blobs.h"

#include <string.h>

#include "fsr3_vkpt_generated.h"

namespace {
constexpr uint32_t kKnownOptions = 0xffu;

static void clear_blob(FfxShaderBlob* blob) { memset(blob, 0, sizeof(*blob)); }

static void copy_bindings(FfxShaderBlob* blob, const Fsr3VkptGeneratedBlob* row)
{
    blob->cbvCount = row->cbv.count;
    blob->srvTextureCount = row->srvTextures.count;
    blob->uavTextureCount = row->uavTextures.count;
    blob->srvBufferCount = row->srvBuffers.count;
    blob->uavBufferCount = row->uavBuffers.count;
    blob->samplerCount = row->samplers.count;
    blob->boundConstantBufferNames = row->cbv.names;
    blob->boundConstantBuffers = row->cbv.slots;
    blob->boundConstantBufferCounts = row->cbv.counts;
    blob->boundConstantBufferSpaces = row->cbv.spaces;
    blob->boundSRVTextureNames = row->srvTextures.names;
    blob->boundSRVTextures = row->srvTextures.slots;
    blob->boundSRVTextureCounts = row->srvTextures.counts;
    blob->boundSRVTextureSpaces = row->srvTextures.spaces;
    blob->boundUAVTextureNames = row->uavTextures.names;
    blob->boundUAVTextures = row->uavTextures.slots;
    blob->boundUAVTextureCounts = row->uavTextures.counts;
    blob->boundUAVTextureSpaces = row->uavTextures.spaces;
    blob->boundSRVBufferNames = row->srvBuffers.names;
    blob->boundSRVBuffers = row->srvBuffers.slots;
    blob->boundSRVBufferCounts = row->srvBuffers.counts;
    blob->boundSRVBufferSpaces = row->srvBuffers.spaces;
    blob->boundUAVBufferNames = row->uavBuffers.names;
    blob->boundUAVBuffers = row->uavBuffers.slots;
    blob->boundUAVBufferCounts = row->uavBuffers.counts;
    blob->boundUAVBufferSpaces = row->uavBuffers.spaces;
    blob->boundSamplerNames = row->samplers.names;
    blob->boundSamplers = row->samplers.slots;
    blob->boundSamplerCounts = row->samplers.counts;
    blob->boundSamplerSpaces = row->samplers.spaces;
}
}

extern "C" FfxErrorCode fsr3UpscalerGetPermutationBlobByIndex(
    FfxFsr3UpscalerPass passId, uint32_t permutationOptions, FfxShaderBlob* outBlob)
{
    if (!outBlob)
        return FFX_ERROR_INVALID_POINTER;
    clear_blob(outBlob);
    if (passId < 0 || passId >= FFX_FSR3UPSCALER_PASS_COUNT || passId == FFX_FSR3UPSCALER_PASS_TCR_AUTOGENERATE)
        return FFX_ERROR_INVALID_ENUM;
    if (permutationOptions & ~kKnownOptions)
        return FFX_ERROR_INVALID_ARGUMENT;

    for (uint32_t i = 0; i < fsr3_vkpt_generated_blob_count; ++i) {
        const Fsr3VkptGeneratedBlob* row = &fsr3_vkpt_generated_blobs[i];
        if (row->pass == static_cast<uint32_t>(passId) && row->options == permutationOptions) {
            outBlob->data = row->bytes;
            outBlob->size = row->size;
            outBlob->entryName = row->entry;
            copy_bindings(outBlob, row);
            return FFX_OK;
        }
    }
    return FFX_ERROR_INVALID_ARGUMENT; /* valid pass, but unsupported permutation */
}
