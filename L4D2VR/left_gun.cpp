#include "left_gun.h"
#include "vr.h"
#include <Windows.h>
#include <algorithm>
#include <cstdio>
#include "sdk.h"
#include "sdk_server.h"
#include "game.h"
#include "hooks.h"
#include "dual_wield.h"
#include "diagnostics.h"
#include "reload_logic.h"
#include "pose_relay.h"

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
		{ C_WeaponCSBase::PISTOL, 0.15f, LeftGun::Semi, 1.5f, "Pistol.Fire", true, false, false, 0, "Pistol.ClipIn", "weapon_muzzle_flash_pistol" },
		{ C_WeaponCSBase::MAGNUM, 0.15f, LeftGun::Semi, 1.25f, "Magnum.Fire", true, false, false, 0, "Magnum.ClipIn", "weapon_muzzle_flash_autoshotgun" },
		{ C_WeaponCSBase::UZI, 0.0625f, LeftGun::Auto, 1.0f, "SMG.Fire", false, true, false, 650, "SMG.ClipIn", "weapon_muzzle_flash_smg" },
		{ C_WeaponCSBase::MAC10, 0.0625f, LeftGun::Auto, 1.2f, "SMG_Silenced.Fire", false, true, false, 650, "SMG_Silenced.ClipIn", "weapon_muzzle_flash_smg_silenced" },
		{ C_WeaponCSBase::MP5, 0.075f, LeftGun::Auto, 1.1f, "Weapon_MP5Navy.Single", false, true, false, 650, "SMG.ClipIn", "weapon_muzzle_flash_smg" },
		// The pump shotguns are pumped between shots
		{ C_WeaponCSBase::PUMPSHOTGUN, 0.8f, LeftGun::Semi, 0.8f, "Shotgun.Fire", false, true, true, 56, nullptr, "weapon_muzzle_flash_autoshotgun" },
		{ C_WeaponCSBase::SHOTGUN_CHROME, 0.8f, LeftGun::Semi, 0.8f, "Shotgun_Chrome.Fire", false, true, true, 56, nullptr, "weapon_muzzle_flash_autoshotgun" },
		{ C_WeaponCSBase::AUTOSHOTGUN, 0.15f, LeftGun::Semi, 0.8f, "AutoShotgun.Fire", false, true, true, 90, nullptr, "weapon_muzzle_flash_autoshotgun" },
		{ C_WeaponCSBase::SPAS, 0.15f, LeftGun::Semi, 0.75f, "AutoShotgun_Spas.Fire", false, true, true, 90, nullptr, "weapon_muzzle_flash_autoshotgun" },
		{ C_WeaponCSBase::M16A1, 0.0875f, LeftGun::Auto, 0.4f, "Rifle.Fire", false, true, false, 360, "Rifle.ClipIn", "weapon_muzzle_flash_assaultrifle" },
		{ C_WeaponCSBase::SCAR, 0.07f, LeftGun::Burst, 0.35f, "Rifle_Desert.Fire", false, true, false, 360, "Rifle_Desert.ClipIn", "weapon_muzzle_flash_assaultrifle" },
		{ C_WeaponCSBase::AK47, 0.13f, LeftGun::Auto, 1.0f, "AK47.Fire", false, true, false, 360, "AK47.ClipIn", "weapon_muzzle_flash_smg" },
		{ C_WeaponCSBase::SG552, 0.0825f, LeftGun::Auto, 0.4f, "Weapon_SG552.Single", false, true, false, 360, "Rifle.ClipIn", "weapon_muzzle_flash_assaultrifle" },
		// The M60 isn't reloaded, as in the game
		{ C_WeaponCSBase::M60, 0.11f, LeftGun::Auto, 1.0f, "M60.Fire", false, true, false, 0, nullptr, "weapon_muzzle_flash_assaultrifle" },
		{ C_WeaponCSBase::HUNTING_RIFLE, 0.25f, LeftGun::Semi, 0.1f, "HuntingRifle.Fire", false, true, false, 150, "HuntingRifle.ClipIn", "weapon_muzzle_flash_huntingrifle" },
		{ C_WeaponCSBase::SNIPER_MILITARY, 0.25f, LeftGun::Semi, 0.5f, "Sniper_Military.Fire", false, true, false, 180, "Sniper_Military.ClipIn", "weapon_muzzle_flash_huntingrifle" },
		// The bolt action rifles are worked between shots
		{ C_WeaponCSBase::AWP, 1.05f, LeftGun::Semi, 0.1f, "Weapon_AWP.Single", false, true, false, 180, "HuntingRifle.ClipIn", "weapon_muzzle_flash_huntingrifle" },
		{ C_WeaponCSBase::SCOUT, 0.9f, LeftGun::Semi, 0.1f, "Weapon_Scout.Single", false, true, false, 180, "HuntingRifle.ClipIn", "weapon_muzzle_flash_huntingrifle" },
	};

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

// ---------------------------------------------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------------------------------------------

// The frame the left hand's item is drawn in: its fist (y along the grip, z where the fingers point), turned to point at
// the right hand while that one holds the gun too
bool VR::GetLeftItemFrame(Mat34 &frame) const
{
	if (!GetLeftGripFrame(frame))
		return false;
	if (!m_RightHoldsLeftGun)
		return true;

	const Vec3 toRight = GetFistPoint(true) - Origin(frame);
	if (Length(toRight) < kSupportMinSpan)
		return true;
	const Vec3 forward = Normalized(toRight);
	const Vec3 grip = Axis(frame, 1);
	const Vec3 up = Normalized(grip - forward * Dot(grip, forward), Cross(forward, Axis(frame, 0)));
	frame = FromAxes(Cross(up, forward), up, forward, Origin(frame));
	return true;
}

// The empty right hand takes the left hand's long gun with a grab on its barrel (UpdateTracking), and holds it while the
// grab is held: the gun points from the left hand towards it. True if it took it.
bool VR::TryRightHoldLeftGun()
{
	const LeftGun::GunData *gun = LeftHandHoldsItem() ? LeftGun::Find(m_LeftItemId) : nullptr;
	Mat34 grip;
	if (!m_RightHandEmpty || !gun || !gun->twoHanded || !GetLeftGripFrame(grip))
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
	in.magazineBase = m_LeftGunWell - up * (spec.magazineLength * 0.5f + 0.5f);
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
	if (events & Reload::Event_BoltReleased)
		PlayWeaponSound(spec.boltForwardSound);
	if (events & (Reload::Event_GrabbedOld | Reload::Event_MagazineOut | Reload::Event_DroppedOld | Reload::Event_GrabbedFresh | Reload::Event_BoltPulled))
		Diagnostics::Log("left gun: reload by the right hand, events 0x%X", events);
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
	Hooks::FireClientBullets(m_Game->m_EngineClient->GetLocalPlayer(), origin, angles, gun.weaponId, seed, gun.spread);

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
