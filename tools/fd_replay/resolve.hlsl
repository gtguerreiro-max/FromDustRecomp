#pragma target 6.0

struct VSOut
{
    float4 pos : SV_POSITION;
};

VSOut vs_main(uint vid : SV_VertexID)
{
    float2 uv = float2(float((vid << 1) & 2), float(vid & 2));
    VSOut vsOut;
    vsOut.pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    return vsOut;
}

Texture2D<float4> g_Source : register(t0);

cbuffer ResolveConstants : register(b0)
{
    float g_ExpBiasFactor;
    uint  g_Swap;
    uint  g_ScaleAlpha;
};

float4 ps_main(VSOut input) : SV_TARGET
{
    float4 color = g_Source.Load(int3(input.pos.xy, 0));

    float4 result = color;
    result.rgb *= g_ExpBiasFactor;

    if (g_ScaleAlpha != 0)
    {
        result.a *= g_ExpBiasFactor;
    }

    if (g_Swap != 0)
    {
        result = result.bgra;
    }

    return result;
}
