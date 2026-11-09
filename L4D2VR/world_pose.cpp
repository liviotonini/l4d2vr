#include "vr.h"
#include <Windows.h>
#include <algorithm>
#include "sdk.h"
#include "game.h"
#include "studio.h"
#include "pose_relay.h"

using namespace HandMath;

namespace
{
	Vec3 ToVec3(const Vector &v) { return Vec3(v.x, v.y, v.z); }

	Mat34 FrameFromPoint(const PoseRelay::TrackedPoint &point)
	{
		const Vec3 forward = Normalized(point.forward);
		const Vec3 up = Normalized(point.up - forward * Dot(forward, point.up), Vec3(0.0f, 0.0f, 1.0f));
		return FromAxes(forward, up, Cross(forward, up));
	}
}

// The wrist of the weapon hand, where the hand holds the controller
Vec3 VR::GetRightWristAbs() const
{
	const Vector forward = VectorRotate(m_RightControllerForwardRaw, m_RightControllerRightRaw, kHandGripTilt);
	const Vector up = VectorRotate(m_RightControllerUpRaw, m_RightControllerRightRaw, kHandGripTilt);
	const Vector wrist = m_RightControllerPosAbsRaw + (forward * m_HandOffsetMeters.x + up * m_HandOffsetMeters.z) * m_VRScale;
	return ToVec3(wrist);
}

// The bones to draw another VR player with: arms following what the player sends. False to draw the model as it is.
bool VR::BuildRemotePlayerBones(int playerIndex, const void *studioHeader, const void *customBones, void *outBones)
{
	if (!m_ShowOtherPlayersMovement || !Studio::IsValid(studioHeader))
		return false;

	PoseRelay::Frame pose;
	if (!m_Game->m_PoseRelay->GetPose(playerIndex, pose))
		return false;

	const int boneCount = Studio::BoneCount(studioHeader);
	if (boneCount <= 0 || boneCount > Studio::kMaxBones)
		return false;

	HandModel &model = m_HandModels[studioHeader];
	if (!model.valid)
	{
		model.rig.Build(Studio::ReadBones(studioHeader));
		model.valid = true;
	}

	const HandRig &rig = model.rig;
	if (!rig.HasHand(HandRig::Left) || !rig.HasHand(HandRig::Right) || rig.HeadBone() < 0)
		return false;

	Mat34 *bones = (Mat34 *)outBones;
	memcpy(bones, customBones, boneCount * sizeof(Mat34));

	// The hands are placed relative to the head of the model. The player's real position in the room can be a bit
	// away from where the game has the player, and this keeps the arms attached to the body anyway.
	const Vec3 head = Origin(bones[rig.HeadBone()]);
	const Vec3 toWorld = head - pose.head.position;

	Mat34 weaponDelta;
	bool haveWeaponDelta = false;

	if (pose.hasRightHand)
	{
		// The game's animation aims the weapon where the player looks. The hand is turned from there to where it aims.
		const Mat34 turn = Multiply(FrameFromPoint(pose.rightWrist), TransposeRotation(FrameFromPoint(pose.head)));
		const Mat34 animatedHand = bones[rig.HandBone(HandRig::Right)];

		Mat34 target = Multiply(turn, animatedHand);
		SetAxis(target, 3, pose.rightWrist.position + toWorld);

		rig.SolveArm(HandRig::Right, target, bones);

		weaponDelta = Multiply(bones[rig.HandBone(HandRig::Right)], InvertRigid(animatedHand));
		haveWeaponDelta = IsFinite(weaponDelta);
		if (haveWeaponDelta)
			m_Game->m_PoseRelay->SetWeaponDelta(playerIndex, weaponDelta);
	}

	if (pose.hasLeftHand)
	{
		if (pose.leftOnWeapon && haveWeaponDelta)
		{
			// On the weapon: it stays where the weapon's animation has it, moved along with the weapon
			rig.SolveArm(HandRig::Left, Multiply(weaponDelta, bones[rig.HandBone(HandRig::Left)]), bones);
		}
		else if (!pose.leftOnWeapon)
		{
			const Mat34 target = rig.HandFromFrame(HandRig::Left, pose.leftWrist.forward, pose.leftWrist.up, pose.leftWrist.position + toWorld);
			rig.SolveArm(HandRig::Left, target, bones);

			if (pose.hasLeftCurls)
				rig.PoseHand(HandRig::Left, bones[rig.HandBone(HandRig::Left)], pose.leftCurls, bones);
		}
	}

	return true;
}

// A weapon in the hand of another VR player moves with the hand. Uses the movement of the hand from the last time that
// player was drawn; it changes smoothly, so one frame makes no visible difference.
bool VR::BuildRemoteWeaponBones(int entityIndex, const void *studioHeader, const void *customBones, void *outBones)
{
	if (!m_ShowOtherPlayersMovement || !m_Game->m_PoseRelay->HasRemotePlayers() || !Studio::IsValid(studioHeader))
		return false;

	const void *weapon = m_Game->GetClientEntity(entityIndex);
	if (!weapon)
		return false;

	const int boneCount = Studio::BoneCount(studioHeader);
	if (boneCount <= 0 || boneCount > Studio::kMaxBones)
		return false;

	for (int player = 1; player <= 32; ++player)
	{
		Mat34 delta;
		if (!m_Game->m_PoseRelay->GetWeaponDelta(player, delta))
			continue;

		C_BasePlayer *owner = (C_BasePlayer *)m_Game->GetClientEntity(player);
		if (!owner || (const void *)owner->GetActiveWeapon() != weapon)
			continue;

		const Mat34 *source = (const Mat34 *)customBones;
		Mat34 *bones = (Mat34 *)outBones;
		for (int i = 0; i < boneCount; ++i)
			bones[i] = Multiply(delta, source[i]);
		return true;
	}

	return false;
}
