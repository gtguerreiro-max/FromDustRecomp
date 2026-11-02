float fd7e3ToFloat(uint v)
{
    v &= 0x3FFu;
    if (v == 0u)
    {
        return 0.0f;
    }

    uint exponent = v >> 7;
    uint mantissa = v & 0x7Fu;
    uint ieeeExp;

    if (exponent == 0u)
    {
        uint m = mantissa;
        uint lzcnt = 0u;

        while ((m & 0x80u) == 0u)
        {
            m <<= 1u;
            lzcnt += 1u;
        }

        ieeeExp = 125u - lzcnt;
        mantissa = (mantissa << lzcnt) & 0x7Fu;
    }
    else
    {
        ieeeExp = exponent + 124u;
    }

    return asfloat((ieeeExp << 23) | (mantissa << 16));
}

uint fdFloatTo7e3(float x)
{
    if (!(x > 0.0f))
    {
        return 0u;
    }

    uint u = asuint(x);

    if (u >= 0x41FF0000u)
    {
        return 0x3FFu;
    }

    if (u == 0u)
    {
        return 0u;
    }

    uint e32 = u >> 23;

    if (e32 == 0u)
    {
        return 0u;
    }

    if (e32 < 125u)
    {
        float y = x * 512.0f;
        float fl = floor(y);
        uint i = uint(fl);
        float diff = y - fl;
        uint roundUp;

        if (diff > 0.5f)
        {
            roundUp = 1u;
        }
        else if (diff < 0.5f)
        {
            roundUp = 0u;
        }
        else
        {
            roundUp = (i & 1u) ? 1u : 0u;
        }

        uint n = i + roundUp;

        if (n >= 128u)
        {
            return 0x80u;
        }

        return n;
    }

    uint e = e32 - 124u;
    uint frac = u & 0x7FFFFFu;
    uint m = frac >> 16;
    uint drop = frac & 0xFFFFu;
    uint sticky = drop & 0x7FFFu;

    if (drop > 0x8000u || (drop == 0x8000u && (sticky != 0u || (m & 1u) != 0u)))
    {
        m += 1u;
    }

    if (m == 128u)
    {
        e += 1u;
        m = 0u;

        if (e > 7u)
        {
            return 0x3FFu;
        }
    }

    return (e << 7) | m;
}
