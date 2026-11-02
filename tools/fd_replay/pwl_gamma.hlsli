float3 fdLinearToPwl(float3 c)
{
    const float N = 1023.0;
    float3 x = clamp(c, float3(0.0, 0.0, 0.0), float3(1.0, 1.0, 1.0)) * N;

    float3 g0 = 4.0 * x;
    float3 g1 = 2.0 * x + 128.0;
    float3 g2 = x + 256.0;
    float3 g3 = 0.5 * x + 512.0;

    float3 g = g0;
    g = lerp(g, g1, step(float3(64.0, 64.0, 64.0), x));
    g = lerp(g, g2, step(float3(128.0, 128.0, 128.0), x));
    g = lerp(g, g3, step(float3(512.0, 512.0, 512.0), x));

    return clamp(g / N, float3(0.0, 0.0, 0.0), float3(1.0, 1.0, 1.0));
}

float3 fdPwlToLinear(float3 c)
{
    const float N = 1023.0;
    float3 y = clamp(c, float3(0.0, 0.0, 0.0), float3(1.0, 1.0, 1.0)) * N;

    float3 l0 = 0.25 * y;
    float3 l1 = 0.5 * y - 64.0;
    float3 l2 = y - 256.0;
    float3 l3 = 2.0 * y - 1024.0;

    float3 l = l0;
    l = lerp(l, l1, step(float3(256.0, 256.0, 256.0), y));
    l = lerp(l, l2, step(float3(384.0, 384.0, 384.0), y));
    l = lerp(l, l3, step(float3(768.0, 768.0, 768.0), y));

    return clamp(l / N, float3(0.0, 0.0, 0.0), float3(1.0, 1.0, 1.0));
}
