#include "vr.h"
#include <Windows.h>
#include <algorithm>
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

	// DT_BaseCombatWeapon::m_bInReload on the client
	const int kClientWeaponInReloadOffset = 0x9BD;

	Vec3 ToVec3(const Vector &v) { return Vec3(v.x, v.y, v.z); }

	// Weapons held with both hands the whole time
	bool NeedsBothHands(int weaponId)
	{
		switch (weaponId)
		{
		case C_WeaponCSBase::FIRST_AID_KIT:
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
	Hooks::m_ModelKinds.clear();
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

	if (isHoldingWeapon2Handed || m_ViewmodelHasTwoWeapons || NeedsBothHands(m_ActiveWeaponId))
		return false;

	return !IsWeaponReloading();
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

	Mat34 *bones = (Mat34 *)outBones;
	memcpy(bones, customBones, boneCount * sizeof(Mat34));

	if (m_LeftHandFollow > 0.0f)
	{
		// Hand turned by the optional fine tuning, around its own axes
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

		const Mat34 hand = rig.HandFromFrame(HandRig::Left, forward, up, m_LeftHandWrist);
		if (!IsFinite(hand))
			return false;

		if (m_LeftHandFollow >= 1.0f)
		{
			rig.PoseHand(HandRig::Left, hand, m_LeftFingerCurls, bones);
		}
		else
		{
			alignas(16) unsigned char storage[sizeof(Mat34) * Studio::kMaxBones];
			Mat34 *posed = (Mat34 *)storage;
			memcpy(posed, bones, boneCount * sizeof(Mat34));
			rig.PoseHand(HandRig::Left, hand, m_LeftFingerCurls, posed);
			rig.BlendHand(HandRig::Left, (const Mat34 *)customBones, posed, m_LeftHandFollow, bones);
		}
	}

	rig.CollapseArm(HandRig::Left, bones);
	rig.CollapseArm(HandRig::Right, bones);
	return true;
}
