// runs material/brdf.slang on known surfaces; tests/brdf_test.cpp checks the results
import core.math;
import material.brdf;

// scalar layout: must match BrdfCase in tests/brdf_test.cpp
struct BrdfCase
{
    float3 n;
    float roughness;
    float3 v;
    float pad0;
    float3 l;
    float pad1;
    float3 cDiff;
    float pad2;
    float3 f0;
    float pad3;
};

// scalar layout: must match BrdfPush in tests/brdf_test.cpp
struct BrdfPush
{
    BrdfCase* cases;
    float4* results; // kResultRows per case
    uint caseCount;
    uint sampleCount;
};

static const uint kResultRows = 5;

[[vk::push_constant]] ConstantBuffer<BrdfPush> push;

// pcg hash: a fixed seed keeps the estimates deterministic
float nextRandom(inout uint state)
{
    state = state * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return float((word >> 22u) ^ word) * (1.0 / 4294967296.0);
}

float3 uniformHemisphere(float3 n, float2 u)
{
    float3 t;
    float3 b;
    buildOrthonormalBasis(n, t, b);
    const float phi = 2.0 * kPi * u.x;
    const float radius = sqrt(saturate(1.0 - u.y * u.y));
    return t * radius * cos(phi) + b * radius * sin(phi) + n * u.y;
}

float maxRelError(float3 actual, float3 expected)
{
    const float3 relative = abs(actual - expected) / max(abs(expected), float3(1e-6, 1e-6, 1e-6));
    return max(relative.x, max(relative.y, relative.z));
}

[shader("compute")]
[numthreads(64, 1, 1)]
void brdfMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= push.caseCount)
    {
        return;
    }
    const BrdfCase c = push.cases[id.x];
    const float count = float(push.sampleCount);
    const bool isViewAbove = dot(c.n, c.v) >= 1e-4;

    float3 sampledSum = float3(0.0, 0.0, 0.0);
    float3 uniformSum = float3(0.0, 0.0, 0.0);
    float pdfSum = 0.0;
    float worstPdfError = 0.0;
    float worstWeightError = 0.0;
    float worstReciprocityError = 0.0;
    uint invalidCount = 0;
    uint nonFiniteCount = 0;
    uint state = (id.x + 1u) * 2654435761u;
    for (uint i = 0; i < push.sampleCount; ++i)
    {
        const float3 u = float3(nextRandom(state), nextRandom(state), nextRandom(state));
        const BrdfSample s = sampleBrdf(c.n, c.cDiff, c.f0, c.roughness, c.v, u);
        if (s.pdf > 0.0)
        {
            const float nDotL = dot(c.n, s.l);
            const float pdf = pdfBrdf(c.n, c.cDiff, c.f0, c.roughness, c.v, s.l);
            worstPdfError = max(worstPdfError, abs(pdf - s.pdf) / s.pdf);
            const float3 forward = evalBrdf(c.n, c.cDiff, c.f0, c.roughness, c.v, s.l);
            const float3 expected = forward * nDotL / s.pdf;
            worstWeightError = max(worstWeightError, maxRelError(s.weight, expected));
            // helmholtz reciprocity holds when both v and l sit above the shading normal
            if (isViewAbove && nDotL >= 1e-4)
            {
                const float3 reverse = evalBrdf(c.n, c.cDiff, c.f0, c.roughness, s.l, c.v);
                worstReciprocityError = max(worstReciprocityError, maxRelError(forward, reverse));
            }
            if (any(isnan(s.weight)) || any(isinf(s.weight)) || isnan(s.pdf) || isinf(s.pdf) || nDotL <= 0.0)
            {
                ++nonFiniteCount;
            }
            sampledSum += s.weight;
        }
        else
        {
            ++invalidCount;
        }

        // integrates f and the pdf without using the sampler under test
        const float3 l = uniformHemisphere(c.n, float2(nextRandom(state), nextRandom(state)));
        uniformSum += evalBrdf(c.n, c.cDiff, c.f0, c.roughness, c.v, l) * dot(c.n, l) * 2.0 * kPi;
        pdfSum += pdfBrdf(c.n, c.cDiff, c.f0, c.roughness, c.v, l) * 2.0 * kPi;
    }

    const uint row = id.x * kResultRows;
    push.results[row] = float4(evalBrdf(c.n, c.cDiff, c.f0, c.roughness, c.v, c.l) * max(dot(c.n, c.l), 0.0),
                               pdfBrdf(c.n, c.cDiff, c.f0, c.roughness, c.v, c.l));
    push.results[row + 1] = float4(sampledSum / count, worstPdfError);
    push.results[row + 2] = float4(uniformSum / count, pdfSum / count);
    push.results[row + 3] =
        float4(worstWeightError, float(invalidCount) / count, float(nonFiniteCount), worstReciprocityError);
    push.results[row + 4] = float4(envBrdfApprox(c.f0, max(dot(c.n, c.v), 1e-4), c.roughness), 0.0);
}
