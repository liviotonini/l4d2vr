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
	// The middle of the fist from the knuckles, towards the palm (as the left hand's, dual_wield.cpp)
	const float kRightGripPalmMeters = 0.022f;
	// Holding the weapon with both hands starts with the left wrist this close to where the animation has it, if the
	// animation has it on the weapon (closer than this to the right hand)
	const float kTwoHandGrabMeters = 0.12f;
	const float kTwoHandSpanMeters = 0.6f;

	// The pose of the weapon and the hand before the throw started, relative to the weapon's frame, per model
	std::unordered_map<const void *, std::vector<Mat34>> g_ThrowPoses;
	// The right hand bone of each weapon model in first person (-1 if it has none)
	std::unordered_map<const void *, int> g_ViewmodelRightHands;
	// The bones of the left pistol in the dual pistols model
	std::unordered_map<const void *, std::vector<int>> g_SecondPistolBones;
	// The syringe of the adrenaline's first person model ("weapon_clip"), -1 if none
	std::unordered_map<const void *, int> g_ViewmodelSyringes;
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
	g_ViewmodelSyringes.clear();
	ClearBodyModels();
	Hooks::m_ModelKinds.clear();
}

// Items held in the fist instead of by a grip: the game's animations hold them away from where the pistol's grip is,
// so the hand and the item are put on the controller instead
bool VR::IsHeldInTheHand(int weaponId)
{
	return IsThrowableWeapon(weaponId) || weaponId == C_WeaponCSBase::PAIN_PILLS || weaponId == C_WeaponCSBase::ADRENALINE || weaponId == C_WeaponCSBase::FIRST_AID_KIT;
}

// The right hand bone where the controller is: the same place and angle as the left hand on its controller. While the
// left hand's item is used the right controller's poses are the left one's (ApplyLeftUseHand): its own are used.
bool VR::GetRightHandTarget(const HandRig &rig, Mat34 &hand, float backMeters) const
{
	if (m_RightControllerDeviceIndex == vr::k_unTrackedDeviceIndexInvalid || !m_Poses[m_RightControllerDeviceIndex].bPoseIsValid)
		return false;

	const bool own = m_LeftUseActive;
	const Vector &rightRaw = own ? m_RightRealRightRaw : m_RightControllerRightRaw;
	const Vector forward = VectorRotate(own ? m_RightRealForwardRaw : m_RightControllerForwardRaw, rightRaw, kHandGripTilt);
	const Vector up = VectorRotate(own ? m_RightRealUpRaw : m_RightControllerUpRaw, rightRaw, kHandGripTilt);
	const Vector wrist = (own ? m_RightRealPosAbs : m_RightControllerPosAbs) + (forward * (m_HandOffsetMeters.x - backMeters) + up * m_HandOffsetMeters.z) * m_VRScale;

	hand = rig.HandFromFrame(HandRig::Right, ToVec3(forward), ToVec3(up), ToVec3(wrist));
	return IsFinite(hand);
}

// The middle of the fist of a hand, where it holds things, in the world. The right one as it was last drawn, holding the
// weapon; otherwise from the controller, like the left hand is drawn on it: along the fingers from the wrist and a bit
// towards the palm.
Vec3 VR::GetFistPoint(bool weaponHand) const
{
	const unsigned long long now = GetTickCount64();
	if (weaponHand && now - m_RightGripMs < 250)
		return Origin(m_RightGripFrame);

	const float fingersMeters = 0.075f, palmMeters = 0.025f;
	if (!weaponHand)
	{
		const Vec3 fingers = Normalized(m_LeftHandForward);
		const Vec3 palm = Cross(fingers, m_LeftHandUp);
		return m_LeftHandWrist + (fingers * fingersMeters + palm * palmMeters) * m_VRScale;
	}

	const bool own = m_LeftUseActive;
	const Vector &rightRaw = own ? m_RightRealRightRaw : m_RightControllerRightRaw;
	const Vec3 fingers = Normalized(ToVec3(VectorRotate(own ? m_RightRealForwardRaw : m_RightControllerForwardRaw, rightRaw, kHandGripTilt)));
	const Vec3 up = ToVec3(VectorRotate(own ? m_RightRealUpRaw : m_RightControllerUpRaw, rightRaw, kHandGripTilt));
	const Vec3 wrist = ToVec3(own ? m_RightRealPosAbs : m_RightControllerPosAbs) + (fingers * m_HandOffsetMeters.x + up * m_HandOffsetMeters.z) * m_VRScale;
	return wrist + (fingers * fingersMeters + Cross(up, fingers) * palmMeters) * m_VRScale;
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

	// The adrenaline's needle, to stab it into the body (ItemUseGestureReady): the syringe is the "weapon_clip" bone, the
	// needle at its end along -y
	if (m_ActiveWeaponId == C_WeaponCSBase::ADRENALINE)
	{
		auto syringe = g_ViewmodelSyringes.find(studioHeader);
		if (syringe == g_ViewmodelSyringes.end())
			syringe = g_ViewmodelSyringes.emplace(studioHeader, Studio::FindBone(studioHeader, "ValveBiped.weapon_clip")).first;
		if (syringe->second >= 0 && syringe->second < boneCount)
		{
			const Mat34 &clip = bones[syringe->second];
			m_RightNeedleDir = Normalized(Axis(clip, 1)) * -1.0f;
			m_RightNeedleTip = Origin(clip) + m_RightNeedleDir * kSyringeNeedleUnits;
			m_RightNeedleMs = GetTickCount64();
		}
	}
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

	// Holding its own item (dual wield), and using it: still there once a grenade has left it
	if (LeftHandShowsItem() || m_LeftUseActive)
		return true;

	if (isHoldingWeapon2Handed || m_ViewmodelHasTwoWeapons || NeedsBothHands(m_ActiveWeaponId))
		return false;

	return !IsWeaponReloading();
}

// The left hand is where the weapon's animation holds the weapon with it (BuildFirstPersonHands), which is where the
// hand goes once the weapon is held with both hands. `known` is false when the animation has no left hand on the weapon.
bool VR::IsAtTwoHandSpot(bool &known) const
{
	known = false;
	if (GetTickCount64() - m_AnimatedHandsMs > 250)
		return false;
	const Vec3 &spot = m_AnimatedHand[HandRig::Left];
	if (Length(spot - m_AnimatedHand[HandRig::Right]) > kTwoHandSpanMeters * m_VRScale)
		return false;
	known = true;
	return Length(m_LeftHandWrist - spot) < kTwoHandGrabMeters * m_VRScale;
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
	const float blend = std::min(kFingerSmoothing * deltaSeconds, 1.0f);
	auto readFingers = [&](vr::VRActionHandle_t skeleton, float curls[HandRig::FingerCount])
	{
		vr::VRSkeletalSummaryData_t summary;
		if (skeleton == vr::k_ulInvalidActionHandle || m_Input->GetSkeletalSummaryData(skeleton, vr::VRSummaryType_FromDevice, &summary) != vr::VRInputError_None)
			return;
		for (int finger = 0; finger < HandRig::FingerCount; ++finger)
		{
			// A resting finger still has a natural bend
			const float wanted = std::clamp(summary.flFingerCurl[finger], 0.0f, 1.0f);
			curls[finger] += (wanted - curls[finger]) * blend;
		}
	};
	readFingers(m_LeftHanded ? m_ActionSkeletonRight : m_ActionSkeletonLeft, m_LeftFingerCurls);
	// The right hand's own fingers are only shown while it's empty, as the left hand's item is used
	if (m_LeftUseActive)
		readFingers(m_LeftHanded ? m_ActionSkeletonLeft : m_ActionSkeletonRight, m_RightFingerCurls);
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

	// Where the weapon's animation has the hands: the left one on the weapon is where it holds it with both hands
	// (IsAtTwoHandSpot)
	m_AnimatedHand[HandRig::Left] = Origin(bones[rig.HandBone(HandRig::Left)]);
	m_AnimatedHand[HandRig::Right] = Origin(bones[rig.HandBone(HandRig::Right)]);
	m_AnimatedHandsMs = GetTickCount64();

	// Melee weapons are placed on the controller (see meleeViewmodelOffsets), so the hand has to be there too. The
	// animation has the hand somewhere else on the handle for each weapon.
	// Items held in the fist go to the controller with the hand, keeping the fingers of the animation (ApplyItemInHand).
	// While the left hand's item is used, the game's weapon hand is at the left controller (ApplyLeftUseHand) and the
	// weapon isn't drawn (Hooks::DrawModelForVR): the left hand holds the item as before, and the right hand is on its own
	// controller, empty.
	Mat34 rightHand;
	if (m_LeftUseActive)
	{
		if (GetRightHandTarget(rig, rightHand))
			rig.PoseHand(HandRig::Right, rightHand, m_RightFingerCurls, bones);
	}
	else if (m_Game->m_IsMeleeWeaponActive && GetRightHandTarget(rig, rightHand, m_MeleeHandBackMeters))
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
		const bool holdsItem = LeftHandShowsItem();
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

	// The right fist, to take what's on the body where it's seen (GetFistPoint)
	const Mat34 rightGrip = rig.GripFrame(HandRig::Right, bones, kRightGripPalmMeters * m_VRScale);
	if (IsFinite(rightGrip))
	{
		m_RightGripFrame = rightGrip;
		m_RightGripMs = GetTickCount64();
	}

	rig.CollapseArm(HandRig::Left, bones);
	rig.CollapseArm(HandRig::Right, bones);
	return true;
}
