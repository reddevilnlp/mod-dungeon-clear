/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, or later.
 */

#include "DcNaxx40AnubApproach.h"

#include "Ai/Dungeon/DungeonClear/Action/DcActionShared.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Raid/Naxx40/Naxx40Encounter.h"
#include "Creature.h"
#include "DcBreadcrumb.h"
#include "DcCombatFlag.h"
#include "DcMovement.h"
#include "DcNaxx40SpiderQuarter.h"
#include "DcPullPlanner.h"
#include "DcRun.h"
#include "DcTankForm.h"
#include "Log.h"
#include "Map.h"
#include "MoveSpline.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Timer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace DcNaxx40AnubApproach
{
namespace
{
    using SpawnId = ObjectGuid::LowType;
    using State = DcRunState::AnubApproach;

    uint32 constexpr Anub = 351009;
    float constexpr ArrivalRadius = 1.5f;
    float constexpr ShootRange = 30.0f;
    uint32 constexpr TagRetryMs = 3000;
    float constexpr PatrolLookAheadSeconds = 2.5f;
    float constexpr PatrolWalkLookAheadSeconds = 5.0f;
    Position const CampOne(3071.304f, -3504.114f, 287.07825f, 5.4861865f);
    Position const CampTwo(3154.773f, -3485.1558f, 287.07556f, 0.25151968f);

    struct Formation
    {
        SpawnId leader;
        Position stand;
        std::array<SpawnId, 10> members;
        SpawnId preferred = 0;
    };

    // The first block progresses from Camp 1 without regrouping there after each fight.
    std::array<Formation, 5> const Formations{{
        {361189, CampOne, {361187, 361188, 361189, 361190, 361205, 361206}},
        {361781, {3084.0f, -3536.0f, 287.07687f},
            {361776, 361777, 361778, 361779, 361780, 361781, 361782, 361783, 361784, 361785}},
        {361193, {3112.0f, -3550.0f, 287.0800f}, {361191, 361192, 361193, 361194, 361207, 361208}},
        {361201, {3110.0f, -3524.0f, 287.07945f},
            {361195, 361196, 361197, 361198, 361199, 361200, 361201, 361202, 361203, 361204}},
        {361214, CampTwo, {361209, 361210, 361211, 361212, 361213, 361214}, 361211}
    }};
    std::array<SpawnId, 10> constexpr Patrol{
        361844, 361854, 361889, 361890, 361891, 361892, 361893, 361894, 361895, 361896};

    Creature* LiveSpawn(Player const& tank, SpawnId spawn)
    {
        if (!spawn)
            return nullptr;
        auto const [first, last] = tank.GetMap()->GetCreatureBySpawnIdStore().equal_range(spawn);
        for (auto it = first; it != last; ++it)
        {
            Creature* member = it->second;
            if (member && member->IsInWorld() && member->IsAlive() && tank.InSamePhase(member))
                return member;
        }
        return nullptr;
    }

    bool HasLivingMember(Player const& tank, Formation const& formation)
    {
        return std::ranges::any_of(formation.members, [&](SpawnId spawn) { return LiveSpawn(tank, spawn); });
    }

    bool NearSpawnGrid(Player const& tank, Position const& point)
    {
        // Both camps and all listed spawn homes lie in the same map grid.
        // An empty creature store before that grid loads is not a cleared route.
        return tank.GetMap()->IsGridLoaded(point.GetPositionX(), point.GetPositionY());
    }

    bool IsWalkingTo(Player* tank, Position const& point)
    {
        G3D::Vector3 const destination(point.GetPositionX(), point.GetPositionY(), point.GetPositionZ());
        return DcMovement::OwnsRunningEscort(tank) && tank->movespline &&
            (tank->movespline->FinalDestination() - destination).squaredLength() <= 1.0f;
    }

    bool PatrolThreatens(Player const& tank, G3D::Vector3 const& point, float seconds)
    {
        for (SpawnId spawn : Patrol)
        {
            Creature const* member = LiveSpawn(tank, spawn);
            if (!member || !member->HasReactState(REACT_AGGRESSIVE))
                continue;
            float const range = member->GetAggroRange(&tank);
            float const speed = member->GetSpeed(member->IsWalking() ? MOVE_WALK : MOVE_RUN);
            if (range > 0.0f && member->GetExactDist(point.x, point.y, point.z) <= range + 6.0f + speed * seconds)
                return true;
        }
        return false;
    }

    bool PatrolThreatensCampTwoLeg(Player const& tank, Movement::PointsArray const& path)
    {
        float distance = 0.0f;
        float const speed = std::max(1.0f, tank.GetSpeed(MOVE_RUN));
        for (std::size_t i = 1; i < path.size(); ++i)
        {
            G3D::Vector3 const leg = path[i] - path[i - 1];
            float const length = leg.length();
            unsigned const samples = std::max(1u, static_cast<unsigned>(std::ceil(length / 4.0f)));
            for (unsigned sample = 1; sample <= samples; ++sample)
            {
                float const fraction = float(sample) / float(samples);
                float const eta = (distance + fraction * length) / speed;
                float const lookAhead = std::min(eta + PatrolLookAheadSeconds, PatrolWalkLookAheadSeconds);
                if (PatrolThreatens(tank, path[i - 1] + leg * fraction, lookAhead))
                    return true;
            }
            distance += length;
        }
        return false;
    }

    bool Walk(PlayerbotAI& ai, Position const& point, char const* phase, bool guardCampTwo)
    {
        Player* tank = ai.GetBot();
        AiObjectContext* context = ai.GetAiObjectContext();
        DcActionShared::SetPhase(context, phase);
        if (IsWalkingTo(tank, point))
        {
            // Recheck the live patrol while moving. A full-run straight-line
            // prediction is too conservative once it turns onto another leg.
            G3D::Vector3 const here(tank->GetPositionX(), tank->GetPositionY(), tank->GetPositionZ());
            if (guardCampTwo && PatrolThreatens(*tank, here, PatrolLookAheadSeconds))
            {
                DcMovement::StopBot(tank, DcMovement::Stop::Hold);
                DcActionShared::StallDungeonClear(&ai,
                    "Anub: Skitterer patrol approaching the raid on the Camp 2 leg.");
            }
            return true;
        }
        auto path = DcNaxx40SpiderQuarter::BuildPullPath(tank, point);
        if (path.empty())
        {
            DcMovement::StopBot(tank, DcMovement::Stop::Hold);
            DcActionShared::StallDungeonClear(&ai, std::string(phase) + ": no complete path.");
            return true;
        }
        if (guardCampTwo && PatrolThreatensCampTwoLeg(*tank, path))
        {
            DcMovement::StopBot(tank, DcMovement::Stop::Hold);
            DcActionShared::StallDungeonClear(&ai, "Anub: waiting for the Skitterer patrol before Camp 2.");
            return true;
        }
        DcActionShared::ClearStall(context);
        DcMovement::SplinePath(&ai, path, MovementPriority::MOVEMENT_NORMAL, FORCED_MOVEMENT_RUN);
        return true;
    }

    bool InTagRange(Player const& tank, Creature const& target, float minRange, float maxRange)
    {
        float const distance = tank.GetExactDist(&target);
        return tank.IsValidAttackTarget(&target) && distance >= minRange && distance <= maxRange &&
            tank.IsWithinLOSInMap(&target);
    }

    Creature* ChooseMember(Player const& tank, Formation const& formation, float minRange, float maxRange)
    {
        if (Creature* preferred = LiveSpawn(tank, formation.preferred))
            return preferred;

        Creature* target = nullptr;
        bool canTag = false;
        float nearest = 0.0f;
        for (SpawnId spawn : formation.members)
            if (Creature* member = LiveSpawn(tank, spawn); member && tank.IsValidAttackTarget(member))
            {
                bool const ready = InTagRange(tank, *member, minRange, maxRange);
                float const distance = tank.GetExactDistSq(member);
                if (!target || (ready != canTag ? ready : distance < nearest))
                {
                    target = member;
                    canTag = ready;
                    nearest = distance;
                }
            }
        return target;
    }

    bool MoveIntoTagRange(PlayerbotAI& ai, Creature const& target, float minRange, float maxRange)
    {
        Player* tank = ai.GetBot();
        if (DcMovement::OwnsRunningEscort(tank))
            return true;
        auto path = DcNaxx40SpiderQuarter::BuildPullPath(tank, target.GetPosition());
        // Stop the walk once a point can fire instead of walking to the mob's feet.
        for (std::size_t i = 1; i < path.size(); ++i)
        {
            G3D::Vector3 const leg = path[i] - path[i - 1];
            unsigned const samples = std::max(1u, static_cast<unsigned>(std::ceil(leg.length())));
            for (unsigned sample = 1; sample <= samples; ++sample)
            {
                G3D::Vector3 const p = path[i - 1] + leg * (float(sample) / float(samples));
                float const distance = target.GetExactDist(p.x, p.y, p.z);
                if (distance >= minRange && distance <= maxRange - 0.5f &&
                    target.IsWithinLOS(p.x, p.y, p.z + 1.5f) &&
                    !PatrolThreatens(*tank, p, PatrolLookAheadSeconds))
                {
                    path.resize(i + 1);
                    path.back() = p;
                    DcActionShared::ClearStall(ai.GetAiObjectContext());
                    return DcMovement::SplinePath(&ai, path, MovementPriority::MOVEMENT_NORMAL, FORCED_MOVEMENT_RUN);
                }
            }
        }
        return false;
    }

    void ClearCampHold(State& route)
    {
        route.campHold.reset();
        route.campHoldSince = 0;
        route.campHoldCheckedAt = 0;
    }

    void HoldRaidAt(Player* tank, State& route, Position const& position)
    {
        if (route.campHold)
            return;
        DcMovement::StopBot(tank, DcMovement::Stop::HardPin);
        route.campHold = position;
        route.campHoldSince = getMSTime();
        route.campHoldCheckedAt = 0;
        LOG_INFO("playerbots.dungeonclear", "[DC:{}] Anub approach: holding raid at ({:.2f},{:.2f},{:.2f})",
                 tank->GetName(), position.GetPositionX(), position.GetPositionY(), position.GetPositionZ());
    }

    bool WaitForRaid(PlayerbotAI& ai, Position const& camp)
    {
        Player* tank = ai.GetBot();
        float const radius = DcActionShared::DC_PULL_SET_RADIUS;
        Player const* waitingMember = nullptr;
        Player const* waitingOwner = nullptr;
        if (!DcPullPlanner::IsPartySetAtCamp(tank, camp, radius, &waitingMember))
        {
            DcActionShared::StallDungeonClear(&ai, waitingMember
                ? "Anub: waiting for " + waitingMember->GetName() + " to hold at camp."
                : "Anub: waiting for the raid to hold at camp.");
            return true;
        }
        if (!DcNaxx40SpiderQuarter::CampPetsReady(tank, camp, radius, &waitingOwner))
        {
            DcActionShared::StallDungeonClear(&ai, waitingOwner
                ? "Anub: waiting for " + waitingOwner->GetName() + "'s pet."
                : "Anub: waiting for pets to reach the pull point.");
            return true;
        }
        return false;
    }
}

bool AppliesTo(Player const* tank, uint32 boss)
{
    return boss == Anub && tank && IsNaxx40Map(tank->FindMap());
}

bool OwnsApproach(Player* tank, AiObjectContext* context, uint32 boss)
{
    if (!context || !AppliesTo(tank, boss))
        return false;
    State const& route = DcRun::Of(context).naxx40AnubApproach;
    return route.instanceId != tank->GetInstanceId() || route.formation < Formations.size();
}

Position const* ActiveCampHold(Player* tank, AiObjectContext* context)
{
    if (!context || !AppliesTo(tank, Anub))
        return nullptr;
    DcRunState& run = DcRun::Of(context);
    State& route = run.naxx40AnubApproach;
    if (!run.enabled || run.paused || route.instanceId != tank->GetInstanceId() || !route.campHold)
        return nullptr;
    auto const next = context->GetValue<std::optional<DungeonBossInfo>>(DcKey::NextDungeonBoss)->Get();
    if (!next || next->entry != Anub)
        return nullptr;

    // Advance cannot release the hold while the combat engine owns the tank.
    // Observe the whole raid here, including a follower pulled by a patrol.
    uint32 const now = getMSTime();
    if (route.campHoldCheckedAt != now)
    {
        route.campHoldCheckedAt = now;
        if (DcCombatFlag::AnyPartyEngagement(tank))
        {
            ClearCampHold(route);
            LOG_INFO("playerbots.dungeonclear", "[DC:{}] Anub approach: combat confirmed -> releasing raid in place",
                     tank->GetName());
            return nullptr;
        }
    }
    return &*route.campHold;
}

bool Drive(PlayerbotAI* ai)
{
    Player* tank = ai ? ai->GetBot() : nullptr;
    if (!AppliesTo(tank, Anub) || !DcMovement::DcMovementAllowed(ai))
        return false;
    AiObjectContext* context = ai->GetAiObjectContext();
    State& route = DcRun::Of(context).naxx40AnubApproach;
    if (route.instanceId != tank->GetInstanceId())
    {
        route = {};
        route.instanceId = tank->GetInstanceId();
    }
    if (route.formation >= Formations.size())
        return false;
    if (!DcCombatFlag::MayDrive(tank, context))
        return true;  // Yield movement and targeting to the combat engine.

    DcRecordBreadcrumb(context, tank);
    if (!route.reachedCampOne)
    {
        if (tank->GetExactDist(&CampOne) > ArrivalRadius)
            return Walk(*ai, CampOne, "Anub: moving to Camp 1", false);
        HoldRaidAt(tank, route, CampOne);
        if (WaitForRaid(*ai, *route.campHold))
            return true;
        tank->SetFacingTo(CampOne.GetOrientation());
        route.reachedCampOne = true;
        route.reachedStand = true;
        LOG_INFO("playerbots.dungeonclear", "[DC:{}] Anub approach: raid ready at Camp 1", tank->GetName());
    }

    while (route.formation < Formations.size() && NearSpawnGrid(*tank, Formations[route.formation].stand) &&
           !HasLivingMember(*tank, Formations[route.formation]))
    {
        if (route.formation == Formations.size() - 1 && !route.reachedStand)
            break;  // Check Camp 2 even if its pack died earlier.
        LOG_INFO("playerbots.dungeonclear", "[DC:{}] Anub approach: formation {} clear",
                 tank->GetName(), Formations[route.formation].leader);
        ++route.formation;
        route.lastTagAttempt = 0;
        route.reachedStand = false;
        ClearCampHold(route);
    }
    if (route.formation >= Formations.size())
    {
        DcActionShared::ClearStall(context);
        LOG_INFO("playerbots.dungeonclear", "[DC:{}] Anub approach: ordered trash clear -> advance to Anub",
                 tank->GetName());
        return false;
    }

    Formation const& formation = Formations[route.formation];
    bool const campTwo = route.formation == Formations.size() - 1;
    if (!route.reachedStand)
    {
        if (tank->GetExactDist(&formation.stand) > ArrivalRadius)
        {
            char const* phase = campTwo ? "Anub: moving to Camp 2"
                : route.formation == 0 ? "Anub: moving to Camp 1" : "Anub: moving to the next pull point";
            return Walk(*ai, formation.stand, phase, campTwo);
        }
        HoldRaidAt(tank, route, formation.stand);
        if (WaitForRaid(*ai, *route.campHold))
            return true;
        if (campTwo)
            tank->SetFacingTo(CampTwo.GetOrientation());
        route.reachedStand = true;
        LOG_INFO("playerbots.dungeonclear",
                 "[DC:{}] Anub approach: raid ready for formation {} at ({:.2f},{:.2f},{:.2f})",
                 tank->GetName(), formation.leader, tank->GetPositionX(), tank->GetPositionY(), tank->GetPositionZ());
    }

    if (!NearSpawnGrid(*tank, formation.stand))
    {
        DcActionShared::StallDungeonClear(ai, "Anub: waiting for the current formation's grid to load.");
        return true;
    }
    if (!HasLivingMember(*tank, formation))
        return true;  // The next tick can now retire the Camp 2 arrival gate.

    // If a fight ended with survivors, gather where the tank finished. Do not
    // bring the raid back to a previous camp to open the same formation again.
    HoldRaidAt(tank, route, tank->GetPosition());
    if (WaitForRaid(*ai, *route.campHold))
        return true;

    DcTankForm::EnsureBearForm(ai);
    auto const opener = DcActionShared::ResolvePullSpell(ai, tank);
    if (!opener)
    {
        DcActionShared::StallDungeonClear(ai, "Anub: the tank needs a ranged pull spell or weapon.");
        return true;
    }
    float const maxRange = std::min(opener->maxRange, ShootRange);
    Creature* target = ChooseMember(*tank, formation, opener->minRange, maxRange);
    if (!target)
    {
        DcActionShared::StallDungeonClear(ai, "Anub: the current formation has no attackable member.");
        return true;
    }
    if (!InTagRange(*tank, *target, opener->minRange, maxRange))
    {
        // Keep the preferred shot at Camp 2. For other members and survivors,
        // walk only far enough forward to get a valid ranged shot.
        bool const fixedOpening = target->GetSpawnId() == formation.preferred;
        if (!fixedOpening && MoveIntoTagRange(*ai, *target, opener->minRange, maxRange))
            return true;
        DcActionShared::StallDungeonClear(ai, "Anub: waiting for range and line of sight to formation " +
                                         std::to_string(formation.leader) + ".");
        return true;
    }

    DcMovement::StopBot(tank, DcMovement::Stop::HardPin);
    G3D::Vector3 const here(tank->GetPositionX(), tank->GetPositionY(), tank->GetPositionZ());
    if (PatrolThreatens(*tank, here, PatrolLookAheadSeconds))
    {
        DcActionShared::StallDungeonClear(ai, "Anub: waiting for the Skitterer patrol to clear the pull point.");
        return true;
    }

    uint32 const now = getMSTime();
    if (route.lastTagAttempt && getMSTimeDiff(route.lastTagAttempt, now) < TagRetryMs)
        return true;
    route.lastTagAttempt = now;
    tank->SetSelection(target->GetGUID());
    DcActionShared::DcFaceIfNeeded(tank, target);
    if (!ai->CastSpell(opener->spellId, target))
    {
        DcActionShared::StallDungeonClear(ai, "Anub: waiting for the tank's ranged opener.");
        return true;
    }

    DcActionShared::ClearStall(context);
    DcActionShared::SetPhase(context, "Anub: ranged pull in place");
    LOG_INFO("playerbots.dungeonclear",
             "[DC:{}] Anub approach: in-place ranged tag on spawn {} (formation {}) with spell {} at {:.1f}yd",
             tank->GetName(), target->GetSpawnId(), formation.leader, opener->spellId, tank->GetExactDist(target));
    return true;
}
}
