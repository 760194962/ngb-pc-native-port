// The console's Xbox Live dashboard in the port (xonlinedash.xbe: Insignia accounts are made there). dashboard.cpp has
// it ready; kernel/k_misc.cpp opens it from a game and goes back to the game from it.
#pragma once

#include <string>

// The dashboard can run now: online play is on, its files are in the port's hard disk (copied once from the registered
// xemu disk image) and are the build the port knows. Decided once per run; false in the dashboard itself.
bool dashboard_available();
std::wstring dashboard_xbe(); // <hard disk>\Partition2\xodash\xonlinedash.xbe

// In the dashboard's process: start the game it was opened from (--return-xbe) and ask this one to end. false when this
// is not the dashboard or the game cannot be started (the caller ends as it would have). Once per run. (k_misc.cpp)
bool dashboard_back_to_game(const char *why);
