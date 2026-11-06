// HDR presentation: the game's final image (8-bit, gamma ~2.2, SDR) to scRGB
// (linear BT.709, 1.0 = 80 nits) in the presenter's FP16 guest output.
//
// Mid tones keep their look at the chosen paper white. Highlights of the 3D
// scene (sky, sun on water, lava, bright sand) are expanded towards the
// display's peak with a smooth curve that starts at `knee` (linear
// brightness). The expansion is measured on the scene before the HUD was drawn
// (`hudless`, copied right after the game's colour grading pass): where the
// final image differs from it, the HUD or a menu covers the scene and the
// pixel stays at paper white (subtitles and icons are not boosted).
Texture2D<float4> source : register(t0);
Texture2D<float4> hudless : register(t1);
RWTexture2D<float4> destination : register(u0);
cbuffer Params : register(b0)
{
    uint2 sourceSize;
    uint2 destinationSize;
    float paperWhite; // scRGB value of SDR white (nits / 80)
    float peakRatio;  // peak brightness / paper white
    float knee;       // linear brightness where the expansion starts
    uint flags;       // bit 0: hudless is this frame's scene; bit 1: show the expansion (diagnostic)
}

float3 ToLinear(float3 c) { return pow(max(c, 0.0), 2.2); }

// Triangular noise in [-1, 1) per pixel: half an 8-bit step of dither before
// the expansion hides the banding of the stretched top of the range.
float Noise(uint2 p)
{
    uint h = p.x * 0x8da6b343u ^ p.y * 0xd8163841u;
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    const float a = float(h & 0xFFFF) / 65536.0, b = float(h >> 16) / 65536.0;
    return a + b - 1.0;
}

[numthreads(8, 8, 1)]
void main(uint3 thread : SV_DispatchThreadID)
{
    if (any(thread.xy >= destinationSize)) return;
    const uint2 pixel = min(uint2((thread.xy + 0.5) * sourceSize / destinationSize), sourceSize - 1);
    const float3 c = source.Load(int3(pixel, 0)).rgb;
    float expansion = 0.0, scene = 0.0;
    float3 color = c;
    if (flags & 1)
    {
        const float3 h = hudless.Load(int3(pixel, 0)).rgb;
        const float3 delta = abs(c - h);
        scene = saturate(1.0 - max(delta.r, max(delta.g, delta.b)) * 24.0);
        if (scene > 0.0)
        {
            color = saturate(c + Noise(thread.xy) * (0.5 / 255.0));
            const float3 l = ToLinear(color);
            // Brightness: luminance, raised towards the brightest channel so
            // saturated highlights (lava, fire, sunsets) also expand.
            const float y = dot(l, float3(0.2126, 0.7152, 0.0722));
            const float b = lerp(y, max(l.r, max(l.g, l.b)), 0.5);
            const float t = saturate((b - knee) / (1.0 - knee));
            expansion = t * t * scene;
        }
    }
    float3 result = ToLinear(color) * (1.0 + (peakRatio - 1.0) * expansion);
    if (flags & 2)
        result = ToLinear(c) * 0.5 + float3(expansion, 0.0, (flags & 1) ? (1.0 - scene) * 0.6 : 0.0);
    destination[thread.xy] = float4(result * paperWhite, 1.0);
}
