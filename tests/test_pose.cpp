#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>
#include "../L4D2VR/vr_pose_protocol.h"
#include "../L4D2VR/pose_playout.h"
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

namespace
{
	struct Shown
	{
		double at;     // real time, ms
		double value;  // the real time the shown pose was sent at
	};

	// A stream through the playout buffer: the sender sends every `interval` ms (its clock starting at `senderStart`), each
	// pose sent at real time s arrives at deliver(s, k) (negative: lost), the receiver (its clock at `receiverStart`)
	// samples 90 times per second. The pose's value is the real time it was sent at.
	template <typename Deliver>
	std::vector<Shown> Play(double interval, double from, double to, double senderStart, double receiverStart, Deliver deliver,
		PosePlayout::Buffer<double> &buffer)
	{
		struct Packet { double arrival; double sent; };
		std::vector<Packet> packets;
		int k = 0;
		for (double sent = from; sent < to; sent += interval, ++k)
		{
			const double arrival = deliver(sent, k);
			if (arrival >= 0.0)
				packets.push_back({ arrival, sent });
		}
		std::stable_sort(packets.begin(), packets.end(), [](const Packet &a, const Packet &b) { return a.arrival < b.arrival; });

		std::vector<Shown> shown;
		size_t next = 0;
		for (double now = from; now < to + 500.0; now += 1000.0 / 90.0)
		{
			for (; next < packets.size() && packets[next].arrival <= now; ++next)
			{
				const uint16_t senderClock = (uint16_t)(long long)std::floor(senderStart + packets[next].sent);
				buffer.Add(senderClock, receiverStart + packets[next].arrival, packets[next].sent);
			}

			const double *a = nullptr, *b = nullptr;
			float t = 0.0f;
			if (buffer.Sample(receiverStart + now, a, b, t))
				shown.push_back({ now, *a + (*b - *a) * t });
		}
		return shown;
	}

	struct PlayStats
	{
		bool backwards = false;
		int frames = 0;
		int uneven = 0;       // frames where it moved much more or less than real time
		double maxDelay = 0.0;
	};

	PlayStats Measure(const std::vector<Shown> &shown, double from, double to)
	{
		PlayStats stats;
		const double frame = 1000.0 / 90.0;
		for (size_t i = 1; i < shown.size(); ++i)
		{
			if (shown[i].at < from || shown[i].at > to)
				continue;
			const double step = shown[i].value - shown[i - 1].value;
			if (step < -0.001)
				stats.backwards = true;
			if (step < frame * 0.72 || step > frame * 1.08)
				++stats.uneven;
			++stats.frames;
			stats.maxDelay = std::max(stats.maxDelay, shown[i].at - shown[i].value);
		}
		return stats;
	}

	unsigned g_Random = 12345;
	double Random01()
	{
		g_Random = g_Random * 1103515245u + 12345u;
		return ((g_Random >> 8) & 0xFFFF) / 65536.0;
	}

	void TestPlayout()
	{
		printf("pose playback\n");
		const double interval = 1000.0 / 30.0;

		{
			// Bunched like the reliable stream did: what was sent arrives together, every 100 ms, 50 ms late
			PosePlayout::Buffer<double> buffer;
			const auto shown = Play(interval, 0.0, 6000.0, 1234.0, 50000.0,
				[](double sent, int) { return std::ceil((sent + 50.0) / 100.0) * 100.0; }, buffer);
			const PlayStats stats = Measure(shown, 1500.0, 6000.0);
			CHECK(stats.frames > 300);
			CHECK(!stats.backwards);
			CHECK(stats.uneven == 0);
			CHECK(stats.maxDelay < 260.0);
		}

		{
			// Every pose on time, 30 ms late: shown with little more delay than the time between two poses
			PosePlayout::Buffer<double> buffer;
			const auto shown = Play(interval, 0.0, 4000.0, 0.0, 0.0, [](double sent, int) { return sent + 30.0; }, buffer);
			const PlayStats stats = Measure(shown, 1000.0, 4000.0);
			CHECK(!stats.backwards);
			CHECK(stats.uneven == 0);
			CHECK(stats.maxDelay < 30.0 + interval + 10.0);
		}

		{
			// Jitter of 50 ms, 15% lost, some out of order (dropped). The sender's clock wraps around 16 bits meanwhile.
			PosePlayout::Buffer<double> buffer;
			g_Random = 777;
			const auto shown = Play(interval, 0.0, 8000.0, 65536.0 - 3000.0, 1e7,
				[](double sent, int) { const double lost = Random01(); const double late = Random01() * 50.0; return lost < 0.15 ? -1.0 : sent + 40.0 + late; },
				buffer);
			const PlayStats stats = Measure(shown, 1500.0, 8000.0);
			CHECK(stats.frames > 500);
			CHECK(!stats.backwards);
			CHECK(stats.uneven <= stats.frames / 50);
			CHECK(stats.maxDelay < 40.0 + 50.0 + 3 * interval + 20.0);
		}

		{
			// A pause of 3 s: a new stream after it, also when the sender's clock went back (the game restarted)
			PosePlayout::Buffer<double> buffer;
			auto shown = Play(interval, 0.0, 2000.0, 100.0, 0.0, [](double sent, int) { return sent + 40.0; }, buffer);
			CHECK(!shown.empty() && shown.back().value > 1900.0);
			shown = Play(interval, 5000.0, 7000.0, 100.0, 0.0, [](double sent, int) { return sent + 40.0; }, buffer);
			PlayStats stats = Measure(shown, 5500.0, 7000.0);
			CHECK(!stats.backwards);
			CHECK(stats.uneven == 0);
			CHECK(stats.maxDelay < 40.0 + interval + 10.0);
			shown = Play(interval, 9000.0, 11000.0, -8000.0, 0.0, [](double sent, int) { return sent + 40.0; }, buffer);
			stats = Measure(shown, 9500.0, 11000.0);
			CHECK(stats.frames > 100);
			CHECK(!stats.backwards);
			CHECK(stats.maxDelay < 40.0 + interval + 10.0);
		}

		{
			// The same pose twice or an older one: ignored
			PosePlayout::Buffer<double> buffer;
			CHECK(buffer.Add(100, 0.0, 1.0));
			CHECK(!buffer.Add(100, 1.0, 2.0));
			CHECK(!buffer.Add(90, 2.0, 3.0));
			CHECK(buffer.Add(133, 3.0, 4.0));
			CHECK(!buffer.Add(2, 4.0, 5.0));        // far older without a pause: out of order
			CHECK(!buffer.Add(65530, 5.0, 6.0));    // older across the wrap
			const double *a = nullptr, *b = nullptr;
			float t = 0.0f;
			CHECK(buffer.Sample(10.0, a, b, t));
			buffer.Reset();
			CHECK(!buffer.Sample(10.0, a, b, t));
		}
	}
}

void RunPoseTests()
{
	TestProtocol();
	TestArmSolver();
	TestPlayout();
}
