#ifndef ENGINE_CORE_BINDING_SLANG
#define ENGINE_CORE_BINDING_SLANG

#include "types.slang"

// kNoneIndex is not a heap index: callers check it first (see sampleBaseColor)
Texture2D tex2D(uint heapIndex)
{
    return getDescriptorFromHandle(DescriptorHandle<Texture2D>(uint2(heapIndex, 0)));
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
