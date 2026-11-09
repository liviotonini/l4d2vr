#include <cstdlib>
#include <string>
#include <vector>
#include "../L4D2VR/vr_pose_protocol.h"
#include "../L4D2VR/hand_rig.h"
#include "test_common.h"

using namespace HandMath;

namespace
{
	struct RawBone { const char *name; int parent; float poseToBone[12]; };

	const RawBone kSurvivor[] = {
#include "data/survivor_gambler.inc"
	};

	std::vector<HandRig::BoneData> SurvivorBones()
	{
		std::vector<HandRig::BoneData> bones;
		for (const RawBone &b : kSurvivor)
		{
			HandRig::BoneData bone;
			bone.name = b.name;
			bone.parent = b.parent;
			memcpy(&bone.poseToBone, b.poseToBone, sizeof(bone.poseToBone));
			bones.push_back(bone);
		}
		return bones;
	}

	int Find(const std::vector<HandRig::BoneData> &bones, const char *name)
	{
		for (size_t i = 0; i < bones.size(); ++i)
			if (bones[i].name == name)
				return (int)i;
		return -1;
	}

	void TestProtocol()
	{
		printf("pose packets\n");
		using namespace l4d2vr_pose;

		WirePacket packet;
		packet.versionAndValidMask = (uint8_t)((kVersion << 4) | kValidMask);
		packet.featureMask = kFeatureBodyYaw | kFeatureLeftFingerCurls;
		packet.handStateFlags = kHandStateRightNativeFingerAnimation;
		packet.bodyYaw = 12345;
		packet.hmd.position[0] = 100;
		packet.hmd.rotation[1] = -2000;
		packet.leftHand.position[2] = -321;
		packet.rightHand.rotation[0] = 32000;
		packet.leftFingerCurls[3] = 200;
		FinalizePacket(packet);

		std::string payload;
		CHECK(EncodePayload(packet, payload));
		CHECK(payload.size() == kEncodedPayloadChars);
		// Goes through a console command: nothing that would split it or need quotes
		CHECK(payload.find_first_of(" \";'\n") == std::string::npos);

		WirePacket decoded;
		CHECK(DecodePayload(payload.c_str(), decoded));
		CHECK(memcmp(&packet, &decoded, sizeof(packet)) == 0);

		// Damaged in any position it's rejected
		for (size_t i = 0; i < payload.size(); ++i)
		{
			std::string bad = payload;
			bad[i] = bad[i] == 'A' ? 'B' : 'A';
			WirePacket ignored;
			CHECK(!DecodePayload(bad.c_str(), ignored));
		}

		// Random input never decodes and never crashes
		srand(1234);
		int accepted = 0;
		for (int round = 0; round < 3000; ++round)
		{
			std::string junk;
			const size_t length = round % 2 ? kEncodedPayloadChars : (size_t)(rand() % 120);
			for (size_t i = 0; i < length; ++i)
				junk.push_back((char)(32 + rand() % 90));
			WirePacket ignored;
			if (DecodePayload(junk.c_str(), ignored))
				++accepted;
		}
		CHECK(accepted == 0);
		CHECK(!DecodePayload("", decoded));
		CHECK(!DecodePayload(nullptr, decoded));

		CHECK(IsSequenceNewer(5, 4));
		CHECK(!IsSequenceNewer(4, 5));
		CHECK(IsSequenceNewer(2, 65530)); // wraps around
	}

	void TestArmSolver()
	{
		printf("arm solver\n");
		const std::vector<HandRig::BoneData> bones = SurvivorBones();
		HandRig rig;
		rig.Build(bones);
		CHECK(rig.HeadBone() >= 0);

		std::vector<Mat34> rest;
		for (const HandRig::BoneData &b : bones)
			rest.push_back(InvertRigid(b.poseToBone));

		for (HandRig::Side side : { HandRig::Left, HandRig::Right })
		{
			const int hand = rig.HandBone(side);
			const int forearm = rig.ForearmBone(side);
			const int upper = rig.UpperArmBone(side);
			const char *fingerName = side == HandRig::Left ? "ValveBiped.Bip01_L_Finger12" : "ValveBiped.Bip01_R_Finger12";
			const int fingertip = Find(bones, fingerName);

			const float upperLength = Length(Origin(rest[forearm]) - Origin(rest[upper]));
			const float lowerLength = Length(Origin(rest[hand]) - Origin(rest[forearm]));
			const float fingerDistance = Length(Origin(rest[fingertip]) - Origin(rest[hand]));
			const Vec3 shoulder = Origin(rest[upper]);

			const float sideSign = side == HandRig::Left ? 1.0f : -1.0f;
			const Vec3 targets[] =
			{
				shoulder + Vec3(0.0f, -12.0f, 2.0f),                             // in front
				shoulder + Vec3(sideSign * 5.0f, -10.0f, -14.0f),                // low
				shoulder + Vec3(sideSign * -2.0f, -9.0f, 12.0f),                 // above the shoulder
				shoulder + Vec3(0.0f, -(upperLength + lowerLength) * 0.99f, 0.0f), // almost straight
			};

			for (const Vec3 &target : targets)
			{
				std::vector<Mat34> pose = rest;
				const Mat34 wanted = rig.HandFromFrame(side, Vec3(0.0f, -1.0f, 0.0f), Vec3(0.0f, 0.0f, 1.0f), target);
				rig.SolveArm(side, wanted, pose.data());

				// It got there, and the shoulder didn't move
				CHECK_NEAR(Length(Origin(pose[hand]) - target), 0.0f, 0.05f);
				CHECK_NEAR(Length(Origin(pose[upper]) - shoulder), 0.0f, 0.001f);

				// The bones didn't stretch
				CHECK_NEAR(Length(Origin(pose[forearm]) - Origin(pose[upper])), upperLength, 0.05f);
				CHECK_NEAR(Length(Origin(pose[hand]) - Origin(pose[forearm])), lowerLength, 0.05f);
				CHECK_NEAR(Length(Origin(pose[fingertip]) - Origin(pose[hand])), fingerDistance, 0.05f);

				// The hand has the orientation it was told to have
				CHECK(Dot(Axis(pose[hand], 0), Axis(wanted, 0)) > 0.999f);

				// Still proper rotations
				CHECK_NEAR(Dot(Cross(Axis(pose[upper], 0), Axis(pose[upper], 1)), Axis(pose[upper], 2)), 1.0f, 0.002f);
				CHECK_NEAR(Dot(Cross(Axis(pose[forearm], 0), Axis(pose[forearm], 1)), Axis(pose[forearm], 2)), 1.0f, 0.002f);
			}

			// Out of reach: the arm is stretched towards it, not broken
			std::vector<Mat34> pose = rest;
			const Vec3 farAway = shoulder + Vec3(0.0f, -500.0f, 0.0f);
			rig.SolveArm(side, rig.HandFromFrame(side, Vec3(0.0f, -1.0f, 0.0f), Vec3(0.0f, 0.0f, 1.0f), farAway), pose.data());
			CHECK(IsFinite(pose[hand]));
			CHECK_NEAR(Length(Origin(pose[hand]) - shoulder), (upperLength + lowerLength) * 0.999f, 0.1f);

			// Too close: also fine
			pose = rest;
			rig.SolveArm(side, rig.HandFromFrame(side, Vec3(0.0f, -1.0f, 0.0f), Vec3(0.0f, 0.0f, 1.0f), shoulder), pose.data());
			CHECK(IsFinite(pose[hand]));
			CHECK(IsFinite(pose[forearm]));
		}
	}
}

void RunPoseTests()
{
	TestProtocol();
	TestArmSolver();
}
