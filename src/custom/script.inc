// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

/**
 * Custom Script Commands
 * Place the body of custom script commands in this file.
 * Format:
 *	BUILDIN_FUNC(command_name)
 *	{
 *		<code>;
 *	}
 **/

//BUILDIN_FUNC(example)
//{
//	ShowInfo("buildin_example: It works!\n");
//	script_pushint(st,1);
//	return 0;
//}

/**
 * getskillinfo(<skill_id>, <type>)
 * Returns skill information flags.
 * type 0: skill_get_inf() — returns INF_* flags (INF_ATTACK_SKILL=0x01, INF_SUPPORT_SKILL=0x10, etc.)
 * type 1: skill_get_nk() NK_NODAMAGE flag (1=no damage / support, 0=damage)
 * type 2: skill_get_sp() — SP cost at level 1
 * type 3: skill_get_range() — base range
 * type 4: returns 1 if skill is a quest skill (INF2_ISQUEST), 0 otherwise
 * type 5: returns 1 if skill is passive (inf == 0 / INF_PASSIVE_SKILL), 0 otherwise
 * type 6: skill_get_sc() — status applied by the skill, or SC_NONE
 */
BUILDIN_FUNC(getskillinfo)
{
	uint16 skill_id = (uint16)script_getnum(st, 2);
	int32 type = script_getnum(st, 3);

	switch (type) {
		case 0:
			script_pushint(st, skill_get_inf(skill_id));
			break;
		case 1:
			script_pushint(st, skill_get_nk(skill_id, NK_NODAMAGE) ? 1 : 0);
			break;
		case 2:
			script_pushint(st, skill_get_sp(skill_id, 1));
			break;
		case 3:
			script_pushint(st, skill_get_range(skill_id, 1));
			break;
		case 4:
			script_pushint(st, skill_get_inf2(skill_id, INF2_ISQUEST) ? 1 : 0);
			break;
		case 5:
			script_pushint(st, (skill_get_inf(skill_id) == INF_PASSIVE_SKILL) ? 1 : 0);
			break;
		case 6:
			script_pushint(st, skill_get_sc(skill_id));
			break;
		default:
			script_pushint(st, 0);
			break;
	}
	return SCRIPT_CMD_SUCCESS;
}

/**
 * isautobattlebuffskill(<skill_id>) -> 0|1
 * Self/ally buff candidate filter for the auto-support menu.
 * Excludes a couple of debuffs that share the buff-skill shape (Decrease AGI,
 * Provoke, etc.) on top of the baseline filter in autobattle_is_buff_skill().
 */
BUILDIN_FUNC(isautobattlebuffskill)
{
	uint16 skill_id = (uint16)script_getnum(st, 2);

	if (!autobattle_is_buff_skill(skill_id)) {
		script_pushint(st, 0);
		return SCRIPT_CMD_SUCCESS;
	}

	// Filter known debuffs that pass the buff-skill shape test (no damage,
	// support inf, has SC). These hit enemies in practice — never offer them
	// as self/ally buffs.
	switch (skill_get_sc(skill_id)) {
		case SC_DECREASEAGI:
		case SC_SIGNUMCRUCIS:
		case SC_AETERNA:
		case SC_PROVOKE:
		case SC_QUAGMIRE:
			script_pushint(st, 0);
			return SCRIPT_CMD_SUCCESS;
		default:
			break;
	}

	script_pushint(st, 1);
	return SCRIPT_CMD_SUCCESS;
}

/**
 * isautobattleattackskill(<skill_id>) -> 0|1
 * Offensive skill picker filter for the auto-attack menu. Returns 1 for damage
 * skills the player can target an enemy with (attack/ground/trap inf, has
 * damage). Excludes passives, quest skills, wedding/spirit/guild/NPC skills.
 */
BUILDIN_FUNC(isautobattleattackskill)
{
	uint16 skill_id = (uint16)script_getnum(st, 2);
	script_pushint(st, autobattle_is_attack_skill(skill_id) ? 1 : 0);
	return SCRIPT_CMD_SUCCESS;
}

/**
 * autobattleitembuffstatus(<item_id>) -> <sc_type or 0>
 * Returns the status effect granted by a recognised buff consumable, or 0
 * (SC_NONE) if the item is not a known buff. Used by the menu to filter the
 * inventory down to selectable item buffs.
 */
BUILDIN_FUNC(autobattleitembuffstatus)
{
	t_itemid item_id = (t_itemid)script_getnum(st, 2);
	script_pushint(st, autobattle_get_item_buff_status(item_id));
	return SCRIPT_CMD_SUCCESS;
}

/**
 * isautoattackon() -> 0|1
 * Returns 1 if the attached player currently has auto-attack enabled, 0
 * otherwise. Lets the menu render a single Enable/Disable toggle that
 * reflects current state instead of requiring two separate options.
 */
BUILDIN_FUNC(isautoattackon)
{
	TBL_PC* sd;
	if (!script_rid2sd(sd))
		return SCRIPT_CMD_SUCCESS;
	script_pushint(st, (sd->autobattle_data.mode & AUTOBATTLE_ATTACK) ? 1 : 0);
	return SCRIPT_CMD_SUCCESS;
}

/**
 * isautobattlemode(<bitmask>) -> 0|1
 * Returns 1 if any of the given mode bits are set in the attached player's
 * auto-battle mode bitmask. Used by the menu to render single Enable/Disable
 * toggles for sub-features (Fly Wing 0x40, Teleskill 0x100, Loot 0x04, etc.).
 */
BUILDIN_FUNC(isautobattlemode)
{
	TBL_PC* sd;
	if (!script_rid2sd(sd))
		return SCRIPT_CMD_SUCCESS;
	uint16 mask = (uint16)script_getnum(st, 2);
	script_pushint(st, (sd->autobattle_data.mode & mask) ? 1 : 0);
	return SCRIPT_CMD_SUCCESS;
}

/**
 * getskillname(<skill_id>)
 * Returns the friendly display name (Description) of a skill by its ID.
 * Falls back to AEGIS name if description is empty.
 * Returns "" if skill not found.
 */
BUILDIN_FUNC(getskillname)
{
	uint16 skill_id = (uint16)script_getnum(st, 2);
	const char* desc = skill_get_desc(skill_id);
	if (desc && desc[0] != '\0')
		script_pushstrcopy(st, desc);
	else {
		// Fallback to AEGIS name if no description
		const char* name = skill_get_name(skill_id);
		if (name)
			script_pushstrcopy(st, name);
		else
			script_pushconststr(st, "");
	}
	return SCRIPT_CMD_SUCCESS;
}

