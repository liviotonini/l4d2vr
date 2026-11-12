#include "left_gun.h"
#include "vr.h"
#include <Windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include "sdk.h"
#include "sdk_server.h"
#include "game.h"
#include "hooks.h"
#include "dual_wield.h"
#include "diagnostics.h"
#include "reload_logic.h"
#include "pose_relay.h"
#include "trace.h"
#include <initializer_list>

using namespace HandMath;

// ---------------------------------------------------------------------------------------------------------------
// The left hand's gun, see left_gun.h.
//
// The game has one active weapon, the right hand's, and its shots come from the right hand. The left hand's gun stays in
// the inventory (the extra slot, dual_wield.cpp): its trigger is the left one, and the mod fires it. Here the shot's sound
// and its hits are shown at once, and the server shoots its bullets (FireTerrorBullets, past the hook that moves the
// right hand's shots), taking them from the gun's magazine, as fast as the gun shoots.
// ---------------------------------------------------------------------------------------------------------------

namespace
{
	const LeftGun::GunData kGuns[] =
	{
		{ C_WeaponCSBase::PISTOL, 0.15f, LeftGun::Semi, 1.5f, "Pistol.Fire", true, false, false, 0, "Pistol.ClipIn", "weapon_muzzle_flash_pistol", "weapon_shell_casing_9mm_L4D2", 6.0f, false },
		{ C_WeaponCSBase::MAGNUM, 0.15f, LeftGun::Semi, 1.25f, "Magnum.Fire", true, false, false, 0, "Magnum.ClipIn", "weapon_muzzle_flash_autoshotgun", "weapon_shell_casing_desert_eagle", 10.0f, false },
		{ C_WeaponCSBase::UZI, 0.0625f, LeftGun::Auto, 1.0f, "SMG.Fire", false, true, false, 650, "SMG.ClipIn", "weapon_muzzle_flash_smg", "weapon_shell_casing_9mm", 3.0f, false },
		{ C_WeaponCSBase::MAC10, 0.0625f, LeftGun::Auto, 1.2f, "SMG_Silenced.Fire", false, true, false, 650, "SMG_Silenced.ClipIn", "weapon_muzzle_flash_smg_silenced", "weapon_shell_casing_9mm_L4D2", 3.0f, false },
		{ C_WeaponCSBase::MP5, 0.075f, LeftGun::Auto, 1.1f, "Weapon_MP5Navy.Single", false, true, false, 650, "SMG.ClipIn", "weapon_muzzle_flash_smg", "weapon_shell_casing_9mm", 2.5f, false },
		// The pump shotguns are pumped between shots
		{ C_WeaponCSBase::PUMPSHOTGUN, 0.8f, LeftGun::Semi, 0.8f, "Shotgun.Fire", false, true, true, 56, nullptr, "weapon_muzzle_flash_autoshotgun", "weapon_shell_casing_shotgun", 9.0f, true },
		{ C_WeaponCSBase::SHOTGUN_CHROME, 0.8f, LeftGun::Semi, 0.8f, "Shotgun_Chrome.Fire", false, true, true, 56, nullptr, "weapon_muzzle_flash_autoshotgun", "weapon_shell_casing_shotgun", 9.0f, true },
		{ C_WeaponCSBase::AUTOSHOTGUN, 0.15f, LeftGun::Semi, 0.8f, "AutoShotgun.Fire", false, true, true, 90, nullptr, "weapon_muzzle_flash_autoshotgun", "weapon_shell_casing_shotgun", 7.0f, false },
		{ C_WeaponCSBase::SPAS, 0.15f, LeftGun::Semi, 0.75f, "AutoShotgun_Spas.Fire", false, true, true, 90, nullptr, "weapon_muzzle_flash_autoshotgun", "weapon_shell_casing_shotgun", 7.0f, false },
		{ C_WeaponCSBase::M16A1, 0.0875f, LeftGun::Auto, 0.4f, "Rifle.Fire", false, true, false, 360, "Rifle.ClipIn", "weapon_muzzle_flash_assaultrifle", "weapon_shell_casing_rifle", 3.0f, false },
		{ C_WeaponCSBase::SCAR, 0.07f, LeftGun::Burst, 0.35f, "Rifle_Desert.Fire", false, true, false, 360, "Rifle_Desert.ClipIn", "weapon_muzzle_flash_assaultrifle", "weapon_shell_casing_rifle", 3.5f, false },
		{ C_WeaponCSBase::AK47, 0.13f, LeftGun::Auto, 1.0f, "AK47.Fire", false, true, false, 360, "AK47.ClipIn", "weapon_muzzle_flash_smg", "weapon_shell_casing_rifle", 4.0f, false },
		{ C_WeaponCSBase::SG552, 0.0825f, LeftGun::Auto, 0.4f, "Weapon_SG552.Single", false, true, false, 360, "Rifle.ClipIn", "weapon_muzzle_flash_assaultrifle", "weapon_shell_casing_rifle", 3.0f, false },
		// The M60 isn't reloaded, as in the game
		{ C_WeaponCSBase::M60, 0.11f, LeftGun::Auto, 1.0f, "M60.Fire", false, true, false, 0, nullptr, "weapon_muzzle_flash_assaultrifle", "weapon_shell_casing_m60", 3.5f, false },
		{ C_WeaponCSBase::HUNTING_RIFLE, 0.25f, LeftGun::Semi, 0.1f, "HuntingRifle.Fire", false, true, false, 150, "HuntingRifle.ClipIn", "weapon_muzzle_flash_huntingrifle", "weapon_shell_casing_rifle", 6.0f, false },
		{ C_WeaponCSBase::SNIPER_MILITARY, 0.25f, LeftGun::Semi, 0.5f, "Sniper_Military.Fire", false, true, false, 180, "Sniper_Military.ClipIn", "weapon_muzzle_flash_huntingrifle", "weapon_shell_casing_rifle", 6.0f, false },
		// The bolt action rifles are worked between shots
		{ C_WeaponCSBase::AWP, 1.05f, LeftGun::Semi, 0.1f, "Weapon_AWP.Single", false, true, false, 180, "HuntingRifle.ClipIn", "weapon_muzzle_flash_huntingrifle", "weapon_shell_casing_rifle", 9.0f, true },
		{ C_WeaponCSBase::SCOUT, 0.9f, LeftGun::Semi, 0.1f, "Weapon_Scout.Single", false, true, false, 180, "HuntingRifle.ClipIn", "weapon_muzzle_flash_huntingrifle", "weapon_shell_casing_rifle", 7.0f, true },
	};

	// The guns' world models (LeftGun::GunModel). The magazine's middle is the middle of its vertices' box; the shotguns'
	// is where shells go in, under the receiver. A bolt with vertices of its own is the middle of them; otherwise where the
	// part is on the model (the slide's back, the charging handle, the bolt handle by the ejection port). The pump of the
	// pump shotguns is their "weapon_bone_Clip", the forend.
	// The grip is the middle of the pistol grip at mid height (the weapon bone is at its top, where the hand's web goes),
	// and how much it leans; on the guns with a plain stock (the pump shotguns, the hunting rifle, the scout), the stock's
	// wrist behind the trigger guard. Read off side views of the models drawn from their triangles.
	const LeftGun::GunModel kGunModels[] =
	{
		{ "w_pistol_a.mdl", { 0.0f, -1.15f, 0.75f }, 2.85f, { 0.0f, 1.9f, 0.3f }, false, { 0.0f, -1.7f, 0.05f }, 12.0f },
		{ "w_desert_eagle.mdl", { 0.0f, -1.0f, 0.75f }, 2.4f, { 0.0f, 2.0f, 0.7f }, false, { 0.0f, -1.6f, 0.0f }, 12.0f },
		{ "w_smg_uzi.mdl", { 0.0f, -2.55f, 1.0f }, 3.75f, { 0.0f, 3.6f, 6.3f }, false, { 0.0f, -1.5f, 0.45f }, 6.0f },
		{ "w_smg_a.mdl", { 0.0f, -1.9f, 0.9f }, 4.3f, { 0.0f, 4.0f, -2.5f }, false, { 0.0f, -1.6f, 0.3f }, 8.0f },
		{ "w_smg_mp5.mdl", { 0.45f, -1.45f, 7.75f }, 4.15f, { 0.8f, 4.6f, 11.0f }, false, { 0.0f, -1.6f, -0.6f }, 32.0f },
		{ "w_shotgun.mdl", { 0.0f, 0.8f, 5.0f }, 0.0f, { 0.2f, 2.5f, 14.3f }, true, { 0.0f, 1.0f, -1.0f }, 45.0f },
		{ "w_pumpshotgun_a.mdl", { 0.0f, 0.8f, 5.0f }, 0.0f, { 0.2f, 2.4f, 14.2f }, true, { 0.0f, 1.0f, -1.0f }, 45.0f },
		{ "w_autoshot_m4super.mdl", { 0.0f, 1.0f, 5.5f }, 0.0f, { -1.0f, 2.6f, 7.5f }, false, { 0.0f, -1.6f, -0.4f }, 25.0f },
		{ "w_shotgun_spas.mdl", { 0.0f, 1.0f, 8.0f }, 0.0f, { -1.0f, 2.8f, 6.0f }, false, { 0.0f, -1.7f, -0.3f }, 22.0f },
		{ "w_rifle_m16a2.mdl", { 0.0f, -1.4f, 6.05f }, 3.2f, { 0.0f, 3.4f, -0.8f }, false, { 0.0f, -1.6f, -0.4f }, 26.0f },
		{ "w_desert_rifle.mdl", { 0.0f, -1.9f, 6.4f }, 4.0f, { 0.0f, 3.9f, 8.7f }, false, { 0.0f, -1.6f, -0.4f }, 28.0f },
		{ "w_rifle_ak47.mdl", { 0.0f, -1.95f, 7.95f }, 4.55f, { -1.0f, 2.9f, 8.2f }, false, { 0.0f, -1.6f, -0.3f }, 22.0f },
		{ "w_rifle_sg552.mdl", { 0.0f, -0.5f, 6.2f }, 3.3f, { -1.2f, 3.3f, 8.5f }, false, { 0.0f, -1.6f, -0.4f }, 26.0f },
		{ "w_m60.mdl", { 0.0f, 0.0f, 0.0f }, 0.0f, { -1.5f, 4.0f, 3.0f }, false, { 0.0f, -1.6f, -0.3f }, 22.0f },
		{ "w_sniper_mini14.mdl", { 0.0f, 1.2f, 7.3f }, 1.7f, { -0.8f, 3.1f, 8.7f }, false, { 0.0f, 1.0f, -1.0f }, 45.0f },
		{ "w_sniper_military.mdl", { 0.0f, 0.7f, 6.7f }, 3.4f, { 1.0f, 4.4f, 13.0f }, false, { 0.0f, -1.4f, -0.6f }, 30.0f },
		{ "w_sniper_awp.mdl", { 0.25f, 0.4f, 7.0f }, 1.8f, { -1.2f, 2.8f, 5.0f }, false, { 0.0f, -0.9f, -0.6f }, 35.0f },
		{ "w_sniper_scout.mdl", { 0.0f, 0.5f, 6.75f }, 1.7f, { -2.0f, 2.4f, 3.0f }, false, { 0.0f, 0.6f, -1.0f }, 45.0f },
	};

	// The grip of a gun whose model isn't one of these
	const Vec3 kDefaultGrip(0.0f, -1.6f, -0.3f);
	const float kDefaultGripRake = 20.0f;

	Vec3 ToVec3(const Vector &v) { return Vec3(v.x, v.y, v.z); }

	// A shot throws the gun's muzzle up in the hand and back, and its bolt goes back and forward again (the slide of a
	// pistol): for this long
	const unsigned long long kRecoilMs = 110;
	const float kRecoilBackPerDegree = 0.06f;
	const unsigned long long kBlowbackMs = 70;
	const float kBlowbackTravel = 1.5f;

	const int kBurstShots = 3;
	const unsigned long long kBurstPauseMs = 350;
	// From the middle of the fist to where the shot leaves, game units along the weapon bone: up the grip, along the barrel
	const float kMuzzleUp = 2.5f;
	const float kMuzzleForward = 6.0f;
	// The empty right hand takes a long gun of the left hand within this of the barrel, from a little in front of the left
	// hand to about where the guns end (units along it, meters from it)
	const float kSupportFrom = 4.0f;
	const float kSupportTo = 34.0f;
	const float kSupportReach = 0.12f;
	// Closer to the left hand than this, units, the right hand doesn't turn the gun
	const float kSupportMinSpan = 3.0f;
	// The server's checks: the shot leaves near the eyes, and not faster than the gun (with room for the network)
	const float kMaxShotFromEyes = 120.0f;
	const float kServerCycleShare = 0.7f;

	// m_iClip1 of a weapon, on the client and on the server; m_iPrimaryAmmoType of a weapon and m_iAmmo of a player
	const int kClientClipOffset = 0x984;
	const int kServerClipOffset = 0x1414;
	const int kClientAmmoTypeOffset = 0x97C;
	const int kClientAmmoArrayOffset = 0xF24;
	const int kServerAmmoArrayOffset = 0x1874;
	// The right hand puts the magazine in within this of where it goes, meters
	const float kMagazineWellReach = 0.12f;
	// The muzzle flash's length, units
	const float kMuzzleFlashLength = 8.0f;

	const HapticEffect kShotHaptic = { { 0.0f, 0.03f, 190.0f, 0.55f } };

	Vector ToVector(const Vec3 &v) { return Vector(v.x, v.y, v.z); }

	template <typename T>
	T Read(const void *entity, int offset)
	{
		return *(const T *)((const uint8_t *)entity + offset);
	}
}

const LeftGun::GunData *LeftGun::Find(int weaponId)
{
	for (const GunData &gun : kGuns)
	{
		if (gun.weaponId == weaponId)
			return &gun;
	}
	return nullptr;
}

const LeftGun::GunModel *LeftGun::FindModel(const char *modelName)
{
	if (!modelName)
		return nullptr;
	const char *file = modelName;
	for (const char *c = modelName; *c; ++c)
	{
		if (*c == '/' || *c == '\\')
			file = c + 1;
	}
	for (const GunModel &model : kGunModels)
	{
		if (_stricmp(model.model, file) == 0)
			return &model;
	}
	return nullptr;
}

// ---------------------------------------------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------------------------------------------

// The grip of the left hand's gun in its frame (LeftGun::GunModel): y up the grip, leaning forward at the top, z towards
// the muzzle, at its middle
Mat34 VR::LeftGunGrip() const
{
	const LeftGun::GunModel *model = m_LeftGunModelIndex == m_LeftItemIndex ? m_LeftGunModel : nullptr;
	const float radians = (model ? model->gripRake : kDefaultGripRake) * 3.14159265f / 180.0f;
	const Vec3 up(0.0f, cosf(radians), sinf(radians)), forward(0.0f, -sinf(radians), cosf(radians));
	return FromAxes(Cross(up, forward), up, forward, model ? model->grip : kDefaultGrip);
}

// The frame the left hand's item is drawn in: its fist (y along the grip, z where the fingers point). A gun points the
// way the right hand's does (the controller turned by the aim's angle, ControllerAngleAdjustment), or at the right hand
// while that one holds it too, with its grip where the fist is: the hand turns to hold it (GetLeftHandDrawn), as the
// game's animation holds the right hand's weapon.
bool VR::GetLeftItemFrame(Mat34 &frame) const
{
	if (!GetLeftGripFrame(frame))
		return false;
	if (!LeftGun::Find(m_LeftItemId))
		return true;

	const Vec3 fist = Origin(frame);
	Vec3 forward = Normalized(ToVec3(m_LeftControllerForward)), up = Normalized(ToVec3(m_LeftControllerUp));
	const Vec3 toRight = GetFistPoint(true) - fist;
	if (m_RightHoldsLeftGun && Length(toRight) >= kSupportMinSpan)
	{
		const Vec3 left = Cross(up, forward);
		forward = Normalized(toRight);
		up = Normalized(up - forward * Dot(up, forward), Cross(forward, left));
	}
	frame = FromAxes(Cross(up, forward), up, forward, Vec3());
	SetAxis(frame, 3, fist - RotateVector(frame, Origin(LeftGunGrip())));
	return IsFinite(frame);
}

// The left hand bone as it's drawn (BuildFirstPersonHands): where its controller is, or closed on the grip of its gun
// (GetLeftItemFrame), its fist where it would be
Mat34 VR::GetLeftHandDrawn(const HandRig &rig) const
{
	Mat34 gun;
	if (LeftHandShowsItem() && LeftGun::Find(m_LeftItemId) && m_LeftGripValid && GetLeftItemFrame(gun))
	{
		const Mat34 hand = Multiply(Multiply(gun, LeftGunGrip()), InvertRigid(m_LeftGripInHand));
		if (IsFinite(hand))
			return hand;
	}
	return GetLeftHandBone(rig);
}

// The empty right hand takes the left hand's long gun with a grab on its barrel (UpdateTracking), and holds it while the
// grab is held: the gun points from the left hand towards it. True if it took it.
bool VR::TryRightHoldLeftGun()
{
	const LeftGun::GunData *gun = LeftHandHoldsItem() ? LeftGun::Find(m_LeftItemId) : nullptr;
	Mat34 grip;
	if (!m_RightHandEmpty || !gun || !gun->twoHanded || !GetLeftItemFrame(grip))
		return false;
	const Vec3 barrel = Normalized(Axis(grip, 2));
	const float distance = Reload::DistanceToSegment(GetFistPoint(true), Origin(grip) + barrel * kSupportFrom, Origin(grip) + barrel * kSupportTo);
	if (distance > kSupportReach * m_VRScale)
		return false;
	m_RightHoldsLeftGun = true;
	m_Haptics.Play(Haptics::Hand::BothHands, HapticEffects::Take);
	Diagnostics::Log("left gun: held with both hands, the right hand %.2f m from its barrel", distance / m_VRScale);
	return true;
}

namespace
{
	// The scopes of the guns, on their world model: the middle of the eyepiece from the weapon bone, up the grip and along
	// the barrel (units, measured from the models' vertices), and the radius of the image (as in the right hand)
	struct LeftScope
	{
		int weaponId;
		float up, along, radius;
	};
	const LeftScope kLeftScopes[] =
	{
		{ C_WeaponCSBase::HUNTING_RIFLE, 5.0f, 2.0f, 0.95f },
		{ C_WeaponCSBase::SNIPER_MILITARY, 6.8f, -4.0f, 0.80f },
		{ C_WeaponCSBase::AWP, 4.8f, 1.0f, 1.30f },
		{ C_WeaponCSBase::SCOUT, 4.2f, 2.0f, 0.90f },
		{ C_WeaponCSBase::SG552, 5.1f, -1.0f, 0.75f },
	};
}

// The scope of the left hand's gun, as it's drawn (VR::UpdateScope): false if it has none
bool VR::GetLeftScope(Vec3 &eyepiece, QAngle &angles, float &radius)
{
	if (!LeftHandHoldsItem())
		return false;
	const LeftScope *scope = nullptr;
	for (const LeftScope &candidate : kLeftScopes)
	{
		if (candidate.weaponId == m_LeftItemId)
			scope = &candidate;
	}
	Mat34 frame;
	if (!scope || !GetLeftItemFrame(frame))
		return false;
	eyepiece = TransformPoint(frame, Vec3(0.0f, scope->up, scope->along));
	QAngle::VectorAngles(ToVector(Normalized(Axis(frame, 2))), ToVector(Normalized(Axis(frame, 1))), angles);
	radius = scope->radius;
	return true;
}

// The left hand's gun takes ammo now: its magazine isn't full, and there's ammo for it
bool VR::LeftGunNeedsAmmo(const LeftGun::GunData &gun) const
{
	const int capacity = GetReloadCapacity(gun.weaponId);
	if (capacity <= 0 || m_LeftGunClip >= capacity || m_LeftGunClipIndex != m_LeftItemIndex)
		return false;
	if (ReloadHasUnlimitedReserve(gun.weaponId))
		return true;
	const void *weapon = m_Game->GetClientEntity(m_LeftItemIndex);
	const void *local = m_Game->GetClientEntity(m_Game->m_EngineClient->GetLocalPlayer());
	const int ammoType = weapon ? Read<int>(weapon, kClientAmmoTypeOffset) : -1;
	return local && ammoType >= 0 && ammoType < 32 && Read<int>(local, kClientAmmoArrayOffset + ammoType * 4) > 0;
}

// Where the left hand's gun takes its magazine, as it was last drawn (BuildBodyModelBones)
bool VR::AtLeftGunWell(const Vec3 &point) const
{
	if (m_RightMagazine && GetTickCount64() - m_LeftGunWellMs < 250)
	{
		const float distance = Length(point - m_LeftGunWell) / m_VRScale;
		if (distance < kMagazineWellReach * 2.0f && distance >= kMagazineWellReach && GetTickCount64() - m_LeftGunWellNearLogMs > 1000)
		{
			m_LeftGunWellNearLogMs = GetTickCount64();
			Diagnostics::Log("left gun: the right hand's magazine is %.2f m from where it goes", distance);
		}
	}
	return GetTickCount64() - m_LeftGunWellMs < 250 && Length(point - m_LeftGunWell) < kMagazineWellReach * m_VRScale;
}

// The empty right hand reloads the left hand's gun (on the right grab, UpdateTracking): with the reload by hand it takes a
// magazine (a shell) from the ammo bag, put in where the gun's goes (UpdateLeftGun); otherwise a grab there loads it.
// True if the grab was for that.
bool VR::TryRightReloadLeftGun()
{
	// With the reload by hand the right hand's grab was taken by it already (UpdateLeftGunReload)
	if (m_ManualReloadEnabled)
		return m_LeftReloadTookGrab;

	// Without it, a grab where the magazine goes loads it
	const LeftGun::GunData *gun = LeftHandHoldsItem() ? LeftGun::Find(m_LeftItemId) : nullptr;
	if (!m_RightHandEmpty || !gun || !LeftGunNeedsAmmo(*gun) || !AtLeftGunWell(GetFistPoint(true)))
		return false;
	LoadLeftGun(*gun);
	return true;
}

namespace
{
	// The reload by hand of the left hand's gun: the same steps as the right hand's weapon (Reload::Machine), the right
	// hand doing them
	Reload::Machine g_LeftMachine;
	int g_LeftMachineGun = 0;
	bool g_RightWasAtBag = false;
	// From the weapon bone of a pump shotgun to where its pump is held, units along the barrel; and where the bolt of a
	// gun whose model has none is taken, above the grip
	const float kPumpForward = 14.0f;
	const float kBoltUp = 3.0f;
}

// Every frame, after the right hand's weapon's reload and before the grabs (UpdateTracking): the right hand takes the
// left hand's gun's magazine out and drops it, takes a new one from the ammo bag and puts it in, pulls its bolt with the
// right trigger when it ran empty, and works its pump or bolt after every shot with the trigger while it holds it too.
// The parts are where the gun was last drawn (BuildBodyModelBones).
void VR::UpdateLeftGunReload()
{
	// The mark, as on the right hand's weapon (UpdateManualReload), shown on the way out whatever that is. The right hand
	// only reloads it while it's empty, and then the mark is this one's.
	struct MarkerUpdate
	{
		VR *vr;
		bool show = false;
		bool inReach = false;
		~MarkerUpdate()
		{
			if (vr->m_RightHandEmpty)
				vr->UpdateBoltMarker(show, inReach, true);
		}
	} marker{ this };
	m_LeftMarkKind = 0;

	m_LeftReloadTookGrab = false;
	const LeftGun::GunData *gun = m_LeftItemIndex ? LeftGun::Find(m_LeftItemId) : nullptr;
	ReloadSpec spec;
	Mat34 frame;
	const unsigned long long now = GetTickCount64();
	const bool active = m_ManualReloadEnabled && gun && LeftHandHoldsItem() && GetReloadSpec(m_LeftItemId, spec) && now - m_LeftGunWellMs < 250
		&& GetLeftItemFrame(frame) && m_Game->m_PoseRelay->IsServerCapable();
	if (g_LeftMachineGun != m_LeftItemIndex)
	{
		// Another gun, or none: what was out is back in
		g_LeftMachine.Reset();
		g_LeftMachineGun = m_LeftItemIndex;
	}
	if (!active)
	{
		m_LeftGunMagazineOut = false;
		m_LeftGunBoltPull = 0.0f;
		m_LeftGunBlocked = false;
		m_RightMagazine = false;
		return;
	}

	const void *weapon = m_Game->GetClientEntity(m_LeftItemIndex);
	const void *local = m_Game->GetClientEntity(m_Game->m_EngineClient->GetLocalPlayer());
	const int ammoType = weapon ? Read<int>(weapon, kClientAmmoTypeOffset) : -1;
	if (!weapon || !local || ammoType < 0 || ammoType >= 32)
		return;

	const Vec3 barrel = Normalized(Axis(frame, 2)), up = Normalized(Axis(frame, 1));
	const Vec3 fist = GetFistPoint(true);
	// The right hand only reloads it while it holds nothing of its own
	const bool rightFree = m_RightHandEmpty;

	Reload::Inputs in;
	in.palm = fist;
	in.held = fist;
	in.socket = m_LeftGunWell;
	in.magazineBase = m_LeftGunMagazineBase;
	// Where the bolt is drawn: back when it's locked back
	in.boltGrab = m_LeftGunHasBolt ? m_LeftGunBolt : spec.pump ? Origin(frame) + barrel * kPumpForward : Origin(frame) + up * kBoltUp;
	in.boltAxis = barrel;
	in.weaponPoint = Origin(frame);
	in.waist = m_WaistWorld;
	in.unitsPerMeter = m_VRScale;
	in.boltTravel = spec.boltTravel;
	in.grip = rightFree && PressedDigitalAction(m_RightHandGrab);
	in.trigger = rightFree && PressedDigitalAction(m_ActionPrimaryAttack) && !m_LeftScopeTookTrigger;
	in.holdsPump = spec.everyShot && m_RightHoldsLeftGun;
	in.pumpEveryShot = spec.everyShot;
	in.clip = Read<int>(weapon, kClientClipOffset);
	in.capacity = std::max(spec.capacity, in.clip);
	in.reserve = ReloadHasUnlimitedReserve(m_LeftItemId) ? 999 : Read<int>(local, kClientAmmoArrayOffset + ammoType * 4);
	in.shells = spec.shells;
	in.hasBolt = spec.boltTravel > 0.0f;
	in.handFree = rightFree;

	const unsigned events = g_LeftMachine.Update(in);
	const Reload::State state = g_LeftMachine.GetState();
	m_LeftReloadTookGrab = (events & (Reload::Event_GrabbedOld | Reload::Event_GrabbedFresh)) != 0;
	m_LeftGunMagazineOut = !spec.shells && g_LeftMachine.MagazineIsOut();
	m_LeftGunBoltPull = g_LeftMachine.BoltPull();
	m_LeftGunBlocked = g_LeftMachine.BlocksFire(in.clip, spec.shells);
	m_RightMagazine = state == Reload::State::HoldingOld || state == Reload::State::HoldingFresh;
	m_LeftGunMagazineShown = !spec.shells;
	if (state != Reload::State::Idle && state != Reload::State::NeedFresh)
		m_RightHandHandledLeftGunMs = now;
	m_RightMagazineIndex = m_LeftItemIndex;

	// The mark: on the empty gun's magazine, to take it out; where the new one goes once it's out; then, if the gun ran
	// empty, on the bolt (or the pump of a gun that isn't held by it)
	if (m_ShowReloadMark)
	{
		const bool takeOut = !spec.shells && in.clip == 0 && in.reserve > 0 && state == Reload::State::Idle && !g_LeftMachine.MagazineIsOut();
		const bool magazineOut = !spec.shells && g_LeftMachine.MagazineIsOut();
		const bool chamberIt = in.hasBolt && g_LeftMachine.NeedsCycle() && in.clip > 0 && (!spec.everyShot || g_LeftMachine.ReloadInProgress())
			&& state != Reload::State::HoldingBolt && !in.holdsPump;
		m_LeftMarkKind = takeOut ? 1 : magazineOut ? 2 : chamberIt ? 3 : 0;
		marker.show = m_LeftMarkKind != 0;
		if (takeOut)
			marker.inReach = Reload::DistanceToSegment(fist, in.socket, in.magazineBase) < Reload::kGrabRadius * m_VRScale;
		else if (magazineOut)
			marker.inReach = state == Reload::State::HoldingFresh;
		else
			marker.inReach = Length(fist - in.boltGrab) < Reload::kBoltGrabRadius * m_VRScale;
	}

	// The right hand reaches the bag where it can take one: a faint tick
	const bool atBag = rightFree && in.reserve > 0 && (state == Reload::State::Idle || state == Reload::State::NeedFresh)
		&& Length(fist - m_WaistWorld) < Reload::kPouchRadius * m_VRScale;
	if (atBag && !g_RightWasAtBag)
		m_Haptics.Play(Haptics::Hand::WeaponHand, HapticEffects::Hint);
	g_RightWasAtBag = atBag;

	if (events & (Reload::Event_Inserted | Reload::Event_ShellInserted))
	{
		char command[64];
		snprintf(command, sizeof(command), "l4d2vr_left_reload %d", ammoType);
		m_Game->m_EngineClient->ServerCmd(command, true);
		Diagnostics::Log("left gun: the right hand put a %s in", spec.shells ? "shell" : "magazine");
	}
	if (events & (Reload::Event_GrabbedOld | Reload::Event_GrabbedFresh | Reload::Event_GrabbedBolt))
		m_Haptics.Play(Haptics::Hand::WeaponHand, HapticEffects::Take);
	if (events & (Reload::Event_Inserted | Reload::Event_ShellInserted | Reload::Event_BoltPulled | Reload::Event_MagazineOut))
		m_Haptics.Play(Haptics::Hand::BothHands, HapticEffects::Click);
	if (events & Reload::Event_MagazineOut)
		PlayWeaponSound(spec.magazineOutSound);
	if (events & (Reload::Event_Inserted | Reload::Event_ShellInserted))
		PlayWeaponSound(spec.magazineInSound);
	if (events & Reload::Event_BoltPulled)
		PlayWeaponSound(spec.boltBackSound);
	// A pump or a bolt action throws its case out when it's worked
	if ((events & Reload::Event_BoltPulled) && gun->workedByHand && now - m_LeftGunShellMs < 250)
		Hooks::ClientParticle(gun->brass, ToVector(m_LeftGunShell), ToVector(m_LeftGunShell + m_LeftGunShellDir * kMuzzleFlashLength));
	if (events & Reload::Event_BoltReleased)
		PlayWeaponSound(spec.boltForwardSound);
	if (events & (Reload::Event_GrabbedOld | Reload::Event_MagazineOut | Reload::Event_DroppedOld | Reload::Event_GrabbedFresh | Reload::Event_BoltPulled))
		Diagnostics::Log("left gun: reload by the right hand, events 0x%X", events);
}

namespace
{
	// The first person laser sight of C_TerrorPlayer (Offsets::ClientLaserSightCreate): a particle system on the player's
	// ParticleProp at the world's origin, whose control points are set every frame: 1 where the beam leaves, 3 32 units
	// along it, 2 where it hits. The left hand's gun's laser is one more of them. The functions are the targets of the
	// calls in that code, checked one by one.
	struct ParticleRef // CUtlReference: emptied by the effect when it goes
	{
		ParticleRef *next = nullptr;
		ParticleRef *prev = nullptr;
		void *effect = nullptr;
	};

	struct LaserCode
	{
		bool read = false, found = false;
		int particleProp = 0;    // CParticleProperty in the player
		int references = 0;      // the list of references in the effect
		const char *name = nullptr;
		void *(__thiscall *create)(void *prop, const char *name, int attachType, int attachment, float x, float y, float z, void *matrix) = nullptr;
		void(__thiscall *addControlPoint)(void *prop, void *effect, int point, void *entity, int attachType, int attachment, float x, float y, float z, void *matrix) = nullptr;
		void(__thiscall *drawForUser)(void *effect, int user) = nullptr;
		void(__thiscall *setControlPoint)(void *effect, int point, const Vector *position) = nullptr;
		void(__thiscall *stopEmission)(void *prop, void *effect) = nullptr;
	};
	LaserCode g_LaserCode;
	ParticleRef g_LeftLaser;
	const float kLaserReach = 4096.0f;
	const float kLaserDirectionPoint = 32.0f;

	bool BytesAre(const uint8_t *at, std::initializer_list<int> bytes)
	{
		for (int byte : bytes)
		{
			if (*at++ != (uint8_t)byte)
				return false;
		}
		return true;
	}

	template <typename T>
	T CallTarget(const uint8_t *call)
	{
		return (T)(call + 5 + *(const int32_t *)(call + 1));
	}

	const LaserCode &ReadLaserCode(Game *game)
	{
		if (g_LaserCode.read)
			return g_LaserCode;
		g_LaserCode.read = true;
		const uint8_t *a = (const uint8_t *)game->m_Offsets->ClientLaserSightCreate.address;
		if (!a)
			return g_LaserCode;
		__try
		{
			LaserCode code;
			code.read = true;
			code.particleProp = *(const int32_t *)(a + 4);
			code.name = *(const char *const *)(a + 9);
			const bool looksRight = code.particleProp > 0 && code.particleProp < 0x4000 && strcmp(code.name, "weapon_laser_sight_FP") == 0
				&& BytesAre(a + 0xB3, { 0x8D, 0x8E }) && *(const int32_t *)(a + 0xB5) == code.particleProp && a[0xB9] == 0xE8
				&& BytesAre(a + 0x14A, { 0x57, 0xE8 })
				&& BytesAre(a + 0x25B, { 0x50, 0x6A, 0x01, 0xE8 })
				&& BytesAre(a - 0x17C, { 0x50, 0x8D, 0x8E }) && *(const int32_t *)(a - 0x179) == code.particleProp && *(a - 0x175) == 0xE8
				&& BytesAre(a + 0x35, { 0x8D, 0x8A });
			if (looksRight)
			{
				code.create = CallTarget<decltype(code.create)>(a + 0xD);
				code.addControlPoint = CallTarget<decltype(code.addControlPoint)>(a + 0xB9);
				code.drawForUser = CallTarget<decltype(code.drawForUser)>(a + 0x14B);
				code.setControlPoint = CallTarget<decltype(code.setControlPoint)>(a + 0x25E);
				code.stopEmission = CallTarget<decltype(code.stopEmission)>(a - 0x175);
				code.references = *(const int32_t *)(a + 0x37);
				code.found = code.references > 0 && code.references < 0x1000;
			}
			g_LaserCode = code;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
		Diagnostics::Log("left gun: the game's laser sight %s", g_LaserCode.found ? "found" : "NOT understood, the left hand's gun shows none");
		return g_LaserCode;
	}

	// As the game links a reference to an effect, and unlinks it
	void LinkReference(ParticleRef &ref, void *effect, int references)
	{
		ParticleRef **list = (ParticleRef **)((uint8_t *)effect + references);
		ref.effect = effect;
		ref.next = *list;
		if (*list)
			(*list)->prev = &ref;
		ref.prev = nullptr;
		*list = &ref;
	}

	void UnlinkReference(ParticleRef &ref, int references)
	{
		if (ref.effect)
		{
			ParticleRef **list = (ParticleRef **)((uint8_t *)ref.effect + references);
			if (ref.prev)
			{
				ref.prev->next = ref.next;
				if (ref.next)
					ref.next->prev = ref.prev;
			}
			else if (*list == &ref)
			{
				*list = ref.next;
				if (ref.next)
					ref.next->prev = nullptr;
			}
		}
		ref.next = ref.prev = nullptr;
		ref.effect = nullptr;
	}

	// The laser goes through the player holding it
	class SkipOneFilter : public CTraceFilter
	{
	public:
		SkipOneFilter(IHandleEntity *skip) : CTraceFilter(skip, 0) {}
		bool ShouldHitEntity(IHandleEntity *entity, int contentsMask) override { return entity != m_pPassEnt; }
	};
}

// Every frame (UpdateDualWield): the left hand's gun shows its laser sight, as the game shows the right hand's, from its
// flashlight along its barrel to what it hits (PlaceLeftGunParts)
void VR::UpdateLeftLaser()
{
	const LaserCode &code = ReadLaserCode(m_Game);
	if (!code.found)
		return;
	void *local = m_Game->GetClientEntity(m_Game->m_EngineClient->GetLocalPlayer());
	void *prop = local ? (uint8_t *)local + code.particleProp : nullptr;
	const unsigned long long now = GetTickCount64();
	const void *weapon = LeftHandShowsItem() && LeftGun::Find(m_LeftItemId) ? m_Game->GetClientEntity(m_LeftItemIndex) : nullptr;
	const bool test = now < m_LeftLaserTestUntil;
	const bool show = prop && (test || (weapon && now - m_LeftGunLaserMs < 250 && WeaponHasLaser(weapon)));

	if (!show)
	{
		if (g_LeftLaser.effect)
		{
			if (prop)
				code.stopEmission(prop, g_LeftLaser.effect);
			UnlinkReference(g_LeftLaser, code.references);
			Diagnostics::Log("left gun: its laser sight off");
		}
		return;
	}

	if (!g_LeftLaser.effect)
	{
		void *effect = code.create(prop, code.name, 5, -1, 0.0f, 0.0f, 0.0f, nullptr);
		if (!effect)
			return;
		LinkReference(g_LeftLaser, effect, code.references);
		for (int point = 1; point <= 3; ++point)
			code.addControlPoint(prop, effect, point, local, 2, 0, 0.0f, 0.0f, 0.0f, nullptr);
		code.drawForUser(effect, 0);
		Diagnostics::Log("left gun: its laser sight on");
	}

	Vector start, direction;
	if (test)
	{
		start = m_SetupOrigin + Vector(0.0f, 0.0f, 10.0f);
		direction = Vector(1.0f, 0.0f, 0.0f);
	}
	else
	{
		start = ToVector(m_LeftGunLaser);
		direction = ToVector(Normalized(m_LeftGunLaserDir));
	}
	CGameTrace trace;
	Ray_t ray;
	ray.Init(start, start + direction * kLaserReach);
	SkipOneFilter filter((IHandleEntity *)local);
	m_Game->m_EngineTrace->TraceRay(ray, MASK_SHOT, &filter, &trace);
	const Vector end = trace.fraction < 1.0f ? trace.endpos : start + direction * kLaserReach;
	const Vector along = start + direction * kLaserDirectionPoint;
	code.setControlPoint(g_LeftLaser.effect, 1, &start);
	code.setControlPoint(g_LeftLaser.effect, 3, &along);
	code.setControlPoint(g_LeftLaser.effect, 2, &end);
}

// The magazine goes in: the server loads the gun from the ammo carried (LeftGun::ServerReload)
void VR::LoadLeftGun(const LeftGun::GunData &gun)
{
	const void *weapon = m_Game->GetClientEntity(m_LeftItemIndex);
	const int ammoType = weapon ? Read<int>(weapon, kClientAmmoTypeOffset) : -1;
	char command[64];
	snprintf(command, sizeof(command), "l4d2vr_left_reload %d", ammoType);
	m_Game->m_EngineClient->ServerCmd(command, true);
	if (gun.loadSound)
	{
		snprintf(command, sizeof(command), "playgamesound %s", gun.loadSound);
		m_Game->ClientCmd_Unrestricted(command);
	}
	m_Haptics.Play(Haptics::Hand::BothHands, HapticEffects::Click);
	Diagnostics::Log("left gun: loaded by the right hand");
}

// Ammo taken in the world (TryGrabWorld): the left hand's gun's ammo is full too, whatever the game gave the right hand's
void VR::GiveLeftGunAmmo()
{
	const LeftGun::GunData *gun = m_LeftItemIndex ? LeftGun::Find(m_LeftItemId) : nullptr;
	const void *weapon = gun && gun->reserveMax > 0 ? m_Game->GetClientEntity(m_LeftItemIndex) : nullptr;
	if (!weapon)
		return;
	char command[48];
	snprintf(command, sizeof(command), "l4d2vr_left_ammo %d", Read<int>(weapon, kClientAmmoTypeOffset));
	m_Game->m_EngineClient->ServerCmd(command, true);
}

// Where the left hand's gun shoots from and towards: from its muzzle (the model's "muzzle_flash", as it was last drawn)
// along its barrel (GetLeftItemDraw puts the weapon bone, barrel along z, in the fist), or the controller's when the
// hand isn't drawn
bool VR::GetLeftGunAim(Vector &origin, QAngle &angles) const
{
	Mat34 grip;
	if (GetLeftItemFrame(grip))
	{
		const Vec3 barrel = Normalized(Axis(grip, 2)), up = Normalized(Axis(grip, 1));
		const bool muzzle = GetTickCount64() - m_LeftGunMuzzleMs < 250;
		origin = ToVector(muzzle ? m_LeftGunMuzzle : Origin(grip) + up * kMuzzleUp + barrel * kMuzzleForward);
		QAngle::VectorAngles(ToVector(barrel), ToVector(up), angles);
		return true;
	}
	if (m_LeftControllerDeviceIndex == vr::k_unTrackedDeviceIndexInvalid)
		return false;
	origin = m_LeftControllerPosAbs;
	QAngle::VectorAngles(m_LeftControllerForward, m_LeftControllerUp, angles);
	return true;
}

// The left hand's gun as it's drawn (GetLeftItemDraw): a shot throws its muzzle up and back for a moment, and its bolt
// goes back and forward again (not on a pump or bolt action, worked by hand). The aim isn't moved.
void VR::ApplyLeftGunShot(Mat34 &frame)
{
	const LeftGun::GunData *gun = LeftGun::Find(m_LeftItemId);
	const unsigned long long since = GetTickCount64() - m_LeftGunShotMs;
	m_LeftGunDrawnPull = m_LeftGunBoltPull;
	if (!gun || gun->weaponId != m_LeftGunShotId)
		return;
	if (since < kRecoilMs)
	{
		const float left = 1.0f - (float)since / (float)kRecoilMs;
		const float kick = left * left;
		const float radians = gun->recoilDegrees * kick * 3.14159265f / 180.0f;
		const Vec3 barrel = Normalized(Axis(frame, 2));
		const Mat34 turn = AxisAngle(Axis(frame, 0), -radians);
		for (int axis = 0; axis < 3; ++axis)
			SetAxis(frame, axis, RotateVector(turn, Axis(frame, axis)));
		SetAxis(frame, 3, Origin(frame) - barrel * (gun->recoilDegrees * kick * kRecoilBackPerDegree));
	}
	if (!gun->workedByHand && since < kBlowbackMs)
		m_LeftGunDrawnPull = std::max(m_LeftGunDrawnPull, kBlowbackTravel * sinf(3.14159265f * (float)since / (float)kBlowbackMs));
}

bool VR::IsLeftGunShot(int seed) const
{
	const unsigned long long now = GetTickCount64();
	for (int i = 0; i < 8; ++i)
	{
		if (m_LeftGunSeeds[i] == seed && seed != 0 && now - m_LeftGunSeedMs[i] < 2000)
			return true;
	}
	return false;
}

// Every frame (UpdateDualWield): the left trigger fires the left hand's gun, one shot per pull, as long as it's held, or
// three at a time, as the gun does
void VR::UpdateLeftGun(unsigned long long now)
{
	const LeftGun::GunData *gun = LeftHandHoldsItem() ? LeftGun::Find(m_LeftItemId) : nullptr;
	// Held with both hands while the right grab is
	if (m_RightHoldsLeftGun && (!gun || !gun->twoHanded || !m_RightHandEmpty || (!PressedDigitalAction(m_RightHandGrab) && !m_RightHoldsLeftGunTest)))
	{
		m_RightHoldsLeftGun = false;
		m_RightHoldsLeftGunTest = false;
		Diagnostics::Log("left gun: let go by the right hand");
	}
	if (m_RightHoldsLeftGun)
		m_RightHandHandledLeftGunMs = now;
	// The test's magazine goes in at once
	if (m_RightMagazineTest)
	{
		m_RightMagazineTest = false;
		if (gun)
			LoadLeftGun(*gun);
	}
	const bool trigger = gun && PressedDigitalAction(m_ActionSecondaryAttack) && !m_ScopeZoomTookTrigger;
	const bool pressed = trigger && !m_LeftGunTriggerDown;
	m_LeftGunTriggerDown = trigger;
	if (!gun)
	{
		m_LeftGunBurstLeft = 0;
		m_LeftGunPull = false;
		return;
	}

	// The magazine as the game says, less the shots it hasn't counted yet
	const void *weapon = m_Game->GetClientEntity(m_LeftItemIndex);
	const int clip = weapon ? Read<int>(weapon, kClientClipOffset) : 0;
	if (m_LeftGunClipIndex != m_LeftItemIndex || clip > m_LeftGunClipSeen)
		m_LeftGunClip = clip;
	else
		m_LeftGunClip = std::min(m_LeftGunClip, clip);
	m_LeftGunClipSeen = clip;
	m_LeftGunClipIndex = m_LeftItemIndex;

	if (pressed)
		m_LeftGunPull = true;

	// The reload by hand: empty, the bolt or the pump to work, the magazine out
	if (m_ManualReloadEnabled && m_LeftGunBlocked)
	{
		m_LeftGunBurstLeft = 0;
		m_LeftGunPull = false;
		if (pressed)
			m_Game->ClientCmd_Unrestricted(gun->pistolClick ? "playgamesound Default.ClipEmpty_Pistol" : "playgamesound Default.ClipEmpty_Rifle");
		return;
	}
	bool wantsShot = false;
	switch (gun->mode)
	{
	case LeftGun::Semi:
		wantsShot = m_LeftGunPull;
		break;
	case LeftGun::Auto:
		wantsShot = trigger;
		break;
	case LeftGun::Burst:
		if (trigger && m_LeftGunBurstLeft == 0 && now >= m_LeftGunNextShotMs)
			m_LeftGunBurstLeft = kBurstShots;
		wantsShot = m_LeftGunBurstLeft > 0;
		break;
	}
	if (!wantsShot || now < m_LeftGunNextShotMs)
		return;
	m_LeftGunPull = false;

	if (m_LeftGunClip <= 0)
	{
		m_LeftGunBurstLeft = 0;
		if (pressed)
			m_Game->ClientCmd_Unrestricted(gun->pistolClick ? "playgamesound Default.ClipEmpty_Pistol" : "playgamesound Default.ClipEmpty_Rifle");
		return;
	}

	Vector origin;
	QAngle angles;
	if (!GetLeftGunAim(origin, angles))
		return;
	FireLeftGun(*gun, origin, angles);
	--m_LeftGunClip;
	m_LeftGunNextShotMs = now + (unsigned long long)(gun->cycleSeconds * 1000.0f);
	if (gun->mode == LeftGun::Burst && --m_LeftGunBurstLeft == 0)
		m_LeftGunNextShotMs = now + kBurstPauseMs;
}

// One shot of the left hand's gun: heard and seen here, its bullets shot by the server (LeftGun::ServerFire), which
// shows it to the other players
void VR::FireLeftGun(const LeftGun::GunData &gun, const Vector &origin, const QAngle &angles)
{
	// Never 0, and different from the last ones: the server's copy of the shot is known by it
	static unsigned s_Counter = 0;
	const int seed = (int)(((GetTickCount64() * 2654435761ull) ^ (++s_Counter * 40503u)) & 0x7FFFFFFF) | 1;
	m_LeftGunSeeds[m_LeftGunSeedNext] = seed;
	m_LeftGunSeedMs[m_LeftGunSeedNext] = GetTickCount64();
	m_LeftGunSeedNext = (m_LeftGunSeedNext + 1) % 8;
	char command[96];
	snprintf(command, sizeof(command), "playgamesound %s", gun.shotSound);
	m_Game->ClientCmd_Unrestricted(command);
	Vector forward;
	QAngle::AngleVectors(angles, &forward, nullptr, nullptr);
	Hooks::ClientParticle(gun.muzzleFlash, origin, origin + forward * kMuzzleFlashLength);
	// The empty case out of the ejection port, the way its attachment points (to the right)
	if (!gun.workedByHand && GetTickCount64() - m_LeftGunShellMs < 250)
		Hooks::ClientParticle(gun.brass, ToVector(m_LeftGunShell), ToVector(m_LeftGunShell + m_LeftGunShellDir * kMuzzleFlashLength));
	Hooks::FireClientBullets(m_Game->m_EngineClient->GetLocalPlayer(), origin, angles, gun.weaponId, seed, gun.spread);
	m_LeftGunShotMs = GetTickCount64();
	m_LeftGunShotId = gun.weaponId;

	snprintf(command, sizeof(command), "l4d2vr_left_fire %.1f %.1f %.1f %.2f %.2f %.2f %d", origin.x, origin.y, origin.z, angles.x, angles.y, angles.z, seed);
	m_Game->m_EngineClient->ServerCmd(command, false);
	m_Haptics.Play(Haptics::Hand::OtherHand, kShotHaptic);
}

// ---------------------------------------------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------------------------------------------

namespace
{
	// The player's left hand's gun, null if it holds none
	void *ServerLeftGun(edict_t *edict, int playerIndex, void *&player, const LeftGun::GunData *&data)
	{
		data = nullptr;
		player = nullptr;
		if (!g_Game || !g_Game->IsValidPlayerIndex(playerIndex) || !edict || !edict->m_pUnk)
			return nullptr;
		player = edict->m_pUnk->GetBaseEntity();
		const Player &vrPlayer = g_Game->m_PlayersVRInfo[playerIndex];
		void *gun = vrPlayer.leftItemIndex ? DualWield::ServerPlayersWeapon(player, vrPlayer.leftItemIndex) : nullptr;
		data = gun ? LeftGun::Find(((Server_WeaponCSBase *)gun)->GetWeaponID()) : nullptr;
		return data ? gun : nullptr;
	}
}

void LeftGun::ServerReload(edict_t *edict, int playerIndex, int ammoType)
{
	void *player;
	const GunData *data;
	void *gun = ServerLeftGun(edict, playerIndex, player, data);
	const int capacity = data ? VR::GetReloadCapacity(data->weaponId) : 0;
	if (!gun || capacity <= 0)
		return;
	int &clip = *(int *)((uint8_t *)gun + kServerClipOffset);
	if (clip < 0 || clip >= capacity)
		return;

	if (VR::ReloadHasUnlimitedReserve(data->weaponId))
	{
		clip = capacity;
	}
	else
	{
		if (ammoType < 0 || ammoType >= 32)
			return;
		int &reserve = *(int *)((uint8_t *)player + kServerAmmoArrayOffset + ammoType * 4);
		const int taken = std::min(data->shells ? 1 : capacity - clip, reserve);
		if (taken <= 0)
			return;
		clip += taken;
		reserve -= taken;
		DualWield::ServerNetworkChanged(player);
	}
	DualWield::ServerNetworkChanged(gun);
	Diagnostics::Log("left gun: player %d loaded weapon %d, %d in it", playerIndex, data->weaponId, clip);
}

void LeftGun::ServerAmmo(edict_t *edict, int playerIndex, int ammoType)
{
	void *player;
	const GunData *data;
	if (!ServerLeftGun(edict, playerIndex, player, data) || data->reserveMax <= 0 || ammoType < 0 || ammoType >= 32)
		return;
	int &reserve = *(int *)((uint8_t *)player + kServerAmmoArrayOffset + ammoType * 4);
	if (reserve >= data->reserveMax)
		return;
	reserve = data->reserveMax;
	DualWield::ServerNetworkChanged(player);
	Diagnostics::Log("left gun: player %d took ammo for weapon %d", playerIndex, data->weaponId);
}

void LeftGun::ServerFire(edict_t *edict, int playerIndex, const Vector &origin, const QAngle &angles, int seed)
{
	if (!g_Game || !g_Game->IsValidPlayerIndex(playerIndex) || !edict || !edict->m_pUnk)
		return;
	Player &vrPlayer = g_Game->m_PlayersVRInfo[playerIndex];
	void *player = edict->m_pUnk->GetBaseEntity();
	void *gun = vrPlayer.leftItemIndex ? DualWield::ServerPlayersWeapon(player, vrPlayer.leftItemIndex) : nullptr;
	const GunData *data = gun ? Find(((Server_WeaponCSBase *)gun)->GetWeaponID()) : nullptr;
	if (!data)
		return;

	const unsigned long long now = GetTickCount64();
	if (now - vrPlayer.leftFireMs < (unsigned long long)(data->cycleSeconds * 1000.0f * kServerCycleShare))
		return;
	Vector eye;
	Hooks::hkEyePosition.fOriginal(player, &eye);
	if (!std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(origin.z) || VectorLength(origin - eye) > kMaxShotFromEyes)
		return;

	int &clip = *(int *)((uint8_t *)gun + kServerClipOffset);
	if (clip <= 0 || clip > 1000)
		return;

	// The lag compensation of the shot uses the player's usercmd, which is only there while they're run: the last one
	// received stands for it (its tick, for where the others were)
	const uintptr_t commandOffsetAt = g_Game->m_Offsets->ServerCurrentCommand.address;
	const int commandOffset = commandOffsetAt ? *(const int *)commandOffsetAt : 0;
	if (commandOffset <= 0 || commandOffset > 0x4000 || !vrPlayer.hasLastCmd)
		return;
	void **current = (void **)((uint8_t *)player + commandOffset);
	void *const saved = *current;
	if (!saved)
		*current = vrPlayer.lastCmd;

	--clip;
	DualWield::ServerNetworkChanged(gun);
	vrPlayer.leftFireMs = now;
	Hooks::FireServerBullets(playerIndex, origin, angles, data->weaponId, seed, data->spread);
	*current = saved;
}
