#pragma once
#include "openvr.h"
#include "vector.h"
#include "haptics.h"
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "hand_rig.h"

#define MAX_STR_LEN 256

class Game;
class IDirect3DTexture9;
class IDirect3DSurface9;
class ITexture;
class SettingsMenu;
struct ConVarData;


struct TrackedDevicePoseData 
{
	std::string TrackedDeviceName;
	Vector TrackedDevicePos;
	Vector TrackedDeviceVel;
	QAngle TrackedDeviceAng;
	QAngle TrackedDeviceAngVel;
};

struct SharedTextureHolder 
{
	vr::VRVulkanTextureData_t m_VulkanData;
	vr::Texture_t m_VRTexture;
};


class VR
{
public:
	Game *m_Game = nullptr;

	vr::IVRSystem *m_System = nullptr;
	vr::IVRInput *m_Input = nullptr;
	vr::IVROverlay *m_Overlay = nullptr;

	vr::VROverlayHandle_t m_MainMenuHandle;
	vr::VROverlayHandle_t m_HUDHandle;

	float m_HorizontalOffsetLeft;
	float m_VerticalOffsetLeft;
	float m_HorizontalOffsetRight;
	float m_VerticalOffsetRight;

	uint32_t m_RenderWidth;
	uint32_t m_RenderHeight;
	float m_Aspect;
	float m_Fov;

	vr::VRTextureBounds_t m_TextureBounds[2];
	vr::TrackedDevicePose_t m_Poses[vr::k_unMaxTrackedDeviceCount];

	Vector m_EyeToHeadTransformPosLeft = { 0,0,0 };
	Vector m_EyeToHeadTransformPosRight = { 0,0,0 };

	Vector m_HmdForward;
	Vector m_HmdRight;
	Vector m_HmdUp;

	Vector m_HmdPosLocalInWorld = { 0,0,0 };

	Vector m_LeftControllerForward;
	Vector m_LeftControllerRight;
	Vector m_LeftControllerUp;

	Vector m_RightControllerForward;
	Vector m_RightControllerRight;
	Vector m_RightControllerUp;

	Vector m_ViewmodelForward;
	Vector m_ViewmodelRight;
	Vector m_ViewmodelUp;

	Vector m_HmdPosAbs = { 0,0,0 };
	Vector m_HmdPosAbsPrev = { 0,0,0 };
	QAngle m_HmdAngAbs;

	Vector m_HmdPosCorrectedPrev = { 0,0,0 };
	Vector m_HmdPosLocalPrev = { 0,0,0 };

	Vector m_SetupOrigin = { 0,0,0 };
	Vector m_SetupOriginPrev = { 0,0,0 };
	Vector m_CameraAnchor = { 0,0,0 };
	Vector m_SetupOriginToHMD = { 0,0,0 };

	float m_HeightOffset = 0.0;
	bool m_RoomscaleActive = false;

	Vector m_LeftControllerPosAbs;
	QAngle m_LeftControllerAngAbs;
	Vector m_RightControllerPosAbs;
	// Right controller pose in the game world without the angle and horizontal position adjustments (the real controller)
	Vector m_RightControllerPosAbsRaw = { 0,0,0 };
	Vector m_RightControllerForwardRaw = { 1,0,0 };
	Vector m_RightControllerRightRaw = { 0,-1,0 };
	Vector m_RightControllerUpRaw = { 0,0,1 };
	vr::TrackedDeviceIndex_t m_RightControllerDeviceIndex = vr::k_unTrackedDeviceIndexInvalid;
	vr::TrackedDeviceIndex_t m_LeftControllerDeviceIndex = vr::k_unTrackedDeviceIndexInvalid;
	QAngle m_RightControllerAngAbs = { 0,0,0 }; // Weapon aim angles. Also read directly by the patched laser sight code in client.dll

	Vector m_ViewmodelPosOffset;
	QAngle m_ViewmodelAngOffset;

	float m_Ipd;																	
	float m_EyeZ;

	Vector m_IntendedPositionOffset = { 0,0,0 };

	enum TextureID
	{
		Texture_None = -1,
		Texture_LeftEye,
		Texture_RightEye,
		Texture_HUD,
		Texture_Blank,
		Texture_Scope
	};

	ITexture *m_LeftEyeTexture;
	ITexture *m_RightEyeTexture;
	ITexture *m_HUDTexture;
	ITexture *m_BlankTexture = nullptr;
	ITexture *m_ScopeTexture = nullptr;

	IDirect3DSurface9 *m_D9LeftEyeSurface;
	IDirect3DSurface9 *m_D9RightEyeSurface;
	IDirect3DSurface9 *m_D9HUDSurface;
	IDirect3DSurface9 *m_D9BlankSurface;
	IDirect3DSurface9 *m_D9ScopeSurface = nullptr;

	SharedTextureHolder m_VKLeftEye;
	SharedTextureHolder m_VKRightEye;
	SharedTextureHolder m_VKBackBuffer;
	SharedTextureHolder m_VKHUD;
	SharedTextureHolder m_VKBlankTexture;
	SharedTextureHolder m_VKScope;

	bool m_IsVREnabled = false;
	bool m_IsInitialized = false;
	bool m_CrashHandlerReinstalled = false;
	bool m_RenderedNewFrame = false;
	bool m_RenderedHud = false;
	bool m_CreatedVRTextures = false;
	TextureID m_CreatingTextureID = Texture_None;

	bool m_PressedTurn = false;
	bool m_PushingThumbstick = false;

	// action set
	vr::VRActionSetHandle_t m_ActionSet;
	vr::VRActionSetHandle_t m_ActionSetBase = vr::k_ulInvalidActionSetHandle; // poses, skeletons and the vibrations
	vr::VRActiveActionSet_t m_ActiveActionSets[2] = {};
	int m_ActiveActionSetCount = 1;
	Haptics m_Haptics;

	// actions
	vr::VRActionHandle_t m_ActionJump;
	vr::VRActionHandle_t m_ActionPrimaryAttack;
	vr::VRActionHandle_t m_ActionSecondaryAttack;
	vr::VRActionHandle_t m_ActionReload;
	vr::VRActionHandle_t m_ActionWalk;
	vr::VRActionHandle_t m_ActionTurn;
	vr::VRActionHandle_t m_ActionUse;
	vr::VRActionHandle_t m_ActionNextItem;
	vr::VRActionHandle_t m_ActionPrevItem;
	vr::VRActionHandle_t m_ActionResetPosition;
	vr::VRActionHandle_t m_ActionCrouch;
	vr::VRActionHandle_t m_ActionFlashlight;
	vr::VRActionHandle_t m_ActionActivateVR;
	vr::VRActionHandle_t m_MenuSelect;
	vr::VRActionHandle_t m_MenuBack;
	vr::VRActionHandle_t m_MenuUp;
	vr::VRActionHandle_t m_MenuDown;
	vr::VRActionHandle_t m_MenuLeft;
	vr::VRActionHandle_t m_MenuRight;
	vr::VRActionHandle_t m_Spray; 
	vr::VRActionHandle_t m_Scoreboard;
	vr::VRActionHandle_t m_ShowHUD;
	vr::VRActionHandle_t m_Pause;
	vr::VRActionHandle_t m_LeftHandGrab;
	vr::VRActionHandle_t m_RightHandGrab;
	vr::VRActionHandle_t m_ToggleWalkSpeed;
	vr::VRActionHandle_t m_ActionSkeletonLeft = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionSkeletonRight = vr::k_ulInvalidActionHandle;

	TrackedDevicePoseData m_HmdPose;
	TrackedDevicePoseData m_LeftControllerPose;
	TrackedDevicePoseData m_RightControllerPose;

	float m_RotationOffset = 0;
	std::chrono::steady_clock::time_point m_PrevFrameTime;


	float m_TurnSpeed = 0.3;
	bool m_SnapTurning = false;
	float m_SnapTurnAngle = 45.0;
	bool m_LeftHanded = false;
	float m_VRScale = 43.2;
	float m_IpdScale = 1.0;
	float m_HudDistance = 1.3;
	float m_HudSize = 1.1;
	bool m_HudAlwaysVisible = false;
	bool m_IsTrackingManualPush = false;
	float m_ControllerAngleAdjustment = -45.0;
	float m_WeaponHorizontalPositionAdjustment = 0;
	float m_MeleeSwingVelocityRequired = 1.1;
	float m_PlayerHeightInMeters = 1.8;
	bool m_ActivateCrouchPhisically = false;
	bool m_UseCrouchButtonAsToggle = false;
	bool m_AttachFlashlightToHeadWhenMelee = true;
	bool m_ScopeEnabled = true;
	float m_ScopeMagnification = 4.0f;
	// Shows the scope whenever a sniper rifle is held (even when not looking through it) and writes its state
	// to VR\scope_debug.txt once per second
	bool m_ScopeDebug = false;
	// Throw the molotov, pipe bomb and bile jar with the arm: hold the trigger, swing and let go
	bool m_GrenadeThrowGesture = true;
	// Writes the time of each part of the frame to VR\\perf.csv once per second
	bool m_DiagnosticsEnabled = false;
	// Doors, items and teammates are used by pointing the weapon hand at them instead of looking at them
	bool m_InteractWithHand = true;
	// Makes fast throws stronger, up to this many times the speed of the hand (the game's own throw is about 17 m/s).
	// Gentle throws keep the real speed of the hand.
	float m_GrenadeThrowStrength = 2.0f;

	// Grenade throw. The release is detected in ProcessInput (VR::Update) and the throw is used by the server hooks
	// (Hooks::dStartGrenadeThrow and dEmitGrenade) and sent to the server in the usercmds (Hooks::dWriteUsercmd),
	// both on the game thread, so it's behind a mutex.
	struct GrenadeThrow
	{
		bool pending = false;                        // not used by the local server yet
		bool gesture = false;                        // thrown with the arm, otherwise aimed with the controller
		int sequence = 0;                            // 1 to 3, tells the server apart the throws it receives
		std::chrono::steady_clock::time_point time;  // when the trigger was let go
		Vector armVelocity = { 0,0,0 };              // game units per second, without the player's velocity
		float handSpeed = 0.0f;                      // m/s
	};
	// How long after the release the server can still use a throw (the game throws later when the trigger is let go
	// before the pin is pulled)
	static constexpr std::chrono::milliseconds kGrenadeThrowValidTime{ 1500 };
	// Speed of the game's own throw, units/s
	static constexpr float kGameThrowSpeed = 750.0f;
	int m_GrenadeThrowSequence = 0;
	struct HandVelocitySample
	{
		std::chrono::steady_clock::time_point time;
		Vector velocity; // m/s, game axes, relative to the player
	};
	std::mutex m_GrenadeThrowMutex;
	GrenadeThrow m_GrenadeThrow;
	std::deque<HandVelocitySample> m_HandVelocityHistory;
	bool m_GrenadeTriggerHeld = false;
	std::chrono::steady_clock::time_point m_GrenadeReleaseTime;

	// Cancelling a throw: while the trigger is held, the other hand presses its trigger (shove) next to the grenade.
	// The grenade is put away and taken out again (putting it away puts the pin back, CBaseCSGrenade::Holster),
	// and the trigger has to be let go before the next throw.
	enum GrenadeCancelState
	{
		GrenadeCancel_None,
		GrenadeCancel_PuttingAway,        // switching to the secondary weapon, the attack stays held so it doesn't throw
		GrenadeCancel_WaitTriggerRelease, // grenade taken out again, the attack is held back until the trigger is let go
	};
	GrenadeCancelState m_GrenadeCancelState = GrenadeCancel_None;
	std::chrono::steady_clock::time_point m_GrenadeCancelTime;
	bool m_ShoveButtonDown = false;
	bool m_ShoveButtonUsedForCancel = false; // the shove button press that cancelled doesn't shove

	// Scope of the sniper rifles: the game's view is rendered again from the scope into m_ScopeTexture,
	// which is shown on two overlays at the eyepiece (the image and a mask with the black ring and the reticle)
	static constexpr int kScopeTextureSize = 768;
	// Degrees from the pose SteamVR reports for the controller to the direction the fingers point when holding it
	static constexpr float kHandGripTilt = -45.0f;
	vr::VROverlayHandle_t m_ScopeHandle = vr::k_ulOverlayHandleInvalid;
	vr::VROverlayHandle_t m_ScopeMaskHandle = vr::k_ulOverlayHandleInvalid;
	int m_ScopeOverlayCreateError = 0;
	int m_ActiveWeaponId = 0;
	// Written by Hooks::dRenderView (UpdateScope), read by SubmitVRTextures
	bool m_ScopeRendered = false;
	float m_ScopeAlpha = 0.0f;
	float m_ScopeOverlayWidth = 0.05f;
	vr::HmdMatrix34_t m_ScopeOverlayTransform = {};
	Vector m_ScopeCameraPos = { 0,0,0 };
	QAngle m_ScopeCameraAng = { 0,0,0 };
	float m_ScopeFov = 10.0f;

	// r_flashlight_attach_to_viewmodel ConVar (found through FlashlightAttachToViewmodel offset)
	ConVarData *m_FlashlightAttachConVar = nullptr;
	bool m_FlashlightAttachConVarLookupDone = false;
	int m_FlashlightAttachDefaultValue = 1;

	// "L4D2VR" tab in the SteamVR dashboard for changing the settings above while playing
	SettingsMenu *m_SettingsMenu = nullptr;
	// Write time of the last config.txt saved by SaveConfigFile(), so WaitForConfigUpdate() doesn't reload our own save
	std::atomic<long long> m_ConfigWriteTimeSavedByMenu{ 0 };

	// The two hands (hands.cpp). Only the hands of the first person arms are drawn. The right one stays on the weapon,
	// the left one follows the controller except when the weapon needs it.
	struct HandModel
	{
		HandRig rig;
		bool valid = false;
	};
	std::unordered_map<const void *, HandModel> m_HandModels;
	HandMath::Mat34 m_LeftHandTarget;       // hand bone, world
	HandMath::Vec3 m_LeftHandForward;       // fingers
	HandMath::Vec3 m_LeftHandUp;            // thumb side
	HandMath::Vec3 m_LeftHandWrist;
	float m_LeftFingerCurls[HandRig::FingerCount] = { HandRig::kRelaxedCurl, HandRig::kRelaxedCurl, HandRig::kRelaxedCurl, HandRig::kRelaxedCurl, HandRig::kRelaxedCurl };
	float m_LeftHandFollow = 0.0f;          // 0 = the weapon's animation, 1 = the controller
	bool m_ViewmodelHasTwoWeapons = false;  // dual pistols, both hands are holding one
	// Where the wrist is from the controller and how the hand is turned, see DefaultHandOffset. Not in the menu.
	Vector m_HandOffsetMeters = { -0.10f, 0.0f, -0.08f }; // forward, right, up
	QAngle m_HandAngleOffset = { 0.0f, 0.0f, 0.0f };

	// HUD on the wrist of the hand that doesn't hold the weapon (wrist_hud.cpp)
	bool m_WristHudEnabled = true;
	vr::VROverlayHandle_t m_WristHudHandle = vr::k_ulOverlayHandleInvalid;
	vr::TrackedDeviceIndex_t m_WristHudDevice = vr::k_unTrackedDeviceIndexInvalid;
	bool m_WristHudVisible = false;
	bool m_WristHudDrawn = false;
	unsigned long long m_WristHudLastRefresh = 0;
	int m_WristHudUploadFailures = 0;
	std::vector<uint8_t> m_WristHudPixels;
	unsigned char m_WristHudLastState[128] = {}; // WristHud::State, kept here so vr.h doesn't need its header

	// Weapon orientation stored relative to the "two hands" frame at the moment the left hand grabs the weapon
	Vector m_TwoHandForwardLocal = { 1,0,0 };
	Vector m_TwoHandUpLocal = { 0,0,1 };

	bool isWalkingSlowly = false;
	bool isHoldingWeapon2Handed = false;
	bool isPressingLeftGrab = false;
	bool isPressingRightGrab = false;
	bool temp = false;
	bool isReloading = false;
	bool isPressingToggleWalkSpeed = false;
	bool m_IsPlayerCrouched = false;
	bool isCrouching = false;
	bool hasCrouchAnimationEnded = false;
	float m_CrouchAnimDeltaTime = 0;
	float cameraAnchorZBeforePhysicalCrouch = 0;
	bool isPressingCrouchButton = false;
	bool isSendingCrouchCommand = false;

	VR() {};
	VR(Game *game);
	int SetActionManifest(const char *fileName);
	void InstallApplicationManifest(const char *fileName);
	void Update();
	void CreateVRTextures();
	void SubmitVRTextures();
	void RepositionOverlays();
	void GetPoses();
	void UpdatePosesAndActions();
	void GetViewParameters();
	void ProcessMenuInput();
	void ProcessInput();
	VMatrix VMatrixFromHmdMatrix(const vr::HmdMatrix34_t &hmdMat);
	vr::HmdMatrix34_t VMatrixToHmdMatrix(const VMatrix &vMat);
	vr::HmdMatrix34_t GetControllerTipMatrix(vr::ETrackedControllerRole controllerRole);
	bool CheckOverlayIntersectionForController(vr::VROverlayHandle_t overlayHandle, vr::ETrackedControllerRole controllerRole);
	QAngle GetRightControllerAbsAngle();
	Vector GetRightControllerAbsPos();
	Vector GetRecommendedViewmodelAbsPos();
	QAngle GetRecommendedViewmodelAbsAngle();
	void UpdateTracking();
	void UpdateHands(float deltaSeconds);
	bool WantsLeftHandOnController() const;
	bool IsWeaponReloading() const;
	bool BuildFirstPersonHands(const void *studioHeader, const void *customBones, void *outBones);
	void ClearModelCaches();
	void UpdateWristHud();
	void HideWristHud();
	void UpdateFlashlightAttachment();
	void PatchLaserSight();
	void CreateScopeOverlays();
	bool UpdateScope();
	void SubmitScopeOverlays();
	void HideScopeOverlays();
	static bool IsThrowableWeapon(int weaponId);
	void UpdateGrenadeThrow();
	void UpdateGrenadeCancel(std::chrono::steady_clock::time_point now, bool trigger);
	bool GetAttackButton(bool trigger);
	bool IsGrenadeThrowInProgress();
	bool HasPendingGestureThrow();
	bool TakeGrenadeThrow(const Vector &gameOrigin, Vector &origin, Vector &armVelocity, int &ageMs);
	Vector GetGrenadeThrowOrigin(const Vector &start, const Vector &hand, const Vector &gameOrigin);
	bool GetGrenadeThrowMessage(short &mousedx, short &mousedy);
	static void EncodeGrenadeThrow(bool gesture, int sequence, const Vector &armVelocity, short &mousedx, short &mousedy);
	static bool DecodeGrenadeThrow(short mousedx, short mousedy, bool &gesture, int &sequence, Vector &armVelocity);
	static void WriteGrenadeThrowLog(const char *format, ...);
	static int GetWeaponSlot(int weaponId);
	void UpdateBodyGrab(bool weaponHand, const Vector &handPosLocalInWorld, int weaponId);
	Vector GetViewAngle();
	Vector GetViewOriginLeft();
	Vector GetViewOriginRight();
	bool PressedDigitalAction(vr::VRActionHandle_t &actionHandle, bool checkIfActionChanged = false);
	bool GetAnalogActionData(vr::VRActionHandle_t &actionHandle, vr::InputAnalogActionData_t &analogDataOut);
	void ResetPosition();
	void GetPoseData(vr::TrackedDevicePose_t &poseRaw, TrackedDevicePoseData &poseOut);
	void ParseConfigFile();
	bool SaveConfigFile();
	void WaitForConfigUpdate();
};