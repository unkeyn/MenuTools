// AMD FidelityFX Super Resolution (FSR) 1.0 - RCAS (MIT)
Texture2D<float4> InputTex : register(t0);
RWTexture2D<float4> OutputTex : register(u0);

SamplerState PointSampler : register(s0);
SamplerState LinearSampler : register(s1);

cbuffer ScalerCB : register(b0)
{
    float2 SourceSize;
    float2 SourceOffset;
    float2 TargetSize;
    float2 TargetOffset;
    float2 FullTargetSize;
    float2 SourceTexSize;
};

#define min3(a, b, c) min(a, min(b, c))
#define max3(a, b, c) max(a, max(b, c))
#define FSR_RCAS_LIMIT (0.25f - (1.0f / 16.0f))

static const float kSharpness = 0.85f;

float3 FsrRcasF(float3 b, float3 d, float3 e, float3 f, float3 h)
{
    float bR = b.r; float bG = b.g; float bB = b.b;
    float dR = d.r; float dG = d.g; float dB = d.b;
    float eR = e.r; float eG = e.g; float eB = e.b;
    float fR = f.r; float fG = f.g; float fB = f.b;
    float hR = h.r; float hG = h.g; float hB = h.b;

    // Luma times 2
    float bL = bB * 0.5f + (bR * 0.5f + bG);
    float dL = dB * 0.5f + (dR * 0.5f + dG);
    float eL = eB * 0.5f + (eR * 0.5f + eG);
    float fL = fB * 0.5f + (fR * 0.5f + fG);
    float hL = hB * 0.5f + (hR * 0.5f + hG);

    // Noise detection
    float nz = 0.25f * bL + 0.25f * dL + 0.25f * fL + 0.25f * hL - eL;
    float range = max3(max3(bL, dL, eL), fL, hL) - min3(min3(bL, dL, eL), fL, hL);
    nz = saturate(abs(nz) * ((range > 0.0f) ? (1.0f / range) : 0.0f));
    nz = -0.5f * nz + 1.0f;

    // Min and max of ring
    float mn4R = min(min3(bR, dR, fR), hR);
    float mn4G = min(min3(bG, dG, fG), hG);
    float mn4B = min(min3(bB, dB, fB), hB);
    float mx4R = max(max3(bR, dR, fR), hR);
    float mx4G = max(max3(bG, dG, fG), hG);
    float mx4B = max(max3(bB, dB, fB), hB);

    float2 peakC = float2(1.0f, -4.0f);
    float hitMinR = min(mn4R, eR) * (1.0f / max(4.0f * mx4R, 1e-5f));
    float hitMinG = min(mn4G, eG) * (1.0f / max(4.0f * mx4G, 1e-5f));
    float hitMinB = min(mn4B, eB) * (1.0f / max(4.0f * mx4B, 1e-5f));

    float hitMaxR = (peakC.x - max(mx4R, eR)) * (1.0f / max(4.0f * mn4R + peakC.y, 1e-5f));
    float hitMaxG = (peakC.x - max(mx4G, eG)) * (1.0f / max(4.0f * mn4G + peakC.y, 1e-5f));
    float hitMaxB = (peakC.x - max(mx4B, eB)) * (1.0f / max(4.0f * mn4B + peakC.y, 1e-5f));

    float lobeR = max(-hitMinR, hitMaxR);
    float lobeG = max(-hitMinG, hitMaxG);
    float lobeB = max(-hitMinB, hitMaxB);
    float lobe = max(-FSR_RCAS_LIMIT, min(max3(lobeR, lobeG, lobeB), 0.0f)) * kSharpness;

    lobe *= nz;

    float rcpL = 1.0f / (4.0f * lobe + 1.0f);
    return float3(
        (lobe * bR + lobe * dR + lobe * hR + lobe * fR + eR) * rcpL,
        (lobe * bG + lobe * dG + lobe * hG + lobe * fG + eG) * rcpL,
        (lobe * bB + lobe * dB + lobe * hB + lobe * fB + eB) * rcpL
    );
}

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)TargetSize.x || id.y >= (uint)TargetSize.y)
    {
        return;
    }

    int2 pos = int2(id.xy);
    float3 b = InputTex.Load(int3(pos.x, max(pos.y - 1, 0), 0)).rgb;
    float3 d = InputTex.Load(int3(max(pos.x - 1, 0), pos.y, 0)).rgb;
    float3 e = InputTex.Load(int3(pos.x, pos.y, 0)).rgb;
    float3 f = InputTex.Load(int3(min(pos.x + 1, (int)TargetSize.x - 1), pos.y, 0)).rgb;
    float3 h = InputTex.Load(int3(pos.x, min(pos.y + 1, (int)TargetSize.y - 1), 0)).rgb;

    float3 c = FsrRcasF(b, d, e, f, h);
    OutputTex[id.xy + uint2(TargetOffset)] = float4(saturate(c), 1.0f);
}
