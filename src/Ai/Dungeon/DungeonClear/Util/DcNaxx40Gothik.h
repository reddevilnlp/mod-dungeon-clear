/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, or later.
 */

#ifndef DUNGEONCLEAR_NAXX40_GOTHIK_H
#define DUNGEONCLEAR_NAXX40_GOTHIK_H

#include "Define.h"
#include "Position.h"

class Creature;
class Player;
class PlayerbotAI;

namespace DcNaxx40Gothik
{
    bool UsesRangedPull(Player const* tank, uint32 nextBoss);
    Position const& PullPoint();
    bool AtPullPoint(Player const& tank);
    bool TryRangedPull(PlayerbotAI* ai, Creature& boss);
}

#endif
