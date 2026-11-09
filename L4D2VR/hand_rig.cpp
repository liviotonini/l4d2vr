#include "hand_rig.h"
#include <algorithm>
#include <cctype>

using namespace HandMath;

namespace
{
	const float kPi = 3.14159265f;

	// How far each finger joint turns between open and closed, degrees
	const float kFingerCurlRange[3] = { 70.0f, 90.0f, 55.0f };
	const float kThumbCurlRange[3] = { 35.0f, 45.0f, 45.0f };

	std::string ToLower(std::string text)
	{
		std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)tolower(c); });
		return text;
	}

	int FindBone(const std::vector<std::string> &names, const std::string &name)
	{
		auto found = std::find(names.begin(), names.end(), name);
		return found == names.end() ? -1 : (int)(found - names.begin());
	}

	bool IsDescendant(const std::vector<HandRig::BoneData> &bones, int bone, int ancestor)
	{
		for (int current = bone; current >= 0; current = bones[current].parent)
		{
			if (current == ancestor)
				return true;
		}
		return false;
	}

	Vec3 RestPosition(const std::vector<Mat34> &rest, int bone)
	{
		return Origin(rest[bone]);
	}
}

void HandRig::Build(const std::vector<BoneData> &bones)
{
	std::vector<std::string> names;
	std::vector<Mat34> rest;
	names.reserve(bones.size());
	rest.reserve(bones.size());
	for (const BoneData &bone : bones)
	{
		names.push_back(ToLower(bone.name));
		rest.push_back(InvertRigid(bone.poseToBone));
	}

	const char *const sideLetters[SideCount] = { "l", "r" };

	for (int s = 0; s < SideCount; ++s)
	{
		SideData &side = m_Sides[s];
		side = SideData();

		const std::string prefix = std::string("valvebiped.bip01_") + sideLetters[s] + "_";
		const int hand = FindBone(names, prefix + "hand");
		const int forearm = FindBone(names, prefix + "forearm");
		const int upperArm = FindBone(names, prefix + "upperarm");
		const int clavicle = FindBone(names, prefix + "clavicle");
		const int indexBase = FindBone(names, prefix + "finger1");
		const int middleBase = FindBone(names, prefix + "finger2");
		const int pinkyBase = FindBone(names, prefix + "finger4");
		if (hand < 0 || forearm < 0 || upperArm < 0 || indexBase < 0 || middleBase < 0 || pinkyBase < 0)
			continue;

		// The hand's own directions in the rest pose, from where the fingers are
		const Vec3 fingers = Normalized(RestPosition(rest, middleBase) - RestPosition(rest, hand));
		Vec3 thumbSide = RestPosition(rest, indexBase) - RestPosition(rest, pinkyBase);
		thumbSide = Normalized(thumbSide - fingers * Dot(fingers, thumbSide), Vec3(0.0f, 1.0f, 0.0f));
		const Vec3 palm = s == Left ? Cross(fingers, thumbSide) : Cross(thumbSide, fingers);

		side.hand = hand;
		side.forearm = forearm;
		side.upperArm = upperArm;
		side.restHand = rest[hand];
		side.restFrame = FromAxes(fingers, thumbSide, palm);

		// Turning about this axis moves the fingers towards the palm
		const Vec3 fingerCurlAxis = Cross(fingers, palm);

		const int thumbBase = FindBone(names, prefix + "finger0");

		const int rootOfArm = clavicle >= 0 ? clavicle : upperArm;
		for (int b = 0; b < (int)bones.size(); ++b)
		{
			if (b == hand)
				continue;

			if (IsDescendant(bones, b, hand))
			{
				Joint joint;
				joint.bone = b;
				joint.parent = bones[b].parent;
				joint.local = Multiply(InvertRigid(rest[joint.parent]), rest[b]);
				joint.finger = -1;
				joint.segment = 0;

				// "finger2" is the base of the middle finger, "finger21" and "finger22" its other joints
				const std::string fingerPrefix = prefix + "finger";
				if (names[b].compare(0, fingerPrefix.size(), fingerPrefix) == 0)
				{
					const std::string number = names[b].substr(fingerPrefix.size());
					if ((number.size() == 1 || number.size() == 2) && isdigit((unsigned char)number[0]) && number[0] <= '4')
					{
						joint.finger = number[0] - '0';
						joint.segment = number.size() == 1 ? 0 : std::clamp(number[1] - '0', 0, 2);
					}
				}

				if (joint.finger >= 0)
				{
					Vec3 axis = fingerCurlAxis;
					if (joint.finger == Thumb && thumbBase >= 0)
					{
						// The thumb closes across the palm, towards the base of the little finger
						const Vec3 direction = Axis(rest[b], 0);
						axis = Normalized(Cross(direction, RestPosition(rest, pinkyBase) - RestPosition(rest, b)), fingerCurlAxis);
					}
					joint.curlAxis = RotateVector(TransposeRotation(rest[b]), axis);
				}

				side.joints.push_back(joint);
			}
			else if (IsDescendant(bones, b, rootOfArm))
			{
				side.arm.push_back(b);
			}
		}
	}
}

void HandRig::CollapseArm(Side s, Mat34 *bones) const
{
	const SideData &side = m_Sides[s];
	if (side.hand < 0)
		return;

	const Vec3 wrist = Origin(bones[side.hand]);
	for (int bone : side.arm)
		bones[bone] = CollapsedAt(wrist);
}

Mat34 HandRig::HandFromFrame(Side s, const Vec3 &forward, const Vec3 &up, const Vec3 &wrist) const
{
	const SideData &side = m_Sides[s];

	const Vec3 fingers = Normalized(forward);
	const Vec3 thumbSide = Normalized(up - fingers * Dot(fingers, up), Vec3(0.0f, 0.0f, 1.0f));
	const Vec3 palm = s == Left ? Cross(fingers, thumbSide) : Cross(thumbSide, fingers);

	Mat34 hand = Multiply(Multiply(FromAxes(fingers, thumbSide, palm), TransposeRotation(side.restFrame)), side.restHand);
	SetAxis(hand, 3, wrist);
	return hand;
}

void HandRig::PoseHand(Side s, const Mat34 &hand, const float curls[FingerCount], Mat34 *bones) const
{
	const SideData &side = m_Sides[s];
	if (side.hand < 0)
		return;

	bones[side.hand] = hand;

	for (const Joint &joint : side.joints)
	{
		if (joint.finger >= 0)
		{
			const float *range = joint.finger == Thumb ? kThumbCurlRange : kFingerCurlRange;
			const float degrees = (curls[joint.finger] - kRelaxedCurl) * range[joint.segment];
			const Mat34 bent = Multiply(joint.local, AxisAngle(joint.curlAxis, degrees * kPi / 180.0f));
			bones[joint.bone] = Multiply(bones[joint.parent], bent);
		}
		else
		{
			bones[joint.bone] = Multiply(bones[joint.parent], joint.local);
		}
	}
}

void HandRig::BlendHand(Side s, const Mat34 *from, const Mat34 *to, float amount, Mat34 *bones) const
{
	const SideData &side = m_Sides[s];
	if (side.hand < 0)
		return;

	bones[side.hand] = Blend(from[side.hand], to[side.hand], amount);
	for (const Joint &joint : side.joints)
		bones[joint.bone] = Blend(from[joint.bone], to[joint.bone], amount);
}
