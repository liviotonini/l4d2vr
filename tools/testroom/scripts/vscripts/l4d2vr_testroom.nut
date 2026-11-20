// L4D2VR test room: the script of the map l4d2vr_testroom (a copy of c2m1_highway with a logic_script that runs this
// file). It puts every weapon and item on tables around the survivors, with no infected and no bots, to test the VR
// mod: guns in front, items and the laser sight machine on the left, melee weapons on the right, grenades behind and
// doors further behind. The map's info_director calls L4D2VR_TestRoom() a second after gameplay starts (when the intro
// has put the survivors in place). To put the tables around you again, wherever you are:
//   sv_cheats 1   then   script L4D2VR_TestRoom(true)

// No infected and no bots. The director reads its options through GetDirectorOptions() (director_base.nut), which
// merges DirectorScript, MapScript, LocalScript and ChallengeScript; MapScript only lives for this map.
::L4D2VR_DirectorOptions <-
{
	cm_NoSurvivorBots = 1
	cm_CommonLimit = 0
	cm_MaxSpecials = 0
	cm_ProhibitBosses = true
	cm_WitchLimit = 0
	CommonLimit = 0
	MaxSpecials = 0
	ProhibitBosses = true
	WitchLimit = 0
	NoMobSpawns = true
	WanderingZombieDensityModifier = 0
}

::L4D2VR_ApplyDirectorOptions <- function()
{
	local scope = getroottable().DirectorScript.MapScript
	if (!scope.rawin("DirectorOptions"))
		scope.DirectorOptions <- {}
	foreach (key, value in ::L4D2VR_DirectorOptions)
		scope.DirectorOptions[key] <- value
}

const TABLE_MODEL = "models/props_interiors/table_folding.mdl"
const AMMO_MODEL = "models/props/terror/ammo_stack.mdl"
const TABLE_LENGTH = 91.0 // along the model's y axis
const TABLE_TOP = 32.0
const ITEM_FLAGS = 10 // 2: must exist, 8: infinite items

// Distances from the survivors (their center when there are several): the space between the tables fits the four
// spots where the intro leaves the survivors, so no table is spawned on top of a player
const FRONT_DISTANCE = 75.0
const SIDE_DISTANCE = 185.0
const BACK_DISTANCE = 85.0
const DOORS_DISTANCE = 190.0

// Front tables, from the player's right to the left
::L4D2VR_Guns <- [
	"weapon_pistol_spawn", "weapon_pistol_magnum_spawn",
	"weapon_smg_spawn", "weapon_smg_silenced_spawn", "weapon_smg_mp5_spawn",
	"weapon_pumpshotgun_spawn", "weapon_shotgun_chrome_spawn", "weapon_autoshotgun_spawn", "weapon_shotgun_spas_spawn",
	"weapon_rifle_spawn", "weapon_rifle_ak47_spawn", "weapon_rifle_desert_spawn", "weapon_rifle_sg552_spawn",
	"weapon_hunting_rifle_spawn", "weapon_sniper_military_spawn", "weapon_sniper_awp_spawn", "weapon_sniper_scout_spawn",
	"weapon_grenade_launcher_spawn", "weapon_rifle_m60_spawn", "weapon_chainsaw_spawn"
]

// Left table, from the front to the back: the laser sight machine (where the grenades used to be), then the items in
// the same places as before. Each laser box gives the laser once to each player.
const LEFT_SLOTS = 9
::L4D2VR_Items <- [
	null, null, null, // the laser sight machine, centered on these three slots
	"weapon_pain_pills_spawn", "weapon_adrenaline_spawn",
	"weapon_first_aid_kit_spawn", "weapon_defibrillator_spawn",
	"weapon_upgradepack_explosive_spawn", "weapon_upgradepack_incendiary_spawn"
]

// Right side: two tables with every melee weapon (the mission file l4d2vr_testroom.txt loads all of them), each lying
// across the table so they don't cover each other
::L4D2VR_Melee <- [
	"fireaxe", "katana", "machete", "baseball_bat", "cricket_bat", "crowbar", "electric_guitar",
	"frying_pan", "tonfa", "golfclub", "knife", "pitchfork", "shovel"
]

// Behind: a table with several of each grenade
const GRENADE_COPIES = 3
::L4D2VR_Grenades <- [ "weapon_molotov_spawn", "weapon_pipe_bomb_spawn", "weapon_vomitjar_spawn" ]

// Further behind: a row of doors of different kinds, to open and close with the hand
::L4D2VR_Doors <- [
	"models/props_doors/doormain01.mdl", "models/props_doors/doormainmetal01.mdl", "models/props_doors/doorglassmain01.mdl",
	"models/props_doors/doormain_rural01.mdl", "models/props_doors/doorfreezer01.mdl", "models/props_doors/checkpoint_door_02.mdl",
	"models/props_doors/doormainmetalwindow01.mdl"
]
const DOOR_HALF_HEIGHT = 53.0 // these models have their origin at the hinge, half way up
const DOOR_SPACING = 80.0

::L4D2VR_Spawned <- 0
::L4D2VR_Placed <- []

// The weapons and items on the tables, put back when taken: the count and "infinite" flag of the weapon_*_spawn
// entities aren't enough (grenades, pills and adrenaline disappeared once taken). { classname, keyvalues, entity }
::L4D2VR_Refills <- []

::L4D2VR_Precache <- function()
{
	PrecacheModel(TABLE_MODEL)
	PrecacheModel(AMMO_MODEL)
	foreach (name in ::L4D2VR_Guns)
		PrecacheEntityFromTable({ classname = name })
	foreach (name in ::L4D2VR_Items)
	{
		if (name)
			PrecacheEntityFromTable({ classname = name })
	}
	foreach (name in ::L4D2VR_Grenades)
		PrecacheEntityFromTable({ classname = name })
	PrecacheEntityFromTable({ classname = "upgrade_laser_sight" })
	foreach (model in ::L4D2VR_Doors)
		PrecacheModel(model)
}

::L4D2VR_Spawn <- function(classname, keyvalues)
{
	local entity = SpawnEntityFromTable(classname, keyvalues)
	if (entity)
	{
		::L4D2VR_Spawned++
		::L4D2VR_Placed.append(entity)
		if (classname.find("weapon_") == 0 && classname != "weapon_ammo_spawn")
			::L4D2VR_Refills.append({ classname = classname, keyvalues = keyvalues, entity = entity, home = entity.GetOrigin() })
	}
	else
		Msg("L4D2VR test room: couldn't spawn " + classname + "\n")
	return entity
}

// Called every half second by a logic_timer in the map: spawns again what was taken from the tables. A spawn counts
// as taken when it's gone or when it left its place (in case the spawn itself becomes the weapon in the hand).
::L4D2VR_Refill <- function()
{
	foreach (item in ::L4D2VR_Refills)
	{
		local entity = item.entity
		if (entity && entity.IsValid() && (entity.GetOrigin() - item.home).Length() < 16.0)
			continue
		item.entity = SpawnEntityFromTable(item.classname, item.keyvalues)
		if (item.entity)
		{
			item.home = item.entity.GetOrigin()
			::L4D2VR_Placed.append(item.entity)
		}
	}
}

// The floor under `point` (the map isn't flat everywhere), or `point` itself when nothing is found
::L4D2VR_Floor <- function(point)
{
	if (!("TraceLine" in getroottable()))
		return point
	local trace = { start = point + Vector(0, 0, 48), end = point - Vector(0, 0, 96), mask = 0x400B } // MASK_SOLID_BRUSHONLY: world and static props, not players
	TraceLine(trace)
	if (("hit" in trace) && trace.hit && !(("startsolid" in trace) && trace.startsolid) && ("pos" in trace))
		return trace.pos
	return point
}

// A table centered at `center` (snapped to the floor), its long side along the direction `yaw` + 90 degrees.
// Returns where the things on it go: the middle of its top.
::L4D2VR_Table <- function(center, yaw)
{
	local floor = ::L4D2VR_Floor(center)
	::L4D2VR_Spawn("prop_dynamic", { model = TABLE_MODEL, origin = floor, angles = Vector(0, yaw, 0), solid = 6 })
	return floor + Vector(0, 0, TABLE_TOP + 1.0)
}

::L4D2VR_Survivors <- function()
{
	local survivors = []
	local player = null
	while (player = Entities.FindByClassname(player, "player"))
	{
		if (player.IsSurvivor())
			survivors.append(player)
	}
	return survivors
}

// Puts everything around the survivors. here: true to use only the player that typed it (from the console) instead of
// the center of all survivors.
::L4D2VR_TestRoom <- function(here = false)
{
	local survivors = ::L4D2VR_Survivors()
	if (survivors.len() == 0)
	{
		Msg("L4D2VR test room: no survivor yet\n")
		return false
	}

	// Remove what was placed before
	foreach (entity in ::L4D2VR_Placed)
	{
		if (entity && entity.IsValid())
			entity.Kill()
	}
	::L4D2VR_Placed = []
	::L4D2VR_Refills = []
	::L4D2VR_Spawned = 0
	::L4D2VR_ApplyDirectorOptions()

	// Center: the first human survivor (or the one at the console), or the middle of all survivors
	local first = survivors[0]
	foreach (survivor in survivors)
	{
		if (!IsPlayerABot(survivor))
		{
			first = survivor
			break
		}
	}
	if (here && ("GetListenServerHost" in getroottable()) && GetListenServerHost())
		first = GetListenServerHost()
	local origin = first.GetOrigin()
	if (!here && survivors.len() > 1)
	{
		origin = Vector(0, 0, 0)
		foreach (survivor in survivors)
			origin = origin + survivor.GetOrigin()
		origin = origin * (1.0 / survivors.len())
	}

	local yaw = first.EyeAngles().y
	local radians = yaw * PI / 180.0
	local forward = Vector(cos(radians), sin(radians), 0)
	local left = Vector(-sin(radians), cos(radians), 0)

	// Front: three tables side by side, guns pointing away
	local count = ::L4D2VR_Guns.len()
	local front = origin + forward * FRONT_DISTANCE
	local tops = []
	for (local i = -1; i <= 1; i++)
		tops.append(::L4D2VR_Table(front + left * (i * TABLE_LENGTH), yaw))
	local spacing = (TABLE_LENGTH * 3 - 20.0) / (count - 1)
	for (local i = 0; i < count; i++)
	{
		local along = -(TABLE_LENGTH * 1.5 - 10.0) + spacing * i
		local top = tops[(along < -TABLE_LENGTH * 0.5) ? 0 : (along > TABLE_LENGTH * 0.5) ? 2 : 1]
		local at = Vector(front.x, front.y, top.z) + left * along
		::L4D2VR_Spawn(::L4D2VR_Guns[i], { origin = at, angles = Vector(0, yaw, 90), count = 99, spawnflags = ITEM_FLAGS })
	}

	// Left: a table along the way the player faces, with the laser sight machine, pills, adrenaline, kits and an ammo pile
	local side = origin + left * SIDE_DISTANCE
	local top = ::L4D2VR_Table(side, yaw - 90)
	spacing = (TABLE_LENGTH - 16.0) / LEFT_SLOTS
	local first_slot = top - forward * (TABLE_LENGTH * 0.5 - 8.0)
	::L4D2VR_Spawn("upgrade_laser_sight", { origin = first_slot + forward * spacing, angles = Vector(0, yaw + 90, 0) })
	for (local i = 0; i < LEFT_SLOTS; i++)
	{
		if (::L4D2VR_Items[i])
			::L4D2VR_Spawn(::L4D2VR_Items[i], { origin = first_slot + forward * (spacing * i), angles = Vector(0, yaw - 90, 0), count = 99, spawnflags = ITEM_FLAGS })
	}
	::L4D2VR_Spawn("weapon_ammo_spawn", { model = AMMO_MODEL, origin = top + forward * (TABLE_LENGTH * 0.5 - 6.0) + left * 22.0, angles = Vector(0, yaw, 0), count = 99, spawnflags = ITEM_FLAGS })

	// Right: two tables in a row with the melee weapons, each turned 90 degrees counterclockwise from how they were
	// (lying across the table instead of along it) so they don't cover each other
	local other = origin - left * SIDE_DISTANCE
	local back_top = ::L4D2VR_Table(other - forward * (TABLE_LENGTH * 0.5), yaw - 90)
	local front_top = ::L4D2VR_Table(other + forward * (TABLE_LENGTH * 0.5), yaw - 90)
	count = ::L4D2VR_Melee.len()
	spacing = (TABLE_LENGTH * 2 - 16.0) / (count - 1)
	for (local i = 0; i < count; i++)
	{
		local along = -(TABLE_LENGTH - 8.0) + spacing * i
		local z = (along < 0 ? back_top : front_top).z
		local at = Vector(other.x, other.y, z) + forward * along
		::L4D2VR_Spawn("weapon_melee_spawn", { origin = at, angles = Vector(0, yaw, 90), melee_weapon = ::L4D2VR_Melee[i], count = 99, spawnflags = ITEM_FLAGS })
	}

	// Behind: a table with several of each grenade (where the laser sights used to be)
	local back = origin - forward * BACK_DISTANCE
	top = ::L4D2VR_Table(back, yaw)
	count = ::L4D2VR_Grenades.len() * GRENADE_COPIES
	spacing = (TABLE_LENGTH - 12.0) / (count - 1)
	for (local i = 0; i < count; i++)
	{
		local at = top + left * (-(TABLE_LENGTH * 0.5 - 6.0) + spacing * i)
		::L4D2VR_Spawn(::L4D2VR_Grenades[i / GRENADE_COPIES], { origin = at, angles = Vector(0, yaw + 180, 0), count = 99, spawnflags = ITEM_FLAGS })
	}

	// Further behind: a row of doors facing the player, each turning on its hinge, opened from either side
	local doors = origin - forward * DOORS_DISTANCE
	count = ::L4D2VR_Doors.len()
	for (local i = 0; i < count; i++)
	{
		local hinge = ::L4D2VR_Floor(doors + left * (-(DOOR_SPACING * (count - 1)) * 0.5 + DOOR_SPACING * i - 28.0)) + Vector(0, 0, DOOR_HALF_HEIGHT)
		::L4D2VR_Spawn("prop_door_rotating", { model = ::L4D2VR_Doors[i], origin = hinge, angles = Vector(0, yaw, 0),
			spawnflags = 8192, distance = 90, speed = 200, hardware = 1, opendir = 0, returndelay = -1, spawnpos = 0 })
	}

	Msg("L4D2VR test room: " + ::L4D2VR_Spawned + " things placed around " + survivors.len() + " survivor(s)\n")
	return true
}

// Runs when the map's logic_script spawns
Msg("L4D2VR test room: script loaded\n")
::L4D2VR_ApplyDirectorOptions()
::L4D2VR_Precache()
