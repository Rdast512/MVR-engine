#ifndef ENGINE_MATERIAL_PBR_SLANG
#define ENGINE_MATERIAL_PBR_SLANG

#include "types.slang"
#include "core/binding.slang"

float4 sampleAlbedo(MeshPushData push, float2 uv)
{
    Texture2D texture = tex2D(push.texture);
    SamplerState samplerState = samplerOf(push.samplerHandle);
    return texture.Sample(samplerState, uv);
}

#endif
