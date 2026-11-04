// Overrides of recompiled guest functions (XenonRecomp emits every function
// as a weak alias of __imp__sub_XXXXXXXX, so a strong definition replaces it).

#include "../kernel/kernel.h"
#include "xenia_bridge.h"
#include "native/live_audit.h"
#include <cstdlib>
#include <cstring>

// D3D's ring buffer kick: stores the new CP_RB_WPTR (MMIO 0x7FC80714). The
// MMIO range is plain memory here, so tell the command processor right away
// instead of waiting for it to poll.
PPC_FUNC_IMPL(__imp__sub_820D7210);
PPC_FUNC(sub_820D7210)
{
    __imp__sub_820D7210(ctx, base);
    gpu::xenia::NotifyWritePointer();
}
// The XDK completes the current command block and reserves another one.
// Preserve commands owned by an unfinished draw before the old block is reused.
PPC_FUNC_IMPL(__imp__sub_820D7D88);
PPC_FUNC(sub_820D7D88)
{
    const uint32_t device=ctx.r3.u32;
    gpu::native::audit::CommandBlock(device,true);
    __imp__sub_820D7D88(ctx,base);
    gpu::native::audit::CommandBlock(device,false);
}
// The lower-level ring reservation is also called directly, without the
// 820D7D88 wrapper. Retain a pending draw before it recycles the old block.
PPC_FUNC_IMPL(__imp__sub_820D70D8);
PPC_FUNC(sub_820D70D8)
{
    const uint32_t device=ctx.r3.u32;
    gpu::native::audit::CommandBlock(device,true);
    __imp__sub_820D70D8(ctx,base);
    gpu::native::audit::CommandBlock(device,false);
}
// Restoring the saved command context switches back from a temporary buffer.
// This is distinct from reserving a new ring block and can happen inside a
// DrawVertices call (notably clear quads).
PPC_FUNC_IMPL(__imp__sub_820D65A8);
PPC_FUNC(sub_820D65A8)
{
    const uint32_t device=ctx.r3.u32;
    gpu::native::audit::CommandBlock(device,true);
    __imp__sub_820D65A8(ctx,base);
    gpu::native::audit::CommandBlock(device,false);
}
PPC_FUNC_IMPL(__imp__sub_820D69A0);
PPC_FUNC(sub_820D69A0)
{
    const uint32_t device=ctx.r3.u32;
    gpu::native::audit::CommandBlock(device,true);
    __imp__sub_820D69A0(ctx,base);
    gpu::native::audit::CommandBlock(device,false);
}
// Some flush dispatches enter the block finalizer directly, bypassing both
// the public flush wrapper and the reserve path. It also changes device+0x30.
PPC_FUNC_IMPL(__imp__sub_820D7C00);
PPC_FUNC(sub_820D7C00)
{
    const uint32_t device=ctx.r3.u32;
    gpu::native::audit::CommandBlock(device,true);
    __imp__sub_820D7C00(ctx,base);
    gpu::native::audit::CommandBlock(device,false);
}

// D3DQuery::GetData: the native diagnostic can inspect completed CPU-visible
// occlusion values separately from the PM4 report producer. Type 9 is the
// sample-count query; timestamps / event queries retain their normal behavior.
PPC_FUNC_IMPL(__imp__sub_820DE478);
PPC_FUNC(sub_820DE478)
{
    const uint32_t query=ctx.r3.u32,result=ctx.r4.u32,size=ctx.r5.u32;
    const char* backend=getenv("FD_RENDERER");
    const bool occlusion=backend&&!strcmp(backend,"native")&&query>=0x40000000&&query<0xBFFFFF60&&
        result>=0x40000000&&uint64_t(result)+size<=0xC0000000&&size>=4&&mem::Read32(query+4)==9;
    __imp__sub_820DE478(ctx,base);
    if(occlusion)
    {
        if(getenv("FD_GPU_QUERY_TRACE"))
        {
            static unsigned traces=0;
            if(traces++<40)kernel::Log("native query result: object %08X status %08X bytes %u samples %u",query,ctx.r3.u32,size,mem::Read32(result));
        }
    }
}

// Logical window scissor. The XDK writes these three registers directly,
// including tiled variants, rather than keeping them in the device shadow.
PPC_FUNC_IMPL(__imp__sub_820D0E88);
PPC_FUNC(sub_820D0E88)
{
    gpu::native::audit::Scissor(ctx.r3.u32,ctx.r4.u32,ctx.r5.u32,ctx.r6.u32,ctx.r7.u32);
    __imp__sub_820D0E88(ctx,base);
}
