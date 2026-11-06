#pragma once
// Fixed-rate world simulation with a free render rate (see game_loop.cpp).
namespace gameloop
{
// World updates per second while decoupled; 0 = the original behaviour (one
// world step per rendered frame). FD_SIM_HZ overrides.
void SetSimulationRate(double hz);
double SimulationRate();
// The pad's buttons / triggers may change for the game only before an update
// that steps the world (edges are consumed by world logic).
bool InputMayChange();
// The display pacer's limit (FPS, 0 = none), for even pacing.
void SetFrameLimit(int fps);
// DLSS Frame Generation paces and fills in frames itself: even pacing steps
// aside while it is active (holding the game loop there kept DLSS-G from
// generating frames).
void SetFrameGenerationActive(bool active);
// Even pacing (fixed-rate world): the rate the game loop holds now (120, 60
// or 30, so each world step stays on screen for the same number of frames)
// and its recent frame time (ms) without the pacing wait; 0 when off.
int EvenPaceRate();
float EvenPaceWorkMs();
}
