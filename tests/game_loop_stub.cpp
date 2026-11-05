// The settings / input tests link the XInput exports without the game loop.
#include "game_loop.h"
namespace gameloop
{
void SetSimulationRate(double) {}
double SimulationRate() { return 0; }
bool InputMayChange() { return true; }
}
