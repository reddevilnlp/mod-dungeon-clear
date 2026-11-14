/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, or later.
 */

#ifndef DUNGEONCLEAR_NAXX40_THADDIUS_H
#define DUNGEONCLEAR_NAXX40_THADDIUS_H

#include "Define.h"

class Player;
class PlayerbotAI;

namespace DcNaxx40Thaddius
{
    bool BlocksGeneric(Player* bot, uint32 nextBoss);
    bool OwnsMovement(PlayerbotAI* ai);
    void RegisterMapUpdate();
}

#endif
