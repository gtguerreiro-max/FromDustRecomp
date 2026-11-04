#include "frame.h"
#include "renderer.h"
#include "replay_frame.h"
#include "agility_exports.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/xenia/third_party/stb/stb_image.h"

using namespace gpu::native;

int main(int argc, char** argv)
{
    if (argc < 3) return 2;
    try
    {
        // Moving these adapters during vector growth must preserve draw storage.
        std::vector<ReplayFrame> frames;
        std::vector<std::vector<uint8_t>> expected;
        for (int i = 2; i < argc; ++i)
        {
            frames.push_back(ReplayFrame::Load(argv[i]));
            int width, height, channels;
            auto* image = stbi_load((std::string(argv[i]) + "/frame.png").c_str(), &width, &height, &channels, 3);
            if (!image) throw std::runtime_error("Missing baseline native PNG");
            expected.emplace_back(image, image + size_t(width) * height * 3);
            stbi_image_free(image);
        }
        Renderer renderer;
        if (!renderer.LoadShaderCatalog(argv[1])) return 1;
        for (size_t i = 0; i < frames.size(); ++i)
        {
            FrameOutput output;
            // No directory: this entry point may not read or write frame files.
            if (renderer.RenderFrame(frames[i].View(), {}, &output)) return 1;
            if (!output.width || !output.height || output.debugErrors || output.rgb != expected[i])
                throw std::runtime_error("In-memory output differs from the validated native image");
            auto invalid = frames[i].View();
            std::vector<Draw> draws(invalid.draws.begin(), invalid.draws.end());
            for (auto& draw : draws)
                if (!draw.clear && !draw.resolve) { draw.constants = nullptr; break; }
            invalid.draws = draws;
            const auto completed = renderer.CompletedFrames();
            if (!renderer.RenderFrame(invalid, {}, &output) || !output.rgb.empty() || output.width ||
                renderer.CompletedFrames() != completed)
                throw std::runtime_error("Invalid memory frame retained stale output or completed");
        }
        printf("Memory frames: %llu frames, %llu draws, exact baseline RGB, no frame files\n",
            renderer.CompletedFrames(), renderer.CompletedDraws());
        return 0;
    }
    catch (const std::exception& error)
    {
        fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
