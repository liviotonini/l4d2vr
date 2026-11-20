#include "vr.h"
#include <Windows.h>
#include "sdk.h"
#include "game.h"
#include "hooks.h"
#include "trace.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <string>
#include <filesystem>
#include <thread>
#include <algorithm>
#include <vector>
#include <cstdarg>
#include <d3d9_vr.h>
#include "settings_menu.h"
#include "diagnostics.h"
#include "pose_relay.h"
#include "reload_logic.h"
#include "body_layout.h"

VR::VR(Game *game) 
{
    m_Game = game;

    char errorString[MAX_STR_LEN];

    vr::HmdError error = vr::VRInitError_None;
    m_System = vr::VR_Init(&error, vr::VRApplication_Scene);

    if (error != vr::VRInitError_None) 
    {
        snprintf(errorString, MAX_STR_LEN, "VR_Init failed: %s", vr::VR_GetVRInitErrorAsEnglishDescription(error));
        Game::errorMsg(errorString);
        return;
    }

    vr::EVRInitError peError = vr::VRInitError_None;

    if (!vr::VRCompositor())
    {
        Game::errorMsg("Compositor initialization failed.");
        return;
    }

    m_Input = vr::VRInput();
    m_System = vr::OpenVRInternal_ModuleContext().VRSystem();

    m_System->GetRecommendedRenderTargetSize(&m_RenderWidth, &m_RenderHeight);

    float l_left = 0.0f, l_right = 0.0f, l_top = 0.0f, l_bottom = 0.0f;
    m_System->GetProjectionRaw(vr::EVREye::Eye_Left, &l_left, &l_right, &l_top, &l_bottom);

    float r_left = 0.0f, r_right = 0.0f, r_top = 0.0f, r_bottom = 0.0f;
    m_System->GetProjectionRaw(vr::EVREye::Eye_Right, &r_left, &r_right, &r_top, &r_bottom);

    float tanHalfFov[2];

    tanHalfFov[0] = std::max({ -l_left, l_right, -r_left, r_right });
    tanHalfFov[1] = std::max({ -l_top, l_bottom, -r_top, r_bottom });

    m_TextureBounds[0].uMin = 0.5f + 0.5f * l_left / tanHalfFov[0];
    m_TextureBounds[0].uMax = 0.5f + 0.5f * l_right / tanHalfFov[0];
    m_TextureBounds[0].vMin = 0.5f - 0.5f * l_bottom / tanHalfFov[1];
    m_TextureBounds[0].vMax = 0.5f - 0.5f * l_top / tanHalfFov[1];

    m_TextureBounds[1].uMin = 0.5f + 0.5f * r_left / tanHalfFov[0];
    m_TextureBounds[1].uMax = 0.5f + 0.5f * r_right / tanHalfFov[0];
    m_TextureBounds[1].vMin = 0.5f - 0.5f * r_bottom / tanHalfFov[1];
    m_TextureBounds[1].vMax = 0.5f - 0.5f * r_top / tanHalfFov[1];

    m_Aspect = tanHalfFov[0] / tanHalfFov[1];
    m_Fov = 2.0f * atan(tanHalfFov[0]) * 360 / (3.14159265358979323846 * 2);

    InstallApplicationManifest("manifest.vrmanifest");
    SetActionManifest("action_manifest.json");

    std::thread configParser(&VR::WaitForConfigUpdate, this);
    configParser.detach();

    while (!g_D3DVR9) 
        Sleep(10);

    g_D3DVR9->GetBackBufferData(&m_VKBackBuffer);
    m_Overlay = vr::VROverlay();
    m_Overlay->CreateOverlay("MenuOverlayKey", "MenuOverlay", &m_MainMenuHandle);
    m_Overlay->CreateOverlay("HUDOverlayKey", "HUDOverlay", &m_HUDHandle);
    m_Overlay->SetOverlayInputMethod(m_MainMenuHandle, vr::VROverlayInputMethod_Mouse);
    m_Overlay->SetOverlayInputMethod(m_HUDHandle, vr::VROverlayInputMethod_Mouse);
    m_Overlay->SetOverlayFlag(m_MainMenuHandle, vr::VROverlayFlags_SendVRDiscreteScrollEvents, true);
    m_Overlay->SetOverlayFlag(m_HUDHandle, vr::VROverlayFlags_SendVRDiscreteScrollEvents, true);

    int windowWidth, windowHeight;
    m_Game->m_MaterialSystem->GetRenderContext()->GetWindowSize(windowWidth, windowHeight);

    const vr::HmdVector2_t mouseScaleHUD = {windowWidth, windowHeight};
    m_Overlay->SetOverlayMouseScale(m_HUDHandle, &mouseScaleHUD);

    const vr::HmdVector2_t mouseScaleMenu = {m_RenderWidth, m_RenderHeight};
    m_Overlay->SetOverlayMouseScale(m_MainMenuHandle, &mouseScaleMenu);

    CreateScopeOverlays();

    UpdatePosesAndActions();

    PatchLaserSight();
    PatchLaserSightSlots();
    PatchViewmodelPoses();

    // The dashboard overlay and its D3D11 device are created on the first Update() (render thread)
    m_SettingsMenu = new SettingsMenu(this);

    m_IsInitialized = true;
    m_IsVREnabled = true;
}

int VR::SetActionManifest(const char *fileName) 
{
    char currentDir[MAX_STR_LEN];
    GetCurrentDirectory(MAX_STR_LEN, currentDir);
    char path[MAX_STR_LEN];
    sprintf_s(path, MAX_STR_LEN, "%s\\VR\\SteamVRActionManifest\\%s", currentDir, fileName);

    if (m_Input->SetActionManifestPath(path) != vr::VRInputError_None) 
    {
        Game::errorMsg("SetActionManifestPath failed");
    }

    m_Input->GetActionHandle("/actions/main/in/ActivateVR", &m_ActionActivateVR);
    m_Input->GetActionHandle("/actions/main/in/Jump", &m_ActionJump);
    m_Input->GetActionHandle("/actions/main/in/PrimaryAttack", &m_ActionPrimaryAttack);
    m_Input->GetActionHandle("/actions/main/in/Reload", &m_ActionReload);
    m_Input->GetActionHandle("/actions/main/in/Use", &m_ActionUse);
    m_Input->GetActionHandle("/actions/main/in/Walk", &m_ActionWalk);
    m_Input->GetActionHandle("/actions/main/in/Turn", &m_ActionTurn);
    m_Input->GetActionHandle("/actions/main/in/SecondaryAttack", &m_ActionSecondaryAttack);
    m_Input->GetActionHandle("/actions/main/in/NextItem", &m_ActionNextItem);
    m_Input->GetActionHandle("/actions/main/in/PrevItem", &m_ActionPrevItem);
    m_Input->GetActionHandle("/actions/main/in/ResetPosition", &m_ActionResetPosition);
    m_Input->GetActionHandle("/actions/main/in/Crouch", &m_ActionCrouch);
    m_Input->GetActionHandle("/actions/main/in/Flashlight", &m_ActionFlashlight);
    m_Input->GetActionHandle("/actions/main/in/MenuSelect", &m_MenuSelect);
    m_Input->GetActionHandle("/actions/main/in/MenuBack", &m_MenuBack);
    m_Input->GetActionHandle("/actions/main/in/MenuUp", &m_MenuUp);
    m_Input->GetActionHandle("/actions/main/in/MenuDown", &m_MenuDown);
    m_Input->GetActionHandle("/actions/main/in/MenuLeft", &m_MenuLeft);
    m_Input->GetActionHandle("/actions/main/in/MenuRight", &m_MenuRight);
    m_Input->GetActionHandle("/actions/main/in/Spray", &m_Spray);
    m_Input->GetActionHandle("/actions/main/in/Scoreboard", &m_Scoreboard);
    m_Input->GetActionHandle("/actions/main/in/ShowHUD", &m_ShowHUD);
    m_Input->GetActionHandle("/actions/main/in/Pause", &m_Pause);
    m_Input->GetActionHandle("/actions/main/in/LeftHandGrab", &m_LeftHandGrab);
    m_Input->GetActionHandle("/actions/main/in/RightHandGrab", &m_RightHandGrab);
    m_Input->GetActionHandle("/actions/main/in/ToggleWalkSpeed", &m_ToggleWalkSpeed);
    m_Input->GetActionHandle("/actions/base/in/skeleton_lefthand", &m_ActionSkeletonLeft);
    m_Input->GetActionHandle("/actions/base/in/skeleton_righthand", &m_ActionSkeletonRight);

    m_Input->GetActionSetHandle("/actions/main", &m_ActionSet);
    m_ActiveActionSets[0] = {};
    m_ActiveActionSets[0].ulActionSet = m_ActionSet;
    m_ActiveActionSetCount = 1;

    // The vibration actions are in /actions/base, which has to be active for them to work
    if (m_Input->GetActionSetHandle("/actions/base", &m_ActionSetBase) == vr::VRInputError_None)
    {
        m_ActiveActionSets[1] = {};
        m_ActiveActionSets[1].ulActionSet = m_ActionSetBase;
        m_ActiveActionSetCount = 2;
    }

    m_Haptics.Init(m_Input, &m_LeftHanded);

    return 0;
}

void VR::InstallApplicationManifest(const char *fileName)
{
    char currentDir[MAX_STR_LEN];
    GetCurrentDirectory(MAX_STR_LEN, currentDir);
    char path[MAX_STR_LEN];
    sprintf_s(path, MAX_STR_LEN, "%s\\VR\\%s", currentDir, fileName);

    vr::VRApplications()->AddApplicationManifest(path);
}

void VR::Update()
{
    if (!m_IsInitialized || !m_Game->m_Initialized)
        return;

    // l4d2vr_dual_test grab: the use of the grab, from the console (there are no controllers in those tests)
    if (m_TestUsePressAt && GetTickCount64() >= m_TestUsePressAt)
    {
        const bool down = GetTickCount64() < m_GrabUsePressUntil;
        m_Game->ClientCmd_Unrestricted(down ? "+use" : "-use");
        if (!down)
            m_TestUsePressAt = 0;
    }

    if (m_IsVREnabled && g_D3DVR9)
    {
        // Prevents crashing at menu
        if (!m_Game->m_EngineClient->IsInGame())
        {
            IMatRenderContext *rndrContext = m_Game->m_MaterialSystem->GetRenderContext();
            rndrContext->SetRenderTarget(NULL);
            ClearModelCaches();
            ClearManualReloadCache();
            m_Game->m_PoseRelay->Reset();
            m_CreatedVRTextures = false; // Have to recreate textures otherwise some workshop maps won't render
        }
    }

    {
        Diagnostics::ScopedTimer timer(Diagnostics::Section_Submit);
        SubmitVRTextures();
    }

    UpdatePosesAndActions();

    {
        Diagnostics::ScopedTimer timer(Diagnostics::Section_Tracking);
        UpdateTracking();
        UpdateBodyItems();
    }
    // The game's laser sight goes with the right hand's weapon when that one isn't drawn (PatchLaserSightSlots)
    m_HideRightLaser = m_RightHandEmpty || m_RightMagazine || m_LeftUseActive;

    {
        Diagnostics::ScopedTimer timer(Diagnostics::Section_Input);
        if (m_Game->m_VguiSurface->IsCursorVisible())
            ProcessMenuInput();
        else
            ProcessInput();
    }

    if (m_SettingsMenu)
        m_SettingsMenu->Update();

    UpdateWristHud();

    // The engine replaces the crash handler while it starts, so it's installed again once the game is running
    if (!m_CrashHandlerReinstalled && m_Game->m_EngineClient->IsInGame())
    {
        m_CrashHandlerReinstalled = true;
        Diagnostics::InstallCrashHandler();
    }

    Diagnostics::g_PerfEnabled.store(m_DiagnosticsEnabled, std::memory_order_relaxed);
    Diagnostics::EndFrame();
}

void VR::CreateVRTextures()
{
    int windowWidth, windowHeight;
    m_Game->m_MaterialSystem->GetRenderContext()->GetWindowSize(windowWidth, windowHeight);

    m_Game->m_MaterialSystem->isGameRunning = false;
    m_Game->m_MaterialSystem->BeginRenderTargetAllocation();
    m_Game->m_MaterialSystem->isGameRunning = true;

    m_CreatingTextureID = Texture_LeftEye;
    m_LeftEyeTexture = m_Game->m_MaterialSystem->CreateNamedRenderTargetTextureEx("leftEye0", m_RenderWidth, m_RenderHeight, RT_SIZE_NO_CHANGE, m_Game->m_MaterialSystem->GetBackBufferFormat(), MATERIAL_RT_DEPTH_SHARED, TEXTUREFLAGS_NOMIP);
    
    m_CreatingTextureID = Texture_RightEye;
    m_RightEyeTexture = m_Game->m_MaterialSystem->CreateNamedRenderTargetTextureEx("rightEye0", m_RenderWidth, m_RenderHeight, RT_SIZE_NO_CHANGE, m_Game->m_MaterialSystem->GetBackBufferFormat(), MATERIAL_RT_DEPTH_SHARED, TEXTUREFLAGS_NOMIP);
    
    m_CreatingTextureID = Texture_HUD;
    m_HUDTexture = m_Game->m_MaterialSystem->CreateNamedRenderTargetTextureEx("vrHUD", windowWidth, windowHeight, RT_SIZE_NO_CHANGE, m_Game->m_MaterialSystem->GetBackBufferFormat(), MATERIAL_RT_DEPTH_SHARED, TEXTUREFLAGS_NOMIP);
    
    m_CreatingTextureID = Texture_Blank;
    m_BlankTexture = m_Game->m_MaterialSystem->CreateNamedRenderTargetTextureEx("blankTexture", 512, 512, RT_SIZE_NO_CHANGE, m_Game->m_MaterialSystem->GetBackBufferFormat(), MATERIAL_RT_DEPTH_SHARED, TEXTUREFLAGS_NOMIP);

    m_CreatingTextureID = Texture_Scope;
    m_ScopeTexture = m_Game->m_MaterialSystem->CreateNamedRenderTargetTextureEx("vrScope", kScopeTextureSize, kScopeTextureSize, RT_SIZE_NO_CHANGE, m_Game->m_MaterialSystem->GetBackBufferFormat(), MATERIAL_RT_DEPTH_SHARED, TEXTUREFLAGS_NOMIP);

    m_CreatingTextureID = Texture_None;

    m_Game->m_MaterialSystem->EndRenderTargetAllocation();

    m_CreatedVRTextures = true;
}

void VR::SubmitVRTextures()
{
    if (!m_RenderedNewFrame)
    {
        if (!m_BlankTexture)
            CreateVRTextures();

        if (!vr::VROverlay()->IsOverlayVisible(m_MainMenuHandle))
            RepositionOverlays();

        vr::VROverlay()->SetOverlayTexture(m_MainMenuHandle, &m_VKBackBuffer.m_VRTexture);
        SetMainMenuOverlayVisible(true);
        SetHudOverlayVisible(false);
        HideScopeOverlays();

        if (!m_Game->m_EngineClient->IsInGame())
        {
            vr::VRCompositor()->Submit(vr::Eye_Left, &m_VKBlankTexture.m_VRTexture, NULL, vr::Submit_Default);
            vr::VRCompositor()->Submit(vr::Eye_Right, &m_VKBlankTexture.m_VRTexture, NULL, vr::Submit_Default);
        }

        return;
    }
    SetMainMenuOverlayVisible(false);

    if (m_Game->m_VguiSurface->IsCursorVisible())
    {
        // We're in the pause menu
        SetHudOverlayVisible(true);
    }

    // Nobody sees the HUD when it's hidden
    if (m_HudOverlayVisible)
        vr::VROverlay()->SetOverlayTexture(m_HUDHandle, &m_VKHUD.m_VRTexture);

    SubmitScopeOverlays();

    vr::VRCompositor()->Submit(vr::Eye_Left, &m_VKLeftEye.m_VRTexture, &(m_TextureBounds)[0], vr::Submit_Default);
    vr::VRCompositor()->Submit(vr::Eye_Right, &m_VKRightEye.m_VRTexture, &(m_TextureBounds)[1], vr::Submit_Default);

    m_RenderedNewFrame = false;
}

void VR::GetPoseData(vr::TrackedDevicePose_t &poseRaw, TrackedDevicePoseData &poseOut)
{
    if (poseRaw.bPoseIsValid) 
    {
        vr::HmdMatrix34_t mat = poseRaw.mDeviceToAbsoluteTracking;
        Vector pos;
        Vector vel;
        QAngle ang;
        QAngle angvel;
        pos.x = -mat.m[2][3];
        pos.y = -mat.m[0][3];
        pos.z = mat.m[1][3];
        ang.x = asin(mat.m[1][2]) * (180.0 / 3.141592654);
        ang.y = atan2f(mat.m[0][2], mat.m[2][2]) * (180.0 / 3.141592654);
        ang.z = atan2f(-mat.m[1][0], mat.m[1][1]) * (180.0 / 3.141592654);
        vel.x = -poseRaw.vVelocity.v[2];
        vel.y = -poseRaw.vVelocity.v[0];
        vel.z = poseRaw.vVelocity.v[1];
        angvel.x = -poseRaw.vAngularVelocity.v[2] * (180.0 / 3.141592654);
        angvel.y = -poseRaw.vAngularVelocity.v[0] * (180.0 / 3.141592654);
        angvel.z = poseRaw.vAngularVelocity.v[1] * (180.0 / 3.141592654);

        poseOut.TrackedDevicePos = pos;
        poseOut.TrackedDeviceVel = vel;
        poseOut.TrackedDeviceAng = ang;
        poseOut.TrackedDeviceAngVel = angvel;
    }
}

void VR::RepositionOverlays()
{
    vr::TrackedDevicePose_t hmdPose = m_Poses[vr::k_unTrackedDeviceIndex_Hmd];
    vr::HmdMatrix34_t hmdMat = hmdPose.mDeviceToAbsoluteTracking;
    Vector hmdPosition = { hmdMat.m[0][3], hmdMat.m[1][3], hmdMat.m[2][3] };
    Vector hmdForward = { -hmdMat.m[0][2], 0, -hmdMat.m[2][2] };

    int windowWidth, windowHeight;
    m_Game->m_MaterialSystem->GetRenderContext()->GetWindowSize(windowWidth, windowHeight);

    vr::HmdMatrix34_t menuTransform = 
    {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 1.0f,
        0.0f, 0.0f, 1.0f, 1.0f
    };

    vr::ETrackingUniverseOrigin trackingOrigin = vr::VRCompositor()->GetTrackingSpace();

    // Reposition main menu overlay
    float renderWidth = m_VKBackBuffer.m_VulkanData.m_nWidth;
    float renderHeight = m_VKBackBuffer.m_VulkanData.m_nHeight;

    float widthRatio = windowWidth / renderWidth;
    float heightRatio = windowHeight / renderHeight;
    menuTransform.m[0][0] *= widthRatio;
    menuTransform.m[1][1] *= heightRatio;

    hmdForward[1] = 0;
    VectorNormalize(hmdForward);

    Vector menuDistance = hmdForward * 3;
    Vector menuNewPos = menuDistance + hmdPosition;

    menuTransform.m[0][3] = menuNewPos.x;
    menuTransform.m[1][3] = menuNewPos.y - 0.25;
    menuTransform.m[2][3] = menuNewPos.z;

    float xScale = menuTransform.m[0][0];
    float hmdRotationDegrees = atan2f(hmdMat.m[0][2], hmdMat.m[2][2]);

    menuTransform.m[0][0] *= cos(hmdRotationDegrees);
    menuTransform.m[0][2] = sin(hmdRotationDegrees);
    menuTransform.m[2][0] = -sin(hmdRotationDegrees) * xScale;
    menuTransform.m[2][2] *= cos(hmdRotationDegrees);

    vr::VROverlay()->SetOverlayTransformAbsolute(m_MainMenuHandle, trackingOrigin, &menuTransform);
    vr::VROverlay()->SetOverlayWidthInMeters(m_MainMenuHandle, 1.5 * (1.0 / heightRatio));

    // Reposition HUD overlay
    vr::HmdMatrix34_t hudTransform =
    {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f
    };

    Vector hudDistance = hmdForward * m_HudDistance;
    Vector hudNewPos = hudDistance + hmdPosition;

    hudTransform.m[0][3] = hudNewPos.x;
    hudTransform.m[1][3] = hudNewPos.y - 0.25;
    hudTransform.m[2][3] = hudNewPos.z;

    hudTransform.m[0][0] *= cos(hmdRotationDegrees);
    hudTransform.m[0][2] = sin(hmdRotationDegrees);
    hudTransform.m[2][0] = -sin(hmdRotationDegrees);
    hudTransform.m[2][2] *= cos(hmdRotationDegrees);

    vr::VROverlay()->SetOverlayTransformAbsolute(m_HUDHandle, trackingOrigin, &hudTransform);
    vr::VROverlay()->SetOverlayWidthInMeters(m_HUDHandle, m_HudSize);
}

void VR::GetPoses() 
{
    vr::TrackedDevicePose_t hmdPose = m_Poses[vr::k_unTrackedDeviceIndex_Hmd];

    vr::TrackedDeviceIndex_t leftControllerIndex = m_System->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand);
    vr::TrackedDeviceIndex_t rightControllerIndex = m_System->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand);

    if (m_LeftHanded)
        std::swap(leftControllerIndex, rightControllerIndex);

    m_RightControllerDeviceIndex = rightControllerIndex;
    m_LeftControllerDeviceIndex = leftControllerIndex;

    vr::TrackedDevicePose_t leftControllerPose = m_Poses[leftControllerIndex];
    vr::TrackedDevicePose_t rightControllerPose = m_Poses[rightControllerIndex];

    GetPoseData(hmdPose, m_HmdPose);
    GetPoseData(leftControllerPose, m_LeftControllerPose);
    GetPoseData(rightControllerPose, m_RightControllerPose);
}

void VR::UpdatePosesAndActions() 
{
    vr::VRCompositor()->WaitGetPoses(m_Poses, vr::k_unMaxTrackedDeviceCount, NULL, 0);
    m_Input->UpdateActionState(m_ActiveActionSets, sizeof(vr::VRActiveActionSet_t), m_ActiveActionSetCount);
}

void VR::GetViewParameters() 
{
    vr::HmdMatrix34_t eyeToHeadLeft = m_System->GetEyeToHeadTransform(vr::Eye_Left);
    vr::HmdMatrix34_t eyeToHeadRight = m_System->GetEyeToHeadTransform(vr::Eye_Right);
    m_EyeToHeadTransformPosLeft.x = eyeToHeadLeft.m[0][3];
    m_EyeToHeadTransformPosLeft.y = eyeToHeadLeft.m[1][3];
    m_EyeToHeadTransformPosLeft.z = eyeToHeadLeft.m[2][3];

    m_EyeToHeadTransformPosRight.x = eyeToHeadRight.m[0][3];
    m_EyeToHeadTransformPosRight.y = eyeToHeadRight.m[1][3];
    m_EyeToHeadTransformPosRight.z = eyeToHeadRight.m[2][3];
}

bool VR::PressedDigitalAction(vr::VRActionHandle_t &actionHandle, bool checkIfActionChanged)
{
    // A trigger held by l4d2vr_dual_test (there are no controllers in those tests)
    if (m_TestHeldAction == &actionHandle && GetTickCount64() < m_TestHeldUntil)
        return !checkIfActionChanged;

    vr::InputDigitalActionData_t digitalActionData;
    vr::EVRInputError result = m_Input->GetDigitalActionData(actionHandle, &digitalActionData, sizeof(digitalActionData), vr::k_ulInvalidInputValueHandle);
    
    if (result == vr::VRInputError_None)
    {
        if (checkIfActionChanged)
            return digitalActionData.bState && digitalActionData.bChanged;
        else
            return digitalActionData.bState;
    }

    return false;
}

bool VR::GetAnalogActionData(vr::VRActionHandle_t &actionHandle, vr::InputAnalogActionData_t &analogDataOut)
{
    vr::EVRInputError result = m_Input->GetAnalogActionData(actionHandle, &analogDataOut, sizeof(analogDataOut), vr::k_ulInvalidInputValueHandle);

    if (result == vr::VRInputError_None)
        return true;

    return false;
}

void VR::ProcessMenuInput()
{
    vr::VROverlayHandle_t currentOverlay = m_Game->m_EngineClient->IsInGame() ? m_HUDHandle : m_MainMenuHandle;
    
    // Check if left or right hand controller is pointing at the overlay
    const bool isHoveringOverlay = CheckOverlayIntersectionForController(currentOverlay, vr::TrackedControllerRole_LeftHand) ||
                                   CheckOverlayIntersectionForController(currentOverlay, vr::TrackedControllerRole_RightHand);

    // Overlays can't process action inputs if the laser is active, so
    // only activate laser if a controller is pointing at the overlay
    if (isHoveringOverlay)
    {
        vr::VROverlay()->SetOverlayFlag(currentOverlay, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, true);
        if (currentOverlay == m_HUDHandle)
            m_HudOverlayInteractive = true;

        int windowWidth, windowHeight;
        m_Game->m_MaterialSystem->GetRenderContext()->GetWindowSize(windowWidth, windowHeight);

        vr::VREvent_t vrEvent;
        while (vr::VROverlay()->PollNextOverlayEvent(currentOverlay, &vrEvent, sizeof(vrEvent)))
        {
            switch (vrEvent.eventType)
            {
            case vr::VREvent_MouseMove:
            {
                float laserX = vrEvent.data.mouse.x;
                float laserY = vrEvent.data.mouse.y;

                if (currentOverlay == m_HUDHandle)
                {
                    laserY = -laserY + windowHeight;
                }
                else // main menu (uses render sized texture)
                {
                    laserX = (laserX / m_RenderWidth) * windowWidth;
                    laserY = ((-laserY + m_RenderHeight) / m_RenderHeight) * windowHeight;
                }

                m_Game->m_VguiInput->SetCursorPos(laserX, laserY);
                break;
            }

            case vr::VREvent_MouseButtonDown:
                // Don't allow holding down the mouse down in the pause menu. The resume button can be clicked before
                // the MouseButtonUp event is polled, which causes issues with the overlay.
                if (currentOverlay == m_MainMenuHandle)
                    m_Game->m_VguiInput->InternalMousePressed(ButtonCode_t::MOUSE_LEFT);
                break;

            case vr::VREvent_MouseButtonUp:
                if (currentOverlay == m_HUDHandle)
                    m_Game->m_VguiInput->InternalMousePressed(ButtonCode_t::MOUSE_LEFT);
                m_Game->m_VguiInput->InternalMouseReleased(ButtonCode_t::MOUSE_LEFT);
                break;

            case vr::VREvent_ScrollDiscrete:
                m_Game->m_VguiInput->InternalMouseWheeled((int)vrEvent.data.scroll.ydelta);
                break;
            }
        }
    }
    else
    {
        vr::VROverlay()->SetOverlayFlag(currentOverlay, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, false);
        if (currentOverlay == m_HUDHandle)
            m_HudOverlayInteractive = false;

        if (PressedDigitalAction(m_MenuSelect, true))
        {
            m_Game->m_VguiInput->InternalKeyCodeTyped(ButtonCode_t::KEY_SPACE);
            m_Game->m_VguiInput->InternalKeyCodePressed(ButtonCode_t::KEY_SPACE);
            m_Game->m_VguiInput->InternalKeyCodeReleased(ButtonCode_t::KEY_SPACE);
        }
        if (PressedDigitalAction(m_MenuBack, true) || PressedDigitalAction(m_Pause, true))
        {
            m_Game->m_VguiInput->InternalKeyCodeTyped(ButtonCode_t::KEY_ESCAPE);
            m_Game->m_VguiInput->InternalKeyCodePressed(ButtonCode_t::KEY_ESCAPE);
            m_Game->m_VguiInput->InternalKeyCodeReleased(ButtonCode_t::KEY_ESCAPE);
        }
        if (PressedDigitalAction(m_MenuUp, true))
        {
            m_Game->m_VguiInput->InternalKeyCodeTyped(ButtonCode_t::KEY_UP);
            m_Game->m_VguiInput->InternalKeyCodePressed(ButtonCode_t::KEY_UP);
            m_Game->m_VguiInput->InternalKeyCodeReleased(ButtonCode_t::KEY_UP);
        }
        if (PressedDigitalAction(m_MenuDown, true))
        {
            m_Game->m_VguiInput->InternalKeyCodeTyped(ButtonCode_t::KEY_DOWN);
            m_Game->m_VguiInput->InternalKeyCodePressed(ButtonCode_t::KEY_DOWN);
            m_Game->m_VguiInput->InternalKeyCodeReleased(ButtonCode_t::KEY_DOWN);
        }
        if (PressedDigitalAction(m_MenuLeft, true))
        {
            m_Game->m_VguiInput->InternalKeyCodeTyped(ButtonCode_t::KEY_LEFT);
            m_Game->m_VguiInput->InternalKeyCodePressed(ButtonCode_t::KEY_LEFT);
            m_Game->m_VguiInput->InternalKeyCodeReleased(ButtonCode_t::KEY_LEFT);
        }
        if (PressedDigitalAction(m_MenuRight, true))
        {
            m_Game->m_VguiInput->InternalKeyCodeTyped(ButtonCode_t::KEY_RIGHT);
            m_Game->m_VguiInput->InternalKeyCodePressed(ButtonCode_t::KEY_RIGHT);
            m_Game->m_VguiInput->InternalKeyCodeReleased(ButtonCode_t::KEY_RIGHT);
        }
    }
}

void VR::ProcessInput()
{
    if (!m_IsVREnabled)
        return;

    SetHudOverlayInteractive(false);

    typedef std::chrono::duration<float, std::milli> duration;
    auto currentTime = std::chrono::steady_clock::now();
    duration elapsed = currentTime - m_PrevFrameTime;
    float deltaTime = elapsed.count();
    m_PrevFrameTime = currentTime;

    vr::InputAnalogActionData_t analogActionData;

    if (GetAnalogActionData(m_ActionTurn, analogActionData))
    {
        if (m_SnapTurning)
        {
            if (!m_PressedTurn && analogActionData.x > 0.5)
            {
                m_RotationOffset -= m_SnapTurnAngle;
                m_PressedTurn = true;
            }
            else if (!m_PressedTurn && analogActionData.x < -0.5)
            {
                m_RotationOffset += m_SnapTurnAngle;
                m_PressedTurn = true;
            }
            else if (analogActionData.x < 0.3 && analogActionData.x > -0.3)
                m_PressedTurn = false;
        }
        // Smooth turning
        else
        {
            float deadzone = 0.2;
            // smoother turning
            float xNormalized = (abs(analogActionData.x) - deadzone) / (1 - deadzone);
            if (analogActionData.x > deadzone)
            {
                m_RotationOffset -= m_TurnSpeed * deltaTime * xNormalized;
            }
            if (analogActionData.x < -deadzone)
            {
                m_RotationOffset += m_TurnSpeed * deltaTime * xNormalized;
            }
        }

        // Wrap from 0 to 360
        m_RotationOffset -= 360 * std::floor(m_RotationOffset / 360);
    }

    // Commands to the game only go out when a button changes (see SetGameButton)
    m_ResendGameButtons = GetTickCount64() - m_GameButtonsResent >= 1000;
    if (m_ResendGameButtons)
        m_GameButtonsResent = GetTickCount64();

    if (GetAnalogActionData(m_ActionWalk, analogActionData))
    {
        // With the speed following the thumbstick (BuildMoveCommand), the keys only tell the game which way it's pushed,
        // from the dead zone on: ladders are climbed by them
        const float keyThreshold = m_AnalogMovement ? kStickDeadZone : 0.5f;
        SetGameButton(GameButton_Forward, analogActionData.y > keyThreshold);
        SetGameButton(GameButton_Back, analogActionData.y < -keyThreshold);
        SetGameButton(GameButton_MoveRight, analogActionData.x > keyThreshold);
        SetGameButton(GameButton_MoveLeft, analogActionData.x < -keyThreshold);

        m_PushingThumbstick = fabsf(analogActionData.y) > keyThreshold || fabsf(analogActionData.x) > keyThreshold;

        std::lock_guard<std::mutex> lock(m_MovementMutex);
        m_MovementInputs.stickX = analogActionData.x;
        m_MovementInputs.stickY = analogActionData.y;
        m_MovementInputs.stickMs = GetTickCount64();
        m_MovementInputs.walking = isWalkingSlowly;
        m_MovementInputs.ducking = m_GameButtonState[GameButton_Duck] == 1;
    }
    else
    {
        SetGameButton(GameButton_Forward, false);
        SetGameButton(GameButton_Back, false);
        SetGameButton(GameButton_MoveRight, false);
        SetGameButton(GameButton_MoveLeft, false);
        m_PushingThumbstick = false;
    }

    UpdateGrenadeThrow();

    // While the left hand's item is used, its trigger is the attack, once the item is out (dual wield). Not for the kit on
    // another player, which is the secondary attack (KitHealsOther).
    const bool triggerHeld = PressedDigitalAction(m_LeftUseActive ? m_ActionSecondaryAttack : m_ActionPrimaryAttack);
    bool attackTrigger = m_LeftUseActive ? triggerHeld && m_LeftUseWasActive && m_ActiveWeaponId == m_LeftItemId && m_KitUse[0] != KitUse_Other : triggerHeld;
    if (!UpdateItemUse())
        attackTrigger = false;
    // The right hand empty: its unseen weapon does nothing (EmptyRightHand). Neither while it holds the left hand's gun's
    // magazine, bolt or pump, which its weapon is put aside for (ReloadNeedsFreeHand off)
    const bool rightHandEmpty = m_RightHandEmpty && !m_LeftUseActive;
    if (rightHandEmpty || (m_RightWorksLeftGun && !m_LeftUseActive))
        attackTrigger = false;
    SetGameButton(GameButton_Attack, GetAttackButton(attackTrigger));
    SetGameButton(GameButton_Jump, PressedDigitalAction(m_ActionJump));
    // A grab next to something in the world presses use for a moment (TryGrabWorld)
    const unsigned long long useTick = GetTickCount64();
    const bool grabbingUse = useTick >= m_GrabUsePressAt && useTick < m_GrabUsePressUntil;
    SetGameButton(GameButton_Use, PressedDigitalAction(m_ActionUse) || grabbingUse);

    SetGameButton(GameButton_Reload, !rightHandEmpty && ((PressedDigitalAction(m_ActionReload) && !ManualReloadOwnsReloadButton()) || isReloading));
    isReloading = false;

    //if (PressedDigitalAction(m_ActionSecondaryAttack) || (m_IsTrackingManualPush && VectorLength(m_LeftControllerPose.TrackedDeviceVel) > 5.0))
    // The swing gestures don't shove while throwing a grenade with the arm (both arms move fast, and holding the shove
    // button stops the game from throwing). Neither does the shove button press that cancelled a throw.
    // A melee weapon in the left hand hits instead of shoving, and its item being used doesn't shove (dual wield)
    const bool leftShoves = !ManualReloadUsesLeftHand() && !LeftHandHoldsMelee() && !m_LeftUseActive;
    // Nor does the right arm while it works the left hand's gun (a quick pump is a fast swing)
    const bool rightShoves = (rightHandEmpty || !m_Game->m_IsMeleeWeaponActive) && GetTickCount64() - m_RightHandHandledLeftGunMs > kNoShoveAfterLeftGunMs;
    const bool shove = m_IsTrackingManualPush && !IsGrenadeThrowInProgress() && ((VectorLength(m_LeftControllerPose.TrackedDeviceVel) > 4.0 && leftShoves) || (rightShoves && VectorLength(m_RightControllerPose.TrackedDeviceVel) > 4.0));
    // The kit healing another player, and the right hand's pills given the game's way (a host without the mod), are the
    // secondary attack too (UpdateGiving, KitHealsOther)
    const bool secondaryForItem = KitHealsOther() || GetTickCount64() < m_GivePressUntil;
    SetGameButton(GameButton_Attack2, shove || secondaryForItem);
    // The game's shove animation isn't shown, the arm already swung (ApplyThrowPose)
    if (shove && !m_ShoveDown)
        m_ShoveHoldUntil = GetTickCount64() + kShoveAnimationMs;
    m_ShoveDown = shove;

    if (PressedDigitalAction(m_ActionPrevItem, true))
    {
        m_Game->ClientCmd_Unrestricted("invprev");
    }
    else if (PressedDigitalAction(m_ActionNextItem, true))
    {
        m_Game->ClientCmd_Unrestricted("invnext");
    }

    if (PressedDigitalAction(m_ActionResetPosition, true))
    {
        ResetPosition();
    }

    m_IsPlayerCrouched = (m_ActivateCrouchPhisically && m_HmdPosLocalInWorld.z <= (27.0f * m_PlayerHeightInMeters));
    if (m_IsPlayerCrouched)
    {
        if (isCrouching == false)
        {
            isCrouching = true;
            hasCrouchAnimationEnded = false;
            m_CrouchAnimDeltaTime = 0;
            cameraAnchorZBeforePhysicalCrouch = m_CameraAnchor.z;
        }
    }
    else if (isCrouching)
    {
        isCrouching = false;
        hasCrouchAnimationEnded = false;
        m_CrouchAnimDeltaTime = 0;
        cameraAnchorZBeforePhysicalCrouch = m_CameraAnchor.z;
        isSendingCrouchCommand = false;
    }

    if (PressedDigitalAction(m_ActionCrouch))
    {
        if (m_UseCrouchButtonAsToggle)
        {
            if (isPressingCrouchButton == false)
            {
                isPressingCrouchButton = true;
                isSendingCrouchCommand = !isSendingCrouchCommand;
            }
        }
        else
        {
            isSendingCrouchCommand = true;
        }
    }
    else
    {
        if (m_UseCrouchButtonAsToggle)
        {
            isPressingCrouchButton = false;
        }
        else
        {
            isSendingCrouchCommand = false;
        }
    }

    SetGameButton(GameButton_Duck, isSendingCrouchCommand || m_IsPlayerCrouched);

    // The thumbstick clicks: a short click turns the flashlight on or off (right) or changes the walking speed (left),
    // once let go; held for 3 seconds it drops what that hand holds instead (UpdateStickHold)
    if (UpdateStickHold(m_RightStickHold, PressedDigitalAction(m_ActionFlashlight), true))
    {
        m_Game->ClientCmd_Unrestricted("impulse 100");
    }

    if (PressedDigitalAction(m_Spray, true))
    {
        m_Game->ClientCmd_Unrestricted("impulse 201");
    }

    if (UpdateStickHold(m_LeftStickHold, PressedDigitalAction(m_ToggleWalkSpeed), false))
    {
        isWalkingSlowly = !isWalkingSlowly;
        m_Game->ClientCmd_Unrestricted(isWalkingSlowly ? "+speed" : "-speed");
    }
    
    bool isControllerVertical = m_RightControllerAngAbs.x > 60 || m_RightControllerAngAbs.x < -45;
    if ((PressedDigitalAction(m_ShowHUD) || PressedDigitalAction(m_Scoreboard) || isControllerVertical || m_HudAlwaysVisible)
        && m_RenderedHud)
    {
        if (!m_HudOverlayVisible || m_HudAlwaysVisible)
            RepositionOverlays();

        SetGameButton(GameButton_ShowScores, PressedDigitalAction(m_Scoreboard));
        SetHudOverlayVisible(true);
    }
    else
    {
        SetHudOverlayVisible(false);
    }
    m_RenderedHud = false;

    if (PressedDigitalAction(m_Pause, true))
    {
        m_Game->ClientCmd_Unrestricted("gameui_activate");
        RepositionOverlays();
    }
}

VMatrix VR::VMatrixFromHmdMatrix(const vr::HmdMatrix34_t &hmdMat)
{
    // VMatrix has a different implicit coordinate system than HmdMatrix34_t, but this function does not convert between them
    VMatrix vMat(
        hmdMat.m[0][0], hmdMat.m[1][0], hmdMat.m[2][0], 0.0f,
        hmdMat.m[0][1], hmdMat.m[1][1], hmdMat.m[2][1], 0.0f,
        hmdMat.m[0][2], hmdMat.m[1][2], hmdMat.m[2][2], 0.0f,
        hmdMat.m[0][3], hmdMat.m[1][3], hmdMat.m[2][3], 1.0f
    );

    return vMat;
}

vr::HmdMatrix34_t VR::VMatrixToHmdMatrix(const VMatrix &vMat)
{
    vr::HmdMatrix34_t hmdMat = {0};

    hmdMat.m[0][0] = vMat.m[0][0];
    hmdMat.m[1][0] = vMat.m[0][1];
    hmdMat.m[2][0] = vMat.m[0][2];

    hmdMat.m[0][1] = vMat.m[1][0];
    hmdMat.m[1][1] = vMat.m[1][1];
    hmdMat.m[2][1] = vMat.m[1][2];

    hmdMat.m[0][2] = vMat.m[2][0];
    hmdMat.m[1][2] = vMat.m[2][1];
    hmdMat.m[2][2] = vMat.m[2][2];

    hmdMat.m[0][3] = vMat.m[3][0];
    hmdMat.m[1][3] = vMat.m[3][1];
    hmdMat.m[2][3] = vMat.m[3][2];

    return hmdMat;
}

vr::HmdMatrix34_t VR::GetControllerTipMatrix(vr::ETrackedControllerRole controllerRole)
{
    vr::VRInputValueHandle_t inputValue = vr::k_ulInvalidInputValueHandle;

    if (controllerRole == vr::TrackedControllerRole_RightHand)
    {
        m_Input->GetInputSourceHandle("/user/hand/right", &inputValue);
    }
    else if (controllerRole == vr::TrackedControllerRole_LeftHand)
    {
        m_Input->GetInputSourceHandle("/user/hand/left", &inputValue);
    }

    if (inputValue != vr::k_ulInvalidInputValueHandle)
    {
        char buffer[vr::k_unMaxPropertyStringSize];

        m_System->GetStringTrackedDeviceProperty(vr::VRSystem()->GetTrackedDeviceIndexForControllerRole(controllerRole), vr::Prop_RenderModelName_String, 
                                                 buffer, vr::k_unMaxPropertyStringSize);

        vr::RenderModel_ControllerMode_State_t controllerState = {0};
        vr::RenderModel_ComponentState_t componentState = {0};

        if (vr::VRRenderModels()->GetComponentStateForDevicePath(buffer, vr::k_pch_Controller_Component_Tip, inputValue, &controllerState, &componentState))
        {
            return componentState.mTrackingToComponentLocal;
        }
    }

    // Not a hand controller role or tip lookup failed, return identity
    const vr::HmdMatrix34_t identity = 
    {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f
    };

    return identity;
}

bool VR::CheckOverlayIntersectionForController(vr::VROverlayHandle_t overlayHandle, vr::ETrackedControllerRole controllerRole)
{
    vr::TrackedDeviceIndex_t deviceIndex = m_System->GetTrackedDeviceIndexForControllerRole(controllerRole);

    if (deviceIndex == vr::k_unTrackedDeviceIndexInvalid)
        return false;

    vr::TrackedDevicePose_t &controllerPose = m_Poses[deviceIndex];

    if (!controllerPose.bPoseIsValid)
        return false;

    VMatrix controllerVMatrix = VMatrixFromHmdMatrix(controllerPose.mDeviceToAbsoluteTracking);
    VMatrix tipVMatrix        = VMatrixFromHmdMatrix(GetControllerTipMatrix(controllerRole));
    tipVMatrix.MatrixMul(controllerVMatrix, controllerVMatrix);

    vr::VROverlayIntersectionParams_t  params  = {0};
    vr::VROverlayIntersectionResults_t results = {0};

    params.eOrigin    = vr::VRCompositor()->GetTrackingSpace();
    params.vSource    = { controllerVMatrix.m[3][0],  controllerVMatrix.m[3][1],  controllerVMatrix.m[3][2]};
    params.vDirection = {-controllerVMatrix.m[2][0], -controllerVMatrix.m[2][1], -controllerVMatrix.m[2][2]};

    return m_Overlay->ComputeOverlayIntersection(overlayHandle, &params, &results);
}

QAngle VR::GetRightControllerAbsAngle()
{
    return m_RightControllerAngAbs;
}

static_assert(Aim::Pistol == C_WeaponCSBase::PISTOL && Aim::Uzi == C_WeaponCSBase::UZI && Aim::PumpShotgun == C_WeaponCSBase::PUMPSHOTGUN
    && Aim::AutoShotgun == C_WeaponCSBase::AUTOSHOTGUN && Aim::Rifle == C_WeaponCSBase::M16A1 && Aim::HuntingRifle == C_WeaponCSBase::HUNTING_RIFLE
    && Aim::Mac10 == C_WeaponCSBase::MAC10 && Aim::ChromeShotgun == C_WeaponCSBase::SHOTGUN_CHROME && Aim::Scar == C_WeaponCSBase::SCAR
    && Aim::MilitarySniper == C_WeaponCSBase::SNIPER_MILITARY && Aim::Spas == C_WeaponCSBase::SPAS && Aim::GrenadeLauncher == C_WeaponCSBase::GRENADE_LAUNCHER
    && Aim::Ak47 == C_WeaponCSBase::AK47 && Aim::Magnum == C_WeaponCSBase::MAGNUM && Aim::Mp5 == C_WeaponCSBase::MP5 && Aim::Sg552 == C_WeaponCSBase::SG552
    && Aim::Awp == C_WeaponCSBase::AWP && Aim::Scout == C_WeaponCSBase::SCOUT && Aim::M60 == C_WeaponCSBase::M60,
    "the weapon IDs of aim_logic.h are the game's");

// The right hand's weapon is a long gun (aim_logic.h) that the left hand doesn't hold too: it spreads and kicks more
bool VR::AimsLongGunWithOneHand() const
{
    return m_LongGunsNeedBothHands && !isHoldingWeapon2Handed && Aim::NeedsBothHands(m_ActiveWeaponId);
}

// A shot of the right hand's gun as this client shows it (Hooks::dClientFireTerrorBullets): with one hand on a long gun
// it kicks the aim. The game runs a usercmd again when the server corrects it, with the same seed: once per seed.
void VR::OnRightGunShot(int weaponId, int seed, bool oneHanded, float gunSpread, float spread)
{
    for (int i = 0; i < 8; ++i)
    {
        if (m_RightShotSeeds[i] == seed)
            return;
    }
    m_RightShotSeeds[m_RightShotSeedNext] = seed;
    m_RightShotSeedNext = (m_RightShotSeedNext + 1) % 8;

    const Aim::GunScript *gun = Aim::FindGun(weaponId);
    if (gun && gun->longGun && oneHanded)
        m_AimKick.Shot(*gun, seed);
    if (GetTickCount64() < m_AimLogUntil)
        Diagnostics::Log("aim: a shot of weapon %d with %s, the gun's spread %.2f, fired with %.2f, the aim kicked %.1f up and %.1f left",
            weaponId, oneHanded && gun && gun->longGun ? "one hand" : "both hands or a short gun", gunSpread, spread, m_AimKick.pitch, m_AimKick.yaw);
}

Vector VR::GetRightControllerAbsPos()
{
    return m_RightControllerPosAbs;
}

Vector VR::GetRecommendedViewmodelAbsPos()
{
    Vector viewmodelPos = GetRightControllerAbsPos();
    viewmodelPos -= m_ViewmodelForward * m_ViewmodelPosOffset.x;
    viewmodelPos -= m_ViewmodelRight * m_ViewmodelPosOffset.y;
    viewmodelPos -= m_ViewmodelUp * m_ViewmodelPosOffset.z;

    return viewmodelPos;
}

QAngle VR::GetRecommendedViewmodelAbsAngle()
{
    QAngle result{};

    QAngle::VectorAngles(m_ViewmodelForward, m_ViewmodelUp, result);

    return result;
}

void printFloat(const char* label, float value) {
    char buffer[100];
    snprintf(buffer, sizeof(buffer), "%s: %f", label, value);  // Format the label and float value into a string

    // Print to Output window
    OutputDebugString(buffer);
}

void writeToFile(const std::string& label) {
    std::ofstream outFile("output.txt", std::ios::app);  // Open the file in append mode
    if (outFile.is_open()) {
        outFile << label << std::endl;  // Write the label and float to the file
        outFile.close();  // Close the file after writing
    }
    else {
        std::cerr << "Error opening file!" << std::endl;
    }
}

void writeToFile(const std::string& label, float value) {
    std::ofstream outFile("output.txt", std::ios::app);  // Open the file in append mode
    if (outFile.is_open()) {
        outFile << label << ": " << value << std::endl;  // Write the label and float to the file
        outFile.close();  // Close the file after writing
    }
    else {
        std::cerr << "Error opening file!" << std::endl;
    }
}

void writeToFile(const std::string& label, Vector vector) {
    std::ofstream outFile("output.txt", std::ios::app);  // Open the file in append mode
    if (outFile.is_open()) {
        outFile << label << " => x: " << vector.x << "; y: " << vector.y << "; z: " << vector.z << std::endl;  // Write the label and float to the file
        outFile.close();  // Close the file after writing
    }
    else {
        std::cerr << "Error opening file!" << std::endl;
    }
}

void writeToFile(const std::string& label, QAngle angle) {
    std::ofstream outFile("output.txt", std::ios::app);  // Open the file in append mode
    if (outFile.is_open()) {
        outFile << label << " => x: " << angle.x << "; y: " << angle.y << "; z: " << angle.z << std::endl;  // Write the label and float to the file
        outFile.close();  // Close the file after writing
    }
    else {
        std::cerr << "Error opening file!" << std::endl;
    }
}


// Builds an orthonormal frame whose forward points from the right hand to the left hand.
// The roll comes from the right controller: its right vector is taken from both the controller's up and right vectors
// projected onto the plane perpendicular to forward. Each term only degenerates when forward is parallel to
// controllerUp or controllerRight respectively, so the sum stays stable for any hands position.
static void GetTwoHandedFrame(const Vector &handsDirection, const Vector &controllerRight, const Vector &controllerUp,
    Vector &forward, Vector &right, Vector &up)
{
    forward = handsDirection;
    VectorNormalize(forward);

    Vector rightFromUp;
    CrossProduct(forward, controllerUp, rightFromUp);
    Vector rightProjected = controllerRight - forward * DotProduct(forward, controllerRight);

    right = rightFromUp + rightProjected;
    if (VectorLength(right) < 0.001f)
        right = rightProjected;
    VectorNormalize(right);

    CrossProduct(right, forward, up);
}

// Expresses a world direction in the coordinates of the frame (forward, right, up), and back.
static Vector WorldToFrame(const Vector &v, const Vector &forward, const Vector &right, const Vector &up)
{
    return Vector(DotProduct(v, forward), DotProduct(v, right), DotProduct(v, up));
}

static Vector FrameToWorld(const Vector &local, const Vector &forward, const Vector &right, const Vector &up)
{
    return forward * local.x + right * local.y + up * local.z;
}

// Checks code bytes at address. -1 in the pattern matches any byte.
static bool CodeBytesMatch(uintptr_t address, std::initializer_list<int> pattern)
{
    const uint8_t *bytes = (const uint8_t *)address;
    int i = 0;
    for (int expected : pattern)
    {
        if (expected != -1 && bytes[i] != expected)
            return false;
        ++i;
    }
    return true;
}

static bool PatchCodeBytes(uintptr_t address, const uint8_t *bytes, size_t size)
{
    DWORD oldProtect;
    if (!VirtualProtect((LPVOID)address, size, PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;

    memcpy((void *)address, bytes, size);

    VirtualProtect((LPVOID)address, size, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), (LPVOID)address, size);
    return true;
}

// The laser sights are drawn on the primary weapons only: C_TerrorPlayer's update (around Offsets::ClientLaserSightCreate)
// gives up, in first and in third person, when the weapon's slot (a virtual returning its script's slot) isn't 0. A pistol
// takes a laser sight too (Hooks::dPlayerAddUpgrade): those two checks are taken out, so any gun with one shows it.
// Giving up removes the player's laser effects, so the first person one gives up instead while the right hand's weapon
// isn't drawn (m_HideRightLaser): put away by the empty hand, it's still the game's active weapon, with its laser.
void VR::PatchLaserSightSlots()
{
    const uintptr_t create = m_Game->m_Offsets->ClientLaserSightCreate.address;
    if (!create)
        return;
    // mov eax, [edx + slot virtual]; call eax; test eax, eax; jne give up
    const uintptr_t firstPerson = create - 0x6B, thirdPerson = create + 0x9EC;
    const bool codeMatches = CodeBytesMatch(firstPerson, { 0x8B, 0x82, 0xFC, 0x04, 0x00, 0x00, 0xFF, 0xD0, 0x85, 0xC0, 0x0F, 0x85 })
        && CodeBytesMatch(thirdPerson, { 0x8B, 0x82, 0xFC, 0x04, 0x00, 0x00, 0xFF, 0xD0, 0x85, 0xC0, 0x0F, 0x85 });
    if (!codeMatches)
    {
        Diagnostics::Log("upgrades: the laser sights' slot check wasn't found in this game version, pistols show none, and a weapon put away keeps its laser");
        return;
    }
    // First person: "movzx eax, byte ptr [&m_HideRightLaser]; test eax, eax; nop", the jne kept. Past it, eax is 0
    // as before, and the next instructions load ecx and edx again.
    uint8_t firstPersonPatch[10] = { 0x0F, 0xB6, 0x05, 0, 0, 0, 0, 0x85, 0xC0, 0x90 };
    const uint32_t hideAddress = (uint32_t)(uintptr_t)&m_HideRightLaser;
    memcpy(&firstPersonPatch[3], &hideAddress, 4);
    PatchCodeBytes(firstPerson, firstPersonPatch, sizeof(firstPersonPatch));
    const uint8_t nops[6] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
    PatchCodeBytes(thirdPerson + 10, nops, sizeof(nops));
}

// L4D2's first person laser sight (C_TerrorPlayer, particle "weapon_laser_sight_FP") traces the laser from a blend
// of the eye position/angles and the viewmodel's "flashlight" attachment. Whenever the weapon points roughly where the
// player looks, it's fully eye based (so the dot sits on the crosshair in the regular game), otherwise the dot is hidden.
// In VR the eyes are the HMD, so the laser came out of the head instead of the weapon.
// This patches that function so the laser trace starts at the weapon attachment and uses the weapon aim angles
// (m_RightControllerAngAbs, the same angles used for the bullets).
void VR::PatchLaserSight()
{
    uintptr_t attachmentAnglesAddress = m_Game->m_Offsets->LaserSightAttachmentAngles.address;
    if (!attachmentAnglesAddress)
        return;

    uintptr_t eyeAnglesCallAddress = attachmentAnglesAddress + 0xE3;
    uintptr_t blendCompareAddress = attachmentAnglesAddress + 0x13C;
    uintptr_t blendDoneAddress = attachmentAnglesAddress + 0x190;

    // Verify all original instructions before changing anything
    bool codeMatches =
        // lea eax, [ebp-0x1F0] (attachment angles); push eax; call AngleVectors
        CodeBytesMatch(attachmentAnglesAddress, { 0x8D, 0x85, 0x10, 0xFE, 0xFF, 0xFF, 0x50, 0xE8 })
        // push edx; mov ecx, esi; mov byte ptr [ebp-0xDD], 0; call C_BasePlayer::EyeAngles; push eax; call AngleVectors
        && CodeBytesMatch(eyeAnglesCallAddress - 10, { 0x52, 0x8B, 0xCE, 0xC6, 0x85, 0x23, 0xFF, 0xFF, 0xFF, 0x00, 0xE8, -1, -1, -1, -1, 0x50, 0xE8 })
        // comiss xmm0, xmm3; movaps xmm2, xmm0; movss [ebp-0x110], xmm2; jbe +9; mov byte ptr [ebp-0xDD], 1; jmp blendDone
        && CodeBytesMatch(blendCompareAddress, { 0x0F, 0x2F, 0xC3, 0x0F, 0x28, 0xD0, 0xF3, 0x0F, 0x11, 0x95, 0xF0, 0xFE, 0xFF, 0xFF, 0x76, 0x09, 0xC6, 0x85, 0x23, 0xFF, 0xFF, 0xFF, 0x01, 0xEB, 0x3B })
        // blendDone: mov eax, [esi+4]
        && CodeBytesMatch(blendDoneAddress, { 0x8B, 0x46, 0x04 });

    if (!codeMatches)
        return;

    uint32_t aimAnglesAddress = (uint32_t)(uintptr_t)&m_RightControllerAngAbs;

    // 1. Weapon direction: "mov eax, &m_RightControllerAngAbs; nop" instead of the attachment angles
    uint8_t weaponAnglesPatch[6] = { 0xB8, 0, 0, 0, 0, 0x90 };
    memcpy(&weaponAnglesPatch[1], &aimAnglesAddress, 4);

    // 2. Eye direction: "mov eax, &m_RightControllerAngAbs" instead of calling EyeAngles(), so the dot isn't hidden
    //    or pulled towards where the HMD is looking
    uint8_t eyeAnglesPatch[5] = { 0xB8, 0, 0, 0, 0 };
    memcpy(&eyeAnglesPatch[1], &aimAnglesAddress, 4);

    // 3. Eye/weapon blend factor = 0, so the trace starts at the weapon attachment instead of the eye position:
    //    xorps xmm2, xmm2; movss [ebp-0x110], xmm2; jmp blendDone
    uint8_t blendPatch[13] = { 0x0F, 0x57, 0xD2, 0xF3, 0x0F, 0x11, 0x95, 0xF0, 0xFE, 0xFF, 0xFF, 0xEB, 0x00 };
    blendPatch[12] = (uint8_t)(blendDoneAddress - (blendCompareAddress + sizeof(blendPatch)));

    PatchCodeBytes(attachmentAnglesAddress, weaponAnglesPatch, sizeof(weaponAnglesPatch));
    PatchCodeBytes(eyeAnglesCallAddress, eyeAnglesPatch, sizeof(eyeAnglesPatch));
    PatchCodeBytes(blendCompareAddress, blendPatch, sizeof(blendPatch));
}

// C_TerrorViewModel::CalcViewModelView leans the gun with two pose parameters of the viewmodel: "ver_aims" from the
// eyes' pitch (the look_poses layer: up to 3 degrees up or down and 4 to the side) and "move_x" from the player's speed
// (the run pose: it lowers the MP5 5 degrees and the SG552 8). In VR the eyes are the headset and the gun is in the hand,
// so the gun pointed away from where it shoots, by how the head looked and how fast the player went. The game is made to
// see the eyes level and the player standing still, where both are in the middle of their range (idle, look_mid).
void VR::PatchViewmodelPoses()
{
    const uintptr_t speed = m_Game->m_Offsets->ViewmodelMoveSpeed.address;
    // sqrtss xmm0, xmm0 -> xorps xmm0, xmm0; nop
    if (speed && CodeBytesMatch(speed, { 0xF3, 0x0F, 0x51, 0xC0 }))
    {
        const uint8_t patch[4] = { 0x0F, 0x57, 0xC0, 0x90 };
        PatchCodeBytes(speed, patch, sizeof(patch));
    }
    else
        Diagnostics::Log("viewmodel: the run pose isn't turned off in this game version, the gun leans while moving");

    const uintptr_t pitch = m_Game->m_Offsets->ViewmodelAimPitch.address;
    // fld dword ptr [eax] -> fldz
    if (pitch && CodeBytesMatch(pitch, { 0xD9, 0x00 }))
    {
        const uint8_t patch[2] = { 0xD9, 0xEE };
        PatchCodeBytes(pitch, patch, sizeof(patch));
    }
    else
        Diagnostics::Log("viewmodel: the look poses aren't turned off in this game version, the gun leans with the head");
}

// ---------------------------------------------------------------------------------------------------------------
// Scope
//
// The game's scope is only a 2D HUD overlay (black in VR), so the sniper rifles get a "render to texture" scope:
// while an eye is behind the eyepiece, Hooks::dRenderView renders the game a third time from the scope with a
// narrow field of view into m_ScopeTexture. The texture is shown on an overlay placed at the eyepiece of the
// weapon model, with a second overlay on top (black ring + reticle) that makes it round.
// The overlays are positioned relative to the controller, so SteamVR keeps them on the weapon.
// ---------------------------------------------------------------------------------------------------------------

namespace
{
    struct ScopeInfo
    {
        int weaponId;
        // Center of the eyepiece in the frame of the viewmodel's "flashlight" attachment (forward, left, up),
        // measured from the models (models/v_models/v_huntingrifle, v_sniper_military, v_snip_awp, v_snip_scout)
        Vector eyepiece;
        // Radius of the visible image, game units
        float radius;
    };

    const ScopeInfo kScopes[] =
    {
        { C_WeaponCSBase::HUNTING_RIFLE,   { -13.5f,  1.28f,  2.14f }, 0.95f },
        { C_WeaponCSBase::SNIPER_MILITARY, { -31.0f,  5.79f,  0.0f  }, 0.80f },
        { C_WeaponCSBase::AWP,             { -21.05f, 3.81f, -1.17f }, 1.30f },
        { C_WeaponCSBase::SCOUT,           { -18.0f,  3.43f,  0.02f }, 0.90f },
        { C_WeaponCSBase::SG552,           { -16.8f,  0.0f,   4.65f }, 0.75f },
    };

    // Length of the scope, and how close the hand has to be to it for its trigger to change the zoom
    constexpr float kScopeBodyLength = 9.0f;  // game units
    constexpr float kScopeZoomReach = 0.11f;  // meters

    // Field of view seen through the eyepiece at 1x magnification
    constexpr float kScopeReferenceFov = 40.0f;

    // The scope image fades in when an eye gets behind the eyepiece and roughly in line with it
    constexpr float kScopeFadeStartDistance = 0.30f; // meters
    constexpr float kScopeFullDistance = 0.20f;
    constexpr float kScopeFadeStartAngle = 25.0f;    // degrees off the scope axis
    constexpr float kScopeFullAngle = 12.0f;

    // The mask overlay is bigger than the image so its black ring covers the corners of the square image
    constexpr float kScopeMaskScale = 1.4142f;
    constexpr int kScopeMaskSize = 512;

    float SmoothStep(float edge0, float edge1, float x)
    {
        float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    const ScopeInfo *FindScope(int weaponId)
    {
        for (const ScopeInfo &scope : kScopes)
        {
            if (scope.weaponId == weaponId)
                return &scope;
        }
        return nullptr;
    }

    // ScopeDebug: appends a line to VR\scope_debug.txt, at most once per second for each caller
    ULONGLONG g_ScopeUpdateDebugTime = 0;
    ULONGLONG g_ScopeSubmitDebugTime = 0;

    void WriteScopeDebug(ULONGLONG &lastWrite, const char *format, ...)
    {
        ULONGLONG now = GetTickCount64();
        if (now - lastWrite < 1000)
            return;
        lastWrite = now;

        char line[1024];
        va_list args;
        va_start(args, format);
        vsnprintf(line, sizeof(line), format, args);
        va_end(args);

        std::ofstream file("VR\\scope_debug.txt", std::ios::app);
        file << now / 1000 << "s " << line << "\n";
    }

    // Black ring around the image and a duplex reticle, transparent everywhere else
    std::vector<uint8_t> CreateScopeMask()
    {
        std::vector<uint8_t> pixels(kScopeMaskSize * kScopeMaskSize * 4, 0);

        const float halfSize = kScopeMaskSize * 0.5f;
        const float pixelSize = 1.0f / halfSize;            // one pixel, in units of the mask radius
        const float imageRadius = 1.0f / kScopeMaskScale;   // edge of the visible image
        const float vignette = 0.08f;                       // soft darkening at the edge of the image
        const float thinLine = 0.006f;                      // reticle half widths, in units of the image radius
        const float thickLine = 0.025f;
        const float thickStart = 0.45f;                     // the thick posts start at 45% of the radius

        for (int y = 0; y < kScopeMaskSize; ++y)
        {
            for (int x = 0; x < kScopeMaskSize; ++x)
            {
                float nx = (x + 0.5f - halfSize) / halfSize;
                float ny = (y + 0.5f - halfSize) / halfSize;
                float r = sqrtf(nx * nx + ny * ny);

                float ring = std::clamp((1.0f - r) / pixelSize, 0.0f, 1.0f)
                    * std::clamp((r - (imageRadius - vignette)) / vignette, 0.0f, 1.0f);

                float reticle = 0.0f;
                if (r < imageRadius)
                {
                    float ax = fabsf(nx) / imageRadius;
                    float ay = fabsf(ny) / imageRadius;
                    float linePixel = pixelSize / imageRadius;
                    float horizontalHalf = ax > thickStart ? thickLine : thinLine;
                    float verticalHalf = ay > thickStart ? thickLine : thinLine;
                    float horizontal = std::clamp((horizontalHalf - ay) / linePixel + 0.5f, 0.0f, 1.0f);
                    float vertical = std::clamp((verticalHalf - ax) / linePixel + 0.5f, 0.0f, 1.0f);
                    reticle = std::max(horizontal, vertical) * 0.9f;
                }

                pixels[(y * kScopeMaskSize + x) * 4 + 3] = (uint8_t)(std::max(ring, reticle) * 255.0f + 0.5f);
            }
        }
        return pixels;
    }
}

void VR::CreateScopeOverlays()
{
    vr::EVROverlayError imageError = m_Overlay->CreateOverlay("ScopeOverlayKey", "ScopeOverlay", &m_ScopeHandle);
    if (imageError != vr::VROverlayError_None)
        m_ScopeHandle = vr::k_ulOverlayHandleInvalid;
    vr::EVROverlayError maskError = m_Overlay->CreateOverlay("ScopeMaskOverlayKey", "ScopeMaskOverlay", &m_ScopeMaskHandle);
    if (maskError != vr::VROverlayError_None)
        m_ScopeMaskHandle = vr::k_ulOverlayHandleInvalid;
    m_ScopeOverlayCreateError = imageError != vr::VROverlayError_None ? imageError : maskError;

    if (m_ScopeHandle == vr::k_ulOverlayHandleInvalid || m_ScopeMaskHandle == vr::k_ulOverlayHandleInvalid)
    {
        if (m_ScopeHandle != vr::k_ulOverlayHandleInvalid)
            m_Overlay->DestroyOverlay(m_ScopeHandle);
        if (m_ScopeMaskHandle != vr::k_ulOverlayHandleInvalid)
            m_Overlay->DestroyOverlay(m_ScopeMaskHandle);
        m_ScopeHandle = vr::k_ulOverlayHandleInvalid;
        m_ScopeMaskHandle = vr::k_ulOverlayHandleInvalid;
        return;
    }

    m_Overlay->SetOverlaySortOrder(m_ScopeHandle, 1);
    m_Overlay->SetOverlaySortOrder(m_ScopeMaskHandle, 2);

    std::vector<uint8_t> mask = CreateScopeMask();
    m_Overlay->SetOverlayRaw(m_ScopeMaskHandle, mask.data(), kScopeMaskSize, kScopeMaskSize, 4);
}

// Called by Hooks::dRenderView before the eyes are rendered. Returns true if the scope view must be rendered this frame.
bool VR::UpdateScope()
{
    m_ScopeAlpha = 0.0f;

    auto skip = [this](const char *reason)
    {
        if (m_ScopeDebug)
            WriteScopeDebug(g_ScopeUpdateDebugTime, "update: not rendered, %s (weapon id %d)", reason, m_ActiveWeaponId);
        return false;
    };

    if (!m_IsVREnabled || !m_ScopeEnabled)
        return skip("VR or ScopeEnabled is off");
    if (!m_ScopeTexture)
        return skip("the scope render target wasn't created");
    if (m_ScopeHandle == vr::k_ulOverlayHandleInvalid)
    {
        if (m_ScopeDebug)
            WriteScopeDebug(g_ScopeUpdateDebugTime, "update: not rendered, creating the overlays failed (error %d)", m_ScopeOverlayCreateError);
        return false;
    }
    // The scope of the right hand's weapon, or of the left hand's gun when the right hand's has none (left_gun.cpp), or
    // is put aside while the right hand has that gun's magazine
    const ScopeInfo *scope = m_RightHandEmpty || m_RightMagazine ? nullptr : FindScope(m_ActiveWeaponId);
    HandMath::Vec3 leftEyepiece;
    QAngle leftAngles;
    float leftRadius = 0.0f;
    const bool left = !scope && GetLeftScope(leftEyepiece, leftAngles, leftRadius);
    m_ScopeOnLeft = left;
    if (!scope && !left)
    {
        m_ScopeBodyWeaponId = 0;
        return skip("no weapon with a scope in the hands");
    }
    if ((left ? m_LeftControllerDeviceIndex : m_RightControllerDeviceIndex) == vr::k_unTrackedDeviceIndexInvalid)
        return skip("no controller for the hand with the scope");

    Vector eyepiece;
    QAngle scopeAngles;
    float scopeRadius;
    int attachment = -1;
    Vector attachmentOrigin;
    if (left)
    {
        eyepiece = Vector(leftEyepiece.x, leftEyepiece.y, leftEyepiece.z);
        scopeAngles = leftAngles;
        scopeRadius = leftRadius;
    }
    else
    {
        scopeAngles = m_RightControllerAngAbs;
        scopeRadius = scope->radius;

        uintptr_t getViewModelAddress = m_Game->m_Offsets->GetViewModel.address;
        if (!getViewModelAddress)
            return skip("GetViewModel signature not found");

        C_BasePlayer *localPlayer = (C_BasePlayer *)m_Game->GetClientEntity(m_Game->m_EngineClient->GetLocalPlayer());
        if (!localPlayer)
            return skip("no local player");

        typedef void *(__thiscall *tGetViewModel)(void *player, int index);
        void *viewModel = ((tGetViewModel)getViewModelAddress)(localPlayer, 0);
        if (!viewModel)
            return skip("no viewmodel");

        // Same calls the game makes for the laser sight: IClientRenderable::LookupAttachment and GetAttachment of the viewmodel.
        // While the fire animation of a bolt action isn't shown, the attachment stays where the weapon is drawn
        // (HoldViewmodelAttachment).
        void *renderable = (uint8_t *)viewModel + 4;
        void **renderableVtable = *(void ***)renderable;
        typedef int(__thiscall *tLookupAttachment)(void *renderable, const char *name);
        typedef bool(__thiscall *tGetAttachment)(void *renderable, int number, Vector &origin, QAngle &angles);

        attachment = ((tLookupAttachment)renderableVtable[0x84 / 4])(renderable, "flashlight");
        if (attachment <= 0)
            return skip("the viewmodel has no flashlight attachment");

        QAngle attachmentAngles;
        if (!((tGetAttachment)renderableVtable[0x8C / 4])(renderable, attachment, attachmentOrigin, attachmentAngles))
            return skip("GetAttachment failed");

        Vector attachmentForward, attachmentRight, attachmentUp;
        QAngle::AngleVectors(attachmentAngles, &attachmentForward, &attachmentRight, &attachmentUp);

        eyepiece = attachmentOrigin + attachmentForward * scope->eyepiece.x - attachmentRight * scope->eyepiece.y + attachmentUp * scope->eyepiece.z;
    }

    // The scope looks where the bullets go
    Vector scopeForward, scopeRight, scopeUp;
    QAngle::AngleVectors(scopeAngles, &scopeForward, &scopeRight, &scopeUp);

    const Vector scopeFront = eyepiece + scopeForward * kScopeBodyLength;
    m_ScopeRear = HandMath::Vec3(eyepiece.x, eyepiece.y, eyepiece.z);
    m_ScopeFront = HandMath::Vec3(scopeFront.x, scopeFront.y, scopeFront.z);
    m_ScopeBodyWeaponId = left ? 0 : m_ActiveWeaponId;

    // Only show (and render) the scope while an eye is behind the eyepiece
    const Vector eyes[2] = { GetViewOriginLeft(), GetViewOriginRight() };
    float alpha = 0.0f;
    float eyeBehind[2], eyeOffAxis[2], eyeAngle[2]; // meters, meters, degrees (only for ScopeDebug)
    for (int i = 0; i < 2; ++i)
    {
        Vector toEye = eyes[i] - eyepiece;
        float behind = -DotProduct(toEye, scopeForward);
        Vector lateral = toEye + scopeForward * behind;
        float angle = RAD2DEG(atan2f(VectorLength(lateral), behind)); // over 90 when the eye is in front of the eyepiece
        float distance = behind / m_VRScale;

        eyeBehind[i] = distance;
        eyeOffAxis[i] = VectorLength(lateral) / m_VRScale;
        eyeAngle[i] = angle;

        if (behind <= 0.0f)
            continue;

        float eyeAlpha = SmoothStep(kScopeFadeStartDistance, kScopeFullDistance, distance) * SmoothStep(kScopeFadeStartAngle, kScopeFullAngle, angle);
        alpha = std::max(alpha, eyeAlpha);
    }

    float alignmentAlpha = alpha;
    if (m_ScopeDebug)
        alpha = 1.0f;

    // Overlay transform relative to the (real) controller of the hand with the scope, in its OpenVR frame (x right, y up,
    // z back), meters
    const Vector &controllerRight = left ? m_LeftControllerRightRaw : m_RightControllerRightRaw;
    const Vector &controllerUp = left ? m_LeftControllerUpRaw : m_RightControllerUpRaw;
    const Vector &controllerForward = left ? m_LeftControllerForwardRaw : m_RightControllerForwardRaw;
    auto toController = [&](const Vector &v)
    {
        return Vector(DotProduct(v, controllerRight), DotProduct(v, controllerUp), -DotProduct(v, controllerForward));
    };

    Vector position = toController(eyepiece - (left ? m_LeftControllerPosAbs : m_RightControllerPosAbsRaw)) * (1.0f / m_VRScale);
    Vector axisX = toController(scopeRight);
    Vector axisY = toController(scopeUp);
    Vector axisZ = toController(scopeForward * -1.0f); // the overlay faces the eye

    m_ScopeOverlayTransform =
    {
        axisX.x, axisY.x, axisZ.x, position.x,
        axisX.y, axisY.y, axisZ.y, position.y,
        axisX.z, axisY.z, axisZ.z, position.z
    };
    m_ScopeOverlayWidth = 2.0f * scopeRadius / m_VRScale;

    if (m_ScopeDebug)
    {
        WriteScopeDebug(g_ScopeUpdateDebugTime,
            "update: weapon id %d, attachment %d at (%.1f %.1f %.1f), eyepiece (%.1f %.1f %.1f), controller (%.1f %.1f %.1f), "
            "left eye %.3f m behind %.3f m off axis %.0f deg, right eye %.3f m behind %.3f m off axis %.0f deg, alignment alpha %.2f, "
            "overlay at (%.3f %.3f %.3f) m from the controller, width %.3f m",
            m_ActiveWeaponId, attachment, attachmentOrigin.x, attachmentOrigin.y, attachmentOrigin.z,
            eyepiece.x, eyepiece.y, eyepiece.z, m_RightControllerPosAbsRaw.x, m_RightControllerPosAbsRaw.y, m_RightControllerPosAbsRaw.z,
            eyeBehind[0], eyeOffAxis[0], eyeAngle[0], eyeBehind[1], eyeOffAxis[1], eyeAngle[1], alignmentAlpha,
            position.x, position.y, position.z, m_ScopeOverlayWidth);
    }

    if (alpha < 0.01f)
        return false;

    float magnification = std::max(m_ScopeZoomHigh ? m_ScopeMagnificationHigh : m_ScopeMagnification, 1.0f);
    m_ScopeFov = 2.0f * RAD2DEG(atanf(tanf(DEG2RAD(kScopeReferenceFov * 0.5f)) / magnification));
    m_ScopeCameraPos = eyepiece;
    m_ScopeCameraAng = scopeAngles;
    m_ScopeAlpha = alpha;

    return true;
}

// The left trigger with the hand on the scope changes between its two magnifications. Called every frame. Returns true
// while that press is the one holding the trigger, so it doesn't work the bolt as well. With both hands on the weapon the
// trigger is for the bolt, and when the hand is closer to the bolt, too.
bool VR::UpdateScopeZoom(bool haveBolt, const HandMath::Vec3 &boltGrab)
{
    // The left hand's gun's scope: the right trigger, with the empty right hand on it, closer to it than to the bolt
    {
        const bool rightTrigger = m_RightHandEmpty && PressedDigitalAction(m_ActionPrimaryAttack);
        const bool rightPressed = rightTrigger && !m_LeftScopeTriggerDown;
        m_LeftScopeTriggerDown = rightTrigger;
        if (!rightTrigger)
            m_LeftScopeTookTrigger = false;
        if (rightPressed && m_ScopeEnabled && m_ScopeOnLeft)
        {
            const HandMath::Vec3 fist = GetFistPoint(true);
            const float toScope = Reload::DistanceToSegment(fist, m_ScopeRear, m_ScopeFront);
            const bool nearerThanBolt = !m_LeftGunHasBolt || !m_ManualReloadEnabled || toScope <= HandMath::Length(fist - m_LeftGunBolt);
            if (toScope < kScopeZoomReach * m_VRScale && nearerThanBolt)
            {
                m_ScopeZoomHigh = !m_ScopeZoomHigh;
                m_LeftScopeTookTrigger = true;
                m_Haptics.Play(Haptics::Hand::WeaponHand, HapticEffects::Take);
                PlayWeaponSound("weapons/awp/gunother/zoom.wav");
            }
        }
    }

    const bool trigger = PressedDigitalAction(m_ActionSecondaryAttack);
    const bool pressed = trigger && !m_ScopeZoomTriggerDown;
    m_ScopeZoomTriggerDown = trigger;
    if (!trigger)
        m_ScopeZoomTookTrigger = false;

    // Each time a weapon is taken it starts with the lower magnification
    if (m_ActiveWeaponId != m_ScopeZoomWeaponId)
    {
        m_ScopeZoomWeaponId = m_ActiveWeaponId;
        m_ScopeZoomHigh = false;
    }

    if (pressed && m_ScopeEnabled && !isHoldingWeapon2Handed && m_ScopeBodyWeaponId == m_ActiveWeaponId && FindScope(m_ActiveWeaponId))
    {
        const HandMath::Vec3 palm = GetLeftPalm();
        const float toScope = Reload::DistanceToSegment(palm, m_ScopeRear, m_ScopeFront);
        const bool nearerThanBolt = !haveBolt || !m_ManualReloadEnabled || toScope <= HandMath::Length(palm - boltGrab);
        if (toScope < kScopeZoomReach * m_VRScale && nearerThanBolt)
        {
            m_ScopeZoomHigh = !m_ScopeZoomHigh;
            m_ScopeZoomTookTrigger = true;
            m_Haptics.Play(Haptics::Hand::OtherHand, HapticEffects::Take);
            PlayWeaponSound("weapons/awp/gunother/zoom.wav");
        }
    }

    return m_ScopeZoomTookTrigger;
}

void VR::SubmitScopeOverlays()
{
    if (m_ScopeHandle == vr::k_ulOverlayHandleInvalid)
        return;

    bool rendered = m_ScopeRendered;
    m_ScopeRendered = false;

    const vr::TrackedDeviceIndex_t scopeDevice = m_ScopeOnLeft ? m_LeftControllerDeviceIndex : m_RightControllerDeviceIndex;
    if (!rendered || m_ScopeAlpha < 0.01f || !m_VKScope.m_VRTexture.handle || scopeDevice == vr::k_unTrackedDeviceIndexInvalid)
    {
        if (m_ScopeDebug && rendered)
            WriteScopeDebug(g_ScopeSubmitDebugTime, "submit: hidden, alpha %.2f, scope texture %s, controller device %u",
                m_ScopeAlpha, m_VKScope.m_VRTexture.handle ? "ok" : "MISSING", m_RightControllerDeviceIndex);
        HideScopeOverlays();
        return;
    }

    // The mask sits half a millimeter in front of the image
    vr::HmdMatrix34_t maskTransform = m_ScopeOverlayTransform;
    for (int i = 0; i < 3; ++i)
        maskTransform.m[i][3] += maskTransform.m[i][2] * 0.0005f;

    vr::EVROverlayError textureError = m_Overlay->SetOverlayTexture(m_ScopeHandle, &m_VKScope.m_VRTexture);
    m_Overlay->SetOverlayWidthInMeters(m_ScopeHandle, m_ScopeOverlayWidth);
    m_Overlay->SetOverlayWidthInMeters(m_ScopeMaskHandle, m_ScopeOverlayWidth * kScopeMaskScale);
    vr::EVROverlayError transformError = m_Overlay->SetOverlayTransformTrackedDeviceRelative(m_ScopeHandle, scopeDevice, &m_ScopeOverlayTransform);
    m_Overlay->SetOverlayTransformTrackedDeviceRelative(m_ScopeMaskHandle, scopeDevice, &maskTransform);
    m_Overlay->SetOverlayAlpha(m_ScopeHandle, m_ScopeAlpha);
    m_Overlay->SetOverlayAlpha(m_ScopeMaskHandle, m_ScopeAlpha);
    vr::EVROverlayError showError = m_Overlay->ShowOverlay(m_ScopeHandle);
    m_Overlay->ShowOverlay(m_ScopeMaskHandle);
    m_ScopeOverlaysVisible = true;

    if (m_ScopeDebug)
        WriteScopeDebug(g_ScopeSubmitDebugTime, "submit: shown, alpha %.2f, errors: texture %d, transform %d, show %d, visible %d, controller device %u",
            m_ScopeAlpha, textureError, transformError, showError, m_Overlay->IsOverlayVisible(m_ScopeHandle), m_RightControllerDeviceIndex);
}

void VR::HideScopeOverlays()
{
    if (!m_ScopeOverlaysVisible)
        return;
    m_ScopeOverlaysVisible = false;

    if (m_ScopeHandle != vr::k_ulOverlayHandleInvalid)
        m_Overlay->HideOverlay(m_ScopeHandle);
    if (m_ScopeMaskHandle != vr::k_ulOverlayHandleInvalid)
        m_Overlay->HideOverlay(m_ScopeMaskHandle);
}

// Each of these is a call to the SteamVR process, so they're only made when something changes
void VR::SetHudOverlayVisible(bool visible)
{
    if (m_HudOverlayVisible == visible)
        return;
    m_HudOverlayVisible = visible;

    if (visible)
        vr::VROverlay()->ShowOverlay(m_HUDHandle);
    else
        vr::VROverlay()->HideOverlay(m_HUDHandle);
}

void VR::SetMainMenuOverlayVisible(bool visible)
{
    if (m_MainMenuOverlayVisible == visible)
        return;
    m_MainMenuOverlayVisible = visible;

    if (visible)
        vr::VROverlay()->ShowOverlay(m_MainMenuHandle);
    else
        vr::VROverlay()->HideOverlay(m_MainMenuHandle);
}

void VR::SetHudOverlayInteractive(bool interactive)
{
    if (m_HudOverlayInteractive == interactive)
        return;
    m_HudOverlayInteractive = interactive;
    vr::VROverlay()->SetOverlayFlag(m_HUDHandle, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, interactive);
}

// Sent again once per second anyway, in case the game released the button by itself (a map change, for example)
void VR::SetGameButton(GameButton button, bool down)
{
    static const char *const kNames[GameButton_Count] = { "forward", "back", "moveleft", "moveright", "attack", "jump", "use", "reload", "attack2", "duck", "showscores" };

    const signed char state = down ? 1 : 0;
    if (m_GameButtonState[button] == state && !m_ResendGameButtons)
        return;
    m_GameButtonState[button] = state;

    char command[32];
    snprintf(command, sizeof(command), "%c%s", down ? '+' : '-', kNames[button]);
    m_Game->ClientCmd_Unrestricted(command);
}

// ---------------------------------------------------------------------------------------------------------------
// Grenade throw
//
// The game throws the molotov, pipe bomb and bile jar from 16 units in front of the eyes at 750 units/s plus the
// player's velocity, 0.2 s after the trigger is let go (CBaseCSGrenade::StartGrenadeThrow / ThrowGrenade).
// With GrenadeThrowGesture the grenade leaves from the hand instead, right away (Hooks::dStartGrenadeThrow), with the
// velocity the hand had when the trigger was let go. Gentle throws keep the real speed of the hand (letting go with a
// still hand drops the grenade), fast throws are made stronger by up to GrenadeThrowStrength.
//
// The throw is replaced in server.dll by the host of the game:
// - the host's own throws come straight from here (TakeGrenadeThrow),
// - players who joined send their throw to the host in the usercmds, in place of the controller angles
//   (GetGrenadeThrowMessage / EncodeGrenadeThrow, decoded by Hooks::dReadUsercmd), and their hand position is the one
//   they send with every usercmd.
// The host adds the player's velocity on the server, like the game does.
// ---------------------------------------------------------------------------------------------------------------

namespace
{
    constexpr float kMaxThrowSpeed = 1100.0f;     // units/s, for the arm part of the throw
    constexpr auto kHandVelocityHistory = std::chrono::milliseconds(150);
    // Hand velocity at the release: the average of the last samples
    constexpr auto kReleaseAverageWindow = std::chrono::milliseconds(35);
    // Fast throws use the fastest point of the swing in this window before the release instead, since people let go a
    // bit late. Gentle throws use the velocity at the release, so stopping the hand and letting go drops the grenade.
    constexpr auto kLateReleaseWindow = std::chrono::milliseconds(100);
    constexpr float kGentleHandSpeed = 2.0f;      // m/s, up to this the grenade gets the real velocity of the hand
    constexpr float kLateReleaseSpeed = 4.0f;     // m/s, fastest point from which the late release is fully corrected
    constexpr float kFullStrengthSpeed = 6.0f;    // m/s, from this hand speed on GrenadeThrowStrength applies fully
    // No shoving with the swing gestures while throwing (the arms move fast)
    constexpr auto kNoShoveAfterRelease = std::chrono::milliseconds(500);
    // Throws from further than this from the game's spawn point are not trusted (tracking lost)
    constexpr float kMaxThrowOriginDistance = 80.0f;
    // Cancel: distance between the controllers (meters), and how long to wait for the grenade to be put away
    constexpr float kCancelHandDistance = 0.2f;
    constexpr auto kCancelSwitchTimeout = std::chrono::milliseconds(1000);

    // The throw is sent in every usercmd for this long after the release, so a lost packet doesn't lose it
    constexpr auto kThrowMessageTime = std::chrono::milliseconds(500);
    // Throw message in mousedx / mousedy (normally the controller angles * 10, so never above 3600):
    // mousedy = 0x4000 | payload bits 16-29, mousedx = payload bits 0-15.
    // Payload: 3 x 9 bit signed velocity in steps of 5 units/s (x, y, z: +-1275), 1 bit "arm throw", 2 bit sequence (1-3)
    constexpr int kThrowMessageMarker = 0x4000;
    constexpr float kThrowMessageVelocityStep = 5.0f;

    float PitchUp(const Vector &v)
    {
        return RAD2DEG(atan2f(v.z, sqrtf(v.x * v.x + v.y * v.y)));
    }
}

namespace
{
    // Appends a line with the time to a log file. Each file is recreated the first time it's written after the game starts.
    void WriteLogLine(const char *path, const char *format, va_list args)
    {
        static std::mutex s_Mutex;
        static std::vector<std::string> s_RecreatedFiles;

        char line[512];
        vsnprintf(line, sizeof(line), format, args);

        SYSTEMTIME time;
        GetLocalTime(&time);
        char stamp[32];
        snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d.%03d ", time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);

        std::lock_guard<std::mutex> lock(s_Mutex);
        const bool recreated = std::find(s_RecreatedFiles.begin(), s_RecreatedFiles.end(), path) != s_RecreatedFiles.end();
        if (!recreated)
            s_RecreatedFiles.push_back(path);

        std::ofstream file(path, recreated ? std::ios::app : std::ios::trunc);
        file << stamp << line << "\n";
    }
}

// Writes to VR\grenade_throw_log.txt, a line or two per throw. Called from VR::Update and from the server hooks.
void VR::WriteGrenadeThrowLog(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    WriteLogLine("VR\\grenade_throw_log.txt", format, args);
    va_end(args);
}

bool VR::IsThrowableWeapon(int weaponId)
{
    return weaponId == C_WeaponCSBase::MOLOTOV || weaponId == C_WeaponCSBase::PIPE_BOMB || weaponId == C_WeaponCSBase::VOMITJAR;
}

// Called by ProcessInput every frame, before the trigger and the shove button are sent to the game
void VR::UpdateGrenadeThrow()
{
    const auto now = std::chrono::steady_clock::now();
    // A grenade in the left hand (dual wield) is thrown with that hand and its trigger
    const bool leftHand = m_LeftUseActive;
    // An empty right hand throws nothing: its grenade is put away (EmptyRightHand)
    // Nor does the right hand while it works the left hand's gun (ReloadNeedsFreeHand off)
    const bool trigger = PressedDigitalAction(leftHand ? m_ActionSecondaryAttack : m_ActionPrimaryAttack) && (leftHand || (!m_RightHandEmpty && !m_RightWorksLeftGun));

    const bool shoveButton = !leftHand && PressedDigitalAction(m_ActionSecondaryAttack);
    const bool shoveButtonPressed = shoveButton && !m_ShoveButtonDown;
    m_ShoveButtonDown = shoveButton;
    if (!shoveButton)
        m_ShoveButtonUsedForCancel = false;

    UpdateGrenadeCancel(now, trigger);

    const bool throwable = IsThrowableWeapon(m_ActiveWeaponId);

    // Cancel: with the trigger held, the other hand presses its trigger (shove) next to the grenade.
    // Works with the game's own throw too (GrenadeThrowGesture off).
    if (throwable && trigger && m_GrenadeCancelState == GrenadeCancel_None && shoveButtonPressed
        && VectorLength(m_LeftControllerPose.TrackedDevicePos - m_RightControllerPose.TrackedDevicePos) < kCancelHandDistance)
    {
        m_GrenadeCancelState = GrenadeCancel_PuttingAway;
        m_GrenadeCancelTime = now;
        m_ShoveButtonUsedForCancel = true;
        m_Game->ClientCmd_Unrestricted("slot2");
        m_Haptics.Play(Haptics::Hand::BothHands, HapticEffects::Cancel);
        WriteGrenadeThrowLog("cancel: putting the grenade away");
    }

    if (!m_GrenadeThrowGesture || !throwable || m_GrenadeCancelState != GrenadeCancel_None)
    {
        m_GrenadeTriggerHeld = false;
        m_HandVelocityHistory.clear();
        return;
    }

    // Hand velocity in game axes: the tracking space turned by the thumbstick turning
    Vector handVelocity = leftHand ? m_LeftControllerPose.TrackedDeviceVel : m_RightControllerPose.TrackedDeviceVel;
    VectorPivotXY(handVelocity, Vector(0, 0, 0), m_RotationOffset);
    m_HandVelocityHistory.push_back({ now, handVelocity });
    while (!m_HandVelocityHistory.empty() && now - m_HandVelocityHistory.front().time > kHandVelocityHistory)
        m_HandVelocityHistory.pop_front();

    if (trigger)
    {
        m_GrenadeTriggerHeld = true;
        return;
    }

    if (!m_GrenadeTriggerHeld)
        return;

    // The trigger was let go: the game starts the throw
    m_GrenadeTriggerHeld = false;
    m_GrenadeReleaseTime = now;

    // Velocity at the release: average of the last samples (always at least the newest one)
    Vector releaseVelocity = { 0,0,0 };
    int releaseSamples = 0;
    for (size_t i = m_HandVelocityHistory.size(); i-- > 0;)
    {
        if (releaseSamples > 0 && now - m_HandVelocityHistory[i].time > kReleaseAverageWindow)
            break;
        releaseVelocity += m_HandVelocityHistory[i].velocity;
        ++releaseSamples;
    }
    if (releaseSamples > 0)
        releaseVelocity *= 1.0f / (float)releaseSamples;

    // Fastest point of the swing just before the release, averaged with the samples next to it
    size_t peak = m_HandVelocityHistory.size();
    float peakSpeed = -1.0f;
    for (size_t i = 0; i < m_HandVelocityHistory.size(); ++i)
    {
        if (now - m_HandVelocityHistory[i].time > kLateReleaseWindow)
            continue;
        const float speed = VectorLength(m_HandVelocityHistory[i].velocity);
        if (speed > peakSpeed)
        {
            peakSpeed = speed;
            peak = i;
        }
    }

    Vector peakVelocity = releaseVelocity;
    if (peak < m_HandVelocityHistory.size())
    {
        peakVelocity = { 0,0,0 };
        const size_t first = peak > 0 ? peak - 1 : 0;
        const size_t last = std::min(peak + 1, m_HandVelocityHistory.size() - 1);
        for (size_t i = first; i <= last; ++i)
            peakVelocity += m_HandVelocityHistory[i].velocity;
        peakVelocity *= 1.0f / (float)(last - first + 1);
    }

    // Gentle throws: the velocity at the release. Fast throws: the fastest point (late release).
    const float lateRelease = SmoothStep(kGentleHandSpeed, kLateReleaseSpeed, VectorLength(peakVelocity));
    const Vector throwHandVelocity = releaseVelocity + (peakVelocity - releaseVelocity) * lateRelease;
    const float handSpeed = VectorLength(throwHandVelocity);

    // Real speed for gentle throws, up to GrenadeThrowStrength times for fast ones
    const float strength = 1.0f + (m_GrenadeThrowStrength - 1.0f) * SmoothStep(kGentleHandSpeed, kFullStrengthSpeed, handSpeed);
    Vector armVelocity = throwHandVelocity * (m_VRScale * strength);
    const float armSpeed = VectorLength(armVelocity);
    if (armSpeed > kMaxThrowSpeed)
        armVelocity *= kMaxThrowSpeed / armSpeed;

    GrenadeThrow grenadeThrow;
    grenadeThrow.pending = true;
    grenadeThrow.gesture = true;
    grenadeThrow.time = now;
    grenadeThrow.handSpeed = handSpeed;
    grenadeThrow.armVelocity = armVelocity;
    m_GrenadeThrowSequence = m_GrenadeThrowSequence % 3 + 1;
    grenadeThrow.sequence = m_GrenadeThrowSequence;

    {
        std::lock_guard<std::mutex> lock(m_GrenadeThrowMutex);
        m_GrenadeThrow = grenadeThrow;
    }

    WriteGrenadeThrowLog("released: hand %.2f m/s (%.2f at the release, %.2f at the fastest point), strength %.2fx, arm part of the throw %.0f units/s at %.0f deg up, %d velocity samples",
        handSpeed, VectorLength(releaseVelocity), VectorLength(peakVelocity), strength, VectorLength(armVelocity),
        PitchUp(armVelocity), (int)m_HandVelocityHistory.size());
}

void VR::UpdateGrenadeCancel(std::chrono::steady_clock::time_point now, bool trigger)
{
    switch (m_GrenadeCancelState)
    {
    case GrenadeCancel_PuttingAway:
        // The secondary weapon is out (the pin is back in): take the grenade out again. The attack is let go in the same
        // usercmd, which doesn't throw since the grenade is being drawn.
        if (!IsThrowableWeapon(m_ActiveWeaponId))
        {
            m_Game->ClientCmd_Unrestricted("slot3");
            m_GrenadeCancelState = GrenadeCancel_WaitTriggerRelease;
            WriteGrenadeThrowLog("cancel: grenade put away after %d ms, taking it out again",
                (int)std::chrono::duration_cast<std::chrono::milliseconds>(now - m_GrenadeCancelTime).count());
        }
        else if (now - m_GrenadeCancelTime > kCancelSwitchTimeout)
        {
            m_GrenadeCancelState = GrenadeCancel_WaitTriggerRelease;
            WriteGrenadeThrowLog("cancel failed: the grenade couldn't be put away");
        }
        break;

    case GrenadeCancel_WaitTriggerRelease:
        if (!trigger)
            m_GrenadeCancelState = GrenadeCancel_None;
        break;

    default:
        break;
    }
}

// The attack sent to the game for the state of the trigger, held or held back while a throw is cancelled
bool VR::GetAttackButton(bool trigger)
{
    switch (m_GrenadeCancelState)
    {
    case GrenadeCancel_PuttingAway:
        return true;
    case GrenadeCancel_WaitTriggerRelease:
        return false;
    default:
        return trigger && !ManualReloadBlocksFire();
    }
}

// True while the trigger is held with a throwable, and shortly after it's let go
bool VR::IsGrenadeThrowInProgress()
{
    if (!m_GrenadeThrowGesture || !IsThrowableWeapon(m_ActiveWeaponId))
        return false;

    return m_GrenadeTriggerHeld || std::chrono::steady_clock::now() - m_GrenadeReleaseTime < kNoShoveAfterRelease;
}

// Server side (Hooks::dStartGrenadeThrow): was the trigger just let go during a swing?
bool VR::HasPendingGestureThrow()
{
    std::lock_guard<std::mutex> lock(m_GrenadeThrowMutex);
    return m_GrenadeThrow.pending && m_GrenadeThrow.gesture
        && std::chrono::steady_clock::now() - m_GrenadeThrow.time < kGrenadeThrowValidTime;
}

// Server side (Hooks::dEmitGrenade): origin and arm velocity for the grenade the local player is throwing.
// Returns false to keep the game's throw.
bool VR::TakeGrenadeThrow(const Vector &gameOrigin, Vector &origin, Vector &armVelocity, int &ageMs)
{
    GrenadeThrow grenadeThrow;
    {
        std::lock_guard<std::mutex> lock(m_GrenadeThrowMutex);
        grenadeThrow = m_GrenadeThrow;
        m_GrenadeThrow.pending = false;
    }

    const auto age = std::chrono::steady_clock::now() - grenadeThrow.time;
    if (!grenadeThrow.pending || age > kGrenadeThrowValidTime)
        return false;

    origin = GetGrenadeThrowOrigin(m_HmdPosAbs, m_RightControllerPosAbsRaw, gameOrigin);
    armVelocity = grenadeThrow.armVelocity;
    ageMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(age).count();
    return true;
}

// The hand, or the point where a wall between the start (head) and the hand is. Players and NPCs are ignored.
Vector VR::GetGrenadeThrowOrigin(const Vector &start, const Vector &hand, const Vector &gameOrigin)
{
    if (VectorLength(hand - gameOrigin) > kMaxThrowOriginDistance)
        return gameOrigin;

    // Same box as the game's own spawn point trace
    CGameTrace trace;
    Ray_t ray;
    ray.Init(start, hand, Vector(-2, -2, -2), Vector(2, 2, 2));
    CTraceFilterSkipNPCsAndPlayers traceFilter(nullptr, 0);
    m_Game->m_EngineTrace->TraceRay(ray, MASK_STATICWORLD | CONTENTS_MOVEABLE, &traceFilter, &trace);

    if (trace.startsolid || trace.allsolid)
        return gameOrigin;

    return trace.endpos;
}

// Called by Hooks::dWriteUsercmd: the throw to send in this usercmd instead of the controller angles, for a short
// time after the trigger was let go
bool VR::GetGrenadeThrowMessage(short &mousedx, short &mousedy)
{
    GrenadeThrow grenadeThrow;
    {
        std::lock_guard<std::mutex> lock(m_GrenadeThrowMutex);
        grenadeThrow = m_GrenadeThrow;
    }

    if (grenadeThrow.sequence == 0 || std::chrono::steady_clock::now() - grenadeThrow.time > kThrowMessageTime)
        return false;

    EncodeGrenadeThrow(grenadeThrow.gesture, grenadeThrow.sequence, grenadeThrow.armVelocity, mousedx, mousedy);
    return true;
}

void VR::EncodeGrenadeThrow(bool gesture, int sequence, const Vector &armVelocity, short &mousedx, short &mousedy)
{
    auto quantize = [](float value) -> uint32_t
    {
        const int steps = std::clamp((int)lroundf(value / kThrowMessageVelocityStep), -255, 255);
        return (uint32_t)steps & 0x1FF;
    };

    const uint32_t payload = quantize(armVelocity.x) | (quantize(armVelocity.y) << 9) | (quantize(armVelocity.z) << 18)
        | ((gesture ? 1u : 0u) << 27) | (((uint32_t)sequence & 3) << 28);

    mousedx = (short)(uint16_t)(payload & 0xFFFF);
    mousedy = (short)(uint16_t)(kThrowMessageMarker | ((payload >> 16) & 0x3FFF));
}

// Returns false if mousedx / mousedy are the controller angles (or a usercmd from an older version of the mod)
bool VR::DecodeGrenadeThrow(short mousedx, short mousedy, bool &gesture, int &sequence, Vector &armVelocity)
{
    const uint16_t high = (uint16_t)mousedy;
    if ((high & 0xC000) != kThrowMessageMarker)
        return false;

    const uint32_t payload = (uint32_t)(uint16_t)mousedx | ((uint32_t)(high & 0x3FFF) << 16);

    auto unquantize = [](uint32_t bits) -> float
    {
        int steps = (int)(bits & 0x1FF);
        if (steps & 0x100)
            steps -= 0x200;
        return steps * kThrowMessageVelocityStep;
    };

    armVelocity = Vector(unquantize(payload), unquantize(payload >> 9), unquantize(payload >> 18));
    gesture = ((payload >> 27) & 1) != 0;
    sequence = (int)((payload >> 28) & 3);
    return sequence != 0;
}

// ---------------------------------------------------------------------------------------------------------------
// Taking items from the body
//
// Pressing a grab button with the hand at one of the places below takes an item (slot1 to slot5), or the other item
// of the pair when that one is already in hand (primary <-> secondary weapon, grenade <-> pills).
//
// The game doesn't track the body, so the places are relative to the head, but not to its pitch and roll:
// - they're measured from where the eyes would be with the head looking straight ahead. The HMD turns around the
//   neck, so looking down moves it forward and down; a simple head model (kEyeToNeck*) finds the neck pivot and puts
//   the reference point back in front of it,
// - their directions use the horizontal direction the head faces.
// Turning the head left or right still turns them.
// The places of the weapon hand are on its side of the body (mirrored with LeftHanded).
// ---------------------------------------------------------------------------------------------------------------

namespace
{
    // From the eyes to the neck pivot, meters (the usual head model of the VR runtimes)
    constexpr float kEyeToNeckForward = 0.08f;
    constexpr float kEyeToNeckDown = 0.075f;

    // The places were tuned in game units at the default VRScale of 43.2 units per meter
    constexpr float Tuned(float units)
    {
        return units / 43.2f;
    }

    // The hand relative to the reference point at the eyes, meters
    struct HandOnBody
    {
        float distance; // straight line
        float side;     // horizontal direction: 1 = towards the weapon hand's side, -1 = the other side
        float forward;  // horizontal direction: 1 = in front, -1 = behind
        float drop;     // how far below
    };

    bool InRange(float value, float center, float tolerance)
    {
        return value >= center - tolerance && value <= center + tolerance;
    }

    struct BodyGrabPlace
    {
        const char *name;
        bool weaponHand; // grabbed with the weapon hand, or with the other one
        int slot;
        int otherSlot;   // taken instead when the item of `slot` is already in hand
        bool (*isAt)(const HandOnBody &hand, float playerHeight);
    };

    // The first place the hand is at is used
    const BodyGrabPlace kBodyGrabPlaces[] =
    {
        { "primary weapon (behind the shoulder)", true, 1, 2, [](const HandOnBody &hand, float)
            { return InRange(hand.distance, Tuned(12.0f), Tuned(2.0f)) && hand.side >= 0.1f && hand.forward <= -0.7f; } },
        // The secondary weapon is taken from the thigh, where the pistol is seen (VR::IsAtHolster)
        { "medkit (back)", true, 4, 4, [](const HandOnBody &hand, float height)
            { return hand.forward < -0.9f && InRange(hand.drop, Tuned(11.9f) * height, Tuned(5.0f)); } },

        // Grenades, pills and adrenaline are taken where they're seen on the chest (body_items.cpp), with either hand
        { "medkit (back)", false, 4, 4, [](const HandOnBody &hand, float height)
            { return hand.forward < -0.9f && InRange(hand.drop, Tuned(11.9f) * height, Tuned(5.0f)); } },
    };

    // VR\body_grab_log.txt, a line per grab button press
    void WriteBodyGrabLog(const char *format, ...)
    {
        va_list args;
        va_start(args, format);
        WriteLogLine("VR\\body_grab_log.txt", format, args);
        va_end(args);
    }
}

// The inventory slot of a weapon, as selected by the slot1 to slot5 commands (0 if none)
int VR::GetWeaponSlot(int weaponId)
{
    switch (weaponId)
    {
    case C_WeaponCSBase::AK47: case C_WeaponCSBase::AUTOSHOTGUN: case C_WeaponCSBase::AWP: case C_WeaponCSBase::GRENADE_LAUNCHER:
    case C_WeaponCSBase::HUNTING_RIFLE: case C_WeaponCSBase::M16A1: case C_WeaponCSBase::M60: case C_WeaponCSBase::MAC10:
    case C_WeaponCSBase::MACHINEGUN: case C_WeaponCSBase::MP5: case C_WeaponCSBase::PUMPSHOTGUN: case C_WeaponCSBase::SHOTGUN_CHROME:
    case C_WeaponCSBase::SNIPER_MILITARY: case C_WeaponCSBase::UZI: case C_WeaponCSBase::SCAR: case C_WeaponCSBase::SPAS:
    case C_WeaponCSBase::SG552: case C_WeaponCSBase::SCOUT:
        return 1;
    case C_WeaponCSBase::MELEE: case C_WeaponCSBase::PISTOL: case C_WeaponCSBase::MAGNUM: case C_WeaponCSBase::CHAINSAW:
        return 2;
    case C_WeaponCSBase::MOLOTOV: case C_WeaponCSBase::PIPE_BOMB: case C_WeaponCSBase::VOMITJAR:
        return 3;
    case C_WeaponCSBase::FIRST_AID_KIT: case C_WeaponCSBase::DEFIBRILLATOR: case C_WeaponCSBase::INCENDIARY_AMMO: case C_WeaponCSBase::FRAG_AMMO:
        return 4;
    case C_WeaponCSBase::PAIN_PILLS: case C_WeaponCSBase::ADRENALINE:
        return 5;
    default:
        return 0;
    }
}

// Called by UpdateTracking when a grab button is pressed (and the weapon isn't being grabbed with both hands)
void VR::UpdateBodyGrab(bool weaponHand, const Vector &handPosLocalInWorld, int weaponId, const HandMath::Vec3 *fistOverride)
{
    // Horizontal direction the body faces (UpdateBodyYaw): the places on the body stay where they are when the head
    // turns to look at them
    const Vector facing = m_BodyFacing;
    const Vector right(facing.y, -facing.x, 0.0f);

    // Where the eyes would be with the head turned that way and looking straight ahead, from the neck pivot
    const Vector neck = m_HmdPosLocalInWorld - m_HmdForward * (kEyeToNeckForward * m_VRScale) - m_HmdUp * (kEyeToNeckDown * m_VRScale);
    const Vector reference = neck + facing * (kEyeToNeckForward * m_VRScale) + Vector(0.0f, 0.0f, kEyeToNeckDown * m_VRScale);

    const Vector toHand = (handPosLocalInWorld - reference) * (1.0f / m_VRScale);
    const float horizontalLength = sqrtf(toHand.x * toHand.x + toHand.y * toHand.y);
    const float weaponSide = m_LeftHanded ? -1.0f : 1.0f;

    HandOnBody hand;
    hand.distance = VectorLength(toHand);
    hand.side = horizontalLength > 0.001f ? weaponSide * (toHand.x * right.x + toHand.y * right.y) / horizontalLength : 0.0f;
    hand.forward = horizontalLength > 0.001f ? (toHand.x * facing.x + toHand.y * facing.y) / horizontalLength : 0.0f;
    hand.drop = -toHand.z;

    const float headPitchDown = RAD2DEG(asinf(std::clamp(-m_HmdForward.z, -1.0f, 1.0f)));
    const char *handName = weaponHand ? "weapon hand" : "other hand";

    // What's seen on the body is taken where the fist is on it, as it's drawn. The places not drawn are by the controller.
    const Vector handWorld = m_CameraAnchor - Vector(0, 0, 64) + handPosLocalInWorld;
    const HandMath::Vec3 fist = fistOverride ? *fistOverride : GetFistPoint(weaponHand);

    // The pills or the grenade on the chest. The left hand takes them into itself (dual wield), and puts them back.
    if (!weaponHand && LeftHandChestGrab(fist))
    {
        WriteBodyGrabLog("%s: chest, left hand (dual wield), holds entity %d", handName, m_LeftItemIndex);
        return;
    }
    // Behind the left shoulder, the left hand puts its melee weapon away or takes it (dual wield). Wider than the right
    // shoulder's place: people reach further back with the hand that doesn't hold the weapon.
    if (!weaponHand && m_DualWieldEnabled && InRange(hand.distance, 0.32f, 0.11f) && hand.side <= -0.1f && hand.forward <= -0.6f
        && LeftShoulderGrab())
    {
        WriteBodyGrabLog("%s: %.2f m away, side %.2f, forward %.2f, %.2f m below: left shoulder, holds entity %d",
            handName, hand.distance, hand.side, hand.forward, hand.drop, m_LeftItemIndex);
        return;
    }

    const int chestSlot = GetChestItemSlotAt(fist);
    if (chestSlot)
    {
        m_RightHandEmpty = false;
        char command[16];
        snprintf(command, sizeof(command), "slot%d", chestSlot);
        m_Game->ClientCmd_Unrestricted(command);
        m_Haptics.Play(weaponHand ? Haptics::Hand::WeaponHand : Haptics::Hand::OtherHand, HapticEffects::Take);
        WriteBodyGrabLog("%s: %.2f m away, side %.2f, forward %.2f, %.2f m below, head pitched %.0f deg down: %s on the chest, %s",
            handName, hand.distance, hand.side, hand.forward, hand.drop, headPitchDown, chestSlot == 3 ? "grenade" : "pills or adrenaline", command);
        return;
    }

    // The right hand puts back on the body what it holds and is left empty (EmptyRightHand), or, holding something else
    // or nothing, takes what's there
    const int heldSlot = weaponHand && !m_RightHandEmpty ? GetWeaponSlot(weaponId) : 0;
    auto putOrTake = [&](int slot, const char *place)
    {
        const char *result = "nothing there";
        if (heldSlot == slot)
        {
            EmptyRightHand(place);
            result = "put there, the hand is empty";
        }
        else if (TakeRightSlot(slot))
        {
            result = "taken";
        }
        m_Haptics.Play(Haptics::Hand::WeaponHand, *result == 'n' ? HapticEffects::Cancel : HapticEffects::Confirm);
        WriteBodyGrabLog("%s: %.2f m away, side %.2f, forward %.2f, %.2f m below, head pitched %.0f deg down: %s, %s",
            handName, hand.distance, hand.side, hand.forward, hand.drop, headPitchDown, place, result);
    };

    // Its grenade, pills or adrenaline where they're seen on the chest
    if (weaponHand && !m_RightHandEmpty)
    {
        const int spot = GetChestSpotAt(fist);
        const bool pills = weaponId == C_WeaponCSBase::PAIN_PILLS || weaponId == C_WeaponCSBase::ADRENALINE;
        if ((spot == 1 && IsThrowableWeapon(weaponId)) || (spot == 0 && pills))
        {
            putOrTake(heldSlot, "chest");
            return;
        }
    }

    // The pistol's place on the thigh (body_items.cpp), which follows the leg when crouching: the pistol or melee weapon
    if (weaponHand && IsAtHolster(fist))
    {
        putOrTake(2, "secondary weapon (thigh)");
        return;
    }

    for (const BodyGrabPlace &place : kBodyGrabPlaces)
    {
        if (place.weaponHand != weaponHand || !place.isAt(hand, m_PlayerHeightInMeters))
            continue;

        // With items in the left hand, a grab only changes what its own hand holds: the left hand takes nothing for the
        // right one, and takes the kit there for itself, or puts its own back
        if (!weaponHand && m_DualWieldEnabled)
        {
            if (place.slot == 4 && LeftHandKitGrab())
            {
                WriteBodyGrabLog("%s: %.2f m away, side %.2f, forward %.2f, %.2f m below: %s, left hand (dual wield), holds entity %d",
                    handName, hand.distance, hand.side, hand.forward, hand.drop, place.name, m_LeftItemIndex);
                return;
            }
            continue;
        }

        if (weaponHand)
        {
            putOrTake(place.slot, place.name);
            return;
        }

        const int slot = GetWeaponSlot(weaponId) == place.slot ? place.otherSlot : place.slot;
        m_RightHandEmpty = false;
        char command[16];
        snprintf(command, sizeof(command), "slot%d", slot);
        m_Game->ClientCmd_Unrestricted(command);
        m_Haptics.Play(Haptics::Hand::OtherHand, HapticEffects::Confirm);

        WriteBodyGrabLog("%s: %.2f m away, side %.2f, forward %.2f, %.2f m below, head pitched %.0f deg down: %s, %s",
            handName, hand.distance, hand.side, hand.forward, hand.drop, headPitchDown, place.name, command);
        return;
    }

    // Nothing on the body here: a weapon, an item or a door handle in the world, if the hand is on one
    if (TryGrabWorld(HandMath::Vec3(handWorld.x, handWorld.y, handWorld.z), !weaponHand))
    {
        m_Haptics.Play(weaponHand ? Haptics::Hand::WeaponHand : Haptics::Hand::OtherHand, HapticEffects::Take);
        return;
    }

    WriteBodyGrabLog("%s: %.2f m away, side %.2f, forward %.2f, %.2f m below, head pitched %.0f deg down: no place",
        handName, hand.distance, hand.side, hand.forward, hand.drop, headPitchDown);
}

// L4D2 attaches the first person flashlight to the "flashlight" attachment of the viewmodel when
// r_flashlight_attach_to_viewmodel is 1 (default). Since the viewmodel follows the controller, swinging
// a melee weapon swings the light around too. When the ConVar is 0 (or while on a ladder) the game
// uses the eye position/angles instead, which follow the HMD.
// The ConVar is FCVAR_CHEAT, so the value is written directly instead of using the console.
void VR::UpdateFlashlightAttachment()
{
    if (!m_FlashlightAttachConVarLookupDone)
    {
        m_FlashlightAttachConVarLookupDone = true;

        uintptr_t operandAddress = m_Game->m_Offsets->FlashlightAttachToViewmodel.address;
        if (operandAddress)
        {
            // The operand is the address of the ConVar's m_pParent field
            uintptr_t parentFieldAddress = *(uintptr_t *)operandAddress;
            ConVarData *conVar = (ConVarData *)(parentFieldAddress - offsetof(ConVarData, parent));

            if (conVar->name && strcmp(conVar->name, "r_flashlight_attach_to_viewmodel") == 0)
            {
                m_FlashlightAttachConVar = conVar;
                ConVarData *parent = conVar->parent ? conVar->parent : conVar;
                m_FlashlightAttachDefaultValue = parent->intValue;
            }
        }
    }

    if (!m_FlashlightAttachConVar)
        return;

    ConVarData *parent = m_FlashlightAttachConVar->parent ? m_FlashlightAttachConVar->parent : m_FlashlightAttachConVar;

    bool attachToHead = m_IsVREnabled && m_AttachFlashlightToHeadWhenMelee && m_Game->m_IsMeleeWeaponActive;
    int desiredValue = attachToHead ? 0 : m_FlashlightAttachDefaultValue;

    if (parent->intValue != desiredValue)
    {
        parent->intValue = desiredValue;
        parent->floatValue = (float)desiredValue;
    }
}

void VR::UpdateTracking()
{
    GetPoses();

    int playerIndex = m_Game->m_EngineClient->GetLocalPlayer();
    C_BasePlayer *localPlayer = (C_BasePlayer *)m_Game->GetClientEntity(playerIndex);
    if (!localPlayer)
        return;

    C_WeaponCSBase* weapon = (C_WeaponCSBase*)localPlayer->GetActiveWeapon();
    int weaponId = 0;
    if (weapon)
    {
        weaponId = weapon->GetWeaponID(); 
    }

    m_Game->m_IsMeleeWeaponActive = localPlayer->IsMeleeWeaponActive();
    m_ActiveWeaponId = weaponId;
    UpdateEmptyHand(localPlayer);

    UpdateFlashlightAttachment();

    // The flashlight and the laser sight leave from the viewmodel's attachments, kept with the weapon as it's drawn
    // (HoldViewmodelAttachment)
    m_ViewmodelRenderable = nullptr;
    if (const uintptr_t getViewModel = m_Game->m_Offsets->GetViewModel.address)
    {
        typedef void *(__thiscall *tGetViewModel)(void *player, int index);
        if (void *viewModel = ((tGetViewModel)getViewModel)(localPlayer, 0))
        {
            m_ViewmodelRenderable = (uint8_t *)viewModel + 4;
            Hooks::HookViewmodelAttachments(m_ViewmodelRenderable);
        }
    }

    // HMD tracking
    QAngle hmdAngLocal = m_HmdPose.TrackedDeviceAng;	
    Vector hmdPosLocal = m_HmdPose.TrackedDevicePos;	

    Vector deltaPosition = hmdPosLocal - m_HmdPosLocalPrev;
    Vector hmdPosCorrected = m_HmdPosCorrectedPrev + deltaPosition;

    VectorPivotXY(hmdPosCorrected, m_HmdPosCorrectedPrev, m_RotationOffset);

    m_HmdPosCorrectedPrev = hmdPosCorrected;
    m_HmdPosLocalPrev = hmdPosLocal;

    hmdAngLocal.y += m_RotationOffset;
    // Wrap angle from -180 to 180
    hmdAngLocal.y -= 360 * std::floor((hmdAngLocal.y + 180) / 360);

    QAngle::AngleVectors(hmdAngLocal, &m_HmdForward, &m_HmdRight, &m_HmdUp);				

    m_HmdPosLocalInWorld = hmdPosCorrected * m_VRScale;
    UpdateMovementTest();

    // The camera goes with the body, but for the part of the body's move that catches up with the head: a step taken
    // for real, which the body walks after (roomscale, BuildMoveCommand). The head is its neck: looking down or around
    // doesn't move the body.
    const Vector neckLocal = m_HmdPosLocalInWorld - m_HmdForward * (kEyeToNeckForward * m_VRScale) - m_HmdUp * (kEyeToNeckDown * m_VRScale);
    {
        const Vector neckWorld = m_CameraAnchor - Vector(0, 0, 64) + neckLocal;
        const Movement::Vec2 bodyMove(m_SetupOrigin.x - m_SetupOriginPrev.x, m_SetupOrigin.y - m_SetupOriginPrev.y);
        const Movement::Vec2 headFromBody(neckWorld.x - m_SetupOriginPrev.x, neckWorld.y - m_SetupOriginPrev.y);
        const Movement::Vec2 follow = m_RoomscaleEnabled ? Movement::CameraFollow(bodyMove, headFromBody, kRoomscaleOvershootMeters * m_VRScale) : bodyMove;
        m_CameraAnchor.x += follow.x;
        m_CameraAnchor.y += follow.y;
    }
    
    typedef std::chrono::duration<float, std::milli> duration;
    auto currentTime = std::chrono::steady_clock::now();
    duration elapsed = currentTime - m_PrevFrameTime;
    float deltaTime = elapsed.count();

    // Which way the body faces, for what's on it (body_items.cpp, the ammo bag, UpdateBodyGrab)
    UpdateBodyYaw(deltaTime / 1000.0f);

    m_CameraAnchor.z = m_SetupOrigin.z + m_HeightOffset;

    float totalCrouchDurationInMilliseconds = 450.0f;
    if (hasCrouchAnimationEnded == false)
    {
        m_CrouchAnimDeltaTime += deltaTime;
        if (m_CrouchAnimDeltaTime >= totalCrouchDurationInMilliseconds)
        {
            hasCrouchAnimationEnded = true;
        }
    }
    if (hasCrouchAnimationEnded == false)
    {
        m_CameraAnchor.z = cameraAnchorZBeforePhysicalCrouch;
    }
    else if (m_IsPlayerCrouched)
    {
        // When crouching in game, the camera moves 18 units down. When crouching phisically, this would make the camera go even further down. 
        // This compensates the extra deltaZ.
        float gameCrouchDeltaZ = 18;
        m_CameraAnchor.z += gameCrouchDeltaZ;
    }

    m_HmdPosAbs = m_CameraAnchor - Vector(0, 0, 64) + m_HmdPosLocalInWorld;

    // Check if camera is clipping inside wall
    CGameTrace trace;
    Ray_t ray;
    CTraceFilterSkipNPCsAndPlayers tracefilter((IHandleEntity*)localPlayer, 0);

    Vector extendedHmdPos = m_HmdPosAbs - m_SetupOrigin;
    VectorNormalize(extendedHmdPos);
    extendedHmdPos = m_HmdPosAbs + (extendedHmdPos * 10);
    ray.Init(m_SetupOrigin, extendedHmdPos);

    m_Game->m_EngineTrace->TraceRay(ray, STANDARD_TRACE_MASK, &tracefilter, &trace);
    if (trace.fraction < 1 && trace.fraction > 0)
    {
        Vector distanceInsideWall = trace.endpos - extendedHmdPos;
        m_CameraAnchor += distanceInsideWall;
        m_HmdPosAbs = m_CameraAnchor - Vector(0, 0, 64) + m_HmdPosLocalInWorld;
    }

    // For the usercmds (BuildMoveCommand): the head from the body now, and whether the body can walk
    {
        const Vector neckWorld = m_CameraAnchor - Vector(0, 0, 64) + neckLocal;
        const float runSpeed = GetLocalRunSpeed(localPlayer);
        std::lock_guard<std::mutex> lock(m_MovementMutex);
        m_MovementInputs.headFromBody = Movement::Vec2(neckWorld.x - m_SetupOrigin.x, neckWorld.y - m_SetupOrigin.y);
        m_MovementInputs.body = Movement::Vec2(m_SetupOrigin.x, m_SetupOrigin.y);
        m_MovementInputs.canWalk = localPlayer->m_hGroundEntity != -1;
        m_MovementInputs.runSpeed = runSpeed;
        m_MovementInputs.trackedMs = GetTickCount64();
    }

    // Reset camera if it somehow gets too far
    m_SetupOriginToHMD = m_HmdPosAbs - m_SetupOrigin;
    if (VectorLength(m_SetupOriginToHMD) > 150)
        ResetPosition();

    m_HmdAngAbs = hmdAngLocal;

    m_HmdPosAbsPrev = m_HmdPosAbs;
    m_SetupOriginPrev = m_SetupOrigin;

    GetViewParameters();
    m_Ipd = m_EyeToHeadTransformPosRight.x * 2;
    m_EyeZ = m_EyeToHeadTransformPosRight.z;

    // Hand tracking
    Vector leftControllerPosLocal = m_LeftControllerPose.TrackedDevicePos;											
    QAngle leftControllerAngLocal = m_LeftControllerPose.TrackedDeviceAng;

    Vector rightControllerPosLocal = m_RightControllerPose.TrackedDevicePos;
    QAngle rightControllerAngLocal = m_RightControllerPose.TrackedDeviceAng;

    Vector hmdToController = rightControllerPosLocal - hmdPosLocal;
    Vector rightControllerPosCorrected = hmdPosCorrected + hmdToController;

    Vector hmdToControllerLeft = leftControllerPosLocal - hmdPosLocal;
    Vector LeftControllerPosCorrected = hmdPosCorrected + hmdToControllerLeft;

    // When using stick turning, pivot the controllers around the HMD
    VectorPivotXY(rightControllerPosCorrected, hmdPosCorrected, m_RotationOffset);
    VectorPivotXY(LeftControllerPosCorrected, hmdPosCorrected, m_RotationOffset);

    Vector rightControllerPosLocalInWorld = rightControllerPosCorrected * m_VRScale;
    Vector leftControllerPosLocalInWorld = LeftControllerPosCorrected * m_VRScale;

    m_RightControllerPosAbs = m_CameraAnchor - Vector(0, 0, 64) + rightControllerPosLocalInWorld;
    m_LeftControllerPosAbs = m_CameraAnchor - Vector(0, 0, 64) + leftControllerPosLocalInWorld;

    rightControllerAngLocal.y += m_RotationOffset;
    // Wrap angle from -180 to 180
    rightControllerAngLocal.y -= 360 * std::floor((rightControllerAngLocal.y + 180) / 360);

    leftControllerAngLocal.y += m_RotationOffset;
    // Wrap angle from -180 to 180
    leftControllerAngLocal.y -= 360 * std::floor((leftControllerAngLocal.y + 180) / 360);

    QAngle::AngleVectors(leftControllerAngLocal, &m_LeftControllerForward, &m_LeftControllerRight, &m_LeftControllerUp);
    QAngle::AngleVectors(rightControllerAngLocal, &m_RightControllerForward, &m_RightControllerRight, &m_RightControllerUp);
    m_LeftControllerForwardRaw = m_LeftControllerForward;
    m_LeftControllerRightRaw = m_LeftControllerRight;
    m_LeftControllerUpRaw = m_LeftControllerUp;

    // The hand holds the controller at a fixed angle, whatever the aim is set to
    {
        const Vector tiltedForward = VectorRotate(m_LeftControllerForward, m_LeftControllerRight, kHandGripTilt);
        const Vector tiltedUp = VectorRotate(m_LeftControllerUp, m_LeftControllerRight, kHandGripTilt);
        m_LeftHandForward = { tiltedForward.x, tiltedForward.y, tiltedForward.z };
        m_LeftHandUp = { tiltedUp.x, tiltedUp.y, tiltedUp.z };
    }

    m_RightControllerPosAbsRaw = m_RightControllerPosAbs;
    m_RightControllerForwardRaw = m_RightControllerForward;
    m_RightControllerRightRaw = m_RightControllerRight;
    m_RightControllerUpRaw = m_RightControllerUp;

    // Adjust controller angle with value specified in config. (default 45 degrees downward.)
    m_LeftControllerForward = VectorRotate(m_LeftControllerForward, m_LeftControllerRight, m_ControllerAngleAdjustment);
    m_LeftControllerUp = VectorRotate(m_LeftControllerUp, m_LeftControllerRight, m_ControllerAngleAdjustment);

    m_RightControllerForward = VectorRotate(m_RightControllerForward, m_RightControllerRight, m_ControllerAngleAdjustment);
    m_RightControllerUp = VectorRotate(m_RightControllerUp, m_RightControllerRight, m_ControllerAngleAdjustment);

    if (m_WeaponHorizontalPositionAdjustment != 0)
    {
        m_RightControllerPosAbs = m_RightControllerPosAbs + (m_RightControllerRight * m_WeaponHorizontalPositionAdjustment);
    }

    // Using the left hand's item: the game's weapon is in the left hand (dual wield)
    ApplyLeftUseHand(m_LeftControllerForwardRaw, m_LeftControllerRightRaw, m_LeftControllerUpRaw);

    // Weapon orientation. Same as the right controller, unless the weapon is being held with 2 hands.
    Vector rightControllerForward_2handAdjust = m_RightControllerForward;
    Vector rightControllerRight_2handAdjust = m_RightControllerRight;
    Vector rightControllerUp_2handAdjust = m_RightControllerUp;
    UpdateManualReload();
    UpdateLeftGunReload();

    if (PressedDigitalAction(m_RightHandGrab) || PressedDigitalAction(m_LeftHandGrab))
    {
        Vector handsDistanceLocalInWorld = leftControllerPosLocalInWorld - rightControllerPosLocalInWorld;
        vec_t handsDistanceLocalInWorldLength = VectorLength(handsDistanceLocalInWorld);
        
        Vector handsDistanceLocalInWorldNormalized;
        VectorCopy(handsDistanceLocalInWorld, handsDistanceLocalInWorldNormalized);
        VectorNormalize(handsDistanceLocalInWorldNormalized);

        // Clamp to avoid acosf returning NaN due to floating point error
        float dotProduct_RightControllerForward_HandsDistLocalWorld = std::clamp(DotProduct(m_RightControllerForward, handsDistanceLocalInWorldNormalized), -1.0f, 1.0f);
        float rightControllerForward_HandsDistLocalWorld_Angle = RAD2DEG(acosf(dotProduct_RightControllerForward_HandsDistLocalWorld));
        float dotProduct_RightControllerUp_HandsDistLocalWorld = DotProduct(m_RightControllerUp, handsDistanceLocalInWorldNormalized);

        if (PressedDigitalAction(m_RightHandGrab))
        {
            if (isPressingRightGrab == false)
            {
                // The empty right hand reloads the left hand's gun, or holds it too when on its barrel (left_gun.cpp).
                // Its pills or kit touching another player are given or used on them instead (UpdateGiving).
                if (!m_ItemTouching[1] && !TryRightReloadLeftGun() && !TryRightHoldLeftGun())
                    UpdateBodyGrab(true, rightControllerPosLocalInWorld, weaponId);

                isPressingRightGrab = true;
            }
        }
        else 
        {
            isPressingRightGrab = false;
        }
        
        if (PressedDigitalAction(m_LeftHandGrab))
        {
            // Check if holding weapon with 2 hands
            float handsDistanceFor2HandWeapon = 15.0f;
            float handsDistanceFor2HandWeaponOffset = 5.0f;

            float angleLimitFor2HandWeapon = 15.0f;
            // The left hand takes the weapon where the weapon's animation holds it, the place it goes to once held. Only
            // when the animation has no left hand on the weapon, at a distance along it.
            bool twoHandSpotKnown = false;
            const bool atTwoHandSpot = IsAtTwoHandSpot(twoHandSpotKnown);
            const bool alongTheWeapon = handsDistanceLocalInWorldLength >= (handsDistanceFor2HandWeapon - handsDistanceFor2HandWeaponOffset)
                && handsDistanceLocalInWorldLength <= (handsDistanceFor2HandWeapon + handsDistanceFor2HandWeaponOffset)
                && rightControllerForward_HandsDistLocalWorld_Angle <= angleLimitFor2HandWeapon;
            if (ManualReloadHandIsBusy() || m_LeftUseActive || m_RightHandEmpty)
            {
                isHoldingWeapon2Handed = false;
            }
            else if (isHoldingWeapon2Handed || (twoHandSpotKnown ? atTwoHandSpot : alongTheWeapon))
            {
                // The weapon pivots around the right hand so that it keeps pointing at the left hand.
                // Only changing the forward vector (keeping the controller's up/right vectors) creates a skewed basis,
                // which twists the viewmodel and its offsets as soon as the weapon is angled away from the controller,
                // so a complete orthonormal frame is built from the hands direction instead.
                Vector handsFrameForward, handsFrameRight, handsFrameUp;
                GetTwoHandedFrame(handsDistanceLocalInWorld, m_RightControllerRight, m_RightControllerUp, handsFrameForward, handsFrameRight, handsFrameUp);

                if (isHoldingWeapon2Handed == false)
                {
                    isHoldingWeapon2Handed = true;

                    // Store the weapon orientation relative to the hands frame when the left hand grabs it,
                    // so the weapon doesn't snap and keeps the grip chosen by the player.
                    m_TwoHandForwardLocal = WorldToFrame(m_RightControllerForward, handsFrameForward, handsFrameRight, handsFrameUp);
                    m_TwoHandUpLocal = WorldToFrame(m_RightControllerUp, handsFrameForward, handsFrameRight, handsFrameUp);
                }

                // Hands direction is unreliable when the controllers are almost touching
                float minHandsDistanceFor2HandAim = 2.0f;
                if (handsDistanceLocalInWorldLength > minHandsDistanceFor2HandAim)
                {
                    rightControllerForward_2handAdjust = FrameToWorld(m_TwoHandForwardLocal, handsFrameForward, handsFrameRight, handsFrameUp);
                    rightControllerUp_2handAdjust = FrameToWorld(m_TwoHandUpLocal, handsFrameForward, handsFrameRight, handsFrameUp);

                    // Re-orthonormalize (right = forward x up, up = right x forward)
                    VectorNormalize(rightControllerForward_2handAdjust);
                    CrossProduct(rightControllerForward_2handAdjust, rightControllerUp_2handAdjust, rightControllerRight_2handAdjust);
                    VectorNormalize(rightControllerRight_2handAdjust);
                    CrossProduct(rightControllerRight_2handAdjust, rightControllerForward_2handAdjust, rightControllerUp_2handAdjust);
                }
            }

            // Check if reloading. The left hand holding its own item (dual wield) only with ReloadNeedsFreeHand off.
            float maxHandsDistanceToReload = 15.0f;

            if (!ManualReloadOwnsReloadButton()
                && (m_LeftItemIndex == 0 || !m_ReloadNeedsFreeHand)
                && isHoldingWeapon2Handed == false
                && dotProduct_RightControllerUp_HandsDistLocalWorld <= -0.9f
                && handsDistanceLocalInWorldLength <= maxHandsDistanceToReload)
            {
                isReloading = true; // Change value here so that next time it executes "ProcessInput", will perform reload action.
            }

            if (isHoldingWeapon2Handed == false && isPressingLeftGrab == false)
            {
                if (!ManualReloadHandIsBusy() && !m_ItemTouching[0])
                    UpdateBodyGrab(false, leftControllerPosLocalInWorld, weaponId);

                isPressingLeftGrab = true;
            }
        }
        else
        {
            isHoldingWeapon2Handed = false;
            isPressingLeftGrab = false;
        }
    }
    else
    {
        isHoldingWeapon2Handed = false;
        isPressingRightGrab = false;
        isPressingLeftGrab = false;
        temp = false;
    }

    // A long gun held with one hand kicks the aim up with its shots (OnRightGunShot): the gun drawn, its laser, its scope
    // and its next shots go with it, until it comes back
    m_AimKick.Update(deltaTime * 0.001f);
    m_LeftAimKick.Update(deltaTime * 0.001f);
    if (m_AimKick.Active())
    {
        const Vector right = rightControllerRight_2handAdjust;
        rightControllerForward_2handAdjust = VectorRotate(rightControllerForward_2handAdjust, right, m_AimKick.pitch);
        rightControllerUp_2handAdjust = VectorRotate(rightControllerUp_2handAdjust, right, m_AimKick.pitch);
        const Vector up = rightControllerUp_2handAdjust;
        rightControllerForward_2handAdjust = VectorRotate(rightControllerForward_2handAdjust, up, m_AimKick.yaw);
        rightControllerRight_2handAdjust = VectorRotate(rightControllerRight_2handAdjust, up, m_AimKick.yaw);
    }

    // controller angles
    QAngle::VectorAngles(m_LeftControllerForward, m_LeftControllerUp, m_LeftControllerAngAbs);
    QAngle::VectorAngles(rightControllerForward_2handAdjust, rightControllerUp_2handAdjust, m_RightControllerAngAbs);
    
    PositionAngle viewmodelOffset = localPlayer->GetViewmodelOffset();

    m_ViewmodelPosOffset = viewmodelOffset.position;
    m_ViewmodelAngOffset = viewmodelOffset.angle;

    m_ViewmodelForward = rightControllerForward_2handAdjust;
    m_ViewmodelUp = rightControllerUp_2handAdjust;

    m_ViewmodelRight = rightControllerRight_2handAdjust;

    // Viewmodel yaw offset
    m_ViewmodelForward = VectorRotate(m_ViewmodelForward, m_ViewmodelUp, m_ViewmodelAngOffset.y);
    m_ViewmodelRight = VectorRotate(m_ViewmodelRight, m_ViewmodelUp, m_ViewmodelAngOffset.y);

    // Viewmodel pitch offset
    m_ViewmodelForward = VectorRotate(m_ViewmodelForward, m_ViewmodelRight, m_ViewmodelAngOffset.x);
    m_ViewmodelUp = VectorRotate(m_ViewmodelUp, m_ViewmodelRight, m_ViewmodelAngOffset.x);

    // Viewmodel roll offset
    m_ViewmodelRight = VectorRotate(m_ViewmodelRight, m_ViewmodelForward, m_ViewmodelAngOffset.z);
    m_ViewmodelUp = VectorRotate(m_ViewmodelUp, m_ViewmodelForward, m_ViewmodelAngOffset.z);

    UpdateDualWield(localPlayer, weaponId, deltaTime * 0.001f);
    UpdateHands(deltaTime * 0.001f);
    m_Game->m_PoseRelay->Update(this, localPlayer);
}



Vector VR::GetViewAngle()
{
    return Vector( m_HmdAngAbs.x, m_HmdAngAbs.y, m_HmdAngAbs.z );
}

Vector VR::GetViewOriginLeft()
{
    Vector viewOriginLeft;

    viewOriginLeft = m_HmdPosAbs + (m_HmdForward * (-(m_EyeZ * m_VRScale)));
    viewOriginLeft = viewOriginLeft + (m_HmdRight * (-((m_Ipd * m_IpdScale * m_VRScale) / 2)));

    return viewOriginLeft;
}

Vector VR::GetViewOriginRight()
{
    Vector viewOriginRight;

    viewOriginRight = m_HmdPosAbs + (m_HmdForward * (-(m_EyeZ * m_VRScale)));
    viewOriginRight = viewOriginRight + (m_HmdRight * (m_Ipd * m_IpdScale * m_VRScale) / 2);

    return viewOriginRight;
}


// Every usercmd (Hooks::dCreateMove): its movement. The thumbstick's, its speed following how far it's pushed (the game's
// keys give it at the full speed), and the body walking to where the head is (roomscale, a step taken for real). Works in
// any game, the host doesn't need the mod: it's the player's own movement.
void VR::BuildMoveCommand(float yawDegrees, float &forward, float &side)
{
    MovementInputs in;
    {
        std::lock_guard<std::mutex> lock(m_MovementMutex);
        in = m_MovementInputs;
    }
    const unsigned long long nowMs = GetTickCount64();
    const auto now = std::chrono::steady_clock::now();
    const float seconds = std::clamp(std::chrono::duration<float>(now - m_LastMoveCommand).count(), 0.0f, 0.1f);
    m_LastMoveCommand = now;

    const float topSpeed = in.walking ? kWalkSpeed : in.ducking ? kCrouchSpeed : in.runSpeed;
    const bool testStick = nowMs < m_TestStickUntil;
    const bool stick = testStick || nowMs - in.stickMs < 250;
    if (m_AnalogMovement && stick)
    {
        const Movement::Vec2 share = testStick ? Movement::StickShare(0.0f, m_TestStickY, kStickDeadZone) : Movement::StickShare(in.stickX, in.stickY, kStickDeadZone);
        const Movement::Vec2 speeds = Movement::StickSpeeds(share, kSlowestMoveSpeed, topSpeed);
        forward = speeds.y;
        side = speeds.x;
    }

    Movement::Vec2 velocity;
    if (m_RoomscaleEnabled && nowMs - in.trackedMs < 250)
    {
        const Movement::Vec2 bodyMove = m_RoomscaleHasLastBody ? in.body - m_RoomscaleLastBody : Movement::Vec2();
        m_RoomscaleLastBody = in.body;
        m_RoomscaleHasLastBody = true;
        Movement::RoomscaleSettings settings;
        settings.startDistance = 0.12f * m_VRScale;
        settings.stopDistance = 0.03f * m_VRScale;
        settings.resumeDistance = 0.05f * m_VRScale;
        settings.minimumAsked = 0.05f * m_VRScale;
        settings.minimumSpeed = kSlowestMoveSpeed;
        velocity = m_Roomscale.Update(in.headFromBody, bodyMove, in.canWalk, topSpeed, seconds, settings);
        float roomForward, roomSide;
        Movement::ToCommand(velocity, yawDegrees, roomForward, roomSide);
        forward += roomForward;
        side += roomSide;
        if (m_AnalogMovement && stick)
            Movement::LimitSpeed(forward, side, topSpeed);
    }
    else
    {
        m_RoomscaleHasLastBody = false;
    }

    if (nowMs < m_MovementTestLogUntil && nowMs - m_MovementLogMs >= 100)
    {
        m_MovementLogMs = nowMs;
        Diagnostics::Log("movement: body %.0f %.0f, head %.1f %.1f from it%s%s, top speed %.0f, command %.0f forward %.0f side",
            in.body.x, in.body.y, in.headFromBody.x, in.headFromBody.y, m_Roomscale.Following() ? ", following" : "",
            m_Roomscale.Blocked() ? ", blocked" : "", topSpeed, forward, side);
    }
}

// l4d2vr_dual_test step_<cm>: the head goes that far to the right of where the view looks, in half a second, and stays
void VR::UpdateMovementTest()
{
    if (!m_TestHeadStartMs)
        return;
    const float progress = std::min((GetTickCount64() - m_TestHeadStartMs) / 500.0f, 1.0f);
    const Vector right(m_HmdRight.x, m_HmdRight.y, 0.0f);
    const float length = VectorLength(right);
    if (length > 0.01f && progress < 1.0f)
        m_TestHeadOffset = right * (m_TestHeadTarget * m_VRScale * progress / length);
    m_HmdPosLocalInWorld += m_TestHeadOffset;
}

void VR::ResetPosition()
{
    m_CameraAnchor += m_SetupOrigin - m_HmdPosAbs;
    m_HeightOffset += m_SetupOrigin.z - m_HmdPosAbs.z;
}

void VR::ParseConfigFile()
{
    std::ifstream configStream("VR\\config.txt");
    std::unordered_map<std::string, std::string> userConfig;

    std::string line;
    while (std::getline(configStream, line))
    {
        std::istringstream sLine(line);
        std::string key;
        if (std::getline(sLine, key, '='))
        {
            std::string value;
            if (std::getline(sLine, value))
                userConfig[key] = value;
        }
    }

    if (userConfig.empty())
        return;

    m_SnapTurning = userConfig["SnapTurning"] == "true";
    m_SnapTurnAngle = std::stof(userConfig["SnapTurnAngle"]);
    m_TurnSpeed = std::stof(userConfig["TurnSpeed"]);
    m_LeftHanded = userConfig["LeftHanded"] == "true";
    m_VRScale = std::stof(userConfig["VRScale"]);
    m_IpdScale = std::stof(userConfig["IPDScale"]);
    m_HudDistance = std::stof(userConfig["HudDistance"]);
    m_HudSize = std::stof(userConfig["HudSize"]);
    m_HudAlwaysVisible = userConfig["HudAlwaysVisible"] == "true";
    m_ControllerAngleAdjustment = std::stof(userConfig["ControllerAngleAdjustment"]);
    m_WeaponHorizontalPositionAdjustment = std::stof(userConfig["WeaponHorizontalPositionAdjustment"]);
    m_IsTrackingManualPush = userConfig["IsTrackingManualPush"] == "true";
    m_MeleeSwingVelocityRequired = std::stof(userConfig["MeleeSwingVelocityRequired"]);
    m_PlayerHeightInMeters = std::stof(userConfig["PlayerHeightInMeters"]);
    m_ActivateCrouchPhisically = userConfig["ActivateCrouchPhisically"] == "true";
    m_RoomscaleEnabled = userConfig["Roomscale"] != "false"; // enabled if missing from config
    m_AnalogMovement = userConfig["AnalogMovement"] != "false"; // enabled if missing from config
    m_UseCrouchButtonAsToggle = userConfig["UseCrouchButtonAsToggle"] == "true";
    m_AttachFlashlightToHeadWhenMelee = userConfig["AttachFlashlightToHeadWhenMelee"] != "false"; // enabled if missing from config
    m_ScopeEnabled = userConfig["ScopeEnabled"] != "false"; // enabled if missing from config

    const std::string &scopeMagnification = userConfig["ScopeMagnification"];
    m_ScopeMagnification = scopeMagnification.empty() ? 4.0f : std::clamp(std::stof(scopeMagnification), 1.0f, 20.0f);
    m_ScopeDebug = userConfig["ScopeDebug"] == "true";

    m_DiagnosticsEnabled = userConfig["DiagnosticsEnabled"] == "true";
    m_InteractWithHand = userConfig["InteractWithHand"] != "false"; // enabled if missing from config
    m_WristHudEnabled = userConfig["WristHudEnabled"] != "false"; // enabled if missing from config
    m_ManualReloadEnabled = userConfig["ManualReload"] != "false"; // enabled if missing from config
    m_ReloadNeedsFreeHand = userConfig["ReloadNeedsFreeHand"] != "false"; // enabled if missing from config
    m_WeaponSpread = userConfig["WeaponSpread"] != "false"; // enabled if missing from config
    m_LongGunsNeedBothHands = userConfig["LongGunsNeedBothHands"] != "false"; // enabled if missing from config
    m_ShareBodyMovement = userConfig["ShareBodyMovement"] != "false"; // enabled if missing from config
    m_ShowOtherPlayersMovement = userConfig["ShowOtherPlayersMovement"] != "false"; // enabled if missing from config

    // Where the magazines and shells are at the waist, meters from the eyes (UpdateManualReload)
    auto readFloat = [&userConfig](const char *key, float fallback, float minimum, float maximum)
    {
        const std::string &text = userConfig[key];
        return text.empty() ? fallback : std::clamp(std::stof(text), minimum, maximum);
    };
    m_ShowReloadMark = userConfig["ShowReloadMark"] != "false"; // enabled if missing from config
    m_SmallBodyItems = userConfig["SmallBodyItems"] != "false"; // enabled if missing from config
    m_DualWieldEnabled = userConfig["DualWield"] != "false"; // enabled if missing from config
    m_ScopeMagnificationHigh = readFloat("ScopeMagnificationHigh", m_ScopeMagnificationHigh, 1.0f, 20.0f);
    m_MeleeHandBackMeters = readFloat("MeleeHandBackMeters", m_MeleeHandBackMeters, -0.2f, 0.2f); // only by editing config.txt
    m_MagazinePouchSide = readFloat("MagazinePouchSide", m_MagazinePouchSide, 0.0f, 0.6f);
    m_MagazinePouchForward = readFloat("MagazinePouchForward", m_MagazinePouchForward, -0.3f, 0.5f);
    m_MagazinePouchBelowEyes = readFloat("MagazinePouchBelowEyes", kMagazinePouchBelowEyesPerHeight * m_PlayerHeightInMeters, 0.2f, 1.2f);

    // Where the pills and the grenade are on the body, only by editing config.txt. Far enough to reach the belt.
    const char *const itemSpotNames[2] = { "Pills", "Grenade" };
    for (int spot = 0; spot < 2; ++spot)
    {
        const BodyLayout::ItemSpot defaults = BodyLayout::DefaultItemSpot(spot, m_PlayerHeightInMeters);
        const std::string name = itemSpotNames[spot];
        m_ItemSpotForward[spot] = readFloat((name + "Forward").c_str(), defaults.forward, -0.4f, 0.5f);
        m_ItemSpotSide[spot] = readFloat((name + "Side").c_str(), defaults.side, -0.5f, 0.5f);
        m_ItemSpotBelowEyes[spot] = readFloat((name + "BelowEyes").c_str(), defaults.below, 0.1f, 1.1f);
    }
    // The pistol on the thigh, moved from where the player's height puts it
    m_HolsterForward = readFloat("HolsterForward", 0.0f, -0.3f, 0.3f);
    m_HolsterOutward = readFloat("HolsterOutward", 0.0f, -0.3f, 0.3f);
    m_HolsterUp = readFloat("HolsterUp", 0.0f, -0.3f, 0.3f);

    // Fine tuning of the hand, only by editing config.txt
    Vector handOffset = m_HandOffsetMeters;
    if (sscanf_s(userConfig["HandOffsetMeters"].c_str(), "%f,%f,%f", &handOffset.x, &handOffset.y, &handOffset.z) == 3)
        m_HandOffsetMeters = handOffset;
    QAngle handAngle = m_HandAngleOffset;
    if (sscanf_s(userConfig["HandAngleOffset"].c_str(), "%f,%f,%f", &handAngle.x, &handAngle.y, &handAngle.z) == 3)
        m_HandAngleOffset = handAngle;

    m_GrenadeThrowGesture = userConfig["GrenadeThrowGesture"] != "false"; // enabled if missing from config
    const std::string &grenadeThrowStrength = userConfig["GrenadeThrowStrength"];
    m_GrenadeThrowStrength = grenadeThrowStrength.empty() ? 2.0f : std::clamp(std::stof(grenadeThrowStrength), 0.5f, 5.0f);
}

static std::string ConfigBoolString(bool value)
{
    return value ? "true" : "false";
}

// Same style as the values in config.txt: "45.0", "0.3", "1.68"
static std::string ConfigFloatString(float value)
{
    if (fabsf(value) < 0.0005f)
        value = 0.0f;

    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%.3f", value);
    std::string result = buffer;

    size_t dot = result.find('.');
    if (dot != std::string::npos)
    {
        size_t lastDigit = result.find_last_not_of('0');
        if (lastDigit == dot)
            ++lastDigit; // keep one decimal
        result.erase(lastDigit + 1);
    }
    return result;
}

// Writes the current settings to VR\config.txt. Keeps the order of the lines and any line it doesn't know about,
// replaces the known values and appends the ones that are missing.
bool VR::SaveConfigFile()
{
    const std::vector<std::pair<std::string, std::string>> values =
    {
        { "TurnSpeed", ConfigFloatString(m_TurnSpeed) },
        { "SnapTurning", ConfigBoolString(m_SnapTurning) },
        { "SnapTurnAngle", ConfigFloatString(m_SnapTurnAngle) },
        { "LeftHanded", ConfigBoolString(m_LeftHanded) },
        { "VRScale", ConfigFloatString(m_VRScale) },
        { "IPDScale", ConfigFloatString(m_IpdScale) },
        { "HudDistance", ConfigFloatString(m_HudDistance) },
        { "HudSize", ConfigFloatString(m_HudSize) },
        { "HudAlwaysVisible", ConfigBoolString(m_HudAlwaysVisible) },
        { "ControllerAngleAdjustment", ConfigFloatString(m_ControllerAngleAdjustment) },
        { "WeaponHorizontalPositionAdjustment", ConfigFloatString(m_WeaponHorizontalPositionAdjustment) },
        { "IsTrackingManualPush", ConfigBoolString(m_IsTrackingManualPush) },
        { "MeleeSwingVelocityRequired", ConfigFloatString(m_MeleeSwingVelocityRequired) },
        { "PlayerHeightInMeters", ConfigFloatString(m_PlayerHeightInMeters) },
        { "ActivateCrouchPhisically", ConfigBoolString(m_ActivateCrouchPhisically) },
        { "Roomscale", ConfigBoolString(m_RoomscaleEnabled) },
        { "AnalogMovement", ConfigBoolString(m_AnalogMovement) },
        { "UseCrouchButtonAsToggle", ConfigBoolString(m_UseCrouchButtonAsToggle) },
        { "AttachFlashlightToHeadWhenMelee", ConfigBoolString(m_AttachFlashlightToHeadWhenMelee) },
        { "ScopeEnabled", ConfigBoolString(m_ScopeEnabled) },
        { "ScopeMagnification", ConfigFloatString(m_ScopeMagnification) },
        { "ScopeMagnificationHigh", ConfigFloatString(m_ScopeMagnificationHigh) },
        { "ScopeDebug", ConfigBoolString(m_ScopeDebug) },
        { "GrenadeThrowGesture", ConfigBoolString(m_GrenadeThrowGesture) },
        { "GrenadeThrowStrength", ConfigFloatString(m_GrenadeThrowStrength) },
        { "DiagnosticsEnabled", ConfigBoolString(m_DiagnosticsEnabled) },
        { "InteractWithHand", ConfigBoolString(m_InteractWithHand) },
        { "WristHudEnabled", ConfigBoolString(m_WristHudEnabled) },
        { "ManualReload", ConfigBoolString(m_ManualReloadEnabled) },
        { "ShowReloadMark", ConfigBoolString(m_ShowReloadMark) },
        { "ReloadNeedsFreeHand", ConfigBoolString(m_ReloadNeedsFreeHand) },
        { "WeaponSpread", ConfigBoolString(m_WeaponSpread) },
        { "LongGunsNeedBothHands", ConfigBoolString(m_LongGunsNeedBothHands) },
        { "SmallBodyItems", ConfigBoolString(m_SmallBodyItems) },
        { "DualWield", ConfigBoolString(m_DualWieldEnabled) },
        { "MagazinePouchSide", ConfigFloatString(m_MagazinePouchSide) },
        { "MagazinePouchForward", ConfigFloatString(m_MagazinePouchForward) },
        { "MagazinePouchBelowEyes", ConfigFloatString(m_MagazinePouchBelowEyes) },
        { "PillsForward", ConfigFloatString(m_ItemSpotForward[0]) },
        { "PillsSide", ConfigFloatString(m_ItemSpotSide[0]) },
        { "PillsBelowEyes", ConfigFloatString(m_ItemSpotBelowEyes[0]) },
        { "GrenadeForward", ConfigFloatString(m_ItemSpotForward[1]) },
        { "GrenadeSide", ConfigFloatString(m_ItemSpotSide[1]) },
        { "GrenadeBelowEyes", ConfigFloatString(m_ItemSpotBelowEyes[1]) },
        { "HolsterForward", ConfigFloatString(m_HolsterForward) },
        { "HolsterOutward", ConfigFloatString(m_HolsterOutward) },
        { "HolsterUp", ConfigFloatString(m_HolsterUp) },
        { "ShareBodyMovement", ConfigBoolString(m_ShareBodyMovement) },
        { "ShowOtherPlayersMovement", ConfigBoolString(m_ShowOtherPlayersMovement) },
    };

    std::vector<std::string> lines;
    {
        std::ifstream configStream("VR\\config.txt");
        std::string line;
        while (std::getline(configStream, line))
            lines.push_back(line);
    }

    std::vector<bool> written(values.size(), false);
    for (std::string &line : lines)
    {
        size_t separator = line.find('=');
        if (separator == std::string::npos)
            continue;

        std::string key = line.substr(0, separator);
        for (size_t i = 0; i < values.size(); ++i)
        {
            if (values[i].first == key)
            {
                line = key + "=" + values[i].second;
                written[i] = true;
                break;
            }
        }
    }

    for (size_t i = 0; i < values.size(); ++i)
    {
        if (!written[i])
            lines.push_back(values[i].first + "=" + values[i].second);
    }

    // Write a temporary file and swap it in, so the config reload thread never reads a half written config.txt
    {
        std::ofstream tempStream("VR\\config.txt.tmp", std::ios::trunc);
        for (size_t i = 0; i < lines.size(); ++i)
        {
            tempStream << lines[i];
            if (i + 1 < lines.size())
                tempStream << '\n';
        }

        if (!tempStream)
            return false;
    }

    if (!MoveFileExA("VR\\config.txt.tmp", "VR\\config.txt", MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return false;

    try
    {
        m_ConfigWriteTimeSavedByMenu = std::filesystem::last_write_time("VR\\config.txt").time_since_epoch().count();
    }
    catch (const std::filesystem::filesystem_error &) {}

    return true;
}

void VR::WaitForConfigUpdate()
{
    char currentDir[MAX_STR_LEN];
    GetCurrentDirectory(MAX_STR_LEN, currentDir);
    char configDir[MAX_STR_LEN];
    sprintf_s(configDir, MAX_STR_LEN, "%s\\VR\\", currentDir);
    HANDLE fileChangeHandle = FindFirstChangeNotificationA(configDir, false, FILE_NOTIFY_CHANGE_LAST_WRITE);

    std::filesystem::file_time_type configLastModified;
    while (1)
    {
        try 
        {
            // Windows only notifies of change within a directory, so extra check here for just config.txt
            auto configModifiedTime = std::filesystem::last_write_time("VR\\config.txt");
            if (configModifiedTime != configLastModified)
            {
                configLastModified = configModifiedTime;

                // Values saved by the settings menu are already applied. Reloading them could undo a change made in the menu right after the save.
                if (configModifiedTime.time_since_epoch().count() != m_ConfigWriteTimeSavedByMenu)
                    ParseConfigFile();
            }
        }
        catch (const std::invalid_argument &e)
        {
            m_Game->errorMsg("Failed to parse config.txt");
        }
        catch (const std::filesystem::filesystem_error &e)
        {
            m_Game->errorMsg("config.txt not found.");
            return;
        }
        
        FindNextChangeNotification(fileChangeHandle);
        WaitForSingleObject(fileChangeHandle, INFINITE);
        Sleep(100); // Sometimes the thread tries to read config.txt before it's finished writing
    }
}