#include "fsr3_vkpt_blobs.h"

#include <assert.h>

int main()
{
    FfxShaderBlob blob = {};
    assert(fsr3UpscalerGetPermutationBlobByIndex(FFX_FSR3UPSCALER_PASS_PREPARE_INPUTS, 7, &blob) == FFX_OK);
    assert(blob.data != nullptr && blob.size != 0 && blob.entryName != nullptr);
    assert(blob.cbvCount + blob.srvTextureCount + blob.uavTextureCount +
           blob.srvBufferCount + blob.uavBufferCount + blob.samplerCount != 0);
    assert(fsr3UpscalerGetPermutationBlobByIndex(FFX_FSR3UPSCALER_PASS_ACCUMULATE_SHARPEN, 39, &blob) == FFX_OK);
    assert(blob.data != nullptr && blob.size != 0);
    assert(fsr3UpscalerGetPermutationBlobByIndex(FFX_FSR3UPSCALER_PASS_COUNT, 0, &blob) == FFX_ERROR_INVALID_ENUM);
    assert(fsr3UpscalerGetPermutationBlobByIndex(FFX_FSR3UPSCALER_PASS_RCAS, 0x100, &blob) == FFX_ERROR_INVALID_ARGUMENT);
    assert(fsr3UpscalerGetPermutationBlobByIndex(FFX_FSR3UPSCALER_PASS_RCAS, 0, nullptr) == FFX_ERROR_INVALID_POINTER);
    return 0;
}
