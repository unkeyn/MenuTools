import re

def parse_matrices(file_path):
    with open(file_path, "r", encoding="utf-8") as f:
        content = f.read()

    # Pass 1:
    p1_match = re.search(r'void Pass1[^{]+{([\s\S]+?)\n}', content)
    p1_body = p1_match.group(1)
    p1_bias = re.search(r'MF4 result = { ([^}]+) };', p1_body).group(1)
    p1_muls = re.findall(r'result = MulAdd\(src\[([^\]]+)\]\[([^\]]+)\], (MF3x4\([^)]+\)), result\);', p1_body)

    # Pass 2:
    p2_match = re.search(r'void Pass2[^{]+{([\s\S]+?)\n}', content)
    p2_body = p2_match.group(1)
    p2_bias = re.search(r'MF4 result = { ([^}]+) };', p2_body).group(1)
    p2_pos_muls = re.findall(r'result = MulAdd\(max\(src\[([^\]]+)\]\[([^\]]+)\], 0\), (MF4x4\([^)]+\)), result\);', p2_body)
    p2_neg_muls = re.findall(r'result = MulAdd\(max\(-src\[([^\]]+)\]\[([^\]]+)\], 0\), (MF4x4\([^)]+\)), result\);', p2_body)

    # Pass 3:
    p3_match = re.search(r'void Pass3[^{]+{([\s\S]+?)\n}', content)
    p3_body = p3_match.group(1)
    p3_bias = re.search(r'MF4 result = { ([^}]+) };', p3_body).group(1)
    p3_muls = re.findall(r'result = MulAdd\(max\(([a-i]), 0\), (MF4x4\([^)]+\)), result\);', p3_body)

    return {
        "p1_bias": p1_bias,
        "p1_muls": p1_muls,
        "p2_bias": p2_bias,
        "p2_pos": p2_pos_muls,
        "p2_neg": p2_neg_muls,
        "p3_bias": p3_bias,
        "p3_muls": p3_muls
    }

def generate_hlsl(data, prefix, title):
    # Pass 1
    p1_code = f"""// {title} - Pass 1 (MIT License - bloc97/Anime4K)
Texture2D<float4> InputTex : register(t0);
RWTexture2D<float4> OutputTex : register(u0);

cbuffer ScalerCB : register(b0)
{{
    float2 SourceSize;
    float2 SourceOffset;
    float2 TargetSize;
    float2 TargetOffset;
    float2 FullTargetSize;
    float2 SourceTexSize;
}};

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID)
{{
    if (id.x >= (uint)SourceSize.x || id.y >= (uint)SourceSize.y) return;

    int2 base = int2(id.xy);
    float3 src[3][3];
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {{
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {{
            int2 p = clamp(base + int2(x, y), 0, int2(SourceSize) - 1);
            src[x + 1][y + 1] = InputTex.Load(int3(p + int2(SourceOffset), 0)).rgb;
        }}
    }}

    float4 res = float4({data['p1_bias']});
"""
    # p1_muls format: (i-1, j-1, mat)
    # in original: src[i-1][j-1]
    for i_str, j_str, mat in data['p1_muls']:
        # e.g. "i - 1", "j - 1"
        i_idx = 0 if "- 1" in i_str else (1 if i_str == "i" else 2)
        j_idx = 0 if "- 1" in j_str else (1 if j_str == "j" else 2)
        mat_hlsl = mat.replace("MF3x4", "float3x4")
        p1_code += f"    res += mul(src[{i_idx}][{j_idx}], {mat_hlsl});\n"
    p1_code += "\n    OutputTex[id.xy] = res;\n}\n"

    with open(f"c:/Projects/MenuTools/Scaler/Shaders/{prefix}_Pass1_CS.hlsl", "w") as f:
        f.write(p1_code)

    # Pass 2
    p2_code = f"""// {title} - Pass 2 (MIT License - bloc97/Anime4K)
Texture2D<float4> InputTex : register(t0);
RWTexture2D<float4> OutputTex : register(u0);

cbuffer ScalerCB : register(b0)
{{
    float2 SourceSize;
    float2 SourceOffset;
    float2 TargetSize;
    float2 TargetOffset;
    float2 FullTargetSize;
    float2 SourceTexSize;
}};

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID)
{{
    if (id.x >= (uint)SourceSize.x || id.y >= (uint)SourceSize.y) return;

    int2 base = int2(id.xy);
    float4 srcPos[3][3];
    float4 srcNeg[3][3];
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {{
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {{
            int2 p = clamp(base + int2(x, y), 0, int2(SourceSize) - 1);
            float4 v = InputTex.Load(int3(p, 0));
            srcPos[x + 1][y + 1] = max(v, 0.0f);
            srcNeg[x + 1][y + 1] = max(-v, 0.0f);
        }}
    }}

    float4 res = float4({data['p2_bias']});
"""
    for i_str, j_str, mat in data['p2_pos']:
        i_idx = 0 if "- 1" in i_str else (1 if i_str == "i" else 2)
        j_idx = 0 if "- 1" in j_str else (1 if j_str == "j" else 2)
        mat_hlsl = mat.replace("MF4x4", "float4x4")
        p2_code += f"    res += mul(srcPos[{i_idx}][{j_idx}], {mat_hlsl});\n"
    for i_str, j_str, mat in data['p2_neg']:
        i_idx = 0 if "- 1" in i_str else (1 if i_str == "i" else 2)
        j_idx = 0 if "- 1" in j_str else (1 if j_str == "j" else 2)
        mat_hlsl = mat.replace("MF4x4", "float4x4")
        p2_code += f"    res += mul(srcNeg[{i_idx}][{j_idx}], {mat_hlsl});\n"
    p2_code += "\n    OutputTex[id.xy] = res;\n}\n"

    with open(f"c:/Projects/MenuTools/Scaler/Shaders/{prefix}_Pass2_CS.hlsl", "w") as f:
        f.write(p2_code)

    # Pass 3
    # mapping of letters a..i:
    # a: (-1, -1), b: (-1, 0), c: (-1, 1)
    # d: ( 0, -1), e: ( 0, 0), f: ( 0, 1)
    # g: ( 1, -1), h: ( 1, 0), i: ( 1, 1)
    letter_map = {
        'a': (0, 0), 'b': (0, 1), 'c': (0, 2),
        'd': (1, 0), 'e': (1, 1), 'f': (1, 2),
        'g': (2, 0), 'h': (2, 1), 'i': (2, 2)
    }

    p3_code = f"""// {title} - Pass 3 (MIT License - bloc97/Anime4K)
Texture2D<float4> InputTex : register(t0);
Texture2D<float4> ConvTex : register(t1);
RWTexture2D<float4> OutputTex2x : register(u0);

SamplerState LinearSampler : register(s1);

cbuffer ScalerCB : register(b0)
{{
    float2 SourceSize;
    float2 SourceOffset;
    float2 TargetSize;
    float2 TargetOffset;
    float2 FullTargetSize;
    float2 SourceTexSize;
}};

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID)
{{
    if (id.x >= (uint)SourceSize.x || id.y >= (uint)SourceSize.y) return;

    int2 base = int2(id.xy);
    float4 src[3][3];
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {{
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {{
            int2 p = clamp(base + int2(x, y), 0, int2(SourceSize) - 1);
            src[x + 1][y + 1] = max(ConvTex.Load(int3(p, 0)), 0.0f);
        }}
    }}

    float4 res = float4({data['p3_bias']});
"""
    for letter, mat in data['p3_muls']:
        xi, yi = letter_map[letter]
        mat_hlsl = mat.replace("MF4x4", "float4x4")
        p3_code += f"    res += mul(src[{xi}][{yi}], {mat_hlsl});\n"

    p3_code += """
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
"""
    with open(f"c:/Projects/MenuTools/Scaler/Shaders/{prefix}_Pass3_CS.hlsl", "w") as f:
        f.write(p3_code)

d_3d = parse_matrices("c:/Projects/MenuTools/scratch_anime4k_3d.hlsl")
generate_hlsl(d_3d, "Anime4K_3D", "Anime4K 3D Upscaler")

d_3d_aa = parse_matrices("c:/Projects/MenuTools/scratch_anime4k_3d_aa.hlsl")
generate_hlsl(d_3d_aa, "Anime4K_3D_AA", "Anime4K 3D AA Upscaler")

print("Generated Anime4K shaders successfully!")
