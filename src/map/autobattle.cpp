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
			struct status_data *st = status_get_status_data(bl);
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
 */
bool autobattle_can_attack(map_session_data *sd, struct block_list *target)
{
	if (!sd || !target || target->prev == nullptr)
		return false;

	// Check if target is alive
	if (status_isdead((block_list*)sd) || status_isdead(target))
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

	// Distance check
	struct status_data *sstatus = status_get_status_data((block_list*)sd);
	if (!sstatus)
		return false;

	int32 range = sstatus->rhw.range;
	if (!check_distance_bl((block_list*)sd, target, range))
		return false;

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

	// ===== AUTO-ATTACK =====
	if (sd->autobattle_data.mode & AUTOBATTLE_ATTACK) {
		struct block_list *target = autobattle_search_target(sd);

		if (target) {
			// Update target
			sd->autobattle_data.target_id = target->id;

			// Only attack if not in combat cooldown
			if (DIFF_TICK(sd->ud.canact_tick, tick) <= 0) {
				unit_attack((block_list*)sd, target->id, 1); // 1 = continuous attack
			}
		} else {
			// No valid target found
			sd->autobattle_data.target_id = -1;
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
				if (!skill_check_condition_castbegin((block_list*)sd, skill.skill_id, skill.skill_lv))
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

	// Stop existing timer
	if (sd->autobattle_data.attack_timer != INVALID_TIMER) {
		delete_timer(sd->autobattle_data.attack_timer, autobattle_process);
	}

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
