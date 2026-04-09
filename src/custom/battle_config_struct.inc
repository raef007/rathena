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
int32 autobattle_exp_penalty_base;   // Base EXP penalty % during auto-battle (0=none, 100=zero)
int32 autobattle_exp_penalty_job;    // Job EXP penalty % during auto-battle (0=none, 100=zero)

int32 fakeplayer_enabled;            // Enable/disable fake player system (0: disabled, 1: enabled)
int32 fakeplayer_respawn_interval;   // How often to respawn dead fake players (seconds)
int32 fakeplayer_online_count;       // Add fake player count to displayed online count (0/1)

