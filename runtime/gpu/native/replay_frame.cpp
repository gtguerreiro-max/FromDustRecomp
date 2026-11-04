#include "replay_frame.h"
#include "texture_tables.h"
#include <bit>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <stdexcept>

namespace gpu::native
{
namespace
{
float AsFloat(uint32_t bits) { return std::bit_cast<float>(bits); }
uint64_t Fingerprint(std::span<const uint8_t> bytes)
{
    uint64_t value=14695981039346656037ull;
    for(uint8_t byte:bytes){value^=byte;value*=1099511628211ull;}
    return value;
}
std::vector<uint8_t> Read(const std::string& path, bool required = false)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        if (required) throw std::runtime_error("Missing replay asset: " + path);
        return {};
    }
    const auto length = file.tellg();
    if (length < 0 || length > 4ll * 1024 * 1024 * 1024) throw std::runtime_error("Invalid replay asset size: " + path);
    std::vector<uint8_t> bytes(static_cast<size_t>(length));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) throw std::runtime_error("Truncated replay asset");
    return bytes;
}
std::vector<Draw> ParseReplay(const std::vector<uint8_t>& file)
{
    std::vector<Draw> draws;
    size_t at = 0;
    auto u32 = [&] {
        if (at > file.size() || file.size() - at < 4) throw std::runtime_error("Truncated replay word");
        uint32_t v; memcpy(&v, file.data() + at, 4); at += 4; return v;
    };
    auto bytes = [&](uint32_t n) {
        if (at > file.size() || n > file.size() - at) throw std::runtime_error("Truncated replay payload");
        const uint8_t* p = file.data() + at; at += n; return p;
    };
    if (file.size() < 8) return draws;
    const uint32_t magic = u32();
    if (magic != 0x50524446 && magic != 0x32524446 && magic != 0x33524446 && magic != 0x34524446 && magic != 0x35524446 && magic != 0x36524446 && magic != 0x37524446 && magic != 0x38524446 && magic != 0x39524446) return draws;
    const bool fullContext = magic == 0x39524446;
    const bool fetchMetadata = magic == 0x38524446 || fullContext;
    const bool blendConstants = magic == 0x37524446 || fetchMetadata;
    const bool stencilState = magic == 0x36524446 || blendConstants;
    const bool pixelCenter = magic == 0x34524446 || magic == 0x35524446 || stencilState;
    const uint32_t regCount = blendConstants ? kRegCount : stencilState ? 26 : pixelCenter ? 24 : 23;
    for (uint32_t count = u32(); count; --count)
    {
        Draw d;
        d.hasStencilState = stencilState;
        d.hasBlendConstants = blendConstants;
        d.hasFetchMetadata = fetchMetadata;
        d.hasContext = fullContext;
        const uint32_t nameLength = u32();
        d.vs.assign(reinterpret_cast<const char*>(bytes(nameLength)), nameLength);
        if (d.vs == "#clear")
        {
            d.clear = true;
            for (uint32_t r = 0; r < regCount; ++r) d.regs[r] = u32();
            d.clearFlags = u32();
            d.clearColor = u32();
            d.clearDepth = AsFloat(u32());
            d.clearStencil = u32();
            for (uint32_t n = u32(); n; --n)
            {
                const int32_t left = int32_t(u32()), top = int32_t(u32());
                const int32_t right = int32_t(u32()), bottom = int32_t(u32());
                d.clearRects.emplace_back(left, top, right, bottom);
            }
            draws.push_back(std::move(d));
            continue;
        }
        if (d.vs == "#resolve")
        {
            d.resolve = true;
            d.copyDest = u32();
            d.copyControl = u32();
            d.copySurface = u32();
            d.copyColor = u32();
            d.copyDepth = u32();
            d.copyDestInfo = u32();
            d.copyWidth = u32();
            d.copyHeight = u32();
            if (magic == 0x35524446 || stencilState) { d.copyX = u32(); d.copyY = u32(); d.copyMip = u32(); d.copyMipCount = u32(); }
            draws.push_back(d);
            continue;
        }
        d.primitive = u32();
        d.indexCount = u32();
        d.indexBytes = u32();
        d.indices = bytes(d.indexBytes);
        d.stride = u32();
        d.vertexBytes = u32();
        d.vertices = bytes(d.vertexBytes);
        for (uint32_t e = u32(); e; --e)
        {
            const uint32_t length = u32();
            d.semantics.emplace_back(reinterpret_cast<const char*>(bytes(length)), length);
        }
        d.constants = bytes(4096);
        d.index32 = u32();
        for (uint32_t r = 0; r < regCount; ++r) d.regs[r] = u32();
        d.hasPixelCenter = pixelCenter;
        for (int32_t& slot : d.slots) slot = int32_t(u32());
        const uint32_t psLength = u32();
        d.ps.assign(reinterpret_cast<const char*>(bytes(psLength)), psLength);
        d.psConstants = bytes(4096);
        for (uint32_t& b : d.bases) b = u32();
        for (uint32_t& w : d.swizzles) w = u32();
        for (uint32_t& r : d.boolLoop) r = u32();
        d.vertexFetch = bytes(512);
        for (int32_t& e : d.expAdjust) e = int32_t(u32());
        if (magic != 0x50524446)
            for (uint32_t& f : d.fetchFormats) f = u32();
        if (magic == 0x33524446 || pixelCenter)
        {
            d.hasSamplerStates = true;
            for (uint32_t& s : d.samplerStates) s = u32();
        }
        if (fetchMetadata)
            for (auto& fetch : d.textureFetches) for (uint32_t& word : fetch) word = u32();
        if (fullContext) for (uint32_t& word : d.context) word = u32();
        draws.push_back(d);
    }
    if (at != file.size()) throw std::runtime_error("Unexpected trailing replay data");
    return draws;
}


}
ReplayFrame ReplayFrame::Load(const std::string& directory)
{
    ReplayFrame frame;
    frame.replay = Read(directory + "/replay.bin", true);
    frame.draws = ParseReplay(frame.replay);
    if (frame.draws.empty()) throw std::runtime_error("Empty or invalid replay");
    const auto statePath=directory+"/native-state.bin";
    if(std::filesystem::exists(statePath))
    {
        const auto state=Read(statePath,true);
        size_t at=0;
        auto word=[&] {
            if(at>state.size()||state.size()-at<4)throw std::runtime_error("Truncated native replay state");
            uint32_t value;memcpy(&value,state.data()+at,4);at+=4;return value;
        };
        if(word()!=0x534E4446||word()!=1||word()!=frame.draws.size())throw std::runtime_error("Invalid native replay state header");
        frame.frontBufferBase=word();
        const uint64_t low=word(),high=word();
        if((low|(high<<32))!=Fingerprint(frame.replay))throw std::runtime_error("Native replay state belongs to a different frame");
        for(auto& draw:frame.draws)
        {
            draw.cpuTextureMask=word();
            if(draw.resolve)for(auto& reg:draw.regs)reg=word();
        }
        if(at!=state.size())throw std::runtime_error("Unexpected native replay state data");
    }
    frame.textures = Read(directory + "/textures.rgba", true);
    frame.mips = Read(directory + "/mips.rgba");
    frame.cubes = Read(directory + "/cubes.rgba");
    frame.volumes = Read(directory + "/volumes.rgba");
    frame.streams = Read(directory + "/streams.bin");
    frame.debugPS = Read(directory + "/debug.ps.dxil");
    frame.depthVS = Read(directory + "/depth_pack.vs.dxil");
    frame.depthPS = Read(directory + "/depth_pack.ps.dxil");
    frame.depthFloatPS = Read(directory + "/depth_float.ps.dxil");
    return frame;
}
FrameInput ReplayFrame::View() const
{
    FrameInput frame{draws, textures, mips, cubes, streams, debugPS, depthVS, depthPS, depthFloatPS};
    frame.frontBufferBase=frontBufferBase;
    frame.volumes=volumes;
    return frame;
}
void ReplayFrame::Save(const FrameInput& frame,const std::string& directory)
{
    if(!frame.resources.empty())
    {
        TextureTables tables;for(const auto& resource:frame.resources)tables.AddResource(resource);
        const auto packed=tables.Pack();auto input=frame;input.resources={};
        input.textures=packed.textures;input.mips=packed.mips;input.cubes=packed.cubes;
        input.volumes=packed.volumes;
        Save(input,directory);return;
    }
    static_assert(std::endian::native==std::endian::little);
    std::filesystem::create_directories(directory);
    auto save=[&](const char* name,std::span<const uint8_t> bytes) {
        std::ofstream file(std::filesystem::path(directory)/name,std::ios::binary);
        if(!bytes.empty())file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
        file.close();if(!file)throw std::runtime_error("Cannot write native frame asset");
    };
    std::vector<uint8_t> bytes;
    auto payload=[&](const void* data,size_t size) {
        if(size&&!data)throw std::runtime_error("Missing native frame serialization payload");
        if(size){auto* begin=static_cast<const uint8_t*>(data);bytes.insert(bytes.end(),begin,begin+size);}
    };
    auto word=[&](uint32_t value){payload(&value,4);};
    auto text=[&](const std::string& value){if(value.size()>UINT32_MAX)throw std::runtime_error("Native frame name too long");word(uint32_t(value.size()));payload(value.data(),value.size());};
    if(frame.draws.empty()||frame.draws.size()>UINT32_MAX)throw std::runtime_error("Invalid native frame command count");
    word(0x39524446);word(uint32_t(frame.draws.size()));
    for(const auto& draw:frame.draws)
    {
        text(draw.clear?"#clear":draw.resolve?"#resolve":draw.vs);
        if(draw.clear)
        {
            payload(draw.regs,sizeof(draw.regs));word(draw.clearFlags);word(draw.clearColor);word(std::bit_cast<uint32_t>(draw.clearDepth));word(draw.clearStencil);
            word(uint32_t(draw.clearRects.size()));
            for(const auto& rectangle:draw.clearRects){word(rectangle.left);word(rectangle.top);word(rectangle.right);word(rectangle.bottom);}
            continue;
        }
        if(draw.resolve)
        {
            for(auto value:{draw.copyDest,draw.copyControl,draw.copySurface,draw.copyColor,draw.copyDepth,draw.copyDestInfo,
                draw.copyWidth,draw.copyHeight,draw.copyX,draw.copyY,draw.copyMip,draw.copyMipCount})word(value);
            continue;
        }
        if(!draw.hasContext||!draw.hasFetchMetadata||!draw.hasSamplerStates||!draw.hasPixelCenter||!draw.hasBlendConstants||!draw.hasStencilState)
            throw std::runtime_error("Native frame serialization requires complete draw state");
        word(draw.primitive);word(draw.indexCount);word(draw.indexBytes);payload(draw.indices,draw.indexBytes);
        word(draw.stride);word(draw.vertexBytes);payload(draw.vertices,draw.vertexBytes);
        word(uint32_t(draw.semantics.size()));for(const auto& semantic:draw.semantics)text(semantic);
        payload(draw.constants,4096);word(draw.index32);payload(draw.regs,sizeof(draw.regs));payload(draw.slots,sizeof(draw.slots));
        text(draw.ps);payload(draw.psConstants,4096);payload(draw.bases,sizeof(draw.bases));payload(draw.swizzles,sizeof(draw.swizzles));
        payload(draw.boolLoop,sizeof(draw.boolLoop));payload(draw.vertexFetch,512);payload(draw.expAdjust,sizeof(draw.expAdjust));
        payload(draw.fetchFormats,sizeof(draw.fetchFormats));payload(draw.samplerStates,sizeof(draw.samplerStates));
        payload(draw.textureFetches,sizeof(draw.textureFetches));payload(draw.context,sizeof(draw.context));
    }
    save("replay.bin",bytes);save("textures.rgba",frame.textures);save("mips.rgba",frame.mips);save("cubes.rgba",frame.cubes);save("streams.bin",frame.streams);
    save("volumes.rgba",frame.volumes);
    save("debug.ps.dxil",frame.debugPS);save("depth_pack.vs.dxil",frame.depthVS);save("depth_pack.ps.dxil",frame.depthPS);save("depth_float.ps.dxil",frame.depthFloatPS);
    // Keep the established FDR9 adapter compatible while retaining live CPU
    // ownership and copy source state. A fingerprint rejects stale sidecars.
    const auto fingerprint=Fingerprint(bytes);
    bytes.clear();word(0x534E4446);word(1);word(uint32_t(frame.draws.size()));word(frame.frontBufferBase);
    word(uint32_t(fingerprint));word(uint32_t(fingerprint>>32));
    for(const auto& draw:frame.draws){word(draw.cpuTextureMask);if(draw.resolve)payload(draw.regs,sizeof(draw.regs));}
    save("native-state.bin",bytes);
}
}
