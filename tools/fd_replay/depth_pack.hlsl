// fd_replay: a depth buffer resolved to a k_8_8_8_8 texture. The game rebuilds
// depth from the bytes, so the depth surface (R32_FLOAT, as rendered) is
// converted to the word the Xbox 360 copy writes: 24-bit depth << 8 | stencil,
// with 20e4 float depth for D24FS8 (Xenia's Float32To20e4, round to nearest
// even). Stencil is not kept and reads as 0.

Texture2D<float> g_Textures[] : register(t0, space0);

cbuffer PackConstants : register(b2, space4)
{
    uint g_DepthIndex; // heap index of the depth view
    uint g_FloatDepth; // 1: D24FS8 (20e4), 0: D24S8 (unorm24)
    uint g_ByteOrder;  // 0: X = bits 24-31 ... W = bits 0-7; 1: reversed
};

void vs_main(uint vid : SV_VertexID, out float4 pos : SV_Position)
{
    const float2 uv = float2((vid << 1) & 2, vid & 2);
    pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
}

uint Float32To20e4(float f)
{
    if (!(f > 0.0))
        return 0;
    uint u = asuint(f);
    if (u >= 0x3FFFFFF8)
        return 0xFFFFFF;
    if (u < 0x38800000)
        u = (0x800000 | (u & 0x7FFFFF)) >> min(113 - (u >> 23), 24u);
    else
        u += 0xC8000000u;
    u += 3 + ((u >> 3) & 1);
    return (u >> 3) & 0xFFFFFF;
}

float4 ps_main(float4 pos : SV_Position) : SV_Target
{
    const float depth = g_Textures[g_DepthIndex].Load(int3(pos.xy, 0));
    const uint depth24 = g_FloatDepth ? Float32To20e4(depth) : uint(saturate(depth) * 16777215.0 + 0.5);
    const uint word = depth24 << 8;
    const float4 bytes = float4(word >> 24, (word >> 16) & 0xFF, (word >> 8) & 0xFF, word & 0xFF) / 255.0;
    return g_ByteOrder ? bytes.wzyx : bytes;
}
