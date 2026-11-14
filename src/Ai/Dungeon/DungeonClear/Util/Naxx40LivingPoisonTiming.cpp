/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, or later.
 */

#include "Naxx40LivingPoisonTiming.h"

#include "Ai/Raid/Naxx40/Naxx40Encounter.h"
#include "Creature.h"
#include "MoveSpline.h"
#include "Player.h"
#include "TemporarySummon.h"
#include "Timer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <list>

namespace DcNaxx40LivingPoisonTiming
{
namespace
{
    constexpr uint32 LivingPoison = 16027;
    constexpr uint32 NaxxramasTrigger = 16082;
    constexpr float ForecastSeconds = 6.0f;
    constexpr float BlastClearance = 5.3f;
    constexpr float TimingSlack = 0.060f;
    constexpr float WaveAgeTolerance = 0.200f;
    constexpr float PositionTolerance = 0.25f;
    constexpr float MaxSplineOriginAdjustment = 1.5f;

    // Individual Progression's LivingPoisonDataList. The spawn and lifetime
    // identify a lane. Its live spline supplies the actual path and speed.
    struct Lane
    {
        G3D::Vector3 spawn;
        uint32 lifetimeMs;
    };

    std::array<Lane, 3> const Lanes{{
        {{3128.590f, -3118.810f, 293.346f}, 15200},
        {{3154.250f, -3125.700f, 293.430f}, 14800},
        {{3175.420f, -3134.860f, 293.340f}, 14800},
    }};

    G3D::Vector3 AsVector(Position const& p)
    {
        return {p.GetPositionX(), p.GetPositionY(), p.GetPositionZ()};
    }
}

Snapshot::Snapshot(Player* observer)
{
    _capturedAt = getMSTime();
    if (!observer || !IsNaxx40Map(observer->FindMap()) ||
        !observer->IsInWorld() || !observer->IsAlive() ||
        observer->GetExactDist(3150.0f, -3140.0f, 294.0f) > 60.0f)
        return;

    std::list<Creature*> triggers;
    observer->GetCreatureListWithEntryInGrid(triggers, NaxxramasTrigger, 120.0f);
    std::erase_if(triggers, [&](Creature* trigger)
    {
        return !trigger->IsAlive() || !observer->InSamePhase(trigger);
    });
    if (triggers.size() != 1)
        return;

    std::list<Creature*> creatures;
    observer->GetCreatureListWithEntryInGrid(creatures, LivingPoison, 120.0f);

    ObjectGuid const summonerGuid = triggers.front()->GetGUID();
    for (Creature* creature : creatures)
    {
        if (!creature->IsAlive() || !observer->InSamePhase(creature))
            continue;

        TempSummon* summon = creature->ToTempSummon();
        if (!summon || summon->GetSummonerGUID() != summonerGuid ||
            summon->GetSummonType() != TEMPSUMMON_TIMED_DESPAWN)
            return;

        G3D::Vector3 const home = AsVector(creature->GetHomePosition());
        auto const lane = std::ranges::find_if(Lanes, [&](Lane const& candidate)
        {
            return (candidate.spawn - home).squaredLength() < PositionTolerance * PositionTolerance;
        });
        if (lane == Lanes.end())
            return;

        uint32 const remainingMs = summon->GetTimer();
        if (!remainingMs || remainingMs > lane->lifetimeMs)
            return;
        float const age = static_cast<float>(lane->lifetimeMs - remainingMs) / 1000.0f;
        float const remaining = static_cast<float>(remainingMs) / 1000.0f;
        Movement::MoveSpline const& spline = *creature->movespline;
        if (!spline.Initialized() || spline.isCyclic() || spline.onTransport || spline.isFalling() ||
            spline._Spline().mode() != Movement::SplineBase::ModeLinear)
            return;

        auto const& curve = spline._Spline();
        if ((curve.getPoint(curve.first()) - home).squaredLength() >
            MaxSplineOriginAdjustment * MaxSplineOriginAdjustment)
            return;

        float const elapsed = static_cast<float>(spline.timePassed()) / 1000.0f;
        float const duration = static_cast<float>(spline.Duration()) / 1000.0f;
        if (!std::isfinite(duration) || duration <= 0.0f ||
            std::abs(std::min(age, duration) - elapsed) > WaveAgeTolerance)
            return;

        G3D::Vector3 const current = AsVector(creature->GetPosition());
        G3D::Vector3 const expected = spline.Finalized() ? spline.FinalDestination() : spline.ComputePosition();
        if ((current - expected).squaredLength() > PositionTolerance * PositionTolerance)
            return;

        float const walkSpeed = creature->GetSpeed(MOVE_WALK);
        if (!std::isfinite(walkSpeed) || !std::isfinite(spline.Velocity()) ||
            walkSpeed <= 0.0f || std::abs(spline.Velocity() - walkSpeed) > 0.05f)
            return;
        auto& waveSpeed = _waveSpeed[static_cast<std::size_t>(lane - Lanes.begin())];
        waveSpeed = std::max(waveSpeed, std::max(walkSpeed, spline.Velocity()));

        float const clockOffset = spline.Finalized() ? -age : -elapsed;
        for (int i = curve.first(); i < curve.last(); ++i)
        {
            float const start = static_cast<float>(curve.length(i)) / 1000.0f + clockOffset;
            float const end = static_cast<float>(curve.length(i + 1)) / 1000.0f + clockOffset;
            if (end <= start)
            {
                if ((curve.getPoint(i + 1) - curve.getPoint(i)).squaredLength() > 0.00001f)
                    return;
                continue;
            }
            if (end >= -TimingSlack && start < remaining + TimingSlack)
                _poison.push_back({curve.getPoint(i), curve.getPoint(i + 1), start, end});
        }
        // The timed summon can sit at the end of its route before despawning.
        if (duration + clockOffset < remaining + TimingSlack)
            _poison.push_back({spline.FinalDestination(), spline.FinalDestination(),
                duration + clockOffset, remaining + TimingSlack});
    }

    _ready = std::ranges::all_of(_waveSpeed, [](float speed) { return speed > 0.0f; });
}

bool Snapshot::TooClose(Segment const& bot, Segment const& poison)
{
    if (bot.endTime < poison.startTime - TimingSlack || bot.startTime > poison.endTime + TimingSlack)
        return false;

    G3D::Vector3 const botVelocity = (bot.end - bot.start) / (bot.endTime - bot.startTime);
    G3D::Vector3 const poisonVelocity = (poison.end - poison.start) / (poison.endTime - poison.startTime);
    G3D::Vector3 const initial = bot.start - poison.start;
    float const botDuration = bot.endTime - bot.startTime;
    float const poisonDuration = poison.endTime - poison.startTime;
    float const earliestOffset = poison.startTime - bot.startTime - TimingSlack;
    float const latestOffset = poison.startTime - bot.startTime + TimingSlack;
    constexpr float ClearanceSquared = BlastClearance * BlastClearance;

    auto separation = [&](float t, float u)
    {
        return initial + botVelocity * t - poisonVelocity * u;
    };
    auto hitsBoundary = [&](float t, float u, float dt, float du)
    {
        G3D::Vector3 const position = separation(t, u);
        G3D::Vector3 const delta = botVelocity * dt - poisonVelocity * du;
        float const lengthSquared = delta.squaredLength();
        float const fraction = lengthSquared > 0.0f
            ? std::clamp(-position.dot(delta) / lengthSquared, 0.0f, 1.0f) : 0.0f;
        return (position + delta * fraction).squaredLength() <= ClearanceSquared;
    };

    // The permitted pairs of local times (t,u) lie inside the rectangle of
    // both segment durations, clipped to earliestOffset <= t-u <= latestOffset.
    // Minimum separation is attained on a boundary or at an interior minimum.
    for (float const t : {0.0f, botDuration})
    {
        float const lower = std::max(0.0f, t - latestOffset);
        float const upper = std::min(poisonDuration, t - earliestOffset);
        if (lower <= upper && hitsBoundary(t, lower, 0.0f, upper - lower))
            return true;
    }
    for (float const u : {0.0f, poisonDuration})
    {
        float const lower = std::max(0.0f, u + earliestOffset);
        float const upper = std::min(botDuration, u + latestOffset);
        if (lower <= upper && hitsBoundary(lower, u, upper - lower, 0.0f))
            return true;
    }
    for (float const offset : {earliestOffset, latestOffset})
    {
        float const lower = std::max(0.0f, offset);
        float const upper = std::min(botDuration, poisonDuration + offset);
        if (lower <= upper && hitsBoundary(lower, lower - offset, upper - lower, upper - lower))
            return true;
    }

    float const botSpeedSquared = botVelocity.squaredLength();
    float const poisonSpeedSquared = poisonVelocity.squaredLength();
    float const velocityDot = botVelocity.dot(poisonVelocity);
    float const determinant = botSpeedSquared * poisonSpeedSquared - velocityDot * velocityDot;
    if (determinant <= 0.00001f)
        return false; // Parallel or stationary: the minimum also lies on a boundary.
    float const botProjection = initial.dot(botVelocity);
    float const poisonProjection = initial.dot(poisonVelocity);
    float const t = (velocityDot * poisonProjection - poisonSpeedSquared * botProjection) / determinant;
    float const u = (botSpeedSquared * poisonProjection - velocityDot * botProjection) / determinant;
    return t > 0.0f && t < botDuration && u > 0.0f && u < poisonDuration &&
        t - u > earliestOffset && t - u < latestOffset &&
        separation(t, u).squaredLength() <= ClearanceSquared;
}

bool Snapshot::NewSummonCouldReach(Segment const& bot, G3D::Vector3 const& spawn, float speed)
{
    // A wave could spawn immediately after the snapshot. At each instant, its
    // maximum possible range from the spawn increases by its walking speed.
    // Solve against that growing sphere to cover any spawn time or path.
    float const duration = bot.endTime - bot.startTime;
    G3D::Vector3 const relative = bot.start - spawn;
    G3D::Vector3 const velocity = (bot.end - bot.start) / duration;
    float const radius = BlastClearance + speed * (bot.startTime + TimingSlack);
    float const a = velocity.squaredLength() - speed * speed;
    float const b = relative.dot(velocity) - radius * speed;
    float const c = relative.squaredLength() - radius * radius;
    if (a <= 0.0f)
        return c <= 0.0f || (a * duration + 2.0f * b) * duration + c <= 0.0f;
    float const t = std::clamp(-b / a, 0.0f, duration);
    return (a * t + 2.0f * b) * t + c <= 0.0f;
}

bool Snapshot::CanCross(Movement::PointsArray const& path, float runSpeed) const
{
    if (!_ready || getMSTime() - _capturedAt > 30 || path.size() < 2 ||
        !std::isfinite(runSpeed) || runSpeed <= 0.0f ||
        std::ranges::any_of(path, [](auto const& point) { return !point.isFinite(); }))
        return false;

    float elapsed = 0.0f;
    for (std::size_t i = 1; i < path.size(); ++i)
    {
        float const duration = (path[i] - path[i - 1]).magnitude() / runSpeed;
        if (duration < 0.001f)
        {
            if (duration > 0.0f)
                return false;
            continue;
        }
        // The core stores per-segment timestamps in integer milliseconds.
        float const roundedDuration = std::floor(duration * 1000.0f) / 1000.0f;
        Segment const bot{path[i - 1], path[i], elapsed, elapsed + roundedDuration};
        if (bot.endTime > ForecastSeconds)
            return false;
        for (std::size_t lane = 0; lane < Lanes.size(); ++lane)
            if (NewSummonCouldReach(bot, Lanes[lane].spawn, _waveSpeed[lane]))
                return false;
        for (Segment const& poison : _poison)
            if (TooClose(bot, poison))
                return false;
        elapsed = bot.endTime;
    }
    return elapsed > 0.0f;
}
}
