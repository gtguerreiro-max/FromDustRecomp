#pragma once
#include "frame.h"

namespace gpu::native
{
// These values are written directly by the draw/window calls, outside the
// device register shadow. They must come from those calls, not stale shadow.
struct PacketState
{
    uint32_t scissorTL = 0, scissorBR = 0, indexOffset = 0;
    bool hasScissor = false;
};

// Copies render state, booleans/loops and complete texture fetch descriptors.
// Geometry, float banks, shader identities and resource table slots are bound
// separately by their owners. No pointers into guest memory are retained.
Draw SnapshotDeviceState(std::span<const uint8_t> device, const PacketState& packet);
}
