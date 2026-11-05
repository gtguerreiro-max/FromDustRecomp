#pragma once
// On-screen button prompts for keyboard and mouse. From Dust draws its
// controller glyphs (A, B, X, LB, RB, LT, RT, Back, stick) from one UI atlas.
// When the player last used the keyboard / mouse, the decoded atlas gets
// keycaps and mouse icons for the current bindings drawn over those glyphs,
// so every menu, tutorial and HUD prompt shows the PC controls.
#include "texture_decode.h"

namespace gpu::native::prompts
{
// Texture resolve hook: returns -1 when `entry` is not the prompt atlas,
// otherwise the prompt generation it was drawn for (patched or not).
int Apply(TextureResource& entry);
}
