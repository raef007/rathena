// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#include "autobattle.hpp"
#include "battle.hpp"      // battle_check_target, BL_* constants
#include "clif.hpp"        // clif_displaymessage
#include "itemdb.hpp"      // item database
#include "map.hpp"         // map_id2bl, map_getcell, map_foreachinarea
#include "mob.hpp"         // mob data
#include "npc.hpp"         // npc_check_areanpc
#include "path.hpp"        // path_search (roam path validation)
#include "pc.hpp"          // map_session_data, pc_*
#include "skill.hpp"       // skill_check, skill_can_use
#include "status.hpp"      // status_get_* functions
#include "unit.hpp"        // unit_attack, unit_move
#include "party.hpp"       // party_search, party_isleader (Phase 25)
#include <common/sql.hpp>  // Sql_Query, mmysql_handle
#include <common/random.hpp> // rnd_value
#include <cmath>           // sin, cos, sqrt for roam waypoint angle sweep
#include <algorithm>
#include <cctype>
#include <string>

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

// Forward declarations — definitions below; needed because the picker calls
// the quadrant helper while picking destinations, and the mob-seek callback
// calls the direction-blacklist helper which is defined later.
static int32 autobattle_quadrant_of(map_session_data *sd, int16 x, int16 y);
static int32 autobattle_direction_quadrant(map_session_data *sd, int16 x, int16 y);
static bool autobattle_direction_blacklisted(map_session_data *sd, int16 x, int16 y, t_tick tick);

/**
 * Multi-slot unreachable-mob blacklist helpers. Single-slot blacklist gets
 * overwritten in mazes with multiple wall-blocked mobs and the bot loops
 * between them. 8-slot ring buffer with round-robin replacement.
 */
static bool autobattle_is_unreachable(map_session_data *sd, int32 id, t_tick tick)
{
	if (id <= 0)
		return false;
	for (int i = 0; i < 8; i++) {
		if (sd->autobattle_data.unreachable_ids[i] == id &&
			DIFF_TICK(tick, sd->autobattle_data.unreachable_until[i]) < 0)
			return true;
	}
	return false;
}

static void autobattle_blacklist_unreachable(map_session_data *sd, int32 id, t_tick until)
{
	if (id <= 0)
		return;
	// If this id already has a slot, just refresh its expiry (longer wins).
	for (int i = 0; i < 8; i++) {
		if (sd->autobattle_data.unreachable_ids[i] == id) {
			if (DIFF_TICK(until, sd->autobattle_data.unreachable_until[i]) > 0)
				sd->autobattle_data.unreachable_until[i] = until;
			return;
		}
	}
	// Otherwise prefer an empty/expired slot before evicting an active one.
	t_tick now = gettick();
	for (int i = 0; i < 8; i++) {
		if (sd->autobattle_data.unreachable_ids[i] == 0 ||
			DIFF_TICK(now, sd->autobattle_data.unreachable_until[i]) >= 0) {
			sd->autobattle_data.unreachable_ids[i] = id;
			sd->autobattle_data.unreachable_until[i] = until;
			return;
		}
	}
	// All slots active — round-robin evict the head.
	uint8 h = sd->autobattle_data.unreachable_head;
	sd->autobattle_data.unreachable_ids[h] = id;
	sd->autobattle_data.unreachable_until[h] = until;
	sd->autobattle_data.unreachable_head = (h + 1) % 8;
}

static bool autobattle_has_status(map_session_data *sd, int16 status_id)
{
	return sd && status_id > SC_NONE && sd->sc.hasSCE((enum sc_type)status_id);
}

static bool autobattle_name_contains(const std::shared_ptr<item_data>& item, const char *needle)
{
	if (!item || !needle)
		return false;

	std::string aegis = item->name;
	std::string display = item->ename;
	std::string key = needle;
	auto lower = [](unsigned char c) { return (char)std::tolower(c); };
	std::transform(aegis.begin(), aegis.end(), aegis.begin(), lower);
	std::transform(display.begin(), display.end(), display.begin(), lower);
	std::transform(key.begin(), key.end(), key.begin(), lower);
	return aegis.find(key) != std::string::npos || display.find(key) != std::string::npos;
}

static bool autobattle_item_is_box(const std::shared_ptr<item_data>& item)
{
	return autobattle_name_contains(item, "box");
}

int16 autobattle_get_item_buff_status(t_itemid item_id)
{
	std::shared_ptr<item_data> item = item_db.find(item_id);
	if (!item || (item->type != IT_USABLE && item->type != IT_CASH))
		return SC_NONE;
	if (autobattle_item_is_box(item))
		return SC_NONE;

	// ASPD potions
	if (item_id == 645 || autobattle_name_contains(item, "concentration_potion"))
		return SC_ASPDPOTION0;
	if (item_id == 656 || autobattle_name_contains(item, "awakening_potion"))
		return SC_ASPDPOTION1;
	if (item_id == 657 || autobattle_name_contains(item, "berserk_potion"))
		return SC_ASPDPOTION2;

	// Stat-buff scrolls / consumables
	if (autobattle_name_contains(item, "abrasive"))
		return SC_INCCRI;
	if (autobattle_name_contains(item, "blessing_scroll") ||
		autobattle_name_contains(item, "bless_scroll") ||
		(autobattle_name_contains(item, "blessing") && autobattle_name_contains(item, "scroll")))
		return SC_BLESSING;
	if (autobattle_name_contains(item, "inc_agi") || autobattle_name_contains(item, "agi_scroll") ||
		autobattle_name_contains(item, "increase_agi"))
		return SC_INCREASEAGI;

	return SC_NONE;
}

bool autobattle_is_buff_skill(uint16 skill_id)
{
	if (skill_id == 0 || !skill_get_index(skill_id))
		return false;

	// Reject obvious non-buff categories
	if (skill_get_inf2(skill_id, INF2_ISTRAP) ||
		skill_get_inf2(skill_id, INF2_ISNPC) ||
		skill_get_inf2(skill_id, INF2_ISQUEST) ||
		skill_get_inf2(skill_id, INF2_ISWEDDING) ||
		skill_get_inf2(skill_id, INF2_ISSPIRIT) ||
		skill_get_inf2(skill_id, INF2_ISGUILD) ||
		skill_get_inf2(skill_id, INF2_ISSONG) ||
		skill_get_inf2(skill_id, INF2_ISENSEMBLE) ||
		skill_get_inf2(skill_id, INF2_ISCHORUS))
		return false;

	uint16 inf = skill_get_inf(skill_id);
	if (inf == INF_PASSIVE_SKILL)
		return false;
	// Only self-cast or ally-target skills count as buffs.
	if (!(inf & (INF_SELF_SKILL | INF_SUPPORT_SKILL)))
		return false;

	// Damage-dealing (e.g. AL_HOLYLIGHT) are not buffs.
	if (!skill_get_nk(skill_id, NK_NODAMAGE))
		return false;

	// A buff must produce a status effect we can check for re-cast.
	if (skill_get_sc(skill_id) <= SC_NONE)
		return false;

	return true;
}

bool autobattle_is_attack_skill(uint16 skill_id)
{
	if (skill_id == 0 || !skill_get_index(skill_id))
		return false;

	if (skill_get_inf2(skill_id, INF2_ISNPC) ||
		skill_get_inf2(skill_id, INF2_ISQUEST) ||
		skill_get_inf2(skill_id, INF2_ISWEDDING) ||
		skill_get_inf2(skill_id, INF2_ISSPIRIT) ||
		skill_get_inf2(skill_id, INF2_ISGUILD))
		return false;

	// Explicit whitelist for offensive skills that are routed through the
	// no-damage castend path (NK_NODAMAGE) but still deal weapon/magic damage.
	// The structural filter below should pick these up via skill_get_type, but
	// we keep the whitelist as a backstop so weird skill_db edits or stale
	// flag combinations don't silently drop them from the menu again.
	switch (skill_id) {
		case KN_BRANDISHSPEAR:
		case KN_SPEARBOOMERANG:
		case KN_BOWLINGBASH:
		case KN_PIERCE:
		case KN_SPEARSTAB:
		case SM_BASH:
		case SM_MAGNUM:
			return true;
		default:
			break;
	}

	uint16 inf = skill_get_inf(skill_id);
	if (inf == INF_PASSIVE_SKILL)
		return false;

	// Must be able to target an enemy: attack/ground/trap.
	if (!(inf & (INF_ATTACK_SKILL | INF_GROUND_SKILL | INF_TRAP_SKILL)))
		return false;

	// Filter out skills that don't deal damage (Sanctuary, Warp Portal, ground
	// utility). We check the battle-flag here instead of NK_NODAMAGE because
	// some offensive skills (e.g. KN_BRANDISHSPEAR) have NK_NODAMAGE set as a
	// routing hint even though they actually deal weapon damage. skill_type
	// is the authoritative "does this hit for damage" signal: BF_NONE for
	// pure utility, BF_WEAPON/MAGIC/MISC for anything that can damage.
	if (skill_get_type(skill_id) == BF_NONE)
		return false;

	return true;
}

// ===== Mob-seek roam =====
// Scan the entire map for the nearest live mob and use its position as the
// ultimate destination. Makes the bot look like it's hunting (because it is)
// instead of wandering randomly. Falls back to random walkable picks when the
// map has no mobs at all.

struct s_autobattle_nearmob_ctx {
	map_session_data *searcher;
	struct block_list *best;
	int32 best_dist; // Manhattan
};
static struct s_autobattle_nearmob_ctx g_nearmob_ctx;

static int32 autobattle_nearmob_callback(struct block_list* bl, va_list ap)
{
	if (!bl || bl->type != BL_MOB)
		return 0;
	struct map_session_data *sd = g_nearmob_ctx.searcher;
	if (!sd)
		return 0;

	struct mob_data *md = (struct mob_data*)bl;

	// Skip dead, summoned (slaves/pets/clones), or in-PvP-mode mobs.
	if (status_isdead(*bl))
		return 0;
	if (md->special_state.ai > 0)
		return 0;

	// Whitelist filter — when the player has a target whitelist, only count
	// mobs in it. Without this, the bot would walk toward any mob even though
	// it'll just ignore them on arrival.
	if (sd->autobattle_data.target_mob_count > 0) {
		bool in_list = false;
		for (int i = 0; i < sd->autobattle_data.target_mob_count; i++) {
			if (sd->autobattle_data.target_mob_ids[i] == md->mob_id) {
				in_list = true;
				break;
			}
		}
		if (!in_list)
			return 0;
	}

	// Skip mobs we already know are unreachable from our current position.
	if (autobattle_is_unreachable(sd, bl->id, gettick()))
		return 0;

	// Skip mobs in a direction-quadrant we recently failed to reach (wall
	// hits in that direction). Forces the bot to commit to another direction
	// for the blacklist duration. After ~2 mins the blacklist lapses and
	// we'll try that direction again — by then position/world have changed.
	if (autobattle_direction_blacklisted(sd, bl->x, bl->y, gettick()))
		return 0;

	int32 dist = abs(bl->x - sd->x) + abs(bl->y - sd->y);
	if (dist < g_nearmob_ctx.best_dist) {
		g_nearmob_ctx.best_dist = dist;
		g_nearmob_ctx.best = bl;
	}
	return 0;
}

/**
 * Find the nearest live mob anywhere on the same map. Returns nullptr if no
 * eligible mob exists (empty map, all dead, all unreachable, or all filtered
 * out by the whitelist).
 */
static struct block_list* autobattle_find_nearest_mob(map_session_data *sd)
{
	if (!sd)
		return nullptr;

	g_nearmob_ctx.searcher = sd;
	g_nearmob_ctx.best = nullptr;
	g_nearmob_ctx.best_dist = INT32_MAX;

	map_foreachinmap(autobattle_nearmob_callback, sd->m, BL_MOB);

	return g_nearmob_ctx.best;
}

/**
 * Pick the ultimate destination. Mob-seek mode: the nearest live mob anywhere
 * on the map becomes the ultimate destination. If no mob exists (or all are
 * filtered/unreachable), fall back to random walkable cell biased toward the
 * stalest map quadrant (preserves coverage on empty maps).
 *
 * No A* validation on the destination itself — we navigate via short A* hops,
 * so the cell just needs to not be a wall. Visit memory is reset on a fresh
 * destination (old footprints are stale relative to the new geometry).
 */
static bool autobattle_pick_ultimate_dest(map_session_data *sd, t_tick tick)
{
	struct map_data *mapdata = map_getmapdata(sd->m);
	if (!mapdata)
		return false;

	// Mob-seek: nearest live mob's position becomes the ultimate destination.
	// The hop loop walks toward it; once the mob enters detection range
	// (15 cells), the normal target search picks it up and combat starts.
	// This is the "feels like the bot is hunting" mode.
	struct block_list *nearest = autobattle_find_nearest_mob(sd);
	if (nearest != nullptr) {
		sd->autobattle_data.roam_dest_x = nearest->x;
		sd->autobattle_data.roam_dest_y = nearest->y;
		sd->autobattle_data.roam_has_dest = true;
		sd->autobattle_data.last_roam_tick = tick;
		sd->autobattle_data.roam_best_dist = abs(nearest->x - sd->x) + abs(nearest->y - sd->y);
		sd->autobattle_data.roam_best_tick = tick;
		sd->autobattle_data.roam_dest_mob_id = nearest->id;

		int32 dest_q = autobattle_quadrant_of(sd, nearest->x, nearest->y);
		sd->autobattle_data.roam_quadrant_tick[dest_q] = tick;

		// Fresh destination = fresh memory.
		sd->autobattle_data.roam_visit_head = 0;
		sd->autobattle_data.roam_visit_count = 0;
		sd->autobattle_data.roam_snapshot_x = sd->x;
		sd->autobattle_data.roam_snapshot_y = sd->y;
		sd->autobattle_data.roam_struggle_count = 0;
		{
			char dbg[160];
			int32 dq = autobattle_direction_quadrant(sd, nearest->x, nearest->y);
			static const char *qname[] = {"NW", "NE", "SW", "SE"};
			snprintf(dbg, sizeof(dbg),
				"[Roam] mob-seek picked mob %d at (%d,%d) — dir %s, dist %d",
				nearest->id, nearest->x, nearest->y, qname[dq],
				abs(nearest->x - sd->x) + abs(nearest->y - sd->y));
			clif_displaymessage(sd->fd, dbg);
		}
		return true;
	}

	// No mob anywhere on the map (or all unreachable/filtered). Fall back to
	// the random walkable picker biased by stalest quadrant — preserves
	// continuous map exploration on empty maps.

	const int32 edge = battle_config.map_edge_size;
	const int16 map_w = (int16)(mapdata->xs - edge * 2);
	const int16 map_h = (int16)(mapdata->ys - edge * 2);
	// Aim for ~3/8 the smaller map dimension. Destinations close to the
	// player produce "wandering in a tight area" where the strike counter
	// and stall timers don't trip — the bot keeps making short forward
	// hops within a pocket. Forcing far destinations means each new pick
	// is meaningfully in a different region of the map. Clamped 30-80 so
	// small maps still have viable picks.
	int16 min_dist = (int16)(((map_w < map_h) ? map_w : map_h) * 3 / 8);
	if (min_dist < 30) min_dist = 30;
	if (min_dist > 80) min_dist = 80;

	// Find the stalest quadrant (oldest last-visited tick, or never visited).
	// Prefer one different from the player's current quadrant so we always
	// commit to going SOMEWHERE other than where we already are.
	int32 cur_q = autobattle_quadrant_of(sd, sd->x, sd->y);
	int32 target_q = -1;
	t_tick oldest = 0;
	bool oldest_set = false;
	for (int32 q = 0; q < 4; q++) {
		if (q == cur_q)
			continue;
		t_tick t = sd->autobattle_data.roam_quadrant_tick[q];
		if (!oldest_set || t < oldest) {
			oldest = t;
			oldest_set = true;
			target_q = q;
		}
	}
	if (target_q < 0)
		target_q = (cur_q + 1) % 4; // Fallback shouldn't happen.

	const int16 mid_x = (int16)(mapdata->xs / 2);
	const int16 mid_y = (int16)(mapdata->ys / 2);
	const int16 qx_lo = (target_q & 1) ? mid_x : (int16)edge;
	const int16 qx_hi = (target_q & 1) ? (int16)(mapdata->xs - edge - 1) : mid_x;
	const int16 qy_lo = (target_q & 2) ? mid_y : (int16)edge;
	const int16 qy_hi = (target_q & 2) ? (int16)(mapdata->ys - edge - 1) : mid_y;

	// First 150 attempts: stay inside the chosen quadrant AND respect the
	// direction blacklist (skip cells in directions where wall hits recently
	// blacklisted us). Next 30 attempts: drop the quadrant constraint but
	// keep the direction blacklist. Last 20 attempts: drop both — pure
	// whole-map sampling so we never return false for lack of options.
	for (int32 attempts = 0; attempts < 200; attempts++) {
		int16 rx, ry;
		if (attempts < 150) {
			rx = rnd_value<int16>(qx_lo, qx_hi);
			ry = rnd_value<int16>(qy_lo, qy_hi);
		} else {
			rx = rnd_value<int16>((int16)edge, (int16)(mapdata->xs - edge - 1));
			ry = rnd_value<int16>((int16)edge, (int16)(mapdata->ys - edge - 1));
		}
		if (map_getcell(sd->m, rx, ry, CELL_CHKNOPASS))
			continue;
		if ((abs(rx - sd->x) + abs(ry - sd->y)) < min_dist)
			continue;
		// Honour direction blacklist for the first 180 attempts. After that,
		// drop the constraint as a last-resort escape so we always have SOME
		// destination to head toward.
		if (attempts < 180 && autobattle_direction_blacklisted(sd, rx, ry, tick))
			continue;

		sd->autobattle_data.roam_dest_x = rx;
		sd->autobattle_data.roam_dest_y = ry;
		sd->autobattle_data.roam_has_dest = true;
		sd->autobattle_data.last_roam_tick = tick;
		sd->autobattle_data.roam_best_dist = abs(rx - sd->x) + abs(ry - sd->y);
		sd->autobattle_data.roam_best_tick = tick;
		sd->autobattle_data.roam_dest_mob_id = 0; // Random pick — no mob attached.

		{
			char dbg[160];
			int32 dq = autobattle_direction_quadrant(sd, rx, ry);
			static const char *qname[] = {"NW", "NE", "SW", "SE"};
			snprintf(dbg, sizeof(dbg),
				"[Roam] random+quadrant picked (%d,%d) — dir %s (mob-seek found nothing)",
				rx, ry, qname[dq]);
			clif_displaymessage(sd->fd, dbg);
		}

		// Mark the destination's quadrant as visited NOW (we'll be there
		// shortly). This stops the same quadrant from being re-picked
		// immediately if the bot bounces destinations rapidly.
		int32 dest_q = autobattle_quadrant_of(sd, rx, ry);
		sd->autobattle_data.roam_quadrant_tick[dest_q] = tick;

		// Fresh destination = fresh memory.
		sd->autobattle_data.roam_visit_head = 0;
		sd->autobattle_data.roam_visit_count = 0;
		sd->autobattle_data.roam_snapshot_x = sd->x;
		sd->autobattle_data.roam_snapshot_y = sd->y;
		sd->autobattle_data.roam_struggle_count = 0;
		return true;
	}
	return false;
}

/**
 * Returns true if (tx, ty) is within `radius` Manhattan cells of any entry in
 * the recent-visit ring buffer. This is the "I've been there recently" check
 * that gives the bot temporal memory of dead-ends.
 */
static bool autobattle_is_recently_visited(map_session_data *sd, int16 tx, int16 ty, int32 radius)
{
	for (uint8 vi = 0; vi < sd->autobattle_data.roam_visit_count; vi++) {
		int32 vd = abs(tx - sd->autobattle_data.roam_visit_x[vi]) +
		           abs(ty - sd->autobattle_data.roam_visit_y[vi]);
		if (vd <= radius)
			return true;
	}
	return false;
}

/**
 * Snapshot the player's position into the visit ring buffer if they've moved
 * far enough since the last snapshot. 3-cell stride × 64 entries = ~190 cells
 * of recent path retained — three times the previous coverage.
 */
static void autobattle_snapshot_visit(map_session_data *sd)
{
	int32 moved = abs(sd->x - sd->autobattle_data.roam_snapshot_x) +
	              abs(sd->y - sd->autobattle_data.roam_snapshot_y);
	if (moved < 3)
		return;

	uint8 h = sd->autobattle_data.roam_visit_head;
	sd->autobattle_data.roam_visit_x[h] = sd->x;
	sd->autobattle_data.roam_visit_y[h] = sd->y;
	sd->autobattle_data.roam_visit_head = (h + 1) % 64;
	if (sd->autobattle_data.roam_visit_count < 64)
		sd->autobattle_data.roam_visit_count++;
	sd->autobattle_data.roam_snapshot_x = sd->x;
	sd->autobattle_data.roam_snapshot_y = sd->y;
}

/**
 * Quadrant index 0..3 for a cell. Splits the map into NW/NE/SW/SE based on
 * the player's current map dimensions.
 *   0 = NW (low x, low y)
 *   1 = NE (high x, low y)
 *   2 = SW (low x, high y)
 *   3 = SE (high x, high y)
 */
static int32 autobattle_quadrant_of(map_session_data *sd, int16 x, int16 y)
{
	struct map_data *mapdata = map_getmapdata(sd->m);
	if (!mapdata)
		return 0;
	int32 q = 0;
	if (x >= mapdata->xs / 2) q |= 1;
	if (y >= mapdata->ys / 2) q |= 2;
	return q;
}

/**
 * Direction-from-player quadrant — 0 NW / 1 NE / 2 SW / 3 SE relative to the
 * player's CURRENT position. Used to mark "this direction keeps hitting walls,
 * stop trying to go that way for a while" via failed_direction_until[].
 */
static int32 autobattle_direction_quadrant(map_session_data *sd, int16 x, int16 y)
{
	int32 q = 0;
	if (x >= sd->x) q |= 1;
	if (y >= sd->y) q |= 2;
	return q;
}

/**
 * Returns true if (x, y) lies in a direction-from-player quadrant that's
 * currently blacklisted (wall-blocked recently). Mob-seek and the
 * random+quadrant fallback both skip blacklisted quadrants.
 */
static bool autobattle_direction_blacklisted(map_session_data *sd, int16 x, int16 y, t_tick tick)
{
	int32 q = autobattle_direction_quadrant(sd, x, y);
	return DIFF_TICK(tick, sd->autobattle_data.failed_direction_until[q]) < 0;
}

/**
 * Roam: persistent far destination + A*-validated short hops biased toward it,
 * with a 32-entry visited-cell memory to escape mazes and dead-ends.
 *
 * The bot picks a random "ultimate" destination anywhere on the map and
 * remembers it across combat. Each tick (when no walk is in flight) it picks
 * a short A*-validated hop biased toward that destination.
 *
 * Four-pass hop search, in priority order:
 *   pass 0: forward-progress AND not-recently-visited  (preferred — purposeful exploration)
 *   pass 1: forward-progress only                       (corridor walking through own footprints)
 *   pass 2: not-recently-visited only                   (sidestep around obstacle into new ground)
 *   pass 3: any A*-reachable hop                        (escape hatch — keep moving)
 *
 * Visited memory snapshots position every 2 cells of movement (32-entry ring
 * buffer). A hop within 3 Manhattan cells of any buffer entry is "recently
 * visited". Old entries rotate out — so a corridor the bot left ~60 cells ago
 * becomes re-visitable. This is what breaks the dead-end loops you see in
 * pyramid maps.
 *
 * If all four passes fail (truly cornered), we abandon the destination and
 * clear visit memory so the next tick gets a clean retry.
 */
static void autobattle_roam_walk(map_session_data *sd, t_tick tick)
{
	if (!sd)
		return;

	// Currently walking? Let the engine drive. While walking, keep snapshotting
	// position so the visit buffer reflects the path we're actually traversing,
	// not just the points where we re-issue walktoxy.
	if (sd->ud.walktimer != INVALID_TIMER) {
		autobattle_snapshot_visit(sd);
		return;
	}

	// No ultimate destination yet? Pick one. Resets visit memory.
	if (!sd->autobattle_data.roam_has_dest) {
		autobattle_pick_ultimate_dest(sd, tick);
		if (!sd->autobattle_data.roam_has_dest)
			return;
	}

	int16 dest_x = sd->autobattle_data.roam_dest_x;
	int16 dest_y = sd->autobattle_data.roam_dest_y;
	int32 cur_to_dest = abs(sd->x - dest_x) + abs(sd->y - dest_y);

	// Arrived? Pick a new ultimate destination.
	if (cur_to_dest <= 3) {
		sd->autobattle_data.roam_has_dest = false;
		autobattle_pick_ultimate_dest(sd, tick);
		if (!sd->autobattle_data.roam_has_dest)
			return;
		dest_x = sd->autobattle_data.roam_dest_x;
		dest_y = sd->autobattle_data.roam_dest_y;
		cur_to_dest = abs(sd->x - dest_x) + abs(sd->y - dest_y);
	}

	// Track best distance to dest. Improvements reset the stall timer; 15s
	// without progress means the dest is in a pocket we can't reach — abandon,
	// blacklist the mob, blacklist the direction. The 3-strikes path covered
	// the "lots of sidesteps" case but missed THIS one: when the bot keeps
	// making *tiny* forward hops along a corridor that ends in a wall, it
	// occasionally improves roam_best_dist by a cell or two before stalling.
	// 3-strikes never fires (pass 0 keeps succeeding), but progress IS stalled.
	// Without the blacklist update here, mob-seek immediately re-picks the
	// same wall-blocked mob and the bot loops forever (the screenshot bug).
	if (cur_to_dest < sd->autobattle_data.roam_best_dist) {
		sd->autobattle_data.roam_best_dist = cur_to_dest;
		sd->autobattle_data.roam_best_tick = tick;
	} else if (DIFF_TICK(tick, sd->autobattle_data.roam_best_tick) > 15000) {
		// Blacklist the failed direction (relative to current player position)
		// for 30s, and the source mob for 30s, so mob-seek picks something
		// different next iteration.
		int32 fq = autobattle_direction_quadrant(sd, dest_x, dest_y);
		sd->autobattle_data.failed_direction_until[fq] = tick + 30000;
		{
			static const char *qname[] = {"NW", "NE", "SW", "SE"};
			char dbg[160];
			snprintf(dbg, sizeof(dbg),
				"[Roam] 15s stall to (%d,%d) — blacklist dir %s + mob %d for 30s",
				dest_x, dest_y, qname[fq], sd->autobattle_data.roam_dest_mob_id);
			clif_displaymessage(sd->fd, dbg);
		}
		if (sd->autobattle_data.roam_dest_mob_id > 0) {
			autobattle_blacklist_unreachable(sd,
				sd->autobattle_data.roam_dest_mob_id, tick + 30000);
			sd->autobattle_data.roam_dest_mob_id = 0;
		}
		sd->autobattle_data.roam_has_dest = false;
		return;
	}

	// Absolute 90s timeout. Genuinely unreachable destinations get rotated —
	// same blacklist update as the 15s stall path.
	if (DIFF_TICK(tick, sd->autobattle_data.last_roam_tick) > 90000) {
		int32 fq = autobattle_direction_quadrant(sd, dest_x, dest_y);
		sd->autobattle_data.failed_direction_until[fq] = tick + 30000;
		if (sd->autobattle_data.roam_dest_mob_id > 0) {
			autobattle_blacklist_unreachable(sd,
				sd->autobattle_data.roam_dest_mob_id, tick + 30000);
			sd->autobattle_data.roam_dest_mob_id = 0;
		}
		sd->autobattle_data.roam_has_dest = false;
		return;
	}

	// Snapshot current position into visit memory before picking the next hop —
	// candidates near our current cell will then be filtered out by pass 0/2.
	autobattle_snapshot_visit(sd);

	// === Hop selection ===
	struct map_data *mapdata = map_getmapdata(sd->m);
	if (!mapdata)
		return;
	const int32 edge = battle_config.map_edge_size;

	// Direction toward destination, normalised. Bias hop angles toward this.
	double dx_d = dest_x - sd->x;
	double dy_d = dest_y - sd->y;
	double len = sqrt(dx_d * dx_d + dy_d * dy_d);
	if (len < 1.0) len = 1.0;
	double ux = dx_d / len;
	double uy = dy_d / len;

	// Angles fan out from "straight at dest" (0°) outward to "behind us" (180°).
	// Order matters: forward first, widen as needed.
	static const double angle_deg[] = {
		0.0, 22.5, -22.5, 45.0, -45.0, 67.5, -67.5,
		90.0, -90.0, 112.5, -112.5, 135.0, -135.0, 157.5, -157.5, 180.0
	};
	static const int32 n_angles = (int32)(sizeof(angle_deg) / sizeof(angle_deg[0]));

	// Hop radii: long fluid hops first, but include 1-2 cell options for tight
	// pyramid-style corridors where a 1-cell-wide passage is the only way out.
	const int32 max_hop = std::max(8, std::min(15, battle_config.max_walk_path - 1));
	const int32 radii[] = { max_hop, max_hop * 2 / 3, max_hop / 2, 4, 2, 1 };
	const int32 n_radii = (int32)(sizeof(radii) / sizeof(radii[0]));

	const int32 visited_radius = 3; // Cells within this distance count as "visited"

	for (int32 pass = 0; pass < 4; pass++) {
		bool require_progress = (pass == 0 || pass == 1);
		bool avoid_visited    = (pass == 0 || pass == 2);

		for (int32 ri = 0; ri < n_radii; ri++) {
			int32 r = radii[ri];

			for (int32 ai = 0; ai < n_angles; ai++) {
				double theta = angle_deg[ai] * 0.017453292519943295;
				double c = cos(theta), s = sin(theta);
				double rx = ux * c - uy * s;
				double ry = ux * s + uy * c;
				int16 tx = sd->x + (int16)(rx * r);
				int16 ty = sd->y + (int16)(ry * r);

				if (tx < edge) tx = (int16)edge;
				if (ty < edge) ty = (int16)edge;
				if (tx >= mapdata->xs - edge) tx = (int16)(mapdata->xs - edge - 1);
				if (ty >= mapdata->ys - edge) ty = (int16)(mapdata->ys - edge - 1);

				if (tx == sd->x && ty == sd->y)
					continue;

				if (require_progress) {
					int32 cand_to_dest = abs(tx - dest_x) + abs(ty - dest_y);
					if (cand_to_dest >= cur_to_dest)
						continue;
				}

				if (avoid_visited && autobattle_is_recently_visited(sd, tx, ty, visited_radius))
					continue;

				if (map_getcell(sd->m, tx, ty, CELL_CHKNOPASS))
					continue;

				// A* validate. This is the wall-staring guard — if A* says
				// no path, we never issue the walk.
				walkpath_data wpd = { 0 };
				if (!path_search(&wpd, sd->m, sd->x, sd->y, tx, ty, 0, CELL_CHKNOPASS))
					continue;
				if (wpd.path_len == 0 || wpd.path_len > battle_config.max_walk_path)
					continue;

				if (unit_walktoxy((block_list*)sd, tx, ty, 0)) {
					// Count strikes: forward-progress hops (pass 0/1) reset
					// the counter — we're going somewhere useful. Sidestep
					// (pass 2) and escape (pass 3) increment — those are
					// "hit a wall, work around it" hops. Three strikes and
					// the destination gets abandoned even if the 15s stall
					// timer hasn't fired yet.
					if (pass <= 1) {
						sd->autobattle_data.roam_struggle_count = 0;
					} else {
						sd->autobattle_data.roam_struggle_count++;
						if (sd->autobattle_data.roam_struggle_count >= 3) {
							// Blacklist the failed direction (relative to
							// current player position) for 30s. Mob-seek will
							// skip mobs that lie in this direction next pick
							// — bot has to commit to a different direction.
							int32 fq = autobattle_direction_quadrant(sd, dest_x, dest_y);
							sd->autobattle_data.failed_direction_until[fq] = tick + 30000;
							{
								static const char *qname[] = {"NW", "NE", "SW", "SE"};
								char dbg[160];
								snprintf(dbg, sizeof(dbg),
									"[Roam] 3-strikes on (%d,%d) — blacklist dir %s + mob %d for 30s",
									dest_x, dest_y, qname[fq], sd->autobattle_data.roam_dest_mob_id);
								clif_displaymessage(sd->fd, dbg);
							}
							sd->autobattle_data.roam_has_dest = false;
							sd->autobattle_data.roam_struggle_count = 0;
							if (sd->autobattle_data.roam_dest_mob_id > 0) {
								autobattle_blacklist_unreachable(sd,
									sd->autobattle_data.roam_dest_mob_id, tick + 30000);
								sd->autobattle_data.roam_dest_mob_id = 0;
							}
						}
					}
					return;
				}
			}
		}
	}

	// All four passes failed — fully cornered. Abandon destination, clear
	// the visit buffer, blacklist the failed direction for 2 mins, and (if
	// this dest came from mob-seek) blacklist the mob bl_id for 30s.
	{
		int32 fq = autobattle_direction_quadrant(sd, dest_x, dest_y);
		sd->autobattle_data.failed_direction_until[fq] = tick + 30000;
	}
	sd->autobattle_data.roam_has_dest = false;
	sd->autobattle_data.roam_visit_head = 0;
	sd->autobattle_data.roam_visit_count = 0;
	if (sd->autobattle_data.roam_dest_mob_id > 0) {
		autobattle_blacklist_unreachable(sd,
			sd->autobattle_data.roam_dest_mob_id, tick + 30000);
		sd->autobattle_data.roam_dest_mob_id = 0;
	}
}

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

	// Skip the recently-unreachable target so the search doesn't immediately
	// re-pick the same wall-mob and trap us in a loop. Cooldown set when
	// unit_walktobl fails (5s) or 3-strikes fires (30s).
	if (autobattle_is_unreachable(sd, bl->id, gettick()))
		return 0;

	// Phase 22: Auto-Target whitelist filter
	if (bl->type == BL_MOB && sd->autobattle_data.target_mob_count > 0) {
		struct mob_data *md = (struct mob_data*)bl;
		bool found = false;
		for (int i = 0; i < sd->autobattle_data.target_mob_count; i++) {
			if (sd->autobattle_data.target_mob_ids[i] == md->mob_id) {
				found = true;
				break;
			}
		}
		if (!found)
			return 0; // Not in whitelist, skip
	}

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
 * Search for auto-attack target based on configured priority.
 *
 * Stickiness: if we already have a target and it's still valid (alive, enemy,
 * in range, in whitelist), keep it. Without this the search picks the
 * priority-best target every tick and "abandons" a low-HP mob the moment a
 * full-HP higher-priority one comes into range — finish your dinner first.
 */
struct block_list* autobattle_search_target(map_session_data *sd)
{
	if (!sd || !(sd->autobattle_data.mode & AUTOBATTLE_ATTACK))
		return nullptr;

	// Stickiness — keep the current target if we're already engaging it in
	// attack range (so we don't yank off a low-HP mob mid-swing for a fresh
	// one). If the current target is still WALKING-distance away (in detection
	// range but not attack range), fall through to a fresh search — that lets
	// PRIORITY_DISTANCE pick a closer mob we walked past en route to the far
	// one. "Finish your dinner" only applies once dinner is on the plate.
	int32 attack_range = 1;
	struct status_data *sstatus_for_range = status_get_status_data(*sd);
	if (sstatus_for_range)
		attack_range = sstatus_for_range->rhw.range;
	if (sd->autobattle_data.attack_skill_id > 0) {
		int32 skill_range = skill_get_range2((block_list*)sd,
			sd->autobattle_data.attack_skill_id,
			sd->autobattle_data.attack_skill_lv, true);
		if (skill_range > attack_range)
			attack_range = skill_range;
	}
	if (attack_range < 1) attack_range = 1;

	if (sd->autobattle_data.target_id > 0 &&
		!autobattle_is_unreachable(sd, sd->autobattle_data.target_id, gettick())) {
		struct block_list *cur = map_id2bl(sd->autobattle_data.target_id);
		if (cur && cur->prev != nullptr && autobattle_can_attack(sd, cur)) {
			int16 dist = distance_bl((block_list*)sd, cur);
			if (dist <= attack_range) {
				// Whitelist re-check: a player toggling target filter mid-fight
				// should NOT keep attacking a mob that's now disallowed.
				bool whitelist_ok = true;
				if (cur->type == BL_MOB && sd->autobattle_data.target_mob_count > 0) {
					struct mob_data *md = (struct mob_data*)cur;
					whitelist_ok = false;
					for (int i = 0; i < sd->autobattle_data.target_mob_count; i++) {
						if (sd->autobattle_data.target_mob_ids[i] == md->mob_id) {
							whitelist_ok = true;
							break;
						}
					}
				}
				if (whitelist_ok)
					return cur;
			}
		}
	}

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

	const t_tick support_follow_teleport_cooldown = 3000;

	// ===== NPC DIALOG GUARD =====
	// If the player is in an NPC dialog (e.g. the AutoBattleConfig menu they
	// opened to configure us), pc_useitem and unit_skilluse will all bail with
	// "any work in progress" errors and clutter the chat. Skip this tick
	// entirely so they can configure in peace.
	if (sd->npc_id != 0)
		goto autobattle_timer_reschedule;

	// ===== DEATH GUARD =====
	// Dead characters can't fight, support, loot, roam, or drink potions.
	// Without this, AUTOBATTLE_AUTOPOT happily chugs Red Potions on a corpse
	// because pc_useitem doesn't itself short-circuit on death.
	if (status_isdead(*sd))
		goto autobattle_timer_reschedule;

	// ===== TIME CAP CHECK =====
	if (sd->autobattle_data.daily_limit > 0) {
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
	} // end daily_limit > 0

	// ===== ITEM GATE CHECK (every ~10 seconds) =====
	// Use tick-based check: fires once every 20 timer intervals (20 * 500ms = 10s)
	if (sd->autobattle_data.start_tick > 0 &&
		(DIFF_TICK(tick, sd->autobattle_data.start_tick) / AUTOBATTLE_TIMER_INTERVAL) % 20 == 0) {
		if (pc_search_inventory(sd, AUTOBATTLE_ITEM_ID) < 0) {
			clif_displaymessage(sd->fd, "[Auto-Battle] Auto-Battle Pass not found. Auto-battle stopped.");
			autobattle_stop(sd);
			return 0;
		}
	}

	// ===== AUTO-SIT (Phase 24) — checked BEFORE attack/roam =====
	if (sd->autobattle_data.mode & AUTOBATTLE_AUTOSIT) {
		int32 hp_pct = (sd->battle_status.max_hp > 0) ?
			(sd->battle_status.hp * 100) / sd->battle_status.max_hp : 100;
		int32 sp_pct = (sd->battle_status.max_sp > 0) ?
			(sd->battle_status.sp * 100) / sd->battle_status.max_sp : 100;

		bool need_sit = false;
		if (sd->autobattle_data.autosit_hp_threshold > 0 && hp_pct < sd->autobattle_data.autosit_hp_threshold)
			need_sit = true;
		if (sd->autobattle_data.autosit_sp_threshold > 0 && sp_pct < sd->autobattle_data.autosit_sp_threshold)
			need_sit = true;

		if (need_sit && !pc_issit(sd)) {
			// Sit down to regen
			pc_setsit(sd);
			skill_sit(sd, true);
			clif_sitting(*(block_list*)sd);
			// Skip attack/roam — resting
			goto autobattle_timer_reschedule;
		}

		if (pc_issit(sd)) {
			// Check if recovered enough to stand
			bool can_stand = true;
			if (sd->autobattle_data.autosit_hp_threshold > 0 && hp_pct < sd->autobattle_data.autosit_hp_recover)
				can_stand = false;
			if (sd->autobattle_data.autosit_sp_threshold > 0 && sp_pct < sd->autobattle_data.autosit_sp_recover)
				can_stand = false;

			// Fight-back: if a mob is attacking us, stand up and fight even if not fully recovered
			if (!can_stand && (sd->autobattle_data.mode & AUTOBATTLE_ATTACK)) {
				// canmove_tick is extended when player takes damage (flinch / walk delay)
				// If it was set recently, we're being attacked — stand up and fight back
				if (sd->ud.canmove_tick > tick - 3000) {
					can_stand = true;
				}
			}

			if (can_stand) {
				if (pc_setstand(sd, false)) {
					skill_sit(sd, false);
					clif_standing(*(block_list*)sd);
				}
				// Recovered or fighting back — resume normal processing below
			} else {
				// Still resting — skip attack/roam
				goto autobattle_timer_reschedule;
			}
		}
	}

	// ===== AUTO-ATTACK =====
	if (sd->autobattle_data.mode & AUTOBATTLE_ATTACK) {

		// If a skill is currently casting, do NOT issue any new walk or
		// skill — we'd interrupt the cast. This was the Storm Gust bug:
		// cast bar visually filled but skill_castend_pos never ran because
		// our tick called unit_walktobl mid-cast (target wiggled out of
		// range) and cancelled the timer. Long-cast ground skills are
		// most affected. Just bail and let the engine finish the cast.
		// ALSO refresh last_combat_tick — actively casting IS engagement,
		// otherwise the 7s Fly Wing/Teleskill timer would fire mid-cast
		// (Storm Gust takes ~5s, plenty of time to elapse the threshold).
		if (sd->ud.skilltimer != INVALID_TIMER) {
			sd->autobattle_data.last_combat_tick = tick;
			goto autobattle_timer_reschedule;
		}

		struct block_list *target = autobattle_search_target(sd);

		if (target) {
			// Found a candidate enemy. We don't claim "in combat" yet —
			// last_combat_tick is updated only when we actually engage (target
			// in attack range, OR successful walk toward it). Otherwise an
			// unreachable wall-mob would refresh the timer every tick and
			// permanently block the Fly Wing 3s-no-combat trigger.
			sd->autobattle_data.target_id = target->id;

			// Phase 23: Determine effective range (skill range or weapon range)
			int32 effective_range;
			bool use_skill = false;
			if (sd->autobattle_data.attack_skill_id > 0) {
				int32 skill_range = skill_get_range2((block_list*)sd,
					sd->autobattle_data.attack_skill_id,
					sd->autobattle_data.attack_skill_lv, true);
				if (skill_range < 1) skill_range = 1;
				effective_range = skill_range;
				use_skill = true;
			} else {
				struct status_data *sstatus = status_get_status_data(*sd);
				effective_range = sstatus ? sstatus->rhw.range : 1;
			}

			if (!check_distance_bl((block_list*)sd, target, effective_range)) {
				// Target in detection range but out of attack/skill range - walk towards it.
				// If walktobl returns 0 the engine couldn't pathfind to the target (wall in
				// the way etc.). Blacklist that target for 5s so the next search doesn't
				// immediately re-pick the same wall-mob and trap us in a loop where
				// last_combat_tick never elapses → Fly Wing never fires.
				if (!unit_walktobl((block_list*)sd, target, effective_range, 1)) {
					sd->autobattle_data.target_id = -1;
					autobattle_blacklist_unreachable(sd, target->id, tick + 5000);
				} else {
					// Walking toward target counts as engagement — refresh combat timer.
					sd->autobattle_data.last_combat_tick = tick;
				}
			} else {
				// Target in attack range. Refresh the combat clock unconditionally
				// — being in range is engagement, even on ticks where canact is
				// still cooling between swings (slow aspd, after a long cast,
				// etc.). Without this, the bot could be auto-attacking a tough
				// mob with high aspd cooldown and Fly Wing/Teleskill could fire
				// on a tick that fell between swings.
				sd->autobattle_data.last_combat_tick = tick;

				if (DIFF_TICK(sd->ud.canact_tick, tick) <= 0) {
					// Phase 23: Try skill first, fallback to normal attack.
					// Ground-targeted skills (Storm Gust, Lord of Vermilion,
					// Magnum Break, Heaven's Drive…) use unit_skilluse_pos
					// with the target's cell. Single-target skills (Bash,
					// Bowling Bash, Pierce…) use unit_skilluse_id with the
					// target's bl_id. Calling the wrong one fails silently
					// and the bot just stares.
					bool skill_used = false;
					if (use_skill) {
						uint16 sk_id = sd->autobattle_data.attack_skill_id;
						uint8 sk_lv = sd->autobattle_data.attack_skill_lv;
						int32 sp_cost = skill_get_sp(sk_id, sk_lv);
						if ((int32)sd->battle_status.sp >= sp_cost &&
							skill_check_condition_castbegin(*sd, sk_id, sk_lv)) {
							bool is_ground = (skill_get_inf(sk_id) & INF_GROUND_SKILL) != 0;
							if (is_ground)
								unit_skilluse_pos((block_list*)sd, target->x, target->y, sk_id, sk_lv);
							else
								unit_skilluse_id((block_list*)sd, target->id, sk_id, sk_lv);
							skill_used = true;
						}
					}
					if (!skill_used) {
						unit_attack((block_list*)sd, target->id, 1); // Normal attack fallback
					}
				}
			}
		} else {
			// No valid target found
			sd->autobattle_data.target_id = -1;

			// Always walk-roam to keep exploring on foot. Each hop is A*-
			// validated so the bot covers ground continuously instead of
			// standing still while waiting for a Fly Wing tick.
			autobattle_roam_walk(sd, tick);

			// Teleport layered on top: when 7s have passed without combat AND
			// 7s since the last teleport. The combat clock is refreshed on
			// actual engagement (in-range hit OR successful walktobl), so the
			// bot won't teleport mid-fight. Two providers:
			//   TELESKILL — cast AL_TELEPORT (no item consumed; needs 10 SP).
			//               Works with costume-granted Teleport since pc_checkskill
			//               returns the skill level regardless of flag.
			//   FLYWING   — consume a Fly Wing from inventory.
			// Both can be toggled independently. If both are on, TELESKILL is
			// tried first (no item cost) and FLYWING is the fallback.
			// After teleport the roam destination + visit memory are stale,
			// so clear them — next tick picks a fresh dest from the new spot.
			bool can_teleskill = (sd->autobattle_data.mode & AUTOBATTLE_TELESKILL) != 0;
			bool can_flywing = (sd->autobattle_data.mode & AUTOBATTLE_FLYWING) != 0;

			if ((can_teleskill || can_flywing) &&
				DIFF_TICK(tick, sd->autobattle_data.last_combat_tick) >= 7000 &&
				DIFF_TICK(tick, sd->autobattle_data.last_flywing_tick) >= 7000) {

				bool teleported = false;

				// Try TELESKILL first.
				if (can_teleskill) {
					uint8 lv = pc_checkskill(sd, AL_TELEPORT);
					if (lv >= 1) {
						int32 sp_cost = skill_get_sp(AL_TELEPORT, 1);
						if ((int32)sd->battle_status.sp >= sp_cost) {
							// Setting state.autocast = 1 suppresses the
							// "Random / Save Point" picker UI — the engine
							// treats this as a programmatic cast and just
							// teleports randomly at Lv1.
							sd->state.autocast = 1;
							unit_skilluse_id((block_list*)sd, sd->id, AL_TELEPORT, 1);
							sd->state.autocast = 0;
							teleported = true;
						}
					}
				}

				// Fallback to FLYWING if teleskill didn't fire (no skill, low
				// SP, or just not enabled).
				if (!teleported && can_flywing) {
					int16 idx = pc_search_inventory(sd, 601); // 601 = Fly Wing
					if (idx >= 0) {
						pc_useitem(sd, idx);
						teleported = true;
					} else if (!can_teleskill) {
						clif_displaymessage(sd->fd, "[Auto-Battle] Out of Fly Wings! Switching to walk mode.");
						sd->autobattle_data.mode &= ~AUTOBATTLE_FLYWING;
					}
				}

				if (teleported) {
					sd->autobattle_data.last_flywing_tick = tick;
					sd->autobattle_data.roam_has_dest = false;
					sd->autobattle_data.roam_visit_head = 0;
					sd->autobattle_data.roam_visit_count = 0;
					sd->autobattle_data.roam_struggle_count = 0;
				}
			}
		}
	}

	// ===== SELF ITEM BUFFS =====
	if ((sd->autobattle_data.mode & AUTOBATTLE_SUPPORT) && sd->autobattle_data.support_item_count > 0) {
		if (DIFF_TICK(tick, sd->autobattle_data.last_item_buff_tick) >= 500) {
			sd->autobattle_data.last_item_buff_tick = tick;
			for (int i = 0; i < sd->autobattle_data.support_item_count; i++) {
				struct s_autosupport_item &item = sd->autobattle_data.support_items[i];
				if (item.item_id == 0 || item.status_id <= SC_NONE)
					continue;
				if (autobattle_has_status(sd, item.status_id))
					continue;
				if (DIFF_TICK(tick, item.last_use_tick) < 2000)
					continue;

				int16 idx = pc_search_inventory(sd, item.item_id);
				if (idx < 0)
					continue;

				pc_useitem(sd, idx);
				item.last_use_tick = tick;
				break;
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

				// Self-only skills (Two-Hand Quicken, Magnificat, Adrenaline Rush, etc.)
				// can never target a party member — the engine redirects to caster. If
				// the stored scope is PARTY/GUILD, the buff-active check would run
				// against an unbuffed party member and force a recast on us, draining
				// SP repeatedly. Force these to SELF semantics regardless of scope.
				bool self_only = (skill_get_inf(skill.skill_id) & INF_SELF_SKILL) != 0;

				// Find target based on scope
				struct map_session_data *target = nullptr;

			switch (self_only ? AUTOSUPPORT_SELF : skill.target_scope) {
				case AUTOSUPPORT_SELF:
					// For buff trigger type on self, check if buff is missing
					if (skill.trigger_type == 1) {
						enum sc_type sc = (skill.buff_id > 0) ? (enum sc_type)skill.buff_id : skill_get_sc(skill.skill_id);
						if (sc == SC_NONE)
							continue;
						if (sd->sc.hasSCE(sc))
							continue; // Buff still active, skip
					}
					target = sd;
					break;
					case AUTOSUPPORT_PARTY:
					case AUTOSUPPORT_GUILD: {
						// Phase 25: Party member targeting
						if (sd->status.party_id == 0) {
							target = sd; // No party, fall back to self
							break;
						}
						struct party_data *p = party_search(sd->status.party_id);
						if (!p) {
							target = sd;
							break;
						}

						struct map_session_data *best_target = nullptr;
						int32 lowest_hp_pct = 100;

						// Always check self first — a dead support is a useless support.
						// Self bypasses the target mode filter so the healer survives regardless of role.
						if (!status_isdead(*sd)) {
							int32 self_hp_pct = (sd->battle_status.max_hp > 0) ?
								(sd->battle_status.hp * 100) / sd->battle_status.max_hp : 100;
							if (skill.trigger_type == 0) {
								if (self_hp_pct < skill.hp_threshold) {
									lowest_hp_pct = self_hp_pct;
									best_target = sd;
								}
							} else if (skill.trigger_type == 1) {
								enum sc_type sc = (skill.buff_id > 0) ? (enum sc_type)skill.buff_id : skill_get_sc(skill.skill_id);
								if (sc != SC_NONE && !sd->sc.hasSCE(sc))
									best_target = sd; // Tentative — party members checked next
							}
						}

						for (int pi = 0; pi < MAX_PARTY; pi++) {
							struct map_session_data *psd = p->data[pi].sd;
							if (!psd || psd == sd || !psd->prev || psd->m != sd->m)
								continue; // Skip self (already checked above), offline, not on map, different map
							if (status_isdead(*psd))
								continue;

							// Target mode filter
							if (sd->autobattle_data.support_target_mode == 1) {
								// Leader only
								if (!party_isleader(psd)) continue;
							} else if (sd->autobattle_data.support_target_mode == 2) {
								// Specific member by name
								if (strcmp(psd->status.name, sd->autobattle_data.support_target_name) != 0) continue;
							} else if (sd->autobattle_data.follow_target_id <= 0 ||
								psd->id != sd->autobattle_data.follow_target_id) {
								continue; // Auto-lock mode supports the current follow target only.
							}

							int32 psd_hp_pct = (psd->battle_status.max_hp > 0) ?
								(psd->battle_status.hp * 100) / psd->battle_status.max_hp : 100;

							if (skill.trigger_type == 0) {
								// HP-based: lowest HP% wins (self already seeded above, party competes)
								if (psd_hp_pct < skill.hp_threshold && psd_hp_pct < lowest_hp_pct) {
									lowest_hp_pct = psd_hp_pct;
									best_target = psd;
								}
							} else if (skill.trigger_type == 1) {
								// Buff-based: first party member missing the buff takes priority over self
								enum sc_type sc = (skill.buff_id > 0) ? (enum sc_type)skill.buff_id : skill_get_sc(skill.skill_id);
								if (sc != SC_NONE && !psd->sc.hasSCE(sc)) {
									best_target = psd;
									break;
								}
							}
						}
						target = best_target;
						break;
					}
					default:
						target = sd;
				}

				if (!target)
					continue;

				// Phase 25: For party targets (not self), check range and walk closer if needed
				if (target != sd) {
					// If target is on different map, teleport there first
					if (target->m != sd->m) {
						struct map_data *target_mapdata = map_getmapdata(target->m);
						if (target_mapdata &&
							DIFF_TICK(tick, sd->autobattle_data.last_follow_tick) >= support_follow_teleport_cooldown) {
							sd->autobattle_data.follow_target_id = target->id;
							sd->autobattle_data.last_follow_tick = tick;
							pc_setpos(sd, target_mapdata->index, target->x, target->y, CLR_TELEPORT);
						}
						continue;
					}
					int32 skill_range = skill_get_range2((block_list*)sd,
						skill.skill_id, skill.skill_lv, true);
					if (skill_range < 1) skill_range = 1;
					if (!check_distance_bl((block_list*)sd, (block_list*)target, skill_range)) {
						sd->autobattle_data.follow_target_id = target->id;
						int16 dist = distance_bl((block_list*)sd, (block_list*)target);
						if (dist > 20 &&
							DIFF_TICK(tick, sd->autobattle_data.last_follow_tick) >= support_follow_teleport_cooldown) {
							// Teleport next to target for fast catch-up
							sd->autobattle_data.last_follow_tick = tick;
							pc_setpos(sd, map_getmapdata(target->m)->index, target->x, target->y, CLR_TELEPORT);
						} else {
							// Walk toward target
							unit_walktobl((block_list*)sd, (block_list*)target, skill_range, 1);
						}
						continue; // Try again next tick after moving closer
					}
				}

			// Check HP threshold (skip for buff trigger type — they trigger on buff expiry, not HP)
			if (skill.trigger_type != 1) {
				int hp_percent = (target->battle_status.hp > 0) ?
					(target->battle_status.hp * 100) / target->battle_status.max_hp : 0;

				if (hp_percent >= skill.hp_threshold)
					continue; // HP not below threshold
			}

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

	// ===== AUTO-SUPPORT FOLLOW (Phase 25) =====
	// When support mode is ON with party scope and no attack target, follow the first valid party target
	if ((sd->autobattle_data.mode & AUTOBATTLE_SUPPORT) &&
		!(sd->autobattle_data.mode & AUTOBATTLE_ATTACK) &&
		sd->autobattle_data.target_id == -1 &&
		sd->status.party_id > 0)
	{
		struct party_data *p = party_search(sd->status.party_id);
		if (p) {
			struct map_session_data *follow_sd = nullptr;

			// Keep following the same valid member instead of bouncing between party members.
			if (sd->autobattle_data.follow_target_id > 0) {
				struct map_session_data *locked_sd = map_id2sd(sd->autobattle_data.follow_target_id);
				if (locked_sd && locked_sd->prev && locked_sd != sd &&
					locked_sd->status.party_id == sd->status.party_id &&
					!status_isdead(*locked_sd)) {
					if (sd->autobattle_data.support_target_mode == 1) {
						if (party_isleader(locked_sd))
							follow_sd = locked_sd;
					} else if (sd->autobattle_data.support_target_mode == 2) {
						if (strcmp(locked_sd->status.name, sd->autobattle_data.support_target_name) == 0)
							follow_sd = locked_sd;
					} else if (locked_sd->m == sd->m) {
						follow_sd = locked_sd;
					}
				}
			}

			if (!follow_sd) {
				int16 best_dist = INT16_MAX;
				for (int pi = 0; pi < MAX_PARTY; pi++) {
					struct map_session_data *psd = p->data[pi].sd;
					if (!psd || !psd->prev || psd == sd)
						continue;
					if (status_isdead(*psd))
						continue;

					if (sd->autobattle_data.support_target_mode == 1) {
						if (!party_isleader(psd)) continue;
					} else if (sd->autobattle_data.support_target_mode == 2) {
						if (strcmp(psd->status.name, sd->autobattle_data.support_target_name) != 0) continue;
					} else if (psd->m != sd->m) {
						continue; // Auto-lock mode has no stable cross-map target.
					}

					if (sd->autobattle_data.support_target_mode == 0) {
						int16 dist = distance_bl((block_list*)sd, (block_list*)psd);
						if (dist < best_dist) {
							best_dist = dist;
							follow_sd = psd;
						}
						continue;
					}

					follow_sd = psd;
					break;
				}
			}

			if (follow_sd) {
				sd->autobattle_data.follow_target_id = follow_sd->id;

				// Cross-map follow: teleport to target's map if on different map
				if (follow_sd->m != sd->m) {
					struct map_data *target_mapdata = map_getmapdata(follow_sd->m);
					if (target_mapdata &&
						DIFF_TICK(tick, sd->autobattle_data.last_follow_tick) >= support_follow_teleport_cooldown) {
						sd->autobattle_data.last_follow_tick = tick;
						pc_setpos(sd, target_mapdata->index, follow_sd->x, follow_sd->y, CLR_TELEPORT);
					}
				}
				// Same map: teleport if very far (>15 cells), walk if moderately far (>3 cells)
				else {
					int16 dist = distance_bl((block_list*)sd, (block_list*)follow_sd);
					if (dist > 20 &&
						DIFF_TICK(tick, sd->autobattle_data.last_follow_tick) >= support_follow_teleport_cooldown) {
						// Long-distance catch-up: teleport next to target
						sd->autobattle_data.last_follow_tick = tick;
						pc_setpos(sd, map_getmapdata(follow_sd->m)->index, follow_sd->x, follow_sd->y, CLR_TELEPORT);
					} else if (dist > 3) {
						// Walk toward follow target
						unit_walktobl((block_list*)sd, (block_list*)follow_sd, 2, 1);
					} else if (dist > 2 && sd->ud.walktimer == INVALID_TIMER) {
						// Re-issue walk if stopped but still not close enough (target moved)
						unit_walktobl((block_list*)sd, (block_list*)follow_sd, 2, 1);
					}
				}
			}
		}
	}

	// ===== AUTO-LOOT =====
	// Floor-pickup with animation: items drop normally, the bot walks over
	// to them and pc_takeitem fires the visible pickup packet. We chose this
	// over engine state.autoloot deliberately — the visual matters more than
	// the slightly higher miss rate, and in practice missed items are rare
	// (the bot stays close to where mobs die).
	if (sd->autobattle_data.mode & AUTOBATTLE_LOOT) {
		g_loot_searcher = sd;
		map_foreachinrange(autobattle_loot_callback, (block_list*)sd, sd->autobattle_data.loot_range, BL_ITEM);
		g_loot_searcher = nullptr;
	}

	// ===== SKILL ROTATION =====
	if (sd->autobattle_data.mode & AUTOBATTLE_SKILLROTATION) {
		// Skill rotation is handled during attack animations
		// This is just a placeholder for future expansion
	}

	// ===== AUTO-POT =====
	if (sd->autobattle_data.mode & AUTOBATTLE_AUTOPOT) {
		if (DIFF_TICK(tick, sd->autobattle_data.last_pot_tick) >= 500) { // Check every 500ms
			sd->autobattle_data.last_pot_tick = tick;
			bool out_of_pots = false;

			// HP potion check
			if (sd->autobattle_data.autopot_hp_id > 0) {
				int32 hp_pct = (sd->battle_status.max_hp > 0) ?
					(sd->battle_status.hp * 100) / sd->battle_status.max_hp : 100;
				if (hp_pct < sd->autobattle_data.autopot_hp_threshold) {
					int16 idx = pc_search_inventory(sd, sd->autobattle_data.autopot_hp_id);
					if (idx >= 0) {
						pc_useitem(sd, idx);
					} else {
						out_of_pots = true;
					}
				}
			}

			// SP potion check
			if (sd->autobattle_data.autopot_sp_id > 0) {
				int32 sp_pct = (sd->battle_status.max_sp > 0) ?
					(sd->battle_status.sp * 100) / sd->battle_status.max_sp : 100;
				if (sp_pct < sd->autobattle_data.autopot_sp_threshold) {
					int16 idx = pc_search_inventory(sd, sd->autobattle_data.autopot_sp_id);
					if (idx >= 0) {
						pc_useitem(sd, idx);
					} else {
						out_of_pots = true;
					}
				}
			}

			// Go home if out of potions
			if (out_of_pots && sd->autobattle_data.gohome_no_pots) {
				clif_displaymessage(sd->fd, "[Auto-Battle] Out of potions! Returning to save point...");
				// Try butterfly wing first
				int16 bw_idx = pc_search_inventory(sd, 602); // 602 = Butterfly Wing
				if (bw_idx >= 0) {
					pc_useitem(sd, bw_idx);
				} else {
					// No butterfly wing — teleport directly
					pc_setpos_savepoint(*sd, CLR_TELEPORT);
				}
				autobattle_stop(sd);
				return 0;
			}
		}
	}

	// Reschedule timer
autobattle_timer_reschedule:
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
void autobattle_toggle_mode(map_session_data *sd, uint16 mode, bool on)
{
	if (!sd)
		return;

	if (on) {
		sd->autobattle_data.mode |= mode;
	} else {
		sd->autobattle_data.mode &= ~mode;
	}

	// Save to DB whenever a *persistent* mode bit changes so the user's
	// config survives crashes between login and logout. ATTACK and ROAM are
	// session toggles — saving on those would cause spurious writes every
	// time autobattle_start fires.
	if (mode & ~(AUTOBATTLE_ATTACK | AUTOBATTLE_ROAM))
		autobattle_save_config_db(sd);
}

/**
 * Set auto-attack range
 */
void autobattle_set_range(map_session_data *sd, uint8 range)
{
	if (!sd)
		return;

	// Detection range is always the maximum supported by this feature.
	sd->autobattle_data.range = 15;
	sd->autobattle_data.loot_range = 15;
}

/**
 * Add auto-support skill
 */
void autobattle_add_support_skill(map_session_data *sd, uint16 skill_id,
	uint8 skill_lv, uint8 hp_threshold, uint8 target_scope, uint8 trigger_type)
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
			sd->autobattle_data.support_skills[i].trigger_type = trigger_type;
			if (trigger_type == 1)
				sd->autobattle_data.support_skills[i].buff_id = (uint16)skill_get_sc(skill_id);
			autobattle_save_config_db(sd);
			return;
		}
	}

	// Add new skill
	struct s_autosupport_skill &skill = sd->autobattle_data.support_skills[sd->autobattle_data.support_skill_count];
	skill.skill_id = skill_id;
	skill.skill_lv = skill_lv;
	skill.hp_threshold = hp_threshold;
	skill.target_scope = target_scope;
	skill.trigger_type = trigger_type;
	skill.last_cast_time = 0;
	if (trigger_type == 1)
		skill.buff_id = (uint16)skill_get_sc(skill_id);
	sd->autobattle_data.support_skill_count++;
	autobattle_save_config_db(sd);
}

/**
 * Remove a specific support skill by skill_id
 * @return true if found and removed, false otherwise
 */
bool autobattle_remove_support_skill(map_session_data *sd, uint16 skill_id)
{
	if (!sd)
		return false;

	for (int i = 0; i < sd->autobattle_data.support_skill_count; i++) {
		if (sd->autobattle_data.support_skills[i].skill_id == skill_id) {
			// Shift remaining entries down
			for (int j = i; j < sd->autobattle_data.support_skill_count - 1; j++)
				sd->autobattle_data.support_skills[j] = sd->autobattle_data.support_skills[j + 1];
			sd->autobattle_data.support_skill_count--;
			// Zero out the now-unused last slot
			memset(&sd->autobattle_data.support_skills[sd->autobattle_data.support_skill_count], 0, sizeof(struct s_autosupport_skill));
			autobattle_save_config_db(sd);
			return true;
		}
	}
	return false;
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
	autobattle_save_config_db(sd);
}

void autobattle_add_support_item(map_session_data *sd, t_itemid item_id, int16 status_id)
{
	if (!sd || item_id == 0 || status_id <= SC_NONE)
		return;

	for (int i = 0; i < sd->autobattle_data.support_item_count; i++) {
		if (sd->autobattle_data.support_items[i].item_id == item_id) {
			sd->autobattle_data.support_items[i].status_id = status_id;
			return;
		}
	}

	if (sd->autobattle_data.support_item_count >= AUTOBATTLE_MAX_ITEM_BUFFS)
		return;

	struct s_autosupport_item &item = sd->autobattle_data.support_items[sd->autobattle_data.support_item_count++];
	item.item_id = item_id;
	item.status_id = status_id;
	item.last_use_tick = 0;
}

void autobattle_clear_support_items(map_session_data *sd)
{
	if (!sd)
		return;

	memset(sd->autobattle_data.support_items, 0, sizeof(sd->autobattle_data.support_items));
	sd->autobattle_data.support_item_count = 0;
	autobattle_save_config_db(sd);
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

	// Check item gate: must have Auto-Battle Pass in inventory
	if (pc_search_inventory(sd, AUTOBATTLE_ITEM_ID) < 0) {
		clif_displaymessage(sd->fd, "[Auto-Battle] You need an Auto-Battle Pass to use this feature.");
		sd->autobattle_data.mode = AUTOBATTLE_OFF;
		return;
	}

	// Check time cap (skip if unlimited: daily_limit == 0)
	if (sd->autobattle_data.daily_limit > 0 && sd->autobattle_data.seconds_remaining <= 0) {
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
	sd->autobattle_data.range = 15;
	sd->autobattle_data.loot_range = 15;
	sd->autobattle_data.last_combat_tick = sd->autobattle_data.start_tick;
	if (sd->autobattle_data.mode & AUTOBATTLE_ATTACK)
		autobattle_toggle_mode(sd, AUTOBATTLE_ROAM, true);

	// Debug: tell the player which features are active this session so they
	// can verify their persistent config actually took effect after login.
	{
		char dbg[256];
		snprintf(dbg, sizeof(dbg),
			"[Auto-Battle] Active features: %s%s%s%s%s%s%s",
			(sd->autobattle_data.mode & AUTOBATTLE_ATTACK)   ? "Attack " : "",
			(sd->autobattle_data.mode & AUTOBATTLE_SUPPORT)  ? "Support " : "",
			(sd->autobattle_data.mode & AUTOBATTLE_LOOT)     ? "Loot " : "",
			(sd->autobattle_data.mode & AUTOBATTLE_AUTOPOT)  ? "Pot " : "",
			(sd->autobattle_data.mode & AUTOBATTLE_AUTOSIT)  ? "Sit " : "",
			(sd->autobattle_data.mode & AUTOBATTLE_FLYWING)  ? "FlyWing " : "",
			(sd->autobattle_data.mode & AUTOBATTLE_TELESKILL)? "Teleskill " : "");
		clif_displaymessage(sd->fd, dbg);
		if (sd->autobattle_data.mode & AUTOBATTLE_AUTOPOT) {
			snprintf(dbg, sizeof(dbg),
				"[Auto-Battle] Pot config: HP item %u @%d%%, SP item %u @%d%%, GoHome %s",
				sd->autobattle_data.autopot_hp_id, sd->autobattle_data.autopot_hp_threshold,
				sd->autobattle_data.autopot_sp_id, sd->autobattle_data.autopot_sp_threshold,
				sd->autobattle_data.gohome_no_pots ? "ON" : "OFF");
			clif_displaymessage(sd->fd, dbg);
		}
		if (sd->autobattle_data.mode & AUTOBATTLE_AUTOSIT) {
			snprintf(dbg, sizeof(dbg),
				"[Auto-Battle] Sit thresholds: HP<%d%% SP<%d%%",
				sd->autobattle_data.autosit_hp_threshold,
				sd->autobattle_data.autosit_sp_threshold);
			clif_displaymessage(sd->fd, dbg);
		}
		if (sd->autobattle_data.mode & AUTOBATTLE_SUPPORT) {
			snprintf(dbg, sizeof(dbg),
				"[Auto-Battle] Support: %d skills, %d items configured",
				sd->autobattle_data.support_skill_count,
				sd->autobattle_data.support_item_count);
			clif_displaymessage(sd->fd, dbg);
		}
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

	// Persist daily usage AND full config to DB before stopping. Save the
	// config BEFORE clearing mode so the saved bitmask reflects what the
	// player had configured (FLYWING, LOOT, etc.) — those flags get restored
	// on next login. The ATTACK bit is stripped on load (re-enabled per
	// session); SUPPORT persists so configured buffs keep firing.
	autobattle_save_time_db(sd);
	autobattle_save_config_db(sd);

	// Clear only the session-toggle bits in memory. Persistent feature bits
	// (LOOT, AUTOPOT, AUTOSIT, FLYWING, TELESKILL, SUPPORT, SKILLROTATION)
	// stay set so re-enabling Auto-Attack within the same session picks
	// everything back up — without this, autobattle_stop would zero the
	// whole mode bitmask and the next @autoattack on would only set ATTACK,
	// silently losing every other feature the user configured this session.
	sd->autobattle_data.mode &= ~(AUTOBATTLE_ATTACK | AUTOBATTLE_ROAM);
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
	sd->autobattle_data.range = 15;
	// Default: nearest first. The "skipped a closer mob" reports came from the
	// old PRIORITY_DAMAGE_DISTANCE default which sent the bot toward whichever
	// mob had the highest base ATK in detection range, ignoring distance unless
	// damage values tied. Distance-first matches what players expect: the bot
	// fights what's right next to it.
	sd->autobattle_data.target_priority = PRIORITY_DISTANCE;
	sd->autobattle_data.target_id = -1;
	sd->autobattle_data.attack_timer = INVALID_TIMER;
	sd->autobattle_data.support_skill_count = 0;
	sd->autobattle_data.support_item_count = 0;
	memset(sd->autobattle_data.support_items, 0, sizeof(sd->autobattle_data.support_items));
	sd->autobattle_data.current_rotation_slot = 0;
	sd->autobattle_data.loot_range = 15;
	sd->autobattle_data.loot_rarity_filter = 0;
	sd->autobattle_data.autopot_hp_id = 0;
	sd->autobattle_data.autopot_hp_threshold = 50;
	sd->autobattle_data.autopot_sp_id = 0;
	sd->autobattle_data.autopot_sp_threshold = 30;
	sd->autobattle_data.gohome_no_pots = false;
	sd->autobattle_data.last_pot_tick = 0;
	sd->autobattle_data.roam_dest_x = 0;
	sd->autobattle_data.roam_dest_y = 0;
	sd->autobattle_data.roam_has_dest = false;
	sd->autobattle_data.roam_last_x = 0;
	sd->autobattle_data.roam_last_y = 0;
	sd->autobattle_data.roam_visit_head = 0;
	sd->autobattle_data.roam_visit_count = 0;
	sd->autobattle_data.roam_snapshot_x = 0;
	sd->autobattle_data.roam_snapshot_y = 0;
	memset(sd->autobattle_data.roam_quadrant_tick, 0, sizeof(sd->autobattle_data.roam_quadrant_tick));
	sd->autobattle_data.unreachable_target_id = 0;
	sd->autobattle_data.unreachable_target_until = 0;
	memset(sd->autobattle_data.unreachable_ids, 0, sizeof(sd->autobattle_data.unreachable_ids));
	memset(sd->autobattle_data.unreachable_until, 0, sizeof(sd->autobattle_data.unreachable_until));
	sd->autobattle_data.unreachable_head = 0;
	sd->autobattle_data.roam_struggle_count = 0;
	sd->autobattle_data.roam_dest_mob_id = 0;
	memset(sd->autobattle_data.failed_direction_until, 0, sizeof(sd->autobattle_data.failed_direction_until));
	sd->autobattle_data.last_flywing_tick = 0;
	sd->autobattle_data.last_support_tick = 0;
	sd->autobattle_data.last_item_buff_tick = 0;
	sd->autobattle_data.last_loot_tick = 0;
	sd->autobattle_data.last_roam_tick = 0;
	sd->autobattle_data.last_combat_tick = 0;
	sd->autobattle_data.start_tick = 0;
	sd->autobattle_data.daily_seconds_used = 0;
	sd->autobattle_data.bonus_seconds = 0;
	sd->autobattle_data.daily_limit = battle_config.autobattle_default_seconds; // fallback
	sd->autobattle_data.seconds_remaining = battle_config.autobattle_default_seconds;
	sd->autobattle_data.time_deduct_accum = 0;
	sd->autobattle_data.exp_penalty_base = battle_config.autobattle_exp_penalty_base;
	sd->autobattle_data.exp_penalty_job = battle_config.autobattle_exp_penalty_job;

	// Phase 24: Auto-Sit defaults
	sd->autobattle_data.autosit_hp_threshold = 0;
	sd->autobattle_data.autosit_sp_threshold = 0;
	sd->autobattle_data.autosit_hp_recover = 0;
	sd->autobattle_data.autosit_sp_recover = 0;

	// Phase 22: Auto-Target defaults
	memset(sd->autobattle_data.target_mob_ids, 0, sizeof(sd->autobattle_data.target_mob_ids));
	sd->autobattle_data.target_mob_count = 0;

	// Phase 23: Auto-Skill defaults
	sd->autobattle_data.attack_skill_id = 0;
	sd->autobattle_data.attack_skill_lv = 0;

	// Phase 25: Auto-Support Enhancement defaults
	sd->autobattle_data.support_target_mode = 0;
	memset(sd->autobattle_data.support_target_name, 0, sizeof(sd->autobattle_data.support_target_name));
	sd->autobattle_data.follow_target_id = -1;
	sd->autobattle_data.last_follow_tick = 0;

	// Load daily time data from database (overrides defaults above)
	autobattle_load_time_db(sd);

	// Load persisted full config from DB (overrides defaults). Mode bits
	// AUTOBATTLE_ATTACK and AUTOBATTLE_SUPPORT are stripped inside the loader
	// so the player explicitly enables those each session.
	autobattle_load_config_db(sd);

	// Load from config if provided
	if (config) {
		sd->autobattle_data.mode = config->mode;
		sd->autobattle_data.range = 15;
		sd->autobattle_data.target_priority = config->target_priority;
		sd->autobattle_data.loot_range = 15;
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
	int32 exp_penalty_base = battle_config.autobattle_exp_penalty_base; // fallback
	int32 exp_penalty_job = battle_config.autobattle_exp_penalty_job; // fallback

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
				else if (strcmp(name, "exp_penalty_base") == 0)
					exp_penalty_base = atoi(val);
				else if (strcmp(name, "exp_penalty_job") == 0)
					exp_penalty_job = atoi(val);
			}
		}
		Sql_FreeResult(mmysql_handle);
	} else {
		Sql_ShowDebug(mmysql_handle);
	}

	// Apply EXP penalty (clamped 0-100)
	sd->autobattle_data.exp_penalty_base = (exp_penalty_base < 0) ? 0 : (exp_penalty_base > 100) ? 100 : exp_penalty_base;
	sd->autobattle_data.exp_penalty_job = (exp_penalty_job < 0) ? 0 : (exp_penalty_job > 100) ? 100 : exp_penalty_job;

	// Determine this player's daily limit
	int32 daily_limit = normal_limit;
#ifdef VIP_ENABLE
	if (pc_isvip(sd))
		daily_limit = vip_limit;
#endif
	sd->autobattle_data.daily_limit = daily_limit;

	// If unlimited (daily_limit == 0), skip per-character time tracking
	if (daily_limit == 0) {
		sd->autobattle_data.seconds_remaining = 0; // unused
		sd->autobattle_data.daily_seconds_used = 0;
		return;
	}

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

/**
 * Build a comma-separated string from an integer array. Caller-owned buffer.
 */
static void autobattle_build_csv(char *buf, size_t bufsize, const uint16 *arr, uint8 count)
{
	buf[0] = '\0';
	size_t off = 0;
	for (uint8 i = 0; i < count && off < bufsize - 1; i++) {
		int32 written = snprintf(buf + off, bufsize - off, "%s%u", (i == 0) ? "" : ",", arr[i]);
		if (written <= 0)
			break;
		off += (size_t)written;
	}
}

static void autobattle_build_csv_u8(char *buf, size_t bufsize, const uint8 *arr, uint8 count)
{
	buf[0] = '\0';
	size_t off = 0;
	for (uint8 i = 0; i < count && off < bufsize - 1; i++) {
		int32 written = snprintf(buf + off, bufsize - off, "%s%u", (i == 0) ? "" : ",", arr[i]);
		if (written <= 0)
			break;
		off += (size_t)written;
	}
}

static void autobattle_build_csv_u32(char *buf, size_t bufsize, const uint32 *arr, uint8 count)
{
	buf[0] = '\0';
	size_t off = 0;
	for (uint8 i = 0; i < count && off < bufsize - 1; i++) {
		int32 written = snprintf(buf + off, bufsize - off, "%s%u", (i == 0) ? "" : ",", arr[i]);
		if (written <= 0)
			break;
		off += (size_t)written;
	}
}

/**
 * Parse a comma-separated string of unsigned ints into an array. Returns the
 * number of values parsed. Stops at `max`.
 */
static uint8 autobattle_parse_csv(const char *str, uint16 *out, uint8 max)
{
	if (!str || !*str || max == 0)
		return 0;
	uint8 n = 0;
	const char *p = str;
	while (*p && n < max) {
		out[n++] = (uint16)strtoul(p, nullptr, 10);
		const char *next = strchr(p, ',');
		if (!next) break;
		p = next + 1;
	}
	return n;
}

static uint8 autobattle_parse_csv_u8(const char *str, uint8 *out, uint8 max)
{
	if (!str || !*str || max == 0)
		return 0;
	uint8 n = 0;
	const char *p = str;
	while (*p && n < max) {
		out[n++] = (uint8)strtoul(p, nullptr, 10);
		const char *next = strchr(p, ',');
		if (!next) break;
		p = next + 1;
	}
	return n;
}

static uint8 autobattle_parse_csv_u32(const char *str, uint32 *out, uint8 max)
{
	if (!str || !*str || max == 0)
		return 0;
	uint8 n = 0;
	const char *p = str;
	while (*p && n < max) {
		out[n++] = (uint32)strtoul(p, nullptr, 10);
		const char *next = strchr(p, ',');
		if (!next) break;
		p = next + 1;
	}
	return n;
}

/**
 * Save full auto-battle config to DB. UPSERTs the row so first-time saves
 * create it. Called from autobattle_stop on logout/disable.
 */
void autobattle_save_config_db(map_session_data *sd)
{
	if (!sd)
		return;

	int32 char_id = sd->status.char_id;

	// Build CSVs for support skills (up to 10 entries each).
	char skill_ids[256], skill_lvs[256], skill_thresh[256], skill_scopes[256], skill_triggers[256];
	{
		uint16 ids[10] = {0}; uint8 lvs[10] = {0}, thresh[10] = {0}, scopes[10] = {0}, triggers[10] = {0};
		for (uint8 i = 0; i < sd->autobattle_data.support_skill_count && i < 10; i++) {
			ids[i]      = sd->autobattle_data.support_skills[i].skill_id;
			lvs[i]      = sd->autobattle_data.support_skills[i].skill_lv;
			thresh[i]   = sd->autobattle_data.support_skills[i].hp_threshold;
			scopes[i]   = sd->autobattle_data.support_skills[i].target_scope;
			triggers[i] = sd->autobattle_data.support_skills[i].trigger_type;
		}
		autobattle_build_csv(skill_ids,    sizeof(skill_ids),    ids,    sd->autobattle_data.support_skill_count);
		autobattle_build_csv_u8(skill_lvs,    sizeof(skill_lvs),    lvs,    sd->autobattle_data.support_skill_count);
		autobattle_build_csv_u8(skill_thresh, sizeof(skill_thresh), thresh, sd->autobattle_data.support_skill_count);
		autobattle_build_csv_u8(skill_scopes, sizeof(skill_scopes), scopes, sd->autobattle_data.support_skill_count);
		autobattle_build_csv_u8(skill_triggers, sizeof(skill_triggers), triggers, sd->autobattle_data.support_skill_count);
	}

	// Build CSVs for support items (up to AUTOBATTLE_MAX_ITEM_BUFFS).
	char item_ids[256], item_status[256];
	{
		uint32 ids[AUTOBATTLE_MAX_ITEM_BUFFS] = {0};
		uint16 status[AUTOBATTLE_MAX_ITEM_BUFFS] = {0};
		for (uint8 i = 0; i < sd->autobattle_data.support_item_count && i < AUTOBATTLE_MAX_ITEM_BUFFS; i++) {
			ids[i]    = (uint32)sd->autobattle_data.support_items[i].item_id;
			status[i] = (uint16)sd->autobattle_data.support_items[i].status_id;
		}
		autobattle_build_csv_u32(item_ids,    sizeof(item_ids),    ids,    sd->autobattle_data.support_item_count);
		autobattle_build_csv(item_status, sizeof(item_status), status, sd->autobattle_data.support_item_count);
	}

	// Build CSV for target mob whitelist (kept for completeness; user prefers
	// not to persist the whitelist itself, but we save it anyway — load logic
	// resets it to empty per the user's "default all" requirement).
	char mob_csv[256];
	autobattle_build_csv(mob_csv, sizeof(mob_csv), sd->autobattle_data.target_mob_ids,
		sd->autobattle_data.target_mob_count);

	// Debug: chat-output what we're saving so we can spot mismatches between
	// what the user thinks is saved vs what actually hits the DB. Remove once
	// the persistence is verified working.
	{
		char dbg[256];
		snprintf(dbg, sizeof(dbg),
			"[Save] mode=0x%X loot=%d pot=%d sit=%d fly=%d tele=%d sup=%d skills=%d items=%d hp_pot=%u sp_pot=%u sit_hp=%u",
			sd->autobattle_data.mode,
			(sd->autobattle_data.mode & AUTOBATTLE_LOOT) ? 1 : 0,
			(sd->autobattle_data.mode & AUTOBATTLE_AUTOPOT) ? 1 : 0,
			(sd->autobattle_data.mode & AUTOBATTLE_AUTOSIT) ? 1 : 0,
			(sd->autobattle_data.mode & AUTOBATTLE_FLYWING) ? 1 : 0,
			(sd->autobattle_data.mode & AUTOBATTLE_TELESKILL) ? 1 : 0,
			(sd->autobattle_data.mode & AUTOBATTLE_SUPPORT) ? 1 : 0,
			sd->autobattle_data.support_skill_count,
			sd->autobattle_data.support_item_count,
			sd->autobattle_data.autopot_hp_id,
			sd->autobattle_data.autopot_sp_id,
			sd->autobattle_data.autosit_hp_threshold);
		clif_displaymessage(sd->fd, dbg);
	}

	// UPSERT the full row. The daily-time fields (daily_seconds_used, etc.)
	// are managed by autobattle_save_time_db — leave them alone here.
	if (SQL_ERROR == Sql_Query(mmysql_handle,
		"INSERT INTO `char_autobattle_config` (`char_id`, `mode`, `range`, `target_priority`, "
		"`support_skill_count`, `support_skill_ids`, `support_skill_lvs`, `support_skill_thresholds`, "
		"`support_skill_scopes`, `support_skill_triggers`, `support_item_count`, `support_item_ids`, "
		"`support_item_status_ids`, `loot_range`, `loot_rarity_filter`, `target_mob_ids`, "
		"`attack_skill_id`, `attack_skill_lv`, `autosit_hp_threshold`, `autosit_sp_threshold`, "
		"`support_target_mode`, `support_target_name`, `autopot_hp_id`, `autopot_hp_threshold`, "
		"`autopot_sp_id`, `autopot_sp_threshold`, `gohome_no_pots`) "
		"VALUES (%d, %u, %u, %u, %u, '%s', '%s', '%s', '%s', '%s', %u, '%s', '%s', %u, %u, '%s', "
		"%u, %u, %u, %u, %u, '%s', %u, %u, %u, %u, %u) "
		"ON DUPLICATE KEY UPDATE "
		"`mode`=VALUES(`mode`), `range`=VALUES(`range`), `target_priority`=VALUES(`target_priority`), "
		"`support_skill_count`=VALUES(`support_skill_count`), `support_skill_ids`=VALUES(`support_skill_ids`), "
		"`support_skill_lvs`=VALUES(`support_skill_lvs`), `support_skill_thresholds`=VALUES(`support_skill_thresholds`), "
		"`support_skill_scopes`=VALUES(`support_skill_scopes`), `support_skill_triggers`=VALUES(`support_skill_triggers`), "
		"`support_item_count`=VALUES(`support_item_count`), `support_item_ids`=VALUES(`support_item_ids`), "
		"`support_item_status_ids`=VALUES(`support_item_status_ids`), "
		"`loot_range`=VALUES(`loot_range`), `loot_rarity_filter`=VALUES(`loot_rarity_filter`), "
		"`target_mob_ids`=VALUES(`target_mob_ids`), `attack_skill_id`=VALUES(`attack_skill_id`), "
		"`attack_skill_lv`=VALUES(`attack_skill_lv`), `autosit_hp_threshold`=VALUES(`autosit_hp_threshold`), "
		"`autosit_sp_threshold`=VALUES(`autosit_sp_threshold`), `support_target_mode`=VALUES(`support_target_mode`), "
		"`support_target_name`=VALUES(`support_target_name`), `autopot_hp_id`=VALUES(`autopot_hp_id`), "
		"`autopot_hp_threshold`=VALUES(`autopot_hp_threshold`), `autopot_sp_id`=VALUES(`autopot_sp_id`), "
		"`autopot_sp_threshold`=VALUES(`autopot_sp_threshold`), `gohome_no_pots`=VALUES(`gohome_no_pots`)",
		char_id,
		(uint32)sd->autobattle_data.mode,
		(uint32)sd->autobattle_data.range,
		(uint32)sd->autobattle_data.target_priority,
		(uint32)sd->autobattle_data.support_skill_count,
		skill_ids, skill_lvs, skill_thresh, skill_scopes, skill_triggers,
		(uint32)sd->autobattle_data.support_item_count,
		item_ids, item_status,
		(uint32)sd->autobattle_data.loot_range,
		(uint32)sd->autobattle_data.loot_rarity_filter,
		mob_csv,
		(uint32)sd->autobattle_data.attack_skill_id,
		(uint32)sd->autobattle_data.attack_skill_lv,
		(uint32)sd->autobattle_data.autosit_hp_threshold,
		(uint32)sd->autobattle_data.autosit_sp_threshold,
		(uint32)sd->autobattle_data.support_target_mode,
		sd->autobattle_data.support_target_name,
		(uint32)sd->autobattle_data.autopot_hp_id,
		(uint32)sd->autobattle_data.autopot_hp_threshold,
		(uint32)sd->autobattle_data.autopot_sp_id,
		(uint32)sd->autobattle_data.autopot_sp_threshold,
		sd->autobattle_data.gohome_no_pots ? 1u : 0u))
	{
		Sql_ShowDebug(mmysql_handle);
		clif_displaymessage(sd->fd, "[Save] !!! SQL ERROR — config did NOT persist. Check map server log.");
	} else {
		clif_displaymessage(sd->fd, "[Save] OK");
	}
}

/**
 * Load full auto-battle config from DB. Called from autobattle_init after the
 * time-cap fields are loaded. Mode bitmask is masked to clear AUTOBATTLE_ATTACK
 * and AUTOBATTLE_SUPPORT — those are session-toggles, not persistent. Target
 * mob whitelist is reset to empty (default = attack all monsters) per the
 * design intent.
 */
void autobattle_load_config_db(map_session_data *sd)
{
	if (!sd)
		return;

	int32 char_id = sd->status.char_id;

	if (SQL_ERROR == Sql_Query(mmysql_handle,
		"SELECT `mode`, `target_priority`, `support_skill_count`, "
		"`support_skill_ids`, `support_skill_lvs`, `support_skill_thresholds`, "
		"`support_skill_scopes`, `support_skill_triggers`, `support_item_count`, "
		"`support_item_ids`, `support_item_status_ids`, `loot_rarity_filter`, "
		"`attack_skill_id`, `attack_skill_lv`, `autosit_hp_threshold`, "
		"`autosit_sp_threshold`, `support_target_mode`, `support_target_name`, "
		"`autopot_hp_id`, `autopot_hp_threshold`, `autopot_sp_id`, "
		"`autopot_sp_threshold`, `gohome_no_pots` "
		"FROM `char_autobattle_config` WHERE `char_id` = %d", char_id))
	{
		Sql_ShowDebug(mmysql_handle);
		clif_displaymessage(sd->fd, "[Load] !!! SQL ERROR on SELECT — likely missing v4 migration columns. Check log.");
		return;
	}

	if (SQL_SUCCESS != Sql_NextRow(mmysql_handle)) {
		Sql_FreeResult(mmysql_handle);
		clif_displaymessage(sd->fd, "[Load] No saved row for this character — first session, defaults applied.");
		return;
	}

	auto get_str = [](int32 col) -> const char* {
		char *v = nullptr;
		Sql_GetData(mmysql_handle, col, &v, nullptr);
		return v ? v : "";
	};
	auto get_u32 = [&get_str](int32 col) -> uint32 {
		const char *v = get_str(col);
		return *v ? (uint32)strtoul(v, nullptr, 10) : 0;
	};

	// Mode: clear ATTACK only — that's the explicit "I want to start
	// auto-fighting now" toggle, so it should be off until the user clicks
	// Enable. SUPPORT IS persisted: once a user added buffs/heals via the
	// menu (which auto-enables SUPPORT), they expect those to keep firing
	// next session. The autobattle timer doesn't actually start ticking
	// until autobattle_start runs (when ATTACK is enabled or @autosupport on
	// is called), so a SUPPORT-only mode at login is dormant — it activates
	// the moment the player turns ATTACK back on.
	uint32 saved_mode = get_u32(0);
	sd->autobattle_data.mode = (uint16)(saved_mode & ~AUTOBATTLE_ATTACK);
	{
		char dbg[256];
		snprintf(dbg, sizeof(dbg),
			"[Load] saved_mode=0x%X loaded_mode=0x%X (loot=%d pot=%d sit=%d fly=%d tele=%d sup=%d)",
			saved_mode, sd->autobattle_data.mode,
			(sd->autobattle_data.mode & AUTOBATTLE_LOOT) ? 1 : 0,
			(sd->autobattle_data.mode & AUTOBATTLE_AUTOPOT) ? 1 : 0,
			(sd->autobattle_data.mode & AUTOBATTLE_AUTOSIT) ? 1 : 0,
			(sd->autobattle_data.mode & AUTOBATTLE_FLYWING) ? 1 : 0,
			(sd->autobattle_data.mode & AUTOBATTLE_TELESKILL) ? 1 : 0,
			(sd->autobattle_data.mode & AUTOBATTLE_SUPPORT) ? 1 : 0);
		clif_displaymessage(sd->fd, dbg);
	}
	// Force PRIORITY_DISTANCE on load — priority isn't user-configurable from
	// any menu, so any saved value other than DISTANCE was set by an old init
	// default before we switched to nearest-first. Don't restore a stale
	// preference no one chose.
	(void)get_u32(1); // Consume the column to keep ordinal indices aligned.
	sd->autobattle_data.target_priority = PRIORITY_DISTANCE;

	// Support skills.
	uint8 skill_count = (uint8)get_u32(2);
	if (skill_count > 10) skill_count = 10;
	uint16 ids[10] = {0}; uint8 lvs[10] = {0}, thresh[10] = {0}, scopes[10] = {0}, triggers[10] = {0};
	autobattle_parse_csv(get_str(3),     ids,      10);
	autobattle_parse_csv_u8(get_str(4),  lvs,      10);
	autobattle_parse_csv_u8(get_str(5),  thresh,   10);
	autobattle_parse_csv_u8(get_str(6),  scopes,   10);
	autobattle_parse_csv_u8(get_str(7),  triggers, 10);
	sd->autobattle_data.support_skill_count = skill_count;
	for (uint8 i = 0; i < skill_count; i++) {
		sd->autobattle_data.support_skills[i].skill_id     = ids[i];
		sd->autobattle_data.support_skills[i].skill_lv     = lvs[i];
		sd->autobattle_data.support_skills[i].hp_threshold = thresh[i];
		sd->autobattle_data.support_skills[i].target_scope = scopes[i];
		sd->autobattle_data.support_skills[i].trigger_type = triggers[i];
		sd->autobattle_data.support_skills[i].buff_id      = (triggers[i] == 1)
			? (uint16)skill_get_sc(ids[i]) : 0;
		sd->autobattle_data.support_skills[i].last_cast_time = 0;
	}

	// Support items.
	uint8 item_count = (uint8)get_u32(8);
	if (item_count > AUTOBATTLE_MAX_ITEM_BUFFS) item_count = AUTOBATTLE_MAX_ITEM_BUFFS;
	uint32 it_ids[AUTOBATTLE_MAX_ITEM_BUFFS] = {0};
	uint16 it_status[AUTOBATTLE_MAX_ITEM_BUFFS] = {0};
	autobattle_parse_csv_u32(get_str(9),  it_ids,    AUTOBATTLE_MAX_ITEM_BUFFS);
	autobattle_parse_csv(get_str(10),     it_status, AUTOBATTLE_MAX_ITEM_BUFFS);
	sd->autobattle_data.support_item_count = item_count;
	for (uint8 i = 0; i < item_count; i++) {
		sd->autobattle_data.support_items[i].item_id       = (t_itemid)it_ids[i];
		sd->autobattle_data.support_items[i].status_id     = (int16)it_status[i];
		sd->autobattle_data.support_items[i].last_use_tick = 0;
	}

	sd->autobattle_data.loot_rarity_filter = (uint8)get_u32(11);
	sd->autobattle_data.attack_skill_id    = (uint16)get_u32(12);
	sd->autobattle_data.attack_skill_lv    = (uint8)get_u32(13);
	sd->autobattle_data.autosit_hp_threshold = (uint8)get_u32(14);
	sd->autobattle_data.autosit_sp_threshold = (uint8)get_u32(15);
	if (sd->autobattle_data.autosit_hp_threshold > 0)
		sd->autobattle_data.autosit_hp_recover = (uint8)std::min((int)sd->autobattle_data.autosit_hp_threshold + 20, 95);
	if (sd->autobattle_data.autosit_sp_threshold > 0)
		sd->autobattle_data.autosit_sp_recover = (uint8)std::min((int)sd->autobattle_data.autosit_sp_threshold + 20, 95);

	sd->autobattle_data.support_target_mode = (uint8)get_u32(16);
	const char *target_name = get_str(17);
	safestrncpy(sd->autobattle_data.support_target_name, target_name,
		sizeof(sd->autobattle_data.support_target_name));

	sd->autobattle_data.autopot_hp_id = (t_itemid)get_u32(18);
	sd->autobattle_data.autopot_hp_threshold = (uint8)get_u32(19);
	if (sd->autobattle_data.autopot_hp_threshold == 0)
		sd->autobattle_data.autopot_hp_threshold = 50;
	sd->autobattle_data.autopot_sp_id = (t_itemid)get_u32(20);
	sd->autobattle_data.autopot_sp_threshold = (uint8)get_u32(21);
	if (sd->autobattle_data.autopot_sp_threshold == 0)
		sd->autobattle_data.autopot_sp_threshold = 30;
	sd->autobattle_data.gohome_no_pots = (get_u32(22) != 0);

	// Per the design intent: target mob whitelist defaults to empty (attack
	// all monsters) every session. Don't restore from DB even if saved.
	memset(sd->autobattle_data.target_mob_ids, 0, sizeof(sd->autobattle_data.target_mob_ids));
	sd->autobattle_data.target_mob_count = 0;

	Sql_FreeResult(mmysql_handle);
}

// ===== Phase 22: Auto-Target Functions =====

/**
 * Set target mob whitelist
 */
void autobattle_set_target_mobs(map_session_data *sd, uint16 *mob_ids, uint8 count)
{
	if (!sd)
		return;

	if (count > 20) count = 20;
	sd->autobattle_data.target_mob_count = count;
	for (int i = 0; i < count; i++)
		sd->autobattle_data.target_mob_ids[i] = mob_ids[i];
	autobattle_save_config_db(sd);
}

/**
 * Clear target mob whitelist (target all)
 */
void autobattle_clear_target_mobs(map_session_data *sd)
{
	if (!sd)
		return;

	memset(sd->autobattle_data.target_mob_ids, 0, sizeof(sd->autobattle_data.target_mob_ids));
	sd->autobattle_data.target_mob_count = 0;
	autobattle_save_config_db(sd);
}

/**
 * Toggle a mob ID in the whitelist (add if missing, remove if present)
 */
void autobattle_toggle_target_mob(map_session_data *sd, uint16 mob_id)
{
	if (!sd || mob_id == 0)
		return;

	// Check if already in list — remove it
	for (int i = 0; i < sd->autobattle_data.target_mob_count; i++) {
		if (sd->autobattle_data.target_mob_ids[i] == mob_id) {
			// Remove by shifting
			for (int j = i; j < sd->autobattle_data.target_mob_count - 1; j++)
				sd->autobattle_data.target_mob_ids[j] = sd->autobattle_data.target_mob_ids[j + 1];
			sd->autobattle_data.target_mob_count--;
			sd->autobattle_data.target_mob_ids[sd->autobattle_data.target_mob_count] = 0;
			autobattle_save_config_db(sd);
			return;
		}
	}

	// Not in list — add if space
	if (sd->autobattle_data.target_mob_count < 20) {
		sd->autobattle_data.target_mob_ids[sd->autobattle_data.target_mob_count] = mob_id;
		sd->autobattle_data.target_mob_count++;
		autobattle_save_config_db(sd);
	}
}

// ===== Phase 23: Auto-Skill Function =====

/**
 * Set offensive skill for auto-attack
 */
void autobattle_set_attack_skill(map_session_data *sd, uint16 skill_id, uint8 skill_lv)
{
	if (!sd)
		return;

	sd->autobattle_data.attack_skill_id = skill_id;
	sd->autobattle_data.attack_skill_lv = skill_lv;
	autobattle_save_config_db(sd);
}

