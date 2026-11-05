#include "geometry.h"
#include <algorithm>
#include <typeinfo>
#include <limits>
#include <bit>
#include <cmath>
#include <cstring>
#include <regex>
#include <stdexcept>

namespace gpu::native
{
namespace
{
uint32_t Big(std::span<const uint8_t> data, size_t offset, uint32_t size = 4)
{
    if (offset > data.size() || size > data.size() - offset) throw std::runtime_error("Truncated guest geometry");
    uint32_t value = 0;
    for (uint32_t i = 0; i < size; ++i) value = value << 8 | data[offset + i];
    return value;
}
int32_t Signed(uint32_t value, uint32_t bits)
{
    const uint32_t sign = 1u << (bits - 1);
    return int32_t((value ^ sign) - sign);
}
void Append(std::vector<uint8_t>& bytes, uint32_t value, uint32_t size = 4)
{
    for (uint32_t i = 0; i < size; ++i) bytes.push_back(uint8_t(value >> (8 * i)));
}
void Float(std::vector<uint8_t>& bytes, float value) { Append(bytes, std::bit_cast<uint32_t>(value)); }
// Big-endian 32-bit words -> little-endian, in bulk (vectorizable).
void SwapWords(uint8_t* destination, const uint8_t* source, size_t bytes)
{
    for (size_t at = 0; at + 4 <= bytes; at += 4)
    {
        uint32_t value; memcpy(&value, source + at, 4);
        value = __builtin_bswap32(value);
        memcpy(destination + at, &value, 4);
    }
}
uint32_t Stream(StreamArena& arena, uint32_t address, std::span<const uint8_t> bytes)
{
    uint64_t revision=arena.revision?arena.revision(address,uint32_t(bytes.size())):0;
    for (auto entry = arena.entries.rbegin(); entry != arena.entries.rend(); ++entry)
    {
        if (entry->address != address || entry->size != bytes.size()) continue;
        if(arena.revision)
        {
            if(entry->revision==revision)return entry->offset;
            continue;
        }
        bool equal = true;
        for (size_t at = 0; at < bytes.size(); at += 4)
        {
            uint32_t cached; memcpy(&cached, arena.bytes.data() + entry->offset + at, 4);
            if (cached != Big(bytes, at)) { equal = false; break; }
        }
        if (equal) return entry->offset;
    }
    if (bytes.size() % 4 || arena.bytes.size() + bytes.size() > UINT32_MAX)
        throw std::runtime_error("Native vertex stream arena overflow");
    const uint32_t offset = uint32_t(arena.bytes.size());
    StreamArena::Converted* cached = nullptr;
    if (arena.revision && arena.persistent)
    {
        cached = &arena.persistent->streams[{address, uint32_t(bytes.size())}];
        cached->used = arena.persistent->frame;
        if (cached->revision == revision && cached->bytes.size() == bytes.size() && !cached->bytes.empty())
        {
            arena.bytes.insert(arena.bytes.end(), cached->bytes.begin(), cached->bytes.end());
            arena.entries.push_back({address, offset, uint32_t(bytes.size()), revision});
            return offset;
        }
    }
    for(unsigned attempt=0;;++attempt)
    {
        arena.bytes.resize(size_t(offset) + bytes.size());
        SwapWords(arena.bytes.data() + offset, bytes.data(), bytes.size());
        if(!arena.revision||arena.revision(address,uint32_t(bytes.size()))==revision)break;
        arena.bytes.resize(offset);
        if(attempt==3)throw std::runtime_error("Vertex stream changed repeatedly during native acquisition");
        revision=arena.revision(address,uint32_t(bytes.size()));
    }
    if (cached)
    {
        cached->revision = revision;
        cached->bytes.assign(arena.bytes.begin() + offset, arena.bytes.end());
    }
    arena.entries.push_back({address, offset, uint32_t(bytes.size()),revision});
    return offset;
}
struct Format { uint32_t size, components; bool floating, half; };
Format Describe(uint32_t format)
{
    switch (format)
    {
    case 6: return {1, 4, false, false};
    case 25: return {2, 2, false, false};
    case 26: return {2, 4, false, false};
    case 31: return {2, 2, true, true};
    case 32: return {2, 4, true, true};
    case 33: return {4, 1, false, false};
    case 34: return {4, 2, false, false};
    case 35: return {4, 4, false, false};
    case 36: return {4, 1, true, false};
    case 37: return {4, 2, true, false};
    case 38: return {4, 4, true, false};
    case 57: return {4, 3, true, false};
    default: return {};
    }
}
float Half(uint32_t bits)
{
    const float sign = bits & 0x8000 ? -1.0f : 1.0f;
    const uint32_t exponent = bits >> 10 & 31, mantissa = bits & 1023;
    if (!exponent) return sign * std::ldexp(float(mantissa), -24);
    if (exponent == 31) return std::bit_cast<float>((bits & 0x8000) << 16 | 0x7F800000 | mantissa << 13);
    return sign * std::ldexp(float(1024 + mantissa), int(exponent) - 25);
}
struct Fetch
{
    uint32_t destination, constant, format, stride;
    int32_t offset, exponent;
    bool signedValue, integer, mini,rounded=false;
};
Fetch Decode(uint32_t w0, uint32_t w1, uint32_t w2)
{
    return {w0 >> 12 & 63, (w0 >> 20 & 31) * 3 + (w0 >> 25 & 3), w1 >> 16 & 63, w2 & 255,
        Signed(w2 >> 8 & 0x7FFFFF, 23), Signed(w1 >> 24 & 63, 6),
        bool(w1 & 0x1000), bool(w1 & 0x2000), bool(w1 & 0x40000000),bool(w1&0x8000)};
}
std::vector<Fetch> Fetches(std::span<const uint8_t> code,bool inheritMini=false)
{
    std::vector<uint32_t> slots;
    uint32_t bound = uint32_t(code.size() / 12);
    for (uint32_t i = 0; i < bound; ++i)
    {
        const uint32_t w0 = Big(code, i * 12), w1 = Big(code, i * 12 + 4), w2 = Big(code, i * 12 + 8);
        for (auto [d0, d1] : {std::pair{w0, w1 & 65535}, std::pair{w1 >> 16 | w2 << 16, w2 >> 16}})
        {
            const uint32_t opcode = d1 >> 12 & 15;
            if (!((opcode >= 1 && opcode <= 6) || opcode == 13 || opcode == 14)) continue;
            const uint32_t address = d0 & 4095, count = d0 >> 12 & 7, sequence = d0 >> 16 & 4095;
            bound = (std::min)(bound, address);
            for (uint32_t k = 0; k < count; ++k) if (sequence >> (2 * k) & 1) slots.push_back(address + k);
        }
    }
    std::vector<Fetch> result;
    Fetch full{};bool hasFull=false;
    for (uint32_t slot : slots)
    {
        if (uint64_t(slot) * 12 + 12 > code.size()) throw std::runtime_error("Vertex fetch outside live shader snapshot");
        const uint32_t w0 = Big(code, slot * 12);
        if (w0 & 31) continue;
        auto fetch = Decode(w0, Big(code, slot * 12 + 4), Big(code, slot * 12 + 8));
        if(inheritMini&&fetch.mini)
        {
            if(!hasFull)throw std::runtime_error("Mini vertex fetch has no full addressing instruction");
            fetch.constant=full.constant;fetch.stride=full.stride;fetch.rounded=full.rounded;
        }
        else if(!fetch.mini){full=fetch;hasFull=true;}
        if (Describe(fetch.format).size) result.push_back(fetch);
    }
    return result;
}
void Element(std::vector<uint8_t>& output, std::span<const uint8_t> bytes, size_t offset, const Fetch& fetch)
{
    const auto format = Describe(fetch.format);
    if (!format.size) throw std::runtime_error("Unsupported vertex element format");
    for (uint32_t component = 0; component < 4; ++component)
    {
        float value = component == 3 ? 1.0f : 0.0f;
        if (component < format.components)
        {
            const uint32_t raw = Big(bytes, offset + component * format.size, format.size);
            if (format.floating) value = format.half ? Half(raw) : std::bit_cast<float>(raw);
            else
            {
                double converted = format.size < 4 && fetch.signedValue ? double(Signed(raw, format.size * 8)) : double(raw);
                if (format.size < 4)
                {
                    if (!fetch.integer) converted = fetch.signedValue ?
                        (std::max)(converted / ((1u << (format.size * 8 - 1)) - 1), -1.0) : converted / ((1u << (format.size * 8)) - 1);
                    converted = std::ldexp(converted, fetch.exponent);
                }
                value = float(converted);
            }
        }
        Float(output, value);
    }
}
}

VertexProgram VertexProgram::FromShader(const NativeShader& shader)
{
    if (shader.stage != ShaderStage::Vertex) throw std::runtime_error("Geometry requires a vertex shader");
    VertexProgram result;
    const uint32_t offset = Big(shader.virtualData, 24);
    result.codeOffset = Big(shader.virtualData, offset);
    const std::regex pattern(R"(r(\d+)\.\w+ = /\* FDVFETCH ([A-Z]+[0-9]+) )");
    for (std::sregex_iterator i(shader.source.begin(), shader.source.end(), pattern), end; i != end; ++i)
        result.inputs.push_back({uint32_t(std::stoul((*i)[1].str())), (*i)[2].str()});
    if (result.inputs.empty()) throw std::runtime_error("Vertex shader has no native fetch metadata");
    return result;
}

Geometry BuildGeometry(const VertexProgram& program, std::span<const uint8_t> livePhysical,
    std::span<const uint32_t, 192> constants, uint32_t primitive, uint32_t count,
    bool indexed, bool index32, uint32_t indexAddress, const GeometryReader& read, StreamArena& arena,const GeometryBounds* bounds)
{
    if (!count || count > 1u << 20 || (primitive != 4 && primitive != 8) || program.codeOffset > livePhysical.size())
        throw std::runtime_error("Unsupported native geometry draw");
    const bool pullPrimary=bounds&&bounds->pullPrimary&&primitive==4;
    const auto live = Fetches(livePhysical.subspan(program.codeOffset),pullPrimary);
    struct Input { Fetch fetch; std::string semantic; bool pulled; };
    std::vector<Input> inputs;
    std::vector<bool> used(live.size());
    Geometry result;
    uint32_t primaryStride = 0;
    for (size_t ordinal = 0; ordinal < program.inputs.size(); ++ordinal)
    {
        const auto& input = program.inputs[ordinal];
        size_t match = 0;
        while (match < live.size() && (used[match] || live[match].destination != input.destination)) ++match;
        if (match == live.size()) throw std::runtime_error("Live vertex fetch does not match native shader metadata at r"+
            std::to_string(input.destination)+" ("+input.semantic+", "+std::to_string(live.size())+" decoded fetches)");
        used[match] = true;
        const auto& fetch = live[match];
        if (fetch.offset < 0 || fetch.constant > 95) throw std::runtime_error("Invalid native vertex fetch");
        const bool pulled = fetch.constant != 95||pullPrimary;
        if(fetch.constant==95&&!primaryStride&&!fetch.mini)primaryStride=fetch.stride*4;
        if (pulled)
        {
            if (ordinal >= 32) throw std::runtime_error("Too many pulled vertex attributes");
            const uint32_t word0 = constants[fetch.constant * 2], word1 = constants[fetch.constant * 2 + 1];
            const uint32_t size = (word1 >> 2 & 0xFFFFFF) * 4;
            if ((word0 & 3) != 3 || !size || size > 32u << 20) throw std::runtime_error("Invalid pulled vertex stream");
            auto bytes = read(word0 & 0x1FFFFFFC, size);
            if (bytes.size() < size || bytes.size() > 32u << 20) throw std::runtime_error("Missing pulled vertex stream");
            auto* descriptor = result.fetches.data() + ordinal * 4;
            descriptor[0] = Stream(arena, word0 & 0x1FFFFFFC, bytes);
            descriptor[1] = fetch.stride * 4 | uint32_t(fetch.offset) * 4 << 16;
            descriptor[2] = uint32_t(bytes.size());
            descriptor[3] = fetch.format | uint32_t(fetch.signedValue) << 8 | uint32_t(fetch.integer) << 9 |
                (uint32_t(fetch.exponent) & 63) << 10 | (pullPrimary?uint32_t(fetch.rounded)<<16:0) | 1u << 31;
        }
        else if (!primaryStride && !fetch.mini) primaryStride = fetch.stride * 4;
        auto existing = std::find_if(inputs.begin(), inputs.end(), [&](const Input& value) { return value.semantic == input.semantic; });
        if (existing == inputs.end()) inputs.push_back({fetch, input.semantic, pulled});
        else if (existing->pulled && !pulled) { inputs.erase(existing); inputs.push_back({fetch, input.semantic, false}); }
    }
    if (!primaryStride) throw std::runtime_error("Missing primary vertex stride");
    const uint32_t primarySize = (constants[191] >> 2 & 0xFFFFFF) * 4;
    if ((constants[190] & 3) != 3 || !primarySize || primarySize > 32u << 20) throw std::runtime_error("Invalid primary vertex stream");
    const auto primary = read(constants[190] & 0x1FFFFFFC, primarySize);
    if (primary.size() < primarySize || primary.size() > 32u << 20) throw std::runtime_error("Missing primary vertex stream");
    result.stride = uint32_t(inputs.size()) * 16;
    for (const auto& input : inputs) result.semantics.push_back(input.semantic);
    size_t vertexCount = primary.size() / primaryStride;
    const uint32_t unit = indexed&& !index32 ? 2 : 4;
    std::span<const uint8_t> guestIndices;
    uint32_t minimum=0,maximum=count-1;
    if(indexed)
    {
        guestIndices=read(indexAddress&0x1FFFFFFF,count*unit);
        if(guestIndices.size()<uint64_t(count)*unit)throw std::runtime_error("Missing native index data");
        if(bounds)guestIndices=guestIndices.first(size_t(count)*unit);
        // Own and endian-convert once, with the bounds reduction in the same
        // pass. Per-byte push_back used to recheck capacity millions of times
        // per frame and the bounds pass separately decoded every index again.
        static_assert(std::endian::native==std::endian::little);
        const uint32_t indexBytes=uint32_t(guestIndices.size()/unit*unit);
        const uint32_t indexBase=indexAddress&0x1FFFFFFF;
        StreamArena::Converted* cachedIndices=nullptr;
        uint64_t indexRevision=0;
        if(arena.revision&&arena.persistent)
        {
            indexRevision=arena.revision(indexBase,indexBytes);
            cachedIndices=&arena.persistent->indices[{indexBase,indexBytes|uint32_t(unit==4)<<31}];
            cachedIndices->used=arena.persistent->frame;
        }
        if(cachedIndices&&cachedIndices->revision==indexRevision&&cachedIndices->bytes.size()==indexBytes&&indexBytes)
        {
            result.indices=cachedIndices->bytes;
            minimum=cachedIndices->minimum;maximum=cachedIndices->maximum;
        }
        else
        {
            result.indices.resize(indexBytes);
            minimum=UINT32_MAX;maximum=0;
            auto convert=[&]<typename Index>()
            {
                const size_t n=indexBytes/sizeof(Index);
                const uint8_t* source=guestIndices.data();
                uint8_t* destination=result.indices.data();
                Index low=(std::numeric_limits<Index>::max)(),high=0;
                for(size_t i=0;i<n;++i)
                {
                    Index value;memcpy(&value,source+i*sizeof(Index),sizeof(value));
                    if constexpr(sizeof(Index)==2)value=__builtin_bswap16(value);else value=__builtin_bswap32(value);
                    memcpy(destination+i*sizeof(Index),&value,sizeof(value));
                    low=(std::min)(low,value);high=(std::max)(high,value);
                }
                if(n){minimum=uint32_t(low);maximum=uint32_t(high);}
            };
            if(unit==2)convert.operator()<uint16_t>();else convert.operator()<uint32_t>();
            if(cachedIndices&&arena.revision(indexBase,indexBytes)==indexRevision)
            {
                cachedIndices->revision=indexRevision;cachedIndices->bytes=result.indices;
                cachedIndices->minimum=minimum;cachedIndices->maximum=maximum;
            }
        }
    }
    if(bounds)
    {
        const int64_t first=int64_t(minimum)+bounds->indexOffset,last=int64_t(maximum)+bounds->indexOffset;
        if(first<0||last>=int64_t(vertexCount))throw std::runtime_error("Native indices exceed primary vertex stream");
        vertexCount=size_t(last)+1;
    }
    result.vertices.reserve(pullPrimary?result.stride:vertexCount * result.stride);
    if(pullPrimary){result.vertices.resize(result.stride,0);result.stride=0;}
    for (size_t vertex = 0; !pullPrimary&&vertex < vertexCount; ++vertex)
        for (const auto& input : inputs)
            if (input.pulled) result.vertices.insert(result.vertices.end(), 16, 0);
            else Element(result.vertices, primary, vertex * primaryStride + size_t(input.fetch.offset) * 4, input.fetch);
    result.primitive = primitive;
    result.indexCount = count;
    result.index32 = indexed ? uint32_t(index32) : 1;
    if (!indexed)
    {
        result.indices.resize(size_t(count)*4);
        for(uint32_t index=0;index<count;++index)memcpy(result.indices.data()+size_t(index)*4,&index,4);
    }
    if (primitive == 8)
    {
        if (count % 3) throw std::runtime_error("Incomplete rectangle list");
        std::vector<uint8_t> expanded, indices;
        auto indexAt = [&](uint32_t index) {
            uint32_t value = 0;
            for (uint32_t b = 0; b < unit; ++b) value |= uint32_t(result.indices[index * unit + b]) << (8 * b);
            return value;
        };
        for (uint32_t start = 0; start < count; start += 3)
        {
            std::vector<std::vector<float>> vertices(3, std::vector<float>(result.stride / 4));
            for (uint32_t k = 0; k < 3; ++k)
            {
                const int64_t index = int64_t(indexAt(start + k))+(bounds?bounds->indexOffset:0);
                if (index<0||index >= int64_t(vertexCount)) throw std::runtime_error("Rectangle index outside vertex stream");
                memcpy(vertices[k].data(), result.vertices.data() + size_t(index) * result.stride, result.stride);
            }
            auto distance = [&](uint32_t a, uint32_t b) {
                const double x = double(vertices[a][0]) - vertices[b][0], y = double(vertices[a][1]) - vertices[b][1];
                return x * x + y * y;
            };
            const double e12 = distance(1,2), e20 = distance(2,0), e01 = distance(0,1);
            const uint32_t a = e12 >= e20 && e12 >= e01 ? 0 : e20 >= e01 ? 1 : 2, b = (a + 1) % 3, c = (a + 2) % 3;
            for (uint32_t k : {a,b,c}) for (float value : vertices[k]) Float(expanded, value);
            for (size_t k = 0; k < vertices[a].size(); ++k) Float(expanded, float(double(vertices[b][k]) + vertices[c][k] - vertices[a][k]));
            const uint32_t base = start / 3 * 4;
            if (base + 3 > 65535) throw std::runtime_error("Expanded rectangle indices exceed 16 bits");
            for (uint32_t index : {base,base+1,base+2,base+2,base+1,base+3}) Append(indices, index, 2);
        }
        result.vertices = std::move(expanded); result.indices = std::move(indices);
        result.index32 = 0; result.primitive = 4; result.indexCount = count / 3 * 6;
        result.replacesIndexOffset=bounds!=nullptr;
    }
    return result;
}

void Geometry::Bind(Draw& draw) const
{
    draw.primitive = primitive; draw.indexCount = indexCount; draw.index32 = index32; draw.stride = stride;
    draw.indices = indices.data(); draw.indexBytes = uint32_t(indices.size());
    draw.vertices = vertices.data(); draw.vertexBytes = uint32_t(vertices.size()); draw.semantics = semantics;
    draw.vertexFetch = reinterpret_cast<const uint8_t*>(fetches.data());
    if(replacesIndexOffset){draw.regs[VGT_INDX_OFFSET]=0;draw.context[0x102]=0;}
}
}
