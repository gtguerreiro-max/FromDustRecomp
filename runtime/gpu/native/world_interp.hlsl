// World interpolation: the simulation's height field (terrain and water
// heights, written by the CPU once per world step at 30 Hz) blended between
// the previous and the current step for every rendered frame, so the terrain
// and the water move at the frame rate instead of in 30 Hz steps.
// Full-screen triangle from depth_pack.hlsl's vs_main; one texel per pixel.
Texture2D<float4> g_Textures[] : register(t0, space0);

cbuffer InterpConstants : register(b2, space4)
{
    uint g_Previous; // heap index of the previous step's copy
    uint g_Current;  // heap index of the current step's copy
    float g_Alpha;   // 0: previous, 1: current
    uint g_Pad;
};

float4 ps_main(float4 position : SV_Position) : SV_Target
{
    const int3 texel = int3(position.xy, 0);
    return lerp(g_Textures[g_Previous].Load(texel), g_Textures[g_Current].Load(texel), g_Alpha);
}
