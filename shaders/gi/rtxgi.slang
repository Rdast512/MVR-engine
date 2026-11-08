module rtxgi;

// cache mode switch (sharc / nrc / both). host sets define at compile.

#ifndef GI_CACHE_MODE
#define GI_CACHE_MODE 0
#endif

public static const uint kGiCacheNone = 0;
public static const uint kGiCacheSharc = 1;
public static const uint kGiCacheNrc = 2;
public static const uint kGiCacheBoth = 3;
