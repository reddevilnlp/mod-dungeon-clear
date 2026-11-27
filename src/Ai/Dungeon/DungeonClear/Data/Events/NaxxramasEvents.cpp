/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonRosterBuilders.h"

#include "InstanceScript.h"
#include "Player.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTargeting.h"

// --- Naxxramas (map 533) -----------------------------------------------------
//
// AUTHORED FROM THE CORE DATA (instance_encounters, the world DB spawn rows,
// spell_target_position, instance_naxxramas.cpp) and Individual Progression's
// 40-man spawns; not yet validated live.
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
//
// --- Getting around: the wing portals and the Frostwyrm Lair -----------------
//
// Naxxramas is a hub with four wings. The zone-in lands the raid in the central
// hub, and each wing ends in a dead end. Two kinds of teleport join the route up:
//
//   * WING PORTALS. Killing a wing's last boss (Maexxna, Loatheb, Thaddius, the
//     Horsemen) activates a "Naxxramas Portal" at the end of that wing, which
//     casts 28444 and lands the user in the hub. Without it the clear walks the
//     whole wing back to reach the next one. One Conditional event per wing
//     teleports the raid from the portal to 28444's landing once the wing's last
//     boss is DONE and the leader is still near the portal. The proximity gate
//     keeps it from firing anywhere but at the end of that wing.
//
//   * FROSTWYRM LAIR. Sapphiron and Kel'Thuzad sit in a lair with no walkable
//     link to the rest of the map. at_naxxramas_hub_portal (areatrigger 4156, in
//     the hub) teleports a player there once all four wing bosses are DONE. The
//     event does the same for the raid: all four wing bosses DONE, Kel'Thuzad
//     not, leader in the hub. REPEATABLE so a wipe in the lair, which sends the
//     raid back to the zone-in beside the hub, re-arms it; the proximity gate
//     reads false the moment the raid has left the hub, so it cannot spin.
//
// DONE IS THE INSTANCE SCRIPT'S BOSS STATE, NOT THE ENCOUNTER MASK. The portals
// themselves (ActivateWingPortal) and at_naxxramas_hub_portal key off
// GetBossState(BOSS_*), and so do these conditions. The DBC completion mask is
// not enough: Individual Progression's 40-man script never calls
// UpdateEncounterState, and its bosses are custom entries (Maexxna is 351006)
// that match no DungeonEncounter credit, so at difficulty 2 the mask stays 0
// all run. Live, the first 40-man run killed Maexxna 50yd from her portal, the
// event never armed, and the raid walked back down the Arachnid Quarter. The
// BOSS_* ids below are the ones naxxramas.h gives both the core and IP scripts.
//
// Both use TeleportParty: the leader walks to the checkpoint (the portal, or the
// hub), the step waits out any fight there, then the whole raid is relocated.
//
// The portal positions are the core's 10/25-man gameobject spawns (spawn mask 3).
// IP's 40-man runs at difficulty 2 and is assumed to use the same rooms.

namespace
{
    constexpr uint32 kMapId = 533;

    constexpr uint32 kFourHorsemenBit = 8;

    constexpr uint32 kBaronRivendare   = 30549;   // 10/25-man horseman
    constexpr uint32 kHighlordMograine = 351037;  // Individual Progression 40-man horseman

    // Wing-end bosses (panel placement).
    constexpr uint32 kMaexxna  = 15952;
    constexpr uint32 kLoatheb  = 16011;
    constexpr uint32 kThaddius = 15928;

    // The instance script's boss-state slots (naxxramas.h BOSS_*, identical in
    // the core and IP scripts) — a different index space from the DBC bits.
    constexpr uint32 kBossLoatheb   = 5;   // BOSS_LOATHEB
    constexpr uint32 kBossMaexxna   = 8;   // BOSS_MAEXXNA
    constexpr uint32 kBossThaddius  = 9;   // BOSS_THADDIUS
    constexpr uint32 kBossHorsemen  = 12;  // BOSS_HORSEMAN
    constexpr uint32 kBossKelThuzad = 14;  // BOSS_KELTHUZAD

    // Hub landing: spell_target_position of 28444, the wing portals' teleport.
    constexpr float kHubX = 3005.51f, kHubY = -3434.64f, kHubZ = 304.195f;
    // Frostwyrm Lair landing: spell_target_position of 72617 (SapphironTeleportPos).
    constexpr float kLairX = 3498.3f, kLairY = -5349.49f, kLairZ = 144.968f;

    struct WingPortal
    {
        float x, y, z;
    };

    // "Naxxramas Portal" gameobject spawns at the end of each wing.
    constexpr WingPortal kMaexxnaPortal  = {3465.16f, -3940.45f, 308.788f};  // 181575
    constexpr WingPortal kLoathebPortal  = {2909.0f, -4025.02f, 273.475f};   // 181577
    constexpr WingPortal kThaddiusPortal = {3539.02f, -2936.82f, 302.476f};  // 181576
    constexpr WingPortal kHorsemenPortal = {2493.02f, -2921.78f, 241.193f};  // 181578

    // How close the leader must be to a wing portal for its event to arm. Each
    // wing boss dies within ~50yd of its portal (Maexxna's web, the Horsemen's
    // corners), and nothing else on the route comes this close to a wing's end.
    constexpr float kPortalArmRange = 80.0f;
    // The checkpoint reach. Maexxna's portal hangs ~9yd above her floor.
    constexpr float kPortalReach = 15.0f;

    // The four zone-in points (areatrigger_teleport 5191-5194) ring the hub
    // landing ~14yd out and 10yd below it, so both ranges take them in.
    constexpr float kHubArmRange = 30.0f;
    constexpr float kHubReach    = 20.0f;

    constexpr uint32 kEventMaexxnaPortal    = 1;
    constexpr uint32 kEventLoathebPortal    = 2;
    constexpr uint32 kEventThaddiusPortal   = 3;
    constexpr uint32 kEventHorsemenPortal   = 4;  // 10/25-man
    constexpr uint32 kEventHorsemenPortal40 = 5;  // IP 40-man
    constexpr uint32 kEventFrostwyrmLair    = 6;

    bool BossDone(InstanceScript* inst, uint32 bossId)
    {
        return inst->GetBossState(bossId) == DONE;
    }

    EventCondition WingPortalDue(uint32 bossId, WingPortal portal)
    {
        return [bossId, portal](Player* bot, AiObjectContext* /*context*/)
        {
            InstanceScript* inst = DcTargeting::GetInstanceScript(bot);
            return inst && BossDone(inst, bossId) &&
                   bot->GetDistance(portal.x, portal.y, portal.z) <= kPortalArmRange;
        };
    }

    bool FrostwyrmLairDue(Player* bot, AiObjectContext* /*context*/)
    {
        InstanceScript* inst = DcTargeting::GetInstanceScript(bot);
        return inst && BossDone(inst, kBossMaexxna) && BossDone(inst, kBossLoatheb) &&
               BossDone(inst, kBossThaddius) && BossDone(inst, kBossHorsemen) &&
               !BossDone(inst, kBossKelThuzad) &&
               bot->GetDistance(kHubX, kHubY, kHubZ) <= kHubArmRange;
    }

    DungeonEvent WingPortalEvent(uint32 id, char const* name, uint32 bossId, WingPortal portal,
                                 uint32 wingBoss)
    {
        return EventBuilder(kMapId, id, name)
            .Conditional(WingPortalDue(bossId, portal))
            .PanelAfterBoss(wingBoss)
            .TeleportParty(portal.x, portal.y, portal.z, kHubX, kHubY, kHubZ, kPortalReach)
            .Build();
    }
}

void RegisterNaxxramasEvents(std::vector<DungeonEvent>& out)
{
    out.push_back(WingPortalEvent(kEventMaexxnaPortal, "Take the Arachnid Quarter portal",
                                  kBossMaexxna, kMaexxnaPortal, kMaexxna));
    out.push_back(WingPortalEvent(kEventLoathebPortal, "Take the Plague Quarter portal",
                                  kBossLoatheb, kLoathebPortal, kLoatheb));
    out.push_back(WingPortalEvent(kEventThaddiusPortal, "Take the Construct Quarter portal",
                                  kBossThaddius, kThaddiusPortal, kThaddius));

    // The Horsemen's portal twice, once per roster anchor, so the `dc bosses`
    // panel lists it after the horseman that tier actually has (the panel hides
    // an event whose boss is missing from the roster).
    DungeonEvent horsemen = WingPortalEvent(kEventHorsemenPortal, "Take the Military Quarter portal",
                                            kBossHorsemen, kHorsemenPortal, kBaronRivendare);
    horsemen.gate = DcDifficultyGate::NormalOnly;
    out.push_back(std::move(horsemen));

    DungeonEvent horsemen40 = WingPortalEvent(kEventHorsemenPortal40, "Take the Military Quarter portal",
                                              kBossHorsemen, kHorsemenPortal, kHighlordMograine);
    horsemen40.gate = DcDifficultyGate::HeroicOnly;
    out.push_back(std::move(horsemen40));

    // Listed after Thaddius (DBC bit 12), i.e. just before Sapphiron (13), whichever
    // wing the raid finishes last.
    out.push_back(EventBuilder(kMapId, kEventFrostwyrmLair, "Enter the Frostwyrm Lair")
                      .Conditional(&FrostwyrmLairDue)
                      .Repeatable()
                      .PanelAfterBoss(kThaddius)
                      .TeleportParty(kHubX, kHubY, kHubZ, kLairX, kLairY, kLairZ, kHubReach)
                      .Build());
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
