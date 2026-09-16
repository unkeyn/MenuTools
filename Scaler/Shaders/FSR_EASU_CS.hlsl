// AMD FidelityFX Super Resolution (FSR) 1.0 - EASU (MIT)
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

void FsrEasuTapF(
    inout float3 aC,
    inout float aW,
    float2 off,
    float2 dir,
    float2 len,
    float lob,
    float clp,
    float3 c)
{
    float2 v;
    v.x = (off.x * dir.x) + (off.y * dir.y);
    v.y = (off.x * (-dir.y)) + (off.y * dir.x);
    v *= len;
    float d2 = v.x * v.x + v.y * v.y;
    d2 = min(d2, clp);
    float wB = (2.0f / 5.0f) * d2 - 1.0f;
    float wA = lob * d2 - 1.0f;
    wB *= wB;
    wA *= wA;
    wB = (25.0f / 16.0f) * wB - (25.0f / 16.0f - 1.0f);
    float w = wB * wA;
    aC += c * w;
    aW += w;
}

void FsrEasuSetF(
    inout float2 dir,
    inout float len,
    float2 pp,
    bool biS, bool biT, bool biU, bool biV,
    float lA, float lB, float lC, float lD, float lE)
{
    float w = 0.0f;
    if (biS) w = (1.0f - pp.x) * (1.0f - pp.y);
    if (biT) w = pp.x * (1.0f - pp.y);
    if (biU) w = (1.0f - pp.x) * pp.y;
    if (biV) w = pp.x * pp.y;

    float dc = lD - lC;
    float cb = lC - lB;
    float lenX = max(abs(dc), abs(cb));
    lenX = (lenX > 0.0f) ? (1.0f / lenX) : 0.0f;
    float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = saturate(abs(dirX) * lenX);
    lenX *= lenX;
    len += lenX * w;

    float ec = lE - lC;
    float ca = lC - lA;
    float lenY = max(abs(ec), abs(ca));
    lenY = (lenY > 0.0f) ? (1.0f / lenY) : 0.0f;
    float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = saturate(abs(dirY) * lenY);
    lenY *= lenY;
    len += lenY * w;
}

float3 FsrEasu(uint2 ip, float4 con0, float4 con1, float4 con2, float2 con3)
{
    float2 pp = float2(ip) * con0.xy + con0.zw;
    float2 fp = floor(pp);
    pp -= fp;

    float2 p0 = fp * con1.xy + con1.zw;
    float2 p1 = p0 + con2.xy;
    float2 p2 = p0 + con2.zw;
    float2 p3 = p0 + con3;

    float4 bczzR = InputTex.GatherRed(PointSampler, p0);
    float4 bczzG = InputTex.GatherGreen(PointSampler, p0);
    float4 bczzB = InputTex.GatherBlue(PointSampler, p0);

    float4 ijfeR = InputTex.GatherRed(PointSampler, p1);
    float4 ijfeG = InputTex.GatherGreen(PointSampler, p1);
    float4 ijfeB = InputTex.GatherBlue(PointSampler, p1);

    float4 klhgR = InputTex.GatherRed(PointSampler, p2);
    float4 klhgG = InputTex.GatherGreen(PointSampler, p2);
    float4 klhgB = InputTex.GatherBlue(PointSampler, p2);

    float4 zzonR = InputTex.GatherRed(PointSampler, p3);
    float4 zzonG = InputTex.GatherGreen(PointSampler, p3);
    float4 zzonB = InputTex.GatherBlue(PointSampler, p3);

    float4 bczzL = bczzB * 0.5f + (bczzR * 0.5f + bczzG);
    float4 ijfeL = ijfeB * 0.5f + (ijfeR * 0.5f + ijfeG);
    float4 klhgL = klhgB * 0.5f + (klhgR * 0.5f + klhgG);
    float4 zzonL = zzonB * 0.5f + (zzonR * 0.5f + zzonG);

    float bL = bczzL.x;
    float cL = bczzL.y;
    float iL = ijfeL.x;
    float jL = ijfeL.y;
    float fL = ijfeL.z;
    float eL = ijfeL.w;
    float kL = klhgL.x;
    float lL = klhgL.y;
    float hL = klhgL.z;
    float gL = klhgL.w;
    float oL = zzonL.z;
    float nL = zzonL.w;

    float2 dir = 0.0f;
    float len = 0.0f;
    FsrEasuSetF(dir, len, pp, true,  false, false, false, bL, eL, fL, gL, jL);
    FsrEasuSetF(dir, len, pp, false, true,  false, false, cL, fL, gL, hL, kL);
    FsrEasuSetF(dir, len, pp, false, false, true,  false, fL, iL, jL, kL, nL);
    FsrEasuSetF(dir, len, pp, false, false, false, true,  gL, jL, kL, lL, oL);

    float2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    bool zro = dirR < (1.0f / 32768.0f);
    dirR = rsqrt(max(dirR, 1.0f / 32768.0f));
    dirR = zro ? 1.0f : dirR;
    dir.x = zro ? 1.0f : dir.x;
    dir *= dirR;

    len = len * 0.5f;
    len *= len;
    float stretch = (dir.x * dir.x + dir.y * dir.y) * (1.0f / max(abs(dir.x), abs(dir.y)));
    float2 len2 = float2(1.0f + (stretch - 1.0f) * len, 1.0f - 0.5f * len);
    float lob = 0.5f + ((0.25f - 0.04f) - 0.5f) * len;
    float clp = 1.0f / lob;

    float3 min4 = min(min3(float3(ijfeR.z, ijfeG.z, ijfeB.z), float3(klhgR.w, klhgG.w, klhgB.w), float3(ijfeR.y, ijfeG.y, ijfeB.y)),
                      float3(klhgR.x, klhgG.x, klhgB.x));
    float3 max4 = max(max3(float3(ijfeR.z, ijfeG.z, ijfeB.z), float3(klhgR.w, klhgG.w, klhgB.w), float3(ijfeR.y, ijfeG.y, ijfeB.y)),
                      float3(klhgR.x, klhgG.x, klhgB.x));

    float3 aC = 0.0f;
    float aW = 0.0f;
    FsrEasuTapF(aC, aW, float2( 0.0f, -1.0f) - pp, dir, len2, lob, clp, float3(bczzR.x, bczzG.x, bczzB.x));
    FsrEasuTapF(aC, aW, float2( 1.0f, -1.0f) - pp, dir, len2, lob, clp, float3(bczzR.y, bczzG.y, bczzB.y));
    FsrEasuTapF(aC, aW, float2(-1.0f,  1.0f) - pp, dir, len2, lob, clp, float3(ijfeR.x, ijfeG.x, ijfeB.x));
    FsrEasuTapF(aC, aW, float2( 0.0f,  1.0f) - pp, dir, len2, lob, clp, float3(ijfeR.y, ijfeG.y, ijfeB.y));
    FsrEasuTapF(aC, aW, float2( 0.0f,  0.0f) - pp, dir, len2, lob, clp, float3(ijfeR.z, ijfeG.z, ijfeB.z));
    FsrEasuTapF(aC, aW, float2(-1.0f,  0.0f) - pp, dir, len2, lob, clp, float3(ijfeR.w, ijfeG.w, ijfeB.w));
    FsrEasuTapF(aC, aW, float2( 1.0f,  1.0f) - pp, dir, len2, lob, clp, float3(klhgR.x, klhgG.x, klhgB.x));
    FsrEasuTapF(aC, aW, float2( 2.0f,  1.0f) - pp, dir, len2, lob, clp, float3(klhgR.y, klhgG.y, klhgB.y));
    FsrEasuTapF(aC, aW, float2( 2.0f,  0.0f) - pp, dir, len2, lob, clp, float3(klhgR.z, klhgG.z, klhgB.z));
    FsrEasuTapF(aC, aW, float2( 1.0f,  0.0f) - pp, dir, len2, lob, clp, float3(klhgR.w, klhgG.w, klhgB.w));
    FsrEasuTapF(aC, aW, float2( 1.0f,  2.0f) - pp, dir, len2, lob, clp, float3(zzonR.z, zzonG.z, zzonB.z));
    FsrEasuTapF(aC, aW, float2( 0.0f,  2.0f) - pp, dir, len2, lob, clp, float3(zzonR.w, zzonG.w, zzonB.w));

    return min(max4, max(min4, aC * (1.0f / aW)));
}

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)TargetSize.x || id.y >= (uint)TargetSize.y)
    {
        return;
    }

    float4 con0;
    con0.x = SourceSize.x / TargetSize.x;
    con0.y = SourceSize.y / TargetSize.y;
    con0.z = 0.5f * con0.x - 0.5f;
    con0.w = 0.5f * con0.y - 0.5f;

    float2 invSrcTex = 1.0f / SourceTexSize;
    float4 con1;
    con1.x = invSrcTex.x;
    con1.y = invSrcTex.y;
    con1.z = SourceOffset.x * invSrcTex.x + invSrcTex.x;
    con1.w = SourceOffset.y * invSrcTex.y - invSrcTex.y;

    float4 con2 = float4(-invSrcTex.x, 2.0f * invSrcTex.y, invSrcTex.x, 2.0f * invSrcTex.y);
    float2 con3 = float2(0.0f, 4.0f * invSrcTex.y);

    float3 color = FsrEasu(id.xy, con0, con1, con2, con3);
    // Write to intermediate texture (unoffsetted)
    OutputTex[id.xy] = float4(saturate(color), 1.0f);
}
