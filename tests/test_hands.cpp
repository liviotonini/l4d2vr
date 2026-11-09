// Tests of the hand posing with the real skeletons of the game.
// Build and run: tests\run_tests.cmd
#include <cstdio>
#include <vector>
#include "../L4D2VR/hand_rig.cpp"
#include "test_common.h"

using namespace HandMath;

namespace
{
	struct RawBone { const char *name; int parent; float poseToBone[12]; };

	const RawBone kGambler[] = {
#include "data/arms_gambler.inc"
	};
	const RawBone kProducer[] = {
#include "data/arms_producer.inc"
	};
	const RawBone kBill[] = {
#include "data/arms_bill.inc"
	};
	const RawBone kSurvivor[] = {
#include "data/survivor_gambler.inc"
	};

	template <size_t N>
	std::vector<HandRig::BoneData> ToBones(const RawBone (&raw)[N])
	{
		std::vector<HandRig::BoneData> bones;
		for (const RawBone &b : raw)
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

	std::vector<Mat34> RestPose(const std::vector<HandRig::BoneData> &bones)
	{
		std::vector<Mat34> rest;
		for (const HandRig::BoneData &b : bones)
			rest.push_back(InvertRigid(b.poseToBone));
		return rest;
	}

	template <size_t N>
	void TestModel(const char *label, const RawBone (&raw)[N])
	{
		printf("%s\n", label);
		const std::vector<HandRig::BoneData> bones = ToBones(raw);
		HandRig rig;
		rig.Build(bones);

		CHECK(rig.HasHand(HandRig::Left));
		CHECK(rig.HasHand(HandRig::Right));

		const int leftHand = Find(bones, "ValveBiped.Bip01_L_Hand");
		const int leftMiddle = Find(bones, "ValveBiped.Bip01_L_Finger2");
		const int leftIndex = Find(bones, "ValveBiped.Bip01_L_Finger1");
		const int leftPinky = Find(bones, "ValveBiped.Bip01_L_Finger4");
		const int leftIndexTip = Find(bones, "ValveBiped.Bip01_L_Finger12");
		const int leftThumbTip = Find(bones, "ValveBiped.Bip01_L_Finger02");
		const int leftForearm = Find(bones, "ValveBiped.Bip01_L_Forearm");
		const int leftUpper = Find(bones, "ValveBiped.Bip01_L_UpperArm");
		const int rightHand = Find(bones, "ValveBiped.Bip01_R_Hand");
		const int rightForearm = Find(bones, "ValveBiped.Bip01_R_Forearm");

		std::vector<Mat34> pose = RestPose(bones);
		const std::vector<Mat34> rest = pose;

		// A frame that isn't axis aligned, in the middle of a map
		const Vec3 forward = Normalized(Vec3(0.6f, 0.7f, -0.3f));
		const Vec3 up = Normalized(Cross(Cross(forward, Vec3(0.2f, 0.1f, 1.0f)), forward));
		const Vec3 wrist(1200.0f, -340.0f, 96.0f);
		const Mat34 hand = rig.HandFromFrame(HandRig::Left, forward, up, wrist);

		CHECK(IsFinite(hand));
		CHECK_NEAR(Length(Axis(hand, 0)), 1.0f, 0.001f);
		CHECK_NEAR(Dot(Axis(hand, 0), Axis(hand, 1)), 0.0f, 0.001f);
		CHECK_NEAR(Dot(Cross(Axis(hand, 0), Axis(hand, 1)), Axis(hand, 2)), 1.0f, 0.001f); // not mirrored
		CHECK_NEAR(Length(Origin(hand) - wrist), 0.0f, 0.001f);

		float curls[5];
		for (float &curl : curls)
			curl = HandRig::kRelaxedCurl;
		rig.PoseHand(HandRig::Left, hand, curls, pose.data());

		// The fingers go where the hand was told to point, and the fingers keep their size
		const Vec3 toMiddle = Origin(pose[leftMiddle]) - wrist;
		CHECK(Dot(Normalized(toMiddle), forward) > 0.95f);
		CHECK_NEAR(Length(toMiddle), Length(Origin(rest[leftMiddle]) - Origin(rest[leftHand])), 0.01f);

		// The thumb side (index) is up
		CHECK(Dot(Normalized(Origin(pose[leftIndex]) - Origin(pose[leftPinky])), up) > 0.9f);

		// The left palm faces the right (the fingers close to that side)
		const Vec3 right = Cross(forward, up);
		float relaxed = Dot(Origin(pose[leftIndexTip]) - Origin(pose[leftIndex]), right);

		for (float &curl : curls)
			curl = 1.0f;
		rig.PoseHand(HandRig::Left, hand, curls, pose.data());
		float closed = Dot(Origin(pose[leftIndexTip]) - Origin(pose[leftIndex]), right);
		CHECK(closed > relaxed + 0.3f);
		const Vec3 closedThumb = Origin(pose[leftThumbTip]);

		for (float &curl : curls)
			curl = 0.0f;
		rig.PoseHand(HandRig::Left, hand, curls, pose.data());
		float open = Dot(Origin(pose[leftIndexTip]) - Origin(pose[leftIndex]), right);
		CHECK(open < relaxed - 0.2f);

		// The thumb moves when closing
		CHECK(Length(Origin(pose[leftThumbTip]) - closedThumb) > 0.5f);

		// A hand in the rest pose gives the rest pose back, nothing drifts
		for (float &curl : curls)
			curl = HandRig::kRelaxedCurl;
		std::vector<Mat34> again = rest;
		rig.PoseHand(HandRig::Left, rest[leftHand], curls, again.data());
		for (size_t i = 0; i < again.size(); ++i)
			CHECK_NEAR(Length(Origin(again[i]) - Origin(rest[i])), 0.0f, 0.01f);

		// Only hands: the arm is folded into the wrist
		rig.CollapseArm(HandRig::Left, pose.data());
		rig.CollapseArm(HandRig::Right, pose.data());
		CHECK_NEAR(Length(Axis(pose[leftForearm], 0)), 0.0f, 0.0001f);
		CHECK_NEAR(Length(Origin(pose[leftForearm]) - Origin(pose[leftHand])), 0.0f, 0.0001f);
		CHECK_NEAR(Length(Origin(pose[leftUpper]) - Origin(pose[leftHand])), 0.0f, 0.0001f);
		CHECK_NEAR(Length(Origin(pose[rightForearm]) - Origin(pose[rightHand])), 0.0f, 0.0001f);
		CHECK(Length(Axis(pose[leftHand], 0)) > 0.99f);   // the hand itself isn't
		CHECK(Length(Axis(pose[leftIndex], 0)) > 0.99f);

		// Blending halfway stays between both and stays a rotation
		const Mat34 halfway = Blend(rest[leftHand], hand, 0.5f);
		CHECK(IsFinite(halfway));
		CHECK_NEAR(Length(Axis(halfway, 0)), 1.0f, 0.001f);
		CHECK_NEAR(Dot(Axis(halfway, 0), Axis(halfway, 1)), 0.0f, 0.001f);

		// The right hand is mirrored: its palm faces the left
		const Mat34 rightPose = rig.HandFromFrame(HandRig::Right, forward, up, wrist);
		CHECK_NEAR(Dot(Cross(Axis(rightPose, 0), Axis(rightPose, 1)), Axis(rightPose, 2)), 1.0f, 0.001f);
	}
}

void RunHandTests()
{
	TestModel("first person arms: Nick", kGambler);
	TestModel("first person arms: Rochelle (has an extra bracelet bone)", kProducer);
	TestModel("first person arms: Bill (more helper bones)", kBill);
	TestModel("survivor model", kSurvivor);
}
