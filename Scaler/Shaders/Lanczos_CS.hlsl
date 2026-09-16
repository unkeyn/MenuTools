// Lanczos-2 Upscaler (MIT)
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

static const float PI = 3.14159265358979323846f;

float Sinc(float x)
{
    if (abs(x) < 1e-4f) return 1.0f;
    float px = x * PI;
    return sin(px) / px;
}

float Lanczos2Weight(float x)
{
    if (abs(x) >= 2.0f) return 0.0f;
    return Sinc(x) * Sinc(x * 0.5f);
}

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)TargetSize.x || id.y >= (uint)TargetSize.y)
    {
        return;
    }

    float2 uv = (float2(id.xy) + 0.5f) / TargetSize;
    float2 srcPixel = uv * SourceSize + SourceOffset;
    float2 invSrcTex = 1.0f / SourceTexSize;

    float2 center = floor(srcPixel - 0.5f) + 0.5f;
    float2 diff = srcPixel - center;

    float4 sumColor = 0.0f;
    float sumWeight = 0.0f;

    [unroll]
    for (int y = -1; y <= 2; ++y)
    {
        float wy = Lanczos2Weight(diff.y - float(y));
        [unroll]
        for (int x = -1; x <= 2; ++x)
        {
            float wx = Lanczos2Weight(diff.x - float(x));
            float w = wx * wy;

            float2 sampleCoord = (center + float2(x, y)) * invSrcTex;
            // Clamp to source crop boundaries
            float2 minCoord = SourceOffset * invSrcTex;
            float2 maxCoord = (SourceOffset + SourceSize - 1.0f) * invSrcTex;
            sampleCoord = clamp(sampleCoord, minCoord, maxCoord);

            float4 c = InputTex.SampleLevel(PointSampler, sampleCoord, 0);
            sumColor += c * w;
            sumWeight += w;
        }
    }

    float4 finalColor = (sumWeight > 0.0f) ? (sumColor / sumWeight) : float4(0, 0, 0, 1);
    OutputTex[id.xy + uint2(TargetOffset)] = float4(saturate(finalColor.rgb), 1.0f);
}
