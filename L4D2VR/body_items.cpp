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
	bool FindClassProp(void *firstClass, const char *className, const char *name, int &offset, int &count)
	{
		__try
		{
			for (const ClientClass *clientClass = (const ClientClass *)firstClass; clientClass; clientClass = clientClass->next)
			{
				if (clientClass->networkName && strcmp(clientClass->networkName, className) == 0)
					return FindProp(clientClass->table, name, 0, 0, offset, count);
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
		return false;
	}

	bool FindPlayerProp(void *firstClass, const char *name, int &offset, int &count)
	{
		return FindClassProp(firstClass, "CTerrorPlayer", name, offset, count);
	}

	const int kClientAmmoArrayOffset = 0xF24; // known, checks the lookup
	int g_MyWeaponsOffset = -1;
	int g_MyWeaponsCount = 0;
	int g_ModelIndexOffset = -1;
	int g_OwnerOffset = -1;
	// The model of a weapon in the world (DT_BaseCombatWeapon). The weapons a player carries have their first person
	// model as their own (m_nModelIndex), with the parts placed for the animations.
	int g_WorldModelIndexOffset = -1;
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
			FindClassProp(classes, "CPistol", "m_iWorldModelIndex", g_WorldModelIndexOffset, unused);
		}
		Diagnostics::Log("body items: m_nModelIndex at 0x%X, m_hOwnerEntity at 0x%X, m_iWorldModelIndex at 0x%X", g_ModelIndexOffset, g_OwnerOffset,
			g_WorldModelIndexOffset);

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

	// An item standing upright on the body, facing forward, `offset` meters away in the body frame. Each model is long
	// along its own axis, as drawn without animation (their vertices, not their hulls, which are turned for some): the
	// pills, the molotov and the pipe bomb along z, the bile jar along y, the adrenaline along x, its needle at +x.
	Mat34 ChestItemFrame(const Mat34 &body, const Vec3 &offset, int weaponId, float unitsPerMeter)
	{
		const Vec3 point = TransformPoint(body, offset * unitsPerMeter);
		const Vec3 forward = Axis(body, 0), up(0.0f, 0.0f, 1.0f), left = Cross(up, forward);
		switch (weaponId)
		{
		case C_WeaponCSBase::VOMITJAR:
			return FromAxes(forward, up, Cross(forward, up), point);
		case C_WeaponCSBase::ADRENALINE:
			// The needle down, like in a pocket
			return FromAxes(up * -1.0f, left, forward, point);
		default:
			return FromAxes(forward, left, up, point);
		}
	}

	// The middle of each model's vertices, put at the item's place
	Vec3 ChestItemCenter(int weaponId)
	{
		switch (weaponId)
		{
		case C_WeaponCSBase::PAIN_PILLS: return Vec3(0.0f, -0.1f, 3.0f);
		case C_WeaponCSBase::ADRENALINE: return Vec3(1.4f, 0.0f, 0.8f);
		case C_WeaponCSBase::MOLOTOV: return Vec3(0.6f, 0.0f, 1.9f);
		case C_WeaponCSBase::PIPE_BOMB: return Vec3(0.35f, -0.1f, 1.45f);
		case C_WeaponCSBase::VOMITJAR: return Vec3(-0.1f, 0.55f, 0.0f);
		default: return Vec3();
		}
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

	// What the right hand put away (EmptyRightHand) is seen where it's carried
	const void *active = m_RightHandEmpty ? nullptr : (const void *)local->GetActiveWeapon();
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
	const Vec3 holsterMoved(m_HolsterForward, (m_LeftHanded ? 1.0f : -1.0f) * m_HolsterOutward, m_HolsterUp);
	m_HolsterPoint = TransformPoint(body, (holster.point + holsterMoved) * m_VRScale);
	m_HolsterBarrel = RotateVector(body, holster.barrel);
	m_HolsterValid = true;

	// The left hand's item
	BodyModelDraw leftItem;
	if (m_BodyDrawCount < kMaxBodyDraws && GetLeftItemDraw(leftItem))
	{
		leftItem.leftGun = LeftGun::Find(m_LeftItemId) != nullptr;
		m_BodyDraws[m_BodyDrawCount++] = leftItem;
		// The magazine the empty right hand takes for it, in its fist
		if (m_RightMagazine && m_LeftGunMagazineShown && leftItem.leftGun && m_BodyDrawCount < kMaxBodyDraws && GetTickCount64() - m_RightGripMs < 250)
		{
			BodyModelDraw magazine;
			magazine.model = leftItem.model;
			magazine.frame = m_RightGripFrame;
			magazine.magazine = true;
			m_BodyDraws[m_BodyDrawCount++] = magazine;
		}
	}

	// The pistol on the thigh of the weapon hand, pointing down along it, where the grab takes it (IsAtHolster). Its
	// weapon bone has y along the grip and z along the barrel: the grip sticks out backwards.
	for (int i = 0; i < g_MyWeaponsCount && m_BodyDrawCount < kMaxBodyDraws; ++i)
	{
		const int handle = Read<int>(local, g_MyWeaponsOffset + i * 4);
		if (!IsValidHandle(handle))
			continue;
		C_WeaponCSBase *weapon = (C_WeaponCSBase *)m_Game->m_ClientEntityList->GetClientEntityFromHandle(handle);
		// The left hand's, in it or behind the left shoulder, isn't on the thigh
		const int index = handle & kHandleIndexMask;
		if (!weapon || weapon == active || index == m_LeftItemIndex || index == m_ExtraWeaponIndex)
			continue;
		const int weaponId = weapon->GetWeaponID();
		if (weaponId != C_WeaponCSBase::PISTOL && weaponId != C_WeaponCSBase::MAGNUM)
			continue;

		const void *model = GetWorldModel(weapon);
		const bool inView = Dot(Normalized(m_HolsterPoint - ToVec3(m_HmdPosAbs)), ToVec3(m_HmdForward)) > kChestViewCosine;
		if (model && inView)
		{
			const Vec3 slide = RotateVector(body, holster.slide);
			BodyModelDraw draw;
			draw.model = model;
			draw.frame = FromAxes(Cross(slide, m_HolsterBarrel), slide, m_HolsterBarrel, m_HolsterPoint);
			draw.alignWeaponBone = true;
			draw.onePistol = true; // two pistols are one on the thigh
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
			draw.hasCenter = true;
			draw.center = ChestItemCenter(weaponId);
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

// Pills are taken by bringing them to the mouth and adrenaline by stabbing it into the body, with the trigger of the hand
// that holds them pressed. With the mod on the host they're used right there, without the game's animation, from either
// hand (DualWield::ServerUseItem); otherwise the right trigger only attacks there. False while the right trigger
// mustn't attack.
bool VR::UpdateItemUse()
{
	const bool hostHasMod = m_Game->m_PoseRelay->IsServerCapable();
	auto usedAtBody = [](int weaponId) { return weaponId == C_WeaponCSBase::PAIN_PILLS || weaponId == C_WeaponCSBase::ADRENALINE; };
	const bool rightItem = !m_LeftUseActive && !m_RightHandEmpty && usedAtBody(m_ActiveWeaponId);
	const bool leftItem = LeftHandHoldsItem() && usedAtBody(m_LeftItemId);

	// The game's sounds, as its animations play them (they aren't played): the adrenaline's cap comes off as the trigger
	// is pressed, the needle shows
	const bool leftTrigger = PressedDigitalAction(m_ActionSecondaryAttack), rightTrigger = PressedDigitalAction(m_ActionPrimaryAttack);
	if (hostHasMod && ((leftItem && m_LeftItemId == C_WeaponCSBase::ADRENALINE && leftTrigger && !m_ItemTriggerDown[0])
		|| (rightItem && m_ActiveWeaponId == C_WeaponCSBase::ADRENALINE && rightTrigger && !m_ItemTriggerDown[1])))
		m_Game->ClientCmd_Unrestricted("playgamesound Adrenaline.CapOff");
	m_ItemTriggerDown[0] = leftTrigger;
	m_ItemTriggerDown[1] = rightTrigger;

	int itemIndex = 0;
	bool leftHand = false;
	if (leftItem && PressedDigitalAction(m_ActionSecondaryAttack) && ItemUseGestureReady(true, m_LeftItemId))
	{
		itemIndex = m_LeftItemIndex;
		leftHand = true;
	}
	else if (rightItem && PressedDigitalAction(m_ActionPrimaryAttack) && ItemUseGestureReady(false, m_ActiveWeaponId))
	{
		itemIndex = -1;
		C_BasePlayer *local = (C_BasePlayer *)m_Game->GetClientEntity(m_Game->m_EngineClient->GetLocalPlayer());
		InventoryItem items[64];
		const int count = local ? GetInventory(local, items, 64) : 0;
		for (int i = 0; i < count; ++i)
		{
			if (items[i].weapon == (void *)local->GetActiveWeapon())
				itemIndex = items[i].index;
		}
	}

	const bool ready = itemIndex != 0;
	if (ready && !m_ItemGestureReady)
	{
		m_Haptics.Play(leftHand ? Haptics::Hand::OtherHand : Haptics::Hand::WeaponHand, HapticEffects::Confirm);
		if (hostHasMod && itemIndex > 0)
		{
			char command[48];
			snprintf(command, sizeof(command), "l4d2vr_use_item %d", itemIndex);
			m_Game->m_EngineClient->ServerCmd(command, true);
			const bool pills = (leftHand ? m_LeftItemId : m_ActiveWeaponId) == C_WeaponCSBase::PAIN_PILLS;
			m_Game->ClientCmd_Unrestricted(pills ? "playgamesound PainPills.Use" : "playgamesound Adrenaline.NeedleIn");
			Diagnostics::Log("items: %s at the %s, entity %d", leftHand ? "left hand's" : "right hand's",
				(leftHand ? m_LeftItemId : m_ActiveWeaponId) == C_WeaponCSBase::PAIN_PILLS ? "mouth" : "body", itemIndex);
		}
	}
	m_ItemGestureReady = ready;
	return !rightItem || (!hostHasMod && ready);
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

// The model of a weapon as it's seen in the world and in other players' hands, which is what a hand holds and the body
// carries. The weapons a player has are drawn by the game with their first person model instead (GetEntityModel), whose
// parts are placed for its animations: the magazine out, the syringe away from the hand.
const void *VR::GetWorldModel(const void *weapon)
{
	if (!g_NetvarsLookedUp)
		LookUpNetvars(m_Game);
	if (!weapon || g_WorldModelIndexOffset < 0)
		return GetEntityModel(weapon);
	const int modelIndex = Read<int>(weapon, g_WorldModelIndexOffset);
	const void *model = modelIndex > 0 && modelIndex < 0x10000 ? m_Game->m_ModelInfo->GetModel(modelIndex) : nullptr;
	return model ? model : GetEntityModel(weapon);
}

namespace
{
	// The bone a model is held by in the third person ("ValveBiped.weapon_bone", "L_weapon_bone"), -1 if none. A name
	// ending with it: the model of two pistols has "weapon_bone_Clip2" (the second one's magazine) before its own.
	int WeaponBoneOf(const void *studioHeader)
	{
		static std::unordered_map<const void *, int> s_Found;
		auto found = s_Found.find(studioHeader);
		if (found != s_Found.end())
			return found->second;

		const char kName[] = "weapon_bone";
		const size_t kLength = sizeof(kName) - 1;
		int bone = -1, containing = -1;
		const int count = Studio::BoneCount(studioHeader);
		for (int i = 0; i < count && bone < 0; ++i)
		{
			const char *name = Studio::BoneName(studioHeader, i);
			const size_t length = strlen(name);
			if (length >= kLength && strcmp(name + length - kLength, kName) == 0)
				bone = i;
			else if (containing < 0 && strstr(name, kName))
				containing = i;
		}
		if (bone < 0)
			bone = containing;
		s_Found[studioHeader] = bone;
		return bone;
	}

	// A weapon's magazine in its world model: "weapon_bone_Clip", and the bullets in it, -1 if none. And its muzzle: the
	// "muzzle_flash" attachment, on a bone, and its ejection port: the "shell" one, pointing out of it.
	struct ClipBones
	{
		int clip = -1, bullets = -1;
		int muzzleBone = -1;
		Mat34 muzzleOnBone;
		int shellBone = -1;
		Mat34 shellOnBone;
		int bolt = -1;   // what's pulled: the slide, the charging handle or the bolt
		const LeftGun::GunModel *parts = nullptr; // where its parts really are, for the game's guns
	};

	const ClipBones &ClipBonesOf(const void *studioHeader)
	{
		static std::unordered_map<const void *, ClipBones> s_Found;
		auto found = s_Found.find(studioHeader);
		if (found != s_Found.end())
			return found->second;
		ClipBones bones;
		const int count = Studio::BoneCount(studioHeader);
		for (int i = 0; i < count; ++i)
		{
			const char *name = Studio::BoneName(studioHeader, i);
			const size_t length = strlen(name);
			if (length >= 16 && strcmp(name + length - 16, "weapon_bone_Clip") == 0 && bones.clip < 0)
				bones.clip = i;
			else if (length >= 24 && strcmp(name + length - 24, "weapon_bone_Clip_Bullets") == 0 && bones.bullets < 0)
				bones.bullets = i;
		}
		const char *const kBolts[] = { "weapon_charger_slide", "weapon_charger", "weapon_bolt", "weapon_bolt_opposite" };
		for (const char *wanted : kBolts)
		{
			for (int i = 0; i < count && bones.bolt < 0; ++i)
			{
				const char *name = Studio::BoneName(studioHeader, i);
				const size_t length = strlen(name), wantedLength = strlen(wanted);
				if (length >= wantedLength && _stricmp(name + length - wantedLength, wanted) == 0)
					bones.bolt = i;
			}
		}
		const uint8_t *base = (const uint8_t *)studioHeader;
		const int32_t attachments = *(const int32_t *)(base + 240), attachmentIndex = *(const int32_t *)(base + 244);
		for (int i = 0; i < attachments; ++i)
		{
			const uint8_t *attachment = base + attachmentIndex + i * 92;
			const char *name = (const char *)attachment + *(const int32_t *)attachment;
			if (_stricmp(name, "muzzle_flash") == 0 && bones.muzzleBone < 0)
			{
				bones.muzzleBone = *(const int32_t *)(attachment + 8);
				memcpy(&bones.muzzleOnBone, attachment + 12, sizeof(float) * 12);
			}
			else if (_stricmp(name, "shell") == 0 && bones.shellBone < 0)
			{
				bones.shellBone = *(const int32_t *)(attachment + 8);
				memcpy(&bones.shellOnBone, attachment + 12, sizeof(float) * 12);
			}
		}
		bones.parts = LeftGun::FindModel(Studio::ModelName(studioHeader));
		return s_Found[studioHeader] = bones;
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

	// The second pistol of the models of two pistols: on the left hand bone and its children ("weapon_bone_Clip2",
	// "weapon_bolt_l", "flash_L")
	const std::vector<int> &SecondPistolBonesOf(const void *studioHeader)
	{
		static std::unordered_map<const void *, std::vector<int>> s_Found;
		auto found = s_Found.find(studioHeader);
		if (found != s_Found.end())
			return found->second;

		std::vector<int> bones;
		const int count = Studio::BoneCount(studioHeader);
		for (int i = 0; i < count; ++i)
		{
			const char *name = Studio::BoneName(studioHeader, i);
			const size_t length = strlen(name);
			if (strstr(name, "L_Hand") || strstr(name, "Clip2") || strstr(name, "flash_L") || (length > 2 && _stricmp(name + length - 2, "_l") == 0))
				bones.push_back(i);
		}
		return s_Found.emplace(studioHeader, std::move(bones)).first->second;
	}
}

// The left hand's gun as it's drawn (BuildBodyModelBones): where its magazine goes and its end that sticks out, its
// bolt (pulled back, or kicked back by a shot: ApplyLeftGunShot), its muzzle and ejection port, kept for the reload
// by hand (UpdateLeftGunReload) and the shots. The parts are where the model has them (LeftGun::GunModel), or its bones
// for a model that isn't one of the game's.
void VR::PlaceLeftGunParts(const void *clipBones, const Mat34 &frame, Mat34 *bones, int boneCount)
{
	const ClipBones &clip = *(const ClipBones *)clipBones;
	// The magazine of a model that isn't one of the game's: from its bone to its end that sticks out, units
	const float kUnknownMagazineLength = 3.5f;
	const LeftGun::GunModel *parts = clip.parts;
	const Vec3 barrel = Normalized(Axis(frame, 2)), up = Normalized(Axis(frame, 1));
	const float pull = m_LeftGunDrawnPull;
	const unsigned long long now = GetTickCount64();
	const bool hasClip = clip.clip >= 0 && clip.clip < boneCount;

	if (parts)
	{
		m_LeftGunWell = TransformPoint(frame, parts->magazine);
		m_LeftGunMagazineBase = TransformPoint(frame, parts->magazine - Vec3(0.0f, parts->magazineHalf + 0.5f, 0.0f));
		m_LeftGunWellMs = now;
	}
	else if (hasClip)
	{
		m_LeftGunWell = Origin(bones[clip.clip]);
		m_LeftGunMagazineBase = m_LeftGunWell - up * kUnknownMagazineLength;
		m_LeftGunWellMs = now;
	}
	if (hasClip)
	{
		const bool bullets = clip.bullets >= 0 && clip.bullets < boneCount;
		if (m_LeftGunMagazineOut)
		{
			// Out of it while the right hand reloads it
			bones[clip.clip] = CollapsedAt(m_LeftGunWell);
			if (bullets)
				bones[clip.bullets] = CollapsedAt(m_LeftGunWell);
		}
		else if (parts && parts->pumpIsClip)
		{
			// The forend of a pump shotgun goes back with the pump
			SetAxis(bones[clip.clip], 3, Origin(bones[clip.clip]) - barrel * pull);
			if (bullets)
				SetAxis(bones[clip.bullets], 3, Origin(bones[clip.bullets]) - barrel * pull);
		}
	}

	// The bolt pulled back along the barrel, and where it is taken
	const bool hasBolt = clip.bolt >= 0 && clip.bolt < boneCount;
	if (hasBolt && !(parts && parts->pumpIsClip))
		SetAxis(bones[clip.bolt], 3, Origin(bones[clip.bolt]) - barrel * pull);
	m_LeftGunHasBolt = parts || hasBolt;
	if (parts)
		m_LeftGunBolt = TransformPoint(frame, parts->bolt) - barrel * pull;
	else if (hasBolt)
		m_LeftGunBolt = Origin(bones[clip.bolt]);

	if (clip.muzzleBone >= 0 && clip.muzzleBone < boneCount)
	{
		m_LeftGunMuzzle = Origin(Multiply(bones[clip.muzzleBone], clip.muzzleOnBone));
		m_LeftGunMuzzleMs = now;
	}
	if (clip.shellBone >= 0 && clip.shellBone < boneCount)
	{
		const Mat34 shell = Multiply(bones[clip.shellBone], clip.shellOnBone);
		m_LeftGunShell = Origin(shell);
		m_LeftGunShellDir = Normalized(Axis(shell, 0));
		m_LeftGunShellMs = now;
	}

	// The mark (UpdateLeftGunReload): relative to the left controller in its OpenVR frame (x right, y up, z back). It
	// faces the eyes, its arrow the way the hand goes: down off the magazine, up where the new one goes, back on the bolt.
	if (m_LeftMarkKind != 0)
	{
		const Vec3 point = m_LeftMarkKind == 1 ? m_LeftGunMagazineBase : m_LeftMarkKind == 2 ? m_LeftGunWell : m_LeftGunBolt;
		const Vec3 controllerRight = ToVec3(m_LeftControllerRightRaw), controllerUp = ToVec3(m_LeftControllerUpRaw), controllerForward = ToVec3(m_LeftControllerForwardRaw);
		auto toController = [&](const Vec3 &v) { return Vec3(Dot(v, controllerRight), Dot(v, controllerUp), -Dot(v, controllerForward)); };
		const Vec3 normal = Normalized(ToVec3(m_HmdPosAbs) - point);
		Vec3 along = m_LeftMarkKind == 1 ? up * -1.0f : m_LeftMarkKind == 2 ? up : barrel * -1.0f;
		along = Normalized(along - normal * Dot(along, normal), Normalized(Cross(Vec3(0.0f, 0.0f, 1.0f), normal)));
		const Vec3 x = toController(along), y = toController(Cross(normal, along)), z = toController(normal);
		const Vec3 p = toController(point - ToVec3(m_LeftControllerPosAbs)) * (1.0f / m_VRScale);
		m_LeftMarkerTransform = { { { x.x, y.x, z.x, p.x }, { x.y, y.y, z.y, p.y }, { x.z, y.z, z.z, p.z } } };
		m_LeftMarkerPlaced = IsFinite(FromAxes(x, y, z, p));
	}
}

// The bones of an item drawn on the body: the model as it was made, its center at the frame, at its scale
void VR::BuildBodyModelBones(const void *studioHeader, const BodyModelDraw &draw, void *outBones)
{
	Mat34 *bones = (Mat34 *)outBones;
	const int boneCount = std::clamp(Studio::BoneCount(studioHeader), 0, Studio::kMaxBones);

	// Held: the bone the model is held by goes where the hand holds it
	const SyringeBones *syringe = draw.syringe ? &SyringeBonesOf(studioHeader) : nullptr;
	const ClipBones *clip = draw.magazine || draw.leftGun ? &ClipBonesOf(studioHeader) : nullptr;
	const int heldBone = syringe ? syringe->syringe : draw.magazine ? clip->clip : draw.alignWeaponBone ? WeaponBoneOf(studioHeader) : -1;
	if (heldBone >= 0 && heldBone < boneCount)
	{
		Mat34 frame = draw.frame;
		// The magazine of one of the game's guns is held by its middle (its bone is elsewhere)
		const int weaponBone = draw.magazine && clip->parts ? WeaponBoneOf(studioHeader) : -1;
		if (weaponBone >= 0 && weaponBone < boneCount)
		{
			const Mat34 weaponToHeld = Multiply(Studio::PoseToBone(studioHeader, heldBone), InvertRigid(Studio::PoseToBone(studioHeader, weaponBone)));
			SetAxis(frame, 3, TransformPoint(frame, TransformPoint(weaponToHeld, clip->parts->magazine) * -1.0f));
		}
		const Mat34 placed = Multiply(frame, Studio::PoseToBone(studioHeader, heldBone));
		for (int i = 0; i < boneCount; ++i)
			bones[i] = Multiply(placed, InvertRigid(Studio::PoseToBone(studioHeader, i)));
		// Only the magazine: the rest of the weapon folded away in it
		if (draw.magazine)
		{
			for (int i = 0; i < boneCount; ++i)
			{
				if (i != clip->clip && i != clip->bullets)
					bones[i] = CollapsedAt(Origin(draw.frame));
			}
		}
		if (draw.leftGun)
			PlaceLeftGunParts(clip, draw.frame, bones, boneCount);
		if (syringe)
		{
			for (int piece : syringe->capPieces)
			{
				if (piece >= 0 && piece < boneCount)
					bones[piece] = CollapsedAt(Origin(draw.frame));
			}
		}
		if (draw.onePistol)
		{
			for (int bone : SecondPistolBonesOf(studioHeader))
			{
				if (bone < boneCount)
					bones[bone] = CollapsedAt(Origin(draw.frame));
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
		draw.hasCenter = true;
		draw.center = ChestItemCenter(weaponId);
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
		bool ammo = false;
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

		// Any weapon or item, but the cola bottles, carried in the arms
		return rule == LeftHandDualWield && !strstr(lowerName, "w_cola");
	}

	// A door that opens, by its model: props_doors/, and others like c1m1's props_downtown/metal_door_112 (the class of the
	// entity on the network would say it, but calling GetClientClass of the entities took the game down)
	bool IsDoor(const char *lowerName)
	{
		const char *file = strrchr(lowerName, '/');
		file = file ? file + 1 : lowerName;
		return (strstr(lowerName, "props_doors/") || strstr(file, "door")) && !strstr(lowerName, "frame") && !strstr(lowerName, "reinforcement")
			&& !strstr(lowerName, "gib") && !strstr(lowerName, "dummy") && !strstr(lowerName, "crossbar") && !strstr(lowerName, "damaged")
			&& !strstr(file, "trim") && !strstr(file, "knob") && !strstr(file, "handle");
	}

	// Ammo for the weapon: piles, cans and the boxes of special ammo put down (not the packs, which are carried)
	bool IsAmmo(const char *lowerName)
	{
		return !strstr(lowerName, "_ammopack") && (strstr(lowerName, "terror/ammo_stack") || strstr(lowerName, "ammo_can") || strstr(lowerName, "coffeeammo")
			|| strstr(lowerName, "exploding_ammo") || strstr(lowerName, "incendiary_ammo"));
	}

	void Consider(WorldTarget &best, const Vec3 &hand, const Vec3 &point, float reach, bool door, int modelIndex, bool ammo = false)
	{
		const float distance = Length(point - hand);
		if (distance < reach && distance < best.distance)
		{
			best.found = true;
			best.door = door;
			best.ammo = ammo;
			best.point = point;
			best.distance = distance;
			best.modelIndex = modelIndex;
		}
	}

	// What the mod needs of an entity in the world, each read on its own: one that can't be read (a wrong guess about
	// it) is skipped, and the others still are looked at
	struct WorldEntity
	{
		const uint8_t *entity = nullptr;
		int modelIndex = 0;
		char lower[128] = {}; // the model's path, lower case
		int owner = 0;
		Vec3 origin, mins, maxs;
		QAngle angles;
	};
	int g_UnreadableEntities = 0, g_FirstUnreadableEntity = 0, g_UnreadableStep = 0;

	bool ReadWorldEntity(Game *game, int index, WorldEntity &out)
	{
		volatile int step = 0;
		__try
		{
			out.entity = (const uint8_t *)game->m_ClientEntityList->GetClientEntity(index);
			if (!out.entity)
				return false;
			out.modelIndex = *(const int16_t *)(out.entity + g_ModelIndexOffset);
			void *model = out.modelIndex != 0 ? game->m_ModelInfo->GetModel(out.modelIndex) : nullptr;
			const char *name = model ? game->m_ModelInfo->GetModelName(model) : nullptr;
			if (!name)
				return false;
			strncpy_s(out.lower, name, _TRUNCATE);
			_strlwr_s(out.lower);
			step = 1;
			out.mins = *(const Vec3 *)((const uint8_t *)model + 0x118);
			out.maxs = *(const Vec3 *)((const uint8_t *)model + 0x124);
			step = 2;
			out.owner = *(const int *)(out.entity + g_OwnerOffset);

			// IClientRenderable::GetRenderOrigin and GetRenderAngles
			void *renderable = (uint8_t *)out.entity + 4;
			void **vtable = *(void ***)renderable;
			typedef const Vector &(__thiscall *tGetOrigin)(void *thisptr);
			typedef const QAngle &(__thiscall *tGetAngles)(void *thisptr);
			step = 4;
			const Vector &origin = ((tGetOrigin)vtable[1])(renderable);
			step = 5;
			out.origin = Vec3(origin.x, origin.y, origin.z);
			out.angles = ((tGetAngles)vtable[2])(renderable);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			if (!g_UnreadableEntities++)
			{
				g_FirstUnreadableEntity = index;
				g_UnreadableStep = step;
			}
			return false;
		}
	}

	// The handle of a door: on the free edge, a bit under the middle of its height. Doors turn around their origin, or
	// around the middle of the model when it has two leaves: then either edge has one.
	void ConsiderDoorHandles(WorldTarget &best, const Vec3 &hand, const WorldEntity &door, float reach)
	{
		const Vec3 &origin = door.origin, &mins = door.mins, &maxs = door.maxs;
		const QAngle &angles = door.angles;
		const int modelIndex = door.modelIndex;

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
	void FindWorldTarget(Game *game, const Vec3 &hand, float unitsPerMeter, WorldTarget &best, float itemReach = 0.0f, const char *filter = nullptr, LeftHandRule rule = AnyHand,
		bool doorsOnly = false)
	{
		const int highest = game->m_ClientEntityList->GetHighestEntityIndex();
		for (int i = 33; i <= highest; ++i)
		{
			WorldEntity entity;
			if (!ReadWorldEntity(game, i, entity))
				continue;

			const bool pickable = IsPickable(entity.lower) && TakenByLeftHand(entity.lower, rule);
			const bool door = !IsPickable(entity.lower) && IsDoor(entity.lower);
			if ((!pickable && !door) || (filter && !strstr(entity.lower, filter)) || (doorsOnly && !door))
				continue;

			// Carried by somebody
			if (pickable && entity.owner != 0 && entity.owner != -1)
				continue;

			if (pickable)
				Consider(best, hand, entity.origin, itemReach > 0.0f ? itemReach : kWorldItemReach * unitsPerMeter, false, entity.modelIndex, IsAmmo(entity.lower));
			else if (itemReach <= 0.0f || doorsOnly)
				ConsiderDoorHandles(best, hand, entity, doorsOnly ? itemReach : kDoorHandleReach * unitsPerMeter);
		}
	}
}

// The doors near `from`, in the log (l4d2vr_dual_test doors): their class, model and distance
void VR::LogDoors(const Vec3 &from)
{
	if (!g_NetvarsLookedUp)
		LookUpNetvars(m_Game);
	int logged = 0;
	g_UnreadableEntities = 0;
	const int highest = m_Game->m_ClientEntityList->GetHighestEntityIndex();
	for (int i = 33; i <= highest && logged < 12; ++i)
	{
		WorldEntity entity;
		if (!ReadWorldEntity(m_Game, i, entity) || !strstr(entity.lower, "door") || Length(entity.origin - from) > 600.0f)
			continue;
		Diagnostics::Log("doors: entity %d %s%s, %.0f units away, turned %.0f", i, entity.lower, IsDoor(entity.lower) ? "" : " (not taken for a door)",
			Length(entity.origin - from), entity.angles.y);
		++logged;
	}
	Diagnostics::Log("doors: %d listed, %d entities couldn't be read (the first %d, at step %d)", logged, g_UnreadableEntities, g_FirstUnreadableEntity, g_UnreadableStep);
}

// The weapon or item lying nearest to `from`, within radiusUnits (the test of the dual wield)
bool VR::FindNearestPickable(const Vec3 &from, float radiusUnits, Vec3 &point, const char *filter)
{
	if (!g_NetvarsLookedUp)
		LookUpNetvars(m_Game);
	if (g_ModelIndexOffset < 0 || g_OwnerOffset < 0)
		return false;

	WorldTarget target;
	const bool doors = filter && strcmp(filter, "door") == 0;
	FindWorldTarget(m_Game, from, m_VRScale, target, radiusUnits, doors ? nullptr : filter, AnyHand, doors);
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

	// Ammo is taken with an empty hand: the right one with its weapon put away (EmptyRightHand), the left one without
	// its item
	if (target.ammo && (leftHand ? m_LeftItemIndex != 0 : !m_RightHandEmpty))
	{
		Diagnostics::Log("grab: ammo, the %s hand isn't empty", leftHand ? "left" : "right");
		return false;
	}

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
	// The ammo is for the left hand's gun too
	if (target.ammo)
		GiveLeftGunAmmo();

	void *model = m_Game->m_ModelInfo->GetModel(target.modelIndex);
	const char *modelName = model ? m_Game->m_ModelInfo->GetModelName(model) : nullptr;
	Diagnostics::Log("grab: %s, %s, %.2f m from the hand, with the %s hand", target.door ? "door" : "item", modelName ? modelName : "no model",
		target.distance / m_VRScale, leftHand ? "left" : "right");
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
