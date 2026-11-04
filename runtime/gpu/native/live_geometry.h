#pragma once
#ifdef FD_NATIVE_RENDERER
#include "geometry.h"
#include "device_state.h"
#include "live_audit.h"
#include "frame_builder.h"
#include <memory>
#include <functional>

namespace gpu::native
{
class LiveGeometryCapture
{
public:
    LiveGeometryCapture(unsigned seconds, unsigned minimumIndexed, std::string report, unsigned minimumPatched=0,bool pullPrimary=false);
    ~LiveGeometryCapture();
    void Begin(uint32_t device, audit::DrawKind kind, const NativeShader* shader, const NativeShader* pixelShader, const PacketState& packet,uint32_t primitive=0,uint32_t count=UINT32_MAX);
    void End(uint32_t device, audit::DrawKind kind);
    void FlushInline();
    void Clear(uint32_t device,uint32_t flags,uint32_t color,float depth,uint32_t stencil,uint32_t rectangleCount,uint32_t rectangles);
    void ResolveBegin(uint32_t device,uint32_t flags,uint32_t texture);
    void ResolveEnd(uint32_t device);
    audit::Status Status() const;
    void Swap(uint64_t previousIndexed);
    void ArmFrame(uint64_t serial);
    bool Finish();
    const GeometryBatch* Captured() const;
    void SetContinuous(std::function<void(Draw&)> command,std::function<void(const GeometryBatch&)> frame);
    void CommandBlock(uint32_t device,bool begin);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
#endif
