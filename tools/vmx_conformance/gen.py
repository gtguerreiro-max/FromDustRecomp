"""Generate a VMX conformance test from XenonRecomp output.

Every distinct vector instruction instance (mnemonic, immediates and register
aliasing pattern) in the given recompiled modules is copied verbatim into a
test function and compared, with random register/memory contents, against an
independent big-endian reference (reference.h).

Usage: python tools/vmx_conformance/gen.py <out.cpp> <ppc module dir> [...]
"""
import re, sys
from pathlib import Path

COMMENT = re.compile(r"^\t// ([a-z0-9.]+)\s*(.*)$")

def snippets(path):
    lines = path.read_text().splitlines()
    i = 0
    while i < len(lines):
        m = COMMENT.match(lines[i])
        if m and m.group(1).startswith(("v", "lvx", "stvx", "lvlx", "lvrx", "stvewx", "stvlx", "stvrx")):
            body = []
            j = i + 1
            while j < len(lines) and not lines[j].startswith("\t// ") and not lines[j].startswith("loc_") and lines[j] != "}":
                body.append(lines[j]); j += 1
            yield m.group(1), [o.strip() for o in m.group(2).split(",")] if m.group(2) else [], body
            i = j
        else:
            i += 1

def key(mn, ops):
    names, out = {}, []
    for o in ops:
        if re.fullmatch(r"[vr]\d+", o):
            names.setdefault(o, f"{o[0]}{len(names)}"); out.append(names[o])
        else:
            out.append(o)
    return mn + " " + ",".join(out)

def reg(o): return int(o[1:])

def imm(o): return int(o, 0)

FLOAT = {"vaddfp", "vsubfp", "vmulfp", "vmaxfp", "vminfp", "vmaddfp", "vmaddcfp", "vnmsubfp", "vcmpgtfp", "vcmpeqfp", "vcmpgefp",
         "vrefp", "vrsqrtefp", "vctsxs", "vctuxs", "vcfpsxws", "vcfpuxws", "vrfin", "vrfiz", "vrfim", "vrfip"}

def reference(mn, ops):
    """C++ body computing the reference result into ctx (named c)."""
    rc = mn.endswith(".")
    core = mn.rstrip(".")
    if core.endswith("128"): core = core[:-3]
    V = lambda o: f"ref::Get(VR(c,{reg(o)}))"
    put = lambda o, e: f"ref::Put(VR(c,{reg(o)}), {e});"
    d = ops[0] if ops else None
    tol, kind = 0, "i"
    binf = {"vaddfp": "a+b", "vsubfp": "a-b", "vmulfp": "a*b", "vmaxfp": "ref::MaxF(a,b)", "vminfp": "ref::MinF(a,b)"}
    binw = {"vadduwm": "a+b", "vsubuwm": "a-b", "vminuw": "a<b?a:b", "vmaxuw": "a>b?a:b",
            "vminsw": "int32_t(a)<int32_t(b)?a:b", "vmaxsw": "int32_t(a)>int32_t(b)?a:b",
            "vslw": "a<<(b&31)", "vsrw": "a>>(b&31)", "vsraw": "uint32_t(int32_t(a)>>(b&31))",
            "vrlw": "(a<<(b&31))|(a>>((32-(b&31))&31))",
            "vcmpequw": "a==b?~0u:0u", "vcmpgtuw": "a>b?~0u:0u", "vcmpgtsw": "int32_t(a)>int32_t(b)?~0u:0u",
            "vsubsws": "ref::SatS32(double(int32_t(a))-double(int32_t(b)))", "vaddsws": "ref::SatS32(double(int32_t(a))+double(int32_t(b)))",
            "vand": "a&b", "vandc": "a&~b", "vor": "a|b", "vxor": "a^b", "vnor": "~(a|b)"}
    binh = {"vadduhm": "uint16_t(a+b)", "vsubuhm": "uint16_t(a-b)", "vminuh": "a<b?a:b", "vmaxuh": "a>b?a:b",
            "vminsh": "int16_t(a)<int16_t(b)?a:b", "vmaxsh": "int16_t(a)>int16_t(b)?a:b",
            "vslh": "uint16_t(a<<(b&15))", "vsrh": "uint16_t(a>>(b&15))", "vsrah": "uint16_t(int16_t(a)>>(b&15))",
            "vcmpequh": "a==b?0xFFFF:0", "vcmpgtuh": "a>b?0xFFFF:0", "vcmpgtsh": "int16_t(a)>int16_t(b)?0xFFFF:0",
            "vavguh": "uint16_t((uint32_t(a)+b+1)>>1)"}
    binb = {"vaddubm": "uint8_t(a+b)", "vsububm": "uint8_t(a-b)", "vminub": "a<b?a:b", "vmaxub": "a>b?a:b",
            "vslb": "uint8_t(a<<(b&7))", "vsrb": "uint8_t(a>>(b&7))", "vsrab": "uint8_t(int8_t(a)>>(b&7))",
            "vcmpequb": "a==b?0xFF:0", "vcmpgtub": "a>b?0xFF:0", "vcmpgtsb": "int8_t(a)>int8_t(b)?0xFF:0",
            "vavgub": "uint8_t((uint32_t(a)+b+1)>>1)"}
    cmpf = {"vcmpgtfp": "a>b", "vcmpeqfp": "a==b", "vcmpgefp": "a>=b"}
    code = []
    if core in binf:
        kind = "f"; code.append(put(d, f"ref::MapF({V(ops[1])},{V(ops[2])},[](float a,float b){{return {binf[core]};}})"))
    elif core in cmpf:
        kind = "f"; code.append(f"ref::Vec r; auto A={V(ops[1])}, B={V(ops[2])}; for(int k=0;k<4;++k){{float a=ref::Flush(ref::F(A,k)),b=ref::Flush(ref::F(B,k)); ref::SW(r,k,({cmpf[core]})?~0u:0u);}}")
        code.append(put(d, "r"))
    elif core in binw:
        code.append(put(d, f"ref::MapW({V(ops[1])},{V(ops[2])},[](uint32_t a,uint32_t b)->uint32_t{{return {binw[core]};}})"))
    elif core in binh:
        code.append(put(d, f"ref::MapH({V(ops[1])},{V(ops[2])},[](uint16_t a,uint16_t b)->uint16_t{{return {binh[core]};}})"))
    elif core in binb:
        code.append(put(d, f"ref::MapB({V(ops[1])},{V(ops[2])},[](uint8_t a,uint8_t b)->uint8_t{{return {binb[core]};}})"))
    elif core in ("vmaddfp", "vmaddcfp", "vnmsubfp"):
        kind, tol = "f", 1
        neg = core == "vnmsubfp"
        code.append(f"ref::Vec r; auto A={V(ops[1])}, B={V(ops[2])}, C={V(ops[3])}; for(int k=0;k<4;++k){{float x=ref::Flush(ref::F(A,k))*ref::Flush(ref::F(B,k)); x={'-(x-ref::Flush(ref::F(C,k)))' if neg else 'x+ref::Flush(ref::F(C,k))'}; ref::SF(r,k,ref::Flush(x));}}")
        code.append(put(d, "r"))
    elif core == "vsel":
        code.append(f"ref::Vec r; auto A={V(ops[1])}, B={V(ops[2])}, M={V(ops[3])}; for(int i=0;i<16;++i) r.b[i]=uint8_t((A.b[i]&~M.b[i])|(B.b[i]&M.b[i]));")
        code.append(put(d, "r"))
    elif core == "vperm":
        code.append(f"ref::Vec r; auto A={V(ops[1])}, B={V(ops[2])}, M={V(ops[3])}; for(int i=0;i<16;++i){{int j=M.b[i]&31; r.b[i]=j<16?A.b[j]:B.b[j-16];}}")
        code.append(put(d, "r"))
    elif core == "vsldoi":
        code.append(put(d, f"ref::Concat({V(ops[1])},{V(ops[2])},{imm(ops[3])})"))
    elif core in ("vmrghw", "vmrglw", "vmrghh", "vmrglh", "vmrghb", "vmrglb"):
        size = {"w": 4, "h": 2, "b": 1}[core[-1]]; n = 16 // size // 2; lo = core[4] == "l"
        code.append(f"ref::Vec r; auto A={V(ops[1])}, B={V(ops[2])}; for(int k=0;k<{n};++k) for(int s=0;s<{size};++s){{ r.b[(2*k)*{size}+s]=A.b[({n if lo else 0}+k)*{size}+s]; r.b[(2*k+1)*{size}+s]=B.b[({n if lo else 0}+k)*{size}+s]; }}")
        code.append(put(d, "r"))
    elif core in ("vpkuwum", "vpkuwus", "vpkuhum", "vpkuhus", "vpkswss", "vpkshss", "vpkswus", "vpkshus"):
        wide = core[4] == "w"
        cnt = 4 if wide else 8
        if core.endswith("um"): sat = "x & " + ("0xFFFF" if wide else "0xFF")
        elif core in ("vpkuwus", "vpkuhus"): sat = f"(x > {'0xFFFFu' if wide else '0xFFu'} ? {'0xFFFFu' if wide else '0xFFu'} : x)"
        else: sat = None
        get = "ref::W" if wide else "ref::H"; setter = "ref::SH" if wide else None
        body = f"uint32_t x={get}(S,k); uint32_t y={sat};"
        store = "ref::SH(r,o+k,uint16_t(y));" if wide else "r.b[o+k]=uint8_t(y);"
        code.append(f"ref::Vec r; auto A={V(ops[1])}, B={V(ops[2])}; for(int h=0;h<2;++h){{ const ref::Vec& S=h?B:A; int o=h*{cnt}; for(int k=0;k<{cnt};++k){{ {body} {store} }} }}")
        code.append(put(d, "r"))
    elif core in ("vspltw", "vsplth", "vspltb"):
        size = {"w": 4, "h": 2, "b": 1}[core[-1]]; u = imm(ops[2])
        code.append(f"ref::Vec r; auto B={V(ops[1])}; for(int k=0;k<{16//size};++k) for(int s=0;s<{size};++s) r.b[k*{size}+s]=B.b[{u % (16//size)}*{size}+s];")
        code.append(put(d, "r"))
    elif core in ("vspltisw", "vspltish", "vspltisb"):
        size = {"w": 4, "h": 2, "b": 1}[core[-1]]; s = imm(ops[1])
        code.append(f"ref::Vec r; int64_t v={s}; for(int k=0;k<{16//size};++k) for(int s=0;s<{size};++s) r.b[k*{size}+s]=uint8_t(uint64_t(v)>>(8*({size}-1-s)));")
        code.append(put(d, "r"))
    elif core in ("vcfsx", "vcsxwfp", "vcfux", "vcuxwfp"):
        signed = core in ("vcfsx", "vcsxwfp"); u = imm(ops[2])
        code.append(f"ref::Vec r; auto B={V(ops[1])}; for(int k=0;k<4;++k){{ double x={'double(int32_t(ref::W(B,k)))' if signed else 'double(ref::W(B,k))'}; ref::SF(r,k,ref::Flush(float(x)*std::ldexp(1.0f,-{u}))); }}")
        code.append(put(d, "r")); kind = "i"
    elif core in ("vctsxs", "vcfpsxws", "vctuxs", "vcfpuxws"):
        signed = core in ("vctsxs", "vcfpsxws"); u = imm(ops[2]); kind = "f"
        code.append(f"ref::Vec r; auto B={V(ops[1])}; for(int k=0;k<4;++k){{ double x=double(ref::Flush(ref::F(B,k)))*double(1u<<{u}); ref::SW(r,k,{'ref::SatS32(x)' if signed else 'ref::SatU32(x)'}); }}")
        code.append(put(d, "r"))
    elif core == "vrlimi":
        mask, rot = imm(ops[2]), imm(ops[3])
        code.append(f"ref::Vec r={V(d)}; auto B={V(ops[1])}; for(int k=0;k<4;++k) if({mask}&(8>>k)) ref::SW(r,k,ref::W(B,(k+{rot})&3));")
        code.append(put(d, "r"))
    elif core == "vpermwi":
        u = imm(ops[2])
        code.append(f"ref::Vec r; auto B={V(ops[1])}; int sel[4]={{({u}>>6)&3,({u}>>4)&3,({u}>>2)&3,{u}&3}}; for(int k=0;k<4;++k) ref::SW(r,k,ref::W(B,sel[k]));")
        code.append(put(d, "r"))
    elif core in ("vrefp", "vrsqrtefp"):
        kind, tol = "f", 4096
        e = "1.0f/x" if core == "vrefp" else "1.0f/std::sqrt(x)"
        code.append(f"ref::Vec r; auto B={V(ops[1])}; for(int k=0;k<4;++k){{ float x=ref::Flush(ref::F(B,k)); ref::SF(r,k,ref::Flush({e})); }}")
        code.append(put(d, "r"))
    elif core in ("vsl", "vslo", "vsro", "vsr"):
        amount = "(B.b[15]&7)" if core in ("vsl", "vsr") else "((B.b[15]>>3)&15)*8"
        fn = "Shl128" if core in ("vsl", "vslo") else "Shr128"
        code.append(f"auto A={V(ops[1])}, B={V(ops[2])}; ref::Vec r=ref::{fn}(A,{amount});")
        code.append(put(d, "r"))
    elif core in ("lvx", "lvlx", "lvrx", "stvx", "stvewx", "stvlx", "stvrx"):
        kind = "m"
        ea = (f"(c.r{reg(ops[1])}.u32+c.r{reg(ops[2])}.u32)" if ops[1] != "r0" else f"c.r{reg(ops[2])}.u32")
        if core == "lvx":
            code.append(f"uint32_t ea={ea}&~15u; ref::Vec r; for(int i=0;i<16;++i) r.b[i]=base[ea+i];"); code.append(put(d, "r"))
        elif core == "lvlx":
            code.append(f"uint32_t ea={ea}; ref::Vec r{{}}; for(int i=0;i<16-int(ea&15);++i) r.b[i]=base[ea+i];"); code.append(put(d, "r"))
        elif core == "lvrx":
            code.append(f"uint32_t ea={ea}; ref::Vec r{{}}; int s=ea&15; for(int i=0;i<s;++i) r.b[16-s+i]=base[(ea&~15u)+i];"); code.append(put(d, "r"))
        elif core == "stvx":
            code.append(f"uint32_t ea={ea}&~15u; auto S={V(d)}; for(int i=0;i<16;++i) base[ea+i]=S.b[i];")
        elif core == "stvewx":
            code.append(f"uint32_t ea={ea}&~3u; auto S={V(d)}; int k=(ea&15)>>2; for(int i=0;i<4;++i) base[ea+i]=S.b[4*k+i];")
        elif core == "stvlx":
            code.append(f"uint32_t ea={ea}; auto S={V(d)}; for(int i=0;i<16-int(ea&15);++i) base[ea+i]=S.b[i];")
        elif core == "stvrx":
            code.append(f"uint32_t ea={ea}; auto S={V(d)}; int s=ea&15; for(int i=0;i<s;++i) base[(ea&~15u)+i]=S.b[16-s+i];")
    else:
        return None
    if rc:
        code.append(f"{{ auto D=ref::Get(VR(c,{reg(d)})); bool all=true, none=true; for(int i=0;i<16;++i){{ if(D.b[i]!=0xFF) all=false; if(D.b[i]) none=false; }} c.cr6.lt=all; c.cr6.gt=0; c.cr6.eq=none; c.cr6.so=0; }}")
    return "\n\t".join(code), kind, tol

def main():
    out = Path(sys.argv[1])
    seen, tests, skipped = {}, [], {}
    for module in sys.argv[2:]:
        for path in sorted(Path(module).glob("ppc_recomp.*.cpp")):
            for mn, ops, body in snippets(path):
                k = key(mn, ops)
                if k in seen: continue
                seen[k] = True
                r = reference(mn, ops)
                if r is None:
                    skipped[mn] = skipped.get(mn, 0) + 1; continue
                tests.append((mn, ops, body, *r))
    src = ['#include "ppc_context.h"', '#include "reference.h"', '#include "harness.h"', ""]
    table = []
    for n, (mn, ops, body, refcode, kind, tol) in enumerate(tests):
        comment = f"{mn} {','.join(ops)}"
        src.append(f"static void T{n}(PPCContext& __restrict ctx, uint8_t* base) {{ PPCRegister temp{{}}; uint32_t ea{{}}; (void)temp; (void)ea;\n" + "\n".join(body) + "\n}")
        src.append(f"static void R{n}(PPCContext& c, uint8_t* base) {{ (void)base;\n\t{refcode}\n}}")
        table.append(f'\t{{"{comment}", T{n}, R{n}, \'{kind}\', {tol}, {1 if mn.endswith(".") else 0}}},')
    src.append("const Test kTests[] = {"); src += table; src.append("};")
    src.append("const size_t kTestCount = sizeof(kTests) / sizeof(kTests[0]);")
    out.write_text("\n".join(src) + "\n")
    print(f"{len(tests)} tests; skipped mnemonics: {skipped}")

main()
