/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, or later.
 */

#include "DcNaxx40Embalming.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "Ai/Raid/Naxx40/Naxx40Encounter.h"
#include "Ai/Raid/RaidClear/Common/WayOut.h"
#include "Ai/Base/Value/AttackersValue.h"
#include "Ai/Dungeon/DungeonClear/Action/DcActionShared.h"
#include "CombatManager.h"
#include "Creature.h"
#include "DcEngageGeometry.h"
#include "DcLeaderSignal.h"
#include "DcMovement.h"
#include "DcNaxx40SlimeCrossing.h"
#include "DcPullContext.h"
#include "DcRun.h"
#include "DcTargeting.h"
#include "DcTickMemo.h"
#include "DungeonClearMath.h"
#include "Group.h"
#include "Map.h"
#include "MoveSpline.h"
#include "Player.h"
#include "Playerbots.h"
#include "Timer.h"

namespace
{
    constexpr uint32 EmbalmingSlime = 351026;
    constexpr float CloudMargin = 11.0f;
    constexpr float RangeMargin = 0.75f;
    Position const SouthCamp(3137.1f, -3268.5f, 294.374f);
    Position const SouthRetreat(3152.0f, -3270.0f, 294.559f);
    Position const ForwardCamp(3133.0f, -3257.0f, 294.504f);
    Position const RetreatTurn(3140.0f, -3260.0f, 294.539f);
    std::array<Position, 3> const SurvivorStands = {{
        {3126.5f, -3235.0f, 294.063f},
        {3127.0f, -3230.0f, 294.063f},
        {3127.5f, -3222.5f, 294.063f},
    }};

    G3D::Vector3 ToPoint(Position const& position)
    {
        return {position.GetPositionX(), position.GetPositionY(), position.GetPositionZ()};
    }

    Movement::PointsArray DryLeg(Player& tank, Position const& from, Position const& to)
    {
        G3D::Vector3 const start = ToPoint(from);
        G3D::Vector3 const end = ToPoint(to);
        if (!tank.GetMap()->isInLineOfSight(start.x, start.y, start.z + 0.75f, end.x, end.y, end.z + 0.75f,
                                            tank.GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS,
                                            VMAP::ModelIgnoreFlags::Nothing))
            return {};

        G3D::Vector3 const leg = end - start;
        int const samples = std::max(1, static_cast<int>(std::ceil(leg.length() / 0.5f)));
        for (int i = 0; i <= samples; ++i)
            if (!DcNaxx40SlimeCrossing::IsDryCampGround(tank, start + leg * (static_cast<float>(i) / samples)))
                return {};
        return {start, start + leg * 0.5f, end};
    }

    Movement::PointsArray RetreatPath(Player& tank, Position const& from, Position const& camp)
    {
        // The direct line from the northern stands to the tank's retreat cuts the corner.
        Movement::PointsArray path = DryLeg(tank, from, RetreatTurn);
        if (path.empty())
            return path;
        auto const last = DryLeg(tank, RetreatTurn, DcNaxx40Embalming::RetreatPoint(camp));
        if (last.empty())
            return {};
        path.insert(path.end(), last.begin() + 1, last.end());
        return path;
    }

    bool ClearsBoss(Player& tank, Movement::PointsArray const& path)
    {
        if (path.empty())
            return false;
        auto* ai = GET_PLAYERBOT_AI(&tank);
        if (!ai)
            return false;
        Creature* const boss =
            DcTargeting::GetLiveBoss(&tank, ai->GetAiObjectContext(), Naxx40PatchwerkEntry);
        if (!boss)
            return true;
        float const radius = DcEngageGeometry::RoomAggroSphereRadius(&tank, boss);
        for (std::size_t i = 1; i < path.size(); ++i)
            if (DungeonClearMath::DistSqToSegment2D(boss->GetPositionX(), boss->GetPositionY(),
                    path[i - 1].x, path[i - 1].y, path[i].x, path[i].y) < radius * radius)
                return false;
        return true;
    }

    Movement::PointsArray StandPath(Player& tank, Position const& stand)
    {
        auto path = DcNaxx40SlimeCrossing::BuildDryCampPath(&tank, stand);
        if (path.empty())
            path = DryLeg(tank, tank.GetPosition(), stand);
        return path;
    }

    bool SafeForwardCamp(Player& tank)
    {
        for (auto const& [id, creature] : tank.GetMap()->GetCreatureBySpawnIdStore())
            if (creature->IsAlive() && creature->IsInWorld() && tank.InSamePhase(creature) &&
                creature->IsHostileTo(&tank) &&
                std::abs(creature->GetPositionZ() - ForwardCamp.GetPositionZ()) < 3.0f &&
                creature->GetExactDist(&ForwardCamp) < DcEngageGeometry::AggroReach(&tank, creature, 6.0f) &&
                creature->IsWithinLOS(ForwardCamp.GetPositionX(), ForwardCamp.GetPositionY(),
                    ForwardCamp.GetPositionZ()))
                return false;
        return true;
    }

    bool IsMovingTo(Player& tank, Position const& end)
    {
        return DcMovement::OwnsRunningEscort(&tank) &&
            (tank.movespline->FinalDestination() - ToPoint(end)).squaredLength() < 0.25f;
    }

    bool DriveDryPath(PlayerbotAI* ai, Movement::PointsArray& path)
    {
        if (!ClearsBoss(*ai->GetBot(), path))
            return false;
        return DcMovement::SplinePath(ai, path, MovementPriority::MOVEMENT_COMBAT, FORCED_MOVEMENT_RUN);
    }

    void CollectSlimes(Player const& member, DcRunState::EmbalmingCamp& hold)
    {
        for (auto const& [guid, ref] : member.GetCombatManager().GetPvECombatRefs())
        {
            Unit const* const enemy = ref->GetOther(&member);
            if (enemy->GetEntry() == EmbalmingSlime && enemy->IsAlive() && !ref->IsSuppressedFor(enemy))
                hold.slimes.try_emplace(guid, enemy->GetPosition());
        }
    }

    DcRunState::EmbalmingCamp const* ReadCamp(Player* bot, Player*& owner)
    {
        if (!bot || !bot->IsAlive() || !IsNaxx40Map(bot->FindMap()))
            return nullptr;

        Group const* const group = bot->GetGroup();
        owner = group ? DcLeaderSignal::FindRunOwner(bot) : nullptr;
        if (!owner || !owner->IsInMap(bot))
            return nullptr;
        PlayerbotAI* const ownerAI = GET_PLAYERBOT_AI(owner);
        if (!ownerAI)
            return nullptr;

        AiObjectContext* const context = ownerAI->GetAiObjectContext();
        DcRunState& run = DcRun::Of(context);
        auto& hold = run.embalmingCamp;
        if (!context->GetValue<bool>(DcKey::PullMode)->Get() || run.paused)
        {
            hold = {};
            return nullptr;
        }

        if (hold.instanceId != bot->GetInstanceId() || hold.group != group->GetGUID())
        {
            hold = {};
            hold.instanceId = bot->GetInstanceId();
            hold.group = group->GetGUID();
        }

        uint32 const now = getMSTime();
        if (!DcTickMemo::MemoValid(hold.checkedAt, now))
        {
            hold.checkedAt = now;
            hold.slimes.clear();
            CollectSlimes(*owner, hold);
            // Slimes can switch victims or survive the puller.
            for (GroupReference const* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player const* const member = ref->GetSource();
                if (member && member != owner && member->IsInMap(owner))
                    CollectSlimes(*member, hold);
            }

            DcPullContext const& pull = context->GetValue<DcPullContext&>(DcKey::PullContext)->Get();
            if (hold.slimes.empty())
            {
                hold.camp.reset();
                hold.authored = false;
            }
            else if (pull.HasCamp())
            {
                hold.camp = pull.camp;
                hold.authored = pull.camp.GetExactDist(&SouthCamp) < 1.0f ||
                    DcNaxx40Embalming::IsForwardCamp(pull.camp);
            }
        }
        return hold.camp ? &hold : nullptr;
    }
}

bool DcNaxx40Embalming::IsSlime(Unit const* unit)
{
    return unit && unit->GetEntry() == EmbalmingSlime && IsNaxx40Map(unit->FindMap());
}

bool DcNaxx40Embalming::CanAttackFromCamp(Player* bot)
{
    return PlayerbotAI::IsRangedDps(bot) && !PlayerbotAI::IsTank(bot) && !PlayerbotAI::IsHeal(bot);
}

std::optional<DcNaxx40Embalming::SurvivorPull> DcNaxx40Embalming::FindSurvivorPull(
    PlayerbotAI* ai, ObjectGuid excluded, ObjectGuid preferred)
{
    Player* const tank = ai->GetBot();
    if (!IsNaxx40Map(tank->FindMap()) || tank->IsInCombat())
        return std::nullopt;
    auto const opener = DcActionShared::ResolvePullSpell(ai, tank);
    if (!opener)
        return std::nullopt;

    DcPullContext const& pull = ai->GetAiObjectContext()->GetValue<DcPullContext&>(DcKey::PullContext)->Get();
    bool const useForward = pull.fizzleCount && !pull.fizzleTarget.IsEmpty() &&
        pull.embalmingRetriedForward != pull.fizzleTarget && IsSurvivorCampSafe(tank, ForwardCamp);

    for (Position const& stand : SurvivorStands)
    {
        Creature* target = nullptr;
        float nearestRange = opener->maxRange - RangeMargin;
        for (auto const& [id, creature] : tank->GetMap()->GetCreatureBySpawnIdStore())
        {
            ObjectGuid const guid = creature->GetGUID();
            if (creature->GetEntry() != EmbalmingSlime || guid == excluded ||
                (!preferred.IsEmpty() && guid != preferred) || !tank->InSamePhase(creature) ||
                !AttackersValue::IsPossibleTarget(creature, tank))
                continue;
            float const range = creature->GetExactDist(&stand);
            if (range < opener->minRange + RangeMargin || range > nearestRange ||
                !creature->IsWithinLOS(stand.GetPositionX(), stand.GetPositionY(), stand.GetPositionZ()))
                continue;
            nearestRange = range;
            target = creature;
        }
        if (!target || !ClearsBoss(*tank, StandPath(*tank, stand)))
            continue;
        bool const forwardForTarget = useForward && target->GetGUID() == pull.fizzleTarget;
        Position const& camp = forwardForTarget ? ForwardCamp : SouthCamp;
        if (ClearsBoss(*tank, RetreatPath(*tank, stand, camp)))
            return SurvivorPull{target->GetGUID(), stand, camp};
    }
    return std::nullopt;
}

bool DcNaxx40Embalming::IsSurvivorCampSafe(Player* tank, Position const& camp)
{
    return !IsForwardCamp(camp) ||
        (SafeForwardCamp(*tank) && ClearsBoss(*tank, DryLeg(*tank, SouthCamp, ForwardCamp)));
}

bool DcNaxx40Embalming::IsForwardCamp(Position const& camp)
{
    return camp.GetExactDist(&ForwardCamp) < 1.0f;
}

bool DcNaxx40Embalming::DriveSurvivorStand(PlayerbotAI* ai, Position const& stand)
{
    Player* const tank = ai->GetBot();
    if (IsMovingTo(*tank, stand))
        return true;
    auto path = StandPath(*tank, stand);
    return DriveDryPath(ai, path);
}

bool DcNaxx40Embalming::DriveSurvivorReturn(PlayerbotAI* ai, Position const& camp)
{
    Player* const tank = ai->GetBot();
    if (IsMovingTo(*tank, RetreatPoint(camp)))
        return true;
    auto path = RetreatPath(*tank, tank->GetPosition(), camp);
    if (path.empty())
        path = DcNaxx40SlimeCrossing::BuildDryCampPath(tank, RetreatPoint(camp));
    return DriveDryPath(ai, path);
}

std::optional<Position> DcNaxx40Embalming::PullCamp(Player* tank, Unit* target)
{
    // The south firing line is dry and looks onto the inbound lane. Keep the
    // normal camp for pulls approached from the west or another floor.
    if (!IsSlime(target) || tank->GetPositionY() > -3245.0f || tank->GetExactDist(&SouthCamp) > 50.0f ||
        !DcEngageGeometry::IsNavReachableWithin(tank, SouthCamp, 1.5f, 3.0f) ||
        !DcEngageGeometry::IsNavReachableWithin(tank, SouthRetreat, 1.5f, 3.0f))
        return std::nullopt;
    return SouthCamp;
}

Position DcNaxx40Embalming::RetreatPoint(Position const& camp)
{
    return camp.GetExactDist(&SouthCamp) < 1.0f || IsForwardCamp(camp) ? SouthRetreat : camp;
}

std::optional<Position> DcNaxx40Embalming::GetCamp(Player* bot, bool* isPuller)
{
    Player* owner = nullptr;
    DcRunState::EmbalmingCamp const* const hold = ReadCamp(bot, owner);
    if (!hold)
        return std::nullopt;
    if (isPuller)
        *isPuller = bot == owner;
    if (bot == owner && hold->authored)
        return RetreatPoint(*hold->camp);
    return hold->camp;
}

bool DcNaxx40Embalming::FindSaferSpot(PlayerbotAI* ai, Position const& camp, Position& destination)
{
    Player* const bot = ai->GetBot();
    Player* owner = nullptr;
    DcRunState::EmbalmingCamp const* const hold = ReadCamp(bot, owner);
    if (!hold)
        return false;

    auto const gap = [&](float x, float y, float z)
    {
        float nearest = 100.0f;
        for (auto const& [guid, slime] : hold->slimes)
            nearest = std::min(nearest, slime.GetExactDist(x, y, z));
        return nearest;
    };
    if (gap(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()) < CloudMargin)
    {
        if (RaidClear::OnItsWayOut(bot, gap, CloudMargin))
        {
            G3D::Vector3 const end = bot->movespline->FinalDestination();
            destination.Relocate(end.x, end.y, end.z);
            return true;
        }
        return RaidClear::FindWayOut(ai, bot, gap, CloudMargin + 3.0f, 2.0f, destination);
    }
    return gap(camp.GetPositionX(), camp.GetPositionY(), camp.GetPositionZ()) >= CloudMargin;
}
