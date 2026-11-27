/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, or later.
 */

#ifndef DUNGEONCLEAR_NAXX40_SPIDER_QUARTER_H
#define DUNGEONCLEAR_NAXX40_SPIDER_QUARTER_H

#include "Define.h"
#include "MoveSplineInitArgs.h"
#include "ObjectGuid.h"

#include <string>
#include <vector>

class Player;
class Unit;
struct DcPullContext;
struct Position;
struct ScriptedPullStage;

namespace DcNaxx40SpiderQuarter
{
    inline constexpr uint32 Anub = 351009;
    inline constexpr uint32 Faerlina = 351007;
    inline constexpr uint32 Maexxna = 351006;
    inline constexpr float CampSlotLimit = 5.0f;

    struct CampPathCheck
    {
        std::string blocker;
        bool noCompletePath = false;
    };

    void AppendStages(std::vector<ScriptedPullStage>& stages);
    bool UsesApproach(Player const* tank, uint32 boss);
    bool IsStage(ScriptedPullStage const* stage);
    bool CampDeferredHere(Player const& tank, ScriptedPullStage const& stage, DcPullContext const& pull);
    bool HadBlockedCamp(Player const& tank, ScriptedPullStage const& stage, DcPullContext const& pull);
    void DeferCamp(Player const& tank, ScriptedPullStage const& stage, DcPullContext& pull);
    void ClearCampDeferral(ScriptedPullStage const& stage, DcPullContext& pull);
    bool ApproachReady(Player* tank, ScriptedPullStage const& stage);
    Unit* NearestPackMember(Player* tank, ScriptedPullStage const& stage, ObjectGuid exclude = ObjectGuid::Empty);
    Movement::PointsArray BuildPullPath(Player* tank, Position const& destination);
    CampPathCheck CheckCampPath(Player* tank, ScriptedPullStage const& stage, Position const& destination,
                                bool requireCompletePath = false);
    bool CampPetsReady(Player* tank, Position const& camp, float radius, Player const** waitingOwner = nullptr);
    bool PatrolClearForTag(Player* tank, ScriptedPullStage const& stage);
    bool PatrolClearOfStand(Player* tank, ScriptedPullStage const& stage, bool departing);
}

#endif
