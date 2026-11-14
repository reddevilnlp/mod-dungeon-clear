/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, or later.
 */

#include "DcNaxx40SlimeCrossing.h"

#include "Ai/Raid/Naxx40/Naxx40Encounter.h"
#include "Ai/Raid/Naxx40/Naxx40Slime.h"
#include "DcMovement.h"
#include "DcRun.h"
#include "GameObject.h"
#include "Log.h"
#include "Map.h"
#include "MoveSpline.h"
#include "PathGenerator.h"
#include "Player.h"
#include "Playerbots.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <optional>
#include <utility>

namespace DcNaxx40SlimeCrossing
{
namespace
{
    struct Crossing
    {
        G3D::Vector3 start;
        G3D::Vector3 end;
        uint32 door;
        bool checkFloor = false;
        float floorProbeLift = 0.0f;
    };

    // Banks marked in-game or verified against terrain, in either route direction.
    std::array<Crossing, 3> const Crossings = {{
        // The mesh runs above the slime but cuts slightly below the raised lip.
        {{3088.4287f, -3219.822f, 294.21243f}, {3066.5212f, -3224.1794f, 294.06342f}, 0, true, 0.25f},
        {{3317.2993f, -3236.3027f, 294.06342f}, {3318.366f, -3263.8232f, 292.595f}, 181123},
        {{3052.70f, -3227.90f, 294.063f}, {3038.50f, -3231.40f, 294.063f}, 0, true},
    }};

    constexpr float BankRadius = 2.5f;
    constexpr float RouteRadius = 3.0f;
    constexpr float MaximumDistance = 33.0f;
    constexpr float SampleStep = 0.5f;
    constexpr float MaximumFloorGap = 2.0f;
    constexpr float MaximumWetSeconds = 2.5f;
    constexpr float MaximumCrossingSeconds = 7.5f;
    constexpr uint32 PatchwerkGate = 181123;
    constexpr float RecoveryDistance = 32.0f;
    constexpr float RecoveryWetDistance = 1.0f;
    constexpr float RecoveryWetDepth = 0.35f;
    constexpr float RecoveryFloorStep = 0.5f;
    constexpr float RecoverySampleStep = 0.25f;
    constexpr float GroundClearance = 0.05f;

    float DistanceToSegmentSquared(G3D::Vector3 const& point, G3D::Vector3 const& from, G3D::Vector3 const& to)
    {
        G3D::Vector3 const line = to - from;
        if (line.squaredLength() == 0.0f)
            return (point - from).squaredLength();

        float const fraction = std::clamp((point - from).dot(line) / line.squaredLength(), 0.0f, 1.0f);
        return (point - from - line * fraction).squaredLength();
    }

    bool RouteReachesBank(G3D::Vector3 current, G3D::Vector3 const& bank, Movement::PointsArray const& planned)
    {
        float length = 0.0f;
        for (G3D::Vector3 const& next : planned)
        {
            G3D::Vector3 const step = next - current;
            float const stepLength = step.length();
            float const remaining = RouteLookahead - length;
            if (stepLength > 0.0f)
            {
                G3D::Vector3 const end = current + step * std::min(1.0f, remaining / stepLength);
                if (DistanceToSegmentSquared(bank, current, end) <= RouteRadius * RouteRadius)
                    return true;
            }
            length += stepLength;
            if (length >= RouteLookahead)
                return false;
            current = next;
        }
        return false;
    }

    bool IsOpen(Player const& bot, Crossing const& crossing)
    {
        if (!crossing.door)
            return true;
        GameObject const* door = bot.FindNearestGameObject(crossing.door, MaximumDistance, true);
        return door && door->GetGoState() != GO_STATE_READY;
    }

    enum class Surface
    {
        Unusable,
        Dry,
        Slime,
    };

    Surface CheckSurface(Player const& bot, G3D::Vector3 const& point, bool checkFloor, float floorProbeLift = 0.0f)
    {
        PositionFullTerrainStatus terrain;
        bot.GetMap()->GetFullTerrainStatusForPosition(bot.GetPhaseMask(), point.x, point.y, point.z,
                                                       bot.GetCollisionHeight(), terrain);
        if (std::abs(point.z - terrain.floorZ) > MaximumFloorGap)
        {
            if (floorProbeLift <= 0.0f)
                return Surface::Unusable;

            PositionFullTerrainStatus raised;
            bot.GetMap()->GetFullTerrainStatusForPosition(bot.GetPhaseMask(), point.x, point.y, point.z + floorProbeLift,
                                                           bot.GetCollisionHeight(), raised);
            if (raised.floorZ <= point.z || raised.floorZ > point.z + floorProbeLift)
                return Surface::Unusable;
            terrain = raised;
        }
        bool const wetFloor = checkFloor && terrain.liquidInfo.Level > terrain.floorZ &&
                              terrain.liquidInfo.Status == LIQUID_MAP_ABOVE_WATER;
        if (!wetFloor && !(terrain.liquidInfo.Status & MAP_LIQUID_STATUS_IN_CONTACT))
            return Surface::Dry;
        return terrain.liquidInfo.Entry == Naxx40SlimeLiquidId ? Surface::Slime : Surface::Unusable;
    }

    char const* CrossingObstruction(Player const& bot, Movement::PointsArray const& path, float wetLimit,
                                    float pathLimit, Crossing const& crossing)
    {
        if (CheckSurface(bot, path.front(), crossing.checkFloor, crossing.floorProbeLift) != Surface::Dry ||
            CheckSurface(bot, path.back(), crossing.checkFloor, crossing.floorProbeLift) != Surface::Dry)
            return "bank is not dry";

        float total = 0.0f;
        float firstWet = 0.0f;
        float lastWet = 0.0f;
        bool foundSlime = false;
        for (std::size_t index = 1; index < path.size(); ++index)
        {
            G3D::Vector3 const& start = path[index - 1];
            G3D::Vector3 const& end = path[index];
            if (!bot.GetMap()->isInLineOfSight(start.x, start.y, start.z + 0.75f, end.x, end.y, end.z + 0.75f,
                                             bot.GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS,
                                             VMAP::ModelIgnoreFlags::Nothing))
                return "line of sight is blocked";

            G3D::Vector3 const line = end - start;
            float const length = line.length();
            int const samples = std::max(1, static_cast<int>(std::ceil(length / SampleStep)));
            for (int sample = 1; sample <= samples; ++sample)
            {
                float const fraction = static_cast<float>(sample) / static_cast<float>(samples);
                Surface const surface = CheckSurface(bot, start + line * fraction, crossing.checkFloor,
                                                     crossing.floorProbeLift);
                if (surface == Surface::Unusable)
                    return "path has unusable ground or liquid";
                if (surface == Surface::Slime)
                {
                    if (!foundSlime)
                        firstWet = total + length * fraction;
                    lastWet = total + length * fraction;
                    foundSlime = true;
                }
            }
            total += length;
            if (total > pathLimit || lastWet - firstWet + SampleStep > wetLimit)
                return "path exceeds crossing limits";
        }
        return foundSlime ? nullptr : "no slime was found along the path";
    }

    bool LaunchCrossing(PlayerbotAI& ai, Movement::PointsArray& points, Crossing const& crossing,
                        bool const reportFailure)
    {
        Player* bot = ai.GetBot();
        auto blocked = [&](char const* reason)
        {
            if (reportFailure && !DcRun::Of(&ai).Throttled(DcThrottle::Naxx40SlimeCrossingLog, 5000))
                LOG_DEBUG("playerbots.dungeonclear", "[DC:{}] Naxx40 slime crossing blocked at ({:.2f}, {:.2f}): {}",
                          bot->GetName(), points.front().x, points.front().y, reason);
            return false;
        };
        float const speed = std::min(bot->GetSpeed(MOVE_RUN), bot->GetSpeed(MOVE_SWIM));
        float const pathLimit = std::min(MaximumDistance, speed * MaximumCrossingSeconds);
        if (speed <= 0.0f)
            return blocked("movement speed is zero");
        if (char const* reason = CrossingObstruction(*bot, points, speed * MaximumWetSeconds, pathLimit, crossing))
            return blocked(reason);

        // The escort generator regenerates two-point paths. Keep the exact verified geometry.
        if (points.size() == 2)
            points.insert(points.begin() + 1, (points.front() + points.back()) * 0.5f);

        uint32 const previousSpline = bot->movespline->GetId();
        if (!DcMovement::SplinePath(&ai, points, MovementPriority::MOVEMENT_FORCED, FORCED_MOVEMENT_RUN) ||
            bot->movespline->GetId() == previousSpline)
            return blocked("movement did not launch");

        AuthorizeNaxx40SlimeCrossing(ai);
        LOG_DEBUG("playerbots.dungeonclear", "[DC:{}] Naxx40 slime crossing to ({:.2f}, {:.2f}, {:.2f})",
                  bot->GetName(), points.back().x, points.back().y, points.back().z);
        return true;
    }

    bool StartCrossing(PlayerbotAI& ai, G3D::Vector3 const& current, G3D::Vector3 const& bank, Crossing const& crossing)
    {
        PathGenerator path(ai.GetBot());
        path.SetUseStraightPath(true);
        if (!path.CalculatePath(bank.x, bank.y, bank.z) || path.GetPathType() != PATHFIND_NORMAL ||
            path.GetPath().size() < 2 || (path.GetActualEndPosition() - bank).squaredLength() > 1.0f)
        {
            if (crossing.floorProbeLift > 0.0f && !DcRun::Of(&ai).Throttled(DcThrottle::Naxx40SlimeCrossingLog, 5000))
                LOG_DEBUG("playerbots.dungeonclear", "[DC:{}] Naxx40 slime crossing blocked at ({:.2f}, {:.2f}): {}",
                          ai.GetBot()->GetName(), current.x, current.y, "navigation did not reach the bank");
            return false;
        }

        Movement::PointsArray points = path.GetPath();
        points.front() = current;
        return LaunchCrossing(ai, points, crossing, crossing.floorProbeLift > 0.0f);
    }

    struct RecoveryGround
    {
        float floor;
        bool wet;
    };

    std::optional<RecoveryGround> ProbeRecoveryGround(Player const& bot, G3D::Vector3 const& point)
    {
        // Probe just above the lip. The path follows the returned floor, not the
        // height of a straight line under a rising ledge or through another storey.
        PositionFullTerrainStatus terrain;
        bot.GetMap()->GetFullTerrainStatusForPosition(bot.GetPhaseMask(), point.x, point.y,
            point.z + RecoveryFloorStep, bot.GetCollisionHeight(), terrain);
        if (std::abs(point.z - terrain.floorZ) > RecoveryFloorStep)
            return std::nullopt;

        LiquidData const& liquid = terrain.liquidInfo;
        if (liquid.Status == LIQUID_MAP_NO_WATER || liquid.Level <= terrain.floorZ)
            return RecoveryGround{terrain.floorZ, false};
        if (liquid.Entry != Naxx40SlimeLiquidId || liquid.Level - terrain.floorZ > RecoveryWetDepth)
            return std::nullopt;
        return RecoveryGround{terrain.floorZ, true};
    }

    Movement::PointsArray ProbeRecoveryPath(Player const& bot, G3D::Vector3 const& start,
                                            G3D::Vector3 const& bend, G3D::Vector3 const& end, bool startsWet)
    {
        Movement::PointsArray points{start};
        bool escaped = !startsWet;
        float distance = 0.0f;
        for (G3D::Vector3 const& target : {bend, end})
        {
            G3D::Vector3 const from = points.back();
            G3D::Vector3 const horizontal(target.x - from.x, target.y - from.y, 0.0f);
            float const length = horizontal.length();
            if (length <= 0.01f)
                continue;
            int const samples = static_cast<int>(std::ceil(length / RecoverySampleStep));
            G3D::Vector3 const step = horizontal / static_cast<float>(samples);
            for (int sample = 1; sample <= samples; ++sample)
            {
                G3D::Vector3 const previous = points.back();
                G3D::Vector3 point(from.x + step.x * sample, from.y + step.y * sample, previous.z);
                auto const ground = ProbeRecoveryGround(bot, point);
                if (!ground)
                    return {};
                point.z = ground->floor + GroundClearance;
                distance += (point - previous).length();
                if (distance > RecoveryDistance || (!escaped && distance > RecoveryWetDistance) ||
                    (escaped && ground->wet))
                    return {};
                escaped = escaped || !ground->wet;
                if (!bot.GetMap()->isInLineOfSight(previous.x, previous.y, previous.z + 0.75f, point.x, point.y,
                                                  point.z + 0.75f, bot.GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS,
                                                  VMAP::ModelIgnoreFlags::Nothing))
                    return {};
                points.push_back(point);
            }
        }
        if (points.size() < 3 || !escaped || std::abs(points.back().z - end.z) > RecoveryFloorStep)
            return {};
        return points;
    }
}

bool TryGrobbulusCrossing(PlayerbotAI* ai)
{
    Player* const bot = ai->GetBot();
    G3D::Vector3 const current(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    Crossing const crossing{GrobbulusBankA, GrobbulusBankB, PatchwerkGate, true, 0.6f};
    if (!IsNaxx40Map(bot->FindMap()) || bot->IsInCombat() || !ai->CanMove() ||
        (current - crossing.start).squaredLength() > 0.75f * 0.75f || !IsOpen(*bot, crossing))
        return false;

    // The straight navmesh path interpolates above the liquid. These VMAP samples
    // follow the floor/lips and are validated again against the live map before launch.
    Movement::PointsArray points{
        current,
        {3317.682f, -3242.518f, 294.049f}, {3317.733f, -3243.979f, 293.679f},
        {3317.750f, -3244.465f, 293.553f}, {3317.800f, -3245.926f, 293.069f},
        {3317.953f, -3250.307f, 293.069f}, {3317.970f, -3250.794f, 293.085f},
        {3318.004f, -3251.767f, 293.556f}, {3318.021f, -3252.254f, 293.785f},
        crossing.end
    };
    return LaunchCrossing(*ai, points, crossing, true);
}

bool CrossingActive(Player* bot)
{
    PlayerbotAI* ai = bot && IsNaxx40Map(bot->FindMap()) ? GET_PLAYERBOT_AI(bot) : nullptr;
    return ai && IsNaxx40SlimeCrossingAuthorized(*ai);
}

bool IsDryCampGround(Player const& bot, G3D::Vector3 const& point)
{
    return CheckSurface(bot, point, true) == Surface::Dry;
}

Movement::PointsArray BuildSurveyedDryLeg(Player* bot, G3D::Vector3 const& end)
{
    G3D::Vector3 const start(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    G3D::Vector3 const leg = end - start;
    if (leg.squaredLength() > 32.0f * 32.0f ||
        !bot->GetMap()->isInLineOfSight(start.x, start.y, start.z + 0.75f, end.x, end.y, end.z + 0.75f,
                                        bot->GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS,
                                        VMAP::ModelIgnoreFlags::Nothing))
        return {};
    int const samples = std::max(1, static_cast<int>(std::ceil(leg.length() / SampleStep)));
    for (int sample = 0; sample <= samples; ++sample)
        if (CheckSurface(*bot, start + leg * (static_cast<float>(sample) / samples), true, 0.6f) != Surface::Dry)
            return {};
    return {start, start + leg * 0.5f, end};
}

Movement::PointsArray BuildDryCampPath(Player* bot, Position const& destination)
{
    G3D::Vector3 const end(destination.GetPositionX(), destination.GetPositionY(), destination.GetPositionZ());
    if (!IsDryCampGround(*bot, end))
        return {};

    PathGenerator path(bot);
    path.SetUseStraightPath(true);
    path.SetNavTerrainCost(NAV_WATER, 25.0f);
    if (!path.CalculatePath(end.x, end.y, end.z) || path.GetPathType() != PATHFIND_NORMAL ||
        path.GetPath().size() < 2 || (path.GetActualEndPosition() - end).squaredLength() > 1.0f)
        return {};

    Movement::PointsArray points = path.GetPath();
    points.front() = {bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
    if (!IsDryCampGround(*bot, points.front()))
        return {};
    for (std::size_t index = 1; index < points.size(); ++index)
    {
        G3D::Vector3 const& start = points[index - 1];
        G3D::Vector3 const leg = points[index] - start;
        int const samples = std::max(1, static_cast<int>(std::ceil(leg.length() / SampleStep)));
        for (int sample = 1; sample <= samples; ++sample)
            if (!IsDryCampGround(*bot, start + leg * (static_cast<float>(sample) / samples)))
                return {};
    }

    if (points.size() == 2)
        points.insert(points.begin() + 1, (points.front() + points.back()) * 0.5f);
    return points;
}

Movement::PointsArray BuildPatchwerkRecoveryPath(Player* bot, Position const& destination)
{
    if (!bot || !IsNaxx40Map(bot->FindMap()) || !bot->IsAlive() || bot->IsInCombat() ||
        bot->IsFlying() || bot->IsFalling() || bot->GetVehicle() || bot->GetTransport())
        return {};

    G3D::Vector3 const start(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    G3D::Vector3 const end(destination.GetPositionX(), destination.GetPositionY(), destination.GetPositionZ());
    G3D::Vector3 const horizontal(end.x - start.x, end.y - start.y, 0.0f);
    float const distance = horizontal.length();
    if (distance < 0.5f || distance > RecoveryDistance || std::abs(end.z - start.z) > 2.0f)
        return {};
    auto const startGround = ProbeRecoveryGround(*bot, start);
    auto const endGround = ProbeRecoveryGround(*bot, end);
    if (!startGround || !endGround || endGround->wet)
        return {};

    // The recorded recovery between the two crossings works with an X-first
    // bend. The other bends cover short wall and pool edges without a grid scan.
    G3D::Vector3 const middle = (start + end) * 0.5f;
    G3D::Vector3 const side = G3D::Vector3(-horizontal.y, horizontal.x, 0.0f) * (4.0f / distance);
    std::array<G3D::Vector3, 5> const candidates = {{
        middle, {end.x, start.y, middle.z}, {start.x, end.y, middle.z}, middle + side, middle - side,
    }};
    Movement::PointsArray best;
    float bestDistance = RecoveryDistance;
    for (G3D::Vector3 const& bend : candidates)
    {
        float const length = std::hypot(bend.x - start.x, bend.y - start.y) +
                             std::hypot(end.x - bend.x, end.y - bend.y);
        if (length > RecoveryDistance || (!best.empty() && length >= bestDistance))
            continue;
        auto path = ProbeRecoveryPath(*bot, start, bend, end, startGround->wet);
        if (!path.empty())
        {
            best = std::move(path);
            bestDistance = length;
        }
    }
    return best;
}

bool IsNearBank(Player const* bot)
{
    if (!bot || !IsNaxx40Map(bot->FindMap()) || !bot->IsAlive() || bot->IsInCombat())
        return false;

    G3D::Vector3 const current(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    return std::any_of(Crossings.begin(), Crossings.end(), [&](Crossing const& crossing)
    {
        return (current - crossing.start).squaredLength() <= BankRadius * BankRadius ||
               (current - crossing.end).squaredLength() <= BankRadius * BankRadius;
    });
}

bool TryCross(PlayerbotAI* ai, Movement::PointsArray const& planned)
{
    Player* bot = ai ? ai->GetBot() : nullptr;
    if (!IsNearBank(bot) || !ai->CanMove() || bot->IsFlying() || bot->IsFalling() || bot->GetVehicle() ||
        bot->GetTransport() || bot->HasUnitState(UNIT_STATE_NOT_MOVE) || bot->HasUnitFlag(UNIT_FLAG_DISABLE_MOVE) ||
        planned.empty())
        return false;
    if (CrossingActive(bot))
        return true;

    G3D::Vector3 const current(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    for (Crossing const& crossing : Crossings)
    {
        if ((current - crossing.start).squaredLength() <= BankRadius * BankRadius && IsOpen(*bot, crossing) &&
            RouteReachesBank(current, crossing.end, planned))
            return StartCrossing(*ai, current, crossing.end, crossing);
        if ((current - crossing.end).squaredLength() <= BankRadius * BankRadius && IsOpen(*bot, crossing) &&
            RouteReachesBank(current, crossing.start, planned))
            return StartCrossing(*ai, current, crossing.start, crossing);
    }
    return false;
}

bool TryCrossTowards(PlayerbotAI* ai, G3D::Vector3 const& destination)
{
    Player* bot = ai ? ai->GetBot() : nullptr;
    if (!IsNearBank(bot))
        return false;

    PathGenerator planned(bot);
    if (!planned.CalculatePath(destination.x, destination.y, destination.z) ||
        planned.GetPathType() != PATHFIND_NORMAL)
        return false;
    return TryCross(ai, planned.GetPath());
}
}
