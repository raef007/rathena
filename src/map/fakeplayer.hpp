// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder
// Fake Player System — populates the world with autonomous mob-based player-lookalikes

#ifndef FAKEPLAYER_HPP
#define FAKEPLAYER_HPP

#include <common/cbasetypes.hpp>

void fakeplayer_init(void);
void fakeplayer_final(void);
int32 fakeplayer_spawn(int32 count);
void fakeplayer_remove_all(void);
int32 fakeplayer_count(void);
bool fakeplayer_is_fakeplayer(int32 id);
bool fakeplayer_is_fakeplayer_name(const char* name);

#endif /* FAKEPLAYER_HPP */
