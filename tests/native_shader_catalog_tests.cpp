#include "shader_catalog.h"

#include <cstdio>
#include <stdexcept>

using namespace gpu::native;
using Bytes = std::vector<uint8_t>;

void Check(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}
void Word(Bytes& bytes, uint32_t value)
{
    for (int i = 0; i < 4; ++i) bytes.push_back(uint8_t(value >> (8 * i)));
}
void BigWord(Bytes& bytes, size_t offset, uint32_t value)
{
    for (int i = 0; i < 4; ++i) bytes[offset + i] = uint8_t(value >> (24 - 8 * i));
}
NativeShader Shader(const char* name, ShaderStage stage, uint8_t metadata, Bytes physical)
{
    NativeShader shader;
    shader.name = name;
    shader.stage = stage;
    shader.virtualData.resize(40);
    BigWord(shader.virtualData, 0, stage == ShaderStage::Vertex ? 0x102A1101 : 0x102A1100);
    BigWord(shader.virtualData, 4, 40);
    BigWord(shader.virtualData, 8, uint32_t(physical.size()));
    shader.virtualData.back() = metadata;
    shader.physicalData = std::move(physical);
    shader.source = "compiled test source";
    shader.dxil = {'D', 'X', 'B', 'C'};
    return shader;
}
Bytes Catalog(std::initializer_list<NativeShader> shaders)
{
    Bytes result;
    Word(result, 0x43534446); Word(result, 1); Word(result, uint32_t(shaders.size()));
    for (const auto& shader : shaders)
    {
        for (auto size : {shader.name.size(), size_t(shader.stage), shader.virtualData.size(),
            shader.physicalData.size(), shader.source.size(), shader.dxil.size()}) Word(result, uint32_t(size));
        result.insert(result.end(), shader.name.begin(), shader.name.end());
        result.insert(result.end(), shader.virtualData.begin(), shader.virtualData.end());
        result.insert(result.end(), shader.physicalData.begin(), shader.physicalData.end());
        result.insert(result.end(), shader.source.begin(), shader.source.end());
        result.insert(result.end(), shader.dxil.begin(), shader.dxil.end());
    }
    return result;
}
void Reject(const Bytes& bytes)
{
    try { ShaderCatalog::Parse(bytes); }
    catch (const std::runtime_error&) { return; }
    throw std::runtime_error("Invalid catalog accepted");
}

int main(int argc, char** argv)
{
    try
    {
        auto first = Shader("first", ShaderStage::Vertex, 1, Bytes(1600, 2));
        auto metadata = first; metadata.name = "metadata"; metadata.virtualData.back() = 2;
        auto tail = first; tail.name = "tail"; tail.physicalData.back() = 3;
        auto pixel = Shader("pixel", ShaderStage::Pixel, 1, Bytes(1600, 2));
        auto bytes = Catalog({first, metadata, tail, pixel});
        auto catalog = ShaderCatalog::Parse(bytes);
        for (const auto& expected : {first, metadata, tail, pixel})
        {
            auto* found = catalog.Find(expected.stage, expected.virtualData, expected.physicalData);
            Check(found && found->name == expected.name, "Full shader identity failed");
        }
        Check(!catalog.Find(ShaderStage::Vertex, first.virtualData, std::span(first.physicalData).first(1024)),
            "Truncated physical identity accepted");
        Check(!catalog.Find(ShaderStage::Pixel, first.virtualData, first.physicalData), "Wrong stage accepted");
        ShaderObjects objects;
        Check(objects.Register(123, first.stage, first.virtualData, first.physicalData, catalog), "Initial object not registered");
        Check(objects.Find(123, first.stage), "Registered object missing");
        Check(!objects.Register(123, first.stage, {}, {}, catalog), "Unknown reinitialization accepted");
        Check(!objects.Find(123, first.stage), "Object reuse retained stale shader");
        Check(objects.Register(123, pixel.stage, pixel.virtualData, pixel.physicalData, catalog), "Stage reuse not registered");
        Check(!objects.Find(123, first.stage), "Stage reuse retained stale vertex binding");
        Check(!objects.Register(0, pixel.stage, pixel.virtualData, pixel.physicalData, catalog), "Null shader object accepted");
        Reject({});
        auto truncated = bytes; truncated.pop_back(); Reject(truncated);
        auto trailing = bytes; trailing.push_back(0); Reject(trailing);
        Reject(Catalog({first, first}));
        auto alias = first; alias.name = "alias"; Reject(Catalog({first, alias}));
        auto bad = first; bad.virtualData[0] = 0; Reject(Catalog({bad}));
        bad = first; bad.dxil[0] = 0; Reject(Catalog({bad}));
        bad = first; bad.name = "../shader"; Reject(Catalog({bad}));
        bad = first; bad.stage = ShaderStage::Pixel; Reject(Catalog({bad}));
        auto bindings=first;
        bindings.source="#define used_Texture2DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 76)\n"
            "#define unused_Texture2DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 0)\n"
            "void main() { tfetchCube(used_TextureCubeDescriptorIndex); }";
        Check(ShaderCatalog::Parse(Catalog({bindings})).Shaders()[0].textureMask==(1u<<19),"Shader texture binding mask includes unused declarations or wrong stage slots");
        puts("Native shader identity, stage, reuse and malformed-input checks passed");
        if (argc == 2)
        {
            auto assets = ShaderCatalog::Load(argv[1]);
            for (const auto& shader : assets.Shaders())
            {
                Check(assets.Find(shader.stage, shader.virtualData, shader.physicalData) == &shader,
                    "Compiled catalog identity does not round trip");
                Check(assets.Find(shader.name, shader.stage) == &shader, "Named catalog lookup failed");
                Check(!assets.Find(shader.name, shader.stage == ShaderStage::Vertex ? ShaderStage::Pixel : ShaderStage::Vertex),
                    "Named lookup accepted wrong stage");
            }
            printf("Compiled native catalog: %zu shaders verified\n", assets.Shaders().size());
        }
        return 0;
    }
    catch (const std::exception& error)
    {
        fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
