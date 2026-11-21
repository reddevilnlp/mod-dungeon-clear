/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonRosterBuilders.h"
#include "Ai/Dungeon/DungeonClear/Overrides/ObjectiveHookRegistry.h"
#include "InstanceScript.h"

namespace
{
    constexpr uint32 MAP_ID = 533;
    constexpr uint32 NAXX40_DIFFICULTY = 2;
    constexpr uint32 HORSEMEN_EVENT = 200;
    constexpr uint32 HORSEMEN_ENCOUNTER_BIT = 8;
    constexpr int32 HORSEMEN_ORDER = 80;
    constexpr uint32 HORSEMEN_BOSS_STATE = 12;
    constexpr uint32 HIGHLORD_MOGRAINE = 351037;

    // Dry staging point inside the Horsemen room, reached through the gate
    // before the v34 test pull. Close arrival keeps the raid inside the room.
    constexpr float STAGE_X = 2555.9f;
    constexpr float STAGE_Y = -2996.4f;
    constexpr float STAGE_Z = 241.4f;
    constexpr float STAGE_RADIUS = 5.0f;
}

void RegisterNaxx40HorsemenEvents(std::vector<DungeonEvent>& out)
{
    // The instance starts all four Horsemen when one is engaged. Mograine
    // is the opener, but his death does not finish the encounter. Let the combat
    // engine run the fight and hold the route until the shared state reads DONE.
    out.push_back(
        EventBuilder(MAP_ID, HORSEMEN_EVENT, "The Four Horsemen")
            .Anchored(HORSEMEN_ORDER)
            .ForDifficulty(DcDiffKey::Raid(NAXX40_DIFFICULTY))
            .Persistent()
            .Custom(DC_HOOK_RAID_MUSTER)
                .Timeout(240000)
            .KillCreatureEngage(HIGHLORD_MOGRAINE, 1, 80.0f)
            .MoveToHoldUntilBossState(STAGE_X, STAGE_Y, STAGE_Z, STAGE_RADIUS,
                                      HORSEMEN_BOSS_STATE, DcBossStateBit(DONE))
                .RestartOnBossState(DcBossStateBit(NOT_STARTED) | DcBossStateBit(FAIL))
                .Timeout(900000)
            .Build());
}

void RegisterNaxx40HorsemenRoster(std::vector<BossRosterPatch>& out)
{
    // The bit-8 DungeonEncounter credits spell 59450, which has no spawn to
    // index. Represent the whole fight as a single objective between Gothik
    // and Patchwerk, with the same completion check on a saved instance.
    BossRosterPatch patch;
    patch.mapId = MAP_ID;
    patch.exactDifficulty = DcDiffKey::Raid(NAXX40_DIFFICULTY);
    DungeonBossInfo horsemen =
        DcRoster::MakeObjective(DcRoster::OBJ(HORSEMEN_EVENT), HORSEMEN_ENCOUNTER_BIT,
                                MAP_ID, "The Four Horsemen", STAGE_X, STAGE_Y, STAGE_Z,
                                STAGE_RADIUS, 0, 0, HORSEMEN_EVENT, HORSEMEN_ORDER);
    horsemen.doneBossStateIndex = HORSEMEN_BOSS_STATE;
    patch.add.push_back(std::move(horsemen));
    out.push_back(std::move(patch));
}
