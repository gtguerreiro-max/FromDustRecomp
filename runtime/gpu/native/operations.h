#pragma once
#include "frame.h"
#include <array>
#include <functional>
namespace gpu::native {
using SurfaceReader=std::function<std::span<const uint8_t>(uint32_t,uint32_t)>;
struct CopyRegisters { uint32_t control,base,pitch,info; };
Draw SnapshotSurfaceState(std::span<const uint8_t> device,const SurfaceReader& read);
Draw SnapshotClear(Draw source,uint32_t flags,uint32_t color,float depth,uint32_t stencil,std::span<const RenderRect> rectangles);
CopyRegisters ParseCopyRegisters(std::span<const uint8_t> commands);
Draw SnapshotResolve(Draw source,uint32_t flags,std::span<const uint32_t,6> fetch,const CopyRegisters& copy);
}
