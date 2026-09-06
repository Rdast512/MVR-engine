#ifndef ENGINE_CORE_BINDING_SLANG
#define ENGINE_CORE_BINDING_SLANG

#include "types.slang"

Texture2D tex2D(DescriptorHandle<Texture2D> handle)
{
    return getDescriptorFromHandle(handle);
}

SamplerState samplerOf(DescriptorHandle<SamplerState> handle)
{
    return getDescriptorFromHandle(handle);
}

T* bdaPtr<T>(uint64_t address)
{
    return reinterpret<T*>(address);
}

#endif
