#include "vr.h"
#include <Windows.h>
#include <algorithm>
#include <unordered_map>
#include "sdk.h"
#include "game.h"
#include "studio.h"
#include "pose_relay.h"
#include "diagnostics.h"

using namespace HandMath;

namespace
{
	// A weapon drawn this far from where its player's animation had "weapon_bone" the last time that player was drawn,
	// turned the same way, is the one in that hand. The player moves between the two draws: a frame at a run is 2 units.
	const float kWeaponMatchDistance = 12.0f;
	const float kWeaponMatchAlignment = 0.9f;

	Vec3 ToVec3(const Vector &v) { return Vec3(v.x, v.y, v.z); }

	Mat34 FrameFromPoint(const PoseRelay::TrackedPoint &point)
	{
		const Vec3 forward = Normalized(point.forward);
		const Vec3 up = Normalized(point.up - forward * Dot(forward, point.up), Vec3(0.0f, 0.0f, 1.0f));
		return FromAxes(forward, up, Cross(forward, up));
	}

	// Where a pitch and a yaw look, without roll: how the game's animation aims the weapon
	Mat34 AimFrame(const Vec3 &forwardIn)
	{
		const Vec3 forward = Normalized(forwardIn);
		const Vec3 world(0.0f, 0.0f, 1.0f);
		const Vec3 up = Normalized(world - forward * Dot(forward, world), Vec3(1.0f, 0.0f, 0.0f));
		return FromAxes(forward, up, Cross(forward, up));
	}

	// The bone a weapon's world model is merged to the hand by, -1 if it has none
	int MergedWeaponBone(const void *studioHeader)
	{
		static std::unordered_map<const void *, int> s_Bones;
		auto found = s_Bones.find(studioHeader);
		if (found != s_Bones.end())
			return found->second;

		const int bone = Studio::FindBone(studioHeader, "ValveBiped.weapon_bone");
		s_Bones[studioHeader] = bone;
		return bone;
	}

	// Once per player and model, for checking in the log that other players' weapons follow their hands
	void LogRemoteWeapon(int player, const void *studioHeader, bool byEntity, bool moved)
	{
		static const void *s_Logged[33] = {};
		if (player < 1 || player > 32 || s_Logged[player] == studioHeader)
			return;
		s_Logged[player] = studioHeader;
		Diagnostics::Log("remote players: %s follows player %d's hand (found %s%s)", Studio::ModelName(studioHeader), player,
			byEntity ? "as the weapon they hold" : "where the game draws it in their hand", moved ? ", moved with them since they were drawn" : "");
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
		model.weaponBone = Studio::FindBone(studioHeader, "ValveBiped.weapon_bone");
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
	if (playerIndex > 0 && playerIndex <= 32)
	{
		// For what the player carries on the chest (BuildRemoteItemBones)
		m_RemoteHeadOffset[playerIndex] = toWorld;
		m_RemoteHeadOffsetValid[playerIndex] = true;
	}

	PoseRelay::WeaponHold hold;
	bool haveHold = false;

	if (pose.hasRightHand)
	{
		// The game's animation aims the weapon where the player's eyes look, as the game has them (pitch and yaw, no
		// roll). The hand is turned from there to where the controller aims. Without the eyes, where the head looks.
		float pitch = 0.0f, yaw = 0.0f;
		Mat34 aim;
		if (GetPlayerEyeAngles(m_Game->GetClientEntity(playerIndex), pitch, yaw))
		{
			Vector eyeForward;
			QAngle::AngleVectors(QAngle(pitch, yaw, 0.0f), &eyeForward, nullptr, nullptr);
			aim = AimFrame(ToVec3(eyeForward));
		}
		else
		{
			aim = AimFrame(pose.head.forward);
		}

		const Mat34 turn = Multiply(FrameFromPoint(pose.rightWrist), TransposeRotation(aim));
		const Mat34 animatedHand = bones[rig.HandBone(HandRig::Right)];

		Mat34 target = Multiply(turn, animatedHand);
		SetAxis(target, 3, pose.rightWrist.position + toWorld);

		rig.SolveArm(HandRig::Right, target, bones);

		// The weapon is merged to the animation's "weapon_bone", in the hand: it goes along with the hand
		hold.delta = Multiply(bones[rig.HandBone(HandRig::Right)], InvertRigid(animatedHand));
		if (model.weaponBone >= 0 && model.weaponBone < boneCount)
		{
			hold.animatedBone = ((const Mat34 *)customBones)[model.weaponBone];
			hold.hasAnimatedBone = true;
		}
		haveHold = IsFinite(hold.delta);
	}
	if (haveHold)
		m_Game->m_PoseRelay->SetWeaponHold(playerIndex, hold);
	else
		m_Game->m_PoseRelay->ClearWeaponHold(playerIndex);

	if (pose.hasLeftHand)
	{
		if (pose.leftOnWeapon && haveHold)
		{
			// On the weapon: it stays where the weapon's animation has it, moved along with the weapon
			rig.SolveArm(HandRig::Left, Multiply(hold.delta, bones[rig.HandBone(HandRig::Left)]), bones);
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

// A weapon in the hand of another VR player moves with the hand, by how the hand was moved the last time that player
// was drawn. It's found as the weapon the player holds, or, whatever entity the game draws it for, by being where the
// player's animation has "weapon_bone" (the weapon is merged to it). If the weapon is drawn before its player in a
// frame, the player has moved since: the weapon is moved by that too, so it doesn't trail behind the hand.
bool VR::BuildRemoteWeaponBones(int entityIndex, const void *studioHeader, const void *customBones, void *outBones)
{
	if (!m_ShowOtherPlayersMovement || !m_Game->m_PoseRelay->HasRemotePlayers() || !Studio::IsValid(studioHeader))
		return false;

	const int boneCount = Studio::BoneCount(studioHeader);
	if (boneCount <= 0 || boneCount > Studio::kMaxBones)
		return false;

	const void *weapon = m_Game->GetClientEntity(entityIndex);
	const Mat34 *source = (const Mat34 *)customBones;
	const int weaponBone = MergedWeaponBone(studioHeader);
	// A weapon lying in the world isn't in anyone's hand, wherever it is
	const bool carried = !weapon || (entityIndex >= 1 && entityIndex <= 32) || HasOwnerEntity(weapon);
	const bool hasWeaponBone = carried && weaponBone >= 0 && weaponBone < boneCount;

	int found = -1;
	bool byEntity = false;
	float distance = kWeaponMatchDistance;
	PoseRelay::WeaponHold hold;

	for (int player = 1; player <= 32; ++player)
	{
		PoseRelay::WeaponHold candidate;
		if (!m_Game->m_PoseRelay->GetWeaponHold(player, candidate))
			continue;

		float away = 1e9f;
		if (hasWeaponBone && candidate.hasAnimatedBone)
		{
			const Mat34 &drawn = source[weaponBone];
			if (Dot(Axis(drawn, 1), Axis(candidate.animatedBone, 1)) > kWeaponMatchAlignment
				&& Dot(Axis(drawn, 2), Axis(candidate.animatedBone, 2)) > kWeaponMatchAlignment)
				away = Length(Origin(drawn) - Origin(candidate.animatedBone));
		}

		C_BasePlayer *owner = weapon ? (C_BasePlayer *)m_Game->GetClientEntity(player) : nullptr;
		if (owner && (const void *)owner->GetActiveWeapon() == weapon)
		{
			found = player;
			byEntity = true;
			distance = away;
			hold = candidate;
			break;
		}

		if (away < distance)
		{
			found = player;
			distance = away;
			hold = candidate;
		}
	}

	if (found < 0)
	{
		// Not in a hand: pills or a grenade they carry go on the chest
		return weapon ? BuildRemoteItemBones(entityIndex, weapon, studioHeader, outBones) : false;
	}

	Mat34 delta = hold.delta;
	bool moved = false;
	if (distance < kWeaponMatchDistance)
	{
		// The delta takes the hand from where the animation had it to where the mod put it, both where the player was.
		// Where the player is now: the same move, shifted by how far the animation's weapon bone went.
		const Vec3 shift = Origin(source[weaponBone]) - Origin(hold.animatedBone);
		SetAxis(delta, 3, Origin(delta) + shift - RotateVector(delta, shift));
		moved = Length(shift) > 0.01f;
	}

	Mat34 *bones = (Mat34 *)outBones;
	for (int i = 0; i < boneCount; ++i)
		bones[i] = Multiply(delta, source[i]);

	LogRemoteWeapon(found, studioHeader, byEntity, moved);
	return true;
}
