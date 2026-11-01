"""Print the recompiled C++ of guest functions (for reading or for a helper
model). Long bodies are cut to --max-lines.

Usage: python tools/extract_funcs.py [--max-lines N] 820DAE88 820DB7D8 ...
"""
import glob, re, sys


def main():
    args = sys.argv[1:]
    limit = 400
    if args[:1] == ["--max-lines"]:
        limit, args = int(args[1]), args[2:]
    wanted = {a.upper() for a in args}
    bodies = {}
    for path in glob.glob("ppc/Default/ppc_recomp.*.cpp"):
        current, lines = None, []
        for line in open(path, encoding="utf-8", errors="ignore"):
            m = re.match(r"PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]{8})\)", line)
            if m:
                if current in wanted:
                    bodies[current] = lines
                current, lines = m.group(1), []
            if current in wanted:
                lines.append(line.rstrip("\n"))
        if current in wanted:
            bodies[current] = lines
    for address in args:
        lines = bodies.get(address.upper(), ["// not found"])
        print("// ---- sub_%s (%d lines)" % (address.upper(), len(lines)))
        print("\n".join(lines[:limit]))
        if len(lines) > limit:
            print("// ... %d more lines" % (len(lines) - limit))


if __name__ == "__main__":
    main()
