#pragma once

// Whether the mod runs: VREnabled in VR\config.txt, on when it's missing. Off, the game runs as it does without the mod:
// nothing is hooked, -insecure isn't needed, and the d3d9.dll calls go to Windows' own d3d9.dll (d3d9_main.cpp).
// Read once, the first time it's asked.
bool L4D2VR_IsEnabled();
