/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, or later.
 */

#ifndef DUNGEONCLEAR_NAXX40_LIVING_POISON_H
#define DUNGEONCLEAR_NAXX40_LIVING_POISON_H

#include "Define.h"

class Player;
class PlayerbotAI;
class Creature;

namespace DcNaxx40LivingPoison
{
    inline constexpr uint32 GrobbulusEntry = 351003;
    inline constexpr uint32 GrobbulusEastPullOrder = 4;
    inline constexpr uint32 GrobbulusWestPullOrder = 6;
    inline constexpr uint32 GrobbulusSouthPullOrder = 7;
    bool BeginApproach(PlayerbotAI* ai, uint32 nextBoss);
    bool BlocksGrobbulusPull(PlayerbotAI* ai, uint32 nextBoss);
    bool UsesGrobbulusPatrol(PlayerbotAI* ai, uint32 nextBoss);
    bool TryGrobbulusPatrolPull(PlayerbotAI* ai, Creature& boss);
    bool GrobbulusScriptedPullReady(Player* tank, uint32 order);
    bool OwnsGrobbulusApproach(PlayerbotAI* ai);
    bool OwnsMovement(PlayerbotAI* ai, bool forRest = false);
    bool IsCrossingLane(Player* player);
    void RegisterMapUpdate();
}

#endif
