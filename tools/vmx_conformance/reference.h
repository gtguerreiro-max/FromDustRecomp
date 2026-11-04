// Reference (big-endian element order) semantics for the VMX / VMX128
// instructions used by the recompiled Galileo modules. Written from the PPC
// AltiVec definitions and Xenia's ppc_emit_altivec.cc, independently of the
// lane-reversed SSE sequences emitted by XenonRecomp.
//
// Register storage in XenonRecomp is the whole 16-byte vector byte-reversed:
// big-endian byte i lives in u8[15 - i], word k in u32[3 - k], etc.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace ref
{
struct Vec { uint8_t b[16]; };  // big-endian byte order

inline Vec Get(const PPCVRegister& r) { Vec v; for (int i = 0; i < 16; ++i) v.b[i] = r.u8[15 - i]; return v; }
inline void Put(PPCVRegister& r, const Vec& v) { for (int i = 0; i < 16; ++i) r.u8[15 - i] = v.b[i]; }

inline uint32_t W(const Vec& v, int k) { return uint32_t(v.b[4*k]) << 24 | uint32_t(v.b[4*k+1]) << 16 | uint32_t(v.b[4*k+2]) << 8 | v.b[4*k+3]; }
inline void SW(Vec& v, int k, uint32_t x) { v.b[4*k] = x >> 24; v.b[4*k+1] = x >> 16; v.b[4*k+2] = x >> 8; v.b[4*k+3] = x; }
inline uint16_t H(const Vec& v, int k) { return uint16_t(v.b[2*k] << 8 | v.b[2*k+1]); }
inline void SH(Vec& v, int k, uint16_t x) { v.b[2*k] = x >> 8; v.b[2*k+1] = uint8_t(x); }
inline float F(const Vec& v, int k) { uint32_t x = W(v, k); float f; memcpy(&f, &x, 4); return f; }
inline void SF(Vec& v, int k, float f) { uint32_t x; memcpy(&x, &f, 4); SW(v, k, x); }

// VMX runs with NJ=1: denormal inputs and results are flushed to zero.
inline float Flush(float f) { return std::fpclassify(f) == FP_SUBNORMAL ? std::copysign(0.0f, f) : f; }

template <class Op> Vec MapF(const Vec& a, const Vec& b, Op op) { Vec d; for (int k = 0; k < 4; ++k) SF(d, k, Flush(op(Flush(F(a, k)), Flush(F(b, k))))); return d; }
template <class Op> Vec MapW(const Vec& a, const Vec& b, Op op) { Vec d; for (int k = 0; k < 4; ++k) SW(d, k, op(W(a, k), W(b, k))); return d; }
template <class Op> Vec MapH(const Vec& a, const Vec& b, Op op) { Vec d; for (int k = 0; k < 8; ++k) SH(d, k, op(H(a, k), H(b, k))); return d; }
template <class Op> Vec MapB(const Vec& a, const Vec& b, Op op) { Vec d; for (int k = 0; k < 16; ++k) d.b[k] = op(a.b[k], b.b[k]); return d; }

inline float MaxF(float a, float b) { if (a == 0 && b == 0) return std::signbit(a) ? b : a; return a > b ? a : b; }
inline float MinF(float a, float b) { if (a == 0 && b == 0) return std::signbit(a) ? a : b; return a < b ? a : b; }

inline uint32_t SatU32(double x) { if (!(x > 0)) return 0; if (x >= 4294967295.0) return 0xFFFFFFFFu; return uint32_t(x); }
inline uint32_t SatS32(double x) { if (std::isnan(x)) return 0; if (x >= 2147483647.0) return 0x7FFFFFFF; if (x <= -2147483648.0) return 0x80000000u; return uint32_t(int32_t(x)); }

inline Vec Concat(const Vec& a, const Vec& b, int start) { Vec d; for (int i = 0; i < 16; ++i) { int j = start + i; d.b[i] = j < 16 ? a.b[j] : b.b[j - 16]; } return d; }

inline Vec Shl128(const Vec& a, int bits)
{
    Vec d{}; for (int i = 0; i < 128; ++i) { int src = i + bits; if (src < 128 && (a.b[src >> 3] >> (7 - (src & 7)) & 1)) d.b[i >> 3] |= uint8_t(1 << (7 - (i & 7))); } return d;
}
inline Vec Shr128(const Vec& a, int bits)
{
    Vec d{}; for (int i = 0; i < 128; ++i) { int src = i - bits; if (src >= 0 && (a.b[src >> 3] >> (7 - (src & 7)) & 1)) d.b[i >> 3] |= uint8_t(1 << (7 - (i & 7))); } return d;
}
}
