#include "vr.h"
#include <Windows.h>
#include <algorithm>
#include <cstring>
#include <unordered_map>
#include "body_layout.h"
#include "dual_wield.h"
#include "sdk.h"
#include "game.h"
#include "hooks.h"
#include "studio.h"
#include "pose_relay.h"
#include "diagnostics.h"

using namespace HandMath;

// What the player carries is seen on the body:
// - the ammo bag at the waist, where magazines and shells are taken (manual reload),
// - pills or adrenaline on the chest by the left shoulder, a grenade on the right shoulder; grabbing one takes it.
// They're the game's own models, drawn with the weapon in first person (DrawBodyModels). Other VR players with the mod
// are seen with theirs at the same places, where the game draws what each player carries.

namespace
{
	// The client's data tables, to find where a networked variable is in an entity (Source engine layout)
	struct RecvTable;
	struct RecvProp
	{
		const char *name;
		int type;
		int flags;
		int stringBufferSize;
		int insideArray;
		const void *extraData;
		RecvProp *arrayProp;
		void *arrayLengthProxy;
		void *proxy;
		void *dataTableProxy;
		RecvTable *dataTable;
		int offset;
		int elementStride;
		int elements;
		const char *parentArrayName;
	};
	struct RecvTable
	{
		RecvProp *props;
		int count;
		void *decoder;
		const char *name;
	};
	struct ClientClass
	{
		void *create;
		void *createEvent;
		const char *networkName;
		RecvTable *table;
		ClientClass *next;
		int classId;
	};
	static_assert(sizeof(RecvProp) == 0x3C, "RecvProp of the Source engine");

	bool FindProp(const RecvTable *table, const char *name, int base, int depth, int &offset, int &count)
	{
		if (!table || !table->props || depth > 10)
			return false;

		for (int i = 0; i < table->count; ++i)
		{
			const RecvProp &prop = table->props[i];
			if (!prop.name)
				continue;
			if (strcmp(prop.name, name) == 0)
			{
				offset = base + prop.offset;
				count = prop.dataTable ? prop.dataTable->count : 1;
				return true;
			}
			if (prop.dataTable && FindProp(prop.dataTable, name, base + prop.offset, depth + 1, offset, count))
				return true;
		}
		return false;
	}

	// A wrong layout would read garbage: it's caught instead of crashing
	bool FindPlayerProp(void *firstClass, const char *name, int &offset, int &count)
	{
		__try
		{
			for (const ClientClass *clientClass = (const ClientClass *)firstClass; clientClass; clientClass = clientClass->next)
			{
				if (clientClass->networkName && strcmp(clientClass->networkName, "CTerrorPlayer") == 0)
					return FindProp(clientClass->table, name, 0, 0, offset, count);
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
		return false;
	}

	const int kClientAmmoArrayOffset = 0xF24; // known, checks the lookup
	int g_MyWeaponsOffset = -1;
	int g_MyWeaponsCount = 0;
	int g_ModelIndexOffset = -1;
	int g_OwnerOffset = -1;
	bool g_NetvarsLookedUp = false;

	bool HasPlayerClass(const ClientClass *clientClass)
	{
		for (int i = 0; clientClass && i < 1000; ++i, clientClass = clientClass->next)
		{
			if (clientClass->networkName && strcmp(clientClass->networkName, "CTerrorPlayer") == 0)
				return true;
		}
		return false;
	}

	// IBaseClientDLL::GetAllClasses, found by its code (mov eax, [g_pClientClassHead]; ret) instead of trusting its place
	// in the interface, which changed between engine versions. The list it gives has to have the player's class.
	void *GetAllClasses(void *baseClientDll, int &foundIndex)
	{
		foundIndex = -1;
		__try
		{
			void **vtable = *(void ***)baseClientDll;
			for (int index = 0; index <= 40; ++index)
			{
				const uint8_t *code = (const uint8_t *)vtable[index];
				if (!code || code[0] != 0xA1 || code[5] != 0xC3)
					continue;
				void *classes = **(void ***)(code + 1);
				if (HasPlayerClass((const ClientClass *)classes))
				{
					foundIndex = index;
					return classes;
				}
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
		return nullptr;
	}

	void LookUpNetvars(Game *game)
	{
		g_NetvarsLookedUp = true;
		int classesIndex = -1;
		void *classes = game->m_BaseClientDll ? GetAllClasses(game->m_BaseClientDll, classesIndex) : nullptr;
		Diagnostics::Log("body items: the client's classes %s (IBaseClientDLL function %d)", classes ? "found" : "not found", classesIndex);

		int ammoOffset = -1, ammoCount = 0, weaponsOffset = -1, weaponsCount = 0;
		const bool foundAmmo = classes && FindPlayerProp(classes, "m_iAmmo", ammoOffset, ammoCount);
		const bool foundWeapons = classes && FindPlayerProp(classes, "m_hMyWeapons", weaponsOffset, weaponsCount);

		int unused = 0;
		if (classes)
		{
			FindPlayerProp(classes, "m_nModelIndex", g_ModelIndexOffset, unused);
			FindPlayerProp(classes, "m_hOwnerEntity", g_OwnerOffset, unused);
		}
		Diagnostics::Log("body items: m_nModelIndex at 0x%X, m_hOwnerEntity at 0x%X", g_ModelIndexOffset, g_OwnerOffset);

		if (foundAmmo && ammoOffset == kClientAmmoArrayOffset && foundWeapons && weaponsCount > 0 && weaponsCount <= 64)
		{
			g_MyWeaponsOffset = weaponsOffset;
			g_MyWeaponsCount = weaponsCount;
			Diagnostics::Log("body items: m_hMyWeapons at 0x%X, %d of them", weaponsOffset, weaponsCount);
		}
		else
		{
			Diagnostics::Log("body items: the inventory wasn't found (m_iAmmo %s at 0x%X, m_hMyWeapons %s at 0x%X, %d), nothing is shown on the chest",
				foundAmmo ? "found" : "not found", ammoOffset, foundWeapons ? "found" : "not found", weaponsOffset, weaponsCount);
		}
	}

	struct BodyModel
	{
		const char *path;
		const void *model;
		bool lookedUp;
		bool loadIt;  // not one the game has loaded for the map
	};

	BodyModel g_Models[] =
	{
		{ "models/props_collectables/backpack.mdl", nullptr, false, true },
		{ "models/w_models/weapons/w_eq_explosive_ammopack.mdl", nullptr, false, false },
		{ "models/w_models/weapons/w_eq_incendiary_ammopack.mdl", nullptr, false, false },
		{ "models/w_models/weapons/w_eq_painpills.mdl", nullptr, false, false },
		{ "models/w_models/weapons/w_eq_adrenaline.mdl", nullptr, false, false },
		{ "models/w_models/weapons/w_eq_molotov.mdl", nullptr, false, false },
		{ "models/w_models/weapons/w_eq_pipebomb.mdl", nullptr, false, false },
		{ "models/w_models/weapons/w_eq_bile_flask.mdl", nullptr, false, false },
		{ "models/v_models/v_adrenaline.mdl", nullptr, false, true },
	};
	enum { Model_Pouch, Model_Bag, Model_BagOther, Model_Pills, Model_Adrenaline, Model_Molotov, Model_PipeBomb, Model_BileJar, Model_AdrenalineView };

	// The game has no ammo pouch: its small backpack, reduced to the size of one, with its back against the hip
	const float kPouchScale = 0.36f;
	const Vec3 kPouchCenter(0.27f, 8.0f, 5.26f);
	const float kChestItemSmallScale = 0.75f;

	// IVModelInfoClient::FindOrLoadModel: gives a model the map didn't load, loading it
	const int kFindOrLoadModelIndex = 41;
	const unsigned char kFindOrLoadModelStart[] = { 0x55, 0x8B, 0xEC, 0x53, 0x8B, 0x5D, 0x08, 0x56, 0x57, 0x8B, 0xF1 };

	const void *LoadModel(Game *game, const char *path)
	{
		void **vtable = *(void ***)game->m_ModelInfo;
		if (memcmp(vtable[kFindOrLoadModelIndex], kFindOrLoadModelStart, sizeof(kFindOrLoadModelStart)) != 0)
			return nullptr;
		typedef const void *(__thiscall *tFindOrLoadModel)(void *thisptr, const char *name);
		return ((tFindOrLoadModel)vtable[kFindOrLoadModelIndex])(game->m_ModelInfo, path);
	}

	const void *GetModel(Game *game, int which)
	{
		BodyModel &entry = g_Models[which];
		if (!entry.lookedUp)
		{
			entry.lookedUp = true;
			if (entry.loadIt)
			{
				entry.model = LoadModel(game, entry.path);
			}
			else
			{
				const int index = game->m_ModelInfo->GetModelIndex(entry.path);
				entry.model = index >= 0 ? game->m_ModelInfo->GetModel(index) : nullptr;
			}
			Diagnostics::Log("body items: %s %s", entry.path, entry.model ? "loaded" : "not loaded by the game");
		}
		return entry.model;
	}

	int ModelOfItem(int weaponId)
	{
		switch (weaponId)
		{
		case C_WeaponCSBase::PAIN_PILLS: return Model_Pills;
		case C_WeaponCSBase::ADRENALINE: return Model_Adrenaline;
		case C_WeaponCSBase::MOLOTOV: return Model_Molotov;
		case C_WeaponCSBase::PIPE_BOMB: return Model_PipeBomb;
		case C_WeaponCSBase::VOMITJAR: return Model_BileJar;
		default: return -1;
		}
	}

	// Which place on the chest an item goes to: 0 pills or adrenaline, 1 grenade, -1 none
	int ChestSpotOf(int weaponId)
	{
		switch (weaponId)
		{
		case C_WeaponCSBase::PAIN_PILLS:
		case C_WeaponCSBase::ADRENALINE:
			return 0;
		case C_WeaponCSBase::MOLOTOV:
		case C_WeaponCSBase::PIPE_BOMB:
		case C_WeaponCSBase::VOMITJAR:
			return 1;
		default:
			return -1;
		}
	}

	// Where the pills and the grenade go: m_ItemSpot* (config.txt), BodyLayout::DefaultItemSpot
	const float kChestGrabRadius = 0.12f; // meters
	const float kChestViewCosine = 0.2f;
	// The weapon hand's fist takes the secondary weapon this close to the pistol on the thigh, from its grip to the end of
	// its barrel, whether it's there or not
	const float kHolsterGrabRadius = 0.12f; // meters
	const float kHolsterLengthMeters = 0.18f; // the pistol's models go 7.6 units along the barrel from the grip, 9.9 the magnum's

	// Using pills and adrenaline (VR::ItemUseGestureReady), meters. The mouth from the eyes, and how close the fist with
	// the pills gets to it.
	const float kMouthBelowEyes = 0.08f;
	const float kMouthForward = 0.02f;
	const float kMouthReach = 0.15f;
	// The body for the needle: a vertical line from the chest down to the thighs, a bit behind the eyes, and how close
	// to it the tip gets, pointing at it (cosine)
	const float kChestBelowEyes = 0.25f;
	const float kBodyLineForward = -0.05f;
	const float kBodyReach = 0.24f;
	const float kNeedleTowardsBody = 0.3f;

	// The fist of another player's left hand from the wrist they send, meters: along the fingers, towards the palm
	const float kRemoteFistForward = 0.075f;
	const float kRemoteFistPalm = 0.025f;

	// Where the head faces horizontally. When looking almost straight up or down the forward vector is too short, so the
	// up vector is used as well: for a head pitched by p, forward * cos(p) + up * sin(p) is level.
	Vec3 HeadFacing(const Vec3 &headForward, const Vec3 &headUp)
	{
		Vec3 facing(headForward.x, headForward.y, 0.0f);
		if (Length(facing) < 0.2f)
			facing = Vec3(headForward.x * headUp.z - headUp.x * headForward.z, headForward.y * headUp.z - headUp.y * headForward.z, 0.0f);
		return Normalized(facing);
	}

	// The place the body items are measured from: where the eyes would be with the head turned the way the body faces
	// and looking straight ahead (the head turns around the neck), facing that way. x forward, y left, z up.
	Mat34 BodyFrame(const Vec3 &head, const Vec3 &headForward, const Vec3 &headUp, const Vec3 &facing, float unitsPerMeter)
	{
		const float neckForward = 0.08f * unitsPerMeter, neckDown = 0.075f * unitsPerMeter;
		const Vec3 neck = head - headForward * neckForward - headUp * neckDown;
		const Vec3 reference = neck + facing * neckForward + Vec3(0.0f, 0.0f, neckDown);
		const Vec3 up(0.0f, 0.0f, 1.0f);
		return FromAxes(facing, Cross(up, facing), up, reference);
	}

	// An item standing upright on the body, facing forward, `offset` meters away in the body frame. The adrenaline's
	// model lies along its y axis.
	Mat34 ChestItemFrame(const Mat34 &body, const Vec3 &offset, int weaponId, float unitsPerMeter)
	{
		const Vec3 point = TransformPoint(body, offset * unitsPerMeter);
		const Vec3 forward = Axis(body, 0), up(0.0f, 0.0f, 1.0f);
		if (weaponId == C_WeaponCSBase::ADRENALINE)
			return FromAxes(forward, up, Cross(forward, up), point);
		return FromAxes(forward, Cross(up, forward), up, point);
	}

	template <typename T>
	T Read(const void *entity, int offset)
	{
		return *(const T *)((const uint8_t *)entity + offset);
	}

	// The entity index in a handle (NUM_ENT_ENTRY_BITS of the engine)
	const int kHandleIndexMask = 0xFFF;

	bool IsValidHandle(int handle)
	{
		return handle != 0 && handle != -1;
	}

	Vec3 ToVec3(const Vector &v) { return Vec3(v.x, v.y, v.z); }
}

void VR::ClearBodyModels()
{
	for (BodyModel &entry : g_Models)
	{
		entry.model = nullptr;
		entry.lookedUp = false;
	}
	m_BodyDrawCount = 0;
	for (bool &valid : m_RemoteHeadOffsetValid)
		valid = false;
}

// Every frame, after the tracking: what is drawn on the body and where
void VR::UpdateBodyItems()
{
	m_BodyDrawCount = 0;
	m_AmmoBagDrawn = false;
	m_ChestItemIds[0] = m_ChestItemIds[1] = 0;
	m_ChestSpotsValid = false;
	m_HolsterValid = false;

	if (!m_Game->m_EngineClient->IsInGame())
		return;

	C_BasePlayer *local = (C_BasePlayer *)m_Game->GetClientEntity(m_Game->m_EngineClient->GetLocalPlayer());
	if (!local)
		return;

	// The bag
	if (m_ManualReloadEnabled)
	{
		const void *pouch = GetModel(m_Game, Model_Pouch);
		const void *bag = pouch;
		if (!bag)
			bag = GetModel(m_Game, Model_Bag);
		if (!bag)
			bag = GetModel(m_Game, Model_BagOther);
		if (bag)
		{
			m_AmmoBagDrawn = true;
			if (m_ReloadPouchVisible)
			{
				const Vec3 up(0.0f, 0.0f, 1.0f);
				const Vec3 forward = Normalized(m_WaistForward);
				const Vec3 outward = Cross(up, forward) * (m_LeftHanded ? -1.0f : 1.0f);
				BodyModelDraw draw;
				draw.model = bag;
				if (pouch)
				{
					// Its y axis goes from the straps to the front
					draw.frame = FromAxes(Cross(outward, up), outward, up, m_WaistWorld);
					draw.scale = kPouchScale;
					draw.hasCenter = true;
					draw.center = kPouchCenter;
				}
				else
				{
					// The ammo pack's thin side faces out
					draw.frame = FromAxes(outward, Cross(up, outward), up, m_WaistWorld);
				}
				m_BodyDraws[m_BodyDrawCount++] = draw;
			}
		}
	}

	// Pills, adrenaline and grenades carried, not in the hand
	if (!g_NetvarsLookedUp)
		LookUpNetvars(m_Game);
	if (g_MyWeaponsOffset < 0)
		return;

	const void *active = local->GetActiveWeapon();
	for (int i = 0; i < g_MyWeaponsCount; ++i)
	{
		const int handle = Read<int>(local, g_MyWeaponsOffset + i * 4);
		if (!IsValidHandle(handle))
			continue;
		C_WeaponCSBase *weapon = (C_WeaponCSBase *)m_Game->m_ClientEntityList->GetClientEntityFromHandle(handle);
		// In a hand: the active weapon, or the left hand's item
		if (!weapon || weapon == active || (handle & kHandleIndexMask) == m_LeftItemIndex)
			continue;
		const int weaponId = weapon->GetWeaponID();
		const int spot = ChestSpotOf(weaponId);
		if (spot >= 0)
			m_ChestItemIds[spot] = weaponId;
	}

	// Turned the way the body faces, so what's on it stays where it is when the head turns to look at it
	const Mat34 body = BodyFrame(ToVec3(m_HmdPosAbs), ToVec3(m_HmdForward), ToVec3(m_HmdUp), ToVec3(m_BodyFacing), m_VRScale);
	Vec3 spotOffsets[2];
	for (int spot = 0; spot < 2; ++spot)
	{
		const BodyLayout::ItemSpot itemSpot = { m_ItemSpotForward[spot], m_ItemSpotSide[spot], m_ItemSpotBelowEyes[spot] };
		spotOffsets[spot] = BodyLayout::ItemSpotOffset(itemSpot, m_LeftHanded);
		m_ChestSpotPoints[spot] = TransformPoint(body, spotOffsets[spot] * m_VRScale);
	}
	m_ChestSpotsValid = true;

	// The pistol's place on the thigh, from the height of the eyes above the floor of the play area (the body frame's
	// origin, in tracking space)
	const float floorZ = m_HmdPosAbs.z - m_HmdPosLocalInWorld.z;
	const BodyLayout::Holster holster = BodyLayout::HolsterOnThigh(m_PlayerHeightInMeters, (Origin(body).z - floorZ) / m_VRScale, m_LeftHanded);
	m_HolsterPoint = TransformPoint(body, holster.point * m_VRScale);
	m_HolsterBarrel = RotateVector(body, holster.barrel);
	m_HolsterValid = true;

	// The left hand's item
	BodyModelDraw leftItem;
	if (m_BodyDrawCount < kMaxBodyDraws && GetLeftItemDraw(leftItem))
		m_BodyDraws[m_BodyDrawCount++] = leftItem;

	// The pistol on the thigh of the weapon hand, pointing down along it, where the grab takes it (IsAtHolster). Its
	// weapon bone has y along the grip and z along the barrel: the grip sticks out backwards.
	for (int i = 0; i < g_MyWeaponsCount && m_BodyDrawCount < kMaxBodyDraws; ++i)
	{
		const int handle = Read<int>(local, g_MyWeaponsOffset + i * 4);
		if (!IsValidHandle(handle))
			continue;
		C_WeaponCSBase *weapon = (C_WeaponCSBase *)m_Game->m_ClientEntityList->GetClientEntityFromHandle(handle);
		if (!weapon || weapon == active || (handle & kHandleIndexMask) == m_LeftItemIndex)
			continue;
		const int weaponId = weapon->GetWeaponID();
		if (weaponId != C_WeaponCSBase::PISTOL && weaponId != C_WeaponCSBase::MAGNUM)
			continue;

		const void *model = GetEntityModel(weapon);
		const bool inView = Dot(Normalized(m_HolsterPoint - ToVec3(m_HmdPosAbs)), ToVec3(m_HmdForward)) > kChestViewCosine;
		if (model && inView)
		{
			const Vec3 slide = RotateVector(body, holster.slide);
			BodyModelDraw draw;
			draw.model = model;
			draw.frame = FromAxes(Cross(slide, m_HolsterBarrel), slide, m_HolsterBarrel, m_HolsterPoint);
			draw.alignWeaponBone = true;
			m_BodyDraws[m_BodyDrawCount++] = draw;
		}
		break;
	}

	for (int spot = 0; spot < 2; ++spot)
	{
		const int weaponId = m_ChestItemIds[spot];
		if (!weaponId)
			continue;
		const Mat34 frame = ChestItemFrame(body, spotOffsets[spot], weaponId, m_VRScale);
		m_ChestItemPoints[spot] = Origin(frame);

		const void *model = GetModel(m_Game, ModelOfItem(weaponId));
		const bool inView = Dot(Normalized(Origin(frame) - ToVec3(m_HmdPosAbs)), ToVec3(m_HmdForward)) > kChestViewCosine;
		if (model && inView && m_BodyDrawCount < kMaxBodyDraws)
		{
			BodyModelDraw draw;
			draw.model = model;
			draw.frame = frame;
			draw.scale = m_SmallBodyItems ? kChestItemSmallScale : 1.0f;
			m_BodyDraws[m_BodyDrawCount++] = draw;
		}
	}
}

// Called after each draw of the weapon in first person, so they're drawn in the same pass as it
void VR::DrawBodyModels(int flags, void *renderable, int entityIndex)
{
	for (int i = 0; i < m_BodyDrawCount; ++i)
		Hooks::DrawBodyModel(flags, renderable, entityIndex, &m_BodyDraws[i]);
}

// The slot of the item on the chest where the hand is (slot3 grenade, slot5 pills or adrenaline), 0 if none
int VR::GetChestItemSlotAt(const Vec3 &handWorld) const
{
	for (int spot = 0; spot < 2; ++spot)
	{
		if (m_ChestItemIds[spot] && Length(handWorld - m_ChestItemPoints[spot]) < kChestGrabRadius * m_VRScale)
			return spot == 0 ? 5 : 3;
	}
	return 0;
}

// Where a pills or grenade spot of the chest is at the hand, with or without the item: 0 pills, 1 grenade, -1 none
int VR::GetChestSpotAt(const Vec3 &handWorld) const
{
	if (!m_ChestSpotsValid)
		return -1;
	for (int spot = 0; spot < 2; ++spot)
	{
		if (Length(handWorld - m_ChestSpotPoints[spot]) < kChestGrabRadius * m_VRScale)
			return spot;
	}
	return -1;
}

// The fist is on the pistol on the thigh, with or without it there: anywhere from its grip to the end of the barrel
bool VR::IsAtHolster(const Vec3 &fist) const
{
	if (!m_HolsterValid)
		return false;
	const float along = std::clamp(Dot(fist - m_HolsterPoint, m_HolsterBarrel), 0.0f, kHolsterLengthMeters * m_VRScale);
	return Length(fist - (m_HolsterPoint + m_HolsterBarrel * along)) < kHolsterGrabRadius * m_VRScale;
}

const void *VR::GetSyringeModel()
{
	return GetModel(m_Game, Model_AdrenalineView);
}

// Pills are taken by bringing them to the mouth, adrenaline by stabbing its needle into the body: true while the item in
// the hand that uses it is there (the trigger only uses them then, ProcessInput)
bool VR::ItemUseGestureReady(bool leftHand, int weaponId)
{
	const Vec3 head = ToVec3(m_HmdPosAbs), headForward = ToVec3(m_HmdForward), headUp = ToVec3(m_HmdUp);
	Mat34 grip;
	const bool leftGrip = leftHand && GetLeftGripFrame(grip);

	if (weaponId == C_WeaponCSBase::PAIN_PILLS)
	{
		const Vec3 mouth = head + (headUp * -kMouthBelowEyes + headForward * kMouthForward) * m_VRScale;
		const Vec3 fist = leftGrip ? Origin(grip) : GetFistPoint(!leftHand);
		return Length(fist - mouth) < kMouthReach * m_VRScale;
	}

	if (weaponId != C_WeaponCSBase::ADRENALINE)
		return false;

	// The needle comes out of the fist by the little finger, in both hands
	Vec3 tip, needle;
	if (leftGrip)
	{
		needle = Normalized(Axis(grip, 1)) * -1.0f;
		tip = Origin(grip) + needle * kSyringeNeedleUnits;
	}
	else if (!leftHand && GetTickCount64() - m_RightNeedleMs < 250)
	{
		tip = m_RightNeedleTip;
		needle = m_RightNeedleDir;
	}
	else
	{
		return false;
	}

	// The body: a line from the chest down to the thighs (where the pistol is, which follows crouching), under the neck
	const Mat34 body = BodyFrame(head, headForward, headUp, ToVec3(m_BodyFacing), m_VRScale);
	const Vec3 top = TransformPoint(body, Vec3(kBodyLineForward, 0.0f, -kChestBelowEyes) * m_VRScale);
	const float bottomZ = m_HolsterValid ? m_HolsterPoint.z : top.z - 0.5f * m_VRScale;
	return BodyLayout::NeedleAtBody(tip, needle, top, bottomZ, kBodyReach * m_VRScale, kNeedleTowardsBody);
}

// Which way the body faces (BodyLayout::FollowBodyYaw), in the play area: the thumbstick turns it with the head
void VR::UpdateBodyYaw(float deltaSeconds)
{
	const float kLookingDown = -0.55f; // forward.z, about 33 degrees down

	const Vec3 facing = HeadFacing(ToVec3(m_HmdForward), ToVec3(m_HmdUp));
	const float headYaw = atan2f(facing.y, facing.x) * 180.0f / 3.14159265f - m_RotationOffset;
	if (!m_BodyYawValid)
	{
		m_BodyYawLocal = headYaw;
		m_BodyYawValid = true;
	}
	m_BodyYawLocal = BodyLayout::FollowBodyYaw(m_BodyYawLocal, headYaw, m_HmdForward.z < kLookingDown, deltaSeconds);

	const float radians = (m_BodyYawLocal + m_RotationOffset) * 3.14159265f / 180.0f;
	m_BodyFacing = Vector(cosf(radians), sinf(radians), 0.0f);
}

// The weapons a player carries. Returns how many were written.
int VR::GetInventory(C_BasePlayer *player, InventoryItem *items, int max)
{
	if (!g_NetvarsLookedUp)
		LookUpNetvars(m_Game);
	if (g_MyWeaponsOffset < 0 || !player)
		return 0;

	int count = 0;
	for (int i = 0; i < g_MyWeaponsCount && count < max; ++i)
	{
		const int handle = Read<int>(player, g_MyWeaponsOffset + i * 4);
		if (!IsValidHandle(handle))
			continue;
		C_WeaponCSBase *weapon = (C_WeaponCSBase *)m_Game->m_ClientEntityList->GetClientEntityFromHandle(handle);
		if (!weapon)
			continue;
		items[count].index = handle & kHandleIndexMask;
		items[count].id = weapon->GetWeaponID();
		items[count].weapon = weapon;
		++count;
	}
	return count;
}

// The model an entity is drawn with
const void *VR::GetEntityModel(const void *entity)
{
	if (!entity || g_ModelIndexOffset < 0)
		return nullptr;
	const int modelIndex = Read<int16_t>(entity, g_ModelIndexOffset);
	return modelIndex > 0 ? m_Game->m_ModelInfo->GetModel(modelIndex) : nullptr;
}

namespace
{
	// The bone a model is held by in the third person ("ValveBiped.weapon_bone", "L_weapon_bone"), -1 if none
	int WeaponBoneOf(const void *studioHeader)
	{
		static std::unordered_map<const void *, int> s_Found;
		auto found = s_Found.find(studioHeader);
		if (found != s_Found.end())
			return found->second;

		int bone = -1;
		const int count = Studio::BoneCount(studioHeader);
		for (int i = 0; i < count && bone < 0; ++i)
		{
			if (strstr(Studio::BoneName(studioHeader, i), "weapon_bone"))
				bone = i;
		}
		s_Found[studioHeader] = bone;
		return bone;
	}

	// The adrenaline's first person model: the syringe is "weapon_clip", centered on its y axis like the weapon bone of
	// the capped one, and the pieces of the cap are "weapon_bolt" and "weapon_trigger"
	struct SyringeBones
	{
		int syringe = -1, capPieces[2] = { -1, -1 };
	};

	const SyringeBones &SyringeBonesOf(const void *studioHeader)
	{
		static std::unordered_map<const void *, SyringeBones> s_Found;
		auto found = s_Found.find(studioHeader);
		if (found != s_Found.end())
			return found->second;

		SyringeBones bones;
		bones.syringe = Studio::FindBone(studioHeader, "ValveBiped.weapon_clip");
		bones.capPieces[0] = Studio::FindBone(studioHeader, "ValveBiped.weapon_bolt");
		bones.capPieces[1] = Studio::FindBone(studioHeader, "ValveBiped.weapon_trigger");
		return s_Found.emplace(studioHeader, bones).first->second;
	}
}

// The bones of an item drawn on the body: the model as it was made, its center at the frame, at its scale
void VR::BuildBodyModelBones(const void *studioHeader, const BodyModelDraw &draw, void *outBones)
{
	Mat34 *bones = (Mat34 *)outBones;
	const int boneCount = std::clamp(Studio::BoneCount(studioHeader), 0, Studio::kMaxBones);

	// Held: the bone the model is held by goes where the hand holds it
	const SyringeBones *syringe = draw.syringe ? &SyringeBonesOf(studioHeader) : nullptr;
	const int heldBone = syringe ? syringe->syringe : draw.alignWeaponBone ? WeaponBoneOf(studioHeader) : -1;
	if (heldBone >= 0 && heldBone < boneCount)
	{
		const Mat34 placed = Multiply(draw.frame, Studio::PoseToBone(studioHeader, heldBone));
		for (int i = 0; i < boneCount; ++i)
			bones[i] = Multiply(placed, InvertRigid(Studio::PoseToBone(studioHeader, i)));
		if (syringe)
		{
			for (int piece : syringe->capPieces)
			{
				if (piece >= 0 && piece < boneCount)
					bones[piece] = CollapsedAt(Origin(draw.frame));
			}
		}
		return;
	}

	const Vec3 hullMin = *(const Vec3 *)((const uint8_t *)studioHeader + 104);
	const Vec3 hullMax = *(const Vec3 *)((const uint8_t *)studioHeader + 116);
	const Vec3 center = draw.hasCenter ? draw.center : (hullMin + hullMax) * 0.5f;

	Mat34 placed = draw.frame;
	for (int axis = 0; axis < 3; ++axis)
		SetAxis(placed, axis, Axis(draw.frame, axis) * draw.scale);
	SetAxis(placed, 3, TransformPoint(placed, center * -1.0f));

	for (int i = 0; i < boneCount; ++i)
		bones[i] = Multiply(placed, InvertRigid(Studio::PoseToBone(studioHeader, i)));
}

// Another VR player's pills or grenade, drawn by the game where that player carries them: moved to the chest. False
// when it isn't one.
bool VR::BuildRemoteItemBones(int entityIndex, const void *weapon, const void *studioHeader, void *outBones)
{
	if (g_MyWeaponsOffset < 0)
		return false;

	// Only an entity found in a player's inventory is known to be a weapon: weapons lying around in the map are drawn
	// with the same models and aren't
	for (int player = 1; player <= 32; ++player)
	{
		if (!m_RemoteHeadOffsetValid[player])
			continue;

		C_BasePlayer *owner = (C_BasePlayer *)m_Game->GetClientEntity(player);
		if (!owner || (const void *)owner->GetActiveWeapon() == weapon)
			continue;

		bool carries = false;
		for (int i = 0; i < g_MyWeaponsCount && !carries; ++i)
		{
			const int handle = Read<int>(owner, g_MyWeaponsOffset + i * 4);
			carries = IsValidHandle(handle) && (handle & kHandleIndexMask) == entityIndex;
		}
		if (!carries)
			continue;

		// In their left hand (dual wield): held like in the local player's, from where the hand is
		if (m_RemoteLeftItem[player] == entityIndex)
		{
			PoseRelay::Frame pose;
			if (!m_Game->m_PoseRelay->GetPose(player, pose) || !pose.hasLeftHand)
				return false;

			const Vec3 fingers = Normalized(pose.leftWrist.forward);
			const Vec3 thumb = Normalized(pose.leftWrist.up - fingers * Dot(fingers, pose.leftWrist.up), Vec3(0.0f, 0.0f, 1.0f));
			const Vec3 palm = Cross(fingers, thumb);
			const Vec3 wrist = pose.leftWrist.position + m_RemoteHeadOffset[player];
			const Vec3 fist = wrist + (fingers * kRemoteFistForward + palm * kRemoteFistPalm) * m_VRScale;
			// Weapon bone: y along the grip, z where the fingers point. A melee weapon's blade comes out by the thumb.
			BodyModelDraw draw;
			draw.frame = FromAxes(Cross(thumb, fingers), thumb, fingers, fist);
			draw.alignWeaponBone = true;
			BuildBodyModelBones(studioHeader, draw, outBones);
			return true;
		}

		const int weaponId = ((C_WeaponCSBase *)weapon)->GetWeaponID();
		const int spot = ChestSpotOf(weaponId);
		if (spot < 0)
			return false;

		PoseRelay::Frame pose;
		if (!m_Game->m_PoseRelay->GetPose(player, pose))
			return false;

		const Mat34 body = BodyFrame(pose.head.position + m_RemoteHeadOffset[player], pose.head.forward, pose.head.up, HeadFacing(pose.head.forward, pose.head.up), m_VRScale);
		BodyModelDraw draw;
		// Where they are by default for someone of 1.8 m: their own settings aren't sent
		draw.frame = ChestItemFrame(body, BodyLayout::ItemSpotOffset(BodyLayout::DefaultItemSpot(spot, 1.8f), false), weaponId, m_VRScale);
		draw.scale = m_SmallBodyItems ? kChestItemSmallScale : 1.0f;
		BuildBodyModelBones(studioHeader, draw, outBones);
		return true;
	}
	return false;
}

// ---------------------------------------------------------------------------------------------------------------
// Taking weapons and items and opening doors with the hand on them. A grab button pressed next to one of them does the
// game's use, aimed at it for a moment (Hooks::GetServerHandAim). The host needs the mod.
// ---------------------------------------------------------------------------------------------------------------

namespace
{
	const float kWorldItemReach = 0.28f; // meters
	const float kDoorHandleReach = 0.24f;

	struct WorldTarget
	{
		bool found = false;
		bool door = false;
		Vec3 point;
		float distance = 1e9f;
		int modelIndex = 0;
	};

	bool IsPickable(const char *lowerName)
	{
		return strstr(lowerName, "w_models") || strstr(lowerName, "weapons/melee/w_") || strstr(lowerName, "terror/ammo_stack")
			|| strstr(lowerName, "ammo_can") || strstr(lowerName, "coffeeammo") || strstr(lowerName, "exploding_ammo") || strstr(lowerName, "incendiary_ammo");
	}

	// A grab only changes what its own hand holds: the left hand takes what goes in it (dual wield), and what doesn't go
	// to the right hand: ammo and upgrades for the weapon it holds
	enum LeftHandRule { AnyHand, LeftHandDualWield, LeftHandOnly };

	bool TakenByLeftHand(const char *lowerName, LeftHandRule rule)
	{
		if (rule == AnyHand)
			return true;

		const bool carriedPack = strstr(lowerName, "_ammopack") != nullptr; // goes to the inventory
		if (!carriedPack && (strstr(lowerName, "terror/ammo_stack") || strstr(lowerName, "ammo_can") || strstr(lowerName, "coffeeammo")
			|| strstr(lowerName, "exploding_ammo") || strstr(lowerName, "incendiary_ammo") || strstr(lowerName, "laser_sight")))
			return true;

		if (rule != LeftHandDualWield)
			return false;
		return (strstr(lowerName, "weapons/melee/w_") && !strstr(lowerName, "chainsaw")) || strstr(lowerName, "w_eq_painpills")
			|| strstr(lowerName, "w_eq_adrenaline") || strstr(lowerName, "w_eq_molotov") || strstr(lowerName, "w_eq_pipebomb")
			|| strstr(lowerName, "w_eq_bile_flask");
	}

	bool IsDoor(const char *lowerName)
	{
		return strstr(lowerName, "props_doors/") && !strstr(lowerName, "frame") && !strstr(lowerName, "reinforcement")
			&& !strstr(lowerName, "gib") && !strstr(lowerName, "dummy") && !strstr(lowerName, "crossbar") && !strstr(lowerName, "damaged");
	}

	void Consider(WorldTarget &best, const Vec3 &hand, const Vec3 &point, float reach, bool door, int modelIndex)
	{
		const float distance = Length(point - hand);
		if (distance < reach && distance < best.distance)
		{
			best.found = true;
			best.door = door;
			best.point = point;
			best.distance = distance;
			best.modelIndex = modelIndex;
		}
	}

	// The handle of a door: on the free edge, a bit under the middle of its height. Doors turn around their origin, or
	// around the middle of the model when it has two leaves: then either edge has one.
	void ConsiderDoorHandles(WorldTarget &best, const Vec3 &hand, const Vec3 &origin, const QAngle &angles, const void *model, float reach, int modelIndex)
	{
		const Vec3 mins = *(const Vec3 *)((const uint8_t *)model + 0x118);
		const Vec3 maxs = *(const Vec3 *)((const uint8_t *)model + 0x124);

		Vector forwardV, rightV, upV;
		QAngle::AngleVectors(angles, &forwardV, &rightV, &upV);
		const Vec3 forward(forwardV.x, forwardV.y, forwardV.z), left(-rightV.x, -rightV.y, -rightV.z), up(upV.x, upV.y, upV.z);

		const bool alongY = maxs.y - mins.y > maxs.x - mins.x;
		const Vec3 axis = alongY ? left : forward;
		const float low = alongY ? mins.y : mins.x, high = alongY ? maxs.y : maxs.x;
		const float height = mins.z + (maxs.z - mins.z) * 0.40f;
		const float fromEdge = 3.0f;

		if (high > -low * 0.5f)
			Consider(best, hand, origin + axis * (high - fromEdge) + up * height, reach, true, modelIndex);
		if (-low > high * 0.5f)
			Consider(best, hand, origin + axis * (low + fromEdge) + up * height, reach, true, modelIndex);
	}

	// A wrong guess about an entity must not take the game down. itemReach: game units, or the hand's reach if 0.
	// filter: only models whose path has it.
	void FindWorldTarget(Game *game, const Vec3 &hand, float unitsPerMeter, WorldTarget &best, float itemReach = 0.0f, const char *filter = nullptr, LeftHandRule rule = AnyHand)
	{
		__try
		{
			const int highest = game->m_ClientEntityList->GetHighestEntityIndex();
			for (int i = 33; i <= highest; ++i)
			{
				const uint8_t *entity = (const uint8_t *)game->m_ClientEntityList->GetClientEntity(i);
				if (!entity)
					continue;

				const int modelIndex = *(const int16_t *)(entity + g_ModelIndexOffset);
				void *model = modelIndex != 0 ? game->m_ModelInfo->GetModel(modelIndex) : nullptr;
				const char *name = model ? game->m_ModelInfo->GetModelName(model) : nullptr;
				if (!name)
					continue;

				char lower[128];
				strncpy_s(lower, name, _TRUNCATE);
				_strlwr_s(lower);
				const bool pickable = IsPickable(lower) && TakenByLeftHand(lower, rule);
				const bool door = !IsPickable(lower) && IsDoor(lower);
				if ((!pickable && !door) || (filter && !strstr(lower, filter)))
					continue;

				// Carried by somebody
				const int owner = *(const int *)(entity + g_OwnerOffset);
				if (pickable && owner != 0 && owner != -1)
					continue;

				// IClientRenderable::GetRenderOrigin and GetRenderAngles
				void *renderable = (uint8_t *)entity + 4;
				void **vtable = *(void ***)renderable;
				typedef const Vector &(__thiscall *tGetOrigin)(void *thisptr);
				typedef const QAngle &(__thiscall *tGetAngles)(void *thisptr);
				const Vector &originV = ((tGetOrigin)vtable[1])(renderable);
				const Vec3 origin(originV.x, originV.y, originV.z);

				if (pickable)
					Consider(best, hand, origin, itemReach > 0.0f ? itemReach : kWorldItemReach * unitsPerMeter, false, modelIndex);
				else if (itemReach <= 0.0f)
					ConsiderDoorHandles(best, hand, origin, ((tGetAngles)vtable[2])(renderable), model, kDoorHandleReach * unitsPerMeter, modelIndex);
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
	}
}

// The weapon or item lying nearest to `from`, within radiusUnits (the test of the dual wield)
bool VR::FindNearestPickable(const Vec3 &from, float radiusUnits, Vec3 &point, const char *filter)
{
	if (!g_NetvarsLookedUp)
		LookUpNetvars(m_Game);
	if (g_ModelIndexOffset < 0 || g_OwnerOffset < 0)
		return false;

	WorldTarget target;
	FindWorldTarget(m_Game, from, m_VRScale, target, radiusUnits, filter);
	point = target.point;
	return target.found;
}

// A grab button pressed with the hand at `handWorld`. True if it took something or opened a door. With the left hand,
// a weapon or an item it takes goes to that hand (dual wield).
bool VR::TryGrabWorld(const Vec3 &handWorld, bool leftHand)
{
	if (g_ModelIndexOffset < 0 || g_OwnerOffset < 0 || !m_Game->m_PoseRelay->IsServerCapable())
		return false;

	WorldTarget target;
	const LeftHandRule rule = !leftHand ? AnyHand : m_DualWieldEnabled ? LeftHandDualWield : LeftHandOnly;
	FindWorldTarget(m_Game, handWorld, m_VRScale, target, 0.0f, nullptr, rule);
	if (!target.found)
		return false;

	// From a little behind the hand, straight at it
	Vec3 direction = target.point - handWorld;
	direction = Normalized(direction, ToVec3(m_RightControllerForward));
	const Vec3 from = handWorld - direction * 10.0f;

	m_GrabUseOrigin = Vector(from.x, from.y, from.z);
	const Vector up = fabsf(direction.z) > 0.98f ? Vector(1.0f, 0.0f, 0.0f) : Vector(0.0f, 0.0f, 1.0f);
	QAngle::VectorAngles(Vector(direction.x, direction.y, direction.z), up, m_GrabUseAngles);

	const unsigned long long now = GetTickCount64();
	m_GrabUseAimUntil = now + 500;
	m_GrabUsePressAt = now + 60;     // after the aim has reached the server
	m_GrabUsePressUntil = now + 220;

	// For the left hand: what's carried now, to know what's new
	const bool toLeftHand = leftHand && m_DualWieldEnabled && !target.door;
	if (toLeftHand)
	{
		C_BasePlayer *local = (C_BasePlayer *)m_Game->GetClientEntity(m_Game->m_EngineClient->GetLocalPlayer());
		InventoryItem items[64];
		const int count = GetInventory(local, items, 64);
		m_InventoryBeforeCount = 0;
		for (int i = 0; i < count; ++i)
			m_InventoryBefore[m_InventoryBeforeCount++] = items[i].index;
		m_LeftGrabUntil = now + 1500;
	}

	char command[128];
	snprintf(command, sizeof(command), "l4d2vr_use_aim %.2f %.2f %.2f %.2f %.2f %.2f %d", m_GrabUseOrigin.x, m_GrabUseOrigin.y, m_GrabUseOrigin.z, m_GrabUseAngles.x, m_GrabUseAngles.y, m_GrabUseAngles.z, toLeftHand ? 1 : 0);
	m_Game->m_EngineClient->ServerCmd(command, true);

	Diagnostics::Log("grab: %s, model %d, %.2f m from the hand", target.door ? "door" : "item", target.modelIndex, target.distance / m_VRScale);
	return true;
}

bool VR::GetGrabUseAim(Vector &origin, QAngle &angles) const
{
	if (GetTickCount64() >= m_GrabUseAimUntil)
		return false;
	origin = m_GrabUseOrigin;
	angles = m_GrabUseAngles;
	return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Two pistols shown as one
// ---------------------------------------------------------------------------------------------------------------

namespace
{
	// The effects of the left pistol's shots come out of the right one: its attachments are replaced in the model
	struct AttachmentPair { const char *right; const char *left; };
	const AttachmentPair kPistolAttachments[3] = { { "muzzle_flash_r", "muzzle_flash_l" }, { "shell_r", "shell_l" }, { "muzzlesmoke_r", "muzzlesmoke_l" } };
	const void *g_OnePistolHeader = nullptr;
	bool g_OnePistolMoved = false;
	unsigned char g_OnePistolSaved[3][Studio::kAttachmentPlacementSize];

	// Where the dual pistols' right pistol has to go so it's where the single pistol is, measured from both models in
	// the game: relative to the weapon's frame (x forward, y left, z up)
	Mat34 MakeOnePistolCorrection()
	{
		Mat34 correction;
		SetAxis(correction, 3, Vec3(-1.5879f, 0.6982f, -0.3613f));
		return correction;
	}
	const Mat34 kOnePistolCorrection = MakeOnePistolCorrection();
}

void VR::UpdateOnePistolModel(const void *studioHeader, bool oneShown)
{
	if (!Studio::IsValid(studioHeader))
		return;

	if (g_OnePistolHeader != studioHeader)
	{
		g_OnePistolHeader = studioHeader;
		g_OnePistolMoved = false;
		for (int i = 0; i < 3; ++i)
			memcpy(g_OnePistolSaved[i], Studio::AttachmentPlacement(studioHeader, Studio::FindAttachment(studioHeader, kPistolAttachments[i].left)), Studio::kAttachmentPlacementSize);
	}

	if (oneShown == g_OnePistolMoved)
		return;
	g_OnePistolMoved = oneShown;

	for (int i = 0; i < 3; ++i)
	{
		void *left = Studio::AttachmentPlacement(studioHeader, Studio::FindAttachment(studioHeader, kPistolAttachments[i].left));
		if (oneShown)
			memcpy(left, Studio::AttachmentPlacement(studioHeader, Studio::FindAttachment(studioHeader, kPistolAttachments[i].right)), Studio::kAttachmentPlacementSize);
		else
			memcpy(left, g_OnePistolSaved[i], Studio::kAttachmentPlacementSize);
	}
}

// Called at the end of the tracking: the dual pistols model is placed so its right pistol is where the single one is
void VR::ApplyOnePistolPlacement()
{
	if (!m_DualAsOnePistol)
		return;

	const Mat34 frame = FromAxes(ToVec3(m_ViewmodelForward), ToVec3(m_ViewmodelRight) * -1.0f, ToVec3(m_ViewmodelUp), ToVec3(GetRecommendedViewmodelAbsPos()));
	const Mat34 placed = Multiply(frame, kOnePistolCorrection);

	auto toVector = [](const Vec3 &v) { return Vector(v.x, v.y, v.z); };
	m_ViewmodelForward = toVector(Axis(placed, 0));
	m_ViewmodelRight = toVector(Axis(placed, 1) * -1.0f);
	m_ViewmodelUp = toVector(Axis(placed, 2));

	const Vec3 fromController = Origin(placed) - ToVec3(GetRightControllerAbsPos());
	m_ViewmodelPosOffset = Vector(-Dot(fromController, Axis(placed, 0)), Dot(fromController, Axis(placed, 1)), -Dot(fromController, Axis(placed, 2)));
}
