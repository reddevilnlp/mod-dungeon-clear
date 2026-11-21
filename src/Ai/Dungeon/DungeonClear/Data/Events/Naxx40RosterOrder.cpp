/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Overrides/BossRosterRegistry.h"

void RegisterNaxx40RosterOrder(std::vector<BossRosterPatch>& out)
{
    BossRosterPatch patch;
    patch.mapId = 533;
    patch.exactDifficulty = DcDiffKey::Raid(2);
    // Keep the Individual Progression encounter order and credit bits. Leave
    // room between consecutive bosses for anchored preparation and exits.
    patch.reorder = {
        {351009, 0}, {351007, 10}, {351006, 20},       // Arachnid
        {351008, 30}, {351005, 40}, {351020, 50},      // Plague
        {351036, 60}, {351035, 70},                    // Military (Horsemen objective: 80)
        {351028, 90}, {351003, 100}, {351004, 110}, {351000, 120}, // Construct
        {351018, 130}, {351019, 140},                  // Frostwyrm Lair
    };
    out.push_back(std::move(patch));
}
