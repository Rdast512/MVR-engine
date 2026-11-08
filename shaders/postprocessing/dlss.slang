module dlss;

// jitter / exposure contract only — ngx is not slang

public struct DlssFrameContract
{
    public float2 jitterOffset;
    public float exposure;
    public float2 renderTargetSize;
};
