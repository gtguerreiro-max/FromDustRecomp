#pragma once
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
#include <immintrin.h>

inline PPCVRegister& VR(PPCContext& c, int n) { return (&c.v0)[n]; }
static_assert(offsetof(PPCContext, v127) - offsetof(PPCContext, v0) == 127 * sizeof(PPCVRegister));

struct Test
{
    const char* name;
    void (*recompiled)(PPCContext& __restrict, uint8_t*);
    void (*reference)(PPCContext&, uint8_t*);
    char kind;      // 'i' integer bits, 'f' float lanes, 'm' memory
    int tolerance;  // allowed float ULP difference
    int rc;         // compares CR6
};
extern const Test kTests[];
extern const size_t kTestCount;

namespace harness
{
constexpr size_t kMemory = 16384;

inline float RandomFloat(std::mt19937& rng)
{
    switch (rng() % 16)
    {
    case 0: return 0.0f;
    case 1: return -0.0f;
    case 2: return 1.0f;
    case 3: return -1.0f;
    case 4: return float(int(rng() % 2001) - 1000);
    case 5: return std::ldexp(float(rng() % 1000) / 1000.0f, int(rng() % 80) - 40);  // includes large / tiny
    default:
    {
        float f = std::ldexp(float(rng() & 0xFFFFFF) / float(1 << 24) + 0.5f, int(rng() % 24) - 12);
        return rng() & 1 ? -f : f;
    }
    }
}

inline uint32_t RandomBits(std::mt19937& rng)
{
    switch (rng() % 8)
    {
    case 0: return 0;
    case 1: return ~0u;
    case 2: return rng() % 64;
    case 3: return 0x7FFFFFFFu - rng() % 4;
    case 4: return 0x80000000u + rng() % 4;
    case 5: return (rng() % 0x20000) * 0x10001u;  // repeated halves
    default: return rng();
    }
}

inline void Fill(PPCContext& c, std::vector<uint8_t>& mem, char kind, std::mt19937& rng)
{
    for (int n = 0; n < 128; ++n)
        for (int k = 0; k < 4; ++k)
        {
            if (kind == 'f') { const float f = RandomFloat(rng); memcpy(&VR(c, n).u32[k], &f, 4); }
            else VR(c, n).u32[k] = RandomBits(rng);
        }
    PPCRegister* gpr = &c.r0;
    for (int n = 0; n < 32; ++n) gpr[n].u64 = rng() % (kMemory / 2);
    for (auto& b : mem) b = uint8_t(rng());
    c.cr6 = {};
    c.fpscr.csr = 0x1F80;  // all exceptions masked, as the runtime's guest threads
    _mm_setcsr(0x1F80);
}

inline uint32_t Ulp(uint32_t a, uint32_t b)
{
    auto key = [](uint32_t x) { return int64_t(x & 0x80000000u ? -int64_t(x & 0x7FFFFFFF) : int64_t(x)); };
    const int64_t d = key(a) - key(b);
    return uint32_t(d < 0 ? -d : d);
}
}

int main()
{
    using namespace harness;
    std::mt19937 rng(12345);
    int failures = 0;
    std::vector<uint8_t> m1(kMemory + 64), m2(kMemory + 64);
    static PPCContext c1, c2;
    for (size_t t = 0; t < kTestCount; ++t)
    {
        const Test& test = kTests[t];
        int bad = 0;
        for (int iter = 0; iter < 400 && bad < 1; ++iter)
        {
            Fill(c1, m1, test.kind == 'm' ? 'i' : test.kind, rng);
            c2 = c1; m2 = m1;
            test.recompiled(c1, m1.data());
            test.reference(c2, m2.data());
            for (int n = 0; n < 128 && !bad; ++n)
                for (int k = 0; k < 4; ++k)
                {
                    const uint32_t a = VR(c1, n).u32[k], b = VR(c2, n).u32[k];
                    if (a == b) continue;
                    if (test.tolerance && Ulp(a, b) <= uint32_t(test.tolerance)) continue;
                    float fa, fb; memcpy(&fa, &a, 4); memcpy(&fb, &b, 4);
                    printf("FAIL %-40s v%d word(be)%d: recompiled %08X (%g) reference %08X (%g)\n", test.name, n, 3 - k, a, fa, b, fb);
                    bad = 1;
                    break;
                }
            if (!bad && memcmp(m1.data(), m2.data(), m1.size()))
            {
                size_t i = 0; while (m1[i] == m2[i]) ++i;
                printf("FAIL %-40s memory +%zu: recompiled %02X reference %02X\n", test.name, i, m1[i], m2[i]);
                bad = 1;
            }
            if (!bad && test.rc && (c1.cr6.lt != c2.cr6.lt || c1.cr6.eq != c2.cr6.eq))
            {
                printf("FAIL %-40s cr6 lt/eq recompiled %d/%d reference %d/%d\n", test.name, c1.cr6.lt, c1.cr6.eq, c2.cr6.lt, c2.cr6.eq);
                bad = 1;
            }
        }
        failures += bad;
    }
    printf("%zu instruction forms tested, %d failing\n", kTestCount, failures);
    return failures ? 1 : 0;
}
