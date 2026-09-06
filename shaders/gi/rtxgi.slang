#ifndef ENGINE_GI_RTXGI_SLANG
#define ENGINE_GI_RTXGI_SLANG

// cache mode switch (sharc / nrc / both). host sets define at compile.

#ifndef GI_CACHE_MODE
#define GI_CACHE_MODE 0
#endif

static const uint kGiCacheNone = 0;
static const uint kGiCacheSharc = 1;
static const uint kGiCacheNrc = 2;
static const uint kGiCacheBoth = 3;

#endif
