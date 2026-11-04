// Build against the official replay API headers matching the installed DLL.
#include "renderdoc_replay.h"
#include <cstdio>
#include <chrono>
#include <cmath>
#include <limits>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>

#define XXH_INLINE_ALL
#include "../../third_party/xenia/third_party/xxhash/xxhash.h"

REPLAY_PROGRAM_MARKER();

static void Actions(const rdcarray<ActionDescription>& actions, const SDFile& file)
{
    for (const auto& action : actions)
    {
        uint64_t output = 0;
        memcpy(&output, &action.outputs[0], sizeof(output));
        printf("event=%u indices=%u output=%llu flags=%llu name=%s\n", action.eventId, action.numIndices,
               (unsigned long long)output, (unsigned long long)action.flags, action.GetName(file).c_str());
        Actions(action.children, file);
    }
}

static const ActionDescription* Find(const rdcarray<ActionDescription>& actions, uint32_t event)
{
    for (const auto& action : actions)
    {
        if (action.eventId == event) return &action;
        if (const auto* found = Find(action.children, event)) return found;
    }
    return nullptr;
}

static void Variable(const ShaderVariable& value, const char* prefix)
{
    printf("%s %s = (%g, %g, %g, %g)\n", prefix, value.name.c_str(),
           value.value.f32v[0], value.value.f32v[1], value.value.f32v[2], value.value.f32v[3]);
    for (const auto& member : value.members) Variable(member, prefix);
}

static uint64_t Id(ResourceId resource)
{
    uint64_t id;
    memcpy(&id, &resource, sizeof(id));
    return id;
}

static const char* Name(IReplayController* controller, ResourceId id)
{
    for (const auto& resource : controller->GetResources())
        if (resource.resourceId == id) return resource.name.c_str();
    return "";
}

static uint32_t Big32(const uint8_t* p)
{
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// Xenia names pipelines with XXH3 hashes of the original microcode. Pixel
// microcode is unchanged at bind time, so these join captures to container IDs.
static int ShaderHashes(const char* directory)
{
    for (const auto& entry : std::filesystem::directory_iterator(directory))
    {
        if (entry.path().extension() != ".bin") continue;
        std::ifstream input(entry.path(), std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
        if (bytes.size() < 36) continue;
        const uint32_t header = Big32(bytes.data() + 24);
        if (uint64_t(header) + 8 > bytes.size()) continue;
        const uint32_t start = Big32(bytes.data() + 4) + Big32(bytes.data() + header);
        const uint32_t size = Big32(bytes.data() + header + 4);
        if (uint64_t(start) + size > bytes.size()) continue;
        printf("%s %s %016llX\n", entry.path().stem().string().c_str(),
               Big32(bytes.data()) & 1 ? "vs" : "ps",
               (unsigned long long)XXH3_64bits(bytes.data() + start, size));
    }
    return 0;
}

static bool SaveTarget(IReplayController* controller, uint32_t event, uint32_t slot, const char* path)
{
    controller->SetFrameEvent(event, true);
    const auto* pipeline = controller->GetD3D12PipelineState();
    if (!pipeline || pipeline->outputMerger.renderTargets.size() <= slot ||
        pipeline->outputMerger.renderTargets[slot].resource == ResourceId::Null())
    {
        fprintf(stderr, "No colour target at event %u\n", event);
        return false;
    }
    const auto& target = pipeline->outputMerger.renderTargets[slot];
    TextureSave save;
    save.resourceId = target.resource;
    save.typeCast = target.format.compType;  // Typeless storage must use the bound view.
    save.destType = FileType::DDS;
    auto status = controller->SaveTexture(save, path);
    if (!status.OK()) fprintf(stderr, "SaveTexture failed at event %u: %d\n", event, int(status.code));
    return status.OK();
}

static void DrawStates(IReplayController* controller, const rdcarray<ActionDescription>& actions, uint32_t last)
{
    for (const auto& action : actions)
    {
        if (action.eventId <= last && (action.flags & ActionFlags::Drawcall))
        {
            controller->SetFrameEvent(action.eventId, false);
            const auto* p = controller->GetD3D12PipelineState();
            if (p) printf("event=%u indices=%u pipeline=%llu vs=%llu ps=%llu rt=%llu reflection=%u name=%s\n",
                action.eventId, action.numIndices, (unsigned long long)Id(p->pipelineResourceId),
                (unsigned long long)Id(p->vertexShader.resourceId), (unsigned long long)Id(p->pixelShader.resourceId),
                (unsigned long long)(p->outputMerger.renderTargets.empty() ? 0 : Id(p->outputMerger.renderTargets[0].resource)),
                p->pixelShader.reflection != nullptr, Name(controller, p->pipelineResourceId));
        }
        DrawStates(controller, action.children, last);
    }
}

static void DrawIndex(IReplayController* controller, const rdcarray<ActionDescription>& actions,
                      const std::unordered_map<uint32_t, ResourceId>& pipelines)
{
    for (const auto& action : actions)
    {
        if ((action.flags & ActionFlags::Drawcall) && !action.events.empty())
        {
            const auto found = pipelines.find(action.events.back().chunkIndex);
            if (found != pipelines.end())
                printf("event=%u indices=%u pipeline=%llu rt=%llu name=%s\n", action.eventId, action.numIndices,
                       (unsigned long long)Id(found->second), (unsigned long long)Id(action.outputs[0]), Name(controller, found->second));
        }
        DrawIndex(controller, action.children, pipelines);
    }
}

static void DrawIndex(IReplayController* controller)
{
    // Pipeline binding is already recorded in structured chunks. Index it once
    // instead of replaying thousands of events merely to read their PSO names.
    std::unordered_map<uint64_t, ResourceId> current;
    std::unordered_map<uint32_t, ResourceId> draws;
    const auto& chunks = controller->GetStructuredFile().chunks;
    for (uint32_t i = 0; i < chunks.size(); ++i)
    {
        const auto* chunk = chunks[i];
        const auto* commandList = chunk->FindChild("pCommandList");
        if (!commandList) continue;
        const uint64_t list = Id(commandList->AsResourceId());
        if (strstr(chunk->name.c_str(), "::SetPipelineState"))
        {
            if (const auto* pipeline = chunk->FindChild("pPipelineState")) current[list] = pipeline->AsResourceId();
        }
        else if (strstr(chunk->name.c_str(), "::Reset"))
        {
            if (const auto* pipeline = chunk->FindChild("pInitialState")) current[list] = pipeline->AsResourceId();
        }
        else if (strstr(chunk->name.c_str(), "::Draw")) draws[i] = current[list];
    }
    DrawIndex(controller, controller->GetRootActions(), draws);
}

int main(int argc, char** argv)
{
    if (argc == 3 && strcmp(argv[1], "--shader-hashes") == 0) return ShaderHashes(argv[2]);
    if (argc < 2) { fprintf(stderr, "Usage: replay_inspect capture.rdc [eventId output.png [pixelX pixelY [primitiveId]]]\n"); return 2; }
    RENDERDOC_InitialiseReplay(GlobalEnvironment(), rdcarray<rdcstr>());
    ICaptureFile* file = RENDERDOC_OpenCaptureFile();
    auto status = file->OpenFile(argv[1], "rdc", nullptr);
    if (!status.OK()) { fprintf(stderr, "OpenFile failed: %d\n", int(status.code)); file->Shutdown(); return 1; }
    auto opened = file->OpenCapture(ReplayOptions(), nullptr);
    file->Shutdown();
    if (!opened.first.OK()) { fprintf(stderr, "Replay failed: %d\n", int(opened.first.code)); return 1; }
    IReplayController* controller = opened.second;
    fprintf(stderr, "Capture API=%u\n", uint32_t(controller->GetAPIProperties().pipelineType));
    int result = 0;
    if(argc==5&&strcmp(argv[2],"--mesh-out")==0)
    {
        const uint32_t event=uint32_t(strtoul(argv[3],nullptr,10));
        controller->SetFrameEvent(event,true);
        const auto mesh=controller->GetPostVSData(0,0,MeshDataStage::VSOut);
        const uint64_t requested=uint64_t(mesh.numIndices)*mesh.vertexByteStride;
        if(!mesh.vertexByteStride||!requested||requested>128ull*1024*1024)
        {
            fprintf(stderr,"No bounded post-VS mesh available at event %u\n",event);result=1;
        }
        else
        {
            std::filesystem::create_directories(argv[4]);
            const auto vertices=controller->GetBufferData(mesh.vertexResourceId,mesh.vertexByteOffset,requested);
            const auto indices=mesh.indexByteStride?controller->GetBufferData(mesh.indexResourceId,mesh.indexByteOffset,uint64_t(mesh.numIndices)*mesh.indexByteStride):bytebuf();
            auto save=[&](const char* name,const bytebuf& bytes){std::ofstream file(std::filesystem::path(argv[4])/name,std::ios::binary);file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());return bool(file);};
            if(vertices.empty()||!save("vertices.bin",vertices)||!save("indices.bin",indices))result=1;
            printf("event=%u vertex_stride=%u index_stride=%u indices=%u base_vertex=%d vertex_bytes=%zu index_bytes=%zu components=%u component_bytes=%u component_type=%u\n",
                event,mesh.vertexByteStride,mesh.indexByteStride,mesh.numIndices,mesh.baseVertex,vertices.size(),indices.size(),mesh.format.compCount,mesh.format.compByteWidth,uint32_t(mesh.format.compType));
            if(mesh.format.compType==CompType::Float&&mesh.format.compByteWidth==4&&mesh.format.compCount==4&&mesh.vertexByteStride>=16)
            {
                float low[4],high[4];std::fill_n(low,4,std::numeric_limits<float>::infinity());std::fill_n(high,4,-std::numeric_limits<float>::infinity());
                size_t finite=0,invalid=0,zeroW=0;
                for(size_t at=0;at+16<=vertices.size();at+=mesh.vertexByteStride)
                {
                    float value[4];memcpy(value,vertices.data()+at,16);bool valid=true;
                    for(float component:value)valid=valid&&std::isfinite(component);
                    if(!valid){++invalid;continue;}++finite;if(value[3]==0)++zeroW;
                    for(unsigned c=0;c<4;++c){low[c]=(std::min)(low[c],value[c]);high[c]=(std::max)(high[c],value[c]);}
                }
                printf("finite_vertices=%zu nonfinite_vertices=%zu zero_w=%zu clip_min=%g,%g,%g,%g clip_max=%g,%g,%g,%g\n",finite,invalid,zeroW,low[0],low[1],low[2],low[3],high[0],high[1],high[2],high[3]);
            }
        }
    }
    else if (argc == 6 && strcmp(argv[2], "--pixel-history") == 0)
    {
        controller->SetFrameEvent(uint32_t(atoi(argv[3])), true);
        const auto* pipeline = controller->GetD3D12PipelineState();
        if (!pipeline || pipeline->outputMerger.renderTargets.empty()) result = 2;
        else
        {
            const auto& target = pipeline->outputMerger.renderTargets[0];
            auto history = controller->PixelHistory(target.resource, uint32_t(atoi(argv[4])), uint32_t(atoi(argv[5])), Subresource(), target.format.compType);
            for (const auto& modification : history)
                printf("event=%u primitive=%u rejected=%u post=(%g,%g,%g,%g) depth=%g\n", modification.eventId, modification.primitiveID,
                    modification.depthTestFailed || modification.stencilTestFailed || modification.shaderDiscarded || modification.backfaceCulled || modification.depthClipped,
                    modification.postMod.col.floatValue[0], modification.postMod.col.floatValue[1], modification.postMod.col.floatValue[2], modification.postMod.col.floatValue[3], modification.postMod.depth);
            if (history.empty()) result = 1;
        }
    }
    else if (argc == 4 && strcmp(argv[2], "--export-list") == 0)
    {
        // TSV: event ID, tab, RTV slot, tab, absolute destination DDS path. One replay session
        // serves every requested checkpoint; failures propagate to the caller.
        std::ifstream list(argv[3]);
        if (!list) result = 2;
        std::string line;
        while (std::getline(list, line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            const auto tab = line.find('\t');
            if (tab == std::string::npos) { result = 2; break; }
            const auto event = strtoul(line.substr(0, tab).c_str(), nullptr, 10);
            const auto secondTab = line.find('\t', tab + 1);
            const auto slotText = secondTab == std::string::npos ? "0" : line.substr(tab + 1, secondTab - tab - 1);
            const auto path = line.substr(secondTab == std::string::npos ? tab + 1 : secondTab + 1);
            if (!event || line.substr(0, tab).find_first_not_of("0123456789") != std::string::npos ||
                slotText.empty() || slotText.find_first_not_of("0123456789") != std::string::npos ||
                path.empty()) { result = 2; break; }
            if (!SaveTarget(controller, uint32_t(event), uint32_t(strtoul(slotText.c_str(), nullptr, 10)), path.c_str())) { result = 1; break; }
            printf("exported event=%lu slot=%s path=%s\n", event, slotText.c_str(), path.c_str());
        }
    }
    else if (argc == 6 && strcmp(argv[2], "--pixel-inputs") == 0)
    {
        controller->SetFrameEvent(uint32_t(strtoul(argv[3], nullptr, 10)), true);
        auto* trace = controller->DebugPixel(uint32_t(strtoul(argv[4], nullptr, 10)),
                                            uint32_t(strtoul(argv[5], nullptr, 10)), DebugPixelInputs());
        if (!trace || !trace->debugger) { fprintf(stderr, "Pixel is not debuggable\n"); result = 1; }
        else for (const auto& input : trace->inputs) Variable(input, "input");
        if (trace) controller->FreeTrace(trace);
    }
    else if (argc == 5 && strcmp(argv[2], "--inputs") == 0)
    {
        controller->SetFrameEvent(uint32_t(strtoul(argv[3], nullptr, 10)), true);
        std::filesystem::create_directories(argv[4]);
        const auto& textures = controller->GetTextures();
        for (const auto& access : controller->GetDescriptorAccess())
        {
            if (access.staticallyUnused) continue;
            if (access.type == DescriptorType::Sampler)
            {
                for (const auto& sampler : controller->GetSamplerDescriptors(access.descriptorStore, {DescriptorRange(access)}))
                    printf("stage=%u sampler=%u element=%u address=%u,%u,%u filter=%u,%u,%u lod=%g..%g bias=%g\n",
                           uint32_t(access.stage), access.index, access.arrayElement, uint32_t(sampler.addressU),
                           uint32_t(sampler.addressV), uint32_t(sampler.addressW), uint32_t(sampler.filter.magnify),
                           uint32_t(sampler.filter.minify), uint32_t(sampler.filter.mip), sampler.minLOD, sampler.maxLOD, sampler.mipBias);
                continue;
            }
            auto descriptors = controller->GetDescriptors(access.descriptorStore, {DescriptorRange(access)});
            if (access.type == DescriptorType::ConstantBuffer)
            {
                for (const auto& descriptor : descriptors)
                {
                    const auto bytes = controller->GetBufferData(descriptor.resource, descriptor.byteOffset,
                        descriptor.byteSize < 65536 ? descriptor.byteSize : 65536);
                    const auto path = std::filesystem::path(argv[4]) / ("constant_" + std::to_string(uint32_t(access.stage)) + "_" + std::to_string(access.index) + ".bin");
                    std::ofstream output(path, std::ios::binary);
                    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                    printf("stage=%u constant=%u bytes=%zu path=%s\n", uint32_t(access.stage), access.index, bytes.size(), path.string().c_str());
                }
                continue;
            }
            for (const auto& descriptor : descriptors)
                for (const auto& texture : textures)
                    if (texture.resourceId == descriptor.resource)
                    {
                        const auto path = std::filesystem::path(argv[4]) / ("input_" + std::to_string(Id(texture.resourceId)) + ".dds");
                        TextureSave save;
                        save.resourceId = texture.resourceId;
                        save.typeCast = descriptor.format.compType;
                        save.destType = FileType::DDS;
                        auto status = controller->SaveTexture(save, path.string().c_str());
                        if (!status.OK()) result = 1;
                        printf("stage=%u binding=%u element=%u texture=%llu %ux%u mips=%u type=%u path=%s\n",
                               uint32_t(access.stage), access.index, access.arrayElement, (unsigned long long)Id(texture.resourceId),
                               texture.width, texture.height, texture.mips, uint32_t(descriptor.format.compType), path.string().c_str());
                    }
        }
    }
    else if (argc == 3 && strcmp(argv[2], "--draw-index") == 0)
        DrawIndex(controller);
    else if (argc >= 4 && strcmp(argv[2], "--draw-state") == 0)
        DrawStates(controller, controller->GetRootActions(), uint32_t(strtoul(argv[3], nullptr, 10)));
    else if (argc >= 3 && strcmp(argv[2], "--textures") == 0)
    {
        const auto& resources = controller->GetResources();
        for (const auto& texture : controller->GetTextures())
        {
            uint64_t id = 0;
            memcpy(&id, &texture.resourceId, sizeof(id));
            const char* name = "";
            for (const auto& resource : resources)
                if (resource.resourceId == texture.resourceId) { name = resource.name.c_str(); break; }
            printf("texture=%llu %ux%u components=%u bytes=%u type=%u samples=%u name=%s\n",
                   (unsigned long long)id, texture.width, texture.height, texture.format.compCount,
                   texture.format.compByteWidth, uint32_t(texture.format.compType), texture.msSamp, name);
        }
    }
    else if (argc >= 4)
    {
        const uint32_t event = uint32_t(strtoul(argv[2], nullptr, 10));
        controller->SetFrameEvent(event, true);
        const auto* action = Find(controller->GetRootActions(), event);
        const auto* pipeline = controller->GetD3D12PipelineState();
        if (pipeline && pipeline->pixelShader.reflection)
            fprintf(stderr, "Pixel shader debuggable=%u: %s\n", pipeline->pixelShader.reflection->debugInfo.debuggable,
                    pipeline->pixelShader.reflection->debugInfo.debugStatus.c_str());
        const bool resourceExport = argc >= 6 && strcmp(argv[4], "--resource") == 0;
        ResourceId output = action ? action->outputs[0] : ResourceId::Null();
        if (output == ResourceId::Null() && pipeline && !pipeline->outputMerger.renderTargets.empty())
            output = pipeline->outputMerger.renderTargets[0].resource;
        if (resourceExport)
        {
            const uint64_t requested = strtoull(argv[5], nullptr, 10);
            output = ResourceId::Null();
            for (const auto& texture : controller->GetTextures())
            {
                uint64_t id;
                memcpy(&id, &texture.resourceId, sizeof(id));
                if (id == requested) { output = texture.resourceId; break; }
            }
        }
        if (!action || (output == ResourceId::Null() && argc < 6)) result = 2;
        else
        {
            TextureSave save;
            save.resourceId = output;
            if (pipeline)
                for (const auto& target : pipeline->outputMerger.renderTargets)
                    if (target.resource == output)
                    {
                        save.typeCast = target.format.compType;
                        fprintf(stderr, "Output view: components=%u bytes=%u type=%u\n", target.format.compCount,
                            target.format.compByteWidth, uint32_t(target.format.compType));
                        break;
                    }
            const char* extension = strrchr(argv[3], '.');
            save.destType = extension && strcmp(extension, ".dds") == 0 ? FileType::DDS : FileType::PNG;
            if (output != ResourceId::Null())
            {
                auto saved = controller->SaveTexture(save, argv[3]);
                if (!saved.OK()) { fprintf(stderr, "SaveTexture failed: %d\n", int(saved.code)); result = 1; }
            }
            else if (resourceExport) { fprintf(stderr, "Texture resource not found\n"); result = 2; }
            if (argc >= 6 && !resourceExport)
            {
                DebugPixelInputs pixelInputs;
                if (argc >= 7) pixelInputs.primitive = uint32_t(strtoul(argv[6], nullptr, 10));
                auto* trace = controller->DebugPixel(uint32_t(atoi(argv[4])), uint32_t(atoi(argv[5])), pixelInputs);
                if (!trace || !trace->debugger) { fprintf(stderr, "Pixel is not debuggable\n"); result = 1; }
                else
                {
                    for (const auto& input : trace->inputs) Variable(input, "input");
                    const auto debugStart = std::chrono::steady_clock::now();
                    uint32_t steps = 0;
                    bool limited = false;
                    for (uint32_t batch = 0; batch < 10000; ++batch)
                    {
                        auto states = controller->ContinueDebug(trace->debugger);
                        if (states.empty()) break;
                        for (const auto& state : states)
                        {
                            if (++steps > 10000 || std::chrono::steady_clock::now() - debugStart > std::chrono::seconds(20))
                            { limited = true; break; }
                            printf("instruction %u\n", state.nextInstruction);
                            for (const auto& change : state.changes) Variable(change.after, "change");
                        }
                        if (limited || batch == 9999 || std::chrono::steady_clock::now() - debugStart > std::chrono::seconds(20))
                        { fprintf(stderr, "Debug step/time limit reached\n"); result = 1; break; }
                    }
                }
                if (trace) controller->FreeTrace(trace);
            }
        }
    }
    else Actions(controller->GetRootActions(), controller->GetStructuredFile());
    controller->Shutdown();
    RENDERDOC_ShutdownReplay();
    return result;
}
