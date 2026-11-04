// Synthetic microcode only. Execute the actual generated predicate expression.
#ifdef GENERATE_PREDICATE_TEST
#include "pch.h"
#include "shader_recompiler.h"
#include <cstdio>

int main()
{
    AluInstruction instruction{};
    instruction.scalarOpcode = AluScalarOpcode::SetpInv;
    instruction.src3Select = 1;
    instruction.scalarWriteMask = 8;
    ShaderRecompiler compiler;
    compiler.recompile(instruction);
    std::puts("struct Result { float value; bool predicate; };\n"
              "inline Result Invert(float value) {\n"
              "struct { float x=0, y=0, z=0, w=0; } r0;\n"
              "r0.w = value; bool p0=false; float ps=0;\n");
    std::puts(compiler.out.c_str());
    std::puts("return {r0.w, p0}; }");
}
#else
#include <cstdio>
#include <initializer_list>
#include "predicate_generated.h"

int main()
{
    // Xenos: invert 0/1, retain other stack values, predicate iff input is 1.
    const float inputs[] = {0, 1, 2, 0.5f, -1};
    const float expected[] = {1, 0, 2, 0.5f, -1};
    for (unsigned i = 0; i < 5; ++i)
    {
        const Result result = Invert(inputs[i]);
        if (result.value != expected[i] || result.predicate != (inputs[i] == 1))
        {
            std::fprintf(stderr, "SETp_INV(%g): value %g, predicate %d\n", inputs[i], result.value, result.predicate);
            return 1;
        }
    }
    for (float input : {0.0f, 1.0f})
        if (Invert(Invert(input).value).value != input) return 1;
    std::puts("7 generated predicate cases passed");
}
#endif
