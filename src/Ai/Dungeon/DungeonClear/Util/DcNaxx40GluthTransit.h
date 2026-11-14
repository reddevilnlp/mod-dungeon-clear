/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, or later.
 */

#ifndef DUNGEONCLEAR_NAXX40_GLUTH_TRANSIT_H
#define DUNGEONCLEAR_NAXX40_GLUTH_TRANSIT_H

#include "Define.h"

class Player;
class PlayerbotAI;

namespace DcNaxx40GluthTransit
{
    bool BlocksGeneric(Player* bot, uint32 nextBoss);
    bool OwnsMovement(PlayerbotAI* ai);
    bool InGasTransit(PlayerbotAI* ai);
    void RegisterMapUpdate();
}

#endif
