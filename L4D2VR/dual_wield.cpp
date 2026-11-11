#include "dual_wield.h"
#include "vr.h"
#include <Windows.h>
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
// - grabbing the chest where the grenade or the pills are (body_items.cpp), and puts it back the same way,
// - grabbing one in the world (TryGrabWorld): the server is told it's for the left hand,
// - holding the hands together with the item in the right hand, and the left grab for 2 seconds. The right grab does
//   the opposite.
// While the left hand works the weapon (magazine, shell, bolt, two handed grip) its item is put away for a moment.
// Its trigger uses a grenade, pills or adrenaline: they become the active weapon in the left hand until used.
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
	// Hooks::TraceMeleeSwing turns the aim by this, degrees
	const float kServerMeleeTraceTilt = 50.0f;

	unsigned long long NowMs() { return GetTickCount64(); }

	int g_ClientExtraMelee = 0;
}

bool DualWield::IsLeftHandItem(int weaponId)
{
	switch (weaponId)
	{
	case C_WeaponCSBase::MELEE:
	case C_WeaponCSBase::MOLOTOV:
	case C_WeaponCSBase::PIPE_BOMB:
	case C_WeaponCSBase::VOMITJAR:
	case C_WeaponCSBase::PAIN_PILLS:
	case C_WeaponCSBase::ADRENALINE:
		return true;
	default:
		return false;
	}
}

const char *DualWield::ClassName(int weaponId)
{
	switch (weaponId)
	{
	case C_WeaponCSBase::PISTOL: return "weapon_pistol";
	case C_WeaponCSBase::MAGNUM: return "weapon_pistol_magnum";
	case C_WeaponCSBase::MELEE: return "weapon_melee";
	case C_WeaponCSBase::CHAINSAW: return "weapon_chainsaw";
	case C_WeaponCSBase::UZI: return "weapon_smg";
	case C_WeaponCSBase::MAC10: return "weapon_smg_silenced";
	case C_WeaponCSBase::MP5: return "weapon_smg_mp5";
	case C_WeaponCSBase::PUMPSHOTGUN: return "weapon_pumpshotgun";
	case C_WeaponCSBase::SHOTGUN_CHROME: return "weapon_shotgun_chrome";
	case C_WeaponCSBase::AUTOSHOTGUN: return "weapon_autoshotgun";
	case C_WeaponCSBase::SPAS: return "weapon_shotgun_spas";
	case C_WeaponCSBase::M16A1: return "weapon_rifle";
	case C_WeaponCSBase::SCAR: return "weapon_rifle_desert";
	case C_WeaponCSBase::AK47: return "weapon_rifle_ak47";
	case C_WeaponCSBase::SG552: return "weapon_rifle_sg552";
	case C_WeaponCSBase::M60: return "weapon_rifle_m60";
	case C_WeaponCSBase::HUNTING_RIFLE: return "weapon_hunting_rifle";
	case C_WeaponCSBase::SNIPER_MILITARY: return "weapon_sniper_military";
	case C_WeaponCSBase::AWP: return "weapon_sniper_awp";
	case C_WeaponCSBase::SCOUT: return "weapon_sniper_scout";
	case C_WeaponCSBase::GRENADE_LAUNCHER: return "weapon_grenade_launcher";
	case C_WeaponCSBase::MOLOTOV: return "weapon_molotov";
	case C_WeaponCSBase::PIPE_BOMB: return "weapon_pipe_bomb";
	case C_WeaponCSBase::VOMITJAR: return "weapon_vomitjar";
	case C_WeaponCSBase::PAIN_PILLS: return "weapon_pain_pills";
	case C_WeaponCSBase::ADRENALINE: return "weapon_adrenaline";
	case C_WeaponCSBase::FIRST_AID_KIT: return "weapon_first_aid_kit";
	case C_WeaponCSBase::DEFIBRILLATOR: return "weapon_defibrillator";
	case C_WeaponCSBase::INCENDIARY_AMMO: return "weapon_upgradepack_incendiary";
	case C_WeaponCSBase::FRAG_AMMO: return "weapon_upgradepack_explosive";
	default: return "";
	}
}

// ---------------------------------------------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------------------------------------------

void DualWield::ClientSetExtraMelee(int entityIndex)
{
	g_ClientExtraMelee = entityIndex;
}

bool DualWield::ClientGetSlot(void *weapon, int &slot)
{
	if (!g_ClientExtraMelee || !weapon || !g_Game || !g_Game->m_ClientEntityList)
		return false;
	if ((void *)g_Game->GetClientEntity(g_ClientExtraMelee) != weapon)
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

void VR::SelectWeapon(int weaponId)
{
	const char *name = DualWield::ClassName(weaponId);
	if (!*name)
		return;
	char command[64];
	snprintf(command, sizeof(command), "use %s", name);
	m_Game->ClientCmd_Unrestricted(command);
}

void VR::TakeIntoLeftHand(int entityIndex, int weaponId)
{
	if (m_LeftItemIndex == entityIndex)
		return;
	m_LeftItemIndex = entityIndex;
	m_LeftItemId = weaponId;
	m_LeftGripValid = false;
	if (weaponId == C_WeaponCSBase::MELEE)
		m_ExtraMeleeIndex = entityIndex;
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
	if (m_LeftItemIndex && m_LeftItemId != C_WeaponCSBase::MELEE)
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

	// A melee weapon in the hand has nowhere to go
	if (m_LeftItemId == C_WeaponCSBase::MELEE)
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

// A grab with the left hand behind the left shoulder: the melee weapon in it goes there, or the one there comes to it.
// True if it did something.
bool VR::LeftShoulderGrab()
{
	if (!m_DualWieldEnabled)
		return false;

	if (m_LeftItemIndex)
	{
		if (m_LeftItemId != C_WeaponCSBase::MELEE)
			return false;
		ClearLeftItem();
		m_Haptics.Play(Haptics::Hand::OtherHand, HapticEffects::Confirm);
		return true;
	}

	// One that isn't in the right hand, the one that was in this hand first
	C_BasePlayer *local = (C_BasePlayer *)m_Game->GetClientEntity(m_Game->m_EngineClient->GetLocalPlayer());
	if (!local)
		return false;
	InventoryItem items[64];
	const int count = GetInventory(local, items, 64);
	const void *active = local->GetActiveWeapon();
	int melee = 0;
	for (int i = 0; i < count; ++i)
	{
		if (items[i].id == C_WeaponCSBase::MELEE && items[i].weapon != active && (!melee || items[i].index == m_ExtraMeleeIndex))
			melee = items[i].index;
	}
	if (!melee)
		return false;

	TakeIntoLeftHand(melee, C_WeaponCSBase::MELEE);
	m_Haptics.Play(Haptics::Hand::OtherHand, HapticEffects::Take);
	return true;
}

// What the left hand holds is drawn in it: the item's own model, with its "weapon_bone" in the fist like in the third
// person
bool VR::GetLeftItemDraw(BodyModelDraw &draw)
{
	if (!LeftHandHoldsItem() || m_LeftHandFollow < 1.0f)
		return false;

	const void *entity = m_Game->GetClientEntity(m_LeftItemIndex);
	const void *model = entity ? GetEntityModel(entity) : nullptr;
	if (!model)
		return false;

	// A melee weapon's weapon bone has y along the blade and z towards the edge: the blade comes out of the fist by the
	// thumb, like a sword held, with the edge on the side of the fingers
	draw = BodyModelDraw();
	draw.model = model;
	draw.alignWeaponBone = true;
	return GetLeftGripFrame(draw.frame);
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

	m_RightControllerPosAbs = m_LeftControllerPosAbs;
	m_RightControllerForward = m_LeftControllerForward;
	m_RightControllerRight = m_LeftControllerRight;
	m_RightControllerUp = m_LeftControllerUp;
	m_RightControllerPosAbsRaw = m_LeftControllerPosAbs;
	m_RightControllerForwardRaw = leftForwardRaw;
	m_RightControllerRightRaw = leftRightRaw;
	m_RightControllerUpRaw = leftUpRaw;
}

// The best weapon for the right hand other than `exceptIndex`: `preferredId`, a primary weapon, a pistol
int VR::PickRightWeapon(const InventoryItem *items, int count, int exceptIndex, int preferredId) const
{
	int best = 0, bestRank = 0;
	for (int i = 0; i < count; ++i)
	{
		if (items[i].index == exceptIndex || items[i].index == m_LeftItemIndex)
			continue;
		int rank = 0;
		if (preferredId && items[i].id == preferredId)
			rank = 4;
		else if (GetWeaponSlot(items[i].id) == 1)
			rank = 3;
		else if (GetWeaponSlot(items[i].id) == 2)
			rank = 2;
		if (rank > bestRank)
		{
			bestRank = rank;
			best = items[i].id;
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
		m_ExtraMeleeIndex = 0;
		DualWield::ClientSetExtraMelee(0);
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

	if (m_ExtraMeleeIndex && !carried(m_ExtraMeleeIndex, C_WeaponCSBase::MELEE))
		m_ExtraMeleeIndex = 0;
	if (m_LeftItemIndex && !carried(m_LeftItemIndex, m_LeftItemId))
		ClearLeftItem();

	// Just grabbed something in the world with the left hand: what's new in the inventory goes to it
	if (m_LeftGrabUntil)
	{
		for (int i = 0; i < count && !m_LeftItemIndex; ++i)
		{
			bool before = false;
			for (int j = 0; j < m_InventoryBeforeCount; ++j)
				before |= m_InventoryBefore[j] == items[i].index;
			if (!before && DualWield::IsLeftHandItem(items[i].id))
			{
				TakeIntoLeftHand(items[i].index, items[i].id);
				m_LeftGrabUntil = 0;
			}
		}
		if (now > m_LeftGrabUntil)
			m_LeftGrabUntil = 0;
	}

	// What the right hand holds, to go back to it
	if (activeIndex && activeIndex != m_LeftItemIndex && activeWeaponId != m_RightWeaponId)
	{
		m_PrevRightWeaponId = m_RightWeaponId;
		m_RightWeaponId = activeWeaponId;
	}

	UpdateLeftUse(now, activeIndex, items, count);

	// The left hand's item became the active weapon without being used (the game takes a weapon picked up in its hand):
	// back to the right hand's
	if (m_LeftItemIndex && activeIndex == m_LeftItemIndex && !m_LeftUseActive && now - m_LastSwitchBackMs > kSwitchBackIntervalMs)
	{
		m_LastSwitchBackMs = now;
		const int right = PickRightWeapon(items, count, m_LeftItemIndex, m_RightWeaponId);
		if (right)
			SelectWeapon(right);
	}

	UpdateLeftSwing(now);
	UpdateHandSwap(now, deltaSeconds, activeIndex, activeWeaponId, items, count);

	DualWield::ClientSetExtraMelee(m_ExtraMeleeIndex);

	// The server has to know, for the melee hits, the slot and the other players
	if (m_LeftItemIndex != m_SentLeftItemIndex || now - m_LeftItemSentMs > kLeftItemResendMs)
	{
		m_SentLeftItemIndex = m_LeftItemIndex;
		m_LeftItemSentMs = now;
		char command[48];
		snprintf(command, sizeof(command), "l4d2vr_left %d", m_LeftItemIndex);
		m_Game->m_EngineClient->ServerCmd(command, true);
	}
}

// The trigger of the left hand uses its grenade, pills or adrenaline: they're taken as the active weapon, and the
// trigger is the attack until it's used (UpdateGrenadeThrow, ProcessInput)
void VR::UpdateLeftUse(unsigned long long now, int activeIndex, const InventoryItem *items, int count)
{
	const bool trigger = PressedDigitalAction(m_ActionSecondaryAttack);
	const bool pressed = trigger && !m_LeftTriggerDown;
	m_LeftTriggerDown = trigger;

	if (!m_LeftUseActive)
	{
		if (pressed && LeftHandHoldsItem() && m_LeftItemId != C_WeaponCSBase::MELEE && !m_ScopeZoomTookTrigger && activeIndex != m_LeftItemIndex)
		{
			m_LeftUseActive = true;
			m_LeftUseWasActive = false;
			m_LeftUseStartMs = now;
			m_LeftUseReleaseMs = 0;
			SelectWeapon(m_LeftItemId);
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
	{
		const int right = PickRightWeapon(items, count, m_LeftItemIndex, m_RightWeaponId);
		if (right)
			SelectWeapon(right);
	}
}

// The left hand's melee weapon hits what it swings through: its pose goes to the server while it swings
void VR::UpdateLeftSwing(unsigned long long now)
{
	if (!LeftHandHoldsMelee() || m_LeftControllerDeviceIndex == vr::k_unTrackedDeviceIndexInvalid)
	{
		m_LeftSwinging = false;
		return;
	}

	const bool swinging = VectorLength(m_LeftControllerPose.TrackedDeviceVel) > m_MeleeSwingVelocityRequired;
	if (swinging && !m_LeftSwinging)
		m_Haptics.Play(Haptics::Hand::OtherHand, HapticEffects::Hint);
	m_LeftSwinging = swinging;

	if (!swinging || now - m_LeftHandSentMs < kLeftHandSendIntervalMs)
		return;
	m_LeftHandSentMs = now;

	// The server traces 50 degrees up from the aim it gets (made for the right hand's weapons, held up from the aim).
	// The aim sent is the blade as it's drawn (GetLeftItemDraw) turned by as much towards its edge, with "up" towards
	// the blade, so the traces go along it.
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

	char command[128];
	snprintf(command, sizeof(command), "l4d2vr_left_hand %.1f %.1f %.1f %.1f %.1f %.1f %d", m_LeftControllerPosAbs.x, m_LeftControllerPosAbs.y, m_LeftControllerPosAbs.z,
		aim.x, aim.y, aim.z, DualWield::kLeftHandSwinging);
	m_Game->m_EngineClient->ServerCmd(command, false);
}

// The hands together and the grab of the hand without the item held for 2 seconds: the item changes hands
void VR::UpdateHandSwap(unsigned long long now, float deltaSeconds, int activeIndex, int activeWeaponId, const InventoryItem *items, int count)
{
	int direction = 0;
	const bool bothValid = m_LeftControllerDeviceIndex != vr::k_unTrackedDeviceIndexInvalid && m_RightControllerDeviceIndex != vr::k_unTrackedDeviceIndexInvalid;
	if (bothValid && !m_LeftUseActive && VectorLength(m_LeftControllerPose.TrackedDevicePos - m_RightControllerPose.TrackedDevicePos) < kHandSwapDistance)
	{
		const bool leftGrab = PressedDigitalAction(m_LeftHandGrab), rightGrab = PressedDigitalAction(m_RightHandGrab);
		if (!m_LeftItemIndex && activeIndex && DualWield::IsLeftHandItem(activeWeaponId) && leftGrab && !rightGrab)
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
	{
		const int right = PickRightWeapon(items, count, activeIndex, m_PrevRightWeaponId);
		TakeIntoLeftHand(activeIndex, activeWeaponId);
		if (right)
			SelectWeapon(right);
		Diagnostics::Log("dual wield: the item went from the right hand to the left");
	}
	else
	{
		const int id = m_LeftItemId;
		ClearLeftItem();
		SelectWeapon(id);
		Diagnostics::Log("dual wield: the item went from the left hand to the right");
	}
	m_Haptics.Play(Haptics::Hand::BothHands, HapticEffects::Click);
	m_HandSwapDirection = 2;
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
			TryGrabWorld(target, left);
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
			Diagnostics::Log("dual wield test: entity %d weapon %d slot %d%s%s", items[i].index, items[i].id, slot,
				items[i].weapon == active ? " (active)" : "", items[i].index == m_LeftItemIndex ? " (left hand)" : "");
		}
		Diagnostics::Log("dual wield test: left item %d, extra melee %d, right weapon %d", m_LeftItemIndex, m_ExtraMeleeIndex, m_RightWeaponId);
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
		if (!m_LeftItemIndex && DualWield::IsLeftHandItem(activeId))
		{
			const int right = PickRightWeapon(items, count, activeIndex, m_PrevRightWeaponId);
			TakeIntoLeftHand(activeIndex, activeId);
			if (right)
				SelectWeapon(right);
		}
		else if (m_LeftItemIndex)
		{
			const int id = m_LeftItemId;
			ClearLeftItem();
			SelectWeapon(id);
		}
		return;
	}

	if (strcmp(action, "shoulder") == 0)
	{
		Diagnostics::Log("dual wield test: left shoulder %s", LeftShoulderGrab() ? "did something" : "did nothing");
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
		Diagnostics::Log("dual wield test: thigh %.1f %.1f %.1f units from the eyes, which are %.2f m above the floor",
			m_HolsterPoint.x - m_HmdPosAbs.x, m_HolsterPoint.y - m_HmdPosAbs.y, m_HolsterPoint.z - m_HmdPosAbs.z, m_HmdPosLocalInWorld.z / m_VRScale);
		UpdateBodyGrab(true, Vector(m_HolsterPoint.x - feet.x, m_HolsterPoint.y - feet.y, m_HolsterPoint.z - feet.z), m_ActiveWeaponId);
		return;
	}

	if (strcmp(action, "use") == 0 && m_LeftItemIndex && m_LeftItemId != C_WeaponCSBase::MELEE)
	{
		m_LeftUseActive = true;
		m_LeftUseWasActive = false;
		m_LeftUseStartMs = NowMs();
		m_LeftUseReleaseMs = NowMs();
		SelectWeapon(m_LeftItemId);
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

	// While a player uses something with the left hand: the melee weapon the game asks the slot of, unowned
	void *g_LeftPickupPlayer = nullptr;
	void *g_LeftPickupWeapon = nullptr;

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
			if (player.extraMelee)
				g_ExtraSlots[g_ExtraSlotCount++] = { player.extraMeleeIndex, player.extraMelee };
		}
	}

	void SetExtraMelee(int playerIndex, void *weapon, int index)
	{
		Player &player = g_ServerGame->m_PlayersVRInfo[playerIndex];
		if (player.extraMelee == weapon && player.extraMeleeIndex == index)
			return;
		player.extraMelee = weapon;
		player.extraMeleeIndex = weapon ? index : 0;
		RebuildExtraSlots();
		Diagnostics::Log("dual wield: player %d %s", playerIndex, weapon ? "carries a melee weapon in the extra slot" : "has no melee weapon in the extra slot");
	}
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
		player.extraMeleeIndex = 0;
		player.extraMelee = nullptr;
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
	if (g_LeftPickupPlayer && weapon && ((Server_WeaponCSBase *)weapon)->GetWeaponID() == C_WeaponCSBase::MELEE && OwnerOf(weapon) == nullptr)
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
		Diagnostics::Log("dual wield: the game asked the slot of melee weapon %d, %s", EntIndex(weapon), OwnerOf(weapon) == player ? "now the player's" : "not taken");
	if (weapon && OwnerOf(weapon) == player)
	{
		SetExtraMelee(playerIndex, weapon, EntIndex(weapon));
		Diagnostics::Log("dual wield: player %d took melee weapon %d with the left hand", playerIndex, EntIndex(weapon));
	}
}

void DualWield::ServerSetLeftItem(edict_t *edict, int playerIndex, int itemIndex)
{
	if (!g_ServerGame || !g_ServerGame->IsValidPlayerIndex(playerIndex) || !edict || !edict->m_pUnk)
		return;

	Player &vrPlayer = g_ServerGame->m_PlayersVRInfo[playerIndex];
	void *playerEntity = edict->m_pUnk->GetBaseEntity();
	vrPlayer.leftItemIndex = itemIndex;
	vrPlayer.leftItem = itemIndex ? PlayersWeapon(playerEntity, itemIndex) : nullptr;
	vrPlayer.leftItemId = vrPlayer.leftItem ? ((Server_WeaponCSBase *)vrPlayer.leftItem)->GetWeaponID() : 0;

	// A melee weapon in the left hand keeps a slot of its own while the player has it
	if (vrPlayer.leftItem && vrPlayer.leftItemId == C_WeaponCSBase::MELEE)
		SetExtraMelee(playerIndex, vrPlayer.leftItem, itemIndex);
}

void DualWield::ServerSetLeftHand(int playerIndex, const Vector &position, const QAngle &angles, int flags)
{
	if (!g_ServerGame || !g_ServerGame->IsValidPlayerIndex(playerIndex))
		return;

	Player &vrPlayer = g_ServerGame->m_PlayersVRInfo[playerIndex];
	const unsigned long long now = GetTickCount64();
	vrPlayer.leftHandPos = position;
	vrPlayer.leftHandAngle = angles;
	vrPlayer.leftHandUntil = now + 250;

	if (flags & kLeftHandSwinging)
	{
		if (now > vrPlayer.leftSwingUntil + 150)
		{
			vrPlayer.leftSwingNew = true;
			vrPlayer.leftSwingPrevAngle = angles;
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

	if (vrPlayer.extraMelee && PlayersWeapon(player, vrPlayer.extraMeleeIndex) != vrPlayer.extraMelee)
		SetExtraMelee(playerIndex, nullptr, 0);

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
