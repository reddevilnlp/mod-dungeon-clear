/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 */

#ifndef _DC_NAXX40_HORSEMEN_H
#define _DC_NAXX40_HORSEMEN_H

#include <optional>

#include "Define.h"

class Player;
class PlayerbotAI;
class Creature;
struct DungeonBossInfo;

namespace DcNaxx40Horsemen
{
    inline constexpr uint32 KorthazzEntry = 351039;
    bool IsObjective(Player const* bot, uint32 entry);
    bool IsEncounterMob(Player const& bot, uint32 entry);
    std::optional<bool> Stage(PlayerbotAI& ai, DungeonBossInfo const& next);
    bool OwnsMovement(PlayerbotAI& ai);
    bool Ready(PlayerbotAI& ai);
    bool ConfirmReady(PlayerbotAI& ai);
    std::optional<bool> TryRangedPull(PlayerbotAI& ai, Creature& boss);
}

#endif
