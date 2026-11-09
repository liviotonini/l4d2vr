#include "settings_menu.h"
#include "vr.h"
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include "thirdparty/imgui/imgui.h"
#include "thirdparty/imgui/imgui_impl_dx11.h"

namespace
{
	constexpr const char *kOverlayKey = "l4d2vr.settings";
	constexpr const char *kOverlayName = "L4D2VR";

	constexpr int kTextureWidth = 1280;
	constexpr int kTextureHeight = 900;
	constexpr float kOverlayWidthInMeters = 2.5f;

	// Big text and widgets, easier to read and to hit with the laser
	constexpr float kFontSize = 26.0f;
	constexpr float kUiScale = 1.5f;

	constexpr auto kFrameInterval = std::chrono::milliseconds(22);      // ~45 fps while the laser is used on the tab
	constexpr auto kIdleFrameInterval = std::chrono::milliseconds(250); // while the tab is open but nobody points at it
	constexpr auto kInputActiveTime = std::chrono::seconds(1);          // how long after the last input to keep the full rate
	constexpr auto kSaveDelay = std::chrono::milliseconds(750);   // wait for the user to stop changing values before writing config.txt

	typedef HRESULT(WINAPI *CreateDXGIFactory1Fn)(REFIID riid, void **factory);

	template <typename T>
	void SafeRelease(T *&object)
	{
		if (object)
		{
			object->Release();
			object = nullptr;
		}
	}

	int ToImGuiMouseButton(uint32_t button)
	{
		switch (button)
		{
		case vr::VRMouseButton_Left:   return ImGuiMouseButton_Left;
		case vr::VRMouseButton_Right:  return ImGuiMouseButton_Right;
		case vr::VRMouseButton_Middle: return ImGuiMouseButton_Middle;
		default:                       return -1;
		}
	}

	bool FileExists(const char *path)
	{
		return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
	}

	// Writes a line to VR\settings_menu_log.txt (recreated each time the game starts). Only used for rare events.
	void Log(const char *format, ...)
	{
		static bool s_Truncated = false;

		char currentDir[MAX_PATH];
		GetCurrentDirectoryA(MAX_PATH, currentDir);
		char path[MAX_PATH];
		snprintf(path, MAX_PATH, "%s\\VR\\settings_menu_log.txt", currentDir);

		FILE *file = nullptr;
		if (fopen_s(&file, path, s_Truncated ? "a" : "w") != 0 || !file)
			return;
		s_Truncated = true;

		SYSTEMTIME time;
		GetLocalTime(&time);
		fprintf(file, "%02d:%02d:%02d.%03d ", time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);

		va_list args;
		va_start(args, format);
		vfprintf(file, format, args);
		va_end(args);

		fputc('\n', file);
		fclose(file);
	}

	void ApplyStyle()
	{
		ImGui::StyleColorsDark();

		ImGuiStyle &style = ImGui::GetStyle();
		style.WindowPadding = ImVec2(18.0f, 14.0f);
		style.FramePadding = ImVec2(10.0f, 6.0f);
		style.ItemSpacing = ImVec2(10.0f, 8.0f);
		style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
		style.WindowRounding = 0.0f;
		style.FrameRounding = 5.0f;
		style.GrabRounding = 5.0f;
		style.TabRounding = 5.0f;
		style.GrabMinSize = 22.0f;
		style.ScrollbarSize = 22.0f;
		style.WindowBorderSize = 0.0f;
		style.ScaleAllSizes(kUiScale);
		style.FontSizeBase = kFontSize;

		const ImVec4 accent = ImVec4(0.66f, 0.17f, 0.15f, 1.00f);
		const ImVec4 accentHovered = ImVec4(0.80f, 0.24f, 0.21f, 1.00f);
		const ImVec4 accentActive = ImVec4(0.90f, 0.31f, 0.27f, 1.00f);

		ImVec4 *colors = style.Colors;
		colors[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.07f, 0.08f, 1.00f);
		colors[ImGuiCol_ChildBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
		colors[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.16f, 0.18f, 1.00f);
		colors[ImGuiCol_FrameBgHovered] = ImVec4(0.22f, 0.22f, 0.25f, 1.00f);
		colors[ImGuiCol_FrameBgActive] = ImVec4(0.27f, 0.27f, 0.31f, 1.00f);
		colors[ImGuiCol_Button] = ImVec4(0.20f, 0.20f, 0.23f, 1.00f);
		colors[ImGuiCol_ButtonHovered] = accentHovered;
		colors[ImGuiCol_ButtonActive] = accentActive;
		colors[ImGuiCol_SliderGrab] = accent;
		colors[ImGuiCol_SliderGrabActive] = accentActive;
		colors[ImGuiCol_CheckMark] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
		colors[ImGuiCol_CheckboxSelectedBg] = accent;
		colors[ImGuiCol_Tab] = ImVec4(0.14f, 0.14f, 0.16f, 1.00f);
		colors[ImGuiCol_TabHovered] = accentHovered;
		colors[ImGuiCol_TabSelected] = accent;
		colors[ImGuiCol_TabSelectedOverline] = accentActive;
		// The window is "unfocused" until it's clicked, keep the tabs looking the same in that case
		colors[ImGuiCol_TabDimmed] = colors[ImGuiCol_Tab];
		colors[ImGuiCol_TabDimmedSelected] = colors[ImGuiCol_TabSelected];
		colors[ImGuiCol_TabDimmedSelectedOverline] = colors[ImGuiCol_TabSelectedOverline];
		colors[ImGuiCol_Separator] = ImVec4(0.25f, 0.25f, 0.28f, 1.00f);
	}
}

SettingsMenu::SettingsMenu(VR *vr)
	: m_VR(vr)
{
}

SettingsMenu::~SettingsMenu()
{
	Shutdown();
}

void SettingsMenu::Update()
{
	if (!m_InitAttempted)
	{
		m_InitAttempted = true;
		m_Initialized = Init();
		if (!m_Initialized)
		{
			OutputDebugStringA("[L4D2VR] Settings menu could not be created, the SteamVR dashboard tab is disabled.\n");
			Log("Init failed, the dashboard tab is disabled");
			Shutdown();
			return;
		}
		Log("Init done");
	}

	if (!m_Initialized)
		return;

	ImGui::SetCurrentContext(m_ImGuiContext);

	ProcessOverlayEvents();

	vr::IVROverlay *overlay = vr::VROverlay();
	const bool overlayVisible = overlay->IsOverlayVisible(m_OverlayHandle);
	const bool activeDashboardOverlay = overlay->IsActiveDashboardOverlay(m_OverlayHandle);
	const bool dashboardVisible = overlay->IsDashboardVisible();

	const int visibilityState = (overlayVisible ? 1 : 0) | (activeDashboardOverlay ? 2 : 0) | (dashboardVisible ? 4 : 0);
	if (visibilityState != m_LastVisibilityState)
	{
		Log("visibility: overlay visible %d, active dashboard overlay %d, dashboard visible %d (%d frames drawn so far)",
			overlayVisible, activeDashboardOverlay, dashboardVisible, m_FramesDrawn);
		m_LastVisibilityState = visibilityState;
		m_NeedsRedraw = true;
	}

	const bool tabVisible = overlayVisible || activeDashboardOverlay;

	// ImGui needs a second frame to lay out the tab bar and show the contents of the selected tab
	if (m_NeedsRedraw)
	{
		m_PendingFrames = 2;
		m_NeedsRedraw = false;
	}

	// Every frame is read back and uploaded through the CPU, so only draw at full rate while the menu is being used
	const auto now = std::chrono::steady_clock::now();
	const bool inUse = now - m_LastInputTime < kInputActiveTime || ImGui::IsAnyItemActive();
	const auto frameInterval = inUse ? kFrameInterval : kIdleFrameInterval;

	if (m_PendingFrames > 0 || (tabVisible && now - m_LastFrameTime >= frameInterval))
	{
		Render();
		if (m_PendingFrames > 0)
			m_PendingFrames--;
	}

	// Save right away when the dashboard is closed
	SaveIfNeeded(!dashboardVisible);
}

bool SettingsMenu::Init()
{
	vr::IVROverlay *overlay = vr::VROverlay();
	if (!overlay)
	{
		Log("Init: no IVROverlay interface");
		return false;
	}

	// d3d11.dll and d3dcompiler_47.dll are delay loaded (so the mod still loads without them). Make sure they exist before using them.
	if (!LoadLibraryA("d3d11.dll") || !LoadLibraryA("d3dcompiler_47.dll"))
	{
		Log("Init: could not load d3d11.dll or d3dcompiler_47.dll");
		return false;
	}

	if (!CreateDevice())
	{
		Log("Init: could not create the D3D11 device or texture");
		return false;
	}

	const vr::EVROverlayError createError = overlay->CreateDashboardOverlay(kOverlayKey, kOverlayName, &m_OverlayHandle, &m_ThumbnailHandle);
	if (createError != vr::VROverlayError_None)
	{
		Log("Init: CreateDashboardOverlay failed: %s (%d)", overlay->GetOverlayErrorNameFromEnum(createError), (int)createError);
		m_OverlayHandle = vr::k_ulOverlayHandleInvalid;
		m_ThumbnailHandle = vr::k_ulOverlayHandleInvalid;
		return false;
	}

	overlay->SetOverlayWidthInMeters(m_OverlayHandle, kOverlayWidthInMeters);
	overlay->SetOverlayInputMethod(m_OverlayHandle, vr::VROverlayInputMethod_Mouse);
	overlay->SetOverlayFlag(m_OverlayHandle, vr::VROverlayFlags_SendVRDiscreteScrollEvents, true);

	const vr::HmdVector2_t mouseScale = { (float)kTextureWidth, (float)kTextureHeight };
	overlay->SetOverlayMouseScale(m_OverlayHandle, &mouseScale);

	char currentDir[MAX_PATH];
	GetCurrentDirectoryA(MAX_PATH, currentDir);
	char thumbnailPath[MAX_PATH];
	snprintf(thumbnailPath, MAX_PATH, "%s\\VR\\l4d2vr_capsule_main.png", currentDir);
	if (FileExists(thumbnailPath))
		overlay->SetOverlayFromFile(m_ThumbnailHandle, thumbnailPath);

	return InitImGui();
}

bool SettingsMenu::InitImGui()
{
	IMGUI_CHECKVERSION();
	m_ImGuiContext = ImGui::CreateContext();
	ImGui::SetCurrentContext(m_ImGuiContext);

	ImGuiIO &io = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.LogFilename = nullptr;
	io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange | ImGuiConfigFlags_NoKeyboard;
	io.DisplaySize = ImVec2((float)kTextureWidth, (float)kTextureHeight);

	ApplyStyle();

	char windowsDir[MAX_PATH];
	GetWindowsDirectoryA(windowsDir, MAX_PATH);
	char fontPath[MAX_PATH];
	snprintf(fontPath, MAX_PATH, "%s\\Fonts\\segoeui.ttf", windowsDir);

	ImFont *font = FileExists(fontPath) ? io.Fonts->AddFontFromFileTTF(fontPath, kFontSize) : nullptr;
	if (!font)
		io.Fonts->AddFontDefaultVector();

	m_ImGuiRendererInitialized = ImGui_ImplDX11_Init(m_Device, m_Context);
	if (!m_ImGuiRendererInitialized)
		Log("Init: ImGui_ImplDX11_Init failed");
	return m_ImGuiRendererInitialized;
}

bool SettingsMenu::CreateDevice()
{
	// The overlay texture has to be on the GPU the headset is connected to
	IDXGIAdapter *adapter = nullptr;
	int32_t adapterIndex = -1;
	if (vr::VRSystem())
		vr::VRSystem()->GetDXGIOutputInfo(&adapterIndex);

	if (adapterIndex >= 0)
	{
		HMODULE dxgiModule = LoadLibraryA("dxgi.dll");
		CreateDXGIFactory1Fn createFactory = dxgiModule ? (CreateDXGIFactory1Fn)GetProcAddress(dxgiModule, "CreateDXGIFactory1") : nullptr;
		IDXGIFactory1 *factory = nullptr;
		if (createFactory && SUCCEEDED(createFactory(__uuidof(IDXGIFactory1), (void **)&factory)))
		{
			if (FAILED(factory->EnumAdapters(adapterIndex, &adapter)))
				adapter = nullptr;
			factory->Release();
		}
	}

	const D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
	HRESULT result = D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
		featureLevels, ARRAYSIZE(featureLevels), D3D11_SDK_VERSION, &m_Device, nullptr, &m_Context);
	SafeRelease(adapter);

	Log("D3D11CreateDevice on adapter %d: 0x%08X", adapterIndex, (unsigned)result);
	if (FAILED(result))
		return false;

	D3D11_TEXTURE2D_DESC textureDesc = {};
	textureDesc.Width = kTextureWidth;
	textureDesc.Height = kTextureHeight;
	textureDesc.MipLevels = 1;
	textureDesc.ArraySize = 1;
	textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	textureDesc.SampleDesc.Count = 1;
	textureDesc.Usage = D3D11_USAGE_DEFAULT;
	textureDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

	if (FAILED(m_Device->CreateTexture2D(&textureDesc, nullptr, &m_Texture)))
		return false;

	// CPU readable copy of the texture, for SetOverlayRaw
	D3D11_TEXTURE2D_DESC stagingDesc = textureDesc;
	stagingDesc.Usage = D3D11_USAGE_STAGING;
	stagingDesc.BindFlags = 0;
	stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	if (FAILED(m_Device->CreateTexture2D(&stagingDesc, nullptr, &m_StagingTexture)))
		return false;

	return SUCCEEDED(m_Device->CreateRenderTargetView(m_Texture, nullptr, &m_RenderTargetView));
}

void SettingsMenu::Shutdown()
{
	if (m_ImGuiContext)
	{
		ImGui::SetCurrentContext(m_ImGuiContext);
		if (m_ImGuiRendererInitialized)
			ImGui_ImplDX11_Shutdown();
		ImGui::DestroyContext(m_ImGuiContext);
		m_ImGuiContext = nullptr;
		m_ImGuiRendererInitialized = false;
	}

	SafeRelease(m_RenderTargetView);
	SafeRelease(m_Texture);
	SafeRelease(m_StagingTexture);
	SafeRelease(m_Context);
	SafeRelease(m_Device);

	if (m_OverlayHandle != vr::k_ulOverlayHandleInvalid && vr::VROverlay())
		vr::VROverlay()->DestroyOverlay(m_OverlayHandle); // also destroys the thumbnail

	m_OverlayHandle = vr::k_ulOverlayHandleInvalid;
	m_ThumbnailHandle = vr::k_ulOverlayHandleInvalid;
	m_Initialized = false;
}

void SettingsMenu::ProcessOverlayEvents()
{
	ImGuiIO &io = ImGui::GetIO();

	vr::VREvent_t event;
	while (vr::VROverlay()->PollNextOverlayEvent(m_OverlayHandle, &event, sizeof(event)))
	{
		if (event.eventType == vr::VREvent_MouseMove || event.eventType == vr::VREvent_MouseButtonDown
			|| event.eventType == vr::VREvent_MouseButtonUp || event.eventType == vr::VREvent_ScrollDiscrete)
			m_LastInputTime = std::chrono::steady_clock::now();

		switch (event.eventType)
		{
		case vr::VREvent_MouseMove:
			// OpenVR mouse coordinates start at the bottom left of the overlay
			io.AddMousePosEvent(event.data.mouse.x, kTextureHeight - event.data.mouse.y);
			break;

		case vr::VREvent_MouseButtonDown:
		case vr::VREvent_MouseButtonUp:
		{
			int button = ToImGuiMouseButton(event.data.mouse.button);
			if (button >= 0)
			{
				io.AddMousePosEvent(event.data.mouse.x, kTextureHeight - event.data.mouse.y);
				io.AddMouseButtonEvent(button, event.eventType == vr::VREvent_MouseButtonDown);
			}
			break;
		}

		case vr::VREvent_ScrollDiscrete:
			io.AddMouseWheelEvent(event.data.scroll.xdelta, event.data.scroll.ydelta);
			break;

		case vr::VREvent_FocusLeave:
			// The laser left the overlay
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
			io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
			break;

		case vr::VREvent_OverlayShown:
			Log("event: OverlayShown");
			m_NeedsRedraw = true;
			break;

		case vr::VREvent_OverlayHidden:
			Log("event: OverlayHidden");
			break;
		}
	}
}

void SettingsMenu::Render()
{
	ImGuiIO &io = ImGui::GetIO();

	const auto now = std::chrono::steady_clock::now();
	const float deltaTime = std::chrono::duration<float>(now - m_LastFrameTime).count();
	m_LastFrameTime = now;

	io.DeltaTime = std::clamp(deltaTime, 0.001f, 0.1f);
	io.DisplaySize = ImVec2((float)kTextureWidth, (float)kTextureHeight);

	ImGui_ImplDX11_NewFrame();
	ImGui::NewFrame();
	DrawMenu();
	ImGui::Render();

	const ImVec4 &background = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
	const float clearColor[4] = { background.x, background.y, background.z, 1.0f };
	m_Context->OMSetRenderTargets(1, &m_RenderTargetView, nullptr);
	m_Context->ClearRenderTargetView(m_RenderTargetView, clearColor);
	ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

	// Send the pixels with SetOverlayRaw instead of sharing the D3D11 texture with SetOverlayTexture. Inside the game's
	// process the shared texture was accepted without errors but the dashboard tab stayed empty, while the same code in a
	// standalone process worked. Copying through the CPU avoids GPU texture sharing completely.
	const auto uploadStart = std::chrono::steady_clock::now();

	m_Context->CopyResource(m_StagingTexture, m_Texture);
	D3D11_MAPPED_SUBRESOURCE mapped;
	const HRESULT mapResult = m_Context->Map(m_StagingTexture, 0, D3D11_MAP_READ, 0, &mapped);
	if (FAILED(mapResult))
	{
		if (!m_LoggedMapError)
			Log("Map of the staging texture failed: 0x%08X", (unsigned)mapResult);
		m_LoggedMapError = true;
		return;
	}

	const size_t rowSize = (size_t)kTextureWidth * 4;
	m_Pixels.resize(rowSize * kTextureHeight);
	for (int y = 0; y < kTextureHeight; y++)
		memcpy(&m_Pixels[y * rowSize], (const unsigned char *)mapped.pData + (size_t)y * mapped.RowPitch, rowSize);
	m_Context->Unmap(m_StagingTexture, 0);

	const vr::EVROverlayError error = vr::VROverlay()->SetOverlayRaw(m_OverlayHandle, m_Pixels.data(), kTextureWidth, kTextureHeight, 4);

	m_FramesDrawn++;
	const float uploadMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - uploadStart).count();
	if (error != m_LastTextureError || m_FramesDrawn == 1 || m_FramesDrawn % 500 == 0)
	{
		Log("SetOverlayRaw: %s (%d) on frame %d, read back and upload took %.2f ms", vr::VROverlay()->GetOverlayErrorNameFromEnum(error), (int)error, m_FramesDrawn, uploadMs);
		m_LastTextureError = error;
	}
}

void SettingsMenu::DrawMenu()
{
	const ImGuiStyle &style = ImGui::GetStyle();
	const ImGuiViewport *viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(viewport->Pos);
	ImGui::SetNextWindowSize(viewport->Size);
	ImGui::Begin("L4D2VR Settings", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings
		| ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);

	ImGui::PushFont(nullptr, kFontSize * 1.35f);
	ImGui::TextUnformatted("L4D2VR Settings");
	ImGui::PopFont();

	// Space for the separator and the status line at the bottom
	const float footerHeight = ImGui::GetTextLineHeight() + style.ItemSpacing.y * 2.0f + 1.0f;

	VR &settings = *m_VR;
	if (ImGui::BeginTabBar("SettingsTabs"))
	{
		if (ImGui::BeginTabItem("Turning"))
		{
			ImGui::BeginChild("Page", ImVec2(0.0f, -footerHeight));
			DrawCheckbox("Snap turning", "Turn in fixed steps instead of turning smoothly.", settings.m_SnapTurning);

			ImGui::BeginDisabled(!settings.m_SnapTurning);
			DrawSlider("Snap turn angle", "Degrees turned on each snap turn.", settings.m_SnapTurnAngle, 10.0f, 90.0f, 5.0f, "%.0f deg");
			ImGui::EndDisabled();

			ImGui::BeginDisabled(settings.m_SnapTurning);
			DrawSlider("Smooth turn speed", "Turning speed with the thumbstick fully pushed. 0.3 = 300 degrees per second.", settings.m_TurnSpeed, 0.05f, 1.5f, 0.05f, "%.2f");
			ImGui::EndDisabled();
			ImGui::EndChild();
			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem("Player"))
		{
			ImGui::BeginChild("Page", ImVec2(0.0f, -footerHeight));
			DrawCheckbox("Left handed", "Hold the weapon with the left controller.", settings.m_LeftHanded);
			DrawSlider("Player height", "Your real height. Used for physical crouching and for the item positions on your body (legs and back).", settings.m_PlayerHeightInMeters, 1.2f, 2.2f, 0.01f, "%.2f m");
			DrawCheckbox("Crouch physically", "Crouch in the game when you crouch in real life.", settings.m_ActivateCrouchPhisically);
			DrawCheckbox("Crouch button toggles", "Pressing the crouch button toggles crouching instead of crouching only while it's held.", settings.m_UseCrouchButtonAsToggle);
			DrawSlider("World scale", "Game units per meter (default 43.2). Lower values make the world look bigger.", settings.m_VRScale, 20.0f, 80.0f, 0.1f, "%.1f");
			DrawSlider("IPD scale", "Multiplies the distance between the eyes. Changes how big the world feels.", settings.m_IpdScale, 0.5f, 2.0f, 0.05f, "%.2f");
			ImGui::EndChild();
			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem("Weapons"))
		{
			ImGui::BeginChild("Page", ImVec2(0.0f, -footerHeight));
			DrawSlider("Controller angle", "Tilts the aim relative to the controller (default -45).", settings.m_ControllerAngleAdjustment, -90.0f, 90.0f, 1.0f, "%.0f deg");
			DrawSlider("Weapon side offset", "Moves the weapon sideways from the hand, in game units.", settings.m_WeaponHorizontalPositionAdjustment, -10.0f, 10.0f, 0.5f, "%.1f");
			DrawCheckbox("Interact with your hand", "Doors, items and teammates are used by pointing your weapon hand at them instead of looking at them. In a game you joined, the host needs this version of the mod.", settings.m_InteractWithHand);
			DrawCheckbox("Shove by swinging", "Shove by swinging the left controller (or the right one when not holding a melee weapon).", settings.m_IsTrackingManualPush);
			DrawSlider("Melee swing speed", "How fast the controller has to move to hit with a melee weapon.", settings.m_MeleeSwingVelocityRequired, 0.5f, 4.0f, 0.05f, "%.2f m/s");
			DrawCheckbox("Throw grenades with your arm", "Hold the trigger, swing your arm and let go: the molotov, pipe bomb or bile jar leaves your hand in the direction and with the speed of the swing. Letting go with a still hand drops it. In a game you joined, the host needs this version of the mod.", settings.m_GrenadeThrowGesture);

			ImGui::BeginDisabled(!settings.m_GrenadeThrowGesture);
			DrawSlider("Throw strength", "Makes fast throws stronger, up to this many times the speed of your hand. Gentle throws and drops always keep the real speed of your hand. 1.0 is real speed for every throw.", settings.m_GrenadeThrowStrength, 1.0f, 4.0f, 0.1f, "%.1fx");
			ImGui::EndDisabled();
			DrawCheckbox("Flashlight on head with melee", "While holding a melee weapon the flashlight follows your head instead of your hand.", settings.m_AttachFlashlightToHeadWhenMelee);
			ImGui::EndChild();
			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem("Scope"))
		{
			ImGui::BeginChild("Page", ImVec2(0.0f, -footerHeight));
			DrawCheckbox("Working scope", "Shows a magnified view in the scope of the sniper rifles when you look through it. Renders the game one more time while you aim, which costs performance.", settings.m_ScopeEnabled);

			ImGui::BeginDisabled(!settings.m_ScopeEnabled);
			DrawSlider("Magnification", "Zoom of the scope.", settings.m_ScopeMagnification, 1.5f, 10.0f, 0.5f, "%.1fx");
			DrawCheckbox("Debug: always show", "Shows the scope image whenever a sniper rifle is held, even when not looking through it, and writes what the scope is doing to VR\\scope_debug.txt.", settings.m_ScopeDebug);
			ImGui::EndDisabled();
			ImGui::EndChild();
			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem("HUD"))
		{
			ImGui::BeginChild("Page", ImVec2(0.0f, -footerHeight));
			DrawSlider("HUD distance", "Distance of the HUD from your eyes.", settings.m_HudDistance, 0.3f, 3.0f, 0.05f, "%.2f m");
			DrawSlider("HUD size", "Width of the HUD.", settings.m_HudSize, 0.3f, 3.0f, 0.05f, "%.2f m");
			DrawCheckbox("HUD always visible", "Keep the HUD visible instead of showing it only with the HUD button or with the controller held upright.", settings.m_HudAlwaysVisible);
			DrawCheckbox("Record performance", "Writes the time of each part of the frame to VR\\perf.csv once per second, to find what is slow. Leave it off when not needed.", settings.m_DiagnosticsEnabled);
			ImGui::EndChild();
			ImGui::EndTabItem();
		}

		ImGui::EndTabBar();
	}

	ImGui::Separator();
	ImGui::TextDisabled("Changes apply immediately and are saved to VR\\config.txt");

	const char *status = nullptr;
	ImVec4 statusColor;
	if (m_LastSaveFailed)
	{
		status = "Could not save config.txt";
		statusColor = ImVec4(0.95f, 0.35f, 0.30f, 1.0f);
	}
	else if (m_HasUnsavedChanges)
	{
		status = "Saving...";
		statusColor = style.Colors[ImGuiCol_TextDisabled];
	}
	else if (m_SavedOnce)
	{
		status = "Saved";
		statusColor = ImVec4(0.45f, 0.80f, 0.45f, 1.0f);
	}

	if (status)
	{
		ImGui::SameLine(ImGui::GetWindowWidth() - style.WindowPadding.x - ImGui::CalcTextSize(status).x);
		ImGui::TextColored(statusColor, "%s", status);
	}

	ImGui::End();
}

void SettingsMenu::DrawSlider(const char *label, const char *description, float &value, float min, float max, float step, const char *format)
{
	ImGui::PushID(label);

	const ImGuiStyle &style = ImGui::GetStyle();
	const float controlsX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x * 0.38f;
	const float buttonSize = ImGui::GetFrameHeight();

	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(label);
	ImGui::SameLine(controlsX);

	float newValue = value;
	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 2.0f * (buttonSize + style.ItemInnerSpacing.x));
	bool changed = ImGui::SliderFloat("##value", &newValue, min, max, format, ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput);

	// Fine adjustment, the laser is not very precise on the slider. Holding the button repeats it.
	ImGui::PushItemFlag(ImGuiItemFlags_ButtonRepeat, true);
	ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
	if (ImGui::Button("-", ImVec2(buttonSize, buttonSize)))
	{
		newValue = value - step;
		changed = true;
	}
	ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
	if (ImGui::Button("+", ImVec2(buttonSize, buttonSize)))
	{
		newValue = value + step;
		changed = true;
	}
	ImGui::PopItemFlag();

	if (changed)
	{
		newValue = std::clamp(std::round(newValue / step) * step, min, max);
		if (newValue != value)
		{
			value = newValue;
			MarkChanged();
		}
	}

	DrawDescription(description);
	ImGui::PopID();
}

void SettingsMenu::DrawCheckbox(const char *label, const char *description, bool &value)
{
	bool newValue = value;
	if (ImGui::Checkbox(label, &newValue))
	{
		value = newValue;
		MarkChanged();
	}

	DrawDescription(description);
}

void SettingsMenu::DrawDescription(const char *description)
{
	ImGui::PushFont(nullptr, kFontSize * 0.8f);
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	ImGui::PushTextWrapPos(0.0f);
	ImGui::TextUnformatted(description);
	ImGui::PopTextWrapPos();
	ImGui::PopStyleColor();
	ImGui::PopFont();
	ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.5f));
}

void SettingsMenu::MarkChanged()
{
	m_HasUnsavedChanges = true;
	m_LastChangeTime = std::chrono::steady_clock::now();
}

void SettingsMenu::SaveIfNeeded(bool force)
{
	if (!m_HasUnsavedChanges)
		return;

	if (!force)
	{
		if (std::chrono::steady_clock::now() - m_LastChangeTime < kSaveDelay)
			return;

		// Still dragging a slider or holding a +/- button
		if (ImGui::IsAnyItemActive())
			return;
	}

	m_LastSaveFailed = !m_VR->SaveConfigFile();
	m_SavedOnce = m_SavedOnce || !m_LastSaveFailed;
	m_HasUnsavedChanges = false;
	m_NeedsRedraw = true; // update the status line
}
