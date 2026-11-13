module binding;

// kNoneIndex is not a heap index: callers check it first (see material/pbr.slang sampleSlot)
public Texture2D tex2D(uint heapIndex)
{
    return getDescriptorFromHandle(DescriptorHandle<Texture2D>(uint2(heapIndex, 0)));
}

// Slang ImageFormat values (SlangImageFormat in slang.h; tests/layout_test.cpp checks them). A storage image
// declares the format of its view, so writes need no StorageImageWriteWithoutFormat
public static const int kImageFormatRgba16f = 2;
public static const int kImageFormatRg16f = 4;
public static const int kImageFormatR11g11b10f = 5;
public static const int kImageFormatR32f = 6;
public static const int kImageFormatRgba8 = 10;
public static const int kImageFormatRg16Snorm = 17;

// format must match the view's VkFormat
public RWTexture2D<T, 0, format> storageTex2D<T : ITexelElement, let format : int>(uint heapIndex)
{
    return getDescriptorFromHandle(DescriptorHandle<RWTexture2D<T, 0, format>>(uint2(heapIndex, 0)));
}

public Texture2D<uint> uintTex2D(uint heapIndex)
{
    return getDescriptorFromHandle(DescriptorHandle<Texture2D<uint>>(uint2(heapIndex, 0)));
}

public SamplerState samplerOf(uint heapIndex)
{
    return getDescriptorFromHandle(DescriptorHandle<SamplerState>(uint2(heapIndex, 0)));
}

public T* bdaPtr<T>(uint64_t address)
{
    return reinterpret<T*>(address);
}
