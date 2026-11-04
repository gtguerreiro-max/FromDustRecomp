#pragma once
#include <cstdint>

namespace gpu::native { struct GeometryBatch; class ShaderCatalog; }

namespace gpu::native::audit
{
enum class DrawKind { Indexed, Vertices, BeginVertices };
struct Status {
    bool enabled=false,busy=false,captureEnabled=false,captureComplete=false;
    uint64_t catalogShaders=0,shaderInitializations=0,matchedShaders=0,frames=0,drawCalls=0,unresolvedDraws=0;
    uint64_t geometryPackets=0,clearPackets=0,resolvePackets=0,captureFailures=0;
};
#ifdef FD_NATIVE_RENDERER
Status GetStatus();
bool Initialize();
void ShaderInit(uint32_t stage, uint32_t object, uint32_t physical);
int TraceShaderCode(uint32_t stage,uint32_t address);
void Draw(uint32_t device, DrawKind kind, uint32_t indexOffset = 0,uint32_t primitive=0,uint32_t count=UINT32_MAX);
void Scissor(uint32_t device, uint32_t left, uint32_t top, uint32_t right, uint32_t bottom);
void DrawEnd(uint32_t device, DrawKind kind);
void BeforeOtherCall();
void CommandBlock(uint32_t device,bool begin);
void Clear(uint32_t device,uint32_t flags,uint32_t color,float depth,uint32_t stencil,uint32_t rectangleCount,uint32_t rectangles);
void ResolveBegin(uint32_t device,uint32_t flags,uint32_t texture);
void ResolveEnd(uint32_t device);
const GeometryBatch* CapturedGeometry();
const ShaderCatalog* CapturedCatalog();
void ArmFrame(uint64_t serial);
void Swap(uint32_t frontBuffer=0);
bool Finish();
#else
inline bool Initialize() { return true; }
inline Status GetStatus() { return {}; }
inline void ShaderInit(uint32_t, uint32_t, uint32_t) {}
inline int TraceShaderCode(uint32_t,uint32_t) { return -1; }
inline void Draw(uint32_t, DrawKind, uint32_t = 0,uint32_t=0,uint32_t=UINT32_MAX) {}
inline void Scissor(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}
inline void DrawEnd(uint32_t, DrawKind) {}
inline void BeforeOtherCall() {}
inline void CommandBlock(uint32_t,bool) {}
inline void Clear(uint32_t,uint32_t,uint32_t,float,uint32_t,uint32_t,uint32_t) {}
inline void ResolveBegin(uint32_t,uint32_t,uint32_t) {}
inline void ResolveEnd(uint32_t) {}
inline const GeometryBatch* CapturedGeometry() { return nullptr; }
inline const ShaderCatalog* CapturedCatalog() { return nullptr; }
inline void ArmFrame(uint64_t) {}
inline void Swap(uint32_t=0) {}
inline bool Finish() { return true; }
#endif
}
