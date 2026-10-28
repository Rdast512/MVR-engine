#ifndef ENGINE_GI_SHARC_SLANG
#define ENGINE_GI_SHARC_SLANG

#include "types.slang"
#include "gi/rtxgi.slang"

// one SHaRC mode per compile unit, set before this include:
//   passes/pathtrace_update.slang  SHARC_UPDATE (sparse paths write the cache)
//   passes/pathtrace_query.slang   SHARC_QUERY  (paths terminate into the cache)
//   passes/sharc_resolve.slang     neither
#ifndef SHARC_UPDATE
#define SHARC_UPDATE 0
#endif
#ifndef SHARC_QUERY
#define SHARC_QUERY 0
#endif
#if SHARC_UPDATE && SHARC_QUERY
#error SHARC_UPDATE and SHARC_QUERY are separate compile units
#endif

#define SHARC_ENABLE_GLSL 0
#ifndef SHARC_ENABLE_RESPONSIVE_LIGHTING
#define SHARC_ENABLE_RESPONSIVE_LIGHTING 0
#endif
// SHaRC buffers are reached through BDA like every other engine buffer
#define RW_STRUCTURED_BUFFER(name, type) type* name
#include "SharcCommon.h"

struct SharcHashEntry
{
    uint key;
    uint2 packed;
};

#endif
