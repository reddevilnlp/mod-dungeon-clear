/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, or later.
 */

#ifndef DUNGEONCLEAR_NAXX40_PATCHWERK_H
#define DUNGEONCLEAR_NAXX40_PATCHWERK_H

#include "Define.h"
#include "G3D/Vector3.h"
#include "ObjectGuid.h"
#include "Position.h"

#include <optional>
#include <vector>

class AiObjectContext;
class Player;
class PlayerbotAI;
class Creature;
class Unit;
struct DcPullContext;
struct DungeonBossInfo;

namespace DcNaxx40Patchwerk
{
    G3D::Vector3 const& WhiteCampPosition();
    G3D::Vector3 const& WhiteCampFiringBank();

    enum class SweepResult
    {
        Inactive,
        Moving,
        Clearing,
        Waiting,
        Blocked,
    };

    bool BlocksBossPull(Player* tank, AiObjectContext* context, uint32 bossEntry);
    bool UsesPatrolCamp(Player* tank, AiObjectContext* context, uint32 bossEntry);
    void ObserveEntry(Player* tank, AiObjectContext* context, uint32 bossEntry);
    bool EntryPullDue(Player& tank, uint32 order);
    bool SetApproachAnchor(Player* tank, AiObjectContext* context, DungeonBossInfo& next);
    bool AtEmbalmingHandoff(Player* tank, AiObjectContext* context, uint32 bossEntry);
    bool DeferEmbalmingUntilLowerEast(Player* tank, AiObjectContext* context, uint32 bossEntry, Unit const* target);
    void NoteClearedLowerEast(Player* tank, AiObjectContext* context, uint32 bossEntry);
    Creature* FindEmbalmingPull(PlayerbotAI* ai, ObjectGuid excluded);
    std::optional<Position> SweepPullCamp(Player* tank, AiObjectContext* context, uint32 bossEntry);
    bool NoteSweepTagTimeout(Player* tank, AiObjectContext* context, uint32 bossEntry);
    bool ValidateSweepCamp(Player* tank, AiObjectContext* context, Position const& camp);
    bool DriveSweepReturn(PlayerbotAI* ai, Position const& camp);
    void HoldForEmbalming(PlayerbotAI* ai);
    bool AtPatrolCamp(Player* tank, AiObjectContext* context);
    bool IsPatrolPull(DcPullContext const& pull);
    bool IsPatrolManeuverActive(Player* bot);
    bool HasPatrolPosition(Player* bot);
    std::optional<Position> PatrolPosition(Player* bot);
    bool PatrolFormationReady(Player* tank);
    bool PatrolBossAtCamp(Unit const& boss);
    bool DrivePatrolReturn(PlayerbotAI* ai);
    bool BeginPatrolPull(PlayerbotAI* ai, AiObjectContext* context, Creature* boss);
    bool DrivePatrolTag(PlayerbotAI* ai, AiObjectContext* context);
    bool CanJoinGrobbulusCampRoute(Player const& tank);
    SweepResult DriveGrobbulusCamp(PlayerbotAI* ai);
    bool ShouldStart(Player* tank, AiObjectContext* context, uint32 bossEntry);
    bool SweepInProgress(Player* tank, AiObjectContext* context, uint32 bossEntry);
    bool HasRouteFollowerAnchor(Player* tank, AiObjectContext* context, uint32 bossEntry);
    bool DriveRouteFollower(PlayerbotAI* ai, Player* tank);
    std::vector<ObjectGuid> LocalTrash(Player* tank, AiObjectContext* context);
    SweepResult Drive(PlayerbotAI* ai, AiObjectContext* context, uint32 bossEntry);
}

#endif
