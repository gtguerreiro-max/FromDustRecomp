// Diagnostic CLI for the same native core linked into the game runtime.
#include <charconv>
#include <cstdio>
#include <cstring>

#include "renderer.h"
#include "agility_exports.h"

int main(int argc, char** argv)
{
    unsigned repeat = 1;
    int first = 1;
    const char* catalog = nullptr;
    while (first < argc && argv[first][0] == '-')
    {
        if (first + 1 >= argc) return 2;
        if (std::strcmp(argv[first], "--repeat") == 0)
        {
            const auto* value = argv[first + 1];
            const auto end = value + std::strlen(value);
            const auto parsed = std::from_chars(value, end, repeat);
            if (parsed.ec != std::errc() || parsed.ptr != end || !repeat || repeat > 1000) return 2;
        }
        else if (std::strcmp(argv[first], "--shader-catalog") == 0) catalog = argv[first + 1];
        else return 2;
        first += 2;
    }
    if (argc <= first)
    {
        std::fprintf(stderr, "usage: fd_replay [--repeat N] [--shader-catalog path] <replay dir> [<replay dir> ...]\n");
        return 2;
    }
    gpu::native::Renderer renderer;
    if (catalog && !renderer.LoadShaderCatalog(catalog)) return 1;
    for (unsigned iteration = 0; iteration < repeat; ++iteration)
        for (int index = first; index < argc; ++index)
            if (const int result = renderer.RenderReplay(argv[index])) return result;
    std::printf("native renderer: %llu frames, %llu game draws, one device\n",
        static_cast<unsigned long long>(renderer.CompletedFrames()),
        static_cast<unsigned long long>(renderer.CompletedDraws()));
    return 0;
}
