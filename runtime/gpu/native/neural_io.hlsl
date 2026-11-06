// DLSS 5 Neural Rendering input and output, as RTX Remix prepares them: the
// graded scene (sRGB-encoded SDR) goes to an FP16 texture with alpha 1 (the
// game's grading shader leaves its blue channel in alpha), and the result
// comes back into the scene's RGB (the pipeline masks the alpha write).
// Full-screen triangle from depth_pack.hlsl's vs_main; one texel per pixel.
Texture2D<float4> g_Textures[] : register(t0, space0);

cbuffer NeuralConstants : register(b2, space4)
{
    uint g_Source;   // heap index: the scene (to_neural) or NR's input copy (from_neural)
    uint g_Output;   // heap index: NR's output (from_neural)
    uint g_Mode;     // from_neural: 0 result, 1 result on the right half, 2 middle twice (before | after)
    uint g_Width;    // scene width
};

float4 ps_to_neural(float4 position : SV_Position) : SV_Target
{
    return float4(saturate(g_Textures[g_Source].Load(int3(position.xy, 0)).rgb), 1.0);
}

float4 ps_from_neural(float4 position : SV_Position) : SV_Target
{
    int2 texel = int2(position.xy);
    const int half = int(g_Width / 2), quarter = int(g_Width / 4);
    if (g_Mode == 1 && texel.x < half) discard;
    if (g_Mode == 2)
    {
        if (texel.x < half) return float4(g_Textures[g_Source].Load(int3(texel.x + quarter, texel.y, 0)).rgb, 1.0);
        texel.x -= quarter;
    }
    return float4(saturate(g_Textures[g_Output].Load(int3(texel, 0)).rgb), 1.0);
}
