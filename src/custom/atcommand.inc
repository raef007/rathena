// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

/**
 * Custom Atcommands
 * Place the body of custom atcommands in this file.
 * Format:
 *	ACMD_FUNC(command_name)
 *	{
 *		<code>;
 *	}
 **/

//ACMD_FUNC(newcommand)
//{
//	clif_displaymessage(fd, "It works!");
//	clif_specialeffect(&sd->bl, EF_HEARTCASTING, AREA);
//	return 0;
//}

/**
 * @fakeplayers [count] [mapname]
 * No args: Show current count
 * count > 0: Remove existing + spawn new (on mapname if specified, otherwise world-wide)
 * count = 0: Remove all
 **/
ACMD_FUNC(fakeplayers)
{
	char msg[256];

	if (!message || !*message) {
		// No arguments — show current count
		snprintf(msg, sizeof(msg), "Currently %d fake players active.", fakeplayer_count());
		clif_displaymessage(fd, msg);
		return 0;
	}

	int32 count = 0;
	char mapname[MAP_NAME_LENGTH_EXT] = "";
	sscanf(message, "%d %23s", &count, mapname);

	if (count < 0) {
		clif_displaymessage(fd, "Usage: @fakeplayers [count] [mapname]");
		return -1;
	}

	if (count == 0) {
		fakeplayer_remove_all();
		clif_displaymessage(fd, "All fake players removed.");
		return 0;
	}

	const char* target_map = (mapname[0] != '\0') ? mapname : nullptr;
	int32 spawned = fakeplayer_spawn(count, target_map);

	if (target_map)
		snprintf(msg, sizeof(msg), "Spawned %d/%d fake players on map '%s'.", spawned, count, target_map);
	else
		snprintf(msg, sizeof(msg), "Spawned %d/%d fake players across the world.", spawned, count);
	clif_displaymessage(fd, msg);
	return 0;
}
