#include "shader_catalog.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <tuple>
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
// The float constant registers a shader reads, [first, end) of the 256 of
// its stage, from its cbuffer declarations ("float4 NAME[n] :
// packoffset(cR);") and the indices it uses: NAME(k) with a literal k reads
// R + k; any other index can reach every register from R on (the accessor
// macros allow indices past the declared size). Anything unexpected keeps
// the full range. Only the reads matter: the capture and the upload then
// copy just this range of each draw's 4 KB bank.
std::pair<uint16_t,uint16_t> ConstantRange(const std::string& source,ShaderStage stage)
{
    const char* const block=stage==ShaderStage::Vertex?"cbuffer VertexShaderConstants":"cbuffer PixelShaderConstants";
    const size_t at=source.find(block);
    if(at==std::string::npos)return{0,0}; // no float constants at all
    const size_t open=source.find('{',at),close=open==std::string::npos?open:source.find("};",open);
    if(open==std::string::npos||close==std::string::npos)return{0,256};
    auto identifier=[](char c){return std::isalnum(static_cast<unsigned char>(c))||c=='_';};
    unsigned lo=256,hi=0;
    for(size_t line=open+1;line<close;)
    {
        size_t eol=source.find('\n',line);
        if(eol==std::string::npos||eol>close)eol=close;
        const std::string text=source.substr(line,eol-line);
        line=eol+1;
        const size_t pack=text.find("packoffset(c");
        if(pack==std::string::npos)continue;
        // "type name[count] : packoffset(cR...)"
        size_t p=text.find_first_not_of(" \t");
        const size_t typeEnd=text.find_first_of(" \t",p);
        if(p==std::string::npos||typeEnd==std::string::npos)return{0,256};
        if(text.compare(p,typeEnd-p,"float4"))return{0,256};
        p=text.find_first_not_of(" \t",typeEnd);
        size_t nameEnd=p;
        while(nameEnd<text.size()&&identifier(text[nameEnd]))++nameEnd;
        if(nameEnd==p)return{0,256};
        const std::string name=text.substr(p,nameEnd-p);
        unsigned count=0;
        if(nameEnd<text.size()&&text[nameEnd]=='[')count=unsigned(std::strtoul(text.c_str()+nameEnd+1,nullptr,10));
        const unsigned reg=unsigned(std::strtoul(text.c_str()+pack+12,nullptr,10));
        if(reg>=256)return{0,256};
        for(size_t use=source.find(name,close);use!=std::string::npos;use=source.find(name,use+name.size()))
        {
            if(use>0&&identifier(source[use-1]))continue;
            size_t after=use+name.size();
            if(after<source.size()&&identifier(source[after]))continue;
            while(after<source.size()&&(source[after]==' '||source[after]=='\t'))++after;
            if(after<source.size()&&source[after]=='(')
            {
                int depth=0;size_t end=after;
                for(;end<source.size();++end){if(source[end]=='(')++depth;else if(source[end]==')'&&--depth==0)break;}
                if(end>=source.size())return{0,256};
                std::string argument=source.substr(after+1,end-after-1);
                argument.erase(0,argument.find_first_not_of(" \t"));
                argument.erase(argument.find_last_not_of(" \t")+1);
                const bool literal=!argument.empty()&&argument.size()<4&&std::all_of(argument.begin(),argument.end(),[](char c){return c>='0'&&c<='9';});
                if(literal&&count)
                {
                    const unsigned index=reg+unsigned(std::stoul(argument));
                    if(index>=256)return{0,256};
                    lo=(std::min)(lo,index);hi=(std::max)(hi,index+1);
                }
                else{lo=(std::min)(lo,reg);hi=256;}
            }
            else{lo=(std::min)(lo,reg);hi=(std::max)(hi,(std::min)(256u,reg+(count?count:1)));}
        }
    }
    if(hi<=lo)return{0,0};
    return{uint16_t(lo),uint16_t(hi)};
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
        if(shader.stage==ShaderStage::Vertex||shader.stage==ShaderStage::Pixel)
            std::tie(shader.constantFirst,shader.constantEnd)=ConstantRange(shader.source,shader.stage);
        // FD_TRACE_CONSTANT_RANGES=1: each shader's constant range at load.
        static const bool traceRanges=std::getenv("FD_TRACE_CONSTANT_RANGES")!=nullptr;
        if(traceRanges)std::fprintf(stderr,"constant range %s %s: %u-%u\n",shader.name.c_str(),shader.stage==ShaderStage::Vertex?"vs":"ps",
            unsigned(shader.constantFirst),unsigned(shader.constantEnd));
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
