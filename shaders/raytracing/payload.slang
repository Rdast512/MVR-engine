#ifndef ENGINE_RT_PAYLOAD_SLANG
#define ENGINE_RT_PAYLOAD_SLANG

struct RtPayload
{
    float3 color;
    float hitT;
    uint instanceId;
    uint primitiveId;
    uint clusterId;
};

#endif
