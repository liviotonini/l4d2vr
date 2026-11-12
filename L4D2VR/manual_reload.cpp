#include "vr.h"
#include <Windows.h>
#include <algorithm>
#include <cstdio>
#include "sdk.h"
#include "game.h"
#include "studio.h"
#include "reload_logic.h"
#include "pose_relay.h"
#include "diagnostics.h"

using namespace HandMath;

namespace
{
	const int kClientClipOffset = 0x984;
	const int kClientAmmoTypeOffset = 0x97C;
	const int kClientAmmoArrayOffset = 0xF24;

	// The sounds of the game's own reload animations (loose files in left4dead2\sound)
	struct Sounds
	{
		const char *magazineOut;
		const char *magazineIn;   // or a shell
		const char *boltBack;     // at the end of the pull
		const char *boltForward;  // when it's let go
	};

	const Sounds kPistolSounds = { "weapons/pistol/gunother/pistol_clip_out_1.wav", "weapons/pistol/gunother/pistol_clip_in_1.wav", "weapons/pistol/gunother/pistol_slideback_1.wav", "weapons/pistol/gunother/pistol_slideforward_1.wav" };
	const Sounds kSmgSounds = { "weapons/smg/gunother/smg_clip_out_1.wav", "weapons/smg/gunother/smg_clip_in_1.wav", "weapons/smg/gunother/smg_slideback_1.wav", "weapons/smg/gunother/smg_slideforward_1.wav" };
	const Sounds kRifleSounds = { "weapons/rifle/gunother/rifle_clip_out_1.wav", "weapons/rifle/gunother/rifle_clip_in_1.wav", "weapons/rifle/gunother/rifle_slideback_1.wav", "weapons/rifle/gunother/rifle_slideforward_1.wav" };
	const Sounds kPumpSounds = { nullptr, "weapons/shotgun/gunother/shotgun_load_shell_2.wav", "weapons/shotgun/gunother/shotgun_pump_1.wav", nullptr };
	const Sounds kAutoShotgunSounds = { nullptr, "weapons/auto_shotgun/gunother/auto_shotgun_load_shell_2.wav", "weapons/auto_shotgun/gunother/autoshotgun_boltback.wav", "weapons/auto_shotgun/gunother/autoshotgun_boltforward.wav" };

	const Sounds kGrenadeLauncherSounds = { nullptr, "weapons/grenade_launcher/grenadeother/grenade_launcher_shellin.wav", nullptr, nullptr };
	const Sounds kChromeSounds = { nullptr, "weapons/shotgun_chrome/gunother/shotgun_load_shell_2.wav", "weapons/shotgun_chrome/gunother/shotgun_pump_1.wav", nullptr };
	const Sounds kAwpSounds = { "weapons/awp/gunother/awp_clipout.wav", "weapons/awp/gunother/awp_clipin.wav", "weapons/awp/gunother/awp_bolt.wav", "weapons/awp/gunother/awp_bolt_forward.wav" };
	const Sounds kScoutSounds = { "weapons/scout/gunother/scout_clipout.wav", "weapons/scout/gunother/scout_clipin.wav", "weapons/scout/gunother/scout_bolt.wav", "weapons/scout/gunother/scout_bolt_forward.wav" };

	// Measured from the game's models: the bones of the magazine (the shell for shotguns) and of the bolt (the slide of
	// pistols, the pump of pump shotguns), the center of their mesh relative to the bone, and how far the bolt travels.
	struct Profile
	{
		int weaponId;
		const char *viewmodel;     // file name of the model in first person
		const char *magazineBone;
		const char *bulletsBone;
		const char *boltBone;
		float magazineCenter[3];
		float boltCenter[3];
		bool shells;
		int capacity;
		float boltTravel;
		const Sounds *sounds;
		bool pump;  // the other hand holds the part that's worked: it goes back and forth with it
		float magazineLength; // game units, along the weapon's up axis
		// Worked by hand after every shot (pump action, bolt action): for how long the game's fire animation, which does
		// it by itself, isn't shown. 0 for the others.
		int firePoseMs;
	};

	const Profile kProfiles[] =
	{
		{ C_WeaponCSBase::M16A1, "v_rifle.mdl", "ValveBiped.weapon_clip", "ValveBiped.weapon_clip_bullets", "ValveBiped.weapon_bolt", { -0.01f, -1.73f, -0.30f }, { 0.21f, 1.22f, -8.56f }, false, 50, 2.5f, &kRifleSounds, false, 7.7f, 0 },
		{ C_WeaponCSBase::AK47, "v_rifle_ak47.mdl", "ValveBiped.weapon_clip", "ValveBiped.weapon_clip_bullets", "ValveBiped.weapon_bolt", { 0.01f, -2.89f, 3.04f }, { -1.37f, -0.25f, 0.88f }, false, 40, 2.5f, &kRifleSounds, false, 10.7f, 0 },
		{ C_WeaponCSBase::SCAR, "v_desert_rifle.mdl", "ValveBiped.weapon_clip", "ValveBiped.weapon_clip_bullets", "ValveBiped.weapon_bolt", { -0.01f, -3.10f, -0.35f }, { 0.96f, 1.09f, 0.65f }, false, 60, 2.5f, &kRifleSounds, false, 10.7f, 0 },
		{ C_WeaponCSBase::SG552, "v_rif_sg552.mdl", "ValveBiped.weapon_clip", nullptr, "ValveBiped.weapon_bolt", { 0.24f, -1.98f, 0.26f }, { 0.79f, 1.13f, 2.79f }, false, 50, 2.5f, &kRifleSounds, false, 5.8f, 0 },
		{ C_WeaponCSBase::UZI, "v_smg.mdl", "ValveBiped.weapon_clip", nullptr, "ValveBiped.weapon_bolt", { -0.02f, -3.03f, 0.05f }, { 0.01f, 0.33f, -7.39f }, false, 50, 1.5f, &kSmgSounds, false, 8.6f, 0 },
		{ C_WeaponCSBase::MAC10, "v_silenced_smg.mdl", "ValveBiped.weapon_clip", "ValveBiped.weapon_clip_bullets", "ValveBiped.weapon_bolt", { -0.07f, -3.25f, -0.04f }, { -0.16f, 0.24f, -9.07f }, false, 50, 1.5f, &kSmgSounds, false, 8.6f, 0 },
		{ C_WeaponCSBase::MP5, "v_smg_mp5.mdl", "ValveBiped.weapon_clip", nullptr, "ValveBiped.weapon_bolt", { -0.00f, -2.71f, 0.78f }, { 0.77f, 0.30f, -0.39f }, false, 50, 1.5f, &kSmgSounds, false, 6.1f, 0 },
		{ C_WeaponCSBase::HUNTING_RIFLE, "v_huntingrifle.mdl", "ValveBiped.weapon_clip", "ValveBiped.weapon_clip_bullets", "ValveBiped.weapon_bolt", { 0.06f, 0.70f, -0.15f }, { -0.39f, 0.04f, 0.45f }, false, 15, 2.5f, &kRifleSounds, false, 3.7f, 0 },
		{ C_WeaponCSBase::SNIPER_MILITARY, "v_sniper_military.mdl", "ValveBiped.weapon_clip", "ValveBiped.weapon_clip_bullets", "ValveBiped.weapon_charger_slide", { 0.12f, -0.83f, 2.74f }, { 0.74f, 0.23f, 2.05f }, false, 30, 2.5f, &kRifleSounds, false, 7.9f, 0 },
		{ C_WeaponCSBase::AWP, "v_snip_awp.mdl", "ValveBiped.weapon_clip", nullptr, "ValveBiped.weapon_bolt", { 0.01f, -1.35f, 0.70f }, { 2.22f, -0.54f, -0.20f }, false, 20, 3.5f, &kAwpSounds, false, 3.3f, 1500 },
		{ C_WeaponCSBase::SCOUT, "v_snip_scout.mdl", "ValveBiped.weapon_clip", nullptr, "ValveBiped.weapon_bolt", { -0.17f, -1.16f, 1.71f }, { 0.97f, 0.89f, 1.13f }, false, 15, 3.5f, &kScoutSounds, false, 1.8f, 1800 },
		{ C_WeaponCSBase::PISTOL, "v_pistola.mdl", "ValveBiped.weapon_clip", "ValveBiped.weapon_clip_bullets", "ValveBiped.weapon_charger_slide", { -0.03f, -0.32f, 0.51f }, { 0.63f, 1.38f, 6.35f }, false, 15, 1.2f, &kPistolSounds, false, 6.3f, 0 },
		{ C_WeaponCSBase::MAGNUM, "v_desert_eagle.mdl", "ValveBiped.weapon_clip", "ValveBiped.weapon_clip_bullets", "ValveBiped.weapon_charger_slide", { -0.01f, -2.89f, 1.13f }, { 0.47f, 2.26f, 5.10f }, false, 8, 1.5f, &kPistolSounds, false, 6.4f, 0 },
		{ C_WeaponCSBase::PUMPSHOTGUN, "v_pumpshotgun.mdl", "ValveBiped.weapon_clip_bullets", nullptr, "ValveBiped.weapon_charger_slide", { -0.00f, 0.07f, 1.37f }, { -0.01f, 0.31f, -4.80f }, true, 8, 4.5f, &kPumpSounds, true, 0.0f, 1050 },
		{ C_WeaponCSBase::SHOTGUN_CHROME, "v_shotgun_chrome.mdl", "ValveBiped.weapon_clip_bullets", nullptr, "ValveBiped.weapon_charger_slide", { -0.00f, 0.07f, 1.37f }, { -0.05f, 0.17f, -5.32f }, true, 8, 4.5f, &kChromeSounds, true, 0.0f, 1050 },
		{ C_WeaponCSBase::AUTOSHOTGUN, "v_autoshotgun.mdl", "ValveBiped.weapon_clip", nullptr, "ValveBiped.weapon_bolt", { -0.00f, 0.21f, 0.04f }, { -0.89f, -0.27f, 0.79f }, true, 10, 2.0f, &kAutoShotgunSounds, false, 0.0f, 0 },
		// One round, loaded like a shell. It has nothing to pull.
		{ C_WeaponCSBase::GRENADE_LAUNCHER, "v_grenade_launcher.mdl", "ValveBiped.weapon_clip", nullptr, nullptr, { -0.16f, 0.27f, 1.02f }, { 0.0f, 0.0f, 0.0f }, true, 1, 0.0f, &kGrenadeLauncherSounds, false, 0.0f, 0 },
		{ C_WeaponCSBase::SPAS, "v_shotgun_spas.mdl", "ValveBiped.weapon_clip", nullptr, "ValveBiped.weapon_bolt", { 0.00f, 0.20f, 0.02f }, { -1.06f, -0.21f, -0.22f }, true, 10, 2.0f, &kAutoShotgunSounds, false, 0.0f, 0 },
	};

	// The weapon a model in first person is, by its file name
	const Profile *FindProfileByModel(const char *modelName)
	{
		const char *fileName = modelName;
		for (const char *c = modelName; *c; ++c)
		{
			if (*c == '/' || *c == '\\')
				fileName = c + 1;
		}
		for (const Profile &profile : kProfiles)
		{
			if (_stricmp(profile.viewmodel, fileName) == 0)
				return &profile;
		}
		return nullptr;
	}

	const Profile *FindProfile(int weaponId)
	{
		for (const Profile &profile : kProfiles)
		{
			if (profile.weaponId == weaponId)
				return &profile;
		}
		return nullptr;
	}

	Reload::Machine g_Machine;

	struct ViewmodelBones
	{
		const Profile *builtFor = nullptr;
		const Profile *byName = nullptr; // the weapon this model is, by its file name (not a custom model)
		bool nameChecked = false;
		int magazine = -1;
		int bullets = -1;
		int bolt = -1;
		int boltParent = -1;
		Mat34 boltRest;  // where the bolt sits on its parent when nothing moves it
		float gamePumpTravel = 0.0f; // how far back the game's own animation takes the pump, for the log
		int actionSounds = -1;       // the sounds of the action in its fire animations: 1 on, 0 off, -1 not set yet
	};

	std::unordered_map<const void *, ViewmodelBones> g_ViewmodelBones;

	// The parts of the weapon, from the last time it was drawn
	Mat34 g_MagazineWorld;
	Vec3 g_Socket;
	Vec3 g_MagazineBase;
	Vec3 g_BoltGrab;
	// Where the weapon was drawn from. The hand (GetLeftPalm) is from the same tracking update, so walking moves both
	// the same way.
	Vec3 g_WeaponPoint;
	bool g_HaveBolt = false;
	ULONGLONG g_LastViewmodelDraw = 0;
	int g_LastWeaponId = 0;
	// What the hand holds relative to it: where it was when grabbed, moving to where it settles
	Mat34 g_HeldAtGrab;
	Mat34 g_HeldInHand;
	ULONGLONG g_GrabTime = 0;
	const float kHeldSettleMs = 150.0f;
	// What was just put in slides from where the hand left it to its place, relative to the weapon's magazine bone
	Mat34 g_InsertFrom;
	ULONGLONG g_InsertTime = 0;
	const float kInsertMs = 110.0f;
	// The pump's resting place is learnt while the weapon is idle: out for a while and not fired lately
	ULONGLONG g_DeployTime = 0;
	ULONGLONG g_LastShotTime = 0;
	int g_LastClipSeen = -1;
	const ULONGLONG kIdleAfterDeployMs = 2000;
	const ULONGLONG kIdleAfterShotMs = 1500;
	bool g_WasAtPouch = false;
	// The game's fire animation of a pump or bolt action works it by itself: it isn't shown for a while after a shot. The
	// shot is seen in the draw itself, from the clip, so not even its first frame is shown.
	int g_DrawClip = -1;
	int g_DrawWeaponId = 0;
	ULONGLONG g_FirePoseUntil = 0;
	// The left arm handled the weapon lately: swinging it doesn't shove (a quick pump is a fast swing)
	ULONGLONG g_LeftHandUsedTime = 0;
	const ULONGLONG kNoShoveAfterHandlingMs = 400;
	// The mark where the bolt is taken: relative to the right controller in its OpenVR frame, from the last draw
	vr::HmdMatrix34_t g_MarkerTransform = {};
	bool g_MarkerPlaced = false;
	bool g_ActionSoundsOff = false;
	const float kMarkerWidth = 0.035f;       // meters
	const int kMarkerSize = 64;              // pixels

	// The waist is only drawn when it can be seen: within this angle of where the head looks (cosine of about 66 degrees)
	const float kPouchViewCosine = 0.4f;

	// Center of what's held from the palm, game units: towards the fingers, to the side of the thumb's edge of the hand
	// (y is left, and the palm of the left hand faces right) and up. Against the palm and sticking out of it, not inside it.
	const Vec3 kMagazineInPalm(0.4f, -1.9f, 0.7f);
	const Vec3 kShellInFingers(1.6f, -1.3f, 0.2f);

	Vec3 ToVec3(const Vector &v) { return Vec3(v.x, v.y, v.z); }
	Vec3 ArrayToVec3(const float v[3]) { return Vec3(v[0], v[1], v[2]); }

	// What the hand holds, at the same angle it has in the weapon
	Mat34 HeldRelativeToHand(const Profile &profile, const Mat34 &magazineWorld, const Mat34 &weaponFrame)
	{
		Mat34 relative = Multiply(TransposeRotation(weaponFrame), magazineWorld);
		const Vec3 place = profile.shells ? kShellInFingers : kMagazineInPalm;
		SetAxis(relative, 3, place - RotateVector(relative, ArrayToVec3(profile.magazineCenter)));
		return relative;
	}

	// At the waist, hanging the way it sits in the weapon, centered on the waist point
	Mat34 PouchWorld(const Profile &profile, const Mat34 &magazineWorld, const Mat34 &weaponFrame, const Vec3 &waist, const Vec3 &waistForward)
	{
		Mat34 relative = Multiply(TransposeRotation(weaponFrame), magazineWorld);
		SetAxis(relative, 3, RotateVector(relative, ArrayToVec3(profile.magazineCenter)) * -1.0f);

		const Vec3 forward = Normalized(waistForward);
		const Vec3 up(0.0f, 0.0f, 1.0f);
		return Multiply(FromAxes(forward, Cross(up, forward), up, waist), relative);
	}

	float EaseOut(float t)
	{
		t = std::clamp(t, 0.0f, 1.0f);
		return 1.0f - (1.0f - t) * (1.0f - t);
	}

	// Palm of the hand that reloads. The wrist comes from UpdateHands.
	const float kWristToPalm = 0.085f; // meters

	template <typename T>
	T Read(const void *entity, int offset)
	{
		return *(const T *)((const uint8_t *)entity + offset);
	}
}

bool VR::ReloadHasUnlimitedReserve(int weaponId)
{
	return weaponId == C_WeaponCSBase::PISTOL || weaponId == C_WeaponCSBase::MAGNUM;
}

bool VR::ManualReloadIsHolding() const
{
	const Reload::State state = g_Machine.GetState();
	return m_ManualReloadEnabled && (state == Reload::State::HoldingOld || state == Reload::State::HoldingFresh);
}

// What the reload by hand of a weapon is, for the left hand's gun (left_gun.cpp)
bool VR::GetReloadSpec(int weaponId, ReloadSpec &spec)
{
	const Profile *profile = FindProfile(weaponId);
	if (!profile)
		return false;
	spec.capacity = profile->capacity;
	spec.shells = profile->shells;
	spec.boltTravel = profile->boltTravel;
	spec.pump = profile->pump;
	spec.everyShot = profile->firePoseMs > 0;
	spec.magazineLength = profile->magazineLength;
	spec.magazineOutSound = profile->sounds->magazineOut;
	spec.magazineInSound = profile->sounds->magazineIn;
	spec.boltBackSound = profile->sounds->boltBack;
	spec.boltForwardSound = profile->sounds->boltForward;
	return true;
}

// How many rounds a weapon holds, 0 for weapons reloaded the game's way
int VR::GetReloadCapacity(int weaponId)
{
	const Profile *profile = FindProfile(weaponId);
	return profile ? profile->capacity : 0;
}

Vec3 VR::GetLeftPalm() const
{
	return m_LeftHandWrist + Normalized(m_LeftHandForward) * (kWristToPalm * m_VRScale);
}

Mat34 VR::GetLeftHandGrabFrame() const
{
	const Vec3 forward = Normalized(m_LeftHandForward);
	const Vec3 up = Normalized(m_LeftHandUp - forward * Dot(forward, m_LeftHandUp), Vec3(0.0f, 0.0f, 1.0f));
	return FromAxes(forward, Cross(up, forward), up, GetLeftPalm());
}

// Plays a sound of the game for this player only, not positioned
void VR::PlayWeaponSound(const char *sound)
{
	if (!sound)
		return;

	char command[160];
	snprintf(command, sizeof(command), "play %s", sound);
	m_Game->ClientCmd_Unrestricted(command);
}

namespace
{
	// A wrong layout or a read only model must not take the game down
	int SetActionSounds(const void *studioHeader, bool on)
	{
		__try
		{
			return Studio::SetFireActionSounds(studioHeader, on);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return -1;
		}
	}

	float Coverage(float distance, float halfWidth)
	{
		return std::clamp(halfWidth + 0.5f - distance, 0.0f, 1.0f);
	}

	// A ring with an arrow pointing to the right (+x), white: the overlay's color tints it
	std::vector<uint8_t> MarkerPixels()
	{
		std::vector<uint8_t> pixels(kMarkerSize * kMarkerSize * 4);
		const float center = kMarkerSize * 0.5f;
		for (int y = 0; y < kMarkerSize; ++y)
		{
			for (int x = 0; x < kMarkerSize; ++x)
			{
				const float dx = x + 0.5f - center, dy = y + 0.5f - center;
				const float ring = Coverage(fabsf(sqrtf(dx * dx + dy * dy) - 27.0f), 2.5f);
				const float shaft = (dx > -14.0f && dx < 12.0f) ? Coverage(fabsf(dy), 2.0f) : 0.0f;
				// Head of the arrow: a triangle from x = 10 to x = 22
				const float head = (dx >= 10.0f && dx <= 22.0f && fabsf(dy) <= (22.0f - dx) * 0.75f) ? 1.0f : 0.0f;
				const float alpha = std::max(ring, std::max(shaft, head));
				uint8_t *pixel = &pixels[(y * kMarkerSize + x) * 4];
				pixel[0] = pixel[1] = pixel[2] = 255;
				pixel[3] = (uint8_t)(alpha * 255.0f);
			}
		}
		return pixels;
	}
}

// The mark on the weapon where the bolt, slide or pump is taken with the left trigger. A SteamVR overlay attached to the
// right controller, so it follows the weapon without lag; it's drawn over everything, like the scope.
void VR::UpdateBoltMarker(bool show, bool inReach)
{
	if (!show || !g_MarkerPlaced || !m_Overlay || m_RightControllerDeviceIndex == vr::k_unTrackedDeviceIndexInvalid)
	{
		if (m_BoltMarkerVisible)
			m_Overlay->HideOverlay(m_BoltMarkerHandle);
		m_BoltMarkerVisible = false;
		return;
	}

	if (m_BoltMarkerHandle == vr::k_ulOverlayHandleInvalid)
	{
		if (m_BoltMarkerFailed)
			return;
		if (m_Overlay->CreateOverlay("BoltMarkerOverlayKey", "BoltMarkerOverlay", &m_BoltMarkerHandle) != vr::VROverlayError_None)
		{
			m_BoltMarkerFailed = true;
			m_BoltMarkerHandle = vr::k_ulOverlayHandleInvalid;
			Diagnostics::Log("manual reload: couldn't create the overlay that marks the bolt");
			return;
		}
		std::vector<uint8_t> pixels = MarkerPixels();
		m_Overlay->SetOverlayRaw(m_BoltMarkerHandle, pixels.data(), kMarkerSize, kMarkerSize, 4);
		m_Overlay->SetOverlayWidthInMeters(m_BoltMarkerHandle, kMarkerWidth);
		Diagnostics::Log("manual reload: the overlay that marks the bolt is ready");
	}

	m_Overlay->SetOverlayTransformTrackedDeviceRelative(m_BoltMarkerHandle, m_RightControllerDeviceIndex, &g_MarkerTransform);
	// Amber: this is where to take it. Green: the hand is there, the trigger takes it.
	if (inReach)
		m_Overlay->SetOverlayColor(m_BoltMarkerHandle, 0.35f, 1.0f, 0.45f);
	else
		m_Overlay->SetOverlayColor(m_BoltMarkerHandle, 1.0f, 0.75f, 0.2f);
	if (!m_BoltMarkerVisible)
		m_Overlay->ShowOverlay(m_BoltMarkerHandle);
	m_BoltMarkerVisible = true;
}

// Pump shotguns are pumped by hand: needs the mod on the host
bool VR::ManualReloadPumpsByHand() const
{
	const Profile *profile = m_ManualReloadEnabled ? FindProfile(m_ActiveWeaponId) : nullptr;
	return profile && profile->firePoseMs > 0 && m_Game->m_PoseRelay->IsServerCapable();
}

// True while the game's fire animation of a pump action would be playing, which isn't shown (ApplyThrowPose). Called
// from the draws, so the shot is seen in the frame it happens.
bool VR::ManualReloadHoldsFirePose()
{
	C_BasePlayer *local = (C_BasePlayer *)m_Game->GetClientEntity(m_Game->m_EngineClient->GetLocalPlayer());
	const C_BaseCombatWeapon *weapon = local ? local->GetActiveWeapon() : nullptr;
	const Profile *profile = m_ManualReloadEnabled ? FindProfile(m_ActiveWeaponId) : nullptr;
	if (!weapon || !profile)
		return false;

	const int clip = Read<int>(weapon, kClientClipOffset);
	const ULONGLONG now = GetTickCount64();
	if (g_DrawWeaponId != m_ActiveWeaponId)
		g_FirePoseUntil = 0;
	else if (g_DrawClip >= 0 && clip < g_DrawClip)
		g_FirePoseUntil = now + profile->firePoseMs;
	g_DrawWeaponId = m_ActiveWeaponId;
	g_DrawClip = clip;
	return now < g_FirePoseUntil;
}

// The left arm is handling the weapon, or just did
bool VR::ManualReloadUsesLeftHand() const
{
	return m_ManualReloadEnabled && GetTickCount64() - g_LeftHandUsedTime < kNoShoveAfterHandlingMs;
}

// Called by UpdateTracking every frame, before it looks at the grab buttons
void VR::UpdateManualReload()
{
	// The bolt's mark is shown or hidden on the way out, whatever path that is
	struct MarkerUpdate
	{
		VR *vr;
		bool show = false;
		bool inReach = false;
		~MarkerUpdate() { vr->UpdateBoltMarker(show, inReach); }
	} marker{ this };

	// Nothing to reload in an empty right hand (EmptyRightHand)
	const Profile *profile = m_ManualReloadEnabled && !m_RightHandEmpty ? FindProfile(m_ActiveWeaponId) : nullptr;
	const ULONGLONG now = GetTickCount64();

	// The left trigger on a scope changes its zoom, whatever the reload is doing
	const bool scopeTookTrigger = UpdateScopeZoom(g_HaveBolt, g_BoltGrab);

	// Reloading the game's way again: its own sounds are back
	if (!m_ManualReloadEnabled && g_ActionSoundsOff)
	{
		for (auto &entry : g_ViewmodelBones)
		{
			if (entry.second.actionSounds == 0)
			{
				SetActionSounds(entry.first, true);
				entry.second.actionSounds = 1;
			}
		}
		g_ActionSoundsOff = false;
	}

	// The way the body faces (UpdateBodyYaw) and the point the waist hangs from, without the pitch and roll of the head.
	// Same as the items on the body.
	const Vector facing = m_BodyFacing;
	const Vector right(facing.y, -facing.x, 0.0f);

	const float neckForward = 0.08f * m_VRScale, neckDown = 0.075f * m_VRScale;
	const Vector neck = m_HmdPosLocalInWorld - m_HmdForward * neckForward - m_HmdUp * neckDown;
	const Vector reference = neck + facing * neckForward + Vector(0.0f, 0.0f, neckDown);

	const float otherSide = m_LeftHanded ? 1.0f : -1.0f;
	const Vector waistLocal = reference + right * (otherSide * m_MagazinePouchSide * m_VRScale) + facing * (m_MagazinePouchForward * m_VRScale)
		- Vector(0.0f, 0.0f, m_MagazinePouchBelowEyes * m_VRScale);
	m_WaistWorld = ToVec3(m_CameraAnchor - Vector(0, 0, 64) + waistLocal);
	m_WaistForward = ToVec3(facing);

	// The bag at the waist costs a draw per eye, so only when it can be seen. It's there whatever the weapon.
	m_ReloadPouchVisible = m_ManualReloadEnabled && Dot(Normalized(m_WaistWorld - ToVec3(m_HmdPosAbs)), ToVec3(m_HmdForward)) > kPouchViewCosine;

	if (m_ActiveWeaponId != g_LastWeaponId || !profile)
	{
		// Changed weapon: the magazine that was out is back in. A reload interrupted like this loses nothing.
		if (m_ActiveWeaponId != g_LastWeaponId)
			g_DeployTime = now;
		g_LastWeaponId = m_ActiveWeaponId;
		g_Machine.Reset();
		g_InsertTime = 0;
		g_LastClipSeen = -1;
		if (!profile)
			return;
	}

	// Two pistols use the game's reload. The weapon isn't drawn in some situations (held by an infected, for example).
	if (now - g_LastViewmodelDraw > 200 || m_ViewmodelHasTwoWeapons)
	{
		if (m_ReloadActive)
			g_Machine.Reset();
		m_ReloadActive = false;
		return;
	}

	C_BasePlayer *local = (C_BasePlayer *)m_Game->GetClientEntity(m_Game->m_EngineClient->GetLocalPlayer());
	const C_BaseCombatWeapon *weapon = local ? local->GetActiveWeapon() : nullptr;
	if (!weapon)
		return;

	const int ammoType = Read<int>(weapon, kClientAmmoTypeOffset);
	if (ammoType < 0 || ammoType >= 32)
		return;

	const Mat34 handFrame = GetLeftHandGrabFrame();
	const Mat34 weaponFrame = FromAxes(ToVec3(m_ViewmodelForward), ToVec3(m_ViewmodelRight) * -1.0f, ToVec3(m_ViewmodelUp));
	const float settled = std::min((now - g_GrabTime) / kHeldSettleMs, 1.0f);
	const Mat34 heldWorld = Multiply(handFrame, Blend(g_HeldAtGrab, g_HeldInHand, settled));
	const Reload::State stateBefore = g_Machine.GetState();
	const bool holdingBefore = stateBefore == Reload::State::HoldingOld || stateBefore == Reload::State::HoldingFresh;

	Reload::Inputs inputs;
	inputs.palm = GetLeftPalm();
	inputs.held = holdingBefore ? TransformPoint(heldWorld, ArrayToVec3(profile->magazineCenter)) : inputs.palm;
	inputs.socket = g_Socket;
	inputs.magazineBase = g_MagazineBase;
	inputs.boltGrab = g_BoltGrab;
	inputs.boltAxis = ToVec3(m_ViewmodelForward);
	inputs.weaponPoint = g_WeaponPoint;
	inputs.waist = m_WaistWorld;
	inputs.unitsPerMeter = m_VRScale;
	inputs.boltTravel = profile->boltTravel;
	inputs.grip = PressedDigitalAction(m_LeftHandGrab);
	// Ammo is only taken with an empty hand (the left hand's item, dual wield)
	inputs.handFree = m_LeftItemIndex == 0;
	// The left trigger shoves no more, and only cancels a grenade, which has no reload
	inputs.trigger = PressedDigitalAction(m_ActionSecondaryAttack) && !scopeTookTrigger;
	// Worked after every shot: holding the weapon with both hands, the trigger works it from wherever the other hand is
	inputs.holdsPump = profile->firePoseMs > 0 && isHoldingWeapon2Handed;
	inputs.pumpEveryShot = profile->firePoseMs > 0;
	// With a host that doesn't have the mod the game works the bolt
	const bool serverCommits = m_Game->m_PoseRelay->IsServerCapable();
	inputs.clip = Read<int>(weapon, kClientClipOffset);
	inputs.capacity = std::max(profile->capacity, inputs.clip);
	// Pistols never run out of magazines, and the game reports their reserve as 0
	inputs.reserve = ReloadHasUnlimitedReserve(m_ActiveWeaponId) ? 999 : Read<int>(local, kClientAmmoArrayOffset + ammoType * 4);
	inputs.shells = profile->shells;
	inputs.hasBolt = g_HaveBolt && serverCommits;

	if (g_LastClipSeen >= 0 && inputs.clip < g_LastClipSeen)
		g_LastShotTime = now;
	g_LastClipSeen = inputs.clip;

	const unsigned events = g_Machine.Update(inputs);
	m_ReloadClip = inputs.clip;
	m_ReloadUsesShells = profile->shells;
	m_ReloadActive = true;

	const Reload::State state = g_Machine.GetState();
	if (state == Reload::State::HoldingOld || state == Reload::State::HoldingFresh || state == Reload::State::HoldingBolt)
		g_LeftHandUsedTime = now;

	// The mark: from the moment the magazine is out until the weapon is loaded. First where the magazine goes, then, if
	// the weapon ran empty, the bolt.
	if (m_ShowReloadMark)
	{
		const bool magazineOut = !profile->shells && g_Machine.MagazineIsOut();
		const bool chamberIt = inputs.hasBolt && g_Machine.NeedsCycle() && inputs.clip > 0 && (profile->firePoseMs == 0 || g_Machine.ReloadInProgress())
			&& state != Reload::State::HoldingBolt && !inputs.holdsPump;
		marker.show = magazineOut || chamberIt;
		marker.inReach = magazineOut ? state == Reload::State::HoldingFresh : Length(inputs.palm - inputs.boltGrab) < Reload::kBoltGrabRadius * m_VRScale;
	}

	// The hand reaches the waist where it can take one: a faint tick, so it can be found without looking
	const bool canTake = inputs.reserve > 0 && (state == Reload::State::Idle || state == Reload::State::NeedFresh);
	const bool atPouch = canTake && Length(inputs.palm - inputs.waist) < Reload::kPouchRadius * m_VRScale;
	if (atPouch && !g_WasAtPouch)
		m_Haptics.Play(Haptics::Hand::OtherHand, HapticEffects::Hint);
	g_WasAtPouch = atPouch;

	if (events & (Reload::Event_GrabbedOld | Reload::Event_GrabbedFresh))
	{
		g_HeldInHand = HeldRelativeToHand(*profile, g_MagazineWorld, weaponFrame);
		// From where it was, in the weapon or at the waist, into the palm
		const Mat34 from = (events & Reload::Event_GrabbedOld) ? g_MagazineWorld : PouchWorld(*profile, g_MagazineWorld, weaponFrame, m_WaistWorld, m_WaistForward);
		g_HeldAtGrab = Multiply(InvertRigid(handFrame), from);
		g_GrabTime = now;
	}

	if (events & (Reload::Event_Inserted | Reload::Event_ShellInserted))
	{
		g_InsertFrom = Multiply(InvertRigid(g_MagazineWorld), heldWorld);
		g_InsertTime = now;

		if (serverCommits)
		{
			char command[64];
			snprintf(command, sizeof(command), "l4d2vr_reload %d %d %d %d", (events & Reload::Event_ShellInserted) ? 1 : 0, ammoType, inputs.reserve, inputs.capacity);
			m_Game->m_EngineClient->ServerCmd(command, true);
		}
		else
		{
			// The host doesn't have the mod: the game's own reload, which it can't tell apart from a button press
			isReloading = true;
		}
	}

	// Feedback: what was taken is felt in the hand that took it, what went in or reached its end in both
	if (events & (Reload::Event_GrabbedOld | Reload::Event_GrabbedFresh | Reload::Event_GrabbedBolt))
		m_Haptics.Play(Haptics::Hand::OtherHand, HapticEffects::Take);
	if (events & (Reload::Event_Inserted | Reload::Event_ShellInserted | Reload::Event_BoltPulled | Reload::Event_MagazineOut))
		m_Haptics.Play(Haptics::Hand::BothHands, HapticEffects::Click);

	if (events & Reload::Event_MagazineOut)
		PlayWeaponSound(profile->sounds->magazineOut);
	if (events & (Reload::Event_Inserted | Reload::Event_ShellInserted))
		PlayWeaponSound(profile->sounds->magazineIn);
	if (events & Reload::Event_BoltPulled)
		PlayWeaponSound(profile->sounds->boltBack);
	if (events & Reload::Event_BoltReleased)
		PlayWeaponSound(profile->sounds->boltForward);
}

bool VR::ManualReloadBlocksFire() const
{
	return m_ReloadActive && m_ManualReloadEnabled && FindProfile(m_ActiveWeaponId) && g_Machine.BlocksFire(m_ReloadClip, m_ReloadUsesShells);
}

// With manual reload the reload button does nothing on the weapons reloaded by hand
bool VR::ManualReloadOwnsReloadButton() const
{
	return m_ReloadActive && m_ManualReloadEnabled && FindProfile(m_ActiveWeaponId) != nullptr;
}

bool VR::ManualReloadBlocksNativeReload() const
{
	return m_ReloadActive && m_ManualReloadEnabled && FindProfile(m_ActiveWeaponId) != nullptr && m_Game->m_PoseRelay->IsServerCapable();
}

bool VR::ManualReloadHandIsBusy() const
{
	return m_ManualReloadEnabled && g_Machine.HandIsBusy();
}

void VR::ClearManualReloadCache()
{
	g_ViewmodelBones.clear();
	g_LastViewmodelDraw = 0;
}

// Called for each draw of the weapon in first person. Finds out where its parts are, and returns the bones to draw with
// when they aren't where the game's animation has them: the magazine out or going in, the bolt pulled or locked back,
// the pump of a pump action where the hand leaves it. `detached` is a second draw of the same model showing only the
// magazine (or shell): in the hand, going in, or at the waist.
bool VR::BuildReloadViewmodel(const void *studioHeader, const void *customBones, void *outBones, void *outDetached, bool &drawDetached)
{
	drawDetached = false;
	m_PumpHandShift = 0.0f;

	const Profile *profile = m_ManualReloadEnabled ? FindProfile(m_ActiveWeaponId) : nullptr;
	if (!profile || !Studio::IsValid(studioHeader))
		return false;

	const int boneCount = Studio::BoneCount(studioHeader);
	if (boneCount <= 0 || boneCount > Studio::kMaxBones)
		return false;

	// While switching weapons, the new weapon's model is drawn for a frame with the old weapon still active, or the
	// other way around: this model is drawn as it is then
	ViewmodelBones &cached = g_ViewmodelBones[studioHeader];
	if (!cached.nameChecked)
	{
		cached.nameChecked = true;
		cached.byName = FindProfileByModel(Studio::ModelName(studioHeader));
	}
	if (cached.byName && cached.byName != profile)
		return false;

	auto found = g_ViewmodelBones.find(studioHeader);
	if (found->second.builtFor != profile)
	{
		ViewmodelBones ids;
		ids.builtFor = profile;
		ids.byName = cached.byName;
		ids.nameChecked = true;
		ids.magazine = Studio::FindBone(studioHeader, profile->magazineBone);
		ids.bullets = profile->bulletsBone ? Studio::FindBone(studioHeader, profile->bulletsBone) : -1;
		ids.bolt = profile->boltBone ? Studio::FindBone(studioHeader, profile->boltBone) : -1;
		if (ids.bolt >= 0)
		{
			// Until the weapon has been idle, the model's own pose: where the mesh was made
			ids.boltParent = Studio::BoneParent(studioHeader, ids.bolt);
			if (ids.boltParent >= 0 && ids.boltParent < boneCount)
				ids.boltRest = Multiply(Studio::PoseToBone(studioHeader, ids.boltParent), InvertRigid(Studio::PoseToBone(studioHeader, ids.bolt)));
			else
				ids.boltParent = -1;
		}
		found->second = ids;

		if (ids.magazine < 0)
			Diagnostics::Log("manual reload: %s has no %s, this weapon uses the game's reload", Studio::ModelName(studioHeader), profile->magazineBone);
		else
			Diagnostics::Log("manual reload: %s, magazine bone %d, bolt bone %d (%s)", Studio::ModelName(studioHeader), ids.magazine, ids.bolt, profile->boltBone ? profile->boltBone : "none");
	}

	ViewmodelBones &ids = found->second;
	if (ids.magazine < 0)
		return false;

	const Mat34 *source = (const Mat34 *)customBones;
	const ULONGLONG now = GetTickCount64();
	const Vec3 forward = ToVec3(m_ViewmodelForward);

	g_MagazineWorld = source[ids.magazine];
	g_Socket = TransformPoint(g_MagazineWorld, ArrayToVec3(profile->magazineCenter));
	g_MagazineBase = g_Socket - ToVec3(m_ViewmodelUp) * (profile->magazineLength * 0.5f + 0.5f);
	g_WeaponPoint = ToVec3(GetRecommendedViewmodelAbsPos());
	g_HaveBolt = ids.bolt >= 0;
	if (g_HaveBolt)
	{
		// Where it's drawn: back when it's locked back
		const float lockedBack = g_Machine.BoltLockedBack() ? profile->boltTravel : 0.0f;
		g_BoltGrab = TransformPoint(source[ids.bolt], ArrayToVec3(profile->boltCenter)) - forward * lockedBack;
	}
	g_LastViewmodelDraw = now;

	// Where the mark goes: relative to the right controller in its OpenVR frame (x right, y up, z back). It faces the eyes,
	// on the bolt with its arrow pointing the way it's pulled, or where the magazine goes with the arrow pointing in.
	const bool markMagazine = !profile->shells && g_Machine.MagazineIsOut();
	if (g_HaveBolt || markMagazine)
	{
		const Vec3 markPoint = markMagazine ? g_Socket : g_BoltGrab;
		const Vec3 controllerRight = ToVec3(m_RightControllerRightRaw), controllerUp = ToVec3(m_RightControllerUpRaw), controllerForward = ToVec3(m_RightControllerForwardRaw);
		auto toController = [&](const Vec3 &v) { return Vec3(Dot(v, controllerRight), Dot(v, controllerUp), -Dot(v, controllerForward)); };

		const Vec3 normal = Normalized(ToVec3(m_HmdPosAbs) - markPoint);
		Vec3 along = markMagazine ? ToVec3(m_ViewmodelUp) : forward * -1.0f;
		along = Normalized(along - normal * Dot(along, normal), Normalized(Cross(Vec3(0.0f, 0.0f, 1.0f), normal)));
		const Vec3 x = toController(along), y = toController(Cross(normal, along)), z = toController(normal);
		const Vec3 p = toController(markPoint - ToVec3(m_RightControllerPosAbsRaw)) * (1.0f / m_VRScale);
		g_MarkerTransform = { { { x.x, y.x, z.x, p.x }, { x.y, y.y, z.y, p.y }, { x.z, y.z, z.z, p.z } } };
		g_MarkerPlaced = IsFinite(FromAxes(x, y, z, p));
	}

	// With a host without the mod the game still works it by itself, and that is what is shown and heard
	const bool byHand = profile->firePoseMs > 0 && m_Game->m_PoseRelay->IsServerCapable();
	if (profile->firePoseMs > 0 && ids.actionSounds != (byHand ? 0 : 1))
	{
		ids.actionSounds = byHand ? 0 : 1;
		const int found = SetActionSounds(studioHeader, !byHand);
		g_ActionSoundsOff = g_ActionSoundsOff || byHand;
		Diagnostics::Log("manual reload: %s, %d action sounds in the fire animation %s", Studio::ModelName(studioHeader), found, byHand ? "off" : "on");
	}

	const bool pinPump = byHand && g_HaveBolt && ids.boltParent >= 0;
	if (pinPump && now - g_DeployTime > kIdleAfterDeployMs && now - g_LastShotTime > kIdleAfterShotMs && !IsWeaponReloading())
		ids.boltRest = Multiply(InvertRigid(source[ids.boltParent]), source[ids.bolt]);

	const Reload::State state = g_Machine.GetState();
	const bool holding = state == Reload::State::HoldingOld || state == Reload::State::HoldingFresh;
	const bool inserting = g_InsertTime != 0 && now - g_InsertTime < (ULONGLONG)kInsertMs;
	// The bag (body_items.cpp) is where the ammo is. Only if the game has no bag model, the magazine itself hangs there.
	const bool showPouch = !holding && !inserting && m_ReloadPouchVisible && !m_AmmoBagDrawn;
	const float boltPull = g_Machine.BoltPull();
	const bool magazineOut = !profile->shells && (g_Machine.MagazineIsOut() || state == Reload::State::HoldingOld || inserting);

	if (!holding && !inserting && !showPouch && boltPull <= 0.0f && !magazineOut && !pinPump)
		return false;

	Mat34 *bones = (Mat34 *)outBones;
	memcpy(bones, customBones, boneCount * sizeof(Mat34));

	if (magazineOut)
	{
		bones[ids.magazine] = CollapsedAt(g_Socket);
		if (ids.bullets >= 0)
			bones[ids.bullets] = CollapsedAt(g_Socket);
	}

	if (pinPump)
	{
		// The game pumps after every shot in its fire animation: the pump stays where the hand has it instead.
		// A hand holding the pump in the animation goes back with it (BuildFirstPersonHands).
		const Mat34 rest = Multiply(source[ids.boltParent], ids.boltRest);
		const float animatedBack = Dot(Origin(rest) - Origin(source[ids.bolt]), forward);
		if (animatedBack > ids.gamePumpTravel + 0.5f)
		{
			ids.gamePumpTravel = animatedBack;
			Diagnostics::Log("manual reload: the game's animation takes the pump of %s %.1f units back", Studio::ModelName(studioHeader), animatedBack);
		}
		bones[ids.bolt] = rest;
		SetAxis(bones[ids.bolt], 3, Origin(rest) - forward * boltPull);
		if (profile->pump)
			m_PumpHandShift = boltPull - animatedBack;
	}
	else if (boltPull > 0.0f && g_HaveBolt)
	{
		SetAxis(bones[ids.bolt], 3, Origin(bones[ids.bolt]) - forward * boltPull);
	}

	if (holding || inserting || showPouch)
	{
		Mat34 held;
		if (holding)
		{
			const float settled = std::min((now - g_GrabTime) / kHeldSettleMs, 1.0f);
			held = Multiply(GetLeftHandGrabFrame(), Blend(g_HeldAtGrab, g_HeldInHand, settled));
		}
		else if (inserting)
		{
			held = Multiply(source[ids.magazine], Blend(g_InsertFrom, Mat34(), EaseOut((now - g_InsertTime) / kInsertMs)));
		}
		else
		{
			const Mat34 weaponFrame = FromAxes(forward, ToVec3(m_ViewmodelRight) * -1.0f, ToVec3(m_ViewmodelUp));
			held = PouchWorld(*profile, g_MagazineWorld, weaponFrame, m_WaistWorld, m_WaistForward);
		}

		Mat34 *detached = (Mat34 *)outDetached;
		const Vec3 point = Origin(held);
		for (int i = 0; i < boneCount; ++i)
			detached[i] = CollapsedAt(point);

		detached[ids.magazine] = held;
		if (ids.bullets >= 0)
			detached[ids.bullets] = Multiply(held, Multiply(InvertRigid(source[ids.magazine]), source[ids.bullets]));

		drawDetached = true;
	}

	return true;
}
