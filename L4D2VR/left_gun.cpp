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
		{ C_WeaponCSBase::PISTOL, 0.15f, LeftGun::Semi, 1.5f, "Pistol.Fire", true, false },
		{ C_WeaponCSBase::MAGNUM, 0.15f, LeftGun::Semi, 1.25f, "Magnum.Fire", true, false },
		{ C_WeaponCSBase::UZI, 0.0625f, LeftGun::Auto, 1.0f, "SMG.Fire", false, true },
		{ C_WeaponCSBase::MAC10, 0.0625f, LeftGun::Auto, 1.2f, "SMG_Silenced.Fire", false, true },
		{ C_WeaponCSBase::MP5, 0.075f, LeftGun::Auto, 1.1f, "Weapon_MP5Navy.Single", false, true },
		// The pump shotguns are pumped between shots
		{ C_WeaponCSBase::PUMPSHOTGUN, 0.8f, LeftGun::Semi, 0.8f, "Shotgun.Fire", false, true },
		{ C_WeaponCSBase::SHOTGUN_CHROME, 0.8f, LeftGun::Semi, 0.8f, "Shotgun_Chrome.Fire", false, true },
		{ C_WeaponCSBase::AUTOSHOTGUN, 0.15f, LeftGun::Semi, 0.8f, "AutoShotgun.Fire", false, true },
		{ C_WeaponCSBase::SPAS, 0.15f, LeftGun::Semi, 0.75f, "AutoShotgun_Spas.Fire", false, true },
		{ C_WeaponCSBase::M16A1, 0.0875f, LeftGun::Auto, 0.4f, "Rifle.Fire", false, true },
		{ C_WeaponCSBase::SCAR, 0.07f, LeftGun::Burst, 0.35f, "Rifle_Desert.Fire", false, true },
		{ C_WeaponCSBase::AK47, 0.13f, LeftGun::Auto, 1.0f, "AK47.Fire", false, true },
		{ C_WeaponCSBase::SG552, 0.0825f, LeftGun::Auto, 0.4f, "Weapon_SG552.Single", false, true },
		{ C_WeaponCSBase::M60, 0.11f, LeftGun::Auto, 1.0f, "M60.Fire", false, true },
		{ C_WeaponCSBase::HUNTING_RIFLE, 0.25f, LeftGun::Semi, 0.1f, "HuntingRifle.Fire", false, true },
		{ C_WeaponCSBase::SNIPER_MILITARY, 0.25f, LeftGun::Semi, 0.5f, "Sniper_Military.Fire", false, true },
		// The bolt action rifles are worked between shots
		{ C_WeaponCSBase::AWP, 1.05f, LeftGun::Semi, 0.1f, "Weapon_AWP.Single", false, true },
		{ C_WeaponCSBase::SCOUT, 0.9f, LeftGun::Semi, 0.1f, "Weapon_Scout.Single", false, true },
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

	// m_iClip1 of a weapon, on the client and on the server
	const int kClientClipOffset = 0x984;
	const int kServerClipOffset = 0x1414;

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

// Where the left hand's gun shoots from and towards: along its barrel as it's drawn (GetLeftItemDraw puts the weapon bone,
// barrel along z, in the fist), or the controller's when the hand isn't drawn
bool VR::GetLeftGunAim(Vector &origin, QAngle &angles) const
{
	Mat34 grip;
	if (GetLeftItemFrame(grip))
	{
		const Vec3 barrel = Normalized(Axis(grip, 2)), up = Normalized(Axis(grip, 1));
		origin = ToVector(Origin(grip) + up * kMuzzleUp + barrel * kMuzzleForward);
		QAngle::VectorAngles(ToVector(barrel), ToVector(up), angles);
		return true;
	}
	if (m_LeftControllerDeviceIndex == vr::k_unTrackedDeviceIndexInvalid)
		return false;
	origin = m_LeftControllerPosAbs;
	QAngle::VectorAngles(m_LeftControllerForward, m_LeftControllerUp, angles);
	return true;
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
	const int seed = (int)(GetTickCount64() * 2654435761ull & 0x7FFFFFFF);
	char command[96];
	snprintf(command, sizeof(command), "playgamesound %s", gun.shotSound);
	m_Game->ClientCmd_Unrestricted(command);
	Hooks::FireClientBullets(m_Game->m_EngineClient->GetLocalPlayer(), origin, angles, gun.weaponId, seed, gun.spread);

	snprintf(command, sizeof(command), "l4d2vr_left_fire %.1f %.1f %.1f %.2f %.2f %.2f %d", origin.x, origin.y, origin.z, angles.x, angles.y, angles.z, seed);
	m_Game->m_EngineClient->ServerCmd(command, false);
	m_Haptics.Play(Haptics::Hand::OtherHand, kShotHaptic);
}

// ---------------------------------------------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------------------------------------------

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
