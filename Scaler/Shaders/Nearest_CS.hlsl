// Nearest Neighbor Upscaler (Ultra-low GPU overhead, 1 load -> 1 store)
Texture2D<float4> InputTex : register(t0);
RWTexture2D<float4> OutputTex : register(u0);

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
    uint2 dst = id.xy;

    if (dst.x >= (uint)TargetSize.x || dst.y >= (uint)TargetSize.y)
    {
        return;
    }

    float2 src = (float2(dst) + 0.5f) * float2(SourceSize) / float2(TargetSize);
    uint2 srcPixel = (uint2)src + (uint2)SourceOffset;

    srcPixel = clamp(
        srcPixel,
        (uint2)SourceOffset,
        (uint2)(SourceOffset + SourceSize - 1.0f));

    OutputTex[dst + (uint2)TargetOffset] = InputTex.Load(int3(srcPixel, 0));
}