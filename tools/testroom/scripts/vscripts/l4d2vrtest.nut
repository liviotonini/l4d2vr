// L4D2VR test room: a game mode that puts every weapon and item on tables around the player, with no infected and no
// bots, to test the VR mod: guns in front, items on the left, melee weapons on the right, laser sights behind and doors
// further behind. Load with:  map c2m1_highway l4d2vrtest
// Type it in the console in game: when launching, the addon is mounted after the map, so +map ignores the mode.
// To put the tables in front of you again, wherever you are:  sv_cheats 1  then  script L4D2VR_TestRoom()

MutationOptions <-
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

const TABLE_MODEL = "models/props_interiors/table_folding.mdl"
const AMMO_MODEL = "models/props/terror/ammo_stack.mdl"
const TABLE_LENGTH = 91.0 // along the model's y axis
const TABLE_TOP = 32.0

// Front tables, from the player's right to the left
::L4D2VR_Guns <- [
	"weapon_pistol_spawn", "weapon_pistol_magnum_spawn",
	"weapon_smg_spawn", "weapon_smg_silenced_spawn", "weapon_smg_mp5_spawn",
	"weapon_pumpshotgun_spawn", "weapon_shotgun_chrome_spawn", "weapon_autoshotgun_spawn", "weapon_shotgun_spas_spawn",
	"weapon_rifle_spawn", "weapon_rifle_ak47_spawn", "weapon_rifle_desert_spawn", "weapon_rifle_sg552_spawn",
	"weapon_hunting_rifle_spawn", "weapon_sniper_military_spawn", "weapon_sniper_awp_spawn", "weapon_sniper_scout_spawn",
	"weapon_grenade_launcher_spawn", "weapon_rifle_m60_spawn", "weapon_chainsaw_spawn"
]

// Left table
::L4D2VR_Items <- [
	"weapon_molotov_spawn", "weapon_pipe_bomb_spawn", "weapon_vomitjar_spawn",
	"weapon_pain_pills_spawn", "weapon_adrenaline_spawn",
	"weapon_first_aid_kit_spawn", "weapon_defibrillator_spawn",
	"weapon_upgradepack_explosive_spawn", "weapon_upgradepack_incendiary_spawn"
]

// Right table: the melee weapons of this campaign (Dark Carnival doesn't have the machete, cricket bat, frying pan,
// tonfa, golf club, pitchfork or shovel)
::L4D2VR_Melee <- [ "fireaxe", "katana", "baseball_bat", "crowbar", "electric_guitar", "knife" ]

// Behind: a table with laser sights (each box gives the laser once to each player, so there are several) and a row of
// doors of different kinds, to open and close with the hand
const LASER_COUNT = 8
::L4D2VR_Doors <- [
	"models/props_doors/doormain01.mdl", "models/props_doors/doormainmetal01.mdl", "models/props_doors/doorglassmain01.mdl",
	"models/props_doors/doormain_rural01.mdl", "models/props_doors/doorfreezer01.mdl", "models/props_doors/checkpoint_door_02.mdl",
	"models/props_doors/doormainmetalwindow01.mdl"
]
const DOOR_HALF_HEIGHT = 53.0 // these models have their origin at the hinge, half way up
const DOOR_SPACING = 80.0

Msg("L4D2VR test room: script loaded\n")

PrecacheModel(TABLE_MODEL)
PrecacheModel(AMMO_MODEL)
foreach (name in ::L4D2VR_Guns)
	PrecacheEntityFromTable({ classname = name })
foreach (name in ::L4D2VR_Items)
	PrecacheEntityFromTable({ classname = name })
PrecacheEntityFromTable({ classname = "upgrade_laser_sight" })
foreach (model in ::L4D2VR_Doors)
	PrecacheModel(model)

::L4D2VR_Spawned <- 0
::L4D2VR_Placed <- false

::L4D2VR_Spawn <- function(classname, keyvalues)
{
	local entity = SpawnEntityFromTable(classname, keyvalues)
	if (entity)
		::L4D2VR_Spawned++
	else
		Msg("L4D2VR test room: couldn't spawn " + classname + "\n")
	return entity
}

// A table centered at `center`, its long side along the direction `yaw` + 90 degrees
::L4D2VR_Table <- function(center, yaw)
{
	::L4D2VR_Spawn("prop_dynamic", { model = TABLE_MODEL, origin = center, angles = Vector(0, yaw, 0), solid = 6 })
}

// Puts everything around the first survivor: guns in front, items on the left, melee weapons on the right
::L4D2VR_TestRoom <- function(quiet = false)
{
	local player = null
	while (player = Entities.FindByClassname(player, "player"))
	{
		if (player.IsSurvivor())
			break
	}
	if (!player)
	{
		if (!quiet)
			Msg("L4D2VR test room: no survivor yet\n")
		return false
	}

	local origin = player.GetOrigin()
	local yaw = player.EyeAngles().y
	local radians = yaw * PI / 180.0
	local forward = Vector(cos(radians), sin(radians), 0)
	local left = Vector(-sin(radians), cos(radians), 0)
	local top = Vector(0, 0, TABLE_TOP + 1.0)
	::L4D2VR_Spawned = 0

	// Front: three tables side by side, 55 units ahead, guns pointing away
	local front = origin + forward * 55.0
	for (local i = -1; i <= 1; i++)
		::L4D2VR_Table(front + left * (i * TABLE_LENGTH), yaw)
	local count = ::L4D2VR_Guns.len()
	local spacing = (TABLE_LENGTH * 3 - 20.0) / (count - 1)
	for (local i = 0; i < count; i++)
	{
		local at = front + top + left * (-(TABLE_LENGTH * 1.5 - 10.0) + spacing * i)
		::L4D2VR_Spawn(::L4D2VR_Guns[i], { origin = at, angles = Vector(0, yaw, 90), count = 99, spawnflags = 2 })
	}

	// Left: a table along the way the player faces, with the grenades, pills, adrenaline, kits and an ammo pile
	local side = origin + left * 175.0
	::L4D2VR_Table(side, yaw - 90)
	count = ::L4D2VR_Items.len()
	spacing = (TABLE_LENGTH - 16.0) / count
	for (local i = 0; i < count; i++)
	{
		local at = side + top + forward * (-(TABLE_LENGTH * 0.5 - 8.0) + spacing * i)
		::L4D2VR_Spawn(::L4D2VR_Items[i], { origin = at, angles = Vector(0, yaw - 90, 0), count = 99, spawnflags = 2 })
	}
	::L4D2VR_Spawn("weapon_ammo_spawn", { model = AMMO_MODEL, origin = side + top + forward * (TABLE_LENGTH * 0.5 - 6.0) + left * 22.0, angles = Vector(0, yaw, 0), count = 99, spawnflags = 2 })

	// Right: a table with the melee weapons
	local other = origin - left * 175.0
	::L4D2VR_Table(other, yaw - 90)
	count = ::L4D2VR_Melee.len()
	spacing = (TABLE_LENGTH - 16.0) / (count - 1)
	for (local i = 0; i < count; i++)
	{
		local at = other + top + forward * (-(TABLE_LENGTH * 0.5 - 8.0) + spacing * i)
		::L4D2VR_Spawn("weapon_melee_spawn", { origin = at, angles = Vector(0, yaw - 90, 90), melee_weapon = ::L4D2VR_Melee[i], count = 99, spawnflags = 2 })
	}

	// Behind: a table with laser sights
	local back = origin - forward * 70.0
	::L4D2VR_Table(back, yaw)
	spacing = (TABLE_LENGTH - 16.0) / (LASER_COUNT - 1)
	for (local i = 0; i < LASER_COUNT; i++)
	{
		local at = back + top + left * (-(TABLE_LENGTH * 0.5 - 8.0) + spacing * i)
		::L4D2VR_Spawn("upgrade_laser_sight", { origin = at, angles = Vector(0, yaw + 180, 0) })
	}

	// Further behind: a row of doors facing the player, each turning on its hinge, opened from either side
	local doors = origin - forward * 170.0
	count = ::L4D2VR_Doors.len()
	for (local i = 0; i < count; i++)
	{
		local hinge = doors + left * (-(DOOR_SPACING * (count - 1)) * 0.5 + DOOR_SPACING * i - 28.0) + Vector(0, 0, DOOR_HALF_HEIGHT)
		::L4D2VR_Spawn("prop_door_rotating", { model = ::L4D2VR_Doors[i], origin = hinge, angles = Vector(0, yaw, 0),
			spawnflags = 8192, distance = 90, speed = 200, hardware = 1, opendir = 0, returndelay = -1, spawnpos = 0 })
	}

	Msg("L4D2VR test room: " + ::L4D2VR_Spawned + " things placed\n")
	return true
}

function OnGameplayStart()
{
	Msg("L4D2VR test room: gameplay start\n")
	if (!::L4D2VR_Placed)
		::L4D2VR_Placed = ::L4D2VR_TestRoom()
}

// In case the game starts without OnGameplayStart: once a survivor is there
function Update()
{
	if (!::L4D2VR_Placed)
		::L4D2VR_Placed = ::L4D2VR_TestRoom(true)
}
