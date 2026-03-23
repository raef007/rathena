// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#include "autobattle.hpp"
#include "battle.hpp"      // battle_check_target, BL_* constants
#include "clif.hpp"        // clif_displaymessage
#include "itemdb.hpp"      // item database
#include "map.hpp"         // map_id2bl, map_getcell, map_foreachinarea
#include "mob.hpp"         // mob data
#include "pc.hpp"          // map_session_data, pc_*
#include "skill.hpp"       // skill_check, skill_can_use
#include "status.hpp"      // status_get_* functions
#include "unit.hpp"        // unit_attack, unit_move
#include <common/sql.hpp>  // Sql_Query, mmysql_handle

extern Sql* mmysql_handle;

#define AUTOBATTLE_TIMER_INTERVAL 500   // Main timer tick in milliseconds

// ===== Static search context for callbacks =====
struct s_autobattle_search_context {
	struct map_session_data *searcher;
	struct block_list *best_target;
	int32 best_target_id;
	int32 best_priority;
	int16 best_distance;
	int32 best_damage;
};

static struct s_autobattle_search_context g_search_context;

/**
 * Callback function for searching auto-attack targets
 * Signature matches: int32 (*func)(block_list*, va_list)
 */
static int32 autobattle_search_target_callback(struct block_list* bl, va_list ap)
{
	if (!bl)
		return 0;

	struct map_session_data *sd = g_search_context.searcher;
	if (!sd)
		return 0;

	// Skip self
	if (bl->id == sd->id)
		return 0;

	// Validate target
	if (!autobattle_can_attack(sd, bl))
		return 0;

	// Get distance
	int16 dist = distance_bl((block_list*)sd, bl);
	if (dist > sd->autobattle_data.range)
		return 0; // Out of range

	// Calculate priority
	int32 priority = 0;
	int32 damage = 0;

	if (sd->autobattle_data.target_priority == PRIORITY_DAMAGE || 
	    sd->autobattle_data.target_priority == PRIORITY_DAMAGE_DISTANCE) {
		// Estimate damage output from target (use mob ATK as proxy)
		if (bl->type == BL_MOB) {
			struct mob_data *md = (struct mob_data*)bl;
			damage = md->status.batk;
			priority = -damage; // Negative so highest damage gets lowest priority (best)
		} else if (bl->type == BL_PC) {
			// For players, use status ATK
			struct status_data *st = status_get_status_data(*bl);
			if (st)
				damage = st->batk;
			priority = -damage;
		}
	}

	// Apply distance as tiebreaker if needed
	if (sd->autobattle_data.target_priority == PRIORITY_DISTANCE || 
	    (sd->autobattle_data.target_priority == PRIORITY_DAMAGE_DISTANCE && damage == g_search_context.best_damage)) {
		priority = dist;
	}

	// Update best target
	if (g_search_context.best_target_id == -1 || priority < g_search_context.best_priority) {
		g_search_context.best_target_id = bl->id;
		g_search_context.best_priority = priority;
		g_search_context.best_distance = dist;
		g_search_context.best_damage = damage;
		g_search_context.best_target = bl;
	}

	return 0; // Continue searching
}

/**
 * Search for auto-attack target based on configured priority
 */
struct block_list* autobattle_search_target(map_session_data *sd)
{
	if (!sd || !(sd->autobattle_data.mode & AUTOBATTLE_ATTACK))
		return nullptr;

	// Initialize search context
	g_search_context.searcher = sd;
	g_search_context.best_target = nullptr;
	g_search_context.best_target_id = -1;
	g_search_context.best_priority = INT_MAX;
	g_search_context.best_distance = INT16_MAX;
	g_search_context.best_damage = 0;

	// Search in defined range
	map_foreachinrange(autobattle_search_target_callback, (block_list*)sd, sd->autobattle_data.range, BL_MOB | BL_PC);

	if (g_search_context.best_target && g_search_context.best_target->prev != nullptr)
		return g_search_context.best_target;

	return nullptr;
}

/**
 * Validate if player is allowed to attack target
 * NOTE: This checks legality only (alive, enemy, not shielded).
 * Distance checks are handled separately by the caller.
 */
bool autobattle_can_attack(map_session_data *sd, struct block_list *target)
{
	if (!sd || !target || target->prev == nullptr)
		return false;

	// Check if target is alive
	if (status_isdead(*sd) || status_isdead(*target))
		return false;

	// Check if it's a valid enemy
	if (battle_check_target((block_list*)sd, target, BCT_ENEMY) <= 0)
		return false;

	// Check if we can use normal attack
	if (!status_check_skilluse((block_list*)sd, target, 0, 0))
		return false;

	// PvP safety: Can only attack other player if they also have auto-attack
	if (target->type == BL_PC) {
		struct map_session_data *target_sd = (struct map_session_data*)target;
		if (!(target_sd->autobattle_data.mode & AUTOBATTLE_ATTACK))
			return false; // Cannot auto-attack players without auto-attack enabled
	}

	return true;
}

/**
 * Callback function for looting items
 * Signature matches: int32 (*func)(block_list*, va_list)
 */
static struct map_session_data *g_loot_searcher = nullptr;

static int32 autobattle_loot_callback(struct block_list* bl, va_list ap)
{
	if (!bl || !g_loot_searcher || bl->type != BL_ITEM)
		return 0;

	struct map_session_data *sd = g_loot_searcher;
	struct flooritem_data *fitem = (struct flooritem_data*)bl;

	// Check distance
	int16 dist = distance_bl((block_list*)sd, bl);
	if (dist > sd->autobattle_data.loot_range)
		return 0;

	// Check rarity filter if enabled
	if (sd->autobattle_data.loot_rarity_filter > 0) {
		// TODO: Implement item rarity checking
	}

	// Attempt to loot item
	pc_takeitem(sd, fitem);

	return 0;
}

/**
 * Main auto-battle process - called by timer
 * Handles: auto-attack, auto-support, auto-loot
 */
int autobattle_process(int tid, t_tick tick, int id, intptr_t data)
{
	struct map_session_data *sd = map_id2sd(id);
	if (!sd)
		return 0;

	// ===== TIME CAP CHECK =====
	sd->autobattle_data.time_deduct_accum += AUTOBATTLE_TIMER_INTERVAL;
	if (sd->autobattle_data.time_deduct_accum >= 1000) {
		int32 secs = sd->autobattle_data.time_deduct_accum / 1000;
		sd->autobattle_data.daily_seconds_used += secs;
		sd->autobattle_data.seconds_remaining -= secs;
		sd->autobattle_data.time_deduct_accum %= 1000;

		// Periodic DB save every 60 seconds of usage
		if (sd->autobattle_data.daily_seconds_used % 60 < secs)
			autobattle_save_time_db(sd);

		// Warn at 5 minutes remaining
		if (sd->autobattle_data.seconds_remaining == 300)
			clif_displaymessage(sd->fd, "[Auto-Battle] Warning: 5 minutes remaining.");
		// Warn at 1 minute remaining
		if (sd->autobattle_data.seconds_remaining == 60)
			clif_displaymessage(sd->fd, "[Auto-Battle] Warning: 1 minute remaining!");
	}
	if (sd->autobattle_data.seconds_remaining <= 0) {
		clif_displaymessage(sd->fd, "[Auto-Battle] Daily time expired. Auto-battle stopped.");
		sd->autobattle_data.seconds_remaining = 0;
		autobattle_save_time_db(sd);
		autobattle_stop(sd);
		return 0;
	}

	// ===== AUTO-ATTACK =====
	if (sd->autobattle_data.mode & AUTOBATTLE_ATTACK) {
		struct block_list *target = autobattle_search_target(sd);

		if (target) {
			// Update target
			sd->autobattle_data.target_id = target->id;

			// Check weapon range for melee approach
			struct status_data *sstatus = status_get_status_data(*sd);
			int32 weapon_range = sstatus ? sstatus->rhw.range : 1;

			if (!check_distance_bl((block_list*)sd, target, weapon_range)) {
				// Target in detection range but out of weapon range - walk towards it
				unit_walktobl((block_list*)sd, target, weapon_range, 1);
			} else if (DIFF_TICK(sd->ud.canact_tick, tick) <= 0) {
				unit_attack((block_list*)sd, target->id, 1); // 1 = continuous attack
			}
		} else {
			// No valid target found
			sd->autobattle_data.target_id = -1;

			// Auto-roam when idle
			if (sd->autobattle_data.mode & AUTOBATTLE_ROAM) {
				if (DIFF_TICK(tick, sd->autobattle_data.last_roam_tick) >= battle_config.autobattle_roam_interval) {
					sd->autobattle_data.last_roam_tick = tick;
					int16 rx = sd->bl.x, ry = sd->bl.y;
					if (map_random_dir((block_list*)sd, &rx, &ry)) {
						unit_walktoxy((block_list*)sd, rx, ry, 0);
					}
				}
			}
		}
	}

	// ===== AUTO-SUPPORT =====
	if (sd->autobattle_data.mode & AUTOBATTLE_SUPPORT) {
		// Throttle support casting (only check every 100ms to avoid spam)
		if (DIFF_TICK(tick, sd->autobattle_data.last_support_tick) >= 100) {
			sd->autobattle_data.last_support_tick = tick;

			for (int i = 0; i < sd->autobattle_data.support_skill_count; i++) {
				struct s_autosupport_skill &skill = sd->autobattle_data.support_skills[i];
				
				if (skill.skill_id == 0)
					continue;

				// Check cooldown
				if (DIFF_TICK(tick, skill.last_cast_time) < 2000) // 2 second cooldown between same skill
					continue;

				// Find target based on scope
				struct map_session_data *target = nullptr;

				switch (skill.target_scope) {
					case AUTOSUPPORT_SELF:
						target = sd;
						break;
					case AUTOSUPPORT_PARTY:
						// TODO: Implement party member selection
						target = sd;
						break;
					case AUTOSUPPORT_GUILD:
						// TODO: Implement guild member selection
						target = sd;
						break;
					default:
						target = sd;
				}

				if (!target)
					continue;

				// Check HP threshold
				int hp_percent = (target->battle_status.hp > 0) ? 
					(target->battle_status.hp * 100) / target->battle_status.max_hp : 0;

				if (hp_percent >= skill.hp_threshold)
					continue; // HP not below threshold

				// Check if skill is usable
				if (!skill_check_condition_castbegin(*sd, skill.skill_id, skill.skill_lv))
					continue;

				// Check MP
				int32 skill_cost = skill_get_sp(skill.skill_id, skill.skill_lv);
				if (sd->status.sp < skill_cost)
					continue;

				// Cast skill
				unit_skilluse_id((block_list*)sd, ((block_list*)target)->id, skill.skill_id, skill.skill_lv);
				skill.last_cast_time = tick;
				break; // Only cast one support skill per frame
			}
		}
	}

	// ===== AUTO-LOOT =====
	if (sd->autobattle_data.mode & AUTOBATTLE_LOOT) {
		// Throttle loot checking (only check every 200ms)
		if (DIFF_TICK(tick, sd->autobattle_data.last_loot_tick) >= 200) {
			sd->autobattle_data.last_loot_tick = tick;

			// Set global context for callback
			g_loot_searcher = sd;

			// Search for items in loot range
			map_foreachinrange(autobattle_loot_callback, (block_list*)sd, sd->autobattle_data.loot_range, BL_ITEM);

			// Clear context
			g_loot_searcher = nullptr;
		}
	}

	// ===== SKILL ROTATION =====
	if (sd->autobattle_data.mode & AUTOBATTLE_SKILLROTATION) {
		// Skill rotation is handled during attack animations
		// This is just a placeholder for future expansion
	}

	// Reschedule timer
	sd->autobattle_data.attack_timer = add_timer(
		tick + AUTOBATTLE_TIMER_INTERVAL,
		autobattle_process,
		sd->id,
		0
	);

	return 0;
}

/**
 * Toggle auto-battle mode
 */
void autobattle_toggle_mode(map_session_data *sd, uint8 mode, bool on)
{
	if (!sd)
		return;

	if (on) {
		sd->autobattle_data.mode |= mode;
	} else {
		sd->autobattle_data.mode &= ~mode;
	}
}

/**
 * Set auto-attack range
 */
void autobattle_set_range(map_session_data *sd, uint8 range)
{
	if (!sd)
		return;

	// Clamp range between 1 and 15 cells
	sd->autobattle_data.range = std::min(std::max(range, (uint8)1), (uint8)15);
}

/**
 * Add auto-support skill
 */
void autobattle_add_support_skill(map_session_data *sd, uint16 skill_id,
	uint8 skill_lv, uint8 hp_threshold, uint8 target_scope)
{
	if (!sd || sd->autobattle_data.support_skill_count >= 10)
		return;

	// Check if skill already exists
	for (int i = 0; i < sd->autobattle_data.support_skill_count; i++) {
		if (sd->autobattle_data.support_skills[i].skill_id == skill_id) {
			// Update existing
			sd->autobattle_data.support_skills[i].skill_lv = skill_lv;
			sd->autobattle_data.support_skills[i].hp_threshold = hp_threshold;
			sd->autobattle_data.support_skills[i].target_scope = target_scope;
			return;
		}
	}

	// Add new skill
	struct s_autosupport_skill &skill = sd->autobattle_data.support_skills[sd->autobattle_data.support_skill_count];
	skill.skill_id = skill_id;
	skill.skill_lv = skill_lv;
	skill.hp_threshold = hp_threshold;
	skill.target_scope = target_scope;
	skill.last_cast_time = 0;
	sd->autobattle_data.support_skill_count++;
}

/**
 * Clear all support skills
 */
void autobattle_clear_support_skills(map_session_data *sd)
{
	if (!sd)
		return;

	memset(sd->autobattle_data.support_skills, 0, sizeof(sd->autobattle_data.support_skills));
	sd->autobattle_data.support_skill_count = 0;
}

/**
 * Configure skill rotation slot
 */
void autobattle_set_skillrotation(map_session_data *sd, uint8 slot,
	uint16 *skill_ids, uint8 count)
{
	if (!sd || slot >= 3 || count > 10)
		return;

	struct s_skillrotation_slot &rotation = sd->autobattle_data.rotations[slot];
	rotation.skill_count = count;
	rotation.current_index = 0;

	for (int i = 0; i < count; i++) {
		rotation.skill_ids[i] = skill_ids[i];
	}
}

/**
 * Start auto-battle system
 */
void autobattle_start(map_session_data *sd)
{
	if (!sd || sd->autobattle_data.mode == AUTOBATTLE_OFF)
		return;

	// Check time cap
	if (sd->autobattle_data.seconds_remaining <= 0) {
		clif_displaymessage(sd->fd, "[Auto-Battle] No daily time remaining. Come back tomorrow or add bonus time.");
		sd->autobattle_data.mode = AUTOBATTLE_OFF;
		return;
	}

	// Stop existing timer
	if (sd->autobattle_data.attack_timer != INVALID_TIMER) {
		delete_timer(sd->autobattle_data.attack_timer, autobattle_process);
	}

	// Record start time and reset accumulator
	sd->autobattle_data.start_tick = gettick();
	sd->autobattle_data.time_deduct_accum = 0;

	// Start new timer
	sd->autobattle_data.attack_timer = add_timer(
		gettick() + AUTOBATTLE_TIMER_INTERVAL,
		autobattle_process,
		sd->id,
		0
	);
}

/**
 * Stop auto-battle system
 */
void autobattle_stop(map_session_data *sd)
{
	if (!sd)
		return;

	if (sd->autobattle_data.attack_timer != INVALID_TIMER) {
		delete_timer(sd->autobattle_data.attack_timer, autobattle_process);
		sd->autobattle_data.attack_timer = INVALID_TIMER;
	}

	// Persist daily usage to DB before stopping
	autobattle_save_time_db(sd);

	sd->autobattle_data.mode = AUTOBATTLE_OFF;
	sd->autobattle_data.target_id = -1;
}

/**
 * Initialize auto-battle state (on character load)
 */
void autobattle_init(map_session_data *sd, const s_autobattle_config *config)
{
	if (!sd)
		return;

	// Initialize default state
	sd->autobattle_data.mode = AUTOBATTLE_OFF;
	sd->autobattle_data.range = 9;
	sd->autobattle_data.target_priority = PRIORITY_DAMAGE_DISTANCE;
	sd->autobattle_data.target_id = -1;
	sd->autobattle_data.attack_timer = INVALID_TIMER;
	sd->autobattle_data.support_skill_count = 0;
	sd->autobattle_data.current_rotation_slot = 0;
	sd->autobattle_data.loot_range = 9;
	sd->autobattle_data.loot_rarity_filter = 0;
	sd->autobattle_data.last_support_tick = 0;
	sd->autobattle_data.last_loot_tick = 0;
	sd->autobattle_data.last_roam_tick = 0;
	sd->autobattle_data.start_tick = 0;
	sd->autobattle_data.daily_seconds_used = 0;
	sd->autobattle_data.bonus_seconds = 0;
	sd->autobattle_data.daily_limit = battle_config.autobattle_default_seconds; // fallback
	sd->autobattle_data.seconds_remaining = battle_config.autobattle_default_seconds;
	sd->autobattle_data.time_deduct_accum = 0;

	// Load daily time data from database (overrides defaults above)
	autobattle_load_time_db(sd);

	// Load from config if provided
	if (config) {
		sd->autobattle_data.mode = config->mode;
		sd->autobattle_data.range = config->range;
		sd->autobattle_data.target_priority = config->target_priority;
		sd->autobattle_data.loot_range = config->loot_range;
		sd->autobattle_data.loot_rarity_filter = config->loot_rarity_filter;

		// Load support skills
		sd->autobattle_data.support_skill_count = config->support_skill_count;
		for (int i = 0; i < config->support_skill_count && i < 10; i++) {
			autobattle_add_support_skill(sd,
				config->support_skill_ids[i],
				config->support_skill_lvs[i],
				config->support_skill_thresholds[i],
				config->support_skill_scopes[i]
			);
		}
	}
}

/**
 * Save auto-battle state to persistent config
 */
void autobattle_save(map_session_data *sd, s_autobattle_config *config)
{
	if (!sd || !config)
		return;

	config->mode = sd->autobattle_data.mode;
	config->range = sd->autobattle_data.range;
	config->target_priority = sd->autobattle_data.target_priority;
	config->loot_range = sd->autobattle_data.loot_range;
	config->loot_rarity_filter = sd->autobattle_data.loot_rarity_filter;

	// Save support skills
	config->support_skill_count = sd->autobattle_data.support_skill_count;
	for (int i = 0; i < sd->autobattle_data.support_skill_count && i < 10; i++) {
		config->support_skill_ids[i] = sd->autobattle_data.support_skills[i].skill_id;
		config->support_skill_lvs[i] = sd->autobattle_data.support_skills[i].skill_lv;
		config->support_skill_thresholds[i] = sd->autobattle_data.support_skills[i].hp_threshold;
		config->support_skill_scopes[i] = sd->autobattle_data.support_skills[i].target_scope;
	}
}

/**
 * Add bonus time to a character's auto-battle allowance (persisted to DB)
 */
void autobattle_add_time(map_session_data *sd, int32 seconds)
{
	if (!sd)
		return;

	sd->autobattle_data.bonus_seconds += seconds;
	sd->autobattle_data.seconds_remaining += seconds;

	// Cap at server maximum
	int32 max_seconds = battle_config.autobattle_max_seconds;
	if (sd->autobattle_data.seconds_remaining > max_seconds)
		sd->autobattle_data.seconds_remaining = max_seconds;

	// Persist bonus immediately
	autobattle_save_time_db(sd);
}

/**
 * Get remaining auto-battle time in seconds
 */
int32 autobattle_get_remaining_time(map_session_data *sd)
{
	if (!sd)
		return 0;

	return sd->autobattle_data.seconds_remaining;
}

/**
 * Load daily time data from database
 * - Reads limits from autobattle_settings table
 * - Reads per-character usage from char_autobattle_config
 * - Auto-resets daily_seconds_used if new calendar day
 */
void autobattle_load_time_db(map_session_data *sd)
{
	if (!sd)
		return;

	int32 char_id = sd->status.char_id;

	// --- Step 1: Read limits from autobattle_settings ---
	int32 normal_limit = battle_config.autobattle_default_seconds; // fallback
	int32 vip_limit = battle_config.autobattle_default_seconds * 2; // fallback
	int32 max_bonus = battle_config.autobattle_max_seconds; // fallback

	if (SQL_ERROR != Sql_Query(mmysql_handle,
		"SELECT `setting_name`, `setting_value` FROM `autobattle_settings`"))
	{
		char *name, *val;
		while (SQL_SUCCESS == Sql_NextRow(mmysql_handle)) {
			Sql_GetData(mmysql_handle, 0, &name, nullptr);
			Sql_GetData(mmysql_handle, 1, &val, nullptr);
			if (name && val) {
				if (strcmp(name, "daily_limit_seconds") == 0)
					normal_limit = atoi(val);
				else if (strcmp(name, "vip_daily_limit_seconds") == 0)
					vip_limit = atoi(val);
				else if (strcmp(name, "max_bonus_seconds") == 0)
					max_bonus = atoi(val);
			}
		}
		Sql_FreeResult(mmysql_handle);
	} else {
		Sql_ShowDebug(mmysql_handle);
	}

	// Determine this player's daily limit
	int32 daily_limit = normal_limit;
#ifdef VIP_ENABLE
	if (pc_isvip(sd))
		daily_limit = vip_limit;
#endif
	sd->autobattle_data.daily_limit = daily_limit;

	// --- Step 2: Read per-character usage ---
	int32 db_daily_used = 0;
	int32 db_bonus = 0;
	bool needs_reset = true; // default: treat as new day
	bool row_exists = false;

	if (SQL_ERROR != Sql_Query(mmysql_handle,
		"SELECT `daily_seconds_used`, `bonus_seconds`, "
		"`last_reset_date` >= CURDATE() "
		"FROM `char_autobattle_config` WHERE `char_id` = %d", char_id))
	{
		if (SQL_SUCCESS == Sql_NextRow(mmysql_handle)) {
			char *col0, *col1, *col2;
			Sql_GetData(mmysql_handle, 0, &col0, nullptr);
			Sql_GetData(mmysql_handle, 1, &col1, nullptr);
			Sql_GetData(mmysql_handle, 2, &col2, nullptr);
			if (col0) db_daily_used = atoi(col0);
			if (col1) db_bonus = atoi(col1);
			if (col2) needs_reset = (atoi(col2) == 0); // 0 = date is before today
			row_exists = true;
		}
		Sql_FreeResult(mmysql_handle);
	} else {
		Sql_ShowDebug(mmysql_handle);
	}

	// --- Step 3: Apply ---
	if (needs_reset) {
		// New day — reset usage, keep bonus
		sd->autobattle_data.daily_seconds_used = 0;
		sd->autobattle_data.bonus_seconds = db_bonus;
	} else {
		// Same day — restore usage
		sd->autobattle_data.daily_seconds_used = db_daily_used;
		sd->autobattle_data.bonus_seconds = db_bonus;
	}

	sd->autobattle_data.seconds_remaining =
		daily_limit - sd->autobattle_data.daily_seconds_used + sd->autobattle_data.bonus_seconds;

	if (sd->autobattle_data.seconds_remaining < 0)
		sd->autobattle_data.seconds_remaining = 0;
	if (sd->autobattle_data.seconds_remaining > max_bonus)
		sd->autobattle_data.seconds_remaining = max_bonus;

	// Create row if it doesn't exist
	if (!row_exists) {
		Sql_Query(mmysql_handle,
			"INSERT INTO `char_autobattle_config` (`char_id`, `daily_seconds_used`, `bonus_seconds`, `last_reset_date`) "
			"VALUES (%d, 0, 0, CURDATE()) "
			"ON DUPLICATE KEY UPDATE `char_id` = `char_id`", char_id);
	}

	// If new day, update the reset date in DB
	if (needs_reset && row_exists) {
		Sql_Query(mmysql_handle,
			"UPDATE `char_autobattle_config` SET `daily_seconds_used` = 0, "
			"`last_reset_date` = CURDATE() WHERE `char_id` = %d", char_id);
	}
}

/**
 * Save daily time data to database
 */
void autobattle_save_time_db(map_session_data *sd)
{
	if (!sd)
		return;

	int32 char_id = sd->status.char_id;

	if (SQL_ERROR == Sql_Query(mmysql_handle,
		"UPDATE `char_autobattle_config` SET "
		"`daily_seconds_used` = %d, `bonus_seconds` = %d, "
		"`last_reset_date` = CURDATE() "
		"WHERE `char_id` = %d",
		sd->autobattle_data.daily_seconds_used,
		sd->autobattle_data.bonus_seconds,
		char_id))
	{
		Sql_ShowDebug(mmysql_handle);
	}
}
