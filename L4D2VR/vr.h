#pragma once
#include "openvr.h"
#include "vector.h"
#include "haptics.h"
#include "left_gun.h"
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
struct IDirect3DTexture9;
class ITexture;
class SettingsMenu;
class C_BasePlayer;
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

	enum GameButton { GameButton_Forward, GameButton_Back, GameButton_MoveLeft, GameButton_MoveRight, GameButton_Attack, GameButton_Jump, GameButton_Use, GameButton_Reload, GameButton_Attack2, GameButton_Duck, GameButton_ShowScores, GameButton_Count };
	signed char m_GameButtonState[GameButton_Count] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
	bool m_ResendGameButtons = false;
	unsigned long long m_GameButtonsResent = 0;
	void SetGameButton(GameButton button, bool down);

	bool m_HudOverlayVisible = false;
	bool m_MainMenuOverlayVisible = false;
	bool m_HudOverlayInteractive = false;
	bool m_ScopeOverlaysVisible = false;
	void SetHudOverlayVisible(bool visible);
	void SetMainMenuOverlayVisible(bool visible);
	void SetHudOverlayInteractive(bool interactive);

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
	float m_ScopeMagnification = 4.0f;      // the left trigger on the scope changes between these two
	float m_ScopeMagnificationHigh = 15.0f;
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
	bool m_ScopeZoomHigh = false;
	bool m_ScopeZoomTriggerDown = false;
	bool m_ScopeZoomTookTrigger = false;
	int m_ScopeZoomWeaponId = 0;
	// Where the scope is, from the last time it was updated: for the hand that changes its zoom
	HandMath::Vec3 m_ScopeRear;
	HandMath::Vec3 m_ScopeFront;
	int m_ScopeBodyWeaponId = 0;
	// The scope's attachment relative to the weapon's frame, kept while the fire animation isn't shown
	HandMath::Mat34 m_ScopeAttachmentInFrame;
	int m_ScopeAttachmentWeaponId = 0;
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
	float m_RightFingerCurls[HandRig::FingerCount] = { HandRig::kRelaxedCurl, HandRig::kRelaxedCurl, HandRig::kRelaxedCurl, HandRig::kRelaxedCurl, HandRig::kRelaxedCurl };
	float m_LeftHandFollow = 0.0f;          // 0 = the weapon's animation, 1 = the controller
	// From the last time the hands were drawn (BuildFirstPersonHands): the right hand's fist (HandRig::GripFrame), and
	// both hand bones where the weapon's animation has them, before the mod moves them
	HandMath::Mat34 m_RightGripFrame;
	unsigned long long m_RightGripMs = 0;
	HandMath::Vec3 m_AnimatedHand[2];       // HandRig::Left, HandRig::Right
	unsigned long long m_AnimatedHandsMs = 0;
	// The needle of the adrenaline in the right hand, from the last time it was drawn (ApplyItemInHand). Its tip is this
	// far along -y from the bone of the syringe, in both of its models (game units).
	static constexpr float kSyringeNeedleUnits = 7.1f;
	HandMath::Vec3 m_RightNeedleTip;
	HandMath::Vec3 m_RightNeedleDir;
	unsigned long long m_RightNeedleMs = 0;
	// Which way the body faces (not the head), horizontal, world (UpdateBodyYaw)
	float m_BodyYawLocal = 0.0f;            // degrees, in the play area, without the turning of the thumbstick
	bool m_BodyYawValid = false;
	Vector m_BodyFacing = { 1, 0, 0 };
	// Pills or adrenaline in the hand that uses them are at the mouth or against the body (ItemUseGestureReady)
	bool m_ItemGestureReady = false;
	int m_LogViewmodelGrip = 0;             // l4d2vr_dual_test vmgrip: draws of the weapon to log the bones of (hooks.cpp)
	// A shove by swinging an arm: the game's animation of it isn't shown, for about as long as it lasts (ApplyThrowPose)
	static constexpr unsigned long long kShoveAnimationMs = 700;
	unsigned long long m_ShoveHoldUntil = 0;
	bool m_ShoveDown = false;
	bool m_ViewmodelHasTwoWeapons = false;  // dual pistols shown as such, both hands are holding one
	bool m_SmallBodyItems = true;           // the pills and grenades on the chest are a bit smaller
	// Where the wrist is from the controller and how the hand is turned, see DefaultHandOffset. Not in the menu.
	Vector m_HandOffsetMeters = { -0.10f, 0.0f, -0.08f }; // forward, right, up
	float m_MeleeHandBackMeters = 0.03f;  // how much further back the hand is on melee weapons. MeleeHandBackMeters in config.txt
	QAngle m_HandAngleOffset = { 0.0f, 0.0f, 0.0f };

	// HUD on the wrist of the hand that doesn't hold the weapon (wrist_hud.cpp)
	bool m_WristHudEnabled = true;

	// Reloading with the hands (manual_reload.cpp, reload_logic.h)
	bool m_ManualReloadEnabled = true;
	bool m_ReloadActive = false;
	bool m_ReloadUsesShells = false;
	int m_ReloadClip = 0;
	HandMath::Vec3 m_WaistWorld;
	HandMath::Vec3 m_WaistForward;
	// Where the magazines and shells are, meters from the eyes looking straight ahead: to the side of the other hand,
	// forward, and down. The default height follows the player's height.
	static constexpr float kMagazinePouchBelowEyesPerHeight = 0.36f;
	float m_MagazinePouchSide = 0.24f;
	float m_MagazinePouchForward = 0.10f;
	float m_MagazinePouchBelowEyes = kMagazinePouchBelowEyesPerHeight * 1.8f;
	bool m_ShowReloadMark = true;        // the mark on the weapon while it's being reloaded
	bool m_ReloadPouchVisible = false;   // the waist can be in view, the bag is drawn
	float m_PumpHandShift = 0.0f;        // game units the hand holding a pump goes back, from the last weapon draw
	const HandRig *m_ArmsRig = nullptr;  // of the first person arms last drawn
	// The mark on the weapon where the bolt is taken (manual_reload.cpp)
	vr::VROverlayHandle_t m_BoltMarkerHandle = vr::k_ulOverlayHandleInvalid;
	bool m_BoltMarkerVisible = false;
	bool m_BoltMarkerFailed = false;

	// What the player carries, seen on the body (body_items.cpp): the ammo bag, pills and grenades on the chest
	struct BodyModelDraw
	{
		const void *model = nullptr;
		HandMath::Mat34 frame;
		float scale = 1.0f;
		bool hasCenter = false;      // otherwise the middle of the model's hull is at the frame
		HandMath::Vec3 center;
		bool alignWeaponBone = false; // the model's "weapon_bone" is at the frame instead, like in a hand
		bool syringe = false;         // the adrenaline's first person model, uncapped: its syringe held like the weapon bone
		bool onePistol = false;       // the model of two pistols shows only one
		bool leftGun = false;         // the left hand's gun: where its magazine goes is kept (m_LeftGunWell)
		bool magazine = false;        // only the weapon's magazine, held by its "weapon_bone_Clip" at the frame
	};
	// A weapon the player carries
	struct InventoryItem
	{
		int index;       // entity index
		int id;          // C_WeaponCSBase::WeaponID
		void *weapon;
	};
	// Using something in the world with the hand on it (the grab buttons): the game's use, aimed at it for a moment
	Vector m_GrabUseOrigin = { 0, 0, 0 };
	QAngle m_GrabUseAngles = { 0, 0, 0 };
	unsigned long long m_GrabUseAimUntil = 0;
	unsigned long long m_GrabUsePressAt = 0;
	unsigned long long m_GrabUsePressUntil = 0;
	unsigned long long m_TestUsePressAt = 0; // l4d2vr_dual_test grab: +use from the console when the grab presses it
	static constexpr int kMaxBodyDraws = 6;
	BodyModelDraw m_BodyDraws[kMaxBodyDraws];
	int m_BodyDrawCount = 0;
	bool m_AmmoBagDrawn = false;           // the game has the bag's model: the magazines don't hang at the waist themselves
	int m_ChestItemIds[2] = { 0, 0 };      // carried and not in the hand: pills or adrenaline, a grenade
	HandMath::Vec3 m_ChestItemPoints[2];
	HandMath::Vec3 m_ChestSpotPoints[2];   // where they go, with or without them
	bool m_ChestSpotsValid = false;
	// Where the pills (0) and the grenade (1) are carried, meters from the eyes looking straight ahead: forward, to the
	// weapon hand's side (negative is the other side) and below. Only in config.txt (PillsForward, GrenadeSide...); the
	// defaults follow the player's height (BodyLayout::DefaultItemSpot).
	float m_ItemSpotForward[2] = { 0.03f, 0.03f };
	float m_ItemSpotSide[2] = { -0.13f, 0.18f };
	float m_ItemSpotBelowEyes[2] = { 0.375f, 0.311f };
	// Moves the pistol's place on the thigh from where the player's height puts it, meters: forward, away from the body
	// (on the weapon hand's side) and up. HolsterForward, HolsterOutward, HolsterUp in config.txt and the menu.
	float m_HolsterForward = 0.0f;
	float m_HolsterOutward = 0.0f;
	float m_HolsterUp = 0.0f;
	// The pistol's place on the thigh, follows the leg when crouching (BodyLayout::HolsterOnThigh)
	HandMath::Vec3 m_HolsterPoint;         // the grip
	HandMath::Vec3 m_HolsterBarrel;        // where the barrel points from it
	bool m_HolsterValid = false;

	// Dual wield (dual_wield.cpp): the left hand holds any weapon or item. DualWield in config.txt.
	bool m_DualWieldEnabled = true;
	int m_LeftItemIndex = 0;               // entity index, 0 when the hand is empty
	int m_LeftItemId = 0;
	int m_ExtraWeaponIndex = 0;            // the weapon in the slot of its own (DualWield::kExtraSlot), the left shoulder's
	int m_SentLeftItemIndex = -1;
	int m_SentExtraWeaponIndex = -1;
	unsigned long long m_LeftItemSentMs = 0;
	int m_RightWeaponIndex = 0;            // what the right hand holds, to go back to it (entity index)
	int m_PrevRightWeaponIndex = 0;
	unsigned long long m_LeftGrabUntil = 0; // grabbed something in the world with the left hand: it goes there
	int m_InventoryBefore[64] = {};
	int m_InventoryBeforeCount = 0;
	unsigned long long m_LastSwitchBackMs = 0;
	// Using the left hand's grenade, pills or adrenaline: the active weapon for a moment, in the left hand
	bool m_LeftUseActive = false;
	bool m_LeftUseWasActive = false;
	unsigned long long m_LeftUseStartMs = 0;
	unsigned long long m_LeftUseReleaseMs = 0;
	bool m_LeftTriggerDown = false;
	// The left hand's gun (left_gun.cpp): the trigger, the next shot, and its magazine as seen here
	bool m_LeftGunTriggerDown = false;
	bool m_LeftGunPull = false;            // a pull not fired yet (a gun that fires once per pull)
	unsigned long long m_LeftGunNextShotMs = 0;
	int m_LeftGunBurstLeft = 0;
	int m_LeftGunClip = 0;                 // the game's, less the shots it hasn't counted yet
	int m_LeftGunClipSeen = 0;
	int m_LeftGunClipIndex = 0;
	bool m_RightHoldsLeftGun = false;      // the empty right hand holds the left hand's gun too (TryRightHoldLeftGun)
	bool m_RightHoldsLeftGunTest = false;  // held so by l4d2vr_dual_test, without the grab
	bool m_RightMagazine = false;          // the empty right hand holds a magazine for the left hand's gun
	bool m_RightMagazineTest = false;
	int m_RightMagazineIndex = 0;          // the gun it's for
	HandMath::Vec3 m_LeftGunWell;          // where the left hand's gun takes its magazine, as last drawn
	unsigned long long m_LeftGunWellMs = 0;
	mutable unsigned long long m_LeftGunWellNearLogMs = 0;
	HandMath::Vec3 m_LeftGunMuzzle;        // and its muzzle
	unsigned long long m_LeftGunMuzzleMs = 0;
	bool m_LeftSwinging = false;
	QAngle m_LeftSwingPrevAim = { 0, 0, 0 }; // the aim sent for the swing, from the last update
	bool m_LeftSwingAimValid = false;
	// The right controller's own pose while its poses are the left one's (ApplyLeftUseHand), where the right hand is drawn
	Vector m_RightRealPosAbs = { 0, 0, 0 };
	Vector m_RightRealForwardRaw = { 1, 0, 0 };
	Vector m_RightRealRightRaw = { 0, -1, 0 };
	Vector m_RightRealUpRaw = { 0, 0, 1 };
	unsigned long long m_LeftHandSentMs = 0;
	// Changing hands: 1 right to left, -1 left to right, 2 done and waiting for the grab to be let go
	int m_HandSwapDirection = 0;
	float m_HandSwapSeconds = 0.0f;
	unsigned long long m_HandSwapPulseMs = 0;
	// The fist of the left hand relative to its hand bone, from the last time the hands were drawn
	HandMath::Mat34 m_LeftGripInHand;
	bool m_LeftGripValid = false;
	// The left controller before the angle adjustment
	Vector m_LeftControllerForwardRaw = { 1,0,0 };
	Vector m_LeftControllerRightRaw = { 0,-1,0 };
	Vector m_LeftControllerUpRaw = { 0,0,1 };
	int m_RemoteLeftItem[33] = {};          // what other VR players hold in the left hand
	// From the head of other VR players' poses to the head of their models, from the last time they were drawn
	HandMath::Vec3 m_RemoteHeadOffset[33];
	bool m_RemoteHeadOffsetValid[33] = {};

	// Other players' movements (pose_relay.cpp, world_pose.cpp)
	bool m_ShareBodyMovement = true;       // the others see my arms
	bool m_ShowOtherPlayersMovement = true; // I see theirs
	vr::VROverlayHandle_t m_WristHudHandle = vr::k_ulOverlayHandleInvalid;
	vr::TrackedDeviceIndex_t m_WristHudDevice = vr::k_unTrackedDeviceIndexInvalid;
	bool m_WristHudVisible = false;
	bool m_WristHudDrawn = false;
	unsigned long long m_WristHudLastRefresh = 0;
	int m_WristHudUploadFailures = 0;
	std::vector<uint8_t> m_WristHudPixels;
	// An image written by the CPU into a texture of the game's device (DXVK), given to SteamVR as a Vulkan texture
	// (UploadOverlayPixels)
	struct OverlayTexture
	{
		IDirect3DTexture9 *texture = nullptr;
		IDirect3DSurface9 *surface = nullptr;
		SharedTextureHolder vk = {};
		int width = 0, height = 0;
	};
	OverlayTexture m_WristHudTexture;
	bool UploadOverlayPixels(vr::VROverlayHandle_t handle, OverlayTexture &target, const uint8_t *rgba, int width, int height);
	unsigned char m_WristHudLastState[300] = {}; // WristHud::State, kept here so vr.h doesn't need its header
	int m_WristHudWeaponId = -1;
	int m_WristHudMaxClip = 0;
	int m_WristHudMaxReserve = 0;

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
	void UpdateManualReload();
	bool IsGameReloadButtonDown() const { return m_GameButtonState[GameButton_Reload] == 1; }
	bool ThrownGrenadeHasLeftTheHand() const;
	const void *ApplyThrowPose(const void *studioHeader, const void *customBones, void *storage);
	bool ManualReloadIsHolding() const;
	static bool ReloadHasUnlimitedReserve(int weaponId);
	static int GetReloadCapacity(int weaponId);
	bool ManualReloadBlocksFire() const;
	bool ManualReloadBlocksNativeReload() const;
	bool ManualReloadOwnsReloadButton() const;
	bool ManualReloadHandIsBusy() const;
	void ClearManualReloadCache();
	bool BuildReloadViewmodel(const void *studioHeader, const void *customBones, void *outBones, void *outDetached, bool &drawDetached);
	void PlayWeaponSound(const char *sound);
	void UpdateBoltMarker(bool show, bool inReach);
	bool ManualReloadPumpsByHand() const;
	bool ManualReloadHoldsFirePose();
	bool ManualReloadUsesLeftHand() const;
	void ClearBodyModels();
	void UpdateBodyItems();
	void DrawBodyModels(int flags, void *renderable, int entityIndex);
	int GetChestItemSlotAt(const HandMath::Vec3 &handWorld) const;
	void BuildBodyModelBones(const void *studioHeader, const BodyModelDraw &draw, void *outBones);
	bool TryGrabWorld(const HandMath::Vec3 &handWorld, bool leftHand = false);
	bool FindNearestPickable(const HandMath::Vec3 &from, float radiusUnits, HandMath::Vec3 &point, const char *filter = nullptr);
	int GetInventory(C_BasePlayer *player, InventoryItem *items, int max);
	const void *GetEntityModel(const void *entity);
	const void *GetWorldModel(const void *weapon);
	void LogDoors(const HandMath::Vec3 &from);
	int GetChestSpotAt(const HandMath::Vec3 &handWorld) const;
	bool IsAtHolster(const HandMath::Vec3 &fist) const;
	const void *GetSyringeModel();
	// Dual wield (dual_wield.cpp)
	void UpdateDualWield(C_BasePlayer *local, int activeWeaponId, float deltaSeconds);
	void UpdateLeftUse(unsigned long long now, int activeIndex, const InventoryItem *items, int count);
	void UpdateLeftSwing(unsigned long long now);
	void UpdateHandSwap(unsigned long long now, float deltaSeconds, int activeIndex, int activeWeaponId, const InventoryItem *items, int count);
	bool LeftHandHoldsItem() const;
	bool LeftHandHoldsMelee() const;
	void TakeIntoLeftHand(int entityIndex, int weaponId);
	void ClearLeftItem();
	bool LeftHandChestGrab(const HandMath::Vec3 &handWorld);
	bool LeftShoulderGrab();
	bool LeftHandKitGrab();
	// The left hand's gun (left_gun.cpp)
	void UpdateLeftGun(unsigned long long now);
	void FireLeftGun(const LeftGun::GunData &gun, const Vector &origin, const QAngle &angles);
	bool GetLeftGunAim(Vector &origin, QAngle &angles) const;
	bool GetLeftItemFrame(HandMath::Mat34 &frame) const;
	bool TryRightHoldLeftGun();
	bool TryRightReloadLeftGun();
	bool LeftGunNeedsAmmo(const LeftGun::GunData &gun) const;
	bool AtLeftGunWell(const HandMath::Vec3 &point) const;
	void LoadLeftGun(const LeftGun::GunData &gun);
	void GiveLeftGunAmmo();
	void GiveToLeftHand(int activeIndex, int activeWeaponId, const InventoryItem *items, int count);
	void GiveToRightHand(const InventoryItem *items, int count);
	bool GetLeftItemDraw(BodyModelDraw &draw);
	bool LeftHandShowsItem() const;
	QAngle GetLeftSwingAim() const;
	bool GetLeftGripFrame(HandMath::Mat34 &frame) const;
	HandMath::Mat34 GetLeftHandBone(const HandRig &rig) const;
	void HoldLeftItem(const HandRig &rig, float curls[HandRig::FingerCount]);
	void KeepLeftGrip(const HandRig &rig, const HandMath::Mat34 *bones);
	void ApplyLeftUseHand(const Vector &leftForwardRaw, const Vector &leftRightRaw, const Vector &leftUpRaw);
	int PickRightWeapon(const InventoryItem *items, int count, int exceptIndex, int preferredIndex) const;
	void SelectWeapon(int entityIndex);
	void OnRemoteLeftItem(int playerIndex, int itemIndex);
	void DualWieldTest(const char *action);
	bool GetGrabUseAim(Vector &origin, QAngle &angles) const;
	bool BuildRemoteItemBones(int entityIndex, const void *weapon, const void *studioHeader, void *outBones);
	static bool IsHeldInTheHand(int weaponId);
	bool GetRightHandTarget(const HandRig &rig, HandMath::Mat34 &hand, float backMeters = 0.0f) const;
	HandMath::Vec3 GetFistPoint(bool weaponHand) const;
	void UpdateBodyYaw(float deltaSeconds);
	bool ItemUseGestureReady(bool leftHand, int weaponId);
	bool UpdateItemUse();
	bool m_ItemTriggerDown[2] = {};          // left, right: the triggers on the last update, for the adrenaline's cap
	// The thumbstick clicks: a short click, or held to drop what the hand holds (UpdateStickHold)
	struct StickHold
	{
		bool down = false;
		bool dropped = false;
		unsigned long long pressedMs = 0;
		unsigned long long pulseMs = 0;
	};
	StickHold m_LeftStickHold, m_RightStickHold;
	bool UpdateStickHold(StickHold &hold, bool down, bool weaponHand);
	void DropFromHand(bool weaponHand);
	void SendDrop(int entityIndex);
	// The right hand empty: what it held was put back on the body. The game always has a weapon out, and draws the hands
	// with it, so that one stays the active weapon, unseen and doing nothing, until the hand takes something
	// (UpdateEmptyHand)
	bool m_RightHandEmpty = false;
	const void *m_EmptyHandWeapon = nullptr;
	unsigned long long m_EmptyHandSelectMs = 0; // the mod changed the unseen weapon (dual wield), not the player
	void EmptyRightHand(const char *why);
	void UpdateEmptyHand(C_BasePlayer *local);
	bool TakeRightSlot(int slot);
	bool IsAtTwoHandSpot(bool &known) const;
	const void *ApplyItemInHand(const void *studioHeader, const void *customBones, void *storage);
	HandMath::Vec3 GetLeftPalm() const;
	HandMath::Mat34 GetLeftHandGrabFrame() const;
	HandMath::Vec3 GetRightWristAbs() const;
	bool BuildRemotePlayerBones(int playerIndex, const void *studioHeader, const void *customBones, void *outBones);
	bool BuildRemoteWeaponBones(int entityIndex, const void *studioHeader, const void *customBones, void *outBones);
	void HideWristHud();
	void UpdateFlashlightAttachment();
	void PatchLaserSight();
	void CreateScopeOverlays();
	bool UpdateScope();
	bool UpdateScopeZoom(bool haveBolt, const HandMath::Vec3 &boltGrab);
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
	void UpdateBodyGrab(bool weaponHand, const Vector &handPosLocalInWorld, int weaponId, const HandMath::Vec3 *fistOverride = nullptr);
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