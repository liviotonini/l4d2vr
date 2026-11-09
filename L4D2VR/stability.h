#pragma once

// Fixes for the limits and noise of the engine, independent from the VR code
namespace Stability
{
	// Sharper system timer, raises the index buffer limit of the renderer, filters the console spam
	void Init();
}
