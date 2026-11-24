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
public static const int kImageFormatR16f = 7;
public static const int kImageFormatRgba8 = 10;
public static const int kImageFormatRgba32ui = 30;
public static const int kImageFormatR32ui = 37;
public static const int kImageFormatR8ui = 39;

// format must match the view's VkFormat
public RWTexture2D<T, 0, format> storageTex2D<T : ITexelElement, let format : int>(uint heapIndex)
{
    return getDescriptorFromHandle(DescriptorHandle<RWTexture2D<T, 0, format>>(uint2(heapIndex, 0)));
}

public Texture2D<uint> uintTex2D(uint heapIndex)
{
    return getDescriptorFromHandle(DescriptorHandle<Texture2D<uint>>(uint2(heapIndex, 0)));
}

public Texture2D<uint4> uint4Tex2D(uint heapIndex)
{
    return getDescriptorFromHandle(DescriptorHandle<Texture2D<uint4>>(uint2(heapIndex, 0)));
}

public SamplerState samplerOf(uint heapIndex)
{
    return getDescriptorFromHandle(DescriptorHandle<SamplerState>(uint2(heapIndex, 0)));
}

public T* bdaPtr<T>(uint64_t address)
{
    return reinterpret<T*>(address);
}

// a BDA array read like a structured buffer under robust buffer access: zeros out of range, stores dropped. For vendor
// HLSL that relies on robustness (RTXDI, NRC bridges)
public struct CheckedBuffer<T>
{
    public uint64_t address;
    public uint count;

    public __subscript(uint index) -> T
    {
        get
        {
            if (index >= count)
            {
                T empty = {};
                return empty;
            }
            return bdaPtr<T>(address)[index];
        }
        set
        {
            if (index < count)
            {
                bdaPtr<T>(address)[index] = newValue;
            }
        }
    }
};
