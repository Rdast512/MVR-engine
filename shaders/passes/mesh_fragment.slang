import types;

// opaque bin: no material access, no discard, so depth testing runs early

[shader("fragment")]
uint fragMain(MeshVertexOut vertIn, uint payload : SV_PrimitiveID) : SV_TARGET
{
    return payload;
}
