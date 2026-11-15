/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, or later.
 */

#include "DcNaxx40Gothik.h"

#include "Ai/Dungeon/DungeonClear/Action/DcActionShared.h"
#include "Ai/Raid/Naxx40/Naxx40Encounter.h"
#include "Creature.h"
#include "DcMovement.h"
#include "DcRun.h"
#include "Log.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Timer.h"

namespace DcNaxx40Gothik
{
bool UsesRangedPull(Player const* tank, uint32 nextBoss)
{
    return nextBoss == 351035 && tank && IsNaxx40Map(tank->FindMap());
}

Position const& PullPoint()
{
    // Dry floor on the living side, with line of sight to the balcony.
    static Position const point{2659.0f, -3387.0f, 267.685f};
    return point;
}

bool AtPullPoint(Player const& tank)
{
    return tank.GetExactDist(PullPoint()) <= 1.5f;
}

bool TryRangedPull(PlayerbotAI* ai, Creature& boss)
{
    Player& tank = *ai->GetBot();
    if (!AtPullPoint(tank) || boss.IsInCombat())
        return false;

    uint32& lastTag = DcRun::Of(ai).naxx40GothikTagMs;
    uint32 const now = getMSTime();
    if (lastTag && getMSTimeDiff(lastTag, now) < 3000)
        return true;
    lastTag = now;
    DcMovement::StopBot(&tank, DcMovement::Stop::HardPin);

    auto const opener = DcActionShared::ResolvePullSpell(ai, &tank);
    if (!opener)
    {
        DcActionShared::StallDungeonClear(ai, "Gothik: the tank needs a ranged pull spell or weapon.");
        return true;
    }

    float const distance = tank.GetExactDist(&boss);
    if (distance < opener->minRange || distance > opener->maxRange || !tank.IsWithinLOSInMap(&boss))
    {
        DcActionShared::StallDungeonClear(ai, "Gothik: the balcony is out of range or line of sight from the pull point.");
        return true;
    }

    tank.SetSelection(boss.GetGUID());
    tank.SetFacingToObject(&boss);
    if (!ai->CastSpell(opener->spellId, &boss))
    {
        DcActionShared::StallDungeonClear(ai, "Gothik: waiting for the tank's ranged opener.");
        return true;
    }
    DcActionShared::ClearStall(ai->GetAiObjectContext());
    DcActionShared::SetPhase(ai->GetAiObjectContext(), "ranged pull for Gothik");
    LOG_INFO("playerbots.dungeonclear",
        "[DC:{}] Naxx40 Gothik ranged tag from ({:.2f},{:.2f},{:.2f}), spell {} at {:.1f}yd",
        tank.GetName(), tank.GetPositionX(), tank.GetPositionY(), tank.GetPositionZ(), opener->spellId, distance);
    return true;
}
}
