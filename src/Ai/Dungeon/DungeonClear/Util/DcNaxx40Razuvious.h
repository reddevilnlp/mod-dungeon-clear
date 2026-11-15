/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, or later.
 */

#ifndef DUNGEONCLEAR_NAXX40_RAZUVIOUS_H
#define DUNGEONCLEAR_NAXX40_RAZUVIOUS_H

#include "Define.h"

class Creature;
class Player;
class PlayerbotAI;

namespace DcNaxx40Razuvious
{
    bool UsesOpening(PlayerbotAI* ai, uint32 nextBoss);
    bool AtCamp(Player const& bot);
    bool Stage(PlayerbotAI* ai);
    bool Open(PlayerbotAI* ai, Creature& boss);
    bool HoldsPosition(PlayerbotAI* ai);
}

#endif
