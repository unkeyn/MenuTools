// Anime4K 3D AA Upscaler - Pass 3 (MIT License - bloc97/Anime4K)
Texture2D<float4> InputTex : register(t0);
Texture2D<float4> ConvTex : register(t1);
RWTexture2D<float4> OutputTex2x : register(u0);

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
    if (id.x >= (uint)SourceSize.x || id.y >= (uint)SourceSize.y) return;

    int2 base = int2(id.xy);
    float4 src[3][3];
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            int2 p = clamp(base + int2(x, y), 0, int2(SourceSize) - 1);
            src[x + 1][y + 1] = max(ConvTex.Load(int3(p, 0)), 0.0f);
        }
    }

    float4 res = float4(-3.1127936e-05, 3.3726166e-05, 4.8580805e-05, -9.541029e-06);
    res += mul(src[0][0], float4x4(-0.00055252935, 0.0011350953, -0.0016148019, 0.0014946404, -0.30635214, -0.017596753, -0.0036547943, 0.016236471, 0.005174489, 0.0030302007, 0.00019672248, 0.0006430973, 0.0007490077, -0.0031795658, -6.158733e-05, 0.0006820584));
    res += mul(src[0][1], float4x4(0.15602079, 0.011071071, -0.0027609533, -0.0034318874, -0.0039016667, 0.016504101, -0.27816474, -0.008282344, 0.19063498, 0.012465078, 0.010091085, -0.004841106, -0.11758087, -0.012808949, 0.0067606894, 0.005216566));
    res += mul(src[0][2], float4x4(0.013258877, -0.014989483, 0.22402754, 0.013204027, 0.00016207264, -0.00042593342, -0.00333761, -0.0012207513, 0.0033727325, -0.007841196, 0.16044731, 0.00594871, -0.0028581345, 0.012616562, -0.15928285, -0.011812331));
    res += mul(src[1][0], float4x4(-0.0048872055, -0.0011780986, -0.0029523429, 0.00082424335, -0.0024385185, -0.26525813, 0.013532772, -0.0008381766, 0.0024996721, 0.0022899017, -0.0017697349, -0.0010618394, 0.0024938583, 0.005421073, 0.0028740794, -0.007808829));
    res += mul(src[1][1], float4x4(-0.08293415, 0.2659366, -0.010839574, 0.023423964, 0.01725351, -0.009252893, -0.011632222, -0.308242, 0.0001496815, 0.16104282, -0.0069378703, 0.00842848, 0.085917845, -0.18407243, -0.006601597, -0.027134055));
    res += mul(src[1][2], float4x4(-0.033873428, -0.011743531, -0.230377, 0.116242796, -0.0018527015, -0.00853698, 0.0059901997, -0.006155517, -0.009841329, 0.006163952, 0.014816026, 0.18667653, 0.016977048, -0.0017093032, 0.19695279, -0.061764043));
    res += mul(src[2][0], float4x4(-0.0003514533, -0.0069080726, 0.0052108583, -0.0016346197, -0.0016860099, 0.006002445, -0.0022835485, -0.0028219873, 0.0005367275, 0.0005437954, 0.00059865275, -0.00014915364, -0.0032214937, -0.00052043283, -0.0031621973, 0.0055843857));
    res += mul(src[2][1], float4x4(-0.006905302, -0.20389622, 0.01891904, -0.018114902, 0.00724176, 0.011335843, -0.0028616642, 0.016452003, -0.00013852821, -0.00039706306, 0.0011838446, 0.0028873065, 0.012857878, 0.16889338, -0.014114007, 0.009388666));
    res += mul(src[2][2], float4x4(0.0040798862, 0.002933288, -0.016012201, -0.14650294, -0.0017411204, 0.0017980475, 0.00056705566, -0.0003218331, -0.0014291195, -0.0062614805, 0.00082543516, -0.00397049, -0.004496662, 0.0008032309, 0.0049529593, 0.117166765));

    float2 invSrcTex = 1.0f / SourceTexSize;
    float2 pBase = float2(id.xy) + SourceOffset;

    float3 in00 = InputTex.SampleLevel(LinearSampler, (pBase + float2(0.25f, 0.25f)) * invSrcTex, 0).rgb;
    float3 in10 = InputTex.SampleLevel(LinearSampler, (pBase + float2(0.75f, 0.25f)) * invSrcTex, 0).rgb;
    float3 in01 = InputTex.SampleLevel(LinearSampler, (pBase + float2(0.25f, 0.75f)) * invSrcTex, 0).rgb;
    float3 in11 = InputTex.SampleLevel(LinearSampler, (pBase + float2(0.75f, 0.75f)) * invSrcTex, 0).rgb;

    OutputTex2x[id.xy * 2 + uint2(0, 0)] = float4(saturate(res.x + in00), 1.0f);
    OutputTex2x[id.xy * 2 + uint2(1, 0)] = float4(saturate(res.y + in10), 1.0f);
    OutputTex2x[id.xy * 2 + uint2(0, 1)] = float4(saturate(res.z + in01), 1.0f);
    OutputTex2x[id.xy * 2 + uint2(1, 1)] = float4(saturate(res.w + in11), 1.0f);
}
