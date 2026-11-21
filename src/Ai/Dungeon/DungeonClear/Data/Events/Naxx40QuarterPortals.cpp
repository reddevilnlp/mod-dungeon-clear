/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonRosterBuilders.h"
#include "InstanceScript.h"
#include "Player.h"

namespace
{
    constexpr uint32 MAP_ID = 533;
    constexpr uint32 NAXX40_DIFFICULTY = 2;
    constexpr float WING_RADIUS = 75.0f;
    constexpr float ARRIVE_RADIUS = 2.0f;

    // Spell 28444 lands each portal user on the raised central platform.
    constexpr float HUB_X = 3005.510f;
    constexpr float HUB_Y = -3434.640f;
    constexpr float HUB_Z = 304.195f;
    constexpr float HUB_RADIUS = 12.0f;

    // These are Individual Progression instance slots and the surveyed dry
    // positions within interact range of their associated wing portals.
    constexpr uint32 MAEXXNA_EXIT_EVENT = 53301;
    constexpr int32 MAEXXNA_EXIT_ORDER = 25;
    constexpr uint32 MAEXXNA_BOSS_STATE = 8;
    constexpr float MAEXXNA_CLICK_X = 3465.753f;
    constexpr float MAEXXNA_CLICK_Y = -3939.677f;
    constexpr float MAEXXNA_CLICK_Z = 306.900f;

    constexpr uint32 LOATHEB_EXIT_EVENT = 53302;
    constexpr int32 LOATHEB_EXIT_ORDER = 55;
    constexpr uint32 LOATHEB_BOSS_STATE = 5;
    constexpr float LOATHEB_CLICK_X = 2909.000f;
    constexpr float LOATHEB_CLICK_Y = -4025.020f;
    constexpr float LOATHEB_CLICK_Z = 273.691f;

    constexpr uint32 HORSEMEN_EXIT_EVENT = 53303;
    constexpr int32 HORSEMEN_EXIT_ORDER = 85;
    constexpr uint32 HORSEMEN_BOSS_STATE = 12;
    constexpr float HORSEMEN_CLICK_X = 2493.020f;
    constexpr float HORSEMEN_CLICK_Y = -2921.780f;
    constexpr float HORSEMEN_CLICK_Z = 241.440f;

    constexpr uint32 THADDIUS_EXIT_EVENT = 53304;
    constexpr int32 THADDIUS_EXIT_ORDER = 125;
    constexpr uint32 THADDIUS_BOSS_STATE = 9;
    constexpr float THADDIUS_CLICK_X = 3539.020f;
    constexpr float THADDIUS_CLICK_Y = -2936.820f;
    constexpr float THADDIUS_CLICK_Z = 302.571f;

    bool QuarterBossDone(Player* tank, uint32 slot)
    {
        if (!tank || !tank->IsAlive())
            return false;
        InstanceScript* instance = tank->GetInstanceScript();
        return instance && instance->IsBossDone(slot);
    }

    bool CanLeaveArachnidQuarter(Player* tank)
    {
        return QuarterBossDone(tank, MAEXXNA_BOSS_STATE) &&
               tank->GetExactDist(MAEXXNA_CLICK_X, MAEXXNA_CLICK_Y, MAEXXNA_CLICK_Z) <= WING_RADIUS;
    }

    bool CanLeavePlagueQuarter(Player* tank)
    {
        return QuarterBossDone(tank, LOATHEB_BOSS_STATE) &&
               tank->GetExactDist(LOATHEB_CLICK_X, LOATHEB_CLICK_Y, LOATHEB_CLICK_Z) <= WING_RADIUS;
    }

    bool CanLeaveMilitaryQuarter(Player* tank)
    {
        if (!QuarterBossDone(tank, HORSEMEN_BOSS_STATE))
            return false;

        // Include the dais and both sides of the room.
        float const x = tank->GetPositionX();
        float const y = tank->GetPositionY();
        float const z = tank->GetPositionZ();
        return x >= 2450.0f && x <= 2630.0f && y >= -3016.0f && y <= -2880.0f &&
               z >= 239.0f && z <= 248.0f;
    }

    bool CanLeaveConstructQuarter(Player* tank)
    {
        return QuarterBossDone(tank, THADDIUS_BOSS_STATE) &&
               tank->GetExactDist(THADDIUS_CLICK_X, THADDIUS_CLICK_Y, THADDIUS_CLICK_Z) <= WING_RADIUS;
    }
}

void RegisterNaxx40QuarterPortalEvents(std::vector<DungeonEvent>& out)
{
    out.push_back(
        EventBuilder(MAP_ID, MAEXXNA_EXIT_EVENT, "Maexxna quarter exit")
            .Anchored(MAEXXNA_EXIT_ORDER)
            .ForDifficulty(DcDiffKey::Raid(NAXX40_DIFFICULTY))
            .PanelAfterBoss(351006)
            .Persistent()
            .StepsOwnMovement()
            .UseGOForGroup(181575, MAEXXNA_CLICK_X, MAEXXNA_CLICK_Y, MAEXXNA_CLICK_Z,
                           HUB_X, HUB_Y, HUB_Z, HUB_RADIUS)
                .Timeout(180000)
            .Build());

    out.push_back(
        EventBuilder(MAP_ID, LOATHEB_EXIT_EVENT, "Loatheb quarter exit")
            .Anchored(LOATHEB_EXIT_ORDER)
            .ForDifficulty(DcDiffKey::Raid(NAXX40_DIFFICULTY))
            .PanelAfterBoss(351020)
            .Persistent()
            .StepsOwnMovement()
            .UseGOForGroup(181577, LOATHEB_CLICK_X, LOATHEB_CLICK_Y, LOATHEB_CLICK_Z,
                           HUB_X, HUB_Y, HUB_Z, HUB_RADIUS)
                .Timeout(180000)
            .Build());

    out.push_back(
        EventBuilder(MAP_ID, HORSEMEN_EXIT_EVENT, "Four Horsemen quarter exit")
            .Anchored(HORSEMEN_EXIT_ORDER)
            .ForDifficulty(DcDiffKey::Raid(NAXX40_DIFFICULTY))
            .PanelAfterBoss(DcRoster::OBJ(200))
            .Persistent()
            .StepsOwnMovement()
            .UseGOForGroup(181578, HORSEMEN_CLICK_X, HORSEMEN_CLICK_Y, HORSEMEN_CLICK_Z,
                           HUB_X, HUB_Y, HUB_Z, HUB_RADIUS)
                .Timeout(180000)
            .Build());

    out.push_back(
        EventBuilder(MAP_ID, THADDIUS_EXIT_EVENT, "Thaddius quarter exit")
            .Anchored(THADDIUS_EXIT_ORDER)
            .ForDifficulty(DcDiffKey::Raid(NAXX40_DIFFICULTY))
            .PanelAfterBoss(351000)
            .Persistent()
            .StepsOwnMovement()
            .UseGOForGroup(181576, THADDIUS_CLICK_X, THADDIUS_CLICK_Y, THADDIUS_CLICK_Z,
                           HUB_X, HUB_Y, HUB_Z, HUB_RADIUS)
                .Timeout(180000)
            .Build());
}

void RegisterNaxx40QuarterPortalRoster(std::vector<BossRosterPatch>& out)
{
    BossRosterPatch patch;
    patch.mapId = MAP_ID;
    patch.exactDifficulty = DcDiffKey::Raid(NAXX40_DIFFICULTY);

    // A completed boss enables an exit only from its wing; a clear resumed at
    // the hub must not backtrack to the old portal.
    DungeonBossInfo maexxna =
        DcRoster::MakeObjective(DcRoster::OBJ(201), 2, MAP_ID, "Maexxna quarter exit",
                                MAEXXNA_CLICK_X, MAEXXNA_CLICK_Y, MAEXXNA_CLICK_Z,
                                ARRIVE_RADIUS, 0, 0, MAEXXNA_EXIT_EVENT, MAEXXNA_EXIT_ORDER);
    maexxna.availableWhen = &CanLeaveArachnidQuarter;
    patch.add.push_back(std::move(maexxna));

    DungeonBossInfo loatheb =
        DcRoster::MakeObjective(DcRoster::OBJ(202), 5, MAP_ID, "Loatheb quarter exit",
                                LOATHEB_CLICK_X, LOATHEB_CLICK_Y, LOATHEB_CLICK_Z,
                                ARRIVE_RADIUS, 0, 0, LOATHEB_EXIT_EVENT, LOATHEB_EXIT_ORDER);
    loatheb.availableWhen = &CanLeavePlagueQuarter;
    patch.add.push_back(std::move(loatheb));

    DungeonBossInfo horsemen =
        DcRoster::MakeObjective(DcRoster::OBJ(203), 8, MAP_ID, "Four Horsemen quarter exit",
                                HORSEMEN_CLICK_X, HORSEMEN_CLICK_Y, HORSEMEN_CLICK_Z,
                                ARRIVE_RADIUS, 0, 0, HORSEMEN_EXIT_EVENT, HORSEMEN_EXIT_ORDER);
    horsemen.availableWhen = &CanLeaveMilitaryQuarter;
    patch.add.push_back(std::move(horsemen));

    DungeonBossInfo thaddius =
        DcRoster::MakeObjective(DcRoster::OBJ(204), 12, MAP_ID, "Thaddius quarter exit",
                                THADDIUS_CLICK_X, THADDIUS_CLICK_Y, THADDIUS_CLICK_Z,
                                ARRIVE_RADIUS, 0, 0, THADDIUS_EXIT_EVENT, THADDIUS_EXIT_ORDER);
    thaddius.availableWhen = &CanLeaveConstructQuarter;
    patch.add.push_back(std::move(thaddius));

    out.push_back(std::move(patch));
}
