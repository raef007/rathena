// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#ifndef AUTOBATTLE_HPP
#define AUTOBATTLE_HPP

#include <common/cbasetypes.hpp>
#include <common/mmo.hpp>
#include <common/timer.hpp>

class map_session_data;
struct block_list;

/// Auto-Battle Pass item ID (must match db/import/item_db.yml entry)
#define AUTOBATTLE_ITEM_ID 30000
#define AUTOBATTLE_MAX_ITEM_BUFFS 10

/**
 * Auto-Battle Mode Flags
 * Used as bitmask in map_session_data::autobattle_data.mode
 */
enum e_autobattle_mode {
	AUTOBATTLE_OFF = 0,
	AUTOBATTLE_ATTACK = 0x01,      ///< Auto-attack enabled
	AUTOBATTLE_SUPPORT = 0x02,     ///< Auto-support (heal/buff) enabled
	AUTOBATTLE_LOOT = 0x04,        ///< Auto-loot enabled
	AUTOBATTLE_SKILLROTATION = 0x08, ///< Skill rotation enabled
	AUTOBATTLE_ROAM = 0x10,        ///< Auto-roam when no enemies in range
	AUTOBATTLE_AUTOPOT = 0x20,     ///< Auto-use potions when HP/SP low
	AUTOBATTLE_FLYWING = 0x40,     ///< Use fly wing to teleport when roaming
	AUTOBATTLE_AUTOSIT = 0x80,     ///< Auto-sit when HP/SP low, stand when recovered
	AUTOBATTLE_TELESKILL = 0x100,  ///< Cast AL_TELEPORT skill (no item consumed). Mutually exclusive with FLYWING.
};

/**
 * Auto-Support Target Scope
 */
enum e_autosupport_scope {
	AUTOSUPPORT_SELF = 0,         ///< Target self only
	AUTOSUPPORT_PARTY = 1,        ///< Target self and party members
	AUTOSUPPORT_GUILD = 2,        ///< Target self, party, and guild members
};

/**
 * Auto-Attack Target Priority
 */
enum e_autobattle_priority {
	PRIORITY_DAMAGE = 0,           ///< Highest damage first
	PRIORITY_DISTANCE = 1,         ///< Closest distance first
	PRIORITY_DAMAGE_DISTANCE = 2,  ///< Damage first, then distance as tiebreaker
};

/**
 * Configuration for a single auto-support skill
 */
struct s_autosupport_skill {
	uint16 skill_id;
	uint8 skill_lv;
	uint8 hp_threshold;            ///< Trigger when target HP < threshold (%)
	uint8 trigger_type;            ///< 0=HP below, 1=missing buff
	uint16 buff_id;                ///< For trigger_type=1, which buff to check
	uint8 target_scope;            ///< e_autosupport_scope
	int32 last_cast_time;          ///< Cooldown tracker
};

struct s_autosupport_item {
	t_itemid item_id;
	int16 status_id;
	t_tick last_use_tick;
};

/**
 * Skill rotation slot (player can save up to 3 different rotations)
 */
struct s_skillrotation_slot {
	uint16 skill_ids[10];
	uint8 skill_count;
	uint8 current_index;           ///< Current position in rotation
};

/**
 * Runtime auto-battle state for a player
 */
struct s_autobattle_data {
	uint16 mode;                   ///< Bitmask of e_autobattle_mode
	uint8 range;                   ///< Attack range in cells (default 9)
	uint8 target_priority;         ///< e_autobattle_priority
	int32 target_id;               ///< Current target (-1 if none)
	int32 attack_timer;            ///< Timer ID for attack loop

	// Auto-support configuration
	uint8 support_skill_count;     ///< Number of configured support skills
	struct s_autosupport_skill support_skills[10]; ///< Up to 10 support skill configs
	uint8 support_item_count;      ///< Number of configured self buff items
	struct s_autosupport_item support_items[AUTOBATTLE_MAX_ITEM_BUFFS];

	// Skill rotation
	uint8 current_rotation_slot;   ///< Which of 3 rotation slots is active
	struct s_skillrotation_slot rotations[3]; ///< 3 configurable rotation slots

	// Auto-loot configuration
	uint16 loot_range;             ///< Loot pickup range (cells, default=attack_range)
	uint8 loot_rarity_filter;      ///< 0=all items, 1=white/blue/purple/gold only, 2=rare+ only

	// Auto-pot configuration
	t_itemid autopot_hp_id;        ///< Item ID for HP potion (0 = disabled)
	uint8 autopot_hp_threshold;    ///< Use HP pot when HP% below this (default 50)
	t_itemid autopot_sp_id;        ///< Item ID for SP potion (0 = disabled)
	uint8 autopot_sp_threshold;    ///< Use SP pot when SP% below this (default 30)
	bool gohome_no_pots;           ///< Butterfly wing / walk home when out of pots
	t_tick last_pot_tick;           ///< Throttle potion usage

	// Roaming state
	int16 roam_dest_x;             ///< Current roam destination X
	int16 roam_dest_y;             ///< Current roam destination Y
	bool roam_has_dest;            ///< Whether we have an active roam destination
	int16 roam_last_x;             ///< Position at previous roam tick (stuck detection)
	int16 roam_last_y;             ///< Position at previous roam tick (stuck detection)
	int32 roam_best_dist;          ///< Best (smallest) distance to dest achieved so far (maze escape)
	t_tick roam_best_tick;         ///< Tick when roam_best_dist was last improved
	int32 roam_initial_dist;       ///< Manhattan distance to dest at the moment it was picked (for ≥30%-progress check)
	// Heading-based directional commit: bot picks one of 8 compass headings
	// and makes 3-5 short picks within ±60° of it before auto-rotating to a
	// new heading. Auto-rotation prevents edge-walking on open maps; the
	// blacklist still fires when a heading is genuinely blocked early. Mob-
	// seek always overrides and resets nothing (heading resumes after combat).
	uint8 roam_heading;                  ///< Current heading 0..7 (N, NE, E, SE, S, SW, W, NW)
	bool  roam_has_heading;              ///< Whether a heading is currently chosen
	uint8 roam_heading_picks_left;       ///< Picks remaining on current heading before auto-rotate
	t_tick roam_heading_blocked_until[8];///< Per-heading blacklist expiry
	// Recent-visit ring buffer: gives the bot temporal memory of where it has
	// been, so it doesn't keep re-entering dead-ends in maze-like corridors.
	// 64 entries × ~3-cell snapshot stride covers ~190 cells of recent path —
	// enough that a small enclosed sub-room of the map can't be fully filled
	// before the bot is pushed out of it.
	int16 roam_visit_x[64];
	int16 roam_visit_y[64];
	uint8 roam_visit_head;         ///< Next write position in ring buffer
	uint8 roam_visit_count;        ///< Number of valid entries (saturates at 64)
	int16 roam_snapshot_x;         ///< Last position snapshotted into visit buffer
	int16 roam_snapshot_y;         ///< (only snapshot when we've moved far enough)

	// Quadrant explore tracker: divides the map into 4 quadrants (NW/NE/SW/SE)
	// and remembers the last tick we set an ultimate destination in each one.
	// When picking a new destination, prefer the staler quadrant. Coarse map
	// memory at near-zero cost — covers the "stop circling the same area" case.
	t_tick roam_quadrant_tick[4];

	// Multi-slot unreachable-target blacklist. When unit_walktobl fails or
	// the bot 3-strikes a mob-seek destination, the failed mob's bl_id and
	// expiry tick are recorded here. Search and mob-seek skip blacklisted
	// IDs. 8 slots so a maze with several wall-blocked mobs doesn't cause
	// a single-slot blacklist to keep getting overwritten and looping.
	// Replacement policy is round-robin via unreachable_head; oldest entry
	// gets evicted first. Inactive slots have id == 0.
	int32 unreachable_target_id;       // legacy field (kept for save/load compat); use the array
	t_tick unreachable_target_until;   // legacy field
	int32 unreachable_ids[8];
	t_tick unreachable_until[8];
	uint8 unreachable_fails[8];   ///< Per-slot repeat-fail count: 1st blacklist = 30s, 2nd+ = 5 min escalation
	uint8 unreachable_head;

	// "3 strikes" wall counter. Increments every time the hop search has to
	// fall back to pass 2 or 3 (sidestep / escape hatch) — i.e. forward-
	// progress hops were all blocked. Resets on a successful forward hop or
	// when a new destination is picked. After 3 strikes we abandon the
	// current ultimate destination, much faster than the 15s stall timer.
	uint8 roam_struggle_count;

	// When mob-seek picks an ultimate destination, store the mob's bl_id here
	// so 3-strikes / stall abandonment can blacklist that mob — the next
	// mob-seek pick then chooses a different mob (likely in a different
	// direction) instead of looping on the same wall-blocked one. 0 = the
	// current dest came from the random+quadrant fallback (no specific mob).
	int32 roam_dest_mob_id;

	// Directional quadrant blacklist: when wall hits abandon a destination,
	// the *direction* that destination was in (relative to player) is
	// blacklisted for 30 seconds. Mob-seek skips mobs in that direction;
	// random+quadrant skips that quadrant. Forces the bot to commit to
	// another direction (north / south / opposite) when a wall keeps blocking
	// pursuit. 4 entries indexed by NW/NE/SW/SE (low x = 0 bit, low y = 0 bit
	// — see autobattle_direction_quadrant).
	t_tick failed_direction_until[4];

	// Cooldown timer for Fly Wing teleport. Independent of roam ticks so the
	// bot can keep walk-roaming continuously while Fly Wing fires every 3s
	// of no-combat as a side-behavior.
	t_tick last_flywing_tick;

	// Auto-sit retaliation: track HP between ticks. If HP dropped since last
	// sample, we got hit — record the tick. Sit logic uses this to stand up
	// and fight back even before HP/SP are fully recovered. canmove_tick
	// (the previous fight-back signal) doesn't reliably extend when sitting,
	// because the engine doesn't bother updating walk timers on a stationary
	// target.
	int32 autosit_last_hp;
	t_tick autosit_last_hit_tick;

	// Auto-support party cycling. In support_target_mode = 0 ("all party"
	// auto-lock), we now CYCLE through party members every 30 seconds
	// instead of locking onto a single one for the whole session. The cycle
	// index is a logical position into the eligible-members list; advance
	// it when 30s have passed since last_cycle_tick. Modes 1 (leader) and
	// 2 (specific name) ignore cycling — only one valid target exists.
	t_tick last_cycle_tick;
	uint8  cycle_member_pos;

	// State tracking
	t_tick last_support_tick;      ///< Throttle support casting
	t_tick last_item_buff_tick;    ///< Throttle self buff item checks
	t_tick last_loot_tick;         ///< Throttle loot checking
	t_tick last_roam_tick;         ///< Throttle roaming movement
	t_tick last_combat_tick;       ///< Last tick where a target was fought/found

	// Time cap (DB-persisted daily limit)
	t_tick start_tick;             ///< When auto-battle was last activated
	int32 seconds_remaining;       ///< Remaining seconds of auto-battle time today
	int32 daily_seconds_used;      ///< Seconds used today (persisted to DB)
	int32 bonus_seconds;           ///< Bonus seconds (from purchases, persisted to DB)
	int32 daily_limit;             ///< Today's limit loaded from DB settings table (0 = unlimited)
	int32 time_deduct_accum;       ///< Accumulator for sub-second time deduction (ms)

	// EXP penalty (loaded from DB, fallback to battle_config)
	int32 exp_penalty_base;        ///< Base EXP penalty % (0=none, 50=half, 100=zero)
	int32 exp_penalty_job;         ///< Job EXP penalty % (0=none, 50=half, 100=zero)

	// Phase 24: Auto-Sit
	uint8 autosit_hp_threshold;    ///< Sit when HP% below this (0 = don't sit for HP)
	uint8 autosit_sp_threshold;    ///< Sit when SP% below this (0 = don't sit for SP)
	uint8 autosit_hp_recover;      ///< Stand when HP% above this (threshold + 20, cap 95)
	uint8 autosit_sp_recover;      ///< Stand when SP% above this (threshold + 20, cap 95)

	// Phase 22: Auto-Target (monster whitelist)
	uint16 target_mob_ids[20];     ///< Whitelist of mob IDs to target (0 = target all)
	uint8 target_mob_count;        ///< Number of entries in whitelist (0 = target all)

	// Phase 23: Auto-Skill (offensive skill)
	uint16 attack_skill_id;        ///< Offensive skill to use (0 = normal attack only)
	uint8 attack_skill_lv;         ///< Skill level to use

	// Phase 25: Auto-Support Enhancement
	uint8 support_target_mode;     ///< 0=auto-lock party member, 1=leader, 2=specific member
	char support_target_name[24];  ///< For specific member mode
	int32 follow_target_id;        ///< Block ID of current follow target (-1 = none)
	t_tick last_follow_tick;       ///< Throttle long-distance follow teleports
};

/**
 * Persistent auto-battle configuration (saved to character DB)
 */
struct s_autobattle_config {
	uint8 mode;                    ///< Default mode (AUTOBATTLE_*)
	uint8 range;                   ///< Default range
	uint8 target_priority;         ///< Default target priority
	uint8 support_skill_count;
	uint8 support_skill_ids[10];
	uint8 support_skill_lvs[10];
	uint8 support_skill_thresholds[10];
	uint8 support_skill_scopes[10];
	uint16 loot_range;
	uint8 loot_rarity_filter;
};

// ==================== FUNCTION DECLARATIONS ====================

/**
 * Search for auto-attack target based on priority
 * @param sd Player session data
 * @return found target block_list pointer, or nullptr if no valid target
 */
struct block_list* autobattle_search_target(map_session_data *sd);

/**
 * Validate if player can attack a target
 * @param sd Player session data
 * @param target Target block_list
 * @return true if attack is legal, false otherwise
 */
bool autobattle_can_attack(map_session_data *sd, struct block_list *target);

/**
 * Main auto-battle process function (timer callback)
 * Handles: attack, support, loot, skill rotation
 * @param tid Timer ID
 * @param tick Current game tick
 * @param id Player account ID
 * @param data User data (unused)
 */
int autobattle_process(int tid, t_tick tick, int id, intptr_t data);

/**
 * Toggle auto-battle mode on/off
 * @param sd Player session data
 * @param mode Mode to toggle (e_autobattle_mode bit)
 * @param on true to enable, false to disable
 */
void autobattle_toggle_mode(map_session_data *sd, uint16 mode, bool on);

/**
 * Set auto-attack range
 * @param sd Player session data
 * @param range Range in cells
 */
void autobattle_set_range(map_session_data *sd, uint8 range);

/**
 * Add auto-support skill configuration
 * @param sd Player session data
 * @param skill_id Skill ID to support with
 * @param skill_lv Skill level
 * @param hp_threshold HP% threshold to trigger
 * @param target_scope e_autosupport_scope
 */
void autobattle_add_support_skill(map_session_data *sd, uint16 skill_id,
	uint8 skill_lv, uint8 hp_threshold, uint8 target_scope, uint8 trigger_type = 0);

/**
 * Clear all auto-support skills
 * @param sd Player session data
 */
void autobattle_clear_support_skills(map_session_data *sd);

/**
 * Add auto-support self item buff configuration.
 * @param sd Player session data
 * @param item_id Item ID to use
 * @param status_id Status effect to wait on before reusing
 */
void autobattle_add_support_item(map_session_data *sd, t_itemid item_id, int16 status_id);

/**
 * Clear all support item buffs
 * @param sd Player session data
 */
void autobattle_clear_support_items(map_session_data *sd);

/**
 * Detect the primary status effect applied by an item buff.
 * @param item_id Item ID to inspect
 * @return sc_type value or SC_NONE
 */
int16 autobattle_get_item_buff_status(t_itemid item_id);

/**
 * Classify a skill as a self/ally buff candidate for the auto-support menu.
 * Filters out trap, NPC, song/ensemble, quest, wedding, spirit, guild skills,
 * passive skills, and damage-dealing skills. Requires a status effect (skill->sc).
 */
bool autobattle_is_buff_skill(uint16 skill_id);

/**
 * Classify a skill as an offensive skill candidate for the auto-attack skill menu.
 * Returns true for damage skills the player can target an enemy with.
 */
bool autobattle_is_attack_skill(uint16 skill_id);

/**
 * Remove a specific support skill by skill_id
 * @param sd Player session data
 * @param skill_id Skill ID to remove
 * @return true if found and removed, false otherwise
 */
bool autobattle_remove_support_skill(map_session_data *sd, uint16 skill_id);

/**
 * Configure skill rotation slot
 * @param sd Player session data
 * @param slot Rotation slot (0-2)
 * @param skill_ids Array of skill IDs
 * @param count Number of skills
 */
void autobattle_set_skillrotation(map_session_data *sd, uint8 slot,
	uint16 *skill_ids, uint8 count);

/**
 * Start auto-battle system for a player
 * @param sd Player session data
 */
void autobattle_start(map_session_data *sd);

/**
 * Stop auto-battle system for a player
 * @param sd Player session data
 */
void autobattle_stop(map_session_data *sd);

/**
 * Initialize auto-battle state (called on character load)
 * @param sd Player session data
 * @param config Persistent config loaded from DB
 */
void autobattle_init(map_session_data *sd, const s_autobattle_config *config);

/**
 * Save auto-battle state to persistent config (called on logout/change)
 * @param sd Player session data
 * @param config Config to save to
 */
void autobattle_save(map_session_data *sd, s_autobattle_config *config);

/**
 * Load daily time data from database (called on login)
 * Reads autobattle_settings for limits, char_autobattle_config for usage
 * @param sd Player session data
 */
void autobattle_load_time_db(map_session_data *sd);

/**
 * Save daily time data to database (called on logout and periodically)
 * @param sd Player session data
 */
void autobattle_save_time_db(map_session_data *sd);

/**
 * Save full auto-battle config to char_autobattle_config (called on stop/logout).
 * Persists the user's HP/SP recovery items, thresholds, sit thresholds, attack
 * skill, support skills/items, gohome flag, and the configured mode bitmask.
 * The Auto-Attack and Auto-Support bits are stripped on load — those are toggled
 * explicitly each session.
 */
void autobattle_save_config_db(map_session_data *sd);

/**
 * Load full auto-battle config from char_autobattle_config (called from init).
 * Populates `sd->autobattle_data` with the saved fields. Mode bitmask is
 * masked on load to clear AUTOBATTLE_ATTACK and AUTOBATTLE_SUPPORT, so the
 * player has to explicitly enable those each session.
 */
void autobattle_load_config_db(map_session_data *sd);

/**
 * Add bonus time to a character's auto-battle allowance (persisted to DB)
 * @param sd Player session data
 * @param seconds Seconds to add
 */
void autobattle_add_time(map_session_data *sd, int32 seconds);

/**
 * Get remaining auto-battle time in seconds
 * @param sd Player session data
 * @return Remaining seconds
 */
int32 autobattle_get_remaining_time(map_session_data *sd);

// Phase 22: Auto-Target
void autobattle_set_target_mobs(map_session_data *sd, uint16 *mob_ids, uint8 count);
void autobattle_clear_target_mobs(map_session_data *sd);
void autobattle_toggle_target_mob(map_session_data *sd, uint16 mob_id);

// Phase 23: Auto-Skill
void autobattle_set_attack_skill(map_session_data *sd, uint16 skill_id, uint8 skill_lv);

#endif // AUTOBATTLE_HPP
