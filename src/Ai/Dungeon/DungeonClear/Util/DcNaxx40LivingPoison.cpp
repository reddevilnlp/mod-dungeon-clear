/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, or later.
 */

#include "DcNaxx40LivingPoison.h"

#include "Ai/Dungeon/DungeonClear/Action/DcActionShared.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Data/ScriptedPullRegistry.h"
#include "Ai/Dungeon/DungeonClear/DcApproachState.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"
#include "Ai/Raid/Naxx40/Naxx40Encounter.h"
#include "Ai/Raid/Naxx40/Naxx40Slime.h"
#include "AllMapScript.h"
#include "ChunkedPathfinder.h"
#include "Creature.h"
#include "DcModuleEnable.h"
#include "DcMovement.h"
#include "DcNaxx40Patchwerk.h"
#include "DcNaxx40SlimeCrossing.h"
#include "DcPlayerbotCompat.h"
#include "DcRun.h"
#include "DcRunProgress.h"
#include "Group.h"
#include "InstanceScript.h"
#include "Log.h"
#include "Map.h"
#include "MapReference.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "MoveSplineInit.h"
#include "Naxx40LivingPoisonTiming.h"
#include "ObjectAccessor.h"
#include "PathGenerator.h"
#include "Player.h"
#include "Playerbots.h"
#include "StringFormat.h"
#include "Timer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <utility>

namespace DcNaxx40LivingPoison
{
namespace
{
    constexpr uint32 Grobbulus = GrobbulusEntry;
    constexpr float ArrivalRadius = 0.25f;
    constexpr float HoldClearance = 6.0f;
    constexpr float RetreatClearance = 2.5f;
    constexpr float MaximumFloorGap = 0.75f;
    constexpr float FloorProbeLift = 0.6f;
    constexpr float CrossingSpeed = 6.5f;
    constexpr uint32 AttemptInterval = 30;
    constexpr uint32 MaximumMapDiff = 60;
    constexpr uint32 NoProgressTimeout = 30000;
    constexpr uint32 StagingPauseMs = 350;
    constexpr std::size_t ReturnCrossing = 1;
    constexpr std::size_t WhiteCampGate = 2;
    constexpr std::size_t ApproachCrossing = 4;
    constexpr std::size_t FirstPoisonRefuge = 5;
    constexpr float BeyondRefugeMargin = 0.5f;
    constexpr float MaximumApproachSlime = 8.0f;

    std::array<G3D::Vector3, 12> const Stops{{
        {3107.1f, -3131.8f, 294.064f},
        DcNaxx40Patchwerk::WhiteCampFiringBank(),
        DcNaxx40Patchwerk::WhiteCampPosition(),
        DcNaxx40Patchwerk::WhiteCampFiringBank(),
        {3107.1f, -3131.8f, 294.064f},
        {3120.5198f, -3142.059f, 294.06287f},
        {3138.1973f, -3142.146f, 294.06287f},
        {3141.1973f, -3140.146f, 294.06287f},
        {3157.1396f, -3145.7231f, 294.06287f},
        {3161.1396f, -3141.7231f, 294.06287f},
        {3176.7861f, -3151.6646f, 294.06287f},
        {3179.7861f, -3154.6646f, 294.06287f},
    }};

    using GrobbulusStage = DcRunState::GrobbulusApproach::Stage;
    using DcNaxx40SlimeCrossing::GrobbulusBankA;
    using DcNaxx40SlimeCrossing::GrobbulusBankB;
    constexpr float GrobbulusArrival = 0.65f;
    constexpr std::size_t GrobbulusWaveSize = 4;
    constexpr uint32 GrobbulusUpdateInterval = 150;
    G3D::Vector3 const GrobbulusLanding{3318.366f, -3263.8232f, 292.595f};
    G3D::Vector3 const EastPullCamp{3288.0f, -3266.0f, 292.595f};
    G3D::Vector3 const GrobbulusRegroup{3322.8599f, -3285.572f, 292.59506f};
    std::array<G3D::Vector3, 7> const GrobbulusDryApproach{{
        {3200.0f, -3157.0f, 293.771f},
        {3220.0f, -3176.5f, 293.810f},
        {3240.0f, -3196.5f, 293.772f},
        {3260.0f, -3216.5f, 293.751f},
        {3280.0f, -3218.0f, 293.797f},
        {3300.0f, -3223.5f, 294.063f},
        GrobbulusBankA,
    }};

    std::array<std::pair<G3D::Vector3, G3D::Vector3>, 3> const Lanes{{
        {{3128.59f, -3118.81f, 293.346f}, {3130.322f, -3156.51f, 293.324f}},
        {{3154.25f, -3125.7f, 293.43f}, {3144.779f, -3158.416f, 293.324f}},
        {{3175.42f, -3134.86f, 293.34f}, {3158.778f, -3164.201f, 293.312f}},
    }};

    G3D::Vector3 Location(Player const* player)
    {
        return {player->GetPositionX(), player->GetPositionY(), player->GetPositionZ()};
    }

    bool OnSouthApproach(Player const* player)
    {
        G3D::Vector3 const point = Location(player);
        return point.x >= 3094.0f && point.x <= 3125.0f && point.y >= -3163.0f && point.y <= -3128.0f &&
            std::abs(point.z - Stops.front().z) < 1.5f;
    }

    bool At(Player const* player, G3D::Vector3 const& point)
    {
        return (Location(player) - point).squaredLength() <= ArrivalRadius * ArrivalRadius;
    }

    bool HeadingTo(Player const* player, G3D::Vector3 const& point)
    {
        return player->movespline && !player->movespline->Finalized() &&
            (player->movespline->FinalDestination() - point).squaredLength() <= ArrivalRadius * ArrivalRadius;
    }

    bool IsParked(Player const* player, G3D::Vector3 const& point)
    {
        return At(player, point) && !player->isMoving() && !HeadingTo(player, point);
    }

    bool CanMove(Player* player)
    {
        return !player->GetVehicle() && !player->GetTransport() && !player->IsRooted() && !player->IsFalling() &&
            !player->HasUnitState(UNIT_STATE_LOST_CONTROL | UNIT_STATE_NOT_MOVE) &&
            !player->HasUnitFlag(UNIT_FLAG_DISABLE_MOVE) && !player->IsMovementPreventedByCasting() &&
            !player->GetMotionMaster()->GetMotionSlot(MOTION_SLOT_CONTROLLED);
    }

    bool CanCrossTogether(Player* player)
    {
        return player->GetSpeed(MOVE_RUN) >= CrossingSpeed &&
            !player->HasUnitMovementFlag(MOVEMENTFLAG_CAN_FLY | MOVEMENTFLAG_DISABLE_GRAVITY |
                                         MOVEMENTFLAG_FLYING | MOVEMENTFLAG_SWIMMING);
    }

    bool LaunchCrossing(Player* player, Movement::PointsArray const& path)
    {
        player->GetMotionMaster()->Clear();
        if (player->IsSitState())
            player->SetStandState(UNIT_STAND_STATE_STAND);

        // Use the exact path and speed forecast for the entire leg, even if a speed buff expires.
        Movement::MoveSplineInit movement(player);
        movement.MovebyPath(path);
        movement.SetWalk(false);
        movement.SetVelocity(CrossingSpeed);
        return movement.Launch() > 0;
    }

    float LaneDistance(G3D::Vector3 const& point, G3D::Vector3 const& start, G3D::Vector3 const& end)
    {
        G3D::Vector3 const delta(end.x - start.x, end.y - start.y, 0.0f);
        G3D::Vector3 const offset(point.x - start.x, point.y - start.y, 0.0f);
        float const fraction = std::clamp(offset.dot(delta) / delta.squaredLength(), 0.0f, 1.0f);
        return (offset - delta * fraction).length();
    }

    bool ValidatePath(Player* player, Movement::PointsArray const& path, bool timed, bool approachCrossing = false)
    {
        Map* map = player->GetMap();
        uint32 const phase = player->GetPhaseMask();
        float const height = player->GetCollisionHeight();
        std::array<float, 3> clearance{};
        float wetLength = 0.0f;
        for (std::size_t lane = 0; lane < Lanes.size(); ++lane)
            clearance[lane] = std::min(HoldClearance,
                LaneDistance(path.front(), Lanes[lane].first, Lanes[lane].second));
        for (std::size_t index = 1; index < path.size(); ++index)
        {
            G3D::Vector3 const& start = path[index - 1];
            G3D::Vector3 const& end = path[index];
            if (!map->isInLineOfSight(start.x, start.y, start.z + 1.0f, end.x, end.y, end.z + 1.0f,
                                     phase, LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing))
                return false;

            G3D::Vector3 const offset = end - start;
            int const samples = std::max(1, static_cast<int>(std::ceil(offset.length() * 2.0f)));
            for (int sample = 0; sample <= samples; ++sample)
            {
                G3D::Vector3 const point = start + offset * (static_cast<float>(sample) / samples);
                PositionFullTerrainStatus terrain;
                map->GetFullTerrainStatusForPosition(phase, point.x, point.y, point.z, height, terrain);
                if (std::abs(terrain.floorZ - point.z) > MaximumFloorGap)
                {
                    map->GetFullTerrainStatusForPosition(phase, point.x, point.y, point.z + FloorProbeLift,
                                                         height, terrain);
                    if (std::abs(terrain.floorZ - point.z) > MaximumFloorGap)
                        return false;
                }
                LiquidData const& liquid = terrain.liquidInfo;
                if (liquid.Entry == Naxx40SlimeLiquidId &&
                    ((liquid.Status & MAP_LIQUID_STATUS_IN_CONTACT) || liquid.Level > terrain.floorZ))
                {
                    if (!approachCrossing || (index == 1 && sample == 0) ||
                        (index + 1 == path.size() && sample == samples))
                        return false;
                    wetLength += offset.length() / samples;
                    if (wetLength > MaximumApproachSlime)
                        return false;
                }
                if (timed)
                    continue;
                for (std::size_t lane = 0; lane < Lanes.size(); ++lane)
                {
                    float const distance = LaneDistance(point, Lanes[lane].first, Lanes[lane].second);
                    if (distance < RetreatClearance || distance + 0.01f < clearance[lane])
                        return false;
                    clearance[lane] = std::max(clearance[lane], std::min(distance, HoldClearance));
                }
            }
        }
        return true;
    }

    Movement::PointsArray BuildPath(Player* player, G3D::Vector3 const& target, bool joint)
    {
        G3D::Vector3 const start = Location(player);
        Movement::PointsArray direct{start, (start + target) * 0.5f, target};
        if (joint || ValidatePath(player, direct, false))
            return direct;

        PathGenerator path(player);
        path.SetUseStraightPath(true);
        path.SetPathLengthLimit(90.0f);
        path.SetNavTerrainCost(NAV_WATER, 25.0f);
        if (!path.CalculatePath(target.x, target.y, target.z) || path.GetPathType() != PATHFIND_NORMAL ||
            path.GetPath().size() < 2 || (path.GetActualEndPosition() - target).squaredLength() > 0.25f)
            return {};
        Movement::PointsArray points = path.GetPath();
        points.front() = start;
        points.back() = target;
        // Two-point escorts regenerate their path in the core. Keep the checked geometry.
        if (points.size() == 2)
            points.insert(points.begin() + 1, (start + target) * 0.5f);
        return ValidatePath(player, points, false) ? points : Movement::PointsArray{};
    }

    void Hold(Player* player)
    {
        LiquidData const& liquid = player->GetLiquidData();
        if (CanMove(player) && !(liquid.Entry == Naxx40SlimeLiquidId && (liquid.Status & MAP_LIQUID_STATUS_SWIMMING)))
            DcMovement::StopBot(player, DcMovement::Stop::Hold);
    }

    Player* GetMember(Player* owner, ObjectGuid guid)
    {
        Player* member = ObjectAccessor::GetPlayer(*owner, guid);
        if (!member || !member->IsAlive() || !member->IsInMap(owner) || member->GetGroup() != owner->GetGroup() ||
            !owner->InSamePhase(member) || !GET_PLAYERBOT_AI(member))
            return nullptr;
        return member;
    }

    bool Near(Player const* member, G3D::Vector3 const& point, float radius = GrobbulusArrival)
    {
        return (Location(member) - point).squaredLength() <= radius * radius;
    }

    bool OnFarBank(Player const* member)
    {
        return member->GetPositionX() >= EastPullCamp.x - 8.0f &&
            member->GetPositionX() <= GrobbulusRegroup.x + 8.0f &&
            member->GetPositionY() <= GrobbulusBankB.y + GrobbulusArrival &&
            member->GetPositionY() >= GrobbulusRegroup.y - 8.0f &&
            std::abs(member->GetPositionZ() - GrobbulusLanding.z) < 2.0f &&
            DcNaxx40SlimeCrossing::IsDryCampGround(*member, Location(member));
    }

    bool AtDryStop(Player* member, G3D::Vector3 const& target, float radius = GrobbulusArrival)
    {
        return Near(member, target, radius) && !member->isMoving() &&
            (!member->movespline || member->movespline->Finalized()) &&
            DcNaxx40SlimeCrossing::IsDryCampGround(*member, Location(member));
    }

    bool HasClearedLanding(Player const* member)
    {
        return OnFarBank(member) && member->GetPositionY() <= GrobbulusLanding.y + GrobbulusArrival &&
            (!member->movespline || member->movespline->Finalized());
    }

    // A declared checkpoint is a short, surveyed dry leg. If the member drifted
    // off that line, use a checked navmesh path from its actual position.
    void MoveToDryStop(Player* member, G3D::Vector3 const& target, float radius = GrobbulusArrival)
    {
        if (AtDryStop(member, target, radius))
        {
            Hold(member);
            return;
        }
        if (!CanMove(member) || member->IsNonMeleeSpellCast(true) || member->IsInCombat() ||
            (member->movespline && !member->movespline->Finalized() &&
             (member->movespline->FinalDestination() - target).squaredLength() < radius * radius))
            return;

        auto path = DcNaxx40SlimeCrossing::BuildSurveyedDryLeg(member, target);
        if (path.empty())
            path = DcNaxx40SlimeCrossing::BuildDryCampPath(member, Position(target.x, target.y, target.z));
        if (!path.empty())
            DcMovement::SplinePath(GET_PLAYERBOT_AI(member), path);
    }

    bool EastSpewersAlive(Player* tank)
    {
        Map* const map = tank->GetMap();
        for (float const y : {-3322.79f, -3299.13f})
            if (!map->IsGridLoaded(3331.0f, y))
                map->LoadGrid(3331.0f, y);
        auto const& spawns = map->GetCreatureBySpawnIdStore();
        for (ObjectGuid::LowType const spawn : {361185, 361186})
        {
            auto const [first, last] = spawns.equal_range(spawn);
            for (auto it = first; it != last; ++it)
                if (Creature const* mob = it->second; mob->IsInWorld() && mob->IsAlive() && tank->InSamePhase(mob))
                    return true;
        }
        return false;
    }

    bool CanRejoinGrobbulusFromFarBank(Player* member)
    {
        if (!CanMove(member) || IsCrossingLane(member) || DcNaxx40SlimeCrossing::CrossingActive(member))
            return false;

        G3D::Vector3 const point = Location(member);
        if (std::abs(point.z - GrobbulusRegroup.z) > 0.6f)
            return false;
        if (OnFarBank(member))
            return point.y <= GrobbulusLanding.y + GrobbulusArrival;

        if (point.x < EastPullCamp.x - 8.0f || point.x > GrobbulusRegroup.x + 8.0f ||
            point.y < GrobbulusRegroup.y - 20.0f || point.y >= GrobbulusRegroup.y - 8.0f)
            return false;

        // An earlier engage approach can run past point 1 on this floor.
        // Admit that position only with a surveyed dry way back to point 1.
        return !DcNaxx40SlimeCrossing::BuildSurveyedDryLeg(member, GrobbulusRegroup).empty();
    }

    void ChangeGrobbulusStage(DcRunState::GrobbulusApproach& leg, GrobbulusStage stage, uint32 now)
    {
        leg.stage = stage;
        leg.arrived = 0;
        leg.lastProgress = now;
    }

    void UpdateGrobbulusApproach(PlayerbotAI* ai, DcRunState& run)
    {
        auto& leg = run.naxx40GrobbulusApproach;
        Player* const owner = ai->GetBot();
        InstanceScript const* instance = owner->GetInstanceScript();
        uint32 const now = getMSTime();
        if (run.paused || !owner->IsAlive() || !owner->GetGroup() || !instance ||
            instance->IsEncounterInProgress())
        {
            leg.lastProgress = now;
            return;
        }

        DcRunProgress::Stamp(run.progress, now);
        if (getMSTimeDiff(leg.lastAttempt, now) < GrobbulusUpdateInterval)
            return;
        leg.lastAttempt = now;
        std::vector<Player*> members;
        members.reserve(leg.members.size());
        bool combat = false;
        for (ObjectGuid const guid : leg.members)
            if (Player* member = GetMember(owner, guid))
            {
                members.push_back(member);
                combat = combat || member->IsInCombat();
            }
        if (combat)
        {
            leg.lastProgress = now;
            return;
        }

        AiObjectContext* const context = ai->GetAiObjectContext();
        if (DcActionShared::PullOwnsTheTank(owner, context, "Grobbulus approach"))
        {
            leg.lastProgress = now;
            return;
        }

        auto setPhase = [&](char const* phase) { DcActionShared::SetPhase(context, phase); };
        uint32 const timeout = leg.stage == GrobbulusStage::Pull
            ? DC_SCRIPTED_PULL_MUSTER_MS + NoProgressTimeout
            : NoProgressTimeout;
        if (getMSTimeDiff(leg.lastProgress, now) >= timeout)
        {
            run.paused = true;
            run.pauseReason = "Grobbulus: check the door, east pull or missing bots, then resume";
            setPhase("waiting on the Grobbulus approach");
            LOG_WARN("playerbots.dungeonclear", "[DC:{}] Grobbulus approach blocked at stage {}; {}/{} present, {} arrived",
                     owner->GetName(), static_cast<uint32>(leg.stage), members.size(), leg.members.size(), leg.arrived);
            if (leg.stage == GrobbulusStage::Muster || leg.stage == GrobbulusStage::Regroup)
            {
                auto const& target = leg.stage == GrobbulusStage::Muster ? EastPullCamp : GrobbulusRegroup;
                for (Player* member : members)
                {
                    if (AtDryStop(member, target, 4.0f))
                        continue;
                    LOG_WARN("playerbots.dungeonclear",
                        "[DC:{}] Grobbulus: {} still at ({:.3f},{:.3f},{:.3f}), floor {:.3f}, "
                        "falling={}, moving={}, controlled={}, selfbot={}",
                        owner->GetName(), member->GetName(), member->GetPositionX(), member->GetPositionY(),
                        member->GetPositionZ(), member->GetFloorZ(), member->IsFalling(), member->isMoving(),
                        static_cast<uint32>(member->GetMotionMaster()->GetMotionSlotType(MOTION_SLOT_CONTROLLED)),
                        DcPlayerbotCompat::IsSelfBot(member));
                }
            }
            return;
        }
        if (members.size() != leg.members.size())
            return;

        auto countArrival = [&](std::size_t count)
        {
            if (count > leg.arrived)
            {
                leg.arrived = count;
                leg.lastProgress = now;
            }
            return count == leg.members.size();
        };
        auto moveGroup = [&](G3D::Vector3 const& target, float radius)
        {
            std::size_t arrived = 0;
            for (Player* member : members)
            {
                if (AtDryStop(member, target, radius))
                {
                    Hold(member);
                    ++arrived;
                }
                else
                    MoveToDryStop(member, target, radius);
            }
            return countArrival(arrived);
        };

        switch (leg.stage)
        {
            case GrobbulusStage::Approach:
                setPhase("moving to the Grobbulus slime bank");
                if (moveGroup(GrobbulusDryApproach[leg.checkpoint], GrobbulusArrival))
                {
                    leg.arrived = 0;
                    if (++leg.checkpoint == GrobbulusDryApproach.size())
                        ChangeGrobbulusStage(leg, GrobbulusStage::Crossing, now);
                }
                break;
            case GrobbulusStage::Crossing:
            {
                setPhase("crossing the Grobbulus slime in waves");
                std::size_t cleared = 0;
                for (Player* member : members)
                {
                    if (IsCrossingLane(member))
                        continue;
                    if (HasClearedLanding(member) ||
                        (!OnFarBank(member) && member->movespline->Finalized() && CanRejoinGrobbulusFromFarBank(member)))
                        ++cleared;
                    else if (OnFarBank(member) && member->GetPositionY() > GrobbulusLanding.y + GrobbulusArrival)
                        MoveToDryStop(member, GrobbulusLanding);
                }
                bool const allCrossed = countArrival(cleared);
                if (std::ranges::any_of(leg.wave, [&](ObjectGuid guid)
                    {
                        Player* member = GetMember(owner, guid);
                        return !member || IsCrossingLane(member) ||
                            !DcNaxx40SlimeCrossing::IsDryCampGround(*member, Location(member)) ||
                            (OnFarBank(member) && !HasClearedLanding(member));
                    }))
                    break;
                for (ObjectGuid const guid : leg.wave)
                {
                    Player* member = GetMember(owner, guid);
                    LOG_INFO("playerbots.dungeonclear",
                             "[DC:{}] Grobbulus: A-B wave {} for {} ended at {} (HP {}/{})",
                             owner->GetName(), leg.waves, member->GetName(),
                             HasClearedLanding(member) ? "far landing" : "dry ground before landing",
                             member->GetHealth(), member->GetMaxHealth());
                }
                leg.wave.clear();
                if (allCrossed)
                {
                    LOG_INFO("playerbots.dungeonclear", "[DC:{}] Grobbulus: {}/{} reached dry landing in {} waves",
                             owner->GetName(), cleared, leg.members.size(), leg.waves);
                    ChangeGrobbulusStage(leg,
                        EastSpewersAlive(owner) ? GrobbulusStage::Muster : GrobbulusStage::Regroup, now);
                    break;
                }
                for (Player* member : members)
                {
                    if (OnFarBank(member))
                        continue;
                    if (!AtDryStop(member, GrobbulusBankA))
                    {
                        if (Near(member, GrobbulusBankA, 6.0f))
                            MoveToDryStop(member, GrobbulusBankA);
                        continue;
                    }
                    PlayerbotAI* const memberAI = GET_PLAYERBOT_AI(member);
                    if (!CanMove(member) || member->IsNonMeleeSpellCast(true) ||
                        !DcNaxx40SlimeCrossing::TryGrobbulusCrossing(memberAI))
                        continue;
                    DcRunState& memberRun = DcRun::Of(memberAI);
                    memberRun.naxx40PoisonSpline = member->movespline->GetId();
                    memberRun.naxx40PoisonInstance = member->GetInstanceId();
                    leg.wave.push_back(member->GetGUID());
                    LOG_INFO("playerbots.dungeonclear",
                             "[DC:{}] Grobbulus: A-B wave {} for {} "
                             "(selfbot={}, spline={}, run={:.2f}, swim={:.2f}, HP {}/{})",
                             owner->GetName(), leg.waves + 1, member->GetName(), DcPlayerbotCompat::IsSelfBot(member),
                             memberRun.naxx40PoisonSpline, member->GetSpeed(MOVE_RUN), member->GetSpeed(MOVE_SWIM),
                             member->GetHealth(), member->GetMaxHealth());
                    if (leg.wave.size() == GrobbulusWaveSize)
                        break;
                }
                if (!leg.wave.empty())
                {
                    ++leg.waves;
                    LOG_INFO("playerbots.dungeonclear", "[DC:{}] Grobbulus: wave {} launched with {} bots ({}/{} across)",
                             owner->GetName(), leg.waves, leg.wave.size(), cleared, leg.members.size());
                }
                break;
            }
            case GrobbulusStage::Muster:
                setPhase("mustering on dry ground for the east Spewers");
                if (moveGroup(EastPullCamp, 4.0f))
                    ChangeGrobbulusStage(leg, GrobbulusStage::Pull, now);
                break;
            case GrobbulusStage::Pull:
                if (!EastSpewersAlive(owner))
                {
                    LOG_INFO("playerbots.dungeonclear", "[DC:{}] Grobbulus: east Spewers clear (361185, 361186)",
                             owner->GetName());
                    ChangeGrobbulusStage(leg, GrobbulusStage::Regroup, now);
                }
                break;
            case GrobbulusStage::Regroup:
                setPhase("regrouping at Grobbulus point 1");
                if (moveGroup(GrobbulusRegroup, 4.0f))
                {
                    LOG_INFO("playerbots.dungeonclear", "[DC:{}] Grobbulus: {}/{} bots regrouped at point 1",
                             owner->GetName(), members.size(), leg.members.size());
                    ChangeGrobbulusStage(leg, GrobbulusStage::Complete, now);
                    context->GetValue<ChunkedPathfinder::Result&>(DcKey::LongPath)->Reset();
                    context->GetValue<DcApproachState&>(DcKey::ApproachState)->Get().Reset();
                    DcActionShared::ClearStall(context);
                    setPhase("moving");
                }
                break;
            case GrobbulusStage::Complete:
                break;
        }
    }

    bool BeginGrobbulusApproach(PlayerbotAI* ai, DcRunState& run)
    {
        Player* const owner = ai->GetBot();
        uint32 const instanceId = owner->GetInstanceId();
        auto& leg = run.naxx40GrobbulusApproach;
        if (leg.instanceId == instanceId)
            return leg.stage != GrobbulusStage::Complete;

        bool const fromRefuge = Near(owner, Stops.back(), 6.0f);
        bool const ownerOnFarBank = !fromRefuge && CanRejoinGrobbulusFromFarBank(owner);
        bool const atCrossing = Near(owner, GrobbulusBankA, 5.0f) || Near(owner, GrobbulusBankB, 5.0f) ||
            Near(owner, GrobbulusLanding, 8.0f) || Near(owner, EastPullCamp, 6.0f) ||
            Near(owner, GrobbulusRegroup, 6.0f);
        if (!fromRefuge && !atCrossing && !ownerOnFarBank)
            return false;

        leg = {};
        leg.instanceId = instanceId;
        leg.stage = fromRefuge ? GrobbulusStage::Approach : GrobbulusStage::Crossing;
        leg.lastProgress = getMSTime();
        bool allOnFarBank = ownerOnFarBank;
        for (GroupReference* ref = owner->GetGroup()->GetFirstMember(); ref; ref = ref->next())
        {
            Player* const candidate = ref->GetSource();
            Player* member = candidate ? GetMember(owner, candidate->GetGUID()) : nullptr;
            if (!member)
                continue;
            PlayerbotAI* const memberAI = GET_PLAYERBOT_AI(member);
            DcRun::Of(memberAI).naxx40PoisonOwner = owner->GetGUID();
            leg.members.push_back(member->GetGUID());
            if (fromRefuge || Near(member, GrobbulusBankA, 5.0f))
                Hold(member);
            else if (!member->IsInCombat() && CanMove(member) &&
                     !IsCrossingLane(member) && !DcNaxx40SlimeCrossing::CrossingActive(member))
                DcMovement::ResolveEscortConflict(member);
            if (allOnFarBank && member != owner && !CanRejoinGrobbulusFromFarBank(member))
                allOnFarBank = false;
        }
        if (leg.members.empty())
        {
            leg = {};
            return false;
        }
        if (allOnFarBank)
        {
            leg.stage = EastSpewersAlive(owner) ? GrobbulusStage::Muster : GrobbulusStage::Regroup;
            LOG_INFO("playerbots.dungeonclear", "[DC:{}] Grobbulus: resuming on the dry side with {} bots ({})",
                     owner->GetName(), leg.members.size(),
                     leg.stage == GrobbulusStage::Muster ? "east pull camp" : "point 1");
            return true;
        }

        // Get a tank and healer through first, especially when the run owner
        // is a different tank from the group's designated main tank.
        auto priority = [&](ObjectGuid guid)
        {
            Player* member = GetMember(owner, guid);
            if (PlayerbotAI::IsMainTank(member))
                return 0;
            if (member == owner)
                return 1;
            if (PlayerbotAI::IsHeal(member))
                return 2;
            return 3;
        };
        std::stable_sort(leg.members.begin(), leg.members.end(),
                         [&](ObjectGuid left, ObjectGuid right) { return priority(left) < priority(right); });
        LOG_INFO("playerbots.dungeonclear", "[DC:{}] Grobbulus: preparing A-B waves for {} bots (instance {})",
                 owner->GetName(), leg.members.size(), instanceId);
        return true;
    }

    void WaitForManualCrossing(PlayerbotAI* ai, DcRunState& run, char const* reason)
    {
        run.paused = true;
        run.pauseReason = reason;
        DcActionShared::SetPhase(ai->GetAiObjectContext(), "waiting at Living Poison");
        LOG_WARN("playerbots.dungeonclear", "[DC:{}] {}", ai->GetBot()->GetName(), run.pauseReason);
    }

    void Finish(PlayerbotAI* ai, DcRunState& run)
    {
        run.naxx40Poison = {};
        run.naxx40PoisonCompletedInstance = ai->GetBot()->GetInstanceId();
        AiObjectContext* context = ai->GetAiObjectContext();
        context->GetValue<ChunkedPathfinder::Result&>(DcKey::LongPath)->Reset();
        context->GetValue<DcApproachState&>(DcKey::ApproachState)->Get().Reset();
        DcActionShared::ClearStall(context);
        DcActionShared::SetPhase(context, "moving");
        LOG_INFO("playerbots.dungeonclear", "[DC:{}] Living Poison crossing complete", ai->GetBot()->GetName());
        BeginGrobbulusApproach(ai, run);
    }

    void UpdateCrossing(PlayerbotAI* ai, uint32 diff)
    {
        Player* owner = ai->GetBot();
        DcRunState& run = DcRun::Of(ai);
        auto& crossing = run.naxx40Poison;
        if (!run.enabled || crossing.instanceId != owner->GetInstanceId())
            return;

        uint32 const now = getMSTime();
        InstanceScript const* instance = owner->GetInstanceScript();
        if (run.paused || !owner->IsAlive() || !owner->GetGroup() || !instance || instance->IsEncounterInProgress())
        {
            crossing.lastProgress = now;
            return;
        }

        DcRunProgress::Stamp(run.progress, now);
        std::erase_if(crossing.members, [&](ObjectGuid guid) { return !GetMember(owner, guid); });
        if (crossing.members.empty())
        {
            Finish(ai, run);
            return;
        }

        G3D::Vector3 const& target = Stops[crossing.nextGate];
        bool const approachCrossing = crossing.nextGate == ApproachCrossing || crossing.nextGate == ReturnCrossing;
        bool const timed = crossing.nextGate > FirstPoisonRefuge &&
            (crossing.nextGate - FirstPoisonRefuge) % 2 == 1;
        bool const joint = timed || approachCrossing;
        std::size_t arrived = 0;
        bool combat = false;
        for (ObjectGuid const guid : crossing.members)
        {
            Player* member = GetMember(owner, guid);
            if (!member)
                continue;
            if (!member->IsInCombat() && At(member, target) && !HeadingTo(member, target))
                Hold(member);
            if (IsParked(member, target))
                ++arrived;
            combat = combat || member->IsInCombat();
        }
        if (combat)
        {
            crossing.lastProgress = now;
            return;
        }

        if (arrived != crossing.arrived)
        {
            crossing.lastProgress = now;
            crossing.arrived = arrived;
        }
        if (arrived == crossing.members.size())
        {
            if (crossing.nextGate >= FirstPoisonRefuge && !timed)
                LOG_INFO("playerbots.dungeonclear", "[DC:{}] Living Poison: {} bots stopped together at refuge {}",
                         owner->GetName(), arrived, (crossing.nextGate - FirstPoisonRefuge) / 2 + 1);
            else if (crossing.nextGate == WhiteCampGate)
                LOG_INFO("playerbots.dungeonclear", "[DC:{}] Living Poison: {} bots regrouped at the Patchwerk camp",
                         owner->GetName(), arrived);
            crossing.stagedAt = joint ? 0 : now;
            crossing.arrived = 0;
            crossing.crossingLane = false;
            crossing.waitingFor.clear();
            crossing.lastProgress = now;
            if (++crossing.nextGate == Stops.size())
                Finish(ai, run);
            return;
        }

        if (now - crossing.lastProgress >= NoProgressTimeout)
        {
            LOG_WARN("playerbots.dungeonclear", "[DC:{}] Living Poison: checkpoint {} arrived {}/{}; {}",
                     owner->GetName(), crossing.nextGate, arrived, crossing.members.size(),
                     crossing.waitingFor.empty() ? "waiting for movement to settle" : crossing.waitingFor);
            WaitForManualCrossing(ai, run, timed
                ? "Living Poison: no safe crossing for the whole raid. Resume to retry or use dc off to take over"
                : "Living Poison: the raid has not reached the safe point. Resume to retry or use dc off to take over");
            return;
        }

        if (joint && (crossing.crossingLane || getMSTimeDiff(crossing.stagedAt, now) < StagingPauseMs))
            return;
        if (now - crossing.lastAttempt < AttemptInterval)
            return;
        crossing.lastAttempt = now;
        crossing.waitingFor.clear();

        auto noteWait = [&](Player const& member, char const* reason)
        {
            if (crossing.waitingFor.empty())
                crossing.waitingFor = Acore::StringFormat("{} at ({:.2f}, {:.2f}, {:.2f}): {}", member.GetName(),
                    member.GetPositionX(), member.GetPositionY(), member.GetPositionZ(), reason);
        };

        struct Departure
        {
            PlayerbotAI* memberAI;
            Movement::PointsArray path;
        };
        std::vector<Departure> departures;
        for (ObjectGuid const guid : crossing.members)
        {
            Player* member = GetMember(owner, guid);
            if (!member)
                continue;
            if (!joint && (IsParked(member, target) || HeadingTo(member, target)))
                continue;
            if (timed && diff > MaximumMapDiff)
            {
                crossing.waitingFor = "map update exceeded the timing margin";
                return;
            }
            if (!CanMove(member) || (joint && !IsParked(member, Stops[crossing.nextGate - 1])))
            {
                noteWait(*member, "unable to move or still settling at the refuge");
                if (joint)
                    return;
                continue;
            }

            PlayerbotAI* memberAI = GET_PLAYERBOT_AI(member);
            if (!memberAI || !memberAI->CanMove() || !DcMovement::DcMovementAllowed(memberAI) ||
                (joint && !CanCrossTogether(member)))
            {
                noteWait(*member, "movement unavailable or run speed too low");
                if (joint)
                    return;
                continue;
            }
            Movement::PointsArray path = BuildPath(member, target, joint);
            if (path.empty())
            {
                noteWait(*member, "no dry path to the refuge");
                if (joint)
                    return;
                continue;
            }
            departures.push_back({memberAI, std::move(path)});
        }
        if (departures.empty() || (joint && departures.size() != crossing.members.size()))
            return;

        if (timed)
        {
            DcNaxx40LivingPoisonTiming::Snapshot const timing(departures.front().memberAI->GetBot());
            auto const safeTiming = [&](Departure const& departure)
            {
                return timing.CanCross(departure.path, CrossingSpeed);
            };
            if (!timing.IsReady())
            {
                crossing.waitingFor = "could not read a complete Living Poison wave";
                return;
            }
            if (!std::ranges::all_of(departures, safeTiming))
            {
                crossing.waitingFor = "no common gap in the Living Poison wave";
                return;
            }
            for (Departure const& departure : departures)
                if (!ValidatePath(departure.memberAI->GetBot(), departure.path, true))
                {
                    noteWait(*departure.memberAI->GetBot(), "crossing path failed the floor or LOS check");
                    return;
                }
            DcNaxx40LivingPoisonTiming::Snapshot const freshTiming(departures.front().memberAI->GetBot());
            if (!std::ranges::all_of(departures, [&](Departure const& departure)
                { return freshTiming.CanCross(departure.path, CrossingSpeed); }))
            {
                crossing.waitingFor = "no common gap after checking the crossing paths";
                return;
            }
        }
        else if (approachCrossing)
        {
            for (Departure const& departure : departures)
                if (!ValidatePath(departure.memberAI->GetBot(), departure.path, false, true))
                {
                    noteWait(*departure.memberAI->GetBot(), "approach to the first refuge failed the slime-bank check");
                    return;
                }
        }

        std::size_t launched = 0;
        for (Departure& departure : departures)
        {
            Player* member = departure.memberAI->GetBot();
            uint32 const previousSpline = member->movespline->GetId();
            bool const moving = joint ? LaunchCrossing(member, departure.path)
                : DcMovement::SplinePath(departure.memberAI, departure.path, MovementPriority::MOVEMENT_FORCED,
                                         FORCED_MOVEMENT_RUN);
            if (moving && member->movespline->GetId() != previousSpline)
            {
                crossing.lastProgress = now;
                if (joint)
                {
                    DcRunState& memberRun = DcRun::Of(departure.memberAI);
                    memberRun.naxx40PoisonSpline = member->movespline->GetId();
                    memberRun.naxx40PoisonInstance = owner->GetInstanceId();
                    if (approachCrossing)
                        AuthorizeNaxx40SlimeCrossing(*departure.memberAI);
                }
                ++launched;
            }
            else if (joint)
            {
                // Map movement has not updated since the launch calls in this tick.
                for (std::size_t index = 0; index < launched; ++index)
                    departures[index].memberAI->GetBot()->StopMoving();
                noteWait(*member, "the movement generator refused the launch");
                return;
            }
        }
        if (joint && launched)
        {
            crossing.crossingLane = true;
            if (timed)
                LOG_INFO("playerbots.dungeonclear", "[DC:{}] Living Poison: launched {} bots together across row {}",
                         owner->GetName(), launched, (crossing.nextGate - FirstPoisonRefuge + 1) / 2);
            else
                LOG_INFO("playerbots.dungeonclear", "[DC:{}] Living Poison: {} bots crossed the slime approach {}",
                         owner->GetName(), launched, crossing.nextGate == ReturnCrossing ? "toward camp" : "together");
        }
    }

    class CrossingMapScript final : public AllMapScript
    {
    public:
        CrossingMapScript() : AllMapScript("DungeonClearNaxx40LivingPoison", {ALLMAPHOOK_ON_MAP_UPDATE}) {}

        void OnMapUpdate(Map* map, uint32 diff) override
        {
            if (!IsNaxx40Map(map) || !DcModule::IsEnabled())
                return;
            for (auto const& reference : map->GetPlayers())
            {
                Player* player = reference.GetSource();
                PlayerbotAI* ai = player ? GET_PLAYERBOT_AI(player) : nullptr;
                if (!ai)
                    continue;
                DcRunState& run = DcRun::Of(ai);
                if (!run.enabled)
                    continue;
                uint32 const instanceId = player->GetInstanceId();
                if (run.naxx40Poison.instanceId == instanceId)
                {
                    AiObjectContext* const context = ai->GetAiObjectContext();
                    auto const next = context->GetValue<std::optional<DungeonBossInfo>>(DcKey::NextDungeonBoss)->Get();
                    if (next && next->entry == Grobbulus)
                        UpdateCrossing(ai, diff);
                    else if (!std::ranges::any_of(run.naxx40Poison.members, [&](ObjectGuid guid)
                        { return IsCrossingLane(GetMember(player, guid)); }))
                    {
                        run.naxx40Poison = {};
                        run.naxx40GrobbulusCamp = {};
                    }
                    continue;
                }

                if (run.naxx40GrobbulusApproach.instanceId == instanceId &&
                    run.naxx40GrobbulusApproach.stage != GrobbulusStage::Complete)
                {
                    AiObjectContext* const context = ai->GetAiObjectContext();
                    auto const next = context->GetValue<std::optional<DungeonBossInfo>>(DcKey::NextDungeonBoss)->Get();
                    if (next && next->entry == Grobbulus)
                        UpdateGrobbulusApproach(ai, run);
                    else
                        run.naxx40GrobbulusApproach = {};
                    continue;
                }

                // Catch an in-flight generic spline when it reaches the white route entrance.
                if (!run.paused && run.naxx40PoisonCompletedInstance != instanceId &&
                    run.naxx40GrobbulusCamp.instanceId != instanceId && DcMovement::OwnsRunningEscort(player) &&
                    (DcNaxx40Patchwerk::CanJoinGrobbulusCampRoute(*player) || OnSouthApproach(player)) &&
                    !DcNaxx40SlimeCrossing::CrossingActive(player))
                {
                    AiObjectContext* const context = ai->GetAiObjectContext();
                    auto const next = context->GetValue<std::optional<DungeonBossInfo>>(DcKey::NextDungeonBoss)->Get();
                    if (next && next->entry == Grobbulus && !player->IsInCombat() &&
                        !DcActionShared::PullOwnsTheTank(player, context, "Patchwerk camp approach"))
                        BeginApproach(ai, next->entry);
                }
            }
        }
    };
}

bool BeginApproach(PlayerbotAI* ai, uint32 nextBoss)
{
    Player* owner = ai->GetBot();
    if (nextBoss != Grobbulus || !IsNaxx40Map(owner->FindMap()) ||
        !owner->IsAlive() || owner->IsInCombat() || !owner->GetGroup())
        return false;
    InstanceScript const* instance = owner->GetInstanceScript();
    if (!instance || instance->IsEncounterInProgress() || instance->GetBossState(Naxx40PatchwerkEncounter) != DONE)
        return false;
    DcRunState& run = DcRun::Of(ai);
    if (!run.enabled || run.paused)
        return false;
    if (run.naxx40Poison.instanceId == owner->GetInstanceId())
        return true;
    if (BeginGrobbulusApproach(ai, run))
        return true;
    if (run.naxx40PoisonCompletedInstance == owner->GetInstanceId())
        return false;

    using DcNaxx40Patchwerk::SweepResult;
    SweepResult const route = DcNaxx40Patchwerk::DriveGrobbulusCamp(ai);
    switch (route)
    {
        case SweepResult::Inactive:
            if (!OnSouthApproach(owner))
                return false;
            break;
        case SweepResult::Moving:
            DcActionShared::ClearStall(ai->GetAiObjectContext());
            DcActionShared::SetPhase(ai->GetAiObjectContext(), "taking the white route to the Patchwerk camp");
            DcRunProgress::Stamp(run.progress, getMSTime());
            return true;
        case SweepResult::Blocked:
            Hold(owner);
            WaitForManualCrossing(ai, run, "Grobbulus: no dry path along the white route to the Patchwerk camp");
            return true;
        case SweepResult::Waiting:
            break;
        default:
            return false;
    }

    auto& crossing = run.naxx40Poison;
    crossing = {};
    crossing.nextGate = route == SweepResult::Waiting ? WhiteCampGate : 0;
    for (GroupReference* ref = owner->GetGroup()->GetFirstMember(); ref; ref = ref->next())
    {
        Player* candidate = ref->GetSource();
        if (!candidate || candidate->FindMap() != owner->FindMap())
            continue;
        Player* member = GetMember(owner, candidate->GetGUID());
        PlayerbotAI* memberAI = member ? GET_PLAYERBOT_AI(member) : nullptr;
        if (!memberAI || Location(member).x >= Stops.back().x - BeyondRefugeMargin)
            continue;
        G3D::Vector3 const point = Location(member);
        G3D::Vector3 const& start = Stops[crossing.nextGate];
        if (member->IsInCombat() || (point - start).squaredLength() > 90.0f * 90.0f ||
            std::abs(point.z - start.z) > 1.5f)
        {
            crossing.members.clear();
            Hold(owner);
            WaitForManualCrossing(ai, run, "Living Poison: regroup the raid on this floor before resuming");
            return true;
        }
        crossing.members.push_back(member->GetGUID());
    }
    if (crossing.members.empty())
        return false;
    auto const leader = std::ranges::find(crossing.members, owner->GetGUID());
    if (leader != crossing.members.end())
        std::iter_swap(crossing.members.begin(), leader);
    crossing.instanceId = owner->GetInstanceId();
    crossing.lastProgress = getMSTime();
    for (ObjectGuid const guid : crossing.members)
    {
        Player* member = GetMember(owner, guid);
        PlayerbotAI* memberAI = member ? GET_PLAYERBOT_AI(member) : nullptr;
        if (memberAI)
        {
            DcRun::Of(memberAI).naxx40PoisonOwner = owner->GetGUID();
            Hold(member);
        }
    }
    DcActionShared::SetPhase(ai->GetAiObjectContext(), "crossing Living Poison");
    LOG_INFO("playerbots.dungeonclear", "[DC:{}] Living Poison crossing for {} bots",
             owner->GetName(), crossing.members.size());
    return true;
}

bool BlocksGrobbulusPull(PlayerbotAI* ai, uint32 nextBoss)
{
    Player* const bot = ai->GetBot();
    if (nextBoss != Grobbulus || !IsNaxx40Map(bot->FindMap()))
        return false;
    DcRunState const& run = DcRun::Of(ai);
    uint32 const instanceId = bot->GetInstanceId();
    if (run.naxx40GrobbulusApproach.instanceId == instanceId)
        return run.naxx40GrobbulusApproach.stage != GrobbulusStage::Complete;
    return run.naxx40PoisonCompletedInstance != instanceId &&
        (run.naxx40Poison.instanceId == instanceId || run.naxx40GrobbulusCamp.instanceId == instanceId);
}

bool GrobbulusEastPullReady(Player* tank)
{
    PlayerbotAI* const ai = GET_PLAYERBOT_AI(tank);
    if (!ai || !IsNaxx40Map(tank->FindMap()))
        return false;
    DcRunState const& run = DcRun::Of(ai);
    return run.enabled && !run.paused && run.naxx40GrobbulusApproach.instanceId == tank->GetInstanceId() &&
        run.naxx40GrobbulusApproach.stage == GrobbulusStage::Pull;
}

bool OwnsGrobbulusApproach(PlayerbotAI* ai)
{
    Player* const tank = ai->GetBot();
    if (!IsNaxx40Map(tank->FindMap()))
        return false;
    DcRunState const& run = DcRun::Of(ai);
    auto const& leg = run.naxx40GrobbulusApproach;
    return run.enabled && leg.instanceId == tank->GetInstanceId() &&
        leg.stage != GrobbulusStage::Pull && leg.stage != GrobbulusStage::Complete;
}

bool OwnsMovement(PlayerbotAI* ai)
{
    Player* bot = ai->GetBot();
    if (!IsNaxx40Map(bot->FindMap()))
        return false;
    if (IsCrossingLane(bot))
        return true;
    if (bot->IsInCombat())
        return false;
    ObjectGuid const ownerGuid = DcRun::Of(ai).naxx40PoisonOwner;
    if (ownerGuid.IsEmpty())
        return false;
    Player* owner = ObjectAccessor::GetPlayer(*bot, ownerGuid);
    PlayerbotAI* ownerAI = owner ? GET_PLAYERBOT_AI(owner) : nullptr;
    if (!ownerAI || owner->GetGroup() != bot->GetGroup())
        return false;
    DcRunState const& run = DcRun::Of(ownerAI);
    InstanceScript const* instance = bot->GetInstanceScript();
    if (!run.enabled || !instance || instance->IsEncounterInProgress())
        return false;
    if (run.naxx40Poison.instanceId == bot->GetInstanceId())
        return true;
    auto const& leg = run.naxx40GrobbulusApproach;
    return leg.instanceId == bot->GetInstanceId() && leg.stage != GrobbulusStage::Pull &&
        leg.stage != GrobbulusStage::Complete &&
        std::ranges::find(leg.members, bot->GetGUID()) != leg.members.end();
}

bool IsCrossingLane(Player* player)
{
    if (!player || !IsNaxx40Map(player->FindMap()) || !player->movespline || player->movespline->Finalized())
        return false;
    PlayerbotAI* ai = GET_PLAYERBOT_AI(player);
    if (!ai)
        return false;
    DcRunState const& run = DcRun::Of(ai);
    return run.naxx40PoisonInstance == player->GetInstanceId() &&
        run.naxx40PoisonSpline == player->movespline->GetId();
}

void RegisterMapUpdate()
{
    new CrossingMapScript();
}
}
