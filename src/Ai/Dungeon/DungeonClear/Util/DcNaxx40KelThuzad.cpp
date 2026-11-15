/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 */

#include "DcNaxx40KelThuzad.h"

#include <string>

#include "Ai/Dungeon/DungeonClear/Action/DcActionShared.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"
#include "Ai/Raid/Naxx40/Naxx40Encounter.h"
#include "DcLeaderSignal.h"
#include "DcMovement.h"
#include "DcNaxx40SlimeCrossing.h"
#include "DcRaidMuster.h"
#include "DcRezRecovery.h"
#include "DcRun.h"
#include "GameObject.h"
#include "Group.h"
#include "InstanceScript.h"
#include "Log.h"
#include "Map.h"
#include "MoveSpline.h"
#include "Player.h"
#include "Playerbots.h"

namespace
{
    constexpr uint32 BossEntry = 351019;
    constexpr uint32 SapphironEncounter = 13;
    constexpr uint32 KelThuzadEncounter = 14;
    constexpr uint32 GateData = 206;
    constexpr uint32 GateEntry = 181228;
    // 35yd past the gate, and 75yd from the boss (proximity aggro starts at ~61yd).
    Position const InsideCamp(3673.0f, -5099.3f, 142.032f);
    Position const PullPoint(3696.0f, -5104.13f, 142.030f);
    // Reach the inside of the gate on the generic corridor/breadcrumb path.
    // The custom dry leg starts no farther west than x=3647, past gate x=3637.4.
    constexpr float StagingRadius = 26.0f;
    constexpr float ArrivalRadius = 1.0f;
    constexpr float CampRadius = 8.0f;

    InstanceScript* OpeningInstance(Player const* player)
    {
        if (!player || !IsNaxx40Map(player->FindMap()) || player->IsInCombat() ||
            player->GetExactDist(&InsideCamp) > StagingRadius || player->GetPositionZ() < 138.0f ||
            player->GetPositionZ() > 149.0f)
            return nullptr;
        InstanceScript* instance = player->GetInstanceScript();
        return instance && instance->IsBossDone(SapphironEncounter) && instance->GetBossState(KelThuzadEncounter) == NOT_STARTED
            ? instance : nullptr;
    }

    bool GateOpen(InstanceScript& instance)
    {
        GameObject* gate = instance.GetGameObject(GateData);
        return gate && gate->GetEntry() == GateEntry && gate->GetGoState() == GO_STATE_ACTIVE;
    }

    bool LeaderStages(Player* leader)
    {
        if (!OpeningInstance(leader))
            return false;
        PlayerbotAI* ai = GET_PLAYERBOT_AI(leader);
        if (!ai || !DcRun::Of(ai).enabled || DcRun::Of(ai).paused)
            return false;
        auto const next =
            ai->GetAiObjectContext()->GetValue<std::optional<DungeonBossInfo>>(DcKey::NextDungeonBoss)->Get();
        return next && next->entry == BossEntry;
    }

    bool OnLeg(Player* player, Position const& end)
    {
        if (!DcMovement::OwnsRunningEscort(player))
            return false;
        G3D::Vector3 const goal = player->movespline->FinalDestination();
        return end.GetExactDist(goal.x, goal.y, goal.z) <= ArrivalRadius;
    }

    bool WalkDry(PlayerbotAI& ai, Position const& end)
    {
        Player* player = ai.GetBot();
        if (OnLeg(player, end))
            return true;
        if (!ai.CanMove() || player->IsFalling() || player->IsBeingTeleported())
            return false;
        auto path = DcNaxx40SlimeCrossing::BuildDryCampPath(player, end);
        if (path.empty())
            return false;
        for (std::size_t index = 1; index < path.size(); ++index)
        {
            auto const& from = path[index - 1];
            auto const& to = path[index];
            if (!player->GetMap()->isInLineOfSight(from.x, from.y, from.z + 0.85f, to.x, to.y, to.z + 0.85f,
                    player->GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing))
                return false;
        }
        DcMovement::ClearMovementWait(player);
        return DcMovement::SplinePath(&ai, path, MovementPriority::MOVEMENT_NORMAL, FORCED_MOVEMENT_RUN);
    }
}

bool DcNaxx40KelThuzad::IsObjective(Player const* player, uint32 entry)
{
    return player && entry == BossEntry && IsNaxx40Map(player->FindMap());
}

Position const& DcNaxx40KelThuzad::Camp()
{
    return InsideCamp;
}

std::optional<bool> DcNaxx40KelThuzad::Stage(PlayerbotAI& ai, DungeonBossInfo const& next)
{
    Player* leader = ai.GetBot();
    if (!IsObjective(leader, next.entry))
        return std::nullopt;
    InstanceScript* instance = OpeningInstance(leader);
    if (!instance)
        return std::nullopt;

    AiObjectContext* context = ai.GetAiObjectContext();
    if (!GateOpen(*instance))
    {
        DcMovement::StopBot(leader, DcMovement::Stop::Hold);
        DcActionShared::SetPhase(context, "Kel'Thuzad: waiting for the room gate");
        return false;
    }
    if (OnLeg(leader, PullPoint))
        return true;
    if (leader->GetExactDist(&InsideCamp) > ArrivalRadius)
    {
        if (WalkDry(ai, InsideCamp))
            return true;
        DcMovement::StopBot(leader, DcMovement::Stop::Hold);
        DcActionShared::StallDungeonClear(&ai, "Kel'Thuzad: waiting for a dry path through the gate");
        return false;
    }

    DcMovement::StopBot(leader, DcMovement::Stop::Hold);
    DcActionShared::ClearStall(context);
    if (DcRezRecovery::IsPending(leader))
        return false;
    Group* group = leader->GetGroup();
    if (!group)
        return false;
    uint32 staged = 0;
    uint32 livingBots = 0;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || !member->IsInWorld() || !member->IsAlive() || !GET_PLAYERBOT_AI(member))
            continue;
        ++livingBots;
        if (member->IsInMap(leader) && member->InSamePhase(leader) && !member->IsBeingTeleported() &&
            !member->IsFalling() && member->GetExactDist(&InsideCamp) <= CampRadius)
            ++staged;
    }
    if (staged != livingBots)
    {
        DcActionShared::SetPhase(context, "Kel'Thuzad: " + std::to_string(staged) +
            " of " + std::to_string(livingBots) + " bots inside the gate");
        return false;
    }
    if (DcRaidMuster::Holds(leader, &ai, context, next))
        return false;
    if (!WalkDry(ai, PullPoint))
        return false;
    DcActionShared::SetPhase(context, "Kel'Thuzad: pulling with the raid inside the gate");
    LOG_INFO("playerbots.dungeonclear", "[DC:{}] Kel'Thuzad opening: instance {} released with {}/{} living bots inside",
        leader->GetName(), leader->GetInstanceId(), staged, livingBots);
    return true;
}

std::optional<bool> DcNaxx40KelThuzad::StageFollower(PlayerbotAI& ai, Player* leader)
{
    Player* member = ai.GetBot();
    if (!member || !leader || member->IsInCombat() || !member->IsInMap(leader) ||
        member->GetExactDist(&InsideCamp) > StagingRadius || !LeaderStages(leader) || DcRezRecovery::IsPending(leader))
        return std::nullopt;
    InstanceScript* instance = leader->GetInstanceScript();
    if (!GateOpen(*instance) || member->GetExactDist(&InsideCamp) <= CampRadius)
    {
        DcMovement::StopBot(member, DcMovement::Stop::Hold);
        return false;
    }
    return WalkDry(ai, InsideCamp);
}

bool DcNaxx40KelThuzad::OwnsMovement(PlayerbotAI& ai)
{
    Player* player = ai.GetBot();
    if (!player || !IsNaxx40Map(player->FindMap()) || player->IsInCombat() ||
        player->GetExactDist(&InsideCamp) > StagingRadius)
        return false;
    Player* leader = DcLeaderSignal::FindRunOwner(player);
    return leader && LeaderStages(leader) && !DcRezRecovery::IsPending(leader);
}
