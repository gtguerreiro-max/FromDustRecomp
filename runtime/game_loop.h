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
}
