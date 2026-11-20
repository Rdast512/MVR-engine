import types;
import core.binding;

// alpha-tested bin (spec/m6/d3_alpha_raster_bin.md): the OMM bake's base-color alpha test, then the payload. The
// discard costs this bin early depth testing; the opaque bin (passes/mesh_fragment.slang) keeps it

[shader("fragment")]
uint alphaFragMain(MeshAlphaVertexOut vertIn, uint payload : SV_PrimitiveID) : SV_TARGET
{
    // implicit derivatives with the resolve's lodBias, so the cut follows the mip the shaded alpha reads
    const float alpha = tex2D(NonUniformResourceIndex(vertIn.baseColor.x))
        .SampleBias(samplerOf(NonUniformResourceIndex(vertIn.baseColor.y)), vertIn.uv, vertIn.alphaTest.y).a;
    if (alpha < vertIn.alphaTest.x)
    {
        discard;
    }
    return payload;
}
