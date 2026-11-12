#include "dual_wield.h"
#include "vr.h"
#include <Windows.h>
#include <d3d9.h>
#include <algorithm>
#include <cstdio>
#include "sdk.h"
#include "sdk_server.h"
#include "game.h"
#include "hooks.h"
#include "offsets.h"
#include "pose_relay.h"
#include "studio.h"
#include "diagnostics.h"

using namespace HandMath;

// ---------------------------------------------------------------------------------------------------------------
// Dual wield, see dual_wield.h.
//
// The left hand takes an item:
// - grabbing the chest where the grenade or the pills are (body_items.cpp), the back where the kit is, or behind the left
//   shoulder where its weapon is, and puts it back the same way,
// - grabbing one in the world (TryGrabWorld): the server is told it's for the left hand. A weapon taken that way takes
//   the place of the one it had, which goes to the ground.
// - holding the hands together with the item in the right hand, and the left grab for 2 seconds: the right hand is left
//   empty. The right grab does the opposite.
// While the left hand works the weapon (magazine, shell, bolt, two handed grip) its item is put away for a moment.
// Its trigger uses a grenade or a kit: they become the active weapon in the left hand until used.
// ---------------------------------------------------------------------------------------------------------------

namespace
{
	const unsigned long long kLeftGrabWindowMs = 1500;    // from the left grab in the world to the item in the inventory
	const unsigned long long kSwitchBackIntervalMs = 400;
	const unsigned long long kLeftUseGiveUpMs = 1500;     // the trigger let go without using the item
	const unsigned long long kLeftUseMaxMs = 8000;
	const unsigned long long kLeftHandSendIntervalMs = 20;
	const unsigned long long kLeftItemResendMs = 3000;    // players who joined later learn it too
	const float kHandSwapDistance = 0.12f;                // meters between the controllers
	const float kHandSwapSeconds = 2.0f;
	const unsigned long long kHandSwapPulseMs = 120;
	// Fingers closed around what the left hand holds
	const float kHoldMeleeCurl = 0.85f;
	const float kHoldItemCurl = 0.7f;
	const float kHoldThumbCurl = 0.6f;
	// From the knuckles to the middle of the fist, meters
	const float kGripPalmOffset = 0.022f;
	const float kKnifeLeanDegrees = 20.0f;
	// A thumbstick click shorter than this is a click; held this long it drops what the hand holds
	const unsigned long long kStickTapMs = 500;
	const unsigned long long kStickDropMs = 3000;
	// The game changes the weapon this long after the mod asked it to (SelectWeapon) while the right hand is empty
	const unsigned long long kEmptyHandSelectMs = 1500;
	// Hooks::TraceMeleeSwing turns the aim by this, degrees
	const float kServerMeleeTraceTilt = 50.0f;

	unsigned long long NowMs() { return GetTickCount64(); }

	int g_ClientExtraWeapon = 0;
}

bool DualWield::UsesExtraSlot(int weaponId)
{
	switch (weaponId)
	{
	case C_WeaponCSBase::PISTOL:
	case C_WeaponCSBase::MAGNUM:
	case C_WeaponCSBase::MELEE:
	case C_WeaponCSBase::CHAINSAW:
	case C_WeaponCSBase::UZI:
	case C_WeaponCSBase::MAC10:
	case C_WeaponCSBase::MP5:
	case C_WeaponCSBase::PUMPSHOTGUN:
	case C_WeaponCSBase::SHOTGUN_CHROME:
	case C_WeaponCSBase::AUTOSHOTGUN:
	case C_WeaponCSBase::SPAS:
	case C_WeaponCSBase::M16A1:
	case C_WeaponCSBase::SCAR:
	case C_WeaponCSBase::AK47:
	case C_WeaponCSBase::SG552:
	case C_WeaponCSBase::M60:
	case C_WeaponCSBase::HUNTING_RIFLE:
	case C_WeaponCSBase::SNIPER_MILITARY:
	case C_WeaponCSBase::AWP:
	case C_WeaponCSBase::SCOUT:
	case C_WeaponCSBase::GRENADE_LAUNCHER:
		return true;
	default:
		return false;
	}
}

bool DualWield::IsLeftUseItem(int weaponId)
{
	switch (weaponId)
	{
	case C_WeaponCSBase::MOLOTOV:
	case C_WeaponCSBase::PIPE_BOMB:
	case C_WeaponCSBase::VOMITJAR:
	case C_WeaponCSBase::FIRST_AID_KIT:
	case C_WeaponCSBase::DEFIBRILLATOR:
	case C_WeaponCSBase::INCENDIARY_AMMO:
	case C_WeaponCSBase::FRAG_AMMO:
		return true;
	default:
		return false;
	}
}

bool DualWield::IsLeftHandItem(int weaponId)
{
	return UsesExtraSlot(weaponId) || IsLeftUseItem(weaponId) || weaponId == C_WeaponCSBase::PAIN_PILLS || weaponId == C_WeaponCSBase::ADRENALINE;
}

// ---------------------------------------------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------------------------------------------

void DualWield::ClientSetExtraWeapon(int entityIndex)
{
	g_ClientExtraWeapon = entityIndex;
}

bool DualWield::ClientGetSlot(void *weapon, int &slot)
{
	if (!g_ClientExtraWeapon || !weapon || !g_Game || !g_Game->m_ClientEntityList)
		return false;
	if ((void *)g_Game->GetClientEntity(g_ClientExtraWeapon) != weapon)
		return false;
	slot = kExtraSlot;
	return true;
}

bool VR::LeftHandHoldsItem() const
{
	return m_LeftItemIndex != 0 && !m_LeftUseActive && !isHoldingWeapon2Handed && !ManualReloadHandIsBusy();
}

bool VR::LeftHandHoldsMelee() const
{
	return LeftHandHoldsItem() && m_LeftItemId == C_WeaponCSBase::MELEE;
}

// The left hand is drawn holding its item: while it holds it, and while it's used, until a grenade leaves the hand
bool VR::LeftHandShowsItem() const
{
	if (m_LeftUseActive)
		return m_LeftItemIndex != 0 && !ThrownGrenadeHasLeftTheHand();
	return LeftHandHoldsItem();
}

// The game takes out that weapon entity (DualWield::ServerSelectWeapon): of two pistols, the one meant, where "use
// weapon_pistol" would take the first one
void VR::SelectWeapon(int entityIndex)
{
	if (!entityIndex)
		return;
	// The right hand stays empty with the weapon the mod takes out for it (UpdateEmptyHand)
	if (m_RightHandEmpty)
		m_EmptyHandSelectMs = NowMs();
	char command[48];
	snprintf(command, sizeof(command), "l4d2vr_select %d", entityIndex);
	m_Game->m_EngineClient->ServerCmd(command, true);
}

void VR::TakeIntoLeftHand(int entityIndex, int weaponId)
{
	if (m_LeftItemIndex == entityIndex)
		return;
	m_LeftItemIndex = entityIndex;
	m_LeftItemId = weaponId;
	m_LeftGripValid = false;
	if (DualWield::UsesExtraSlot(weaponId))
	{
		// One weapon for the left hand: the one it had, in it or behind the shoulder, goes to the ground. Otherwise it
		// would lose the slot of its own and take the place of one of the right hand's.
		if (m_ExtraWeaponIndex && m_ExtraWeaponIndex != entityIndex)
			SendDrop(m_ExtraWeaponIndex);
		m_ExtraWeaponIndex = entityIndex;
	}
	Diagnostics::Log("dual wield: entity %d (weapon %d) in the left hand", entityIndex, weaponId);
}

void VR::ClearLeftItem()
{
	if (m_LeftItemIndex)
		Diagnostics::Log("dual wield: the left hand is empty");
	m_LeftItemIndex = 0;
	m_LeftItemId = 0;
	m_LeftUseActive = false;
}

// A grab with the left hand at the chest: the grenade or the pills seen there go to the left hand, or the one in the
// hand goes back. True if it did something.
bool VR::LeftHandChestGrab(const Vec3 &handWorld)
{
	if (!m_DualWieldEnabled)
		return false;

	C_BasePlayer *local = (C_BasePlayer *)m_Game->GetClientEntity(m_Game->m_EngineClient->GetLocalPlayer());
	if (!local)
		return false;

	// Putting it back where it was taken
	if (m_LeftItemIndex && !DualWield::UsesExtraSlot(m_LeftItemId))
	{
		const int spot = GetChestSpotAt(handWorld);
		const bool pills = m_LeftItemId == C_WeaponCSBase::PAIN_PILLS || m_LeftItemId == C_WeaponCSBase::ADRENALINE;
		if (spot >= 0 && (spot == 0) == pills)
		{
			ClearLeftItem();
			m_Haptics.Play(Haptics::Hand::OtherHand, HapticEffects::Confirm);
			return true;
		}
	}

	const int chestSlot = GetChestItemSlotAt(handWorld);
	if (!chestSlot)
		return false;

	// A weapon in the hand has nowhere to go here
	if (m_LeftItemIndex && DualWield::UsesExtraSlot(m_LeftItemId))
	{
		m_Haptics.Play(Haptics::Hand::OtherHand, HapticEffects::Cancel);
		return true;
	}

	InventoryItem items[64];
	const int count = GetInventory(local, items, 64);
	for (int i = 0; i < count; ++i)
	{
		const bool pills = items[i].id == C_WeaponCSBase::PAIN_PILLS || items[i].id == C_WeaponCSBase::ADRENALINE;
		const bool grenade = IsThrowableWeapon(items[i].id);
		if ((chestSlot == 5 && pills) || (chestSlot == 3 && grenade))
		{
			TakeIntoLeftHand(items[i].index, items[i].id);
			m_Haptics.Play(Haptics::Hand::OtherHand, HapticEffects::Take);
			return true;
		}
	}
	return false;
}

// A grab with the left hand behind the left shoulder: the weapon in it goes there, or the one there comes to it. True if
// it did something.
bool VR::LeftShoulderGrab()
{
	if (!m_DualWieldEnabled)
		return false;

	if (m_LeftItemIndex)
	{
		// Items have places of their own
		if (m_LeftItemIndex != m_ExtraWeaponIndex)
			return false;
		ClearLeftItem();
		m_Haptics.Play(Haptics::Hand::OtherHand, HapticEffects::Confirm);
		return true;
	}

	// The weapon of the extra slot, unless the right hand holds it
	C_BasePlayer *local = (C_BasePlayer *)m_Game->GetClientEntity(m_Game->m_EngineClient->GetLocalPlayer());
	if (!local || !m_ExtraWeaponIndex)
		return false;
	InventoryItem items[64];
	const int count = GetInventory(local, items, 64);
	const void *active = local->GetActiveWeapon();
	for (int i = 0; i < count; ++i)
	{
		if (items[i].index != m_ExtraWeaponIndex || (items[i].weapon == active && !m_RightHandEmpty))
			continue;
		TakeIntoLeftHand(items[i].index, items[i].id);
		m_Haptics.Play(Haptics::Hand::OtherHand, HapticEffects::Take);
		return true;
	}
	return false;
}

// A grab with the left hand on the back, where the kit is carried: the kit, defibrillator or upgrade pack there goes to
// the left hand, or the one in it goes back. True if it did something.
bool VR::LeftHandKitGrab()
{
	if (!m_DualWieldEnabled)
		return false;

	if (m_LeftItemIndex)
	{
		if (GetWeaponSlot(m_LeftItemId) != 4)
			return false;
		ClearLeftItem();
		m_Haptics.Play(Haptics::Hand::OtherHand, HapticEffects::Confirm);
		return true;
	}

	C_BasePlayer *local = (C_BasePlayer *)m_Game->GetClientEntity(m_Game->m_EngineClient->GetLocalPlayer());
	if (!local)
		return false;
	InventoryItem items[64];
	const int count = GetInventory(local, items, 64);
	const void *active = local->GetActiveWeapon();
	for (int i = 0; i < count; ++i)
	{
		if (GetWeaponSlot(items[i].id) != 4 || (items[i].weapon == active && !m_RightHandEmpty))
			continue;
		TakeIntoLeftHand(items[i].index, items[i].id);
		m_Haptics.Play(Haptics::Hand::OtherHand, HapticEffects::Take);
		return true;
	}
	return false;
}

// What the left hand holds is drawn in it: the item's own model, with its "weapon_bone" in the fist like in the third
// person
bool VR::GetLeftItemDraw(BodyModelDraw &draw)
{
	if (!LeftHandShowsItem() || m_LeftHandFollow < 1.0f)
		return false;

	const void *entity = m_Game->GetClientEntity(m_LeftItemIndex);
	const void *model = entity ? GetWorldModel(entity) : nullptr;
	if (!model)
		return false;

	// A melee weapon's weapon bone has y along the blade and z towards the edge: the blade comes out of the fist by the
	// thumb, like a sword held, with the edge on the side of the fingers
	draw = BodyModelDraw();
	draw.model = model;
	draw.alignWeaponBone = true;

	// Where a gun's grip is (GetLeftItemFrame)
	if (model != m_LeftGunModelPtr || m_LeftGunModelIndex != m_LeftItemIndex)
	{
		m_LeftGunModelPtr = model;
		m_LeftGunModelIndex = m_LeftItemIndex;
		m_LeftGunModel = LeftGun::FindModel(m_Game->m_ModelInfo->GetModelName((void *)model));
	}

	// The adrenaline's model has its cap on. With the trigger held, ready to be stabbed in, it's the first person one
	// instead, which shows the needle (out by the little finger).
	if (m_LeftItemId == C_WeaponCSBase::ADRENALINE && m_LeftTriggerDown)
	{
		if (const void *syringe = GetSyringeModel())
		{
			draw.model = syringe;
			draw.alignWeaponBone = false;
			draw.syringe = true;
		}
	}
	if (!GetLeftItemFrame(draw.frame))
		return false;
	if (LeftGun::Find(m_LeftItemId))
		ApplyLeftGunShot(draw.frame);

	// The game's animation holds the knife in the right hand with its blade leaning 20 degrees towards the fingers (the
	// katana about 6, which the fist's grip already gives). Measured with l4d2vr_dual_test vmgrip.
	if (m_LeftItemId == C_WeaponCSBase::MELEE)
	{
		const char *name = m_Game->m_ModelInfo->GetModelName((void *)model);
		if (name && strstr(name, "knife"))
		{
			const float radians = kKnifeLeanDegrees * 3.14159265f / 180.0f;
			const Vec3 grip = Axis(draw.frame, 1), fingers = Axis(draw.frame, 2);
			SetAxis(draw.frame, 1, grip * cosf(radians) + fingers * sinf(radians));
			SetAxis(draw.frame, 2, fingers * cosf(radians) - grip * sinf(radians));
		}
	}
	return true;
}

// The left hand's fist in the world: y along the grip, out by the thumb, z where the fingers point (HandRig::GripFrame)
bool VR::GetLeftGripFrame(Mat34 &frame) const
{
	if (!m_LeftGripValid || !m_ArmsRig)
		return false;
	const Mat34 hand = GetLeftHandBone(*m_ArmsRig);
	if (!IsFinite(hand))
		return false;
	frame = Multiply(hand, m_LeftGripInHand);
	return true;
}

// The left hand bone where its controller is, turned by the optional fine tuning
Mat34 VR::GetLeftHandBone(const HandRig &rig) const
{
	Vec3 forward = m_LeftHandForward;
	Vec3 up = m_LeftHandUp;
	if (m_HandAngleOffset.x != 0.0f || m_HandAngleOffset.y != 0.0f || m_HandAngleOffset.z != 0.0f)
	{
		const Vec3 right = Cross(forward, up);
		const float toRadians = 3.14159265f / 180.0f;
		const Mat34 turn = Multiply(Multiply(AxisAngle(up, -m_HandAngleOffset.y * toRadians), AxisAngle(right, m_HandAngleOffset.x * toRadians)), AxisAngle(forward, m_HandAngleOffset.z * toRadians));
		forward = RotateVector(turn, forward);
		up = RotateVector(turn, up);
	}
	return rig.HandFromFrame(HandRig::Left, forward, up, m_LeftHandWrist);
}

// Called by BuildFirstPersonHands: the fingers close around the item, and where the fist is is kept for drawing it
void VR::HoldLeftItem(const HandRig &rig, float curls[HandRig::FingerCount])
{
	const float closed = m_LeftItemId == C_WeaponCSBase::MELEE ? kHoldMeleeCurl : kHoldItemCurl;
	for (int finger = 0; finger < HandRig::FingerCount; ++finger)
		curls[finger] = finger == HandRig::Thumb ? kHoldThumbCurl : closed;
}

void VR::KeepLeftGrip(const HandRig &rig, const Mat34 *bones)
{
	const Mat34 &hand = bones[rig.HandBone(HandRig::Left)];
	m_LeftGripInHand = Multiply(InvertRigid(hand), rig.GripFrame(HandRig::Left, bones, kGripPalmOffset * m_VRScale));
	m_LeftGripValid = IsFinite(m_LeftGripInHand);
}

// During the use of the left hand's item the game's weapon is in the left hand: the poses of the right controller
// are replaced with the left one's (called by UpdateTracking)
void VR::ApplyLeftUseHand(const Vector &leftForwardRaw, const Vector &leftRightRaw, const Vector &leftUpRaw)
{
	if (!m_LeftUseActive)
		return;

	// Where the right hand really is, to draw it there (GetRightHandTarget)
	m_RightRealPosAbs = m_RightControllerPosAbs;
	m_RightRealForwardRaw = m_RightControllerForwardRaw;
	m_RightRealRightRaw = m_RightControllerRightRaw;
	m_RightRealUpRaw = m_RightControllerUpRaw;

	m_RightControllerPosAbs = m_LeftControllerPosAbs;
	m_RightControllerForward = m_LeftControllerForward;
	m_RightControllerRight = m_LeftControllerRight;
	m_RightControllerUp = m_LeftControllerUp;
	m_RightControllerPosAbsRaw = m_LeftControllerPosAbs;
	m_RightControllerForwardRaw = leftForwardRaw;
	m_RightControllerRightRaw = leftRightRaw;
	m_RightControllerUpRaw = leftUpRaw;
}

// The best weapon for the right hand other than `exceptIndex` (entity index, 0 if none): `preferredIndex`, a primary
// weapon, a pistol. Not the left hand's, and not the one behind the left shoulder unless it's the preferred one.
int VR::PickRightWeapon(const InventoryItem *items, int count, int exceptIndex, int preferredIndex) const
{
	int best = 0, bestRank = 0;
	for (int i = 0; i < count; ++i)
	{
		const int index = items[i].index;
		if (index == exceptIndex || index == m_LeftItemIndex || (index == m_ExtraWeaponIndex && index != preferredIndex))
			continue;
		int rank = 0;
		if (preferredIndex && index == preferredIndex)
			rank = 4;
		else if (GetWeaponSlot(items[i].id) == 1)
			rank = 3;
		else if (GetWeaponSlot(items[i].id) == 2)
			rank = 2;
		if (rank > bestRank)
		{
			bestRank = rank;
			best = index;
		}
	}
	return best;
}

void VR::UpdateDualWield(C_BasePlayer *local, int activeWeaponId, float deltaSeconds)
{
	const unsigned long long now = NowMs();

	if (!m_DualWieldEnabled || !local || !m_Game->m_PoseRelay->IsServerCapable())
	{
		if (m_LeftItemIndex)
			ClearLeftItem();
		m_ExtraWeaponIndex = 0;
		DualWield::ClientSetExtraWeapon(0);
		return;
	}

	InventoryItem items[64];
	const int count = GetInventory(local, items, 64);
	const void *active = local->GetActiveWeapon();
	int activeIndex = 0;
	auto carried = [&](int index, int id)
	{
		for (int i = 0; i < count; ++i)
		{
			if (items[i].index == index && (id == 0 || items[i].id == id))
				return true;
		}
		return false;
	};
	for (int i = 0; i < count; ++i)
	{
		if (items[i].weapon == active)
			activeIndex = items[i].index;
	}

	if (m_ExtraWeaponIndex && !carried(m_ExtraWeaponIndex, 0))
		m_ExtraWeaponIndex = 0;
	if (m_LeftItemIndex && !carried(m_LeftItemIndex, m_LeftItemId))
		ClearLeftItem();

	// Just grabbed something in the world with the left hand: what's new in the inventory goes to it, in place of what it
	// held (a weapon taken that way took the extra slot from the one it had, which the game dropped)
	if (m_LeftGrabUntil)
	{
		for (int i = 0; i < count; ++i)
		{
			bool before = false;
			for (int j = 0; j < m_InventoryBeforeCount; ++j)
				before |= m_InventoryBefore[j] == items[i].index;
			if (!before && DualWield::IsLeftHandItem(items[i].id))
			{
				TakeIntoLeftHand(items[i].index, items[i].id);
				m_LeftGrabUntil = 0;
				break;
			}
		}
		if (now > m_LeftGrabUntil)
			m_LeftGrabUntil = 0;
	}

	// What the right hand holds, to go back to it
	if (activeIndex && activeIndex != m_LeftItemIndex && !m_RightHandEmpty && activeIndex != m_RightWeaponIndex)
	{
		m_PrevRightWeaponIndex = m_RightWeaponIndex;
		m_RightWeaponIndex = activeIndex;
	}

	UpdateLeftUse(now, activeIndex, items, count);
	UpdateLeftGun(now);

	// The left hand's item became the active weapon without being used (the game takes a weapon picked up in its hand):
	// back to the right hand's, or with the right hand empty to the one it put away, unseen (UpdateEmptyHand)
	if (m_LeftItemIndex && activeIndex == m_LeftItemIndex && !m_LeftUseActive && now - m_LastSwitchBackMs > kSwitchBackIntervalMs)
	{
		int preferred = m_RightWeaponIndex;
		if (m_RightHandEmpty)
		{
			preferred = 0;
			for (int i = 0; i < count; ++i)
			{
				if (items[i].weapon == m_EmptyHandWeapon)
					preferred = items[i].index;
			}
		}
		const int back = PickRightWeapon(items, count, m_LeftItemIndex, preferred);
		if (back)
		{
			m_LastSwitchBackMs = now;
			SelectWeapon(back);
		}
	}

	UpdateLeftSwing(now);
	UpdateHandSwap(now, deltaSeconds, activeIndex, activeWeaponId, items, count);

	DualWield::ClientSetExtraWeapon(m_ExtraWeaponIndex);

	// The server has to know, for the melee hits, the slots and the other players
	if (m_LeftItemIndex != m_SentLeftItemIndex || m_ExtraWeaponIndex != m_SentExtraWeaponIndex || now - m_LeftItemSentMs > kLeftItemResendMs)
	{
		m_SentLeftItemIndex = m_LeftItemIndex;
		m_SentExtraWeaponIndex = m_ExtraWeaponIndex;
		m_LeftItemSentMs = now;
		char command[48];
		snprintf(command, sizeof(command), "l4d2vr_left %d %d", m_LeftItemIndex, m_ExtraWeaponIndex);
		m_Game->m_EngineClient->ServerCmd(command, true);
	}
}

// The trigger of the left hand throws its grenade or uses its kit, defibrillator or upgrade pack: it's taken as the
// active weapon, and the trigger is the attack until it's used (UpdateGrenadeThrow, ProcessInput). Pills and adrenaline
// are used without that, at the mouth or against the body (VR::UpdateItemUse).
void VR::UpdateLeftUse(unsigned long long now, int activeIndex, const InventoryItem *items, int count)
{
	const bool trigger = PressedDigitalAction(m_ActionSecondaryAttack);
	const bool pressed = trigger && !m_LeftTriggerDown;
	m_LeftTriggerDown = trigger;

	if (!m_LeftUseActive)
	{
		if (pressed && LeftHandHoldsItem() && DualWield::IsLeftUseItem(m_LeftItemId) && !m_ScopeZoomTookTrigger && activeIndex != m_LeftItemIndex)
		{
			m_LeftUseActive = true;
			m_LeftUseWasActive = false;
			m_LeftUseStartMs = now;
			m_LeftUseReleaseMs = 0;
			SelectWeapon(m_LeftItemIndex);
			m_Haptics.Play(Haptics::Hand::OtherHand, HapticEffects::Confirm);
			Diagnostics::Log("dual wield: using the left hand's item (weapon %d)", m_LeftItemId);
		}
		return;
	}

	if (activeIndex && activeIndex == m_LeftItemIndex)
		m_LeftUseWasActive = true;
	if (trigger)
		m_LeftUseReleaseMs = 0;
	else if (!m_LeftUseReleaseMs)
		m_LeftUseReleaseMs = now;

	const char *reason = nullptr;
	if (!m_LeftItemIndex)
		reason = "used";
	else if (m_LeftUseWasActive && activeIndex != m_LeftItemIndex)
		reason = "put away by the game";
	else if (m_LeftUseReleaseMs && now - m_LeftUseReleaseMs > kLeftUseGiveUpMs)
		reason = "trigger let go";
	else if (now - m_LeftUseStartMs > kLeftUseMaxMs)
		reason = "too long";
	if (!reason)
		return;

	m_LeftUseActive = false;
	Diagnostics::Log("dual wield: end of the left hand's use (%s)", reason);
	if (activeIndex == 0 || activeIndex == m_LeftItemIndex || !m_LeftItemIndex)
		SelectWeapon(PickRightWeapon(items, count, m_LeftItemIndex, m_RightWeaponIndex));
}

// The left hand's melee weapon hits what it swings through, the same way as the right hand's (Hooks::dWriteUsercmd,
// dProcessUsercmds): while the hand moves faster than the swing speed its pose goes to the server, which traces the
// blade from one pose to the next. Each time the hand gets slower than that, the next swing can hit the same things
// again.
void VR::UpdateLeftSwing(unsigned long long now)
{
	if (!LeftHandHoldsMelee() || m_LeftControllerDeviceIndex == vr::k_unTrackedDeviceIndexInvalid)
	{
		m_LeftSwinging = false;
		m_LeftSwingAimValid = false;
		return;
	}

	const QAngle aim = GetLeftSwingAim();
	const bool swinging = VectorLength(m_LeftControllerPose.TrackedDeviceVel) > m_MeleeSwingVelocityRequired;
	const bool newSwing = swinging && !m_LeftSwinging;
	const QAngle from = m_LeftSwingAimValid ? m_LeftSwingPrevAim : aim;
	m_LeftSwinging = swinging;
	m_LeftSwingPrevAim = aim;
	m_LeftSwingAimValid = true;

	if (!swinging || (!newSwing && now - m_LeftHandSentMs < kLeftHandSendIntervalMs))
		return;
	m_LeftHandSentMs = now;

	char command[160];
	if (newSwing)
	{
		m_Haptics.Play(Haptics::Hand::OtherHand, HapticEffects::Hint);
		snprintf(command, sizeof(command), "l4d2vr_left_hand %.1f %.1f %.1f %.1f %.1f %.1f %d %.1f %.1f %.1f", m_LeftControllerPosAbs.x, m_LeftControllerPosAbs.y,
			m_LeftControllerPosAbs.z, aim.x, aim.y, aim.z, DualWield::kLeftHandSwinging | DualWield::kLeftHandNewSwing, from.x, from.y, from.z);
	}
	else
	{
		snprintf(command, sizeof(command), "l4d2vr_left_hand %.1f %.1f %.1f %.1f %.1f %.1f %d", m_LeftControllerPosAbs.x, m_LeftControllerPosAbs.y,
			m_LeftControllerPosAbs.z, aim.x, aim.y, aim.z, DualWield::kLeftHandSwinging);
	}
	m_Game->m_EngineClient->ServerCmd(command, false);
}

// The server traces 50 degrees up from the aim it gets (made for the right hand's weapons, held up from the aim). The
// aim sent for the left hand is the blade as it's drawn (GetLeftItemDraw) turned by as much towards its edge, with "up"
// towards the blade, so the traces go along it.
QAngle VR::GetLeftSwingAim() const
{
	Vector forward = VectorRotate(m_LeftControllerForward, m_LeftControllerRight, -kServerMeleeTraceTilt);
	Vector up = VectorRotate(m_LeftControllerUp, m_LeftControllerRight, -kServerMeleeTraceTilt);
	Mat34 grip;
	if (GetLeftGripFrame(grip))
	{
		const Vec3 blade = Normalized(Axis(grip, 1)), edge = Normalized(Axis(grip, 2));
		const float radians = kServerMeleeTraceTilt * 3.14159265f / 180.0f;
		const Vec3 aimForward = blade * cosf(radians) + edge * sinf(radians);
		const Vec3 aimUp = blade * sinf(radians) - edge * cosf(radians);
		forward = Vector(aimForward.x, aimForward.y, aimForward.z);
		up = Vector(aimUp.x, aimUp.y, aimUp.z);
	}
	QAngle aim;
	QAngle::VectorAngles(forward, up, aim);
	return aim;
}

// A thumbstick click: true for a short one, when it's let go. Held, it vibrates more and more, and at 3 seconds it drops
// what the hand holds and the click does nothing else.
bool VR::UpdateStickHold(StickHold &hold, bool down, bool weaponHand)
{
	const unsigned long long now = NowMs();
	if (down && !hold.down)
	{
		hold = StickHold();
		hold.down = true;
		hold.pressedMs = now;
		return false;
	}
	if (!down)
	{
		const bool tap = hold.down && !hold.dropped && now - hold.pressedMs < kStickTapMs;
		hold.down = false;
		return tap;
	}
	if (hold.dropped)
		return false;

	const unsigned long long held = now - hold.pressedMs;
	const Haptics::Hand hand = weaponHand ? Haptics::Hand::WeaponHand : Haptics::Hand::OtherHand;
	if (held >= kStickDropMs)
	{
		hold.dropped = true;
		DropFromHand(weaponHand);
		m_Haptics.Play(hand, HapticEffects::Click);
	}
	else if (held >= kStickTapMs && now - hold.pulseMs >= kHandSwapPulseMs)
	{
		hold.pulseMs = now;
		const float amplitude = 0.1f + 0.5f * (float)(held - kStickTapMs) / (float)(kStickDropMs - kStickTapMs);
		m_Haptics.Play(hand, { { 0.0f, 0.04f, 150.0f, amplitude } });
	}
	return false;
}

// What a hand holds goes to the ground (DualWield::ServerDropItem): the right hand's weapon, after which the game takes
// another one, or the left hand's item, which leaves the hand empty. Needs the mod on the host.
void VR::DropFromHand(bool weaponHand)
{
	if (!m_Game->m_PoseRelay->IsServerCapable())
	{
		Diagnostics::Log("dual wield: dropping needs the mod on the host");
		return;
	}

	int index = 0;
	if (!weaponHand)
	{
		index = m_LeftItemIndex;
	}
	else if (!m_LeftUseActive && !m_RightHandEmpty)
	{
		// The game draws the hands with the model of the weapon in them: with nothing at all to hold they'd be gone. So
		// the right hand keeps its weapon when it's the last thing carried (the left hand's item aside).
		C_BasePlayer *local = (C_BasePlayer *)m_Game->GetClientEntity(m_Game->m_EngineClient->GetLocalPlayer());
		InventoryItem items[64];
		const int count = local ? GetInventory(local, items, 64) : 0;
		int others = 0;
		for (int i = 0; i < count; ++i)
		{
			if (items[i].index == m_LeftItemIndex)
				continue;
			if (items[i].weapon == (void *)local->GetActiveWeapon())
				index = items[i].index;
			else
				++others;
		}
		if (index && !others)
		{
			m_Haptics.Play(Haptics::Hand::WeaponHand, HapticEffects::Cancel);
			Diagnostics::Log("dual wield: the right hand keeps its weapon, the last one carried");
			return;
		}
	}
	if (index)
		SendDrop(index);
}

// The right hand put what it held back on the body (UpdateBodyGrab) and is empty: that weapon stays the game's active
// one, unseen (Hooks::DrawModelForVR), with the hand open on its controller (BuildFirstPersonHands), and it does nothing
// (ProcessInput and the rest check m_RightHandEmpty)
void VR::EmptyRightHand(const char *why)
{
	C_BasePlayer *local = (C_BasePlayer *)m_Game->GetClientEntity(m_Game->m_EngineClient->GetLocalPlayer());
	const void *active = local ? (const void *)local->GetActiveWeapon() : nullptr;
	if (!active)
		return;
	m_RightHandEmpty = true;
	m_EmptyHandWeapon = active;
	isHoldingWeapon2Handed = false;
	Diagnostics::Log("hands: the right hand is empty (%s)", why);
}

// The right hand takes the weapon of a slot from the body (1 primary, 2 pistol or melee, 3 grenade, 4 kit, 5 pills). False
// if the player has none there.
bool VR::TakeRightSlot(int slot)
{
	C_BasePlayer *local = (C_BasePlayer *)m_Game->GetClientEntity(m_Game->m_EngineClient->GetLocalPlayer());
	InventoryItem items[64];
	const int count = local ? GetInventory(local, items, 64) : 0;
	bool has = false;
	for (int i = 0; i < count && !has; ++i)
		has = items[i].index != m_LeftItemIndex && GetWeaponSlot(items[i].id) == slot;
	if (!has)
		return false;

	m_RightHandEmpty = false;
	char command[16];
	snprintf(command, sizeof(command), "slot%d", slot);
	m_Game->ClientCmd_Unrestricted(command);
	return true;
}

// Every frame: the empty hand ends when the game takes out another weapon by itself (a weapon picked up, being down, the
// weapon buttons). The left hand using its item, and the mod changing the unseen weapon for it, don't end it.
void VR::UpdateEmptyHand(C_BasePlayer *local)
{
	if (!m_RightHandEmpty)
		return;
	const void *active = local ? (const void *)local->GetActiveWeapon() : nullptr;
	if (!active)
	{
		m_RightHandEmpty = false;
		return;
	}
	if (active == m_EmptyHandWeapon || m_LeftUseActive)
		return;
	if (m_LeftItemIndex && active == (const void *)m_Game->GetClientEntity(m_LeftItemIndex))
		return;
	// The left hand grabbed something in the world, which the game may take out: it's the left hand's (UpdateDualWield)
	if (m_LeftGrabUntil && NowMs() <= m_LeftGrabUntil)
		return;
	if (NowMs() - m_EmptyHandSelectMs < kEmptyHandSelectMs)
	{
		m_EmptyHandWeapon = active;
		return;
	}
	m_RightHandEmpty = false;
	Diagnostics::Log("hands: the right hand holds the weapon the game took out");
}

// A weapon of the player goes to the ground, falling from where it is (DualWield::ServerDropItem)
void VR::SendDrop(int entityIndex)
{
	char command[64];
	snprintf(command, sizeof(command), "l4d2vr_drop %d 0 0 0", entityIndex);
	m_Game->m_EngineClient->ServerCmd(command, true);
	Diagnostics::Log("dual wield: dropping entity %d", entityIndex);
}

// The hands together and the grab of the hand without the item held for 2 seconds: the item changes hands
void VR::UpdateHandSwap(unsigned long long now, float deltaSeconds, int activeIndex, int activeWeaponId, const InventoryItem *items, int count)
{
	int direction = 0;
	const bool bothValid = m_LeftControllerDeviceIndex != vr::k_unTrackedDeviceIndexInvalid && m_RightControllerDeviceIndex != vr::k_unTrackedDeviceIndexInvalid;
	// Not while the hands hold a weapon together, or the right one loads the left one's: they're close then too
	const bool handsBusy = m_LeftUseActive || isHoldingWeapon2Handed || m_RightHoldsLeftGun || m_RightMagazine;
	if (bothValid && !handsBusy && VectorLength(m_LeftControllerPose.TrackedDevicePos - m_RightControllerPose.TrackedDevicePos) < kHandSwapDistance)
	{
		const bool leftGrab = PressedDigitalAction(m_LeftHandGrab), rightGrab = PressedDigitalAction(m_RightHandGrab);
		if (!m_LeftItemIndex && activeIndex && !m_RightHandEmpty && DualWield::IsLeftHandItem(activeWeaponId) && leftGrab && !rightGrab)
			direction = 1;
		else if (m_LeftItemIndex && rightGrab && !leftGrab)
			direction = -1;
	}

	// Done: waits for the grab to be let go
	if (m_HandSwapDirection == 2 && direction != 0)
		return;

	if (direction != m_HandSwapDirection)
	{
		m_HandSwapDirection = direction;
		m_HandSwapSeconds = 0.0f;
		m_HandSwapPulseMs = 0;
	}
	if (direction == 0)
		return;

	m_HandSwapSeconds += deltaSeconds;
	if (now - m_HandSwapPulseMs >= kHandSwapPulseMs)
	{
		m_HandSwapPulseMs = now;
		const float amplitude = 0.1f + 0.5f * std::clamp(m_HandSwapSeconds / kHandSwapSeconds, 0.0f, 1.0f);
		m_Haptics.Play(Haptics::Hand::BothHands, { { 0.0f, 0.04f, 150.0f, amplitude } });
	}

	if (m_HandSwapSeconds < kHandSwapSeconds)
		return;

	if (direction == 1)
		GiveToLeftHand(activeIndex, activeWeaponId, items, count);
	else
		GiveToRightHand(items, count);
	m_Haptics.Play(Haptics::Hand::BothHands, HapticEffects::Click);
	m_HandSwapDirection = 2;
}

// What the right hand holds goes to the empty left hand, and the right hand is left empty. The game takes out another
// weapon unseen if the player has one, so the left hand's item isn't the game's (its use takes it out).
void VR::GiveToLeftHand(int activeIndex, int activeWeaponId, const InventoryItem *items, int count)
{
	const int other = PickRightWeapon(items, count, activeIndex, m_PrevRightWeaponIndex);
	TakeIntoLeftHand(activeIndex, activeWeaponId);
	EmptyRightHand("gave what it held to the left hand");
	SelectWeapon(other);
	Diagnostics::Log("dual wield: the item went from the right hand to the left");
}

// What the left hand holds goes to the right hand. A weapon of the extra slot goes back to its own slot, unless the
// player has another weapon there: then it keeps the extra slot while the right hand has it.
void VR::GiveToRightHand(const InventoryItem *items, int count)
{
	const int index = m_LeftItemIndex, id = m_LeftItemId;
	ClearLeftItem();
	if (index == m_ExtraWeaponIndex)
	{
		bool slotTaken = false;
		for (int i = 0; i < count; ++i)
			slotTaken |= items[i].index != index && GetWeaponSlot(items[i].id) == GetWeaponSlot(id);
		if (!slotTaken)
			m_ExtraWeaponIndex = 0;
	}
	m_RightHandEmpty = false;
	SelectWeapon(index);
	Diagnostics::Log("dual wield: the item went from the left hand to the right%s", index == m_ExtraWeaponIndex ? ", in the extra slot" : "");
}

// Another player says what their left hand holds (relayed by the server)
void VR::OnRemoteLeftItem(int playerIndex, int itemIndex)
{
	if (playerIndex >= 1 && playerIndex <= 32)
		m_RemoteLeftItem[playerIndex] = itemIndex;
}

// Test of the dual wield without the headset, from the console: "l4d2vr_dual_test grab" takes the nearest weapon or
// item with the left hand, "status" writes the inventory to the log
void VR::DualWieldTest(const char *action)
{
	C_BasePlayer *local = (C_BasePlayer *)m_Game->GetClientEntity(m_Game->m_EngineClient->GetLocalPlayer());
	if (!local)
		return;

	// The model's name has what comes after '_': "grab_katana", "near_painpills"
	const char *underscore = strchr(action, '_');
	const char *filter = underscore ? underscore + 1 : nullptr;

	// near_<model>: the player goes next to the nearest one (needs sv_cheats)
	if (strncmp(action, "near", 4) == 0)
	{
		const Vec3 eye(m_SetupOrigin.x, m_SetupOrigin.y, m_SetupOrigin.z);
		Vec3 target;
		if (FindNearestPickable(eye, 600.0f, target, filter))
		{
			Vec3 away(eye.x - target.x, eye.y - target.y, 0.0f);
			away = Normalized(away) * 55.0f;
			char command[96];
			snprintf(command, sizeof(command), "setpos %.0f %.0f %.0f", target.x + away.x, target.y + away.y, eye.z - 60.0f);
			Diagnostics::Log("dual wield test: %s (eyes at %.0f %.0f %.0f, it's at %.0f %.0f %.0f)", command, eye.x, eye.y, eye.z, target.x, target.y, target.z);
			m_Game->ClientCmd_Unrestricted(command);
		}
		return;
	}

	// grab_<model> with the left hand, grabright_<model> with the right one
	if (strncmp(action, "grab", 4) == 0)
	{
		const bool left = !strstr(action, "right");
		Vec3 target;
		if (FindNearestPickable(Vec3(m_SetupOrigin.x, m_SetupOrigin.y, m_SetupOrigin.z), 400.0f, target, filter))
		{
			Diagnostics::Log("dual wield test: %s grab at %.0f %.0f %.0f", left ? "left" : "right", target.x, target.y, target.z);
			// Without controllers ProcessInput presses nothing: the use goes as the console's, at the grab's moment
			if (TryGrabWorld(target, left))
				m_TestUsePressAt = m_GrabUsePressAt;
		}
		else
		{
			Diagnostics::Log("dual wield test: nothing to take near");
		}
		return;
	}

	if (strcmp(action, "status") == 0)
	{
		InventoryItem items[64];
		const int count = GetInventory(local, items, 64);
		const void *active = local->GetActiveWeapon();
		// The game's own GetSlot, through the hook
		typedef int(__thiscall *tGetSlot)(void *thisptr);
		const tGetSlot getSlot = (tGetSlot)m_Game->m_Offsets->ClientGetSlot.address;
		for (int i = 0; i < count; ++i)
		{
			const int slot = getSlot ? getSlot(items[i].weapon) : -1;
			const void *model = GetEntityModel(items[i].weapon), *worldModel = GetWorldModel(items[i].weapon);
			Diagnostics::Log("dual wield test: entity %d weapon %d slot %d%s%s, %s, in the world %s", items[i].index, items[i].id, slot,
				items[i].weapon == active ? " (active)" : "", items[i].index == m_LeftItemIndex ? " (left hand)" : "",
				model ? m_Game->m_ModelInfo->GetModelName((void *)model) : "no model", worldModel ? m_Game->m_ModelInfo->GetModelName((void *)worldModel) : "no model");
		}
		Diagnostics::Log("dual wield test: left item %d, extra weapon %d, right weapon %d, right hand %s", m_LeftItemIndex, m_ExtraWeaponIndex, m_RightWeaponIndex,
			m_RightHandEmpty ? "empty" : "holding it");
		return;
	}

	if (strcmp(action, "swap") == 0)
	{
		InventoryItem items[64];
		const int count = GetInventory(local, items, 64);
		int activeIndex = 0, activeId = 0;
		for (int i = 0; i < count; ++i)
		{
			if (items[i].weapon == local->GetActiveWeapon())
			{
				activeIndex = items[i].index;
				activeId = items[i].id;
			}
		}
		if (!m_LeftItemIndex && !m_RightHandEmpty && DualWield::IsLeftHandItem(activeId))
			GiveToLeftHand(activeIndex, activeId, items, count);
		else if (m_LeftItemIndex)
			GiveToRightHand(items, count);
		return;
	}

	if (strcmp(action, "shoulder") == 0)
	{
		Diagnostics::Log("dual wield test: left shoulder %s", LeftShoulderGrab() ? "did something" : "did nothing");
		return;
	}
	// support: the right hand holds the left hand's gun too, as far as it can without controllers
	if (strcmp(action, "support") == 0)
	{
		Vector before, after;
		QAngle beforeAngles(0.0f, 0.0f, 0.0f), afterAngles(0.0f, 0.0f, 0.0f);
		const bool had = GetLeftGunAim(before, beforeAngles);
		m_RightHoldsLeftGun = m_RightHoldsLeftGunTest = m_RightHandEmpty && LeftGun::Find(m_LeftItemId) && LeftGun::Find(m_LeftItemId)->twoHanded;
		const bool has = GetLeftGunAim(after, afterAngles);
		Diagnostics::Log("dual wield test: the right hand %s the left hand's gun; aim %s %.0f %.0f, then %s %.0f %.0f", m_RightHoldsLeftGun ? "holds" : "can't hold",
			had ? "" : "(none)", beforeAngles.x, beforeAngles.y, has ? "" : "(none)", afterAngles.x, afterAngles.y);
		return;
	}
	// magazine: the right hand takes one for the left hand's gun, and puts it in at once; ammoleft: ammo taken for it
	if (strcmp(action, "magazine") == 0)
	{
		const LeftGun::GunData *gun = LeftGun::Find(m_LeftItemId);
		m_RightMagazine = m_RightMagazineTest = gun && m_RightHandEmpty && LeftGunNeedsAmmo(*gun);
		m_RightMagazineIndex = m_LeftItemIndex;
		Diagnostics::Log("dual wield test: the right hand %s a magazine for the left hand's gun", m_RightMagazine ? "takes" : "can't take");
		return;
	}
	if (strcmp(action, "ammoleft") == 0)
	{
		GiveLeftGunAmmo();
		return;
	}
	// fireleft: a shot of the left hand's gun from the eyes, where they look (no controllers)
	if (strcmp(action, "fireleft") == 0)
	{
		const LeftGun::GunData *gun = LeftGun::Find(m_LeftItemId);
		const void *weapon = gun ? m_Game->GetClientEntity(m_LeftItemIndex) : nullptr;
		if (!weapon)
		{
			Diagnostics::Log("dual wield test: the left hand has no gun");
			return;
		}
		QAngle view;
		m_Game->m_EngineClient->GetViewAngles(view);
		FireLeftGun(*gun, m_SetupOrigin, view);
		Diagnostics::Log("dual wield test: the left hand's gun fired, its magazine had %d", *(const int *)((const uint8_t *)weapon + 0x984));
		return;
	}
	if (strcmp(action, "kit") == 0)
	{
		Diagnostics::Log("dual wield test: left hand at the kit %s", LeftHandKitGrab() ? "did something" : "did nothing");
		return;
	}

	// hands: where the weapon's animation has the left hand, which is where it takes the weapon with both hands
	if (strcmp(action, "hands") == 0)
	{
		bool known = false;
		const bool there = IsAtTwoHandSpot(known);
		const Vec3 span = m_AnimatedHand[HandRig::Left] - m_AnimatedHand[HandRig::Right];
		const Vec3 forward(m_ViewmodelForward.x, m_ViewmodelForward.y, m_ViewmodelForward.z);
		Diagnostics::Log("dual wield test: weapon %d, the animation's left hand is %.2f m from the right one (%.2f m along the weapon), %s; the left hand %s",
			m_ActiveWeaponId, Length(span) / m_VRScale, Dot(span, forward) / m_VRScale, known ? "on the weapon" : "not on the weapon",
			there ? "is there" : "isn't there");
		return;
	}

	// useitem: the pills or adrenaline carried used at once, as the gesture does; drop/dropleft: what a hand holds drops
	if (strcmp(action, "useitem") == 0)
	{
		InventoryItem items[64];
		const int count = GetInventory(local, items, 64);
		for (int i = 0; i < count; ++i)
		{
			if (items[i].id == C_WeaponCSBase::PAIN_PILLS || items[i].id == C_WeaponCSBase::ADRENALINE)
			{
				char command[48];
				snprintf(command, sizeof(command), "l4d2vr_use_item %d", items[i].index);
				m_Game->m_EngineClient->ServerCmd(command, true);
				Diagnostics::Log("dual wield test: %s", command);
				break;
			}
		}
		return;
	}
	if (strcmp(action, "drop") == 0 || strcmp(action, "dropleft") == 0)
	{
		DropFromHand(strcmp(action, "drop") == 0);
		return;
	}

	if (strcmp(action, "doors") == 0)
	{
		LogDoors(Vec3(m_SetupOrigin.x, m_SetupOrigin.y, m_SetupOrigin.z));
		return;
	}

	// lefttrigger_<ms>, righttrigger_<ms>: the trigger held that long
	if (strncmp(action, "lefttrigger", 11) == 0 || strncmp(action, "righttrigger", 12) == 0)
	{
		m_TestHeldAction = action[0] == 'l' ? &m_ActionSecondaryAttack : &m_ActionPrimaryAttack;
		m_TestHeldUntil = NowMs() + (filter ? std::clamp(atoi(filter), 0, 10000) : 1500);
		return;
	}
	// armthrow: a throw with the arm released now, which the game's next throw (-attack) uses (Hooks::dStartGrenadeThrow)
	if (strcmp(action, "armthrow") == 0)
	{
		GrenadeThrow grenadeThrow;
		grenadeThrow.pending = true;
		grenadeThrow.gesture = true;
		grenadeThrow.time = std::chrono::steady_clock::now();
		grenadeThrow.handSpeed = 4.0f;
		grenadeThrow.armVelocity = m_HmdForward * 500.0f;
		m_GrenadeThrowSequence = m_GrenadeThrowSequence % 3 + 1;
		grenadeThrow.sequence = m_GrenadeThrowSequence;
		std::lock_guard<std::mutex> lock(m_GrenadeThrowMutex);
		m_GrenadeThrow = grenadeThrow;
		return;
	}
	// leftgrenade: the grenade carried goes to the left hand, as from the chest
	if (strcmp(action, "leftgrenade") == 0)
	{
		InventoryItem items[64];
		const int count = GetInventory(local, items, 64);
		for (int i = 0; i < count; ++i)
		{
			if (IsThrowableWeapon(items[i].id))
			{
				TakeIntoLeftHand(items[i].index, items[i].id);
				break;
			}
		}
		return;
	}

	// empty: the right hand puts its weapon away; take_<slot>: it takes the one of that slot (UpdateBodyGrab)
	if (strcmp(action, "empty") == 0)
	{
		EmptyRightHand("test");
		return;
	}
	if (strncmp(action, "take", 4) == 0 && filter)
	{
		Diagnostics::Log("dual wield test: take slot %s: %s", filter, TakeRightSlot(atoi(filter)) ? "taken" : "nothing there");
		return;
	}

	// texoverlay: images given to an overlay through a texture of the game's device (UploadOverlayPixels)
	if (strcmp(action, "texoverlay") == 0)
	{
		vr::VROverlayHandle_t handle = vr::k_ulOverlayHandleInvalid;
		if (m_Overlay->CreateOverlay("L4D2VRTextureTest", "L4D2VR texture test", &handle) != vr::VROverlayError_None)
			return;
		OverlayTexture texture;
		std::vector<uint8_t> pixels(256 * 128 * 4, 200);
		int done = 0;
		for (int i = 0; i < 300; ++i)
		{
			pixels[0] = (uint8_t)i;
			done += UploadOverlayPixels(handle, texture, pixels.data(), 256, 128) ? 1 : 0;
		}
		Diagnostics::Log("dual wield test: %d of 300 images given to an overlay through a texture", done);
		m_Overlay->DestroyOverlay(handle);
		if (texture.surface)
			texture.surface->Release();
		if (texture.texture)
			texture.texture->Release();
		return;
	}

	// vmgrip: how the weapon's animation holds it in the right hand, in the log
	if (strcmp(action, "vmgrip") == 0)
	{
		m_LogViewmodelGrip = 1;
		return;
	}

	// needle: the adrenaline's first person model, for its needle
	if (strcmp(action, "needle") == 0)
	{
		Diagnostics::Log("dual wield test: the adrenaline's first person model is %s", GetSyringeModel() ? "loaded" : "NOT loaded");
		return;
	}

	// thigh: the weapon hand's grab where the pistol is on the thigh
	if (strcmp(action, "thigh") == 0)
	{
		if (!m_HolsterValid)
		{
			Diagnostics::Log("dual wield test: no place on the thigh yet");
			return;
		}
		const Vector feet = m_CameraAnchor - Vector(0, 0, 64);
		const Vec3 fist = GetFistPoint(true);
		Diagnostics::Log("dual wield test: thigh %.1f %.1f %.1f units from the eyes, which are %.2f m above the floor; the body faces %.0f deg, the head %.0f deg; the right fist is %.1f units from the controller",
			m_HolsterPoint.x - m_HmdPosAbs.x, m_HolsterPoint.y - m_HmdPosAbs.y, m_HolsterPoint.z - m_HmdPosAbs.z, m_HmdPosLocalInWorld.z / m_VRScale,
			atan2f(m_BodyFacing.y, m_BodyFacing.x) * 57.2958f, atan2f(m_HmdForward.y, m_HmdForward.x) * 57.2958f,
			Length(fist - Vec3(m_RightControllerPosAbs.x, m_RightControllerPosAbs.y, m_RightControllerPosAbs.z)));
		const Vec3 onPistol = m_HolsterPoint + m_HolsterBarrel * (0.05f * m_VRScale);
		UpdateBodyGrab(true, Vector(m_HolsterPoint.x - feet.x, m_HolsterPoint.y - feet.y, m_HolsterPoint.z - feet.z), m_ActiveWeaponId, &onPistol);
		return;
	}

	if (strcmp(action, "use") == 0 && m_LeftItemIndex && DualWield::IsLeftUseItem(m_LeftItemId))
	{
		m_LeftUseActive = true;
		m_LeftUseWasActive = false;
		m_LeftUseStartMs = NowMs();
		m_LeftUseReleaseMs = NowMs();
		SelectWeapon(m_LeftItemIndex);
	}
}

// ---------------------------------------------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------------------------------------------

namespace
{
	Game *g_ServerGame = nullptr;

	struct ExtraSlot
	{
		int index;
		void *weapon;
	};
	ExtraSlot g_ExtraSlots[DualWield::kMaxPlayers];
	int g_ExtraSlotCount = 0;

	// While a player uses something with the left hand: the weapon the game asks the slot of, unowned
	void *g_LeftPickupPlayer = nullptr;
	void *g_LeftPickupWeapon = nullptr;
	// The client's word on the extra slot waits this long after the server gave it a weapon taken with the left hand
	const unsigned long long kExtraTakenMs = 1000;

	bool g_LoggedEdicts = false;

	typedef int(__thiscall *tEntindex)(void *thisptr);

	int EntIndex(void *entity)
	{
		static tEntindex entindex = (tEntindex)g_ServerGame->m_Offsets->CBaseEntity_entindex.address;
		return entity ? entindex(entity) : -1;
	}

	// The server's entities by index. CBaseEntity::entindex has where they are:
	//   mov ecx, [gpGlobals]; sub eax, [ecx + pEdicts]; sar eax, 4   (an edict is 16 bytes)
	const int kEdictSize = 16;
	const int kPEdictsOffset = 0x58;
	const int kMaxEdicts = 2048;

	void *EntityByIndexUnsafe(int index)
	{
		const uint8_t *code = (const uint8_t *)g_ServerGame->m_Offsets->CBaseEntity_entindex.address;
		if (code[8] != 0x8B || code[9] != 0x0D || code[14] != 0x2B || code[15] != 0x41 || code[16] != kPEdictsOffset)
			return nullptr;
		const uint8_t *globals = **(const uint8_t ***)(code + 10);
		const uint8_t *edicts = globals ? *(const uint8_t **)(globals + kPEdictsOffset) : nullptr;
		if (!edicts)
			return nullptr;
		IServerUnknown *unknown = *(IServerUnknown **)(edicts + index * kEdictSize + 12);
		void *entity = unknown ? unknown->GetBaseEntity() : nullptr;
		return entity && EntIndex(entity) == index ? entity : nullptr;
	}

	void *EntityByIndex(int index)
	{
		if (index <= 0 || index >= kMaxEdicts || !g_ServerGame)
			return nullptr;
		__try
		{
			return EntityByIndexUnsafe(index);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return nullptr;
		}
	}

	// The player that has the weapon, null if none or if it isn't one
	void *OwnerOf(void *weapon)
	{
		if (!weapon || !Hooks::m_GetGrenadeOwner)
			return nullptr;
		__try
		{
			return Hooks::m_GetGrenadeOwner(weapon);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return nullptr;
		}
	}

	// The weapon `index` of `player`, null if the player doesn't have it
	void *PlayersWeapon(void *player, int index)
	{
		void *entity = EntityByIndex(index);
		return entity && OwnerOf(entity) == player ? entity : nullptr;
	}

	void RebuildExtraSlots()
	{
		g_ExtraSlotCount = 0;
		for (int i = 0; i < DualWield::kMaxPlayers && i < (int)g_ServerGame->m_PlayersVRInfo.size(); ++i)
		{
			const Player &player = g_ServerGame->m_PlayersVRInfo[i];
			if (player.extraWeapon)
				g_ExtraSlots[g_ExtraSlotCount++] = { player.extraWeaponIndex, player.extraWeapon };
		}
	}

	int WeaponIdOf(void *weapon)
	{
		return weapon ? ((Server_WeaponCSBase *)weapon)->GetWeaponID() : 0;
	}

	void SetExtraWeapon(int playerIndex, void *weapon, int index)
	{
		Player &player = g_ServerGame->m_PlayersVRInfo[playerIndex];
		if (player.extraWeapon == weapon && player.extraWeaponIndex == index)
			return;
		player.extraWeapon = weapon;
		player.extraWeaponIndex = weapon ? index : 0;
		RebuildExtraSlots();
		if (weapon)
			Diagnostics::Log("dual wield: player %d carries weapon %d (entity %d) in the extra slot", playerIndex, WeaponIdOf(weapon), index);
		else
			Diagnostics::Log("dual wield: player %d has nothing in the extra slot", playerIndex);
	}

	// The end of the game's use of pills and adrenaline, once their animation is over (the item's frame, Offsets::
	// ItemUseFinish): the player down or dead uses nothing, then the item's effect (virtual, the item's), the player's
	// buttons as they were, the player's Weapon_Drop of it (virtual) and UTIL_Remove. The offsets of the virtuals are read
	// from that code, so they follow the game.
	struct ItemUseCode
	{
		bool found = false;
		int incapacitated = 0, alive = 0, effect = 0, drop = 0, buttons = 0;
		void(__cdecl *remove)(void *entity) = nullptr;
	};

	const ItemUseCode &GetItemUseCode()
	{
		static ItemUseCode code;
		static bool read = false;
		if (read)
			return code;
		read = true;

		const uint8_t *at = (const uint8_t *)g_ServerGame->m_Offsets->ItemUseFinish.address;
		if (!at)
		{
			Diagnostics::Log("dual wield: the end of the use of pills wasn't found, they're used the game's way");
			return code;
		}
		code.incapacitated = *(const int *)(at + 4);
		code.alive = *(const int *)(at + 20);
		code.effect = *(const int *)(at + 36);
		code.drop = *(const int *)(at + 52);
		code.buttons = *(const int *)(at + 62);
		code.remove = (void(__cdecl *)(void *))(at + 77 + *(const int *)(at + 73));
		const auto vtableOffset = [](int offset) { return offset > 0 && offset < 0x2000 && offset % 4 == 0; };
		code.found = vtableOffset(code.incapacitated) && vtableOffset(code.alive) && vtableOffset(code.effect) && vtableOffset(code.drop)
			&& code.buttons > 0 && code.buttons < 0x4000;
		Diagnostics::Log("dual wield: the end of the use of pills %s (effect 0x%X, drop 0x%X, buttons 0x%X)", code.found ? "found" : "NOT understood",
			code.effect, code.drop, code.buttons);
		return code;
	}

	template <typename T>
	T Virtual(void *object, int offset)
	{
		return (T)(*(void ***)object)[offset / 4];
	}

	bool UseItemNowUnsafe(void *player, void *item, const ItemUseCode &code)
	{
		typedef bool(__thiscall *tTest)(void *thisptr);
		if (Virtual<tTest>(player, code.incapacitated)(player) || !Virtual<tTest>(player, code.alive)(player))
			return false;

		int *buttons = (int *)((uint8_t *)player + code.buttons);
		const int savedButtons = *buttons;
		typedef void(__thiscall *tEffect)(void *thisptr, void *user);
		Virtual<tEffect>(item, code.effect)(item, player);
		*buttons = savedButtons;

		typedef void(__thiscall *tDrop)(void *thisptr, void *weapon, const Vector *target, const Vector *velocity);
		Virtual<tDrop>(player, code.drop)(player, item, nullptr, nullptr);
		code.remove(item);
		return true;
	}

	bool DropNowUnsafe(void *player, void *weapon, const ItemUseCode &code, const Vector &velocity)
	{
		typedef void(__thiscall *tDrop)(void *thisptr, void *weapon, const Vector *target, const Vector *velocity);
		Virtual<tDrop>(player, code.drop)(player, weapon, nullptr, &velocity);
		return true;
	}
}

// The player's pills or adrenaline used at once, as the game does once its animation is over: from the hand that
// brought them to the mouth or stabbed them in (VR::ItemUseGestureReady), whether they're the active weapon or not
void DualWield::ServerUseItem(edict_t *edict, int playerIndex, int itemIndex)
{
	if (!g_ServerGame || !edict || !edict->m_pUnk)
		return;
	void *player = edict->m_pUnk->GetBaseEntity();
	void *item = PlayersWeapon(player, itemIndex);
	const int id = item ? ((Server_WeaponCSBase *)item)->GetWeaponID() : 0;
	if (id != C_WeaponCSBase::PAIN_PILLS && id != C_WeaponCSBase::ADRENALINE)
		return;
	const ItemUseCode &code = GetItemUseCode();
	if (!code.found)
		return;

	bool used = false;
	__try
	{
		used = UseItemNowUnsafe(player, item, code);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		Diagnostics::Log("dual wield: using the item of player %d failed", playerIndex);
		return;
	}
	Diagnostics::Log("dual wield: player %d %s %s", playerIndex, used ? "used" : "can't use", id == C_WeaponCSBase::PAIN_PILLS ? "pills" : "adrenaline");
}

// The player drops one of their weapons (held with the stick pressed, VR::UpdateStickHold), with the hand's speed
void DualWield::ServerDropItem(edict_t *edict, int playerIndex, int weaponIndex, const Vector &velocity)
{
	if (!g_ServerGame || !edict || !edict->m_pUnk)
		return;
	void *player = edict->m_pUnk->GetBaseEntity();
	void *weapon = PlayersWeapon(player, weaponIndex);
	const ItemUseCode &code = GetItemUseCode();
	if (!weapon || !code.found)
		return;

	__try
	{
		DropNowUnsafe(player, weapon, code, velocity);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		Diagnostics::Log("dual wield: dropping the weapon of player %d failed", playerIndex);
		return;
	}
	Diagnostics::Log("dual wield: player %d dropped weapon %d", playerIndex, ((Server_WeaponCSBase *)weapon)->GetWeaponID());
}

void DualWield::ServerInit(Game *game)
{
	g_ServerGame = game;
}

void DualWield::ServerReset()
{
	g_ExtraSlotCount = 0;
	g_LeftPickupPlayer = nullptr;
	g_LeftPickupWeapon = nullptr;
	if (!g_ServerGame)
		return;
	for (Player &player : g_ServerGame->m_PlayersVRInfo)
	{
		player.useAimLeft = false;
		player.leftItemIndex = 0;
		player.leftItem = nullptr;
		player.leftItemId = 0;
		player.extraWeaponIndex = 0;
		player.extraWeapon = nullptr;
		player.extraTakenMs = 0;
		player.leftHandUntil = 0;
		player.leftSwingUntil = 0;
	}
}

bool DualWield::ServerGetSlot(void *weapon, int &slot)
{
	for (int i = 0; i < g_ExtraSlotCount; ++i)
	{
		if (g_ExtraSlots[i].weapon == weapon && EntIndex(weapon) == g_ExtraSlots[i].index)
		{
			slot = kExtraSlot;
			return true;
		}
	}

	// Being taken with the left hand: the game looks for what it replaces in this slot
	if (g_LeftPickupPlayer && weapon && UsesExtraSlot(WeaponIdOf(weapon)) && OwnerOf(weapon) == nullptr)
	{
		g_LeftPickupWeapon = weapon;
		slot = kExtraSlot;
		return true;
	}
	return false;
}

void DualWield::ServerBeginUse(void *player, int playerIndex)
{
	if (!g_ServerGame || !g_ServerGame->IsValidPlayerIndex(playerIndex))
		return;
	const Player &vrPlayer = g_ServerGame->m_PlayersVRInfo[playerIndex];
	if (!vrPlayer.useAimLeft || GetTickCount64() >= vrPlayer.useAimUntil)
		return;
	g_LeftPickupPlayer = player;
	g_LeftPickupWeapon = nullptr;

	static unsigned long long s_Logged = 0;
	if (s_Logged != vrPlayer.useAimUntil)
	{
		s_Logged = vrPlayer.useAimUntil;
		Diagnostics::Log("dual wield: player %d uses with the left hand", playerIndex);
	}
}

void DualWield::ServerEndUse(void *player, int playerIndex)
{
	if (g_LeftPickupPlayer != player)
		return;
	g_LeftPickupPlayer = nullptr;

	void *weapon = g_LeftPickupWeapon;
	g_LeftPickupWeapon = nullptr;
	if (weapon)
		Diagnostics::Log("dual wield: the game asked the slot of weapon %d (entity %d), %s", WeaponIdOf(weapon), EntIndex(weapon), OwnerOf(weapon) == player ? "now the player's" : "not taken");
	if (weapon && OwnerOf(weapon) == player)
	{
		SetExtraWeapon(playerIndex, weapon, EntIndex(weapon));
		g_ServerGame->m_PlayersVRInfo[playerIndex].extraTakenMs = GetTickCount64();
		Diagnostics::Log("dual wield: player %d took weapon %d with the left hand", playerIndex, EntIndex(weapon));
	}
}

void DualWield::ServerSetLeftItem(edict_t *edict, int playerIndex, int itemIndex, int extraIndex)
{
	if (!g_ServerGame || !g_ServerGame->IsValidPlayerIndex(playerIndex) || !edict || !edict->m_pUnk)
		return;

	Player &vrPlayer = g_ServerGame->m_PlayersVRInfo[playerIndex];
	void *playerEntity = edict->m_pUnk->GetBaseEntity();
	vrPlayer.leftItemIndex = itemIndex;
	vrPlayer.leftItem = itemIndex ? PlayersWeapon(playerEntity, itemIndex) : nullptr;
	vrPlayer.leftItemId = WeaponIdOf(vrPlayer.leftItem);

	// The weapon in the extra slot, as the player says: the one in the left hand or behind the shoulder, none once the
	// right hand took it to a free slot of its own. One just taken with the left hand isn't known by the player yet.
	if (extraIndex >= 0)
	{
		void *extra = extraIndex ? PlayersWeapon(playerEntity, extraIndex) : nullptr;
		if (extra && !UsesExtraSlot(WeaponIdOf(extra)))
			extra = nullptr;
		if (extra != vrPlayer.extraWeapon && GetTickCount64() - vrPlayer.extraTakenMs < kExtraTakenMs)
			return;
		SetExtraWeapon(playerIndex, extra, extra ? extraIndex : 0);
	}
	else if (vrPlayer.leftItem && UsesExtraSlot(vrPlayer.leftItemId))
	{
		SetExtraWeapon(playerIndex, vrPlayer.leftItem, itemIndex);
	}
}

// One weapon of the player taken out the way the game's "use" does it (CBasePlayer::SelectItem with the weapon: the
// checks, then Weapon_Switch)
bool DualWield::ServerTakeOut(void *player, void *weapon)
{
	typedef void(__thiscall *tSelectItem)(void *player, void *weapon);
	typedef void *(__thiscall *tGetActiveWeapon)(void *player);
	const tSelectItem selectItem = g_ServerGame ? (tSelectItem)g_ServerGame->m_Offsets->ServerSelectItem.address : nullptr;
	const tGetActiveWeapon getActiveWeapon = g_ServerGame ? (tGetActiveWeapon)g_ServerGame->m_Offsets->GetActiveWeapon.address : nullptr;
	if (!selectItem || !getActiveWeapon || !player || !weapon)
		return false;
	__try
	{
		if (getActiveWeapon(player) != weapon)
			selectItem(player, weapon);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

void *DualWield::ServerPlayersWeapon(void *player, int index)
{
	return PlayersWeapon(player, index);
}

// As the game's code does it: the entity's edict is flagged changed, all of it compared (FL_EDICT_CHANGED |
// FL_FULL_EDICT_CHANGED), or, while it can't be, the entity remembers it for later
void DualWield::ServerNetworkChanged(void *entity)
{
	const int kNetworkPending = 0x64, kNetworkPendingFlags = 0x68, kEdict = 0x28;
	if (!entity)
		return;
	uint8_t *bytes = (uint8_t *)entity;
	if (bytes[kNetworkPending])
	{
		bytes[kNetworkPendingFlags] |= 1;
		return;
	}
	if (uint32_t *edictFlags = *(uint32_t **)(bytes + kEdict))
		*edictFlags |= 0x101;
}

void DualWield::ServerSelectWeapon(edict_t *edict, int playerIndex, int weaponIndex)
{
	if (!g_ServerGame || !edict || !edict->m_pUnk)
		return;
	void *player = edict->m_pUnk->GetBaseEntity();
	void *weapon = PlayersWeapon(player, weaponIndex);
	typedef void *(__thiscall *tGetActiveWeapon)(void *player);
	const tGetActiveWeapon getActiveWeapon = (tGetActiveWeapon)g_ServerGame->m_Offsets->GetActiveWeapon.address;
	const bool done = weapon && ServerTakeOut(player, weapon);
	Diagnostics::Log("dual wield: player %d takes out entity %d: %s", playerIndex, weaponIndex, !weapon ? "not theirs" : !done ? "failed"
		: getActiveWeapon && getActiveWeapon(player) == weapon ? "out" : "the game refused");
}

void DualWield::ServerSetLeftHand(int playerIndex, const Vector &position, const QAngle &angles, int flags, const QAngle *from)
{
	if (!g_ServerGame || !g_ServerGame->IsValidPlayerIndex(playerIndex))
		return;

	Player &vrPlayer = g_ServerGame->m_PlayersVRInfo[playerIndex];
	const unsigned long long now = GetTickCount64();
	vrPlayer.leftHandPos = position;
	vrPlayer.leftHandAngle = angles;
	vrPlayer.leftHandUntil = now + 250;

	// Like the right hand's (Hooks::dProcessUsercmds): a swing starts again each time the hand slowed down below the
	// swing speed, and what it hits can be hit again. Traced from where the hand was just before.
	if (flags & kLeftHandSwinging)
	{
		if ((flags & kLeftHandNewSwing) || now > vrPlayer.leftSwingUntil + 150)
		{
			vrPlayer.leftSwingNew = true;
			vrPlayer.leftSwingPrevAngle = from ? *from : angles;
		}
		vrPlayer.leftSwingUntil = now + 120;
		vrPlayer.leftSwingTraced = false;
	}
}

void DualWield::ServerAfterUsercmds(edict_t *edict, void *player, int playerIndex)
{
	if (!g_ServerGame || !player || !g_ServerGame->IsValidPlayerIndex(playerIndex))
		return;

	if (!g_LoggedEdicts)
	{
		g_LoggedEdicts = true;
		Diagnostics::Log("dual wield: the server's entities %s", EntityByIndex(playerIndex) == player ? "found" : "NOT found, the left hand can't hold anything");
	}

	Player &vrPlayer = g_ServerGame->m_PlayersVRInfo[playerIndex];

	if (vrPlayer.leftItemIndex)
	{
		vrPlayer.leftItem = PlayersWeapon(player, vrPlayer.leftItemIndex);
		vrPlayer.leftItemId = vrPlayer.leftItem ? ((Server_WeaponCSBase *)vrPlayer.leftItem)->GetWeaponID() : 0;
	}
	else
	{
		vrPlayer.leftItem = nullptr;
		vrPlayer.leftItemId = 0;
	}

	if (vrPlayer.extraWeapon && PlayersWeapon(player, vrPlayer.extraWeaponIndex) != vrPlayer.extraWeapon)
		SetExtraWeapon(playerIndex, nullptr, 0);

	// The left hand's melee weapon hits what it went through since the last pose
	const unsigned long long now = GetTickCount64();
	if (vrPlayer.leftItem && vrPlayer.leftItemId == C_WeaponCSBase::MELEE && now < vrPlayer.leftSwingUntil && !vrPlayer.leftSwingTraced)
	{
		Hooks::TraceMeleeSwing(vrPlayer.leftItem, vrPlayer.leftSwingPrevAngle, vrPlayer.leftHandAngle, &vrPlayer.leftHandPos, vrPlayer.leftSwingNew);
		vrPlayer.leftSwingNew = false;
		vrPlayer.leftSwingPrevAngle = vrPlayer.leftHandAngle;
		vrPlayer.leftSwingTraced = true;
	}
}
