/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonRosterBuilders.h"

// --- Naxxramas (map 533) -----------------------------------------------------
//
// AUTHORED FROM THE CORE DATA (instance_encounters, the world DB spawn rows)
// and Individual Progression's 40-man spawns; not yet validated live.
//
// The other encounters auto-derive from BossSpawnIndex. The one it drops is:
//
//   * THE FOUR HORSEMEN (DBC bit 8). The encounter is real, but its
//     instance_encounters credit is a CAST-SPELL (59450), not a kill, and
//     BossSpawnIndex only indexes ENCOUNTER_CREDIT_KILL_CREATURE rows (see the
//     MakeBossWithBit note in DungeonRosterBuilders.h). Without the row the clear
//     finishes Gothik (bit 7) and walks back to Patchwerk (bit 9), leaving the
//     Military Quarter unfinished. MakeBossWithBit puts the bit back. Completion
//     rides the encounter mask like any other boss: the core sets bit 8 when the
//     credit spell lands.
//
// ANCHOR: one horseman. Engaging any of them zone-pulls all four, so the ordinary
// boss row (raid muster, engage, pull) needs no event of its own. The 10/25-man
// spawns and IP's 40-man spawns are different creatures, hence two patches:
//
//   * NormalOnly (10N, 25N; raid difficulty 0/1): Baron Rivendare (30549).
//   * HeroicOnly (IP 40-man, raid difficulty 2 — map 533 has no difficulty 3):
//     Highlord Mograine (351037).
//
// ORDER: none needed. Bit 8 sorts between Gothik (7) and Patchwerk (9) on its own.

namespace
{
    constexpr uint32 kMapId = 533;

    constexpr uint32 kFourHorsemenBit = 8;

    constexpr uint32 kBaronRivendare   = 30549;   // 10/25-man horseman
    constexpr uint32 kHighlordMograine = 351037;  // Individual Progression 40-man horseman
}

void RegisterNaxxramasRoster(std::vector<BossRosterPatch>& t)
{
    using namespace DcRoster;

    // 10N/25N: Baron Rivendare's spawn (creature guid 130963).
    BossRosterPatch normal;
    normal.mapId = kMapId;
    normal.gate = DcDifficultyGate::NormalOnly;
    normal.add.push_back(MakeBossWithBit(kBaronRivendare, kMapId, "The Four Horsemen",
                                         2524.32f, -2951.28f, 245.633f, kFourHorsemenBit));
    t.push_back(std::move(normal));

    // IP 40-man: Highlord Mograine's spawn.
    BossRosterPatch forty;
    forty.mapId = kMapId;
    forty.gate = DcDifficultyGate::HeroicOnly;
    forty.add.push_back(MakeBossWithBit(kHighlordMograine, kMapId, "The Four Horsemen",
                                        2519.95f, -2947.47f, 245.64f, kFourHorsemenBit));
    t.push_back(std::move(forty));
}
