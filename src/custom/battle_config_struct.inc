// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

/**
 * Custom battle config structure
 * Place the definition of custom battle configuration structure members in this file.
 *
 * Make sure that the name you want is not already taken by looking into battle.hpp
 * Do not forget to add initilization logic to battle_config_init.inc
 *
 * Format:
 *	 <datatype> name;
 **/

int32 autobattle_default_seconds;    // Default auto-battle time per character (seconds)
int32 autobattle_vip_bonus_seconds;  // Extra seconds granted to VIP players on login
int32 autobattle_max_seconds;        // Maximum auto-battle time including bonuses (seconds)
int32 autobattle_roam_interval;      // Milliseconds between roam attempts when idle

