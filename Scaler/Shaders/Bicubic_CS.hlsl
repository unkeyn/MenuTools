// Bicubic Catmull-Rom Upscaler (MIT License)
// Mathematically complete 9-tap bilinear Catmull-Rom (B=0, C=0.5) with source crop clamping
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

    float2 coord = srcPixel - 0.5f;
    float2 i = floor(coord);
    float2 f = coord - i;
    float2 f2 = f * f;
    float2 f3 = f2 * f;

    float2 w0 = -0.5f * f3 + f2 - 0.5f * f;
    float2 w1 =  1.5f * f3 - 2.5f * f2 + 1.0f;
    float2 w2 = -1.5f * f3 + 2.0f * f2 + 0.5f * f;
    float2 w3 =  0.5f * f3 - 0.5f * f2;

    float2 w12 = w1 + w2;

    // Crop boundary clamping: prevent sampling outside [SourceOffset, SourceOffset + SourceSize]
    float2 minCoord = SourceOffset + 0.5f;
    float2 maxCoord = SourceOffset + SourceSize - 0.5f;

    float2 tc0  = clamp(i - 0.5f, minCoord, maxCoord) * invSrcTex;
    float2 tc12 = clamp(i + 0.5f + w2 / w12, minCoord, maxCoord) * invSrcTex;
    float2 tc3  = clamp(i + 2.5f, minCoord, maxCoord) * invSrcTex;

    float4 c = 
        InputTex.SampleLevel(LinearSampler, float2(tc0.x,  tc0.y),  0) * (w0.x  * w0.y)  +
        InputTex.SampleLevel(LinearSampler, float2(tc12.x, tc0.y),  0) * (w12.x * w0.y)  +
        InputTex.SampleLevel(LinearSampler, float2(tc3.x,  tc0.y),  0) * (w3.x  * w0.y)  +

        InputTex.SampleLevel(LinearSampler, float2(tc0.x,  tc12.y), 0) * (w0.x  * w12.y) +
        InputTex.SampleLevel(LinearSampler, float2(tc12.x, tc12.y), 0) * (w12.x * w12.y) +
        InputTex.SampleLevel(LinearSampler, float2(tc3.x,  tc12.y), 0) * (w3.x  * w12.y) +

        InputTex.SampleLevel(LinearSampler, float2(tc0.x,  tc3.y),  0) * (w0.x  * w3.y)  +
        InputTex.SampleLevel(LinearSampler, float2(tc12.x, tc3.y),  0) * (w12.x * w3.y)  +
        InputTex.SampleLevel(LinearSampler, float2(tc3.x,  tc3.y),  0) * (w3.x  * w3.y);

    OutputTex[id.xy + uint2(TargetOffset)] = float4(max(0.0f, c.rgb), 1.0f);
}
