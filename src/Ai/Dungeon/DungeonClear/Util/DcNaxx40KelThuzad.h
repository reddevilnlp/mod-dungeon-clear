/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 */

#ifndef _DC_NAXX40_KELTHUZAD_H
#define _DC_NAXX40_KELTHUZAD_H

#include <optional>
#include "Define.h"

class Player;
class PlayerbotAI;
struct DungeonBossInfo;
struct Position;

namespace DcNaxx40KelThuzad
{
    bool IsObjective(Player const* player, uint32 entry);
    Position const& Camp();
    std::optional<bool> Stage(PlayerbotAI& ai, DungeonBossInfo const& next);
    std::optional<bool> StageFollower(PlayerbotAI& ai, Player* leader);
    bool OwnsMovement(PlayerbotAI& ai);
}

#endif
