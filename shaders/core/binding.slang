#ifndef ENGINE_CORE_BINDING_SLANG
#define ENGINE_CORE_BINDING_SLANG

#include "types.slang"

// kNoneIndex is not a heap index: callers check it first (see material/pbr.slang sampleSlot)
Texture2D tex2D(uint heapIndex)
{
    return getDescriptorFromHandle(DescriptorHandle<Texture2D>(uint2(heapIndex, 0)));
}

RWTexture2D<float> storageTex2D(uint heapIndex)
{
    return getDescriptorFromHandle(DescriptorHandle<RWTexture2D<float>>(uint2(heapIndex, 0)));
}

// write-without-format: the format comes from the view (RenderTargets checks the format features)
RWTexture2D<float4> storageTex2D4(uint heapIndex)
{
    return getDescriptorFromHandle(DescriptorHandle<RWTexture2D<float4>>(uint2(heapIndex, 0)));
}

Texture2D<uint> uintTex2D(uint heapIndex)
{
    return getDescriptorFromHandle(DescriptorHandle<Texture2D<uint>>(uint2(heapIndex, 0)));
}

SamplerState samplerOf(uint heapIndex)
{
    return getDescriptorFromHandle(DescriptorHandle<SamplerState>(uint2(heapIndex, 0)));
}

T* bdaPtr<T>(uint64_t address)
{
    return reinterpret<T*>(address);
}

#endif
