#include "vr.h"
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <unordered_map>
#include <vector>
#include "sdk.h"
#include "game.h"
#include "hooks.h"
#include "studio.h"
#include "diagnostics.h"

using namespace HandMath;

namespace
{
	// How fast the left hand goes between the weapon and the controller, per second
	const float kFollowSpeed = 8.0f;
	const float kFingerSmoothing = 18.0f;
	const float kHoldingMagazineCurl = 0.55f;

	// The pose of the weapon and the hand before the throw started, relative to the weapon's frame, per model
	std::unordered_map<const void *, std::vector<Mat34>> g_ThrowPoses;
	// The right hand bone of each weapon model in first person (-1 if it has none)
	std::unordered_map<const void *, int> g_ViewmodelRightHands;
	// The bones of the left pistol in the dual pistols model
	std::unordered_map<const void *, std::vector<int>> g_SecondPistolBones;
	// After the throw the hand stays as it was until the game changes weapon
	const auto kThrownGrenadeTime = std::chrono::milliseconds(1500);

	// DT_BaseCombatWeapon::m_bInReload on the client
	const int kClientWeaponInReloadOffset = 0x9BD;

	Vec3 ToVec3(const Vector &v) { return Vec3(v.x, v.y, v.z); }

	// Weapons held with both hands the whole time
	bool NeedsBothHands(int weaponId)
	{
		switch (weaponId)
		{
		case C_WeaponCSBase::DEFIBRILLATOR:
		case C_WeaponCSBase::CHAINSAW:
		case C_WeaponCSBase::GASCAN:
		case C_WeaponCSBase::PROPANE_TANK:
		case C_WeaponCSBase::OXYGEN_TANK:
		case C_WeaponCSBase::GNOME_CHOMPSKI:
		case C_WeaponCSBase::COLA_BOTTLES:
		case C_WeaponCSBase::FIREWORKS_BOX:
		case C_WeaponCSBase::INCENDIARY_AMMO:
		case C_WeaponCSBase::FRAG_AMMO:
			return true;
		default:
			return false;
		}
	}
}

void VR::ClearModelCaches()
{
	m_HandModels.clear();
	m_ArmsRig = nullptr;
	g_ThrowPoses.clear();
	g_ViewmodelRightHands.clear();
	g_SecondPistolBones.clear();
	ClearBodyModels();
	Hooks::m_ModelKinds.clear();
}

// Items held in the fist instead of by a grip: the game's animations hold them away from where the pistol's grip is,
// so the hand and the item are put on the controller instead
bool VR::IsHeldInTheHand(int weaponId)
{
	return IsThrowableWeapon(weaponId) || weaponId == C_WeaponCSBase::PAIN_PILLS || weaponId == C_WeaponCSBase::ADRENALINE || weaponId == C_WeaponCSBase::FIRST_AID_KIT;
}

// The right hand bone where the controller is: the same place and angle as the left hand on its controller
bool VR::GetRightHandTarget(const HandRig &rig, Mat34 &hand, float backMeters) const
{
	if (m_RightControllerDeviceIndex == vr::k_unTrackedDeviceIndexInvalid || !m_Poses[m_RightControllerDeviceIndex].bPoseIsValid)
		return false;

	const Vector forward = VectorRotate(m_RightControllerForwardRaw, m_RightControllerRightRaw, kHandGripTilt);
	const Vector up = VectorRotate(m_RightControllerUpRaw, m_RightControllerRightRaw, kHandGripTilt);
	const Vector wrist = m_RightControllerPosAbs + (forward * (m_HandOffsetMeters.x - backMeters) + up * m_HandOffsetMeters.z) * m_VRScale;

	hand = rig.HandFromFrame(HandRig::Right, ToVec3(forward), ToVec3(up), ToVec3(wrist));
	return IsFinite(hand);
}

// The weapon model of an item held in the fist moves with the hand, so the item stays in it (see BuildFirstPersonHands
// for the hand itself). Returns the bones to draw with.
const void *VR::ApplyItemInHand(const void *studioHeader, const void *customBones, void *storage)
{
	// Two pistols shown as one: the left pistol is folded away
	if (m_DualAsOnePistol && Studio::IsValid(studioHeader))
	{
		const int count = Studio::BoneCount(studioHeader);
		if (count <= 0 || count > Studio::kMaxBones)
			return customBones;

		auto cached = g_SecondPistolBones.find(studioHeader);
		if (cached == g_SecondPistolBones.end())
		{
			std::vector<int> left;
			for (int i = 0; i < count; ++i)
			{
				const char *name = Studio::BoneName(studioHeader, i);
				const size_t length = strlen(name);
				if (length > 2 && _stricmp(name + length - 2, "_l") == 0 && strstr(name, "weapon"))
					left.push_back(i);
			}
			Diagnostics::Log("two pistols shown as one: %d bones of the left pistol in %s", (int)left.size(), Studio::ModelName(studioHeader));
			cached = g_SecondPistolBones.emplace(studioHeader, std::move(left)).first;
		}

		Mat34 *folded = (Mat34 *)storage;
		memcpy(folded, customBones, count * sizeof(Mat34));
		for (int bone : cached->second)
			folded[bone] = CollapsedAt(Origin(folded[bone]));
		return folded;
	}

	if (!IsHeldInTheHand(m_ActiveWeaponId) || !m_ArmsRig || !Studio::IsValid(studioHeader))
		return customBones;

	const int boneCount = Studio::BoneCount(studioHeader);
	if (boneCount <= 0 || boneCount > Studio::kMaxBones)
		return customBones;

	auto found = g_ViewmodelRightHands.find(studioHeader);
	if (found == g_ViewmodelRightHands.end())
		found = g_ViewmodelRightHands.emplace(studioHeader, Studio::FindBone(studioHeader, "ValveBiped.Bip01_R_Hand")).first;
	const int handBone = found->second;
	if (handBone < 0 || handBone >= boneCount)
		return customBones;

	Mat34 target;
	if (!GetRightHandTarget(*m_ArmsRig, target))
		return customBones;

	const Mat34 *source = (const Mat34 *)customBones;
	const Mat34 move = Multiply(target, InvertRigid(source[handBone]));
	Mat34 *bones = (Mat34 *)storage;
	for (int i = 0; i < boneCount; ++i)
		bones[i] = Multiply(move, source[i]);
	return bones;
}

bool VR::IsWeaponReloading() const
{
	C_BasePlayer *player = (C_BasePlayer *)m_Game->GetClientEntity(m_Game->m_EngineClient->GetLocalPlayer());
	if (!player)
		return false;

	C_BaseCombatWeapon *weapon = player->GetActiveWeapon();
	return weapon && *((const uint8_t *)weapon + kClientWeaponInReloadOffset) != 0;
}

bool VR::WantsLeftHandOnController() const
{
	if (m_LeftControllerDeviceIndex == vr::k_unTrackedDeviceIndexInvalid || !m_Poses[m_LeftControllerDeviceIndex].bPoseIsValid)
		return false;

	// Holding its own item (dual wield)
	if (LeftHandHoldsItem())
		return true;

	if (isHoldingWeapon2Handed || m_ViewmodelHasTwoWeapons || NeedsBothHands(m_ActiveWeaponId))
		return false;

	return !IsWeaponReloading();
}

bool VR::ThrownGrenadeHasLeftTheHand() const
{
	return m_GrenadeThrowGesture && IsThrowableWeapon(m_ActiveWeaponId) && !m_GrenadeTriggerHeld
		&& std::chrono::steady_clock::now() - m_GrenadeReleaseTime < kThrownGrenadeTime;
}

// Animations of the game that don't fit VR are not shown: the weapon and the hands keep the last pose from before they
// started, relative to the weapon's frame.
// - While throwing with the arm the game plays its wind-up animation, which pulls the hand back: the grenade stays
//   where the real hand is.
// - A pump action pumps by itself in its fire animation: it's pumped by hand instead (ManualReloadHoldsFirePose).
// Returns the bones to draw with.
const void *VR::ApplyThrowPose(const void *studioHeader, const void *customBones, void *storage)
{
	bool hold;
	if (m_GrenadeThrowGesture && IsThrowableWeapon(m_ActiveWeaponId))
		hold = m_GrenadeTriggerHeld || ThrownGrenadeHasLeftTheHand();
	else if (ManualReloadPumpsByHand())
		hold = ManualReloadHoldsFirePose();
	else
		return customBones;

	if (!Studio::IsValid(studioHeader))
		return customBones;

	const int boneCount = Studio::BoneCount(studioHeader);
	if (boneCount <= 0 || boneCount > Studio::kMaxBones)
		return customBones;

	const Mat34 frame = FromAxes(ToVec3(m_ViewmodelForward), ToVec3(m_ViewmodelRight) * -1.0f, ToVec3(m_ViewmodelUp), ToVec3(GetRecommendedViewmodelAbsPos()));
	std::vector<Mat34> &pose = g_ThrowPoses[studioHeader];

	if (!hold)
	{
		pose.resize(boneCount);
		const Mat34 inverse = InvertRigid(frame);
		const Mat34 *source = (const Mat34 *)customBones;
		for (int i = 0; i < boneCount; ++i)
			pose[i] = Multiply(inverse, source[i]);
		return customBones;
	}

	if ((int)pose.size() != boneCount)
		return customBones;

	Mat34 *bones = (Mat34 *)storage;
	for (int i = 0; i < boneCount; ++i)
		bones[i] = Multiply(frame, pose[i]);
	return bones;
}

void VR::UpdateHands(float deltaSeconds)
{
	deltaSeconds = std::clamp(deltaSeconds, 0.0f, 0.1f);

	const float target = WantsLeftHandOnController() ? 1.0f : 0.0f;
	const float step = kFollowSpeed * deltaSeconds;
	m_LeftHandFollow = m_LeftHandFollow < target ? std::min(m_LeftHandFollow + step, target) : std::max(m_LeftHandFollow - step, target);

	const Vec3 forward = Normalized(m_LeftHandForward);
	const Vec3 up = m_LeftHandUp;
	const Vec3 right = Cross(forward, up);

	m_LeftHandWrist = ToVec3(m_LeftControllerPosAbs)
		+ (forward * m_HandOffsetMeters.x + right * m_HandOffsetMeters.y + up * m_HandOffsetMeters.z) * m_VRScale;

	// Fingers. SteamVR gives how closed each one is, from the sensors of the controller.
	vr::VRActionHandle_t skeleton = m_LeftHanded ? m_ActionSkeletonRight : m_ActionSkeletonLeft;
	vr::VRSkeletalSummaryData_t summary;
	if (skeleton != vr::k_ulInvalidActionHandle
		&& m_Input->GetSkeletalSummaryData(skeleton, vr::VRSummaryType_FromDevice, &summary) == vr::VRInputError_None)
	{
		const float blend = std::min(kFingerSmoothing * deltaSeconds, 1.0f);
		for (int finger = 0; finger < HandRig::FingerCount; ++finger)
		{
			// A resting finger still has a natural bend
			const float wanted = std::clamp(summary.flFingerCurl[finger], 0.0f, 1.0f);
			m_LeftFingerCurls[finger] += (wanted - m_LeftFingerCurls[finger]) * blend;
		}
	}
}

// Writes the bones to draw the first person arms with: only the hands are visible, and the left one follows the
// controller. Returns false to draw the model as it is.
bool VR::BuildFirstPersonHands(const void *studioHeader, const void *customBones, void *outBones)
{
	if (!Studio::IsValid(studioHeader))
		return false;

	const int boneCount = Studio::BoneCount(studioHeader);
	if (boneCount <= 0 || boneCount > Studio::kMaxBones)
		return false;

	HandModel &model = m_HandModels[studioHeader];
	if (!model.valid)
	{
		model.rig.Build(Studio::ReadBones(studioHeader));
		model.valid = true;
		Diagnostics::Log("hands: %s has %d bones, left hand %s, right hand %s", Studio::ModelName(studioHeader), boneCount,
			model.rig.HasHand(HandRig::Left) ? "found" : "NOT FOUND", model.rig.HasHand(HandRig::Right) ? "found" : "NOT FOUND");
	}

	const HandRig &rig = model.rig;
	if (!rig.HasHand(HandRig::Left) || !rig.HasHand(HandRig::Right))
		return false;
	m_ArmsRig = &rig;

	Mat34 *bones = (Mat34 *)outBones;
	memcpy(bones, customBones, boneCount * sizeof(Mat34));

	// Melee weapons are placed on the controller (see meleeViewmodelOffsets), so the hand has to be there too. The
	// animation has the hand somewhere else on the handle for each weapon.
	// Items held in the fist go to the controller with the hand, keeping the fingers of the animation (ApplyItemInHand).
	Mat34 rightHand;
	if (m_Game->m_IsMeleeWeaponActive && GetRightHandTarget(rig, rightHand, m_MeleeHandBackMeters))
	{
		// The hand keeps the grip of the animation, with its fingers around the handle, and only slides along the handle
		// to where the hand really is
		const Vec3 grip = rig.GripAxis(HandRig::Right, bones);
		Mat34 slide;
		SetAxis(slide, 3, grip * Dot(Origin(rightHand) - Origin(bones[rig.HandBone(HandRig::Right)]), grip));
		rig.MoveHand(HandRig::Right, slide, bones);
	}
	else if (IsHeldInTheHand(m_ActiveWeaponId) && GetRightHandTarget(rig, rightHand))
		rig.MoveHand(HandRig::Right, Multiply(rightHand, InvertRigid(bones[rig.HandBone(HandRig::Right)])), bones);

	// Holding a pump action by the pump, the hand goes back and forth with it, and not with the game's own pumping
	// (see BuildReloadViewmodel)
	if (m_PumpHandShift != 0.0f && m_LeftHandFollow < 1.0f)
	{
		Mat34 move;
		SetAxis(move, 3, ToVec3(m_ViewmodelForward) * -m_PumpHandShift);
		rig.MoveHand(HandRig::Left, move, bones);
	}

	if (m_LeftHandFollow > 0.0f)
	{
		float curls[HandRig::FingerCount];
		memcpy(curls, m_LeftFingerCurls, sizeof(curls));
		if (ManualReloadIsHolding())
		{
			// Closing the hand completely would put the fingers through what it holds
			for (float &curl : curls)
				curl = std::min(curl, kHoldingMagazineCurl);
		}

		// Holding its own item (dual wield): closed around it
		const bool holdsItem = LeftHandHoldsItem();
		if (holdsItem)
			HoldLeftItem(rig, curls);

		// Hand turned by the optional fine tuning, around its own axes
		const Mat34 hand = GetLeftHandBone(rig);
		if (!IsFinite(hand))
			return false;

		if (m_LeftHandFollow >= 1.0f)
		{
			rig.PoseHand(HandRig::Left, hand, curls, bones);
			if (holdsItem)
				KeepLeftGrip(rig, bones);
		}
		else
		{
			alignas(16) unsigned char storage[sizeof(Mat34) * Studio::kMaxBones];
			Mat34 *posed = (Mat34 *)storage;
			memcpy(posed, bones, boneCount * sizeof(Mat34));
			rig.PoseHand(HandRig::Left, hand, curls, posed);
			rig.BlendHand(HandRig::Left, bones, posed, m_LeftHandFollow, bones);
		}
	}

	// Using the left hand's item: the game's weapon hand is at the left controller (ApplyLeftUseHand) and holds it
	if (m_LeftUseActive)
		rig.CollapseHand(HandRig::Left, bones);

	rig.CollapseArm(HandRig::Left, bones);
	rig.CollapseArm(HandRig::Right, bones);
	return true;
}
