#include <atomic>
#ifdef FD_NATIVE_RENDERER
#include <chrono>
#include "../../kernel/kernel.h"
#endif
#include "live_geometry.h"
#ifdef FD_NATIVE_RENDERER
#include "operations.h"
#include "command_stream.h"
#include "../xenia_bridge.h"
#include "../../pointer.h"
#include <bit>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace gpu::native
{
namespace
{
bool Readable(uint32_t address, uint32_t size)
{
    return address >= 0x40000000 && uint64_t(address) + size <= 0xC0000000;
}
struct Pending
{
    uint32_t device, before;
    audit::DrawKind kind;
    const NativeShader* shader = nullptr;
    std::string pixelShader;
    std::array<uint32_t,1024> vertexConstants, pixelConstants;
    std::array<uint32_t,192> fetches;
    std::optional<Draw> state;
    std::string stateError;
    std::vector<uint8_t> physical;
    std::vector<uint8_t> physicalBefore;
    uint32_t physicalAddress=0;
    bool completedShader=false;
    uint32_t primitive=0,count=UINT32_MAX;
    std::vector<uint8_t> commands;
};
struct PendingResolve { uint32_t device,cursor,flags;Draw source;std::array<uint32_t,6> fetch;std::vector<uint8_t> commands; };
void AppendCommands(std::vector<uint8_t>& bytes,uint32_t before,uint32_t after)
{
    if(after==before)return;
    if(after<before||after-before>0x80000||!Readable(before+4,after-before))
        throw std::runtime_error("Discontinuous native command block "+std::to_string(before)+" -> "+std::to_string(after));
    const auto* first=mem::Ptr<uint8_t>(before+4);
    bytes.insert(bytes.end(),first,first+after-before);
}
Draw SurfaceState(uint32_t device) {
    if(!Readable(device,0x3294))throw std::runtime_error("Unreadable native operation device");
    return SnapshotSurfaceState({mem::Ptr<uint8_t>(device),0x3294},[](uint32_t address,uint32_t size)->std::span<const uint8_t>{
        if(!Readable(address,size))return {};return {mem::Ptr<uint8_t>(address),size};
    });
}
}
extern std::atomic<uint64_t> g_streamHitBytes,g_streamMissBytes,g_indexHitBytes,g_indexMissBytes;
struct LiveGeometryCapture::Impl
{
    enum class State { Waiting, Capturing, Complete } state = State::Waiting;
    uint64_t armedAt, expectedIndexed = 0, calls = 0, failures = 0, vertexBytes = 0, indexBytes = 0;
    uint64_t geometryPackets=0,clearPackets=0,resolvePackets=0;
    uint64_t patchedShaderSnapshots=0;
    unsigned minimumIndexed,minimumPatched=0;
    uint64_t skippedUnpatchedFrames=0;
    uint32_t thread = 0;
    std::string report;
    std::vector<std::string> errors;
    // Heap-held: a pending draw carries ~25 KB of constants and device state,
    // and moving it by value copied all of that three times per draw.
    std::vector<std::unique_ptr<Pending>> stack;
    std::unique_ptr<Pending> inlineDraw;
    // Finished pending draws, reused (allocating and zeroing one cost more
    // than the draw's own bookkeeping).
    std::vector<std::unique_ptr<Pending>> freePending;
    std::unique_ptr<Pending> TakePending()
    {
        if (freePending.empty()) return std::make_unique<Pending>();
        auto pending = std::move(freePending.back()); freePending.pop_back();
        pending->shader = nullptr; pending->pixelShader.clear(); pending->state.reset(); pending->stateError.clear();
        pending->physical.clear(); pending->physicalBefore.clear(); pending->physicalAddress = 0;
        pending->completedShader = false; pending->primitive = 0; pending->count = UINT32_MAX; pending->commands.clear();
        return pending;
    }
    void Recycle(std::unique_ptr<Pending> pending) { if (pending && freePending.size() < 16) freePending.push_back(std::move(pending)); }
    std::optional<PendingResolve> resolve;
    GeometryBatch batch;
    std::map<std::string, VertexProgram> programs;
    std::map<const NativeShader*, bool> cameraShaders;
    bool frameCamera = false;
    struct VertexLoad{const NativeShader* shader=nullptr;bool immediate=false;uint32_t start=0;std::vector<uint8_t> code;};
    std::map<uint32_t,VertexLoad> vertexLoads; // per device: the vertex microcode the GPU last loaded
    bool reportFailed = false;
    bool pullPrimary=false;
    std::function<void(Draw&)> onCommand;
    std::function<void(GeometryBatch&)> onFrame;
    std::map<std::pair<uint32_t,uint32_t>,uint64_t> watchedStreams;
    // FD_NATIVE_PROFILE: CPU cost of the live collection, per frame.
    bool profile=getenv("FD_NATIVE_PROFILE")!=nullptr;
    double profileMs[4]{}; // begin, build geometry, commit, process other
    uint64_t profileIndexBytes=0,profileStreamBytes=0,profileFrames=0;
    std::shared_ptr<StreamArena::Persistent> persistentStreams=std::make_shared<StreamArena::Persistent>();
    void TrackStreams()
    {
        batch.streams.persistent=persistentStreams;
        persistentStreams->NextFrame();
        batch.streams.revision=[this](uint32_t address,uint32_t size) {
            auto [at,inserted]=watchedStreams.try_emplace({address,size},UINT64_MAX);
            auto revision=xenia::PhysicalRevision(address,size);
            if(inserted||at->second!=revision)
            {
                xenia::WatchPhysical(address,size);
                revision=xenia::PhysicalRevision(address,size);at->second=revision;
            }
            return revision;
        };
    }
    void Commit(GeometryBatch::Command&& command)
    {
        batch.commands.push_back(std::move(command));
        Notify(batch.commands.back());
    }
    // Draw commands are built in place (a command is ~15 KB: constants and
    // device state; moving it copies all of it).
    void Notify(GeometryBatch::Command& command)
    {
        if(onCommand)
        {
            if(!command.state.clear&&!command.state.resolve){command.state.vs=command.vertexShader;command.state.ps=command.pixelShader;}
            onCommand(command.state);
        }
    }

    void Error(const std::string& message)
    {
        ++failures;
        if (errors.size() < 8) errors.push_back(message);
    }
    bool EnterThread()
    {
        const auto current=GetCurrentThreadId();
        // Device initialization hands rendering to the engine's worker. In
        // continuous mode serialized operations may change thread at a call
        // boundary, while an unfinished operation must retain its owner.
        if(!thread||(onFrame&&stack.empty()&&!resolve&&!inlineDraw))thread=current;
        return thread==current;
    }
    void Process(Pending& pending)
    {
        const auto processStart=profile?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};
        const double nested=profileMs[1]+profileMs[2];
        struct ProcessTimer{Impl& c;std::chrono::steady_clock::time_point start;double nested;
            ~ProcessTimer(){if(c.profile)c.profileMs[3]+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()-(c.profileMs[1]+c.profileMs[2]-nested);}
        } processTimer{*this,processStart,nested};
        ++calls;
        try
        {
            if (!pending.shader) throw std::runtime_error("Unknown live vertex shader");
            if (!pending.state) throw std::runtime_error(pending.stateError);
            // Scene camera for mouse pointing: the first full-screen terrain draw.
            if (!frameCamera && pending.shader)
            {
                auto [known, inserted] = cameraShaders.try_emplace(pending.shader, false);
                if (inserted)
                {
                    const auto& text = pending.shader->source;
                    known->second = text.find("VS_DustConst(8)") != std::string::npos && text.find("VS_DustConst(11)") != std::string::npos;
                }
                const float width = 2 * std::fabs(std::bit_cast<float>(pending.state->regs[VPORT_XSCALE]));
                if (known->second && width >= 1000)
                {
                    float rows[16];
                    for (int i = 0; i < 16; ++i) rows[i] = std::bit_cast<float>(pending.vertexConstants[28 * 4 + i]);
                    pointer::SetCamera(rows, width, 2 * std::fabs(std::bit_cast<float>(pending.state->regs[VPORT_YSCALE])));
                    frameCamera = true;
                }
            }
            AppendCommands(pending.commands,pending.before,mem::Read32(pending.device+0x30));
            const uint32_t after=uint32_t(pending.commands.size());
            auto word=[&](uint32_t at){return CommandWord(pending.commands,at);};
            // BeginVertices writes fetch 95 directly into its committed region.
            // The saved shader/fetch snapshot belongs to Begin, not the next call.
            if (pending.kind == audit::DrawKind::BeginVertices)
                for (uint32_t at = 0; at + 12 <= after; at += 4)
                {
                    const uint32_t header = word(at);
                    if (header == 0x000548BA && at + 28 <= after)
                    {
                        pending.fetches[190] = word(at + 20);
                        pending.fetches[191] = word(at + 24);
                    }
                    else if (header == 0x000148BE)
                    {
                        pending.fetches[190] = word(at + 4);
                        pending.fetches[191] = word(at + 8);
                    }
                }
            struct Packet {uint32_t initiator,address,indexOffset;};
            std::vector<Packet> packets;
            uint32_t indexOffset=pending.state->regs[VGT_INDX_OFFSET];
            // Decode packet boundaries, retaining every chunk of this engine
            // call. The XDK splits index lists larger than 65,535 indices.
            for(const auto& packet:CommandPackets(pending.commands))
            {
                const uint32_t header=packet.header,type=header>>30,opcode=header>>8&127,count=uint32_t(packet.payload.size()/4);
                auto valueAt=[&](uint32_t index){return CommandWord(packet.payload,index*4);};
                // Vertex shader loads: the GPU keeps the microcode it loaded.
                // When one shader is used with several vertex declarations in
                // a frame, the XDK patches the object's microcode in place
                // and sends later patches inline (IM_LOAD_IMMEDIATE), so the
                // object's memory no longer holds the program the draw runs.
                if(type==3&&(opcode==0x27||opcode==0x2B)&&count>=2&&(valueAt(0)&3)==0)
                {
                    auto& load=vertexLoads[pending.device];
                    load.shader=pending.shader;load.code.clear();
                    const uint32_t dwords=valueAt(1)&0xFFFF;
                    load.immediate=opcode==0x2B;load.start=valueAt(1)>>16;
                    if(load.immediate&&count>=2+dwords)load.code.assign(packet.payload.begin()+8,packet.payload.begin()+8+size_t(dwords)*4);
                    else if(load.immediate)load.shader=nullptr;
                }
                if(type==0&&header)
                    for(uint32_t i=0;i<count;++i)if(((header&0x7FFF)+(header&0x8000?0:i))==0x2102)indexOffset=valueAt(i);
                if(type!=3||(opcode!=0x22&&opcode!=0x36)||count>8)continue;
                const uint32_t offset = opcode == 0x22 ? 1 : 0;
                if (count <= offset)continue;
                const uint32_t value = valueAt(offset), source = value >> 6 & 3;
                if((value>>16)&&((value&63)==4||(value&63)==8)&&(source==0||source==2)&&
                    (source!=0||count>=offset+3)&&(pending.kind!=audit::DrawKind::Indexed||source==0)&&
                    (!pending.primitive||(value&63)==(pending.primitive&63)))
                    packets.push_back({value,source?0:valueAt(offset+1),indexOffset});
            }
            if(pending.count==0)return;
            if(packets.empty())throw std::runtime_error("No matching completed engine draw packet");
            size_t first=packets.size()-1;
            uint64_t total=packets[first].initiator>>16;
            if(pending.count!=UINT32_MAX)
            {
                while(total<pending.count&&first){--first;total+=packets[first].initiator>>16;}
                if(total!=pending.count)throw std::runtime_error("Native engine draw chunks do not cover the requested count");
            }
            auto program = programs.find(pending.shader->name);
            if (program == programs.end()) program = programs.emplace(pending.shader->name, VertexProgram::FromShader(*pending.shader)).first;
            if(!pending.completedShader||pending.physical.empty())
                throw std::runtime_error("Missing completed native vertex shader");
            if(auto load=vertexLoads.find(pending.device);load!=vertexLoads.end()&&load->second.shader==pending.shader&&load->second.immediate)
            {
                const size_t at=size_t(program->second.codeOffset)+size_t(load->second.start)*4;
                if(at+load->second.code.size()>pending.physical.size())throw std::runtime_error("Inline vertex shader exceeds its object");
                std::copy(load->second.code.begin(),load->second.code.end(),pending.physical.begin()+at);
                static const bool trace=getenv("FD_TRACE_IMLOAD")!=nullptr;
                if(trace)kernel::Log("native geometry: vs %s draw %llu uses its inline microcode (%zu bytes at %zu)",pending.shader->name.c_str(),(unsigned long long)calls,load->second.code.size(),at);
            }
            GeometryReader reader = [](uint32_t address, uint32_t size) -> std::span<const uint8_t> {
                if (!size || size > 32u << 20 || uint64_t(address) + size > 0x20000000) return {};
                return {mem::Ptr<uint8_t>(mem::kPhysicalBase | address), size};
            };
            for(;first<packets.size();++first)
            {
            const auto [initiator,indexAddress,baseVertex]=packets[first];
            const GeometryBounds bounds{int32_t(baseVertex),pullPrimary};
            const auto buildStart=profile?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};
            const size_t streamBefore=batch.streams.Size();
            auto geometry = BuildGeometry(program->second, pending.physical, pending.fetches,
                initiator & 63, initiator >> 16, ((initiator >> 6) & 3) == 0, (initiator & 0x800) != 0,
                indexAddress, reader, batch.streams,&bounds);
            if(profile)
            {
                profileMs[1]+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-buildStart).count();
                profileIndexBytes+=geometry.IndexBytes().size();profileStreamBytes+=batch.streams.Size()-streamBefore;
            }
            vertexBytes += geometry.vertices.size(); indexBytes += geometry.IndexBytes().size();
            const auto commitStart=profile?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};
            auto& command = batch.commands.emplace_back(pending.shader->name, pending.pixelShader,
                pending.vertexConstants, pending.pixelConstants, *pending.state, std::move(geometry));
            command.state.regs[VGT_INDX_OFFSET]=baseVertex;
            Notify(command);
            if(profile)profileMs[2]+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-commitStart).count();
            ++geometryPackets;
            }
        }
        catch (const std::exception& error) {
            const std::string shader=pending.shader?pending.shader->name:"unknown";
            if(failures<8&&Readable(pending.device,0x4000))
            {
                const auto path=std::filesystem::path(report).wstring()+L".device-failure-"+std::to_wstring(failures)+L".bin";
                if(FILE* file=_wfopen(path.c_str(),L"wb")){fwrite(mem::Ptr<uint8_t>(pending.device),1,0x4000,file);fclose(file);}
            }
            if(failures<8&&!pending.commands.empty())
            {
                const auto path=std::filesystem::path(report).wstring()+L".draw-failure-"+std::to_wstring(failures)+L".bin";
                if(FILE* file=_wfopen(path.c_str(),L"wb")){fwrite(pending.commands.data(),1,pending.commands.size(),file);fclose(file);}
            }
            if(failures<8&&!pending.physical.empty()) {
                const auto stem=std::filesystem::path(report).wstring()+L".failure-"+std::to_wstring(failures)+L"-"+std::wstring(shader.begin(),shader.end());
                auto save=[&](const wchar_t* suffix,const uint8_t* bytes,size_t size){
                    if(FILE* file=_wfopen((stem+suffix).c_str(),L"wb")){fwrite(bytes,1,size,file);fclose(file);}
                };
                const auto& before=pending.physicalBefore.empty()?pending.physical:pending.physicalBefore;
                save(L"-before.bin",before.data(),before.size());
                if(pending.completedShader)save(L"-after.bin",pending.physical.data(),pending.physical.size());
            }
            Error("VS "+shader+": "+error.what());
        }
    }
    bool Write()
    {
        const std::filesystem::path path(report), temporary = path.wstring() + L".new";
        FILE* file = _wfopen(temporary.c_str(), L"wb");
        if (!file) return false;
        fprintf(file, "{\n  \"mode\": \"live-native-geometry\",\n  \"scope\": \"engine-call-final-draw\",\n"
            "  \"native_rendering\": false,\n  \"complete\": %s,\n  \"passed\": %s,\n"
            "  \"previous_frame_indexed_calls\": %llu,\n  \"captured_calls\": %llu,\n"
            "  \"geometry_packets\": %zu,\n  \"failures\": %llu,\n  \"vertex_bytes\": %llu,\n"
            "  \"index_bytes\": %llu,\n  \"pulled_stream_bytes\": %zu,\n  \"constant_bytes\": %zu,\n"
            "  \"state_packets\": %zu,\n  \"clear_packets\": %llu,\n  \"resolve_packets\": %llu,\n  \"ordered_commands\": %zu,\n  \"patched_shader_snapshots\": %llu,\n  \"skipped_unpatched_frames\": %llu,\n"
            "  \"state_scope\": \"device-shadow-and-logical-scissor\",\n  \"errors\": [",
            state == State::Complete ? "true" : "false",
            state == State::Complete && !failures && calls && !reportFailed ? "true" : "false",
            expectedIndexed, calls, size_t(geometryPackets), failures, vertexBytes, indexBytes, batch.streams.bytes.size(), size_t(geometryPackets) * 8192,
            batch.commands.size(),clearPackets,resolvePackets,batch.commands.size(),patchedShaderSnapshots,skippedUnpatchedFrames);
        const char* separator = "";
        for (const auto& error : errors) { fprintf(file, "%s\"%s\"", separator, error.c_str()); separator = ", "; }
        fprintf(file, "]\n}\n");
        const bool written = ferror(file) == 0;
        const int closed = fclose(file);
        return written && !closed && MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }
};

LiveGeometryCapture::LiveGeometryCapture(unsigned seconds, unsigned minimumIndexed, std::string report,unsigned minimumPatched,bool pullPrimary) : impl(std::make_unique<Impl>())
{
    impl->armedAt = GetTickCount64() + uint64_t(seconds) * 1000;
    impl->minimumIndexed = minimumIndexed;
    impl->minimumPatched = minimumPatched;
    impl->pullPrimary=pullPrimary;
    impl->report = std::move(report);
    if (!impl->Write()) throw std::runtime_error("Cannot create native geometry report");
}
LiveGeometryCapture::~LiveGeometryCapture() = default;
void LiveGeometryCapture::Begin(uint32_t device, audit::DrawKind kind, const NativeShader* shader, const NativeShader* pixelShader, const PacketState& packet,uint32_t primitive,uint32_t count)
{
    FlushInline();
    auto& c = *impl;
    if (c.state != Impl::State::Capturing) return;
    if (!c.EnterThread()) { c.Error("Geometry calls crossed unfinished render operations"); return; }
    const auto beginStart=c.profile?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};
    struct BeginTimer{Impl& c;std::chrono::steady_clock::time_point start;
        ~BeginTimer(){if(c.profile)c.profileMs[0]+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();}
    } beginTimer{c,beginStart};
    auto owned = c.TakePending(); Pending& pending = *owned;
    pending.device = device; pending.kind = kind; pending.shader = shader;
    pending.primitive=primitive;pending.count=count;
    if (pixelShader) pending.pixelShader = pixelShader->name;
    if (Readable(device, 0x3294))
    {
        try { pending.state = SnapshotDeviceState({mem::Ptr<uint8_t>(device),0x3294},packet); }
        catch(const std::exception& error) { pending.stateError=error.what(); }
        pending.before = mem::Read32(device + 0x30);
        // Big-endian guest words, swapped in bulk (vectorized).
        auto readWords = [](uint32_t* out, const uint8_t* in, size_t count) {
            memcpy(out, in, count * 4);
            for (size_t i = 0; i < count; ++i) out[i] = _byteswap_ulong(out[i]);
        };
        readWords(pending.vertexConstants.data(), mem::Ptr<uint8_t>(device + 0x780), 1024);
        readWords(pending.pixelConstants.data(), mem::Ptr<uint8_t>(device + 0x1780), 1024);
        readWords(pending.fetches.data(), mem::Ptr<uint8_t>(device + 0x480), 192);
        const uint32_t object = mem::Read32(device + 0x3290);
        if (shader && Readable(object, 0x24))
        {
            const uint32_t physical = mem::Read32(object + 0x20), size = uint32_t(shader->physicalData.size());
            if (Readable(physical, size)) {pending.physicalAddress=physical;pending.physical.assign(mem::Ptr<uint8_t>(physical), mem::Ptr<uint8_t>(physical) + size);}
        }
    }
    else pending.stateError="Unreadable native device state";
    c.stack.push_back(std::move(owned));
}
void LiveGeometryCapture::End(uint32_t device, audit::DrawKind kind)
{
    auto& c = *impl;
    if (c.state != Impl::State::Capturing || c.thread != GetCurrentThreadId()) return;
    if (c.stack.empty() || c.stack.back()->device != device || c.stack.back()->kind != kind) { c.Error("Unbalanced native geometry hooks"); return; }
    auto owned = std::move(c.stack.back()); c.stack.pop_back();
    Pending& pending = *owned;
    // The XDK patches vertex declarations in the draw's flush. Snapshot at
    // End even for BeginVertices: inline geometry is processed later, after
    // the caller fills its reserved bytes, and the shader may be rebound.
    if(!pending.physical.empty()&&Readable(pending.physicalAddress,uint32_t(pending.physical.size()))) {
        const std::span<const uint8_t> completed{mem::Ptr<uint8_t>(pending.physicalAddress),pending.physical.size()};
        if(!std::equal(pending.physical.begin(),pending.physical.end(),completed.begin()))++c.patchedShaderSnapshots;
        pending.physicalBefore=std::move(pending.physical);
        pending.physical.assign(completed.begin(),completed.end());
        pending.completedShader=true;
    }
    if (kind == audit::DrawKind::BeginVertices) c.inlineDraw = std::move(owned);
    else { c.Process(pending); c.Recycle(std::move(owned)); }
}
void LiveGeometryCapture::FlushInline()
{
    auto& c = *impl;
    if (c.inlineDraw) { auto pending = std::move(c.inlineDraw); c.Process(*pending); c.Recycle(std::move(pending)); }
}
void LiveGeometryCapture::Swap(uint64_t previousIndexed)
{
    auto& c = *impl;
    FlushInline();
    if (c.state == Impl::State::Capturing)
    {
        if (!c.stack.empty()||c.resolve) c.Error("Frame ended inside an engine operation");
        if(c.onFrame)
        {
            if(c.failures)throw std::runtime_error(c.errors.empty()?"Native continuous geometry failed":c.errors.front());
            c.onFrame(c.batch);
            if(c.profile&&++c.profileFrames%300==0)
            {
                const double n=300;
                kernel::Log("native capture profile: begin %.2f ms, geometry %.2f ms, commit %.2f ms, other %.2f ms; %.0f KB indices, %.0f KB streams per frame",
                    c.profileMs[0]/n,c.profileMs[1]/n,c.profileMs[2]/n,c.profileMs[3]/n,c.profileIndexBytes/n/1024,c.profileStreamBytes/n/1024);
                kernel::Log("native capture cache per frame: streams %.0f KB hit / %.0f KB converted, indices %.0f KB hit / %.0f KB converted",
                    g_streamHitBytes.exchange(0)/n/1024,g_streamMissBytes.exchange(0)/n/1024,g_indexHitBytes.exchange(0)/n/1024,g_indexMissBytes.exchange(0)/n/1024);
                std::fill(std::begin(c.profileMs),std::end(c.profileMs),0.0);c.profileIndexBytes=c.profileStreamBytes=0;
            }
            const auto serial=c.batch.serial+1;
            // Keep the previous frame's capacity (RenderFrame has completed):
            // growing these vectors moved every large command and re-copied
            // the stream arena, and freeing them returned pages to the OS.
            const size_t commandCount=c.batch.commands.size(),streamBytes=c.batch.streams.bytes.size();
            auto commands=std::move(c.batch.commands);auto bytes=std::move(c.batch.streams.bytes);auto entries=std::move(c.batch.streams.entries);
            commands.clear();bytes.clear();entries.clear();
            c.batch={};c.batch.serial=serial;c.calls=c.vertexBytes=c.indexBytes=0;
            c.batch.commands=std::move(commands);c.batch.streams.bytes=std::move(bytes);c.batch.streams.entries=std::move(entries);
            c.batch.commands.reserve(commandCount+commandCount/4+32);
            c.batch.streams.bytes.reserve(streamBytes+streamBytes/4+65536);
            c.TrackStreams();
            c.geometryPackets=c.clearPackets=c.resolvePackets=c.patchedShaderSnapshots=0;
            c.thread=0;c.frameCamera=false;
            return;
        }
        if(!c.failures&&c.patchedShaderSnapshots<c.minimumPatched) {
            // Targeted first-bind regression: retain the first complete valid
            // frame exercising a patch. Errors are terminal, never discarded.
            ++c.skippedUnpatchedFrames;
            c.batch={};c.calls=c.vertexBytes=c.indexBytes=0;
            c.geometryPackets=c.clearPackets=c.resolvePackets=c.patchedShaderSnapshots=0;
            c.thread=0;c.expectedIndexed=previousIndexed;
            c.state=previousIndexed>=c.minimumIndexed?Impl::State::Capturing:Impl::State::Waiting;
            if(!c.Write())c.reportFailed=true;
            return;
        }
        c.state = Impl::State::Complete;
        if (!c.Write()) c.reportFailed = true;
        kernel::Log("native geometry: %zu packets, %llu failures, %llu vertex bytes, %zu pulled stream bytes", c.batch.commands.size(), c.failures, c.vertexBytes, c.batch.streams.bytes.size());
    }
    else if (c.state == Impl::State::Waiting && GetTickCount64() >= c.armedAt && previousIndexed >= c.minimumIndexed)
    {
        c.expectedIndexed = previousIndexed;
        c.state = Impl::State::Capturing;
        kernel::Log("native geometry: copying one live scene frame");
    }
}
bool LiveGeometryCapture::Finish()
{
    auto& c = *impl;
    if (!c.Write()) c.reportFailed = true;
    return c.state == Impl::State::Complete && c.calls && !c.failures && !c.reportFailed;
}
const GeometryBatch* LiveGeometryCapture::Captured() const
{
    const auto& c = *impl;
    return c.state == Impl::State::Complete && !c.failures ? &c.batch : nullptr;
}
void LiveGeometryCapture::ArmFrame(uint64_t serial)
{
    auto& c=*impl;
    if(c.state!=Impl::State::Waiting)throw std::runtime_error("Native frame capture was already armed");
    c.state=Impl::State::Capturing;
    c.batch.serial=serial;
}
void LiveGeometryCapture::Clear(uint32_t device,uint32_t flags,uint32_t color,float depth,uint32_t stencil,uint32_t count,uint32_t rectangles) {
    FlushInline();auto& c=*impl;
    if(c.state!=Impl::State::Capturing)return;
    try {
        if(!c.EnterThread())throw std::runtime_error("Clear crossed an unfinished native operation");
        if(count>256||(count&&!Readable(rectangles,count*16)))throw std::runtime_error("Unreadable native clear rectangles");
        std::vector<RenderRect> rects;
        for(uint32_t i=0;i<count;++i)rects.emplace_back(int32_t(mem::Read32(rectangles+i*16)),int32_t(mem::Read32(rectangles+i*16+4)),int32_t(mem::Read32(rectangles+i*16+8)),int32_t(mem::Read32(rectangles+i*16+12)));
        GeometryBatch::Command command;
        command.state=SnapshotClear(SurfaceState(device),flags,color,depth,stencil,rects);
        c.Commit(std::move(command));++c.clearPackets;
    } catch(const std::exception& error){c.Error(error.what());}
}
void LiveGeometryCapture::ResolveBegin(uint32_t device,uint32_t flags,uint32_t texture) {
    FlushInline();auto& c=*impl;
    if(c.state!=Impl::State::Capturing)return;
    if(!texture)return; // XDK flush-only calls do not produce a texture copy.
    try {
        if(!c.EnterThread()||c.resolve)throw std::runtime_error("Overlapping native resolve operations");
        if(!Readable(texture,0x34))throw std::runtime_error("Unreadable native resolve texture");
        PendingResolve pending{device,mem::Read32(device+0x30),flags,SurfaceState(device),{}};
        for(uint32_t i=0;i<6;++i)pending.fetch[i]=mem::Read32(texture+0x1C+i*4);
        c.resolve=std::move(pending);
    } catch(const std::exception& error){c.Error(error.what());}
}
void LiveGeometryCapture::ResolveEnd(uint32_t device) {
    auto& c=*impl;
    if(c.state!=Impl::State::Capturing||!c.resolve)return;
    auto pending=std::move(*c.resolve);c.resolve.reset();
    try {
        if(c.thread!=GetCurrentThreadId()||device!=pending.device)throw std::runtime_error("Unbalanced native resolve hooks");
        const uint32_t after=mem::Read32(device+0x30);
        AppendCommands(pending.commands,pending.cursor,after);
        const auto copy=ParseCopyRegisters(pending.commands);
        GeometryBatch::Command command;
        command.state=SnapshotResolve(std::move(pending.source),pending.flags,pending.fetch,copy);
        std::copy(pending.fetch.begin(),pending.fetch.end(),command.state.textureFetches[0]);
        c.Commit(std::move(command));++c.resolvePackets;
    } catch(const std::exception& error){
        const auto path=std::filesystem::path(c.report).wstring()+L".resolve-failure.bin";
        if(FILE* file=_wfopen(path.c_str(),L"wb")){fwrite(pending.commands.data(),1,pending.commands.size(),file);fclose(file);}
        c.Error(error.what());
    }
}
audit::Status LiveGeometryCapture::Status() const {
    const auto& c=*impl;audit::Status result;
    result.captureEnabled=true;result.captureComplete=c.state==Impl::State::Complete;
    result.geometryPackets=c.geometryPackets;result.clearPackets=c.clearPackets;result.resolvePackets=c.resolvePackets;result.captureFailures=c.failures;
    return result;
}
void LiveGeometryCapture::SetContinuous(std::function<void(Draw&)> command,std::function<void(GeometryBatch&)> frame)
{
    auto& c=*impl;
    c.onCommand=std::move(command);c.onFrame=std::move(frame);c.pullPrimary=true;c.state=Impl::State::Capturing;
    c.TrackStreams();
}
void LiveGeometryCapture::CommandBlock(uint32_t device,bool begin)
{
    auto& c=*impl;
    if(c.state!=Impl::State::Capturing)return;
    try
    {
        // BeginVertices returns before the caller fills its reserved stream.
        // That deferred draw lives outside stack. Finish it while the old
        // block and its inline vertices still exist, before reservation / a
        // context restore can recycle either one.
        if(begin&&c.inlineDraw&&c.inlineDraw->device==device)FlushInline();
        const uint32_t cursor=mem::Read32(device+0x30);
        auto preserve=[&](uint32_t& before,std::vector<uint8_t>& bytes){if(begin)AppendCommands(bytes,before,cursor);before=cursor;};
        for(auto& pending:c.stack)if(pending->device==device)preserve(pending->before,pending->commands);
        if(c.resolve&&c.resolve->device==device)preserve(c.resolve->cursor,c.resolve->commands);
    }
    catch(const std::exception& error){c.Error(error.what());}
}
}
#endif
