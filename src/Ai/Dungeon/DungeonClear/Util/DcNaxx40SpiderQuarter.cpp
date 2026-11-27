/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, or later.
 */

#include "DcNaxx40SpiderQuarter.h"

#include "Ai/Dungeon/DungeonClear/Action/DcActionShared.h"
#include "Ai/Dungeon/DungeonClear/Data/ScriptedPullRegistry.h"
#include "Ai/Dungeon/DungeonClear/DcPullContext.h"
#include "Ai/Raid/Naxx40/Naxx40Encounter.h"
#include "Creature.h"
#include "DcPlayerbotCompat.h"
#include "Group.h"
#include "Map.h"
#include "MotionMaster.h"
#include "PathGenerator.h"
#include "Pet.h"
#include "Player.h"
#include "World.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <vector>

namespace
{
    using SpawnId = ObjectGuid::LowType;

    constexpr SpawnId PatrolMembers[] = {
        361844, 361854, 361889, 361890, 361891, 361892, 361893, 361894, 361895, 361896,
        361850, 361851, 361852, 361853, 361855, 361856, 361857, 361858,
        361786, 361787, 361788, 361789, 361790, 361791, 361792, 361793,
        361303, 361304, 361875, 361876,
    };

    // The failed Shoot took 2.26 seconds to land. Allow time for the patrol to
    // move between checking the stand and the ranged tag actually engaging it.
    constexpr float TagTravelSeconds = 2.5f;
    constexpr float WalkInSeconds = 5.0f;
    constexpr float CampFootprint = 6.0f;
    constexpr float MaxProbeDistanceSq = 105.0f * 105.0f;

    bool UsesSpiderStage(Player* tank, ScriptedPullStage const& stage)
    {
        return tank && IsNaxx40Map(tank->FindMap()) && DcNaxx40SpiderQuarter::IsStage(&stage);
    }

    DcPullContext::BlockedSpiderCamp const* FindCampDeferral(Player const& tank,
        ScriptedPullStage const& stage, DcPullContext const& pull)
    {
        auto const it = std::ranges::find(pull.blockedSpiderCamps, stage.order,
                                         &DcPullContext::BlockedSpiderCamp::order);
        if (it == pull.blockedSpiderCamps.end() || it->instanceId != tank.GetInstanceId())
            return nullptr;
        return &*it;
    }

    Creature* FindLiveSpawn(Player const& tank, SpawnId spawn)
    {
        auto const [first, last] = tank.GetMap()->GetCreatureBySpawnIdStore().equal_range(spawn);
        for (auto it = first; it != last; ++it)
        {
            Creature* creature = it->second;
            if (creature && creature->IsInWorld() && creature->IsAlive() && tank.InSamePhase(creature))
                return creature;
        }
        return nullptr;
    }

    float PatrolSpeed(Creature const& creature)
    {
        return creature.GetSpeed(creature.IsWalking() ? MOVE_WALK : MOVE_RUN);
    }

    float TagTravel(Creature const& creature)
    {
        SpawnId const spawn = creature.GetSpawnId();
        if (std::ranges::find(PatrolMembers, spawn) != std::end(PatrolMembers))
            return PatrolSpeed(creature) * TagTravelSeconds;
        return 0.0f;
    }

    bool CanAssistDuringTag(Creature const& caller, Creature const& assistant, Player const& tank, float radius)
    {
        float const margin = TagTravel(caller) + TagTravel(assistant);
        if (!caller.IsWithinDistInMap(&assistant, radius + margin, true, false, false) ||
            !assistant.CanAssistTo(&caller, &tank))
            return false;

        // Native family assistance checks 3D center distance and LOS. A moving
        // patrol can reach the call range and acquire LOS during the tag.
        return margin > 0.0f || caller.IsWithinLOSInMap(&assistant);
    }

    bool NearHostile(Player const& tank, Creature const* creature)
    {
        return creature && creature->IsAlive() && creature->IsInWorld() && tank.InSamePhase(creature) &&
            tank.IsHostileTo(creature) && tank.GetExactDistSq(creature) < MaxProbeDistanceSq &&
            tank.IsValidAttackTarget(creature);
    }

    bool ThreatensPoint(Player const& tank, Creature const& creature, G3D::Vector3 const& point, float margin)
    {
        float const aggro = creature.GetAggroRange(&tank);
        if (aggro <= 0.0f || !creature.HasReactState(REACT_AGGRESSIVE))
            return false;
        float const distance = creature.GetExactDist(point.x, point.y, point.z);
        return distance <= aggro + margin && creature.IsWithinLOS(point.x, point.y, point.z + 1.5f);
    }
}

bool DcNaxx40SpiderQuarter::UsesApproach(Player const* tank, uint32 boss)
{
    return (boss == Anub || boss == Faerlina || boss == Maexxna) && tank && IsNaxx40Map(tank->FindMap());
}

bool DcNaxx40SpiderQuarter::IsStage(ScriptedPullStage const* stage)
{
    return stage && stage->mapId == 533 &&
        (stage->bossEntry == Anub || stage->bossEntry == Faerlina || stage->bossEntry == Maexxna);
}

bool DcNaxx40SpiderQuarter::CampDeferredHere(Player const& tank, ScriptedPullStage const& stage,
    DcPullContext const& pull)
{
    // Re-probe only after the tank has made progress along the corridor.
    constexpr float RetryDistance = 6.0f;
    auto const* failure = FindCampDeferral(tank, stage, pull);
    return failure && tank.GetExactDistSq(&failure->from) < RetryDistance * RetryDistance;
}

bool DcNaxx40SpiderQuarter::HadBlockedCamp(Player const& tank, ScriptedPullStage const& stage,
    DcPullContext const& pull)
{
    return FindCampDeferral(tank, stage, pull) != nullptr;
}

void DcNaxx40SpiderQuarter::DeferCamp(Player const& tank, ScriptedPullStage const& stage, DcPullContext& pull)
{
    ClearCampDeferral(stage, pull);
    pull.blockedSpiderCamps.push_back({stage.order, tank.GetInstanceId(), tank.GetPosition()});
}

void DcNaxx40SpiderQuarter::ClearCampDeferral(ScriptedPullStage const& stage, DcPullContext& pull)
{
    std::erase_if(pull.blockedSpiderCamps, [&](DcPullContext::BlockedSpiderCamp const& failure)
    {
        return failure.order == stage.order;
    });
}

bool DcNaxx40SpiderQuarter::ApproachReady(Player* tank, ScriptedPullStage const& stage)
{
    if (!UsesSpiderStage(tank, stage))
        return false;
    // The upper tunnel stand lies beside the stationary Necropolis Acolytes.
    // Let the corridor fight reach their floor before arming this melee pull.
    if (std::ranges::find(stage.spawnIds, 361956u) != stage.spawnIds.end() &&
        (FindLiveSpawn(*tank, 361368) || FindLiveSpawn(*tank, 361369)))
        return false;
    return std::abs(tank->GetPositionZ() - stage.armZ) <= 8.0f ||
        tank->GetExactDist(stage.campX, stage.campY, stage.campZ) <= 8.0f ||
        tank->GetExactDist(stage.standX, stage.standY, stage.standZ) <= 8.0f;
}

Unit* DcNaxx40SpiderQuarter::NearestPackMember(Player* tank, ScriptedPullStage const& stage, ObjectGuid exclude)
{
    if (!UsesSpiderStage(tank, stage))
        return nullptr;

    Position const stand(stage.standX, stage.standY, stage.standZ);
    auto const opener = tank->GetExactDist(&stand) <= 2.0f
        ? DcActionShared::ResolvePullSpell(GET_PLAYERBOT_AI(tank), tank)
        : std::nullopt;
    Creature* nearest = nullptr;
    float bestDistance = std::numeric_limits<float>::max();
    bool bestTaggable = false;
    for (SpawnId spawn : stage.spawnIds)
    {
        Creature* member = FindLiveSpawn(*tank, spawn);
        if (!member || member->GetGUID() == exclude || !tank->IsValidAttackTarget(member))
            continue;

        float const distance = stand.GetExactDist(*member);
        float const currentDistance = tank->GetExactDist(member);
        bool const taggable = opener && currentDistance >= opener->minRange && currentDistance <= opener->maxRange &&
            tank->IsWithinLOSInMap(member);
        if (taggable != bestTaggable ? taggable : distance < bestDistance)
        {
            nearest = member;
            bestDistance = distance;
            bestTaggable = taggable;
        }
    }
    return nearest;
}

Movement::PointsArray DcNaxx40SpiderQuarter::BuildPullPath(Player* tank, Position const& destination)
{
    G3D::Vector3 const end(destination.GetPositionX(), destination.GetPositionY(), destination.GetPositionZ());
    // Use the normal movement path on the ramp. A straight-path chord can
    // skip the change in grade between the camp and the lower corridor.
    PathGenerator path(tank);
    if (!path.CalculatePath(end.x, end.y, end.z) || path.GetPathType() != PATHFIND_NORMAL ||
        path.GetPath().size() < 2 || (path.GetActualEndPosition() - end).squaredLength() > 1.0f)
        return {};

    Movement::PointsArray points = path.GetPath();
    points.front() = {tank->GetPositionX(), tank->GetPositionY(), tank->GetPositionZ()};
    return points;
}

DcNaxx40SpiderQuarter::CampPathCheck DcNaxx40SpiderQuarter::CheckCampPath(Player* tank,
    ScriptedPullStage const& stage, Position const& destination, bool requireCompletePath)
{
    if (!UsesSpiderStage(tank, stage))
        return {};

    // The party can start moving as soon as the camp is published.
    Movement::PointsArray path;
    if (requireCompletePath || tank->GetExactDist(&destination) > 2.0f)
    {
        path = BuildPullPath(tank, destination);
        if (path.empty())
            return {"no complete path from the tank to camp", true};
    }
    path.emplace_back(destination.GetPositionX(), destination.GetPositionY(), destination.GetPositionZ());

    for (auto const& [spawn, creature] : tank->GetMap()->GetCreatureBySpawnIdStore())
    {
        if (!NearHostile(*tank, creature))
            continue;
        float const patrolMargin = TagTravel(*creature);
        if (ThreatensPoint(*tank, *creature, path.back(), CampFootprint + patrolMargin))
            return {"camp within aggro range of spawn " + std::to_string(spawn)};
        for (std::size_t i = 1; i < path.size(); ++i)
        {
            G3D::Vector3 const leg = path[i] - path[i - 1];
            int const samples = std::max(1, static_cast<int>(std::ceil(leg.length() / 4.0f)));
            for (int sample = 1; sample <= samples; ++sample)
                if (ThreatensPoint(*tank, *creature, path[i - 1] + leg * (static_cast<float>(sample) / samples),
                                   patrolMargin))
                    return {"camp approach within aggro range of spawn " + std::to_string(spawn)};
        }
    }
    return {};
}

bool DcNaxx40SpiderQuarter::CampPetsReady(Player* tank, Position const& camp, float radius,
                                         Player const** waitingOwner)
{
    if (!tank || !IsNaxx40Map(tank->FindMap()))
        return true;

    Group* group = tank->GetGroup();
    if (!group)
        return true;

    bool ready = true;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* owner = ref->GetSource();
        if (!owner || owner == tank || !owner->IsAlive() || !owner->IsInMap(tank) ||
            DcPlayerbotCompat::IsHumanControlled(owner))
            continue;

        Pet* pet = owner->GetPet();
        if (!pet || !pet->IsAlive() || !pet->IsInMap(tank))
            continue;
        if (pet->GetExactDist(&camp) <= radius && !pet->GetVictim())
            continue;

        if (ready && waitingOwner)
            *waitingOwner = owner;
        ready = false;
        if (owner->GetExactDist(&camp) <= radius &&
            (pet->GetVictim() || pet->GetMotionMaster()->GetCurrentMovementGeneratorType() != FOLLOW_MOTION_TYPE))
            if (PlayerbotAI* ownerAI = GET_PLAYERBOT_AI(owner))
                ownerAI->PetFollow();
    }
    return ready;
}

bool DcNaxx40SpiderQuarter::PatrolClearForTag(Player* tank, ScriptedPullStage const& stage)
{
    if (!UsesSpiderStage(tank, stage) || stage.spawnIds.empty())
        return true;

    float const radius = sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS);
    if (radius <= 0.0f)
        return true;

    std::vector<Creature const*> callers;
    callers.reserve(stage.spawnIds.size());
    for (SpawnId spawn : stage.spawnIds)
        if (Creature* creature = FindLiveSpawn(*tank, spawn))
            callers.push_back(creature);

    for (auto const& [spawn, creature] : tank->GetMap()->GetCreatureBySpawnIdStore())
    {
        if (!NearHostile(*tank, creature) || std::ranges::find(stage.spawnIds, spawn) != stage.spawnIds.end())
            continue;
        for (Creature const* caller : callers)
            if (CanAssistDuringTag(*caller, *creature, *tank, radius))
                return false;
    }
    return true;
}

bool DcNaxx40SpiderQuarter::PatrolClearOfStand(Player* tank, ScriptedPullStage const& stage, bool departing)
{
    if (!UsesSpiderStage(tank, stage))
        return true;

    G3D::Vector3 const stand(stage.standX, stage.standY, stage.standZ);
    float const seconds = departing ? WalkInSeconds : TagTravelSeconds;
    return std::ranges::none_of(PatrolMembers, [&](SpawnId spawn)
    {
        if (std::ranges::find(stage.spawnIds, spawn) != stage.spawnIds.end())
            return false;
        Creature const* member = FindLiveSpawn(*tank, spawn);
        if (!member)
            return false;

        // Creature::CanStartAttack gets the level- and config-adjusted radius
        // from GetAggroRange. Leave room for the patrol to move during the leg.
        return ThreatensPoint(*tank, *member, stand, PatrolSpeed(*member) * seconds);
    });
}
