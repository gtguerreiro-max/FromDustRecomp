// Overrides of recompiled guest functions (XenonRecomp emits every function
// as a weak alias of __imp__sub_XXXXXXXX, so a strong definition replaces it).

#include "../kernel/kernel.h"
#include "xenia_bridge.h"

// D3D's ring buffer kick: stores the new CP_RB_WPTR (MMIO 0x7FC80714). The
// MMIO range is plain memory here, so tell the command processor right away
// instead of waiting for it to poll.
PPC_FUNC_IMPL(__imp__sub_820D7210);
PPC_FUNC(sub_820D7210)
{
    __imp__sub_820D7210(ctx, base);
    gpu::xenia::NotifyWritePointer();
}
