#include "geometry.h"
#include "replay_frame.h"
#include "renderer.h"
#include "agility_exports.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/xenia/third_party/stb/stb_image.h"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../third_party/xenia/third_party/stb/stb_image_write.h"

using namespace gpu::native;
using Bytes = std::vector<uint8_t>;
Bytes Read(const std::string& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Missing geometry fixture: " + path);
    const auto size = file.tellg(); file.seekg(0);
    Bytes data(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(data.data()), data.size())) throw std::runtime_error("Truncated fixture");
    return data;
}
struct Reader
{
    std::span<const uint8_t> bytes;
    std::span<const uint8_t> Take(size_t count)
    {
        if (count > bytes.size()) throw std::runtime_error("Truncated geometry fixture");
        const auto result = bytes.first(count); bytes = bytes.subspan(count); return result;
    }
    uint32_t Word() { auto value = Take(4); uint32_t result; memcpy(&result, value.data(), 4); return result; }
};

int main(int argc, char** argv)
{
    const bool pull=argc>1&&std::string(argv[1])=="--render-pulled";
    const bool render=pull||(argc>1&&std::string(argv[1])=="--render-bounded");
    const bool bounded=render||(argc>1&&std::string(argv[1])=="--bounded");
    const int first=bounded?2:1;
    if (argc < first+3 || (argc-first)%2 != 1) return 2;
    try
    {
        const auto catalog = ShaderCatalog::Load(argv[first]);
        Renderer renderer;
        if(render&&!renderer.LoadShaderCatalog(argv[first]))return 1;
        std::map<std::string, VertexProgram> programs;
        uint64_t passed = 0,beforeBytes=0,afterBytes=0;
        for (int path = first+1; path < argc; path += 2)
        {
            const std::string capture = argv[path], replay = argv[path + 1];
            auto frame = ReplayFrame::Load(replay);
            auto view = frame.View();
            StreamArena frameArena;
            std::vector<std::pair<uint32_t,Geometry>> converted;
            auto bufferBytes = Read(capture + "/buffers.bin");
            Reader buffer{bufferBytes};
            std::map<uint32_t, std::span<const uint8_t>> buffers;
            while (!buffer.bytes.empty())
            {
                if (buffer.Word() != 0x46444246) throw std::runtime_error("Bad captured buffer magic");
                const auto address = buffer.Word(), size = buffer.Word();
                auto payload = buffer.Take(size);
                auto& previous = buffers[address];
                if (previous.size() <= payload.size() || memcmp(previous.data(), payload.data(), payload.size())) previous = payload;
            }
            const GeometryReader read = [&](uint32_t address, uint32_t) -> std::span<const uint8_t> {
                auto found = buffers.find(address);
                return found == buffers.end() ? std::span<const uint8_t>{} : found->second;
            };
            auto fixture = Read(replay + "/geometry-tests.bin");
            Reader cases{fixture};
            if (cases.Word() != 0x54474446 || cases.Word() != 2) throw std::runtime_error("Bad geometry case magic");
            for (uint32_t count = cases.Word(); count; --count)
            {
                const auto record = cases.Word(), primitive = cases.Word(), indices = cases.Word();
                const bool indexed = cases.Word(), index32 = cases.Word();
                const auto address = cases.Word(), size = cases.Word();
                const auto code = cases.Take(size);
                std::array<uint32_t,192> fetches;
                auto fetchBytes = cases.Take(sizeof(fetches)); memcpy(fetches.data(), fetchBytes.data(), fetchBytes.size());
                const auto& expected = view.draws[record];
                auto found = programs.find(expected.vs);
                if (found == programs.end()) found = programs.emplace(expected.vs,
                    VertexProgram::FromShader(*catalog.Find(expected.vs, ShaderStage::Vertex))).first;
                StreamArena isolatedArena;
                auto& arena=bounded?frameArena:isolatedArena;
                try
                {
                    const bool gpuPrimary=pull&&primitive==4;
                    const GeometryBounds bounds{int32_t(expected.regs[VGT_INDX_OFFSET]),pull};
                    auto actual = BuildGeometry(found->second, code,
                        fetches,
                        primitive, indices, indexed, index32, address, read, arena,bounded?&bounds:nullptr);
                    if (actual.primitive != expected.primitive || actual.indexCount != expected.indexCount ||
                        actual.index32 != expected.index32 || actual.stride != (gpuPrimary?0:expected.stride) ||
                        actual.semantics != expected.semantics || (bounded?actual.vertices.size()>expected.vertexBytes:actual.vertices.size()!=expected.vertexBytes) ||
                        (bounded?actual.indices.size()>expected.indexBytes:actual.indices.size()!=expected.indexBytes) ||
                        (!gpuPrimary&&memcmp(actual.vertices.data(), expected.vertices, actual.vertices.size())) ||
                        memcmp(actual.indices.data(), expected.indices, actual.indices.size()))
                    {
                        fprintf(stderr, "primitive %u/%u count %u/%u index32 %u/%u stride %u/%u vertices %zu/%u indices %zu/%u\n",
                            actual.primitive,expected.primitive,actual.indexCount,expected.indexCount,actual.index32,expected.index32,
                            actual.stride,expected.stride,actual.vertices.size(),expected.vertexBytes,actual.indices.size(),expected.indexBytes);
                        for (size_t b = 0; b < actual.vertices.size() && b < expected.vertexBytes; ++b)
                            if (actual.vertices[b] != expected.vertices[b]) { fprintf(stderr,"first vertex byte difference %zu: %u/%u\n",b,actual.vertices[b],expected.vertices[b]); break; }
                        throw std::runtime_error("Native geometry differs from validated replay");
                    }
                    for (size_t slot = 0; slot < 32; ++slot)
                    {
                        uint32_t wanted[4]; memcpy(wanted, expected.vertexFetch + slot * 16, 16);
                        const auto* actualFetch = actual.fetches.data() + slot * 4;
                        if(gpuPrimary&&!wanted[2])continue;
                        if (memcmp(wanted + 1, actualFetch + 1, 12) ||
                            (wanted[2] && memcmp(view.streams.data() + wanted[0], arena.bytes.data() + actualFetch[0], wanted[2])))
                            throw std::runtime_error("Native pulled stream differs from validated replay");
                    }
                    beforeBytes+=expected.vertexBytes;afterBytes+=actual.vertices.size();
                    if(render)converted.emplace_back(record,std::move(actual));
                }
                catch (const std::exception& error)
                {
                    fprintf(stderr, "%s record %u VS %s: %s\n", replay.c_str(), record, expected.vs.c_str(), error.what());
                    return 1;
                }
                ++passed;
            }
            if(render)
            {
                std::vector<Draw> draws(view.draws.begin(),view.draws.end());
                for(const auto& [record,geometry]:converted)geometry.Bind(draws.at(record));
                view.draws=draws;view.streams=frameArena.bytes;
                FrameOutput output;
                if(renderer.RenderFrame(view,{},&output))return 1;
                int width,height,channels;
                auto* png=stbi_load((replay+"/frame.png").c_str(),&width,&height,&channels,3);
                if(!png)throw std::runtime_error("Missing bounded geometry baseline image");
                bool equal=output.width==width&&output.height==height&&output.rgb.size()==size_t(width)*height*3;
                uint64_t changed=0;unsigned maximum=0;
                if(equal)for(size_t b=0;b<output.rgb.size();++b)
                {
                    const auto difference=unsigned(std::abs(int(output.rgb[b])-int(png[b])));
                    changed+=difference!=0;maximum=(std::max)(maximum,difference);
                }
                equal=equal&&(pull?(maximum<=1&&changed<=64):changed==0);stbi_image_free(png);
                if(changed)printf("%s GPU vertex fetch RGB: %llu changed channels, maximum error %u\n",replay.c_str(),changed,maximum);
                if(pull)stbi_write_png((replay+"/geometry-pulled.png").c_str(),output.width,output.height,3,output.rgb.data(),output.width*3);
                if(!equal)throw std::runtime_error("Bounded native geometry changed rendered RGB");
            }
        }
        printf("Native geometry: %llu draws exactly match replay vertices, indices and pulled streams\n", passed);
        if(bounded)printf("Bounded native geometry: %llu -> %llu converted vertex bytes, same referenced data\n",beforeBytes,afterBytes);
        if(render)printf("Bounded native geometry frames: %llu frames, %llu draws, %s baseline RGB\n",renderer.CompletedFrames(),renderer.CompletedDraws(),pull?"at most 64 unit channel differences per frame":"exact");
        return 0;
    }
    catch (const std::exception& error) { fprintf(stderr, "%s\n", error.what()); return 1; }
}
