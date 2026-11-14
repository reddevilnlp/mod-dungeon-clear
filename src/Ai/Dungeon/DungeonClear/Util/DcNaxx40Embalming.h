/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, or later.
 */

#ifndef DUNGEONCLEAR_NAXX40_EMBALMING_H
#define DUNGEONCLEAR_NAXX40_EMBALMING_H

#include <optional>

#include "Position.h"

class Player;
class Unit;
class PlayerbotAI;

namespace DcNaxx40Embalming
{
    bool IsSlime(Unit const* unit);
    bool CanAttackFromCamp(Player* bot);
    std::optional<Position> PullCamp(Player* tank, Unit* target);
    Position RetreatPoint(Position const& camp);
    std::optional<Position> GetCamp(Player* bot, bool* isPuller = nullptr);
    bool FindSaferSpot(PlayerbotAI* ai, Position const& camp, Position& destination);
}

#endif
