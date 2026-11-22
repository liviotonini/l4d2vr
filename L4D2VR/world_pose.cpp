#include "vr.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include "sdk.h"
#include "game.h"
#include "studio.h"
#include "pose_relay.h"
#include "diagnostics.h"

using namespace HandMath;

namespace
{
	// A weapon of the model the player holds drawn this far from where its player's animation had "weapon_bone" the last
	// time that player was drawn, turned the same way, is the one in that hand. A frame at a run is 2 units.
	const float kWeaponMatchDistance = 12.0f;
	const float kWeaponMatchAlignment = 0.9f;
	// Between the player's draw and the weapon's the player moved this much at most; more is a teleport
	const float kMaxMoveBetweenDraws = 40.0f;

	// How the weapon drawn was told to be the one in a player's hand
	enum class WeaponFound { No, AsTheWeapon, ByItsRenderable, AsItsOwner, ByPlace };

	const char *FoundText(WeaponFound how)
	{
		switch (how)
		{
		case WeaponFound::AsTheWeapon: return "drawn as the weapon they hold";
		case WeaponFound::ByItsRenderable: return "drawn with its renderable";
		case WeaponFound::AsItsOwner: return "drawn as its owner, the player";
		case WeaponFound::ByPlace: return "drawn where their animation has it";
		default: return "NOT taken as theirs";
		}
	}

	// Where the game draws an entity from (IClientRenderable::GetRenderOrigin)
	bool RenderOrigin(const void *entity, Vec3 &origin)
	{
		if (!entity)
			return false;
		__try
		{
			void *renderable = (uint8_t *)entity + 4;
			typedef const Vector &(__thiscall *tGetOrigin)(void *thisptr);
			const Vector &v = ((tGetOrigin)(*(void ***)renderable)[1])(renderable);
			origin = Vec3(v.x, v.y, v.z);
			return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

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

	// Once per player, model and outcome, for checking in the log how the game draws other players' weapons and whether
	// they follow the hands: the entity the draw is for (the weapon, the player or another), and how far its weapon bone
	// is from the one of the player's animation
	void LogRemoteWeapon(int player, const void *studioHeader, WeaponFound how, int entityIndex, const char *entityIs, float boneDistance,
		float boneAlignment, float moved)
	{
		static const void *s_Model[33] = {};
		static WeaponFound s_How[33] = {};
		if (player < 1 || player > 32 || (s_Model[player] == studioHeader && s_How[player] == how))
			return;
		s_Model[player] = studioHeader;
		s_How[player] = how;
		Diagnostics::Log("remote players: %s of player %d, %s (entity %d, %s); weapon bone %.1f units from theirs, aligned %.2f; player moved %.1f since drawn",
			Studio::ModelName(studioHeader), player, FoundText(how), entityIndex, entityIs, boneDistance, boneAlignment, moved);
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

		// What it is, to know it when the game draws it: the game can draw it as its own entity or as the player's
		const void *player = m_Game->GetClientEntity(playerIndex);
		hold.weapon = player ? (const void *)((C_BasePlayer *)player)->GetActiveWeapon() : nullptr;
		hold.worldModel = hold.weapon ? GetWorldModel(hold.weapon) : nullptr;
		hold.entityModel = hold.weapon ? GetEntityModel(hold.weapon) : nullptr;
		hold.hasPlayerOrigin = RenderOrigin(player, hold.playerOrigin);
		haveHold = IsFinite(hold.delta);

		static const void *s_LoggedHeld[33] = {};
		if (playerIndex >= 1 && playerIndex <= 32 && s_LoggedHeld[playerIndex] != hold.worldModel)
		{
			s_LoggedHeld[playerIndex] = hold.worldModel;
			const char *name = hold.worldModel ? m_Game->m_ModelInfo->GetModelName((void *)hold.worldModel) : nullptr;
			Diagnostics::Log("remote players: player %d holds %s", playerIndex, name ? name : "nothing");
		}
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
// was drawn. Left 4 Dead 2 can draw a weapon merged to a player as that player (its entity index and renderable), so
// the weapon is known by its model being the one of the weapon the player holds, drawn as that weapon or as that player,
// or, failing those, by being where the player's animation has "weapon_bone" (the weapon is merged to it). If the weapon
// is drawn before its player in a frame, the player has moved since: the weapon is moved by that too, so it doesn't
// trail behind the hand.
bool VR::BuildRemoteWeaponBones(int entityIndex, const void *model, const void *renderable, const void *studioHeader, const void *customBones, void *outBones)
{
	if (!m_ShowOtherPlayersMovement || !m_Game->m_PoseRelay->HasRemotePlayers() || !Studio::IsValid(studioHeader))
		return false;

	const int boneCount = Studio::BoneCount(studioHeader);
	if (boneCount <= 0 || boneCount > Studio::kMaxBones)
		return false;

	const void *drawn = m_Game->GetClientEntity(entityIndex);
	const Mat34 *source = (const Mat34 *)customBones;
	const int weaponBone = MergedWeaponBone(studioHeader);
	const bool hasWeaponBone = weaponBone >= 0 && weaponBone < boneCount;
	const bool drawnAsPlayer = entityIndex >= 1 && entityIndex <= 32;

	int found = -1;
	WeaponFound how = WeaponFound::No;
	PoseRelay::WeaponHold hold;
	float boneDistance = -1.0f, boneAlignment = -1.0f;
	int unmatched = -1;   // a player holding a weapon of this model, which wasn't taken as theirs (for the log)
	float unmatchedDistance = -1.0f, unmatchedAlignment = -1.0f;

	for (int player = 1; player <= 32 && found < 0; ++player)
	{
		PoseRelay::WeaponHold candidate;
		if (!m_Game->m_PoseRelay->GetWeaponHold(player, candidate) || !candidate.weapon)
			continue;

		const bool sameModel = model && (model == candidate.worldModel || model == candidate.entityModel);
		const void *playerEntity = m_Game->GetClientEntity(player);

		float distance = -1.0f, alignment = -1.0f;
		if (hasWeaponBone && candidate.hasAnimatedBone)
		{
			const Mat34 &bone = source[weaponBone];
			distance = Length(Origin(bone) - Origin(candidate.animatedBone));
			alignment = std::min(Dot(Axis(bone, 1), Axis(candidate.animatedBone, 1)), Dot(Axis(bone, 2), Axis(candidate.animatedBone, 2)));
		}

		WeaponFound candidateHow = WeaponFound::No;
		if (drawn && drawn == candidate.weapon)
			candidateHow = WeaponFound::AsTheWeapon;
		else if (renderable && renderable == (const uint8_t *)candidate.weapon + 4)
			candidateHow = WeaponFound::ByItsRenderable;
		else if (sameModel && (entityIndex == player || (renderable && playerEntity && renderable == (const uint8_t *)playerEntity + 4)))
			candidateHow = WeaponFound::AsItsOwner;
		else if (sameModel && (drawnAsPlayer || !drawn || HasOwnerEntity(drawn)) && distance >= 0.0f && distance < kWeaponMatchDistance
			&& alignment > kWeaponMatchAlignment)
			candidateHow = WeaponFound::ByPlace; // a weapon lying in the world isn't in anyone's hand, wherever it is

		if (candidateHow != WeaponFound::No)
		{
			found = player;
			how = candidateHow;
			hold = candidate;
			boneDistance = distance;
			boneAlignment = alignment;
		}
		else if (sameModel && unmatched < 0)
		{
			unmatched = player;
			unmatchedDistance = distance;
			unmatchedAlignment = alignment;
		}
	}

	const char *entityIs = !drawn ? "no entity" : drawnAsPlayer ? "a player" : HasOwnerEntity(drawn) ? "carried" : "not carried";

	if (found < 0)
	{
		if (unmatched >= 0)
			LogRemoteWeapon(unmatched, studioHeader, WeaponFound::No, entityIndex, entityIs, unmatchedDistance, unmatchedAlignment, 0.0f);

		// Not in a hand: pills or a grenade they carry go on the chest
		return BuildRemoteItemBones(entityIndex, drawn, model, studioHeader, outBones);
	}

	// The delta takes the hand from where the animation had it to where the mod put it, both where the player was when
	// drawn. Where the player is now: the same move, shifted by how far the player went.
	Mat34 delta = hold.delta;
	float moved = 0.0f;
	Vec3 now;
	if (hold.hasPlayerOrigin && RenderOrigin(m_Game->GetClientEntity(found), now))
	{
		const Vec3 shift = now - hold.playerOrigin;
		moved = Length(shift);
		if (moved > 0.001f && moved < kMaxMoveBetweenDraws)
			SetAxis(delta, 3, Origin(delta) + shift - RotateVector(delta, shift));
	}

	Mat34 *bones = (Mat34 *)outBones;
	for (int i = 0; i < boneCount; ++i)
		bones[i] = Multiply(delta, source[i]);

	LogRemoteWeapon(found, studioHeader, how, entityIndex, entityIs, boneDistance, boneAlignment, moved);
	return true;
}
