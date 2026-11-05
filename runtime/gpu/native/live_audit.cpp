#include "live_audit.h"

#ifdef FD_NATIVE_RENDERER
#include "shader_catalog.h"
#include "../../kernel/kernel.h"
#include "live_geometry.h"
#include "live_resources.h"
#include "renderer.h"
#include "../xenia_bridge.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <stdexcept>
#include <chrono>
#include <condition_variable>
#include <thread>
#include "replay_frame.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace gpu::native::audit
{
namespace
{
struct Frame
{
    uint64_t calls[3] = {}, unresolved = 0;
    std::set<std::string> shaders;
    std::map<std::pair<uint32_t, uint32_t>, uint64_t> missing;
    uint64_t Draws() const { return calls[0] + calls[1] + calls[2]; }
};
// Off-thread native rendering. At VdSwap the game's render thread hands the
// captured frame (commands, converted streams, texture references) to the
// render worker and goes on with the next frame while this one is rendered,
// submitted and presented. One frame is in flight: the next hand-over waits for
// it, and the vectors come back for reuse. FD_NATIVE_SYNC=1 renders inline.
struct RenderJob
{
    std::vector<GeometryBatch::Command> commands;
    std::vector<uint8_t> bytes;
    std::vector<StreamArena::Entry> entries;
    std::vector<StreamArena::Segment> segments;
    uint32_t segmentEnd = 0;
    std::vector<FrameInput::StreamSegment> views;
    std::vector<std::shared_ptr<TextureResource>> textures;
    uint32_t frontBuffer = 0;
    bool dump = false;
};
class RenderWorker
{
public:
    explicit RenderWorker(std::function<void(RenderJob&)> render) : render(std::move(render))
    {
        sync = getenv("FD_NATIVE_SYNC") != nullptr;
        if (!sync) thread = std::thread([this] { Run(); });
    }
    // Game thread: waits for the previous frame, then takes this frame's data.
    void Submit(GeometryBatch& batch, std::span<const std::shared_ptr<TextureResource>> textures, uint32_t frontBuffer, bool dump)
    {
        std::unique_lock lock(mutex);
        idle.wait(lock, [&] { return !pending && !busy; });
        job.commands.swap(batch.commands);
        job.bytes.swap(batch.streams.bytes);
        job.entries.swap(batch.streams.entries);
        job.segments.swap(batch.streams.segments);
        job.segmentEnd = batch.streams.segmentEnd;
        batch.streams.segmentEnd = 0;
        job.textures.assign(textures.begin(), textures.end());
        job.frontBuffer = frontBuffer;
        job.dump = dump;
        if (sync)
        {
            lock.unlock();
            Execute();
            return;
        }
        pending = true;
        ready.notify_one();
    }
private:
    void Execute()
    {
        try { render(job); }
        catch (const std::exception& error)
        {
            kernel::Log("native frame: %s", error.what());
            ExitProcess(1);
        }
        job.commands.clear();job.bytes.clear();job.entries.clear();job.textures.clear();job.segments.clear();job.views.clear();job.segmentEnd=0;
    }
    void Run()
    {
        kernel::Log("native renderer: rendering off the game thread");
        for (;;)
        {
            {
                std::unique_lock lock(mutex);
                ready.wait(lock, [&] { return pending; });
                pending = false;
                busy = true;
            }
            Execute();
            {
                std::lock_guard lock(mutex);
                busy = false;
            }
            idle.notify_all();
        }
    }
    std::function<void(RenderJob&)> render;
    std::mutex mutex;
    std::condition_variable ready, idle;
    RenderJob job;
    bool pending = false, busy = false, sync = false;
    std::thread thread;
};
struct Context
{
    explicit Context(ShaderCatalog value) : catalog(std::move(value)) {}
    ShaderCatalog catalog;
    ShaderObjects objects;
    std::unique_ptr<LiveGeometryCapture> geometry;
    std::map<uint32_t,PacketState> windows;
    std::string report;
    std::mutex mutex;
    bool started = false, reportFailed = false;
    bool controlledFrame=false;
    bool native=false;
    uint32_t frontBuffer=0;
    std::unique_ptr<Renderer> renderer;
    LiveResources resources;
    std::unique_ptr<RenderWorker> worker;
    uint64_t dumpAt=UINT64_MAX;
    double packMs=0,renderMs=0,presentMs=0;
    uint64_t frames = 0, initializations = 0, matched = 0, invalid = 0;
    Frame pending, last, total;
    std::map<std::pair<uint32_t, uint32_t>, uint64_t> unknownInitializations;
};
std::unique_ptr<Context> context;
std::mutex shaderTraceMutex;
std::map<std::pair<uint32_t,uint32_t>,int> shaderTraceCodes;

bool Readable(uint32_t address, uint32_t size)
{
    // The runtime commits the guest address space up front. Limit inspection
    // to game data and reject address wrap.
    return address >= 0x40000000 && uint64_t(address) + size <= 0xC0000000;
}

bool WriteReport(Context& c)
{
    const std::filesystem::path path(c.report), temporary = path.wstring() + L".new";
    FILE* file = _wfopen(temporary.c_str(), L"wb");
    if (!file) return false;
    const bool covered = c.frames && c.total.Draws() && !c.total.unresolved && !c.reportFailed;
    fprintf(file, "{\n  \"mode\": \"live-shader-audit\",\n  \"render_backend\": \"%s\",\n"
        "  \"native_rendering\": %s,\n  \"catalog_shaders\": %zu,\n"
        "  \"shader_initializations\": %llu,\n  \"matched_initializations\": %llu,\n"
        "  \"invalid_initializations\": %llu,\n  \"completed_frames\": %llu,\n"
        "  \"cpu_draw_calls\": %llu,\n  \"indexed_calls\": %llu,\n"
        "  \"vertices_calls\": %llu,\n  \"begin_vertices_calls\": %llu,\n"
        "  \"unresolved_draw_calls\": %llu,\n  \"last_frame_draw_calls\": %llu,\n"
        "  \"pending_draw_calls\": %llu,\n  \"shader_coverage_passed\": %s,\n",
        c.native?"native":"xenia",c.native&&c.frames?"true":"false",c.catalog.Shaders().size(), c.initializations, c.matched, c.invalid, c.frames,
        c.total.Draws(), c.total.calls[0], c.total.calls[1], c.total.calls[2],
        c.total.unresolved, c.last.Draws(), c.pending.Draws(), covered ? "true" : "false");
    fprintf(file, "  \"used_shaders\": [");
    const char* separator = "";
    for (const auto& name : c.total.shaders)
    {
        fprintf(file, "%s\"%s\"", separator, name.c_str());
        separator = ", ";
    }
    fprintf(file, "],\n  \"missing_draw_shaders\": [");
    separator = "";
    for (const auto& [key, calls] : c.total.missing)
    {
        fprintf(file, "%s{\"stage\": %u, \"object\": \"%08X\", \"calls\": %llu}", separator,
            key.first, key.second, calls);
        separator = ", ";
    }
    fprintf(file, "],\n  \"unknown_initializations\": [");
    separator = "";
    for (const auto& [key, calls] : c.unknownInitializations)
    {
        fprintf(file, "%s{\"stage\": %u, \"object\": \"%08X\", \"calls\": %llu}", separator,
            key.first, key.second, calls);
        separator = ", ";
    }
    fprintf(file, "]\n}\n");
    const bool okay = ferror(file) == 0;
    const int closed = fclose(file);
    return okay && closed == 0 && MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

void Flush(Context& c)
{
    if (!WriteReport(c))
    {
        c.reportFailed = true;
        kernel::Log("native shader audit: cannot write report %s", c.report.c_str());
    }
}
}

bool Initialize()
{
    const char* path = getenv("FD_NATIVE_SHADER_CATALOG");
    const char* backend=getenv("FD_RENDERER");
    const bool native=backend&&!strcmp(backend,"native");
    const char* geometryDelay = getenv("FD_NATIVE_GEOMETRY_CAPTURE");
    const char* nativeFrame=getenv("FD_NATIVE_FRAME_CAPTURE");
    const bool controlledFrame=nativeFrame&&atoi(nativeFrame)!=0;
    if (!path || !*path)
    {
        if ((geometryDelay && *geometryDelay)||controlledFrame||native) { kernel::Log("native geometry requires FD_NATIVE_SHADER_CATALOG"); return false; }
        return true;
    }
    try
    {
        auto created = std::make_unique<Context>(ShaderCatalog::Load(path));
        const char* report = getenv("FD_NATIVE_AUDIT_REPORT");
        created->report = report && *report ? report :
            (std::filesystem::path(path).parent_path() / "live-shader-audit.json").string();
        if (!WriteReport(*created)) throw std::runtime_error("Cannot create native shader audit report");
        created->controlledFrame=controlledFrame;
        created->native=native;created->started=native;
        if(const char* delay=getenv("FD_NATIVE_DUMP_AT");native&&delay)created->dumpAt=GetTickCount64()+uint64_t(atoi(delay))*1000;
        if ((geometryDelay && *geometryDelay)||controlledFrame||native)
        {
            const int seconds = controlledFrame||native?1800:atoi(geometryDelay);
            const char* minimum = getenv("FD_NATIVE_GEOMETRY_MIN_INDEXED");
            const int indexed = minimum ? atoi(minimum) : 500;
            const char* patchLimit=getenv("FD_NATIVE_GEOMETRY_MIN_PATCHED");
            const int minimumPatched=patchLimit?atoi(patchLimit):0;
            if (seconds <= 0 || seconds > 1800 || indexed < 0||minimumPatched<0||minimumPatched>10000) throw std::runtime_error("Invalid native geometry capture limits");
            const char* destination = getenv("FD_NATIVE_GEOMETRY_REPORT");
            const std::string reportPath = destination && *destination ? destination :
                (std::filesystem::path(path).parent_path() / "live-geometry.json").string();
            if(controlledFrame||native)
                for(const auto& shader:created->catalog.Shaders())
                    if(shader.stage==ShaderStage::Vertex&&(shader.source.find("g_VertexBaseOffset")==std::string::npos||shader.source.find("fdVertexIndex")==std::string::npos))
                        throw std::runtime_error("Native live frame requires rebuilt vertex addressing shaders");
            created->geometry = std::make_unique<LiveGeometryCapture>(unsigned(seconds), unsigned(indexed), reportPath,unsigned(minimumPatched),controlledFrame||native);
            if(native)
            {
                created->renderer=std::make_unique<Renderer>();
                if(!created->renderer->LoadShaderCatalog(path))throw std::runtime_error("Cannot load the native renderer shader catalog");
                auto* c=created.get();
                created->geometry->SetContinuous([c](gpu::native::Draw& draw){
                    if(draw.resolve)c->resources.Resolve(draw.textureFetches[0]);
                    else if(!draw.clear)c->resources.Bind(draw,c->catalog);
                },[c](GeometryBatch& batch){
                    if(batch.commands.empty()){c->resources.BeginFrame();return;}
                    static const bool profile=getenv("FD_NATIVE_PROFILE")!=nullptr;
                    static uint64_t profileFrames=0;
                    if(profile&&++profileFrames%300==0)kernel::Log("native texture profile: %llu new keys, %llu captures, %.0f KB, %.2f ms this frame",
                        (unsigned long long)c->resources.profileInserted,(unsigned long long)c->resources.profileCaptured,c->resources.profileCaptureBytes/1024.0,c->resources.profileCaptureMs);
                    if(getenv("FD_TRACE_FRAME_COUNTS")&&batch.commands.size()>500&&c->frames%60==0)
                    {
                        unsigned mesh=0,terrain=0,depth=0;
                        for(const auto& command:batch.commands){mesh+=command.pixelShader=="086";terrain+=command.pixelShader=="146";depth+=command.vertexShader=="067";}
                        kernel::Log("native CPU frame %llu: %zu records, mesh %u terrain %u mesh-depth %u",c->frames+1,batch.commands.size(),mesh,terrain,depth);
                    }
                    const bool dump=GetTickCount64()>=c->dumpAt;
                    if(dump)c->dumpAt=UINT64_MAX;
                    c->worker->Submit(batch,c->resources.FrameResources(),c->frontBuffer,dump);
                    c->resources.BeginFrame();
                });
                created->worker=std::make_unique<RenderWorker>([c](RenderJob& job){
                    if(!c->renderer->CompletedFrames()&&!c->renderer->UseDevice(xenia::NativeDevice()))throw std::runtime_error("Native UI device unavailable");
                    const auto started=std::chrono::steady_clock::now();
                    std::vector<gpu::native::Draw> draws;draws.reserve(job.commands.size());
                    for(const auto& command:job.commands){gpu::native::Draw draw;command.BindGeometryAndConstants(draw);draws.push_back(std::move(draw));}
                    FrameInput input;input.draws=draws;input.resources=job.textures;
                    input.streams=job.bytes;input.continuous=true;input.frontBufferBase=job.frontBuffer;
                    job.views.reserve(job.segments.size());
                    for(const auto& segment:job.segments)job.views.push_back({segment.offset,uint32_t(segment.data->size()),segment.data->data()});
                    input.streamSegments=job.views;input.streamSegmentBytes=job.segmentEnd;
                    std::string diagnostic;
                    std::vector<uint8_t> flat;
                    if(job.dump)
                    {
                        // Replays keep one contiguous stream file.
                        flat=job.bytes;flat.resize((std::max)(size_t(job.segmentEnd),flat.size()));
                        for(const auto& view:job.views)memcpy(flat.data()+view.offset,view.data,view.size);
                        FrameInput saved=input;saved.streams=flat;saved.streamSegments={};saved.streamSegmentBytes=0;
                        diagnostic="native-session-frame";ReplayFrame::Save(saved,diagnostic);
                    }
                    const auto packed=std::chrono::steady_clock::now();
                    if(c->renderer->RenderFrame(input,diagnostic))throw std::runtime_error("Native frame execution failed");
                    const auto rendered=std::chrono::steady_clock::now();
                    if(!xenia::PresentNative(*c->renderer))throw std::runtime_error("Native presentation failed");
                    const auto presented=std::chrono::steady_clock::now();
                    c->packMs+=std::chrono::duration<double,std::milli>(packed-started).count();
                    c->renderMs+=std::chrono::duration<double,std::milli>(rendered-packed).count();
                    c->presentMs+=std::chrono::duration<double,std::milli>(presented-rendered).count();
                    if(c->renderer->CompletedFrames()%60==0){kernel::Log("native frame costs: pack %.2f ms, render %.2f ms, present/pace %.2f ms",c->packMs/60,c->renderMs/60,c->presentMs/60);c->packMs=c->renderMs=c->presentMs=0;}
                });
            }
        }
        kernel::Log("native shader registry: %zu compiled shaders loaded; backend %s", created->catalog.Shaders().size(),native?"native":"Xenia");
        context = std::move(created);
        return true;
    }
    catch (const std::exception& error)
    {
        kernel::Log("native shader audit: %s", error.what());
        return false;
    }
}

void ShaderInit(uint32_t stage, uint32_t object, uint32_t physical)
{
    if (!context) return;
    auto& c = *context;
    std::lock_guard lock(c.mutex);
    ++c.initializations;
    const uint32_t virtualOffset = stage == 0 ? 0x368 : 0x28;
    const uint64_t virtualAddress = uint64_t(object) + virtualOffset;
    std::span<const uint8_t> virtualData, physicalData;
    bool valid = stage <= 1 && virtualAddress <= UINT32_MAX && Readable(uint32_t(virtualAddress), 36);
    if (valid)
    {
        const uint32_t address = uint32_t(virtualAddress), flags = mem::Read32(address);
        const uint32_t virtualSize = mem::Read32(address + 4), physicalSize = mem::Read32(address + 8);
        valid = (flags & 0xFFFFFFFE) == 0x102A1100 && ((flags & 1) ? 0u : 1u) == stage &&
            virtualSize >= 36 && virtualSize <= 1024 * 1024 && physicalSize && physicalSize <= 1024 * 1024 &&
            Readable(address, virtualSize) && Readable(physical, physicalSize);
        if (valid)
        {
            virtualData = {mem::Ptr<uint8_t>(address), virtualSize};
            physicalData = {mem::Ptr<uint8_t>(physical), physicalSize};
        }
    }
    const auto* shader = c.objects.Register(object, ShaderStage(stage), virtualData, physicalData, c.catalog);
    static const bool traceCounts=getenv("FD_TRACE_FRAME_COUNTS")!=nullptr;
    if(shader&&traceCounts)
    {
        auto big=[&](size_t at){if(at+4>shader->virtualData.size())throw std::runtime_error("Invalid shader trace metadata");const auto* p=shader->virtualData.data()+at;return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];};
        const auto codeOffset=big(big(24));
        std::lock_guard traceLock(shaderTraceMutex);
        shaderTraceCodes[{stage,(physical+codeOffset)&0x1FFFFFFF}]=atoi(shader->name.c_str());
    }
    if (shader) ++c.matched;
    else
    {
        if (!valid) ++c.invalid;
        auto& attempts = c.unknownInitializations[{stage, object}];
        if (++attempts == 1)
            kernel::Log("native shader audit: unknown %s object %08X (metadata %s)", stage ? "PS" : "VS", object, valid ? "valid" : "invalid");
    }
}

int TraceShaderCode(uint32_t stage,uint32_t address)
{
    std::lock_guard lock(shaderTraceMutex);
    const auto found=shaderTraceCodes.find({stage,address&0x1FFFFFFF});
    return found==shaderTraceCodes.end()?-1:found->second;
}

void Draw(uint32_t device, DrawKind kind, uint32_t indexOffset,uint32_t primitive,uint32_t count)
{
    if (!context) return;
    auto& c = *context;
    std::lock_guard lock(c.mutex);
    if (!c.started) return; // Exclude the partial frame before the first swap.
    if (c.geometry)
    {
        const uint32_t object = Readable(device, 0x3294) ? mem::Read32(device + 0x3290) : 0;
        const uint32_t pixel = Readable(device, 0x3294) ? mem::Read32(device + 0x328C) : 0;
        auto packet=c.windows[device]; packet.indexOffset=indexOffset;
        c.geometry->Begin(device, kind, c.objects.Find(object, ShaderStage::Vertex), c.objects.Find(pixel, ShaderStage::Pixel),packet,primitive,count);
    }
    ++c.pending.calls[uint32_t(kind)];
    bool unresolved = false;
    for (uint32_t stage = 0; stage < 2; ++stage)
    {
        const uint32_t object = Readable(device, 0x3294) ? mem::Read32(device + (stage ? 0x328C : 0x3290)) : 0;
        if (stage == 1 && !object) continue; // Depth-only draw.
        const auto* shader = c.objects.Find(object, ShaderStage(stage));
        if (shader) c.pending.shaders.insert(shader->name);
        else
        {
            unresolved = true;
            ++c.pending.missing[{stage, object}];
        }
    }
    if (unresolved) ++c.pending.unresolved;
}

void Swap(uint32_t frontBuffer)
{
    if (!context) return;
    auto& c = *context;
    std::lock_guard lock(c.mutex);
    if(!c.native&&frontBuffer)return;
    if(c.native&&!frontBuffer)return; // Finalize at VdSwap once its front buffer is known.
    c.frontBuffer=frontBuffer;
    try {if (c.geometry) c.geometry->Swap(c.pending.calls[0]);}
    catch(const std::exception& error){kernel::Log("native frame: %s",error.what());c.geometry->Finish();ExitProcess(1);}
    if (c.started)
    {
        ++c.frames;
        for (uint32_t i = 0; i < 3; ++i) c.total.calls[i] += c.pending.calls[i];
        c.total.unresolved += c.pending.unresolved;
        c.total.shaders.insert(c.pending.shaders.begin(), c.pending.shaders.end());
        for (const auto& [key, calls] : c.pending.missing) c.total.missing[key] += calls;
        c.last = std::move(c.pending);
    }
    c.pending = {};
    c.started = true;
    if (c.frames % 60 == 0) Flush(c);
}

bool Finish()
{
    if (!context) return true;
    auto& c = *context;
    std::lock_guard lock(c.mutex);
    Flush(c);
    const bool geometryPassed = c.native ? c.renderer->CompletedFrames()!=0 : !c.geometry || c.geometry->Finish();
    const bool passed = c.frames && c.total.Draws() && !c.total.unresolved && !c.reportFailed && geometryPassed;
    kernel::Log("native shader audit: %llu complete frames, %llu CPU draw calls, %llu unresolved, %zu shaders used (%s)",
        c.frames, c.total.Draws(), c.total.unresolved, c.total.shaders.size(), passed ? "pass" : "FAIL");
    return passed;
}

void DrawEnd(uint32_t device, DrawKind kind)
{
    if (!context || !context->geometry) return;
    std::lock_guard lock(context->mutex);
    context->geometry->End(device, kind);
}
void BeforeOtherCall()
{
    if (!context || !context->geometry) return;
    std::lock_guard lock(context->mutex);
    context->geometry->FlushInline();
}
void CommandBlock(uint32_t device,bool begin)
{
    if(!context||!context->geometry)return;
    std::lock_guard lock(context->mutex);context->geometry->CommandBlock(device,begin);
}
void Scissor(uint32_t device,uint32_t left,uint32_t top,uint32_t right,uint32_t bottom)
{
    if(!context || !context->geometry)return;
    std::lock_guard lock(context->mutex);
    context->geometry->FlushInline();
    context->windows[device]={(left&0x3FFF)|((top&0x3FFF)<<16),(right&0x3FFF)|((bottom&0x3FFF)<<16),0,true};
}
const GeometryBatch* CapturedGeometry()
{
    if (!context || !context->geometry) return nullptr;
    std::lock_guard lock(context->mutex);
    return context->geometry->Captured();
}
const ShaderCatalog* CapturedCatalog(){return context?&context->catalog:nullptr;}
void ArmFrame(uint64_t serial)
{
    if(!context||!context->controlledFrame)return;
    std::lock_guard lock(context->mutex);
    context->geometry->ArmFrame(serial);
}
Status GetStatus() {
    if(!context)return {};
    std::unique_lock lock(context->mutex,std::try_to_lock);
    Status result;result.enabled=true;
    if(!lock.owns_lock()){result.busy=true;return result;}
    const auto& c=*context;
    if(c.geometry)result=c.geometry->Status();
    result.enabled=true;result.catalogShaders=c.catalog.Shaders().size();result.shaderInitializations=c.initializations;
    result.matchedShaders=c.matched;result.frames=c.frames;result.drawCalls=c.total.Draws();result.unresolvedDraws=c.total.unresolved;
    return result;
}
void Clear(uint32_t device,uint32_t flags,uint32_t color,float depth,uint32_t stencil,uint32_t count,uint32_t rectangles) {
    if(!context||!context->geometry)return;
    std::lock_guard lock(context->mutex);context->geometry->Clear(device,flags,color,depth,stencil,count,rectangles);
}
void ResolveBegin(uint32_t device,uint32_t flags,uint32_t texture) {
    if(!context||!context->geometry)return;
    std::lock_guard lock(context->mutex);context->geometry->ResolveBegin(device,flags,texture);
}
void ResolveEnd(uint32_t device) {
    if(!context||!context->geometry)return;
    std::lock_guard lock(context->mutex);context->geometry->ResolveEnd(device);
}
}
#endif
