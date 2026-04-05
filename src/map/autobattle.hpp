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

	// State tracking
	t_tick last_support_tick;      ///< Throttle support casting
	t_tick last_loot_tick;         ///< Throttle loot checking
	t_tick last_roam_tick;         ///< Throttle roaming movement

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
	uint8 support_target_mode;     ///< 0=all party, 1=leader, 2=specific member
	char support_target_name[24];  ///< For specific member mode
	int32 follow_target_id;        ///< Account ID of current follow target (-1 = none)
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
