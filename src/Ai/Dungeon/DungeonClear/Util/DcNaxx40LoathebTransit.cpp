/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, or later.
 */

#include "DcNaxx40LoathebTransit.h"

#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/DcPullContext.h"
#include "Ai/Raid/Naxx40/Naxx40Encounter.h"
#include "AllMapScript.h"
#include "DcCombatFlag.h"
#include "DcLeaderSignal.h"
#include "DcModuleEnable.h"
#include "DcMovement.h"
#include "DcRun.h"
#include "Group.h"
#include "InstanceScript.h"
#include "Log.h"
#include "MapReference.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "ObjectAccessor.h"
#include "PathGenerator.h"
#include "Player.h"
#include "Playerbots.h"
#include "Timer.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace DcNaxx40LoathebTransit
{
namespace
{
    constexpr uint32 Loatheb = 351020;
    constexpr uint32 HeiganEncounter = 4;
    constexpr uint32 LoathebEncounter = 5;
    constexpr uint32 UpdateInterval = 400;
    constexpr uint32 MoveRetry = 1600;
    constexpr uint32 TransitTimeout = 45000;
    constexpr uint32 CampTimeout = 75000;
    constexpr float CampRadius = 7.0f;
    constexpr float StartRadius = 28.0f;

    // The Detour funnel has two turns. This chain identifies the corridor;
    // each character uses the navmesh from its actual position.
    std::array<G3D::Vector3, 5> const Corridor{{
        {2769.389f, -3744.7686f, 273.6191f},
        {2792.267f, -3748.533f, 273.691f},
        {2823.733f, -3756.000f, 273.691f},
        {2849.067f, -3780.267f, 273.691f},
        {2909.766f, -3793.4153f, 273.58624f},
    }};
    using Transit = DcRunState::LoathebTransit;

    G3D::Vector3 Location(Player const& player)
    {
        return {player.GetPositionX(), player.GetPositionY(), player.GetPositionZ()};
    }

    bool InCorridor(Player const& player)
    {
        G3D::Vector3 const point = Location(player);
        for (std::size_t i = 1; i < Corridor.size(); ++i)
        {
            G3D::Vector3 const leg = Corridor[i] - Corridor[i - 1];
            G3D::Vector3 const offset = point - Corridor[i - 1];
            float const fraction = std::clamp(offset.dot(leg) / leg.squaredMagnitude(), 0.0f, 1.0f);
            if ((offset - leg * fraction).squaredMagnitude() <= 14.0f * 14.0f)
                return true;
        }
        return false;
    }

    bool AtCamp(Player const& player)
    {
        return (Location(player) - Corridor.back()).squaredMagnitude() <= CampRadius * CampRadius;
    }

    bool NearEntrance(Player const& player)
    {
        G3D::Vector3 const offset = Location(player) - Corridor.front();
        return std::abs(offset.z) <= 4.0f && offset.squaredMagnitude() <= StartRadius * StartRadius;
    }

    bool CanTransit(Player* player)
    {
        if (!player || !player->IsAlive() || !player->GetGroup() || !IsNaxx40Map(player->FindMap()))
            return false;
        InstanceScript const* instance = player->GetInstanceScript();
        return instance && instance->IsBossDone(HeiganEncounter) && !instance->IsBossDone(LoathebEncounter) &&
            !instance->IsEncounterInProgress();
    }

    bool HasCampGlide(Player& member)
    {
        return DcMovement::OwnsRunningEscort(&member) && member.movespline &&
            (member.movespline->FinalDestination() - Corridor.back()).squaredMagnitude() <= 4.0f;
    }

    void MoveToCamp(PlayerbotAI* ai, Player& member, uint32 now)
    {
        DcRunState& local = DcRun::Of(ai);
        uint32& lastAttempt = local.naxx40LoathebMoveAttempt;
        if (HasCampGlide(member) || member.IsNonMeleeSpellCast(false) ||
            (lastAttempt && getMSTimeDiff(lastAttempt, now) < MoveRetry))
            return;
        lastAttempt = now;

        G3D::Vector3 const& camp = Corridor.back();
        PathGenerator path(&member);
        path.SetUseStraightPath(true);
        path.SetExcludeFlags(NAV_WATER | NAV_MAGMA | NAV_SLIME | NAV_GROUND_STEEP);
        if (!path.CalculatePath(camp.x, camp.y, camp.z) || path.GetPathType() != PATHFIND_NORMAL ||
            path.GetPath().empty() || (path.GetPath().back() - camp).squaredMagnitude() > 4.0f)
            return;

        Movement::PointsArray points = path.GetPath();
        if (points.size() == 2)
            points.insert(points.begin() + 1, (points.front() + points.back()) * 0.5f);
        if (DcMovement::SplinePath(ai, points, MovementPriority::MOVEMENT_COMBAT, FORCED_MOVEMENT_RUN))
            local.naxx40LoathebSpline = member.movespline->GetId();
    }

    Player* ActiveLeader(PlayerbotAI* ai);

    void StandDownPull(PlayerbotAI& ai)
    {
        AiObjectContext* context = ai.GetAiObjectContext();
        DcPullContext& pull = context->GetValue<DcPullContext&>(DcKey::PullContext)->Get();
        pull.Reset();
        // Reuse the pull governor's normal preference restoration at handoff.
        pull.eventForced = true;
        context->GetValue<bool>(DcKey::PullMode)->Set(false);
        DcLeaderSignal::SetLeaderDazeImmunity(ai.GetBot(), false);
    }

    void Release(Player& member, DcRunState& local)
    {
        if (local.naxx40LoathebSpline && HasCampGlide(member) && !AtCamp(member))
            DcMovement::StopBot(&member, DcMovement::Stop::Hold);
        if (local.naxx40LoathebSpline)
            DcMovement::ClearMovementWait(&member);
        local.naxx40LoathebTransitOwner.Clear();
        local.naxx40LoathebMoveAttempt = 0;
        local.naxx40LoathebSpline = 0;
        local.naxx40LoathebArrived = false;
    }

    void Drive(PlayerbotAI& leaderAI, Player& leader, Transit& transit, uint32 now)
    {
        if (transit.lastUpdate && getMSTimeDiff(transit.lastUpdate, now) < UpdateInterval)
            return;
        if (!transit.lastUpdate)
            transit.started = now;
        transit.lastUpdate = now;

        uint32 crossing = 0;
        uint32 arrived = 0;
        for (GroupReference* ref = leader.GetGroup()->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || !member->IsInMap(&leader) || !leader.InSamePhase(member) || !member->IsAlive())
                continue;
            PlayerbotAI* ai = GET_PLAYERBOT_AI(member);
            if (!ai)
                continue;

            DcRunState& local = DcRun::Of(ai);
            ObjectGuid& owner = local.naxx40LoathebTransitOwner;
            if (owner != leader.GetGUID() && !InCorridor(*member) && !NearEntrance(*member))
                continue;
            owner = leader.GetGUID();
            ++crossing;
            local.naxx40LoathebArrived |= AtCamp(*member);
            if (local.naxx40LoathebArrived)
                ++arrived;
            else
                MoveToCamp(ai, *member, now);
        }

        uint32 const elapsed = getMSTimeDiff(transit.started, now);
        bool const leaderArrived = DcRun::Of(&leaderAI).naxx40LoathebArrived;
        bool const regrouped = leaderArrived && crossing && arrived == crossing;
        bool const campBusy = leaderArrived && DcCombatFlag::AnyPartyEngagement(&leader);
        if ((!leaderArrived && elapsed < TransitTimeout) ||
            (leaderArrived && (!regrouped || campBusy) && elapsed < CampTimeout))
            return;

        StandDownPull(leaderAI);
        transit.finished = true;
        LOG_INFO("playerbots.dungeonclear",
            "[DC:{}] Naxx40 Loatheb transit {} after {:.1f}s, camp {}/{}, combat={} in instance {}",
            leader.GetName(), regrouped ? "regrouped" : "timed out",
            elapsed / 1000.0f, arrived, crossing, campBusy, transit.instanceId);
    }

    class LoathebTransitMapScript final : public AllMapScript
    {
    public:
        LoathebTransitMapScript() : AllMapScript("DungeonClearNaxx40LoathebTransit", {ALLMAPHOOK_ON_MAP_UPDATE}) {}

        void OnMapUpdate(Map* map, uint32) override
        {
            if (!IsNaxx40Map(map) || !DcModule::IsEnabled())
                return;

            for (auto const& ref : map->GetPlayers())
            {
                Player* leader = ref.GetSource();
                PlayerbotAI* ai = leader ? GET_PLAYERBOT_AI(leader) : nullptr;
                if (!ai)
                    continue;
                DcRunState& run = DcRun::Of(ai);
                if (!run.naxx40LoathebTransitOwner.IsEmpty() && !ActiveLeader(ai))
                    Release(*leader, run);
                if (!run.enabled || run.paused ||
                    (run.selectedBossEntry && run.selectedBossEntry != Loatheb) || !CanTransit(leader))
                    continue;

                Transit& transit = run.naxx40LoathebTransit;
                if (transit.instanceId != leader->GetInstanceId() || transit.group != leader->GetGroup()->GetGUID())
                    transit = {};
                if (transit.finished || (!transit.started && !NearEntrance(*leader)))
                    continue;

                uint32 const now = getMSTime();
                if (!transit.started)
                {
                    if (!InCorridor(*leader))
                        continue;
                    auto const next = ai->GetAiObjectContext()->GetValue<std::optional<DungeonBossInfo>>(
                        DcKey::NextDungeonBoss)->Get();
                    if (!next || next->entry != Loatheb)
                        continue;

                    transit.group = leader->GetGroup()->GetGUID();
                    transit.instanceId = leader->GetInstanceId();
                    transit.started = now;
                    StandDownPull(*ai);
                    LOG_INFO("playerbots.dungeonclear", "[DC:{}] Naxx40 Loatheb transit started in instance {}",
                        leader->GetName(), transit.instanceId);
                }
                Drive(*ai, *leader, transit, now);
            }
        }
    };

    Player* ActiveLeader(PlayerbotAI* ai)
    {
        Player* member = ai ? ai->GetBot() : nullptr;
        if (!member || !IsNaxx40Map(member->FindMap()))
            return nullptr;
        ObjectGuid const guid = DcRun::Of(ai).naxx40LoathebTransitOwner;
        Player* leader = guid.IsEmpty() ? nullptr : ObjectAccessor::GetPlayer(*member, guid);
        if (!leader || !member->IsInMap(leader) || !leader->InSamePhase(member) ||
            !member->GetGroup() || member->GetGroup() != leader->GetGroup())
            return nullptr;
        PlayerbotAI* leaderAI = GET_PLAYERBOT_AI(leader);
        if (!leaderAI || !CanTransit(leader))
            return nullptr;
        DcRunState const& run = DcRun::Of(leaderAI);
        Transit const& transit = run.naxx40LoathebTransit;
        return run.enabled && !run.paused && (!run.selectedBossEntry || run.selectedBossEntry == Loatheb) &&
            transit.started && !transit.finished &&
            transit.instanceId == member->GetInstanceId() && transit.group == member->GetGroup()->GetGUID()
            ? leader : nullptr;
    }
}

bool IsCrossing(PlayerbotAI* ai)
{
    return ActiveLeader(ai) && !DcRun::Of(ai).naxx40LoathebArrived && !AtCamp(*ai->GetBot());
}

bool HoldsProgression(PlayerbotAI* ai)
{
    Player* leader = ActiveLeader(ai);
    return leader && leader == ai->GetBot();
}

void RegisterMapUpdate()
{
    new LoathebTransitMapScript();
}
}
