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
		default:
			script_pushint(st, 0);
			break;
	}
	return SCRIPT_CMD_SUCCESS;
}

/**
 * getskillname(<skill_id>)
 * Returns the user-friendly display name of a skill by its ID.
 * Uses skill_get_desc() which returns the Description field (e.g. "Bash")
 * instead of skill_get_name() which returns the AegisName (e.g. "SM_BASH").
 * Returns "" if skill not found.
 */
BUILDIN_FUNC(getskillname)
{
	uint16 skill_id = (uint16)script_getnum(st, 2);
	const char* desc = skill_get_desc(skill_id);
	if (desc && desc[0] != '\0')
		script_pushstrcopy(st, desc);
	else {
		// Fallback to AegisName if no description
		const char* name = skill_get_name(skill_id);
		if (name)
			script_pushstrcopy(st, name);
		else
			script_pushconststr(st, "");
	}
	return SCRIPT_CMD_SUCCESS;
}

