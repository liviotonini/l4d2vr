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
#include <d3d9_vr.h>

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

    UpdatePosesAndActions();

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

    m_Input->GetActionSetHandle("/actions/main", &m_ActionSet);
    m_ActiveActionSet = {};
    m_ActiveActionSet.ulActionSet = m_ActionSet;

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

    if (m_IsVREnabled && g_D3DVR9)
    {
        // Prevents crashing at menu
        if (!m_Game->m_EngineClient->IsInGame())
        {
            IMatRenderContext *rndrContext = m_Game->m_MaterialSystem->GetRenderContext();
            rndrContext->SetRenderTarget(NULL);
            m_Game->m_CachedArmsModel = false;
            m_CreatedVRTextures = false; // Have to recreate textures otherwise some workshop maps won't render
        }
    }

    SubmitVRTextures();
    UpdatePosesAndActions();
    UpdateTracking();

    if (m_Game->m_VguiSurface->IsCursorVisible())
        ProcessMenuInput();
    else
        ProcessInput();
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
        vr::VROverlay()->ShowOverlay(m_MainMenuHandle);
        vr::VROverlay()->HideOverlay(m_HUDHandle);

        if (!m_Game->m_EngineClient->IsInGame())
        {
            vr::VRCompositor()->Submit(vr::Eye_Left, &m_VKBlankTexture.m_VRTexture, NULL, vr::Submit_Default);
            vr::VRCompositor()->Submit(vr::Eye_Right, &m_VKBlankTexture.m_VRTexture, NULL, vr::Submit_Default);
        }

        return;
    }
    vr::VROverlay()->HideOverlay(m_MainMenuHandle);

    vr::VROverlay()->SetOverlayTexture(m_HUDHandle, &m_VKHUD.m_VRTexture);

    if (m_Game->m_VguiSurface->IsCursorVisible())
    {
        // We're in the pause menu
        vr::VROverlay()->ShowOverlay(m_HUDHandle);
    }

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

    vr::TrackedDevicePose_t leftControllerPose = m_Poses[leftControllerIndex];
    vr::TrackedDevicePose_t rightControllerPose = m_Poses[rightControllerIndex];

    GetPoseData(hmdPose, m_HmdPose);
    GetPoseData(leftControllerPose, m_LeftControllerPose);
    GetPoseData(rightControllerPose, m_RightControllerPose);
}

void VR::UpdatePosesAndActions() 
{
    vr::VRCompositor()->WaitGetPoses(m_Poses, vr::k_unMaxTrackedDeviceCount, NULL, 0);
    m_Input->UpdateActionState(&m_ActiveActionSet, sizeof(vr::VRActiveActionSet_t), 1);
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

    vr::VROverlay()->SetOverlayFlag(m_HUDHandle, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, false);

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

    // TODO: Instead of ClientCmding, override Usercmd in CreateMove
    if (GetAnalogActionData(m_ActionWalk, analogActionData))		
    {
        bool pushingStickX = true;
        bool pushingStickY = true;

        if (analogActionData.y > 0.5)
        {
            m_Game->ClientCmd_Unrestricted("-back");
            m_Game->ClientCmd_Unrestricted("+forward");
        }
        else if (analogActionData.y < -0.5)
        {
            m_Game->ClientCmd_Unrestricted("-forward");
            m_Game->ClientCmd_Unrestricted("+back");
        }
        else
        {
            m_Game->ClientCmd_Unrestricted("-back");
            m_Game->ClientCmd_Unrestricted("-forward");
            pushingStickY = false;
        }

        if (analogActionData.x > 0.5)
        {
            m_Game->ClientCmd_Unrestricted("-moveleft");
            m_Game->ClientCmd_Unrestricted("+moveright");
        }
        else if (analogActionData.x < -0.5)
        {
            m_Game->ClientCmd_Unrestricted("-moveright");
            m_Game->ClientCmd_Unrestricted("+moveleft");
        }
        else
        {
            m_Game->ClientCmd_Unrestricted("-moveright");
            m_Game->ClientCmd_Unrestricted("-moveleft");
            pushingStickX = false;
        }

        m_PushingThumbstick = pushingStickX || pushingStickY;
    }
    else
    {
        m_Game->ClientCmd_Unrestricted("-forward");
        m_Game->ClientCmd_Unrestricted("-back");
        m_Game->ClientCmd_Unrestricted("-moveleft");
        m_Game->ClientCmd_Unrestricted("-moveright");
    }

    if (PressedDigitalAction(m_ActionPrimaryAttack))
    {
        m_Game->ClientCmd_Unrestricted("+attack");
    }
    else
    {
        m_Game->ClientCmd_Unrestricted("-attack");
    }

    if (PressedDigitalAction(m_ActionJump))
    {
        m_Game->ClientCmd_Unrestricted("+jump");
    }
    else
    {
        m_Game->ClientCmd_Unrestricted("-jump");
    }

    if (PressedDigitalAction(m_ActionUse))
    {
        m_Game->ClientCmd_Unrestricted("+use");
    }
    else
    {
        m_Game->ClientCmd_Unrestricted("-use");
    }

    if (PressedDigitalAction(m_ActionReload) || isReloading)
    {
        m_Game->ClientCmd_Unrestricted("+reload");
        isReloading = false;
    }
    else
    {
        m_Game->ClientCmd_Unrestricted("-reload");
    }

    //if (PressedDigitalAction(m_ActionSecondaryAttack) || (m_IsTrackingManualPush && VectorLength(m_LeftControllerPose.TrackedDeviceVel) > 5.0))
    if (PressedDigitalAction(m_ActionSecondaryAttack) || (m_IsTrackingManualPush && (VectorLength(m_LeftControllerPose.TrackedDeviceVel) > 4.0 || ( !m_Game->m_IsMeleeWeaponActive && VectorLength(m_RightControllerPose.TrackedDeviceVel) > 4.0))))
    {
        m_Game->ClientCmd_Unrestricted("+attack2");       
    }
    else
    {
        m_Game->ClientCmd_Unrestricted("-attack2");
    }

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

    if (isSendingCrouchCommand || m_IsPlayerCrouched)
    {
        m_Game->ClientCmd_Unrestricted("+duck");
    }
    else 
    {
        m_Game->ClientCmd_Unrestricted("-duck");
    }

    if (PressedDigitalAction(m_ActionFlashlight, true))
    {
        m_Game->ClientCmd_Unrestricted("impulse 100");
    }

    if (PressedDigitalAction(m_Spray, true))
    {
        m_Game->ClientCmd_Unrestricted("impulse 201");
    }

    if (PressedDigitalAction(m_ToggleWalkSpeed))
    {
        if (isPressingToggleWalkSpeed == false)
        {
            isWalkingSlowly = !isWalkingSlowly;
            if (isWalkingSlowly)
            {
                m_Game->ClientCmd_Unrestricted("+speed");
            }
            else
            {
                m_Game->ClientCmd_Unrestricted("-speed");
            }
            isPressingToggleWalkSpeed = true;
        }
    }
    else 
    {
        isPressingToggleWalkSpeed = false;
    }
    
    bool isControllerVertical = m_RightControllerAngAbs.x > 60 || m_RightControllerAngAbs.x < -45;
    if ((PressedDigitalAction(m_ShowHUD) || PressedDigitalAction(m_Scoreboard) || isControllerVertical || m_HudAlwaysVisible)
        && m_RenderedHud)
    {
        if (!vr::VROverlay()->IsOverlayVisible(m_HUDHandle) || m_HudAlwaysVisible)
            RepositionOverlays();

        if (PressedDigitalAction(m_Scoreboard))
            m_Game->ClientCmd_Unrestricted("+showscores");
        else
            m_Game->ClientCmd_Unrestricted("-showscores");

        vr::VROverlay()->ShowOverlay(m_HUDHandle);
    }
    else
    {
        vr::VROverlay()->HideOverlay(m_HUDHandle);
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

    UpdateFlashlightAttachment();

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

    // Roomscale setup
    Vector cameraMovingDirection = m_SetupOrigin - m_SetupOriginPrev;
    Vector cameraToPlayer = m_HmdPosAbsPrev - m_SetupOriginPrev;
    cameraMovingDirection.z = 0;
    cameraToPlayer.z = 0;
    float cameraFollowing = DotProduct(cameraMovingDirection, cameraToPlayer);
    float cameraDistance = VectorLength(cameraToPlayer);

    if (localPlayer->m_hGroundEntity != -1 && localPlayer->m_vecVelocity.IsZero())
        m_RoomscaleActive = true;

    // TODO: Get roomscale to work while using thumbstick
    if ((cameraFollowing < 0 && cameraDistance > 1) || (m_PushingThumbstick))
        m_RoomscaleActive = false;

    if (!m_RoomscaleActive)
        m_CameraAnchor += m_SetupOrigin - m_SetupOriginPrev;
    
    typedef std::chrono::duration<float, std::milli> duration;
    auto currentTime = std::chrono::steady_clock::now();
    duration elapsed = currentTime - m_PrevFrameTime; 
    float deltaTime = elapsed.count();

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

    rightControllerAngLocal.y += m_RotationOffset;
    // Wrap angle from -180 to 180
    rightControllerAngLocal.y -= 360 * std::floor((rightControllerAngLocal.y + 180) / 360);

    leftControllerAngLocal.y += m_RotationOffset;
    // Wrap angle from -180 to 180
    leftControllerAngLocal.y -= 360 * std::floor((leftControllerAngLocal.y + 180) / 360);

    QAngle::AngleVectors(leftControllerAngLocal, &m_LeftControllerForward, &m_LeftControllerRight, &m_LeftControllerUp);			
    QAngle::AngleVectors(rightControllerAngLocal, &m_RightControllerForward, &m_RightControllerRight, &m_RightControllerUp);	

    // Adjust controller angle with value specified in config. (default 45 degrees downward.)
    m_LeftControllerForward = VectorRotate(m_LeftControllerForward, m_LeftControllerRight, m_ControllerAngleAdjustment);
    m_LeftControllerUp = VectorRotate(m_LeftControllerUp, m_LeftControllerRight, m_ControllerAngleAdjustment);

    m_RightControllerForward = VectorRotate(m_RightControllerForward, m_RightControllerRight, m_ControllerAngleAdjustment);
    m_RightControllerUp = VectorRotate(m_RightControllerUp, m_RightControllerRight, m_ControllerAngleAdjustment);

    if (m_WeaponHorizontalPositionAdjustment != 0)
    {
        m_RightControllerPosAbs = m_RightControllerPosAbs + (m_RightControllerRight * m_WeaponHorizontalPositionAdjustment);
    }

    // Weapon orientation. Same as the right controller, unless the weapon is being held with 2 hands.
    Vector rightControllerForward_2handAdjust = m_RightControllerForward;
    Vector rightControllerRight_2handAdjust = m_RightControllerRight;
    Vector rightControllerUp_2handAdjust = m_RightControllerUp;
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

        Vector hmdToControllerWorld = rightControllerPosLocalInWorld - m_HmdPosLocalInWorld;
        
        Vector hmdToControllerWorld2DNormalized;
        VectorCopy(hmdToControllerWorld, hmdToControllerWorld2DNormalized);
        hmdToControllerWorld2DNormalized.z = 0;
        VectorNormalize(hmdToControllerWorld2DNormalized);

        float dotProduct_hmdToControllerWorld2DNorm_HmdRight = DotProduct2D(hmdToControllerWorld2DNormalized, m_HmdRight);
        float dotProduct_hmdToControllerWorld2DNorm_HmdForward = DotProduct2D(hmdToControllerWorld2DNormalized, m_HmdForward);

        float hmdToControllerWorldLength = VectorLength(hmdToControllerWorld);
        float hmdToControllerWorldZDist = m_HmdPosLocalInWorld.z - rightControllerPosLocalInWorld.z;

        Vector hmdToControllerWorldLeft = leftControllerPosLocalInWorld - m_HmdPosLocalInWorld;

        Vector hmdToControllerWorldLeft2DNormalized;
        VectorCopy(hmdToControllerWorldLeft, hmdToControllerWorldLeft2DNormalized);
        hmdToControllerWorldLeft2DNormalized.z = 0;
        VectorNormalize(hmdToControllerWorldLeft2DNormalized);

        float dotProduct_hmdToControllerWorldLeft2DNorm_HmdRight = DotProduct2D(hmdToControllerWorldLeft2DNormalized, m_HmdRight);
        float dotProduct_hmdToControllerWorldLeft2DNorm_HmdForward = DotProduct2D(hmdToControllerWorldLeft2DNormalized, m_HmdForward);

        float hmdToControllerWorldLeftLength = VectorLength(hmdToControllerWorldLeft);
        float hmdToControllerWorldLeftZDist = m_HmdPosLocalInWorld.z - leftControllerPosLocalInWorld.z;

         //if (temp == false)
         //{
            //writeToFile("");
            //writeToFile("######");
            //writeToFile("######");

             //writeToFile("WeaponId", weaponId);
            //writeToFile("hmdPosLocal", hmdPosLocal);
            //writeToFile("hmdAngLocal", hmdAngLocal);
            //writeToFile("m_RotationOffset", m_RotationOffset);
            //writeToFile("m_HmdPosLocalInWorld", m_HmdPosLocalInWorld);

            //writeToFile("hmdToControllerWorldLength", hmdToControllerWorldLength);
            //writeToFile("dotProduct_hmdToControllerWorld2DNorm_HmdRight", dotProduct_hmdToControllerWorld2DNorm_HmdRight);
            //writeToFile("dotProduct_hmdToControllerWorld2DNorm_HmdForward", dotProduct_hmdToControllerWorld2DNorm_HmdForward);
            //writeToFile("hmdToControllerWorldZDist", hmdToControllerWorldZDist);
            //
            //writeToFile("hmdToControllerWorldLeftLength", hmdToControllerWorldLeftLength);
            //writeToFile("dotProduct_hmdToControllerWorldLeft2DNorm_HmdRight", dotProduct_hmdToControllerWorldLeft2DNorm_HmdRight);
            //writeToFile("dotProduct_hmdToControllerWorldLeft2DNorm_HmdForward", dotProduct_hmdToControllerWorldLeft2DNorm_HmdForward);
            //writeToFile("hmdToControllerWorldLeftZDist", hmdToControllerWorldLeftZDist);


            //writeToFile("deltaTime", deltaTime);
            //writeToFile("m_HeightOffset", m_HeightOffset);
            //writeToFile("m_SetupOrigin.z", m_SetupOrigin.z);

            //temp = true;
        //}

        if (PressedDigitalAction(m_RightHandGrab))
        {
            if (isPressingRightGrab == false)
            {
                // Check if grabbing primary weapon
                float hmdToControllerShoulderMaxZDist = 8.0f;
                float hmdToControllerShoulderDist = 12.0f;
                float hmdToControllerShoulderDistOffset = 2.0f;

                if (hmdToControllerWorldLength >= (hmdToControllerShoulderDist - hmdToControllerShoulderDistOffset)
                    && hmdToControllerWorldLength <= (hmdToControllerShoulderDist + hmdToControllerShoulderDistOffset)
                    && dotProduct_hmdToControllerWorld2DNorm_HmdRight >= 0.1f
                    && dotProduct_hmdToControllerWorld2DNorm_HmdForward <= -0.7f
                    )
                {
                    if (weaponId == (int)weapon->AK47 || weaponId == (int)weapon->AUTOSHOTGUN || weaponId == (int)weapon->AWP
                        || weaponId == (int)weapon->GRENADE_LAUNCHER || weaponId == (int)weapon->HUNTING_RIFLE
                        || weaponId == (int)weapon->M16A1 || weaponId == (int)weapon->M60
                        || weaponId == (int)weapon->MAC10 || weaponId == (int)weapon->MACHINEGUN || weaponId == (int)weapon->MP5
                        || weaponId == (int)weapon->PUMPSHOTGUN || weaponId == (int)weapon->SHOTGUN_CHROME || weaponId == (int)weapon->SNIPER_MILITARY
                        || weaponId == (int)weapon->UZI || weaponId == (int)weapon->AUTOSHOTGUN || weaponId == (int)weapon->SCAR || weaponId == (int)weapon->SPAS
                        || weaponId == (int)weapon->SG552 || weaponId == (int)weapon->SCOUT
                        )
                    {
                        m_Game->ClientCmd_Unrestricted("slot2");
                    }
                    else
                    {
                        m_Game->ClientCmd_Unrestricted("slot1");
                    }
                }

                // Check in grabbing secondary weapon
                float hmdToControllerLegHolsterDist = m_PlayerHeightInMeters * 22.5f;
                float hmdToControllerLegHolsterDistOffset = 5.0f;
                float hmdToControllerLegMaxZDist = m_PlayerHeightInMeters * 21.5f;
                float hmdToControllerLegMaxZDistOffset = 5.0f;

                if (hmdToControllerWorldLength >= (hmdToControllerLegHolsterDist - hmdToControllerLegHolsterDistOffset)
                    && hmdToControllerWorldLength <= (hmdToControllerLegHolsterDist + hmdToControllerLegHolsterDistOffset)
                    && dotProduct_hmdToControllerWorld2DNorm_HmdRight >= 0.8f
                    && hmdToControllerWorldZDist >= (hmdToControllerLegMaxZDist - hmdToControllerLegMaxZDistOffset)
                    && hmdToControllerWorldZDist <= (hmdToControllerLegMaxZDist + hmdToControllerLegMaxZDistOffset))
                {
                    if (weaponId == (int)weapon->MELEE || weaponId == (int)weapon->PISTOL
                        || weaponId == (int)weapon->MAGNUM || weaponId == (int)weapon->CHAINSAW)
                    {
                        m_Game->ClientCmd_Unrestricted("slot1");
                    }
                    else
                    {
                        m_Game->ClientCmd_Unrestricted("slot2");
                    }
                }

                // Check if grabbing Medkit
                float hmdToControllerBackDist = m_PlayerHeightInMeters * 11.9f;
                float hmdToControllerBackDistOffset = 5.0f;

                if (dotProduct_hmdToControllerWorld2DNorm_HmdForward < -0.9f
                    && hmdToControllerWorldZDist >= (hmdToControllerBackDist - hmdToControllerBackDistOffset)
                    && hmdToControllerWorldZDist <= (hmdToControllerBackDist + hmdToControllerBackDistOffset))
                {
                    m_Game->ClientCmd_Unrestricted("slot4");
                }

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
            if (isHoldingWeapon2Handed ||
                (handsDistanceLocalInWorldLength >= (handsDistanceFor2HandWeapon - handsDistanceFor2HandWeaponOffset)
                    && handsDistanceLocalInWorldLength <= (handsDistanceFor2HandWeapon + handsDistanceFor2HandWeaponOffset)
                    && rightControllerForward_HandsDistLocalWorld_Angle <= angleLimitFor2HandWeapon))
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

            // Check if reloading
            float maxHandsDistanceToReload = 15.0f;

            if (isHoldingWeapon2Handed == false
                && dotProduct_RightControllerUp_HandsDistLocalWorld <= -0.9f
                && handsDistanceLocalInWorldLength <= maxHandsDistanceToReload)
            {
                isReloading = true; // Change value here so that next time it executes "ProcessInput", will perform reload action.
            }

            if (isHoldingWeapon2Handed == false && isPressingLeftGrab == false)
            {
                // Check if grabbing Bomb
                float hmdToControllerLeftShoulderMaxZDist = 8.0f;
                float hmdToControllerLeftShoulderDist = 14.0f;
                float hmdToControllerLeftShoulderDistOffset = 2.0f;

                if (isHoldingWeapon2Handed == false
                    && hmdToControllerWorldLeftLength >= (hmdToControllerLeftShoulderDist - hmdToControllerLeftShoulderDistOffset)
                    && hmdToControllerWorldLeftLength <= (hmdToControllerLeftShoulderDist + hmdToControllerLeftShoulderDistOffset)
                    && dotProduct_hmdToControllerWorldLeft2DNorm_HmdRight <= -0.4f
                    //&& dotProduct_hmdToControllerWorldLeft2DNorm_HmdForward <= -0.7f
                    && hmdToControllerWorldLeftZDist <= hmdToControllerLeftShoulderMaxZDist)
                {
                    if (weaponId == (int)weapon->PIPE_BOMB || weaponId == (int)weapon->VOMITJAR || weaponId == (int)weapon->MOLOTOV)
                    {
                        m_Game->ClientCmd_Unrestricted("slot5");
                    }
                    else
                    {
                        m_Game->ClientCmd_Unrestricted("slot3");
                    }
                }

                // Check if grabbing Pills / Adrenaline
                float hmdToControllerLeftLegHolsterDist = m_PlayerHeightInMeters * 22.5f;
                float hmdToControllerLeftLegHolsterDistOffset = 5.0f;
                float hmdToControllerLeftLegMaxZDist = m_PlayerHeightInMeters * 21.5f;
                float hmdToControllerLeftLegMaxZDistOffset = 5.0f;

                if (isHoldingWeapon2Handed == false
                    && hmdToControllerWorldLeftLength >= (hmdToControllerLeftLegHolsterDist - hmdToControllerLeftLegHolsterDistOffset)
                    && hmdToControllerWorldLeftLength <= (hmdToControllerLeftLegHolsterDist + hmdToControllerLeftLegHolsterDistOffset)
                    && dotProduct_hmdToControllerWorldLeft2DNorm_HmdRight <= -0.8f
                    && hmdToControllerWorldLeftZDist >= (hmdToControllerLeftLegMaxZDist - hmdToControllerLeftLegMaxZDistOffset)
                    && hmdToControllerWorldLeftZDist <= (hmdToControllerLeftLegMaxZDist + hmdToControllerLeftLegMaxZDistOffset))
                {
                    if (weaponId == (int)weapon->PAIN_PILLS || weaponId == (int)weapon->ADRENALINE)
                    {
                        m_Game->ClientCmd_Unrestricted("slot3");
                    }
                    else
                    {
                        m_Game->ClientCmd_Unrestricted("slot5");
                    }
                }

                // Check if grabbing Medkit
                float hmdToControllerLeftBackDist = m_PlayerHeightInMeters * 11.9f; // 0.5f
                float hmdToControllerLeftBackDistOffset = 5.0f;

                if (isHoldingWeapon2Handed == false
                    && dotProduct_hmdToControllerWorldLeft2DNorm_HmdForward < -0.9f
                    && hmdToControllerWorldLeftZDist >= (hmdToControllerLeftBackDist - hmdToControllerLeftBackDistOffset)
                    && hmdToControllerWorldLeftZDist <= (hmdToControllerLeftBackDist + hmdToControllerLeftBackDistOffset))
                {
                    m_Game->ClientCmd_Unrestricted("slot4");
                }

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
    m_HideArms = userConfig["HideArms"] == "true";
    m_HudDistance = std::stof(userConfig["HudDistance"]);
    m_HudSize = std::stof(userConfig["HudSize"]);
    m_HudAlwaysVisible = userConfig["HudAlwaysVisible"] == "true";
    m_ControllerAngleAdjustment = std::stof(userConfig["ControllerAngleAdjustment"]);
    m_WeaponHorizontalPositionAdjustment = std::stof(userConfig["WeaponHorizontalPositionAdjustment"]);
    m_IsTrackingManualPush = userConfig["IsTrackingManualPush"] == "true";
    m_MeleeSwingVelocityRequired = std::stof(userConfig["MeleeSwingVelocityRequired"]);
    m_PlayerHeightInMeters = std::stof(userConfig["PlayerHeightInMeters"]);
    m_ActivateCrouchPhisically = userConfig["ActivateCrouchPhisically"] == "true";
    m_UseCrouchButtonAsToggle = userConfig["UseCrouchButtonAsToggle"] == "true";
    m_AttachFlashlightToHeadWhenMelee = userConfig["AttachFlashlightToHeadWhenMelee"] != "false"; // enabled if missing from config
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