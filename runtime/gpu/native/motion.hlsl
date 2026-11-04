// Camera motion vectors and linear-format depth for DLSS. Each scene pixel is
// reprojected from its depth with this frame's and the previous frame's
// world-to-clip matrices (the terrain shaders' VS_DustConst[8..11]). Moving
// objects and deforming water/terrain only get camera motion.
// Motion is in render pixels from the current to the previous position.

Texture2D<float> g_Textures[] : register(t0, space0);
Texture2DMS<float> g_DepthMS[] : register(t0, space4);

cbuffer MotionConstants : register(b2, space4)
{
    float4 g_CurrentToPrevious[4]; // rows: current clip (ndc, w = 1) -> previous clip
    float4 g_Viewport;             // current x scale, x offset, y scale, y offset (pixels)
    float4 g_PreviousViewport;
    float2 g_DepthTransform;       // current z scale, z offset
    uint g_DepthIndex;
    uint g_SampleCount;
    uint g_SampleIndex;
    uint g_Valid;                  // 0: no previous frame, motion is zero
};

float ReadDepth(int2 position)
{
    if (g_SampleCount > 1)
        return g_DepthMS[g_DepthIndex].Load(position, g_SampleIndex);
    return g_Textures[g_DepthIndex].Load(int3(position, 0));
}

void vs_main(uint vid : SV_VertexID, out float4 pos : SV_Position)
{
    const float2 uv = float2((vid << 1) & 2, vid & 2);
    pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
}

struct Output
{
    float2 motion : SV_Target0;
    float depth : SV_Target1;
};

Output ps_main(float4 pos : SV_Position)
{
    Output o;
    const float depth = ReadDepth(int2(pos.xy));
    o.depth = depth;
    o.motion = 0;
    if (!g_Valid)
        return o;
    const float4 ndc = float4((pos.x - g_Viewport.y) / g_Viewport.x, (pos.y - g_Viewport.w) / g_Viewport.z,
        (depth - g_DepthTransform.y) / g_DepthTransform.x, 1.0);
    const float4 previous = float4(dot(g_CurrentToPrevious[0], ndc), dot(g_CurrentToPrevious[1], ndc),
        dot(g_CurrentToPrevious[2], ndc), dot(g_CurrentToPrevious[3], ndc));
    if (!(abs(previous.w) > 1e-6))
        return o;
    const float2 previousPixel = previous.xy / previous.w * float2(g_PreviousViewport.x, g_PreviousViewport.z) +
        float2(g_PreviousViewport.y, g_PreviousViewport.w);
    o.motion = previousPixel - pos.xy;
    return o;
}
