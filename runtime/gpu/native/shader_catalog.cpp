#include "shader_catalog.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <set>
#include <regex>
#include <stdexcept>

namespace gpu::native
{
namespace
{
uint32_t BigWord(std::span<const uint8_t> bytes, size_t offset)
{
    return uint32_t(bytes[offset]) << 24 | uint32_t(bytes[offset + 1]) << 16 |
        uint32_t(bytes[offset + 2]) << 8 | bytes[offset + 3];
}
bool Equal(std::span<const uint8_t> a, std::span<const uint8_t> b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}
uint32_t TextureMask(const std::string& source)
{
    const auto main=source.find("void main(");
    if(main==std::string::npos)return 0;
    static const std::regex declaration(R"(#define\s+(\w+_Texture2DDescriptorIndex)\s+vk::RawBufferLoad<uint>\(g_PushConstants.SharedConstants\s*\+\s*(\d+)\))");
    uint32_t mask=0;
    for(std::sregex_iterator at(source.begin(),source.end(),declaration),end;at!=end;++at)
    {
        const auto slot=std::stoul((*at)[2].str());
        if(slot%4||slot>=128)throw std::runtime_error("Invalid native shader texture binding metadata");
        const auto name=(*at)[1].str();
        const auto prefix=name.substr(0,name.size()-std::string("2DDescriptorIndex").size());
        if(source.find(prefix,main)!=std::string::npos)mask|=1u<<(slot/4);
    }
    return mask;
}
}

ShaderCatalog ShaderCatalog::Load(const std::string& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Cannot open native shader catalog: " + path);
    const auto size = file.tellg();
    if (size < 0 || size > 256 * 1024 * 1024) throw std::runtime_error("Invalid native shader catalog size");
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
        throw std::runtime_error("Cannot read native shader catalog");
    return Parse(bytes);
}

ShaderCatalog ShaderCatalog::Parse(std::span<const uint8_t> bytes)
{
    size_t offset = 0;
    auto take = [&](size_t count) {
        if (count > bytes.size() - offset) throw std::runtime_error("Truncated native shader catalog");
        auto result = bytes.subspan(offset, count);
        offset += count;
        return result;
    };
    auto word = [&] {
        auto value = take(4);
        return uint32_t(value[0]) | uint32_t(value[1]) << 8 | uint32_t(value[2]) << 16 | uint32_t(value[3]) << 24;
    };
    if (word() != 0x43534446 || word() != 1) throw std::runtime_error("Unsupported native shader catalog"); // FDSC
    const uint32_t count = word();
    if (!count || count > 10000) throw std::runtime_error("Invalid native shader count");
    ShaderCatalog result;
    result.shaders.reserve(count);
    std::set<std::string> names;
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t nameSize = word(), stage = word(), virtualSize = word(), physicalSize = word();
        const uint32_t sourceSize = word(), dxilSize = word();
        if (!nameSize || nameSize > 128 || stage > 1 || virtualSize < 36 || virtualSize > 1024 * 1024 ||
            !physicalSize || physicalSize > 1024 * 1024 || !sourceSize || sourceSize > 8 * 1024 * 1024 ||
            dxilSize < 4 || dxilSize > 8 * 1024 * 1024)
            throw std::runtime_error("Invalid native shader entry sizes");
        NativeShader shader;
        auto name = take(nameSize);
        shader.name.assign(reinterpret_cast<const char*>(name.data()), name.size());
        if (shader.name.find_first_not_of("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_-") != std::string::npos ||
            !names.insert(shader.name).second) throw std::runtime_error("Invalid or duplicate native shader name");
        shader.stage = ShaderStage(stage);
        auto virtualData = take(virtualSize), physicalData = take(physicalSize);
        if ((BigWord(virtualData, 0) & 0xFFFFFFFE) != 0x102A1100 ||
            ((BigWord(virtualData, 0) & 1) ? 0u : 1u) != stage ||
            BigWord(virtualData, 4) != virtualSize || BigWord(virtualData, 8) != physicalSize)
            throw std::runtime_error("Native shader container metadata mismatch");
        if (result.Find(shader.stage, virtualData, physicalData))
            throw std::runtime_error("Ambiguous native shader identity");
        shader.virtualData.assign(virtualData.begin(), virtualData.end());
        shader.physicalData.assign(physicalData.begin(), physicalData.end());
        auto source = take(sourceSize), dxil = take(dxilSize);
        if (std::memcmp(dxil.data(), "DXBC", 4)) throw std::runtime_error("Invalid native shader DXIL container");
        shader.source.assign(reinterpret_cast<const char*>(source.data()), source.size());
        shader.textureMask=TextureMask(shader.source);
        shader.dxil.assign(dxil.begin(), dxil.end());
        result.namedShaders.emplace(shader.name,result.shaders.size());
        result.shaders.push_back(std::move(shader));
    }
    if (offset != bytes.size()) throw std::runtime_error("Unexpected trailing shader catalog data");
    return result;
}

const NativeShader* ShaderCatalog::Find(ShaderStage stage, std::span<const uint8_t> virtualData,
    std::span<const uint8_t> physicalData) const
{
    for (const auto& shader : shaders)
        if (shader.stage == stage && Equal(shader.virtualData, virtualData) && Equal(shader.physicalData, physicalData))
            return &shader;
    return nullptr;
}

const NativeShader* ShaderObjects::Register(uint32_t object, ShaderStage stage,
    std::span<const uint8_t> virtualData, std::span<const uint8_t> physicalData, const ShaderCatalog& catalog)
{
    objects.erase(object);
    const auto* shader = object ? catalog.Find(stage, virtualData, physicalData) : nullptr;
    if (shader) objects[object] = shader;
    return shader;
}

const NativeShader* ShaderCatalog::Find(const std::string& name, ShaderStage stage) const
{
    const auto found=namedShaders.find(name);
    if(found==namedShaders.end())return nullptr;
    const auto& shader=shaders[found->second];
    return shader.stage==stage?&shader:nullptr;
}

const NativeShader* ShaderObjects::Find(uint32_t object, ShaderStage stage) const
{
    const auto found = objects.find(object);
    return found != objects.end() && found->second->stage == stage ? found->second : nullptr;
}
}
