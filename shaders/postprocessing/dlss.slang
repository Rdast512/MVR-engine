#ifndef ENGINE_POST_DLSS_SLANG
#define ENGINE_POST_DLSS_SLANG

#include "types.slang"

// jitter / exposure contract only — ngx is not slang

struct DlssFrameContract
{
    float2 jitterOffset;
    float exposure;
    float2 renderTargetSize;
};

#endif
