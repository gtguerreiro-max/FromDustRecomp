#pragma once
// Input -> present latency: from the moment a changed pad state (buttons,
// triggers or sticks, including the PC mouse and keyboard mapping) reaches
// the game, to the return of the Present call of the first frame the game
// captured after it. Display scan-out and, with frame generation, the
// Streamline pacer's hold of the real frame come on top.
#include <cstdint>

namespace latency
{
void InputChanged();                 // XamInputGetState returned a changed state
void FrameCaptured();                // VdSwap: the frame being built is complete
uint64_t CapturedFrames();
void FrameDelivered(uint64_t frame); // that captured frame was handed to the presenter
void Presented();                    // a swap chain Present returned
struct Stats { double averageMs = 0, minimumMs = 0, maximumMs = 0; uint32_t samples = 0; };
Stats Get();                         // the last complete 2 s window
}
