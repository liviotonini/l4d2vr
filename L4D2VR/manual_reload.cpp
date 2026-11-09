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

	// Measured from the game's models: the bones of the magazine (the shell for shotguns) and of the bolt (the slide of
	// pistols, the pump of pump shotguns), the center of their mesh relative to the bone, and how far the bolt travels.
	struct Profile
	{
		int weaponId;
		const char *magazineBone;
		const char *bulletsBone;
		const char *boltBone;
		float magazineCenter[3];
		float boltCenter[3];
		bool shells;
		int capacity;
		float boltTravel;
	};

	const Profile kProfiles[] =
	{
		{ C_WeaponCSBase::M16A1, "ValveBiped.weapon_clip", "ValveBiped.weapon_clip_bullets", "ValveBiped.weapon_bolt", { -0.01f, -1.73f, -0.30f }, { 0.21f, 1.22f, -8.56f }, false, 50, 2.5f },
		{ C_WeaponCSBase::AK47, "ValveBiped.weapon_clip", "ValveBiped.weapon_clip_bullets", "ValveBiped.weapon_bolt", { 0.01f, -2.89f, 3.04f }, { -1.37f, -0.25f, 0.88f }, false, 40, 2.5f },
		{ C_WeaponCSBase::SCAR, "ValveBiped.weapon_clip", "ValveBiped.weapon_clip_bullets", "ValveBiped.weapon_bolt", { -0.01f, -3.10f, -0.35f }, { 0.96f, 1.09f, 0.65f }, false, 60, 2.5f },
		{ C_WeaponCSBase::SG552, "ValveBiped.weapon_clip", nullptr, "ValveBiped.weapon_bolt", { 0.24f, -1.98f, 0.26f }, { 0.79f, 1.13f, 2.79f }, false, 50, 2.5f },
		{ C_WeaponCSBase::UZI, "ValveBiped.weapon_clip", nullptr, "ValveBiped.weapon_bolt", { -0.02f, -3.03f, 0.05f }, { 0.01f, 0.33f, -7.39f }, false, 50, 1.5f },
		{ C_WeaponCSBase::MAC10, "ValveBiped.weapon_clip", "ValveBiped.weapon_clip_bullets", "ValveBiped.weapon_bolt", { -0.07f, -3.25f, -0.04f }, { -0.16f, 0.24f, -9.07f }, false, 50, 1.5f },
		{ C_WeaponCSBase::MP5, "ValveBiped.weapon_clip", nullptr, "ValveBiped.weapon_bolt", { -0.00f, -2.71f, 0.78f }, { 0.77f, 0.30f, -0.39f }, false, 50, 1.5f },
		{ C_WeaponCSBase::HUNTING_RIFLE, "ValveBiped.weapon_clip", "ValveBiped.weapon_clip_bullets", "ValveBiped.weapon_bolt", { 0.06f, 0.70f, -0.15f }, { -0.39f, 0.04f, 0.45f }, false, 15, 2.5f },
		{ C_WeaponCSBase::SNIPER_MILITARY, "ValveBiped.weapon_clip", "ValveBiped.weapon_clip_bullets", "ValveBiped.weapon_charger_slide", { 0.12f, -0.83f, 2.74f }, { 0.74f, 0.23f, 2.05f }, false, 30, 2.5f },
		{ C_WeaponCSBase::AWP, "ValveBiped.weapon_clip", nullptr, "ValveBiped.weapon_bolt", { 0.01f, -1.35f, 0.70f }, { 2.22f, -0.54f, -0.20f }, false, 20, 2.5f },
		{ C_WeaponCSBase::SCOUT, "ValveBiped.weapon_clip", nullptr, "ValveBiped.weapon_bolt", { -0.17f, -1.16f, 1.71f }, { 0.97f, 0.89f, 1.13f }, false, 15, 2.5f },
		{ C_WeaponCSBase::PISTOL, "ValveBiped.weapon_clip", "ValveBiped.weapon_clip_bullets", "ValveBiped.weapon_charger_slide", { -0.03f, -0.32f, 0.51f }, { 0.63f, 1.38f, 6.35f }, false, 15, 1.2f },
		{ C_WeaponCSBase::MAGNUM, "ValveBiped.weapon_clip", "ValveBiped.weapon_clip_bullets", "ValveBiped.weapon_charger_slide", { -0.01f, -2.89f, 1.13f }, { 0.47f, 2.26f, 5.10f }, false, 8, 1.5f },
		{ C_WeaponCSBase::PUMPSHOTGUN, "ValveBiped.weapon_clip_bullets", nullptr, "ValveBiped.weapon_charger_slide", { -0.00f, 0.07f, 1.37f }, { -0.01f, 0.31f, -4.80f }, true, 8, 5.5f },
		{ C_WeaponCSBase::SHOTGUN_CHROME, "ValveBiped.weapon_clip_bullets", nullptr, "ValveBiped.weapon_charger_slide", { -0.00f, 0.07f, 1.37f }, { -0.05f, 0.17f, -5.32f }, true, 8, 5.5f },
		{ C_WeaponCSBase::AUTOSHOTGUN, "ValveBiped.weapon_clip", nullptr, "ValveBiped.weapon_bolt", { -0.00f, 0.21f, 0.04f }, { -0.89f, -0.27f, 0.79f }, true, 10, 2.0f },
		{ C_WeaponCSBase::SPAS, "ValveBiped.weapon_clip", nullptr, "ValveBiped.weapon_bolt", { 0.00f, 0.20f, 0.02f }, { -1.06f, -0.21f, -0.22f }, true, 10, 2.0f },
	};

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
		int magazine = -1;
		int bullets = -1;
		int bolt = -1;
	};

	std::unordered_map<const void *, ViewmodelBones> g_ViewmodelBones;

	// The parts of the weapon, from the last time it was drawn
	Mat34 g_MagazineWorld;
	Vec3 g_Socket;
	Vec3 g_BoltGrab;
	bool g_HaveBolt = false;
	ULONGLONG g_LastViewmodelDraw = 0;
	int g_LastWeaponId = 0;
	// What the hand holds relative to it: where it was when grabbed, moving to where it settles
	Mat34 g_HeldAtGrab;
	Mat34 g_HeldInHand;
	ULONGLONG g_GrabTime = 0;
	const float kHeldSettleMs = 150.0f;

	// Center of what's held from the palm, game units: towards the fingers, to the side of the thumb's edge of the hand
	// (y is left, and the palm of the left hand faces right) and up. Against the palm and sticking out of it, not inside it.
	const Vec3 kMagazineInPalm(0.4f, -1.9f, 0.7f);
	const Vec3 kShellInFingers(1.6f, -1.3f, 0.2f);

	// What the hand holds, at the same angle it has in the weapon
	Mat34 HeldRelativeToHand(const Profile &profile, const Mat34 &magazineWorld, const Mat34 &weaponFrame)
	{
		Mat34 relative = Multiply(TransposeRotation(weaponFrame), magazineWorld);
		const Vec3 place = profile.shells ? kShellInFingers : kMagazineInPalm;
		SetAxis(relative, 3, place - RotateVector(relative, Vec3(profile.magazineCenter[0], profile.magazineCenter[1], profile.magazineCenter[2])));
		return relative;
	}

	Vec3 ToVec3(const Vector &v) { return Vec3(v.x, v.y, v.z); }
	Vec3 ArrayToVec3(const float v[3]) { return Vec3(v[0], v[1], v[2]); }

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

// Called by UpdateTracking every frame, before it looks at the grab buttons
void VR::UpdateManualReload()
{
	const Profile *profile = m_ManualReloadEnabled ? FindProfile(m_ActiveWeaponId) : nullptr;

	if (m_ActiveWeaponId != g_LastWeaponId || !profile)
	{
		// Changed weapon: the magazine that was out is back in. A reload interrupted like this loses nothing.
		g_LastWeaponId = m_ActiveWeaponId;
		g_Machine.Reset();
		if (!profile)
			return;
	}

	// Two pistols use the game's reload. The weapon isn't drawn in some situations (held by an infected, for example).
	if (GetTickCount64() - g_LastViewmodelDraw > 200 || m_ViewmodelHasTwoWeapons)
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

	// Facing and the point the waist hangs from, without the pitch and roll of the head. Same as the items on the body.
	Vector facing(m_HmdForward.x, m_HmdForward.y, 0.0f);
	if (VectorLength(facing) < 0.2f)
		facing = Vector(m_HmdForward.x * m_HmdUp.z - m_HmdUp.x * m_HmdForward.z, m_HmdForward.y * m_HmdUp.z - m_HmdUp.y * m_HmdForward.z, 0.0f);
	VectorNormalize(facing);
	const Vector right(facing.y, -facing.x, 0.0f);

	const float neckForward = 0.08f * m_VRScale, neckDown = 0.075f * m_VRScale;
	const Vector neck = m_HmdPosLocalInWorld - m_HmdForward * neckForward - m_HmdUp * neckDown;
	const Vector reference = neck + facing * neckForward + Vector(0.0f, 0.0f, neckDown);

	const float otherSide = m_LeftHanded ? 1.0f : -1.0f;
	const Vector waistLocal = reference + right * (otherSide * 0.20f * m_VRScale) + facing * (0.02f * m_VRScale) - Vector(0.0f, 0.0f, 0.40f * m_PlayerHeightInMeters * m_VRScale);
	m_WaistWorld = ToVec3(m_CameraAnchor - Vector(0, 0, 64) + waistLocal);
	m_WaistForward = ToVec3(facing);

	Reload::Inputs inputs;
	inputs.palm = GetLeftPalm();
	inputs.socket = g_Socket;
	inputs.boltGrab = g_BoltGrab;
	inputs.boltAxis = ToVec3(m_ViewmodelForward);
	inputs.waist = m_WaistWorld;
	inputs.unitsPerMeter = m_VRScale;
	inputs.boltTravel = profile->boltTravel;
	inputs.grip = PressedDigitalAction(m_LeftHandGrab);
	// With a host that doesn't have the mod, the button is the game's reload and the game works the bolt
	const bool serverCommits = m_Game->m_PoseRelay->IsServerCapable();
	inputs.ejectPressed = serverCommits && PressedDigitalAction(m_ActionReload, true);
	inputs.clip = Read<int>(weapon, kClientClipOffset);
	inputs.capacity = std::max(profile->capacity, inputs.clip);
	// Pistols never run out of magazines, and the game reports their reserve as 0
	inputs.reserve = ReloadHasUnlimitedReserve(m_ActiveWeaponId) ? 999 : Read<int>(local, kClientAmmoArrayOffset + ammoType * 4);
	inputs.shells = profile->shells;
	inputs.hasBolt = g_HaveBolt && serverCommits;

	const Mat34 handFrame = GetLeftHandGrabFrame();

	const unsigned events = g_Machine.Update(inputs);
	m_ReloadClip = inputs.clip;
	m_ReloadUsesShells = profile->shells;
	m_ReloadActive = true;

	if (events & (Reload::Event_GrabbedOld | Reload::Event_GrabbedFresh))
	{
		const Mat34 weaponFrame = FromAxes(ToVec3(m_ViewmodelForward), ToVec3(m_ViewmodelRight) * -1.0f, ToVec3(m_ViewmodelUp));
		g_HeldInHand = HeldRelativeToHand(*profile, g_MagazineWorld, weaponFrame);
		g_HeldAtGrab = (events & Reload::Event_GrabbedOld) ? Multiply(InvertRigid(handFrame), g_MagazineWorld) : g_HeldInHand;
		g_GrabTime = GetTickCount64();
	}

	if (events & (Reload::Event_Inserted | Reload::Event_ShellInserted))
	{
		if (m_Game->m_PoseRelay->IsServerCapable())
		{
			char command[64];
			snprintf(command, sizeof(command), "l4d2vr_reload %d %d %d", (events & Reload::Event_ShellInserted) ? 1 : 0, ammoType, inputs.reserve);
			m_Game->m_EngineClient->ServerCmd(command, true);
		}
		else
		{
			// The host doesn't have the mod: the game's own reload, which it can't tell apart from a button press
			isReloading = true;
		}
	}

	if (events & (Reload::Event_GrabbedOld | Reload::Event_GrabbedFresh | Reload::Event_GrabbedBolt | Reload::Event_Inserted | Reload::Event_ShellInserted | Reload::Event_BoltPulled | Reload::Event_Ejected))
		m_Haptics.Play(Haptics::Hand::OtherHand, HapticEffects::Confirm);
}

bool VR::ManualReloadBlocksFire() const
{
	return m_ReloadActive && m_ManualReloadEnabled && FindProfile(m_ActiveWeaponId) && g_Machine.BlocksFire(m_ReloadClip, m_ReloadUsesShells);
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

// Called for each draw of the weapon in first person. Finds out where its parts are, and when the magazine or the bolt
// aren't where the animation has them, returns the bones to draw with. `detached` is a second draw of the same model
// showing only the magazine.
bool VR::BuildReloadViewmodel(const void *studioHeader, const void *customBones, void *outBones, void *outDetached, bool &drawDetached)
{
	drawDetached = false;

	const Profile *profile = m_ManualReloadEnabled ? FindProfile(m_ActiveWeaponId) : nullptr;
	if (!profile || !Studio::IsValid(studioHeader))
		return false;

	const int boneCount = Studio::BoneCount(studioHeader);
	if (boneCount <= 0 || boneCount > Studio::kMaxBones)
		return false;

	auto found = g_ViewmodelBones.find(studioHeader);
	if (found == g_ViewmodelBones.end())
	{
		ViewmodelBones ids;
		ids.magazine = Studio::FindBone(studioHeader, profile->magazineBone);
		ids.bullets = profile->bulletsBone ? Studio::FindBone(studioHeader, profile->bulletsBone) : -1;
		ids.bolt = Studio::FindBone(studioHeader, profile->boltBone);
		found = g_ViewmodelBones.emplace(studioHeader, ids).first;

		if (ids.magazine < 0)
			Diagnostics::Log("manual reload: %s has no %s, this weapon uses the game's reload", Studio::ModelName(studioHeader), profile->magazineBone);
	}

	const ViewmodelBones &ids = found->second;
	if (ids.magazine < 0)
		return false;

	const Mat34 *source = (const Mat34 *)customBones;

	g_MagazineWorld = source[ids.magazine];
	g_Socket = TransformPoint(g_MagazineWorld, ArrayToVec3(profile->magazineCenter));
	g_HaveBolt = ids.bolt >= 0;
	if (g_HaveBolt)
		g_BoltGrab = TransformPoint(source[ids.bolt], ArrayToVec3(profile->boltCenter));
	g_LastViewmodelDraw = GetTickCount64();

	const Reload::State state = g_Machine.GetState();
	const bool showHeld = state == Reload::State::HoldingOld || state == Reload::State::HoldingFresh;
	const bool showAtWaist = state == Reload::State::NeedFresh && !profile->shells;
	const float boltPull = g_Machine.BoltPull();
	const bool magazineOut = !profile->shells && (g_Machine.MagazineIsOut() || state == Reload::State::HoldingOld);

	if (!showHeld && !showAtWaist && boltPull <= 0.0f && !magazineOut)
		return false;

	Mat34 *bones = (Mat34 *)outBones;
	memcpy(bones, customBones, boneCount * sizeof(Mat34));

	if (magazineOut)
	{
		bones[ids.magazine] = CollapsedAt(g_Socket);
		if (ids.bullets >= 0)
			bones[ids.bullets] = CollapsedAt(g_Socket);
	}

	if (boltPull > 0.0f && g_HaveBolt)
	{
		const Vec3 moved = Origin(bones[ids.bolt]) - ToVec3(m_ViewmodelForward) * boltPull;
		SetAxis(bones[ids.bolt], 3, moved);
	}

	if (showHeld || showAtWaist)
	{
		Mat34 held;
		if (showHeld)
		{
			const float settled = std::min((GetTickCount64() - g_GrabTime) / kHeldSettleMs, 1.0f);
			held = Multiply(GetLeftHandGrabFrame(), Blend(g_HeldAtGrab, g_HeldInHand, settled));
		}
		else
		{
			// At the waist, hanging the way it sits in the weapon
			const Mat34 weaponFrame = FromAxes(ToVec3(m_ViewmodelForward), ToVec3(m_ViewmodelRight) * -1.0f, ToVec3(m_ViewmodelUp));
			Mat34 relative = Multiply(TransposeRotation(weaponFrame), g_MagazineWorld);
			SetAxis(relative, 3, RotateVector(relative, ArrayToVec3(profile->magazineCenter)) * -1.0f);

			const Vec3 forward = Normalized(m_WaistForward);
			const Vec3 up(0.0f, 0.0f, 1.0f);
			held = Multiply(FromAxes(forward, Cross(up, forward), up, m_WaistWorld), relative);
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
