// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder
// Fake Player System — populates the world with autonomous mob-based player-lookalikes

#include "fakeplayer.hpp"

#include <cstring>
#include <vector>
#include <algorithm>

#include <common/cbasetypes.hpp>
#include <common/mmo.hpp>
#include <common/nullpo.hpp>
#include <common/random.hpp>
#include <common/showmsg.hpp>
#include <common/timer.hpp>

#include "battle.hpp"
#include "clif.hpp"
#include "map.hpp"
#include "mob.hpp"
#include "status.hpp"
#include "unit.hpp"

// Clone range — mirrors definitions in mob.cpp
#define MIN_MOB_DB 1000
#define MAX_MOB_DB 3999
#define MIN_MOB_DB2 20020
#define MOB_CLONE_START MAX_MOB_DB
#define MOB_CLONE_END MIN_MOB_DB2

// ============================================================
// Fantasy name pool (~200 names)
// ============================================================
static const char* fakeplayer_names[] = {
	"Aldric", "Myrna", "Fenwick", "Seraphina", "Tormund", "Isolde",
	"Ragnar", "Fendrel", "Kalista", "Orin", "Ysolde", "Theron",
	"Brynhild", "Cedric", "Elara", "Garrick", "Hiraeth", "Ingrid",
	"Jareth", "Kaelith", "Lysander", "Morwen", "Nerissa", "Oberon",
	"Phaedra", "Quillon", "Rosmund", "Sylvain", "Tatiana", "Ulric",
	"Vaelith", "Wynter", "Xander", "Yvaine", "Zephyr", "Arwen",
	"Baldur", "Celeste", "Darius", "Elowen", "Faelan", "Gwendolyn",
	"Hawke", "Idris", "Jorvyn", "Kiera", "Leander", "Miriel",
	"Nyx", "Orion", "Petra", "Quillan", "Rhiannon", "Soren",
	"Thalassa", "Uriel", "Vesper", "Wren", "Ximena", "Yorick",
	"Zarek", "Astrid", "Bjorn", "Cressida", "Dorian", "Evander",
	"Freya", "Galahad", "Helena", "Ivar", "Junia", "Kael",
	"Lirien", "Magnus", "Niamh", "Osric", "Persephone", "Quintus",
	"Rowan", "Siegfried", "Tristan", "Undine", "Valor", "Willow",
	"Xerxes", "Yara", "Zara", "Aelric", "Branwen", "Corwin",
	"Dahlia", "Emeric", "Fiora", "Grimald", "Hestia", "Isen",
	"Juliana", "Kendrick", "Lirael", "Mordecai", "Nessa", "Othello",
	"Primrose", "Raguel", "Sabine", "Tobias", "Una", "Viktor",
	"Wulfric", "Xiara", "Yoren", "Zelda", "Aeron", "Brigitte",
	"Callum", "Delphine", "Erwin", "Fiona", "Gideon", "Helga",
	"Ignatius", "Jasira", "Kellan", "Lorelei", "Marik", "Nadira",
	"Oswald", "Pandora", "Roland", "Selene", "Thane", "Ursula",
	"Varian", "Winona", "Xena", "Ygritte", "Zane", "Ansel",
	"Beatrix", "Corvus", "Daphne", "Eldric", "Felicity", "Galen",
	"Hazel", "Inara", "Jasper", "Katarina", "Lucian", "Maren",
	"Nikolai", "Ophelia", "Percival", "Quinn", "Raine", "Sterling",
	"Tamsin", "Uther", "Vivienne", "Wolfgang", "Yuki", "Zenith",
	"Alaric", "Briar", "Cassius", "Eloise", "Fenris", "Greta",
	"Hadrian", "Ivy", "Kieran", "Lennox", "Maia", "Nolan",
	"Odessa", "Phoenix", "Remy", "Sage", "Thalia", "Voss",
	"Wynn", "Xyla", "Yael", "Zephyra", "Alistair", "Brynn",
	"Cyrus", "Ember", "Flynn", "Greer", "Hollis", "Idara",
	"Jorah", "Knox", "Lyanna", "Merrick", "Neria", "Orla",
};
static const int32 fakeplayer_name_count = sizeof(fakeplayer_names) / sizeof(fakeplayer_names[0]);

// ============================================================
// Job class pool
// ============================================================
static const int32 fakeplayer_jobs[] = {
	0,    // Novice
	1,    // Swordsman
	2,    // Mage
	3,    // Archer
	4,    // Acolyte
	5,    // Merchant
	6,    // Thief
	7,    // Knight
	8,    // Priest
	9,    // Wizard
	10,   // Blacksmith
	11,   // Hunter
	12,   // Assassin
	14,   // Crusader
	15,   // Monk
	16,   // Sage
	17,   // Rogue
	18,   // Alchemist
	19,   // Bard
	20,   // Dancer
	4008, // Lord Knight
	4009, // High Priest
	4010, // High Wizard
	4011, // Whitesmith
	4012, // Sniper
	4013, // Assassin Cross
	4015, // Paladin
	4016, // Champion
	4017, // Professor
	4018, // Stalker
	4019, // Creator
	4020, // Clown
	4021, // Gypsy
};
static const int32 fakeplayer_job_count = sizeof(fakeplayer_jobs) / sizeof(fakeplayer_jobs[0]);

// ============================================================
// Weapon sprite pools per job archetype
// ============================================================
enum e_weapon_class {
	WCLASS_SWORD,
	WCLASS_STAFF,
	WCLASS_BOW,
	WCLASS_MACE,
	WCLASS_AXE,
	WCLASS_DAGGER,
	WCLASS_KATAR,
	WCLASS_KNUCKLE,
	WCLASS_MUSICAL,
	WCLASS_WHIP,
	WCLASS_BOOK,
	WCLASS_SPEAR,
	WCLASS_NONE,
};

// Weapon pools use actual item nameids — the client resolves weapon sprites
// from the item ID (or view_id), NOT from weapon type constants.
// Most weapons have view_id=0 in item_db, so the nameid IS the LOOK_WEAPON value.
struct s_weapon_pool {
	int32 sprites[10];
	int32 count;
};

static const s_weapon_pool weapon_pools[] = {
	// WCLASS_SWORD  — Swordsman/Knight/Crusader/LK/Paladin (1hSwords + 2hSwords)
	{ { 1101, 1104, 1107, 1108, 1116, 1117, 1119, 0, 0, 0 }, 7 },
	// WCLASS_STAFF  — Mage/Wizard/Sage/HW/Professor
	{ { 1601, 1602, 1604, 1605, 1606, 1607, 0, 0, 0, 0 }, 6 },
	// WCLASS_BOW    — Archer/Hunter/Sniper
	{ { 1701, 1702, 1703, 1704, 1705, 1706, 0, 0, 0, 0 }, 6 },
	// WCLASS_MACE   — Acolyte/Priest/Monk/HP/Champion
	{ { 1501, 1502, 1503, 1504, 1505, 1506, 0, 0, 0, 0 }, 6 },
	// WCLASS_AXE    — Merchant/Blacksmith/Alchemist/WS/Creator
	{ { 1301, 1302, 1303, 1304, 1305, 1306, 0, 0, 0, 0 }, 6 },
	// WCLASS_DAGGER — Thief/Rogue/Stalker
	{ { 1201, 1204, 1207, 1210, 1213, 1222, 0, 0, 0, 0 }, 6 },
	// WCLASS_KATAR  — Assassin/Assassin Cross
	{ { 1250, 1251, 1252, 1253, 1254, 1255, 0, 0, 0, 0 }, 6 },
	// WCLASS_KNUCKLE — Monk/Champion
	{ { 1801, 1802, 1803, 1804, 1805, 0, 0, 0, 0, 0 }, 5 },
	// WCLASS_MUSICAL — Bard/Clown
	{ { 1901, 1902, 1903, 1904, 1905, 0, 0, 0, 0, 0 }, 5 },
	// WCLASS_WHIP    — Dancer/Gypsy
	{ { 1950, 1951, 1952, 1953, 1954, 0, 0, 0, 0, 0 }, 5 },
	// WCLASS_BOOK   — Sage/Professor
	{ { 1550, 1551, 1552, 1553, 1554, 0, 0, 0, 0, 0 }, 5 },
	// WCLASS_SPEAR  — Crusader/Paladin/Knight/LK (1hSpear)
	{ { 1401, 1402, 1403, 1404, 1405, 0, 0, 0, 0, 0 }, 5 },
	// WCLASS_NONE   — Novice (fists)
	{ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, 1 },
};

static e_weapon_class fakeplayer_get_weapon_class(int32 job) {
	switch (job) {
		case 1: case 7: case 14: case 4008: case 4015:  // Swordsman/Knight/Crusader/LK/Paladin
			return WCLASS_SWORD;
		case 2: case 9: case 4010:                       // Mage/Wizard/HW
			return WCLASS_STAFF;
		case 16: case 4017:                              // Sage/Professor
			return WCLASS_BOOK;
		case 3: case 11: case 4012:                      // Archer/Hunter/Sniper
			return WCLASS_BOW;
		case 4: case 8: case 4009:                       // Acolyte/Priest/HP
			return WCLASS_MACE;
		case 15: case 4016:                              // Monk/Champion
			return WCLASS_KNUCKLE;
		case 5: case 10: case 18: case 4011: case 4019:  // Merchant/BS/Alch/WS/Creator
			return WCLASS_AXE;
		case 6: case 17: case 4018:                      // Thief/Rogue/Stalker
			return WCLASS_DAGGER;
		case 12: case 4013:                              // Assassin/SinX
			return WCLASS_KATAR;
		case 19: case 4020:                              // Bard/Clown
			return WCLASS_MUSICAL;
		case 20: case 4021:                              // Dancer/Gypsy
			return WCLASS_WHIP;
		default:
			return WCLASS_NONE;
	}
}

// ============================================================
// Headgear sprite pools
// ============================================================
static const int32 head_top_sprites[] = {
	7, 11, 14, 16, 18, 19, 20, 29, 33, 40,
	42, 44, 45, 51, 57, 69, 70, 72, 80, 86,
	93, 101, 107, 110, 120, 131, 137
};
static const int32 head_top_count = sizeof(head_top_sprites) / sizeof(head_top_sprites[0]);

static const int32 head_mid_sprites[] = {
	3, 10, 12, 26, 27
};
static const int32 head_mid_count  = sizeof(head_mid_sprites) / sizeof(head_mid_sprites[0]);

static const int32 head_bottom_sprites[] = {
	8, 21, 22, 23, 24
};
static const int32 head_bottom_count = sizeof(head_bottom_sprites) / sizeof(head_bottom_sprites[0]);

// ============================================================
// Town map list for weighted distribution (3x weight)
// ============================================================
static const char* town_maps[] = {
	"prontera", "geffen", "payon", "morocc", "alberta", "aldebaran",
	"izlude", "yuno", "lighthalzen", "rachel", "hugel", "einbroch",
	"amatsu", "gonryun", "louyang", "niflheim", "comodo", "umbala",
	"brasilis", "dewata", "malangdo", "malaya", "eclage",
};
static const int32 town_map_count = sizeof(town_maps) / sizeof(town_maps[0]);

// ============================================================
// Internal tracking
// ============================================================
static std::vector<int32> fakeplayer_ids;       // GIDs of spawned fake player mobs
static int32 fakeplayer_respawn_timer_id = INVALID_TIMER;

// ============================================================
// Map eligibility check
// ============================================================
static bool fakeplayer_map_eligible(int16 m) {
	struct map_data *mapdata = map_getmapdata(m);
	if (mapdata == nullptr)
		return false;

	// Exclude instance maps
	if (mapdata->instance_id > 0)
		return false;

	// Exclude PvP, GvG, Battleground maps
	if (mapdata->getMapFlag(MF_PVP) || mapdata->getMapFlag(MF_GVG) ||
	    mapdata->getMapFlag(MF_GVG_CASTLE) || mapdata->getMapFlag(MF_GVG_DUNGEON) ||
	    mapdata->getMapFlag(MF_GVG_TE) || mapdata->getMapFlag(MF_GVG_TE_CASTLE) ||
	    mapdata->getMapFlag(MF_BATTLEGROUND))
		return false;

	// Exclude jail maps by name
	if (strstr(mapdata->name, "sec_pri") != nullptr)
		return false;

	// Exclude maps with no skill flag (often special maps)
	// Don't exclude nomobloot - that's about drop restrictions not mob presence

	return true;
}

// ============================================================
// Weighted map selection
// ============================================================
static int16 fakeplayer_select_map(void) {
	// Build weighted list of eligible maps
	struct s_weighted_map {
		int16 m;
		int32 weight;
	};

	static std::vector<s_weighted_map> weighted_maps;
	static bool initialized = false;

	if (!initialized) {
		for (int16 m = 0; m < map_num; m++) {
			if (!fakeplayer_map_eligible(m))
				continue;

			struct map_data *mapdata = map_getmapdata(m);
			int32 weight = 1; // default

			// Check if it's a town map (3x weight)
			bool is_town = false;
			for (int32 t = 0; t < town_map_count; t++) {
				if (strncmp(mapdata->name, town_maps[t], strlen(town_maps[t])) == 0) {
					is_town = true;
					break;
				}
			}
			if (is_town || mapdata->getMapFlag(MF_TOWN)) {
				weight = 3;
			}
			// Field maps (2x weight)
			else if (strstr(mapdata->name, "_fild") != nullptr || strstr(mapdata->name, "fild") != nullptr) {
				weight = 2;
			}
			// Indoor maps (reduced weight)
			else if (strstr(mapdata->name, "_in") != nullptr) {
				weight = 1; // keep at 1 instead of 0.5
			}
			// Dungeon maps stay at 1x

			weighted_maps.push_back({ m, weight });
		}
		initialized = true;
	}

	if (weighted_maps.empty())
		return -1;

	// Calculate total weight
	int32 total_weight = 0;
	for (const auto &wm : weighted_maps)
		total_weight += wm.weight;

	// Pick random weighted entry
	int32 roll = rnd() % total_weight;
	for (const auto &wm : weighted_maps) {
		roll -= wm.weight;
		if (roll < 0)
			return wm.m;
	}

	return weighted_maps[0].m;
}

// ============================================================
// Generate random name
// ============================================================
static void fakeplayer_generate_name(char *out, int32 max_len) {
	const char *base = fakeplayer_names[rnd() % fakeplayer_name_count];
	int32 suffix = rnd() % 100;
	snprintf(out, max_len, "%s%d", base, suffix);
}

// ============================================================
// Generate random view_data (appearance)
// ============================================================
static void fakeplayer_generate_viewdata(struct view_data *vd, int32 job) {
	memset(vd, 0, sizeof(struct view_data));

	vd->look[LOOK_BASE] = job;

	// Random gender
	vd->sex = (rnd() % 2) ? SEX_MALE : SEX_FEMALE;

	// Hair style (1-27)
	vd->look[LOOK_HAIR] = 1 + (rnd() % 27);

	// Hair color (0-3)
	vd->look[LOOK_HAIR_COLOR] = rnd() % 4;

	// Clothes color (0-3)
	vd->look[LOOK_CLOTHES_COLOR] = rnd() % 4;

	// Weapon sprite based on job class
	e_weapon_class wclass = fakeplayer_get_weapon_class(job);
	const s_weapon_pool &pool = weapon_pools[wclass];
	vd->look[LOOK_WEAPON] = pool.sprites[rnd() % pool.count];

	// Shield for melee 1H weapon jobs (~40% chance)
	if (wclass == WCLASS_SWORD || wclass == WCLASS_MACE || wclass == WCLASS_AXE ||
	    wclass == WCLASS_SPEAR || wclass == WCLASS_DAGGER) {
		if (rnd() % 100 < 40)
			vd->look[LOOK_SHIELD] = 1 + (rnd() % 4);
	}

	// Head top (~50% chance)
	if (rnd() % 100 < 50)
		vd->look[LOOK_HEAD_TOP] = head_top_sprites[rnd() % head_top_count];

	// Head mid (~30% chance)
	if (rnd() % 100 < 30)
		vd->look[LOOK_HEAD_MID] = head_mid_sprites[rnd() % head_mid_count];

	// Head bottom (~20% chance)
	if (rnd() % 100 < 20)
		vd->look[LOOK_HEAD_BOTTOM] = head_bottom_sprites[rnd() % head_bottom_count];
}

// ============================================================
// Spawn a single fake player on specified map
// ============================================================
static int32 fakeplayer_spawn_one(int16 m) {
	int32 mob_id;

	// Find free clone slot
	ARR_FIND(MOB_CLONE_START, MOB_CLONE_END, mob_id, !mob_db.exists(mob_id));
	if (mob_id >= MOB_CLONE_END) {
		ShowWarning("fakeplayer_spawn_one: No free clone slots available.\n");
		return 0;
	}

	// Create mob_db entry
	std::shared_ptr<s_mob_db> db = std::make_shared<s_mob_db>();
	mob_db.put(mob_id, db);

	// Generate name
	char name[NAME_LENGTH];
	fakeplayer_generate_name(name, NAME_LENGTH);
	db->sprite = name;
	db->name = name;
	db->jname = name;

	// Random level (weighted toward mid-levels: 10-99)
	int32 level = 10 + (rnd() % 45) + (rnd() % 45); // peaks around 55
	db->lv = level;

	// Pick job class early so we can set range and stats accordingly
	int32 job_class = fakeplayer_jobs[rnd() % fakeplayer_job_count];
	e_weapon_class wclass = fakeplayer_get_weapon_class(job_class);

	// Stats — scaled to be combat-capable
	struct status_data *status = &db->status;
	memset(status, 0, sizeof(struct status_data));

	status->max_hp = 1000 + level * 80;
	status->hp = status->max_hp;
	status->max_sp = 100 + level * 10;
	status->sp = status->max_sp;

	// ATK based on level (base values, status_calc_misc will refine)
	status->rhw.atk = level * 2;
	status->rhw.atk2 = level * 4;

	// Attack range based on weapon class
	switch (wclass) {
		case WCLASS_BOW:
			status->rhw.range = 9; // Bow range
			break;
		case WCLASS_STAFF:
			status->rhw.range = 1; // Melee
			break;
		default:
			status->rhw.range = 1; // Melee
			break;
	}

	// DEF based on level
	status->def = level / 2;
	status->mdef = level / 3;

	// Base stats — stronger so they can actually fight
	status->str = 20 + level;
	status->agi = 20 + level * 2 / 3;
	status->vit = 15 + level / 2;
	status->int_ = 15 + level / 2;
	status->dex = 20 + level;
	status->luk = 10 + level / 3;

	// Movement & size
	status->speed = DEFAULT_WALK_SPEED;
	status->aspd_rate = 1000;
	status->amotion = 1872; // default animation motion
	status->dmotion = 576;  // damage motion
	status->size = SZ_SMALL; // SZ_SMALL(0) = normal size; SZ_MEDIUM(1) = small; SZ_BIG(2) = large
	status->race = RC_DEMIHUMAN;
	status->ele_lv = 1;
	status->def_ele = ELE_NEUTRAL;

	// Randomized behavior:
	// 40% Hunters (seek and fight mobs)
	// 30% Walkers (wander around, no combat)
	// 20% Idlers  (stand in place)
	// 10% Sitters (sit down)
	int32 behavior_roll = rnd() % 100;
	int32 behavior_type; // 0=hunter, 1=walker, 2=idler, 3=sitter
	if (behavior_roll < 40) {
		status->mode = static_cast<enum e_mode>(MD_CANMOVE | MD_AGGRESSIVE | MD_CANATTACK);
		behavior_type = 0;
	} else if (behavior_roll < 70) {
		status->mode = static_cast<enum e_mode>(MD_CANMOVE);
		behavior_type = 1;
	} else if (behavior_roll < 90) {
		status->mode = static_cast<enum e_mode>(MD_NORANDOMWALK);
		behavior_type = 2;
	} else {
		status->mode = static_cast<enum e_mode>(MD_NORANDOMWALK);
		behavior_type = 3;
	}

	// Compute derived stats (hit, flee, cri, def2, mdef2, batk)
	// This mirrors what MobDatabase::loadingFinished() does for regular mobs
	{
		mob_data dummy = {};
		dummy.type = BL_MOB;
		dummy.level = level;
		status_calc_misc(&dummy, status, level);
	}

	// View/chase range
	db->range2 = AREA_SIZE; // view range
	db->range3 = AREA_SIZE; // chase range

	// No drops/exp
	db->base_exp = 0;
	db->job_exp = 0;

	// Find walkable position
	int16 x = 0, y = 0;
	map_search_freecell(nullptr, m, &x, &y, -1, -1, 1);

	// Spawn using mob_once_spawn_sub
	mob_data *md = mob_once_spawn_sub(nullptr, m, x, y, name, mob_id, "", SZ_SMALL, AI_NONE);
	if (!md) {
		mob_db.erase(mob_id);
		return 0;
	}

	// Mark as fakeplayer
	md->special_state.clone = 1;
	md->special_state.fakeplayer = 1;

	// Create per-mob viewdata and generate appearance
	// This ensures the viewdata is directly on the mob instance, not shared via db->vd
	mob_set_dynamic_viewdata(md);
	fakeplayer_generate_viewdata(md->vd, job_class);
	md->vd->look[LOOK_BODY2] = job_class; // Required by modern clients

	// Set sitter pose on per-mob viewdata
	if (behavior_type == 3)
		md->vd->dead_sit = 2;

	// Spawn on map
	mob_spawn(md);

	return md->id;
}

// ============================================================
// Respawn timer callback — check and respawn dead fake players
// ============================================================
static TIMER_FUNC(fakeplayer_respawn_check) {
	if (!battle_config.fakeplayer_enabled)
		return 0;

	int32 target_count = (int32)fakeplayer_ids.size();
	if (target_count == 0)
		return 0;

	std::vector<int32> alive_ids;
	int32 dead_count = 0;

	for (int32 gid : fakeplayer_ids) {
		block_list *bl = map_id2bl(gid);
		if (bl != nullptr && bl->type == BL_MOB) {
			mob_data *md = (mob_data *)bl;
			if (md->special_state.fakeplayer && status_get_hp(bl) > 0) {
				alive_ids.push_back(gid);
				continue;
			}
		}
		dead_count++;
	}

	// Respawn dead fake players on new maps
	for (int32 i = 0; i < dead_count; i++) {
		int16 m = fakeplayer_select_map();
		if (m < 0)
			break;

		int32 new_gid = fakeplayer_spawn_one(m);
		if (new_gid > 0)
			alive_ids.push_back(new_gid);
	}

	fakeplayer_ids = alive_ids;
	return 0;
}

// ============================================================
// Public API
// ============================================================

/**
 * Spawn `count` fake players across random eligible maps.
 * Removes any existing fake players first.
 */
int32 fakeplayer_spawn(int32 count) {
	if (!battle_config.fakeplayer_enabled) {
		ShowWarning("fakeplayer_spawn: System is disabled via fakeplayer_enabled config.\n");
		return 0;
	}

	// Remove existing fake players
	fakeplayer_remove_all();

	int32 spawned = 0;
	for (int32 i = 0; i < count; i++) {
		int16 m = fakeplayer_select_map();
		if (m < 0) {
			ShowWarning("fakeplayer_spawn: No eligible maps found.\n");
			break;
		}

		int32 gid = fakeplayer_spawn_one(m);
		if (gid > 0) {
			fakeplayer_ids.push_back(gid);
			spawned++;
		}
	}

	ShowInfo("Fake Players: Spawned %d/%d fake players across the world.\n", spawned, count);
	return spawned;
}

/**
 * Remove all fake players from the game.
 */
void fakeplayer_remove_all(void) {
	for (int32 gid : fakeplayer_ids) {
		block_list *bl = map_id2bl(gid);
		if (bl != nullptr && bl->type == BL_MOB) {
			mob_data *md = (mob_data *)bl;
			if (md->special_state.fakeplayer) {
				uint32 mid = md->mob_id;
				unit_free(bl, CLR_OUTSIGHT);
				if (mob_is_clone(mid))
					mob_db.erase(mid);
			}
		}
	}
	fakeplayer_ids.clear();
}

/**
 * Return current active fake player count.
 */
int32 fakeplayer_count(void) {
	return (int32)fakeplayer_ids.size();
}

/**
 * Check if a block_list ID belongs to a fake player.
 */
bool fakeplayer_is_fakeplayer(int32 id) {
	block_list *bl = map_id2bl(id);
	if (bl != nullptr && bl->type == BL_MOB) {
		mob_data *md = (mob_data *)bl;
		return md->special_state.fakeplayer != 0;
	}
	return false;
}

/**
 * Check if a name belongs to a currently alive fake player.
 */
bool fakeplayer_is_fakeplayer_name(const char* name) {
	if (name == nullptr)
		return false;

	for (int32 gid : fakeplayer_ids) {
		block_list *bl = map_id2bl(gid);
		if (bl != nullptr && bl->type == BL_MOB) {
			mob_data *md = (mob_data *)bl;
			if (md->special_state.fakeplayer && strcmp(md->name, name) == 0)
				return true;
		}
	}
	return false;
}

/**
 * Initialize fake player system — sets up respawn timer.
 */
void fakeplayer_init(void) {
	fakeplayer_ids.clear();

	// Set up periodic respawn check
	int32 interval = battle_config.fakeplayer_respawn_interval * 1000; // config is in seconds
	if (interval < 10000)
		interval = 60000; // minimum 10 seconds, default 60

	fakeplayer_respawn_timer_id = add_timer_interval(
		gettick() + interval,
		fakeplayer_respawn_check,
		0, 0,
		interval
	);

	ShowStatus("Fake Player system initialized (respawn check every %ds).\n", interval / 1000);
}

/**
 * Cleanup fake player system.
 */
void fakeplayer_final(void) {
	fakeplayer_remove_all();

	if (fakeplayer_respawn_timer_id != INVALID_TIMER) {
		delete_timer(fakeplayer_respawn_timer_id, fakeplayer_respawn_check);
		fakeplayer_respawn_timer_id = INVALID_TIMER;
	}

	ShowStatus("Fake Player system finalized.\n");
}
