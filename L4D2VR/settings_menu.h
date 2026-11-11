#pragma once
#include "openvr.h"
#include <chrono>
#include <vector>

class VR;
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct ID3D11RenderTargetView;
struct ImGuiContext;

// "L4D2VR" tab in the SteamVR dashboard for changing the config.txt settings while playing.
// Changes are applied immediately and saved to VR\config.txt.
//
// The menu is drawn with Dear ImGui on its own D3D11 device, so it never touches the state of the game's D3D9 (DXVK) device.
// The pixels are read back and sent with SetOverlayRaw (sharing the D3D11 texture with SetOverlayTexture showed an empty tab).
// Everything runs on the game's render thread from VR::Update(), like the other overlay calls.
class SettingsMenu
{
public:
	explicit SettingsMenu(VR *vr);
	~SettingsMenu();

	void Update();

private:
	bool Init();
	bool CreateDevice();
	bool CreateOverlay();
	bool InitImGui();
	void Shutdown();

	void ProcessOverlayEvents();
	void Render();
	void DrawMenu();
	void DrawSlider(const char *label, const char *description, float &value, float min, float max, float step, const char *format);
	void DrawCheckbox(const char *label, const char *description, bool &value);
	void DrawDescription(const char *description);

	void MarkChanged();
	void Save();

	VR *m_VR = nullptr;
	bool m_InitAttempted = false;
	bool m_Initialized = false;

	vr::VROverlayHandle_t m_OverlayHandle = vr::k_ulOverlayHandleInvalid;
	vr::VROverlayHandle_t m_ThumbnailHandle = vr::k_ulOverlayHandleInvalid;

	ID3D11Device *m_Device = nullptr;
	ID3D11DeviceContext *m_Context = nullptr;
	ID3D11Texture2D *m_Texture = nullptr;
	ID3D11Texture2D *m_StagingTexture = nullptr; // CPU readable copy of m_Texture, for SetOverlayRaw
	ID3D11RenderTargetView *m_RenderTargetView = nullptr;
	std::vector<unsigned char> m_Pixels;

	ImGuiContext *m_ImGuiContext = nullptr;
	bool m_ImGuiRendererInitialized = false;

	bool m_NeedsRedraw = true;
	int m_PendingFrames = 0; // frames to draw right away, once the tab is visible
	std::chrono::steady_clock::time_point m_LastFrameTime;

	// Every SetOverlayRaw takes a block of shared memory that SteamVR gives back once it has used it. With too many of
	// them waiting (201) it refuses every upload of the game, the wrist HUD's too. So the image is only sent when it
	// changed, and after a refusal the overlay is made again, which gives its blocks back.
	unsigned long long m_UploadedHash = 0;
	bool m_UploadedValid = false;
	int m_UploadFailures = 0;
	std::chrono::steady_clock::time_point m_RetryUploadTime;
	// Clicks right after the tab opens are ignored: the laser can already be pressed on a slider
	std::chrono::steady_clock::time_point m_ShownTime;

	// Diagnostics, written to VR\settings_menu_log.txt
	int m_FramesDrawn = 0;
	int m_LastVisibilityState = -1;
	vr::EVROverlayError m_LastTextureError = vr::VROverlayError_None;
	bool m_LoggedMapError = false;

	// Last laser pointer input on the tab, to draw at full rate only while it's being used
	std::chrono::steady_clock::time_point m_LastInputTime;

	// Changes are applied at once and written to config.txt with the Save button only
	bool m_HasUnsavedChanges = false;
	bool m_SavedOnce = false;
	bool m_LastSaveFailed = false;
};
