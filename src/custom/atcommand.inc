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
 * @fakeplayers [count]
 * No args: Show current count
 * count > 0: Remove existing + spawn new
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

	int32 count = atoi(message);

	if (count < 0) {
		clif_displaymessage(fd, "Usage: @fakeplayers [count] — count must be 0 or positive.");
		return -1;
	}

	if (count == 0) {
		fakeplayer_remove_all();
		clif_displaymessage(fd, "All fake players removed.");
		return 0;
	}

	int32 spawned = fakeplayer_spawn(count);
	snprintf(msg, sizeof(msg), "Spawned %d/%d fake players across the world.", spawned, count);
	clif_displaymessage(fd, msg);
	return 0;
}
