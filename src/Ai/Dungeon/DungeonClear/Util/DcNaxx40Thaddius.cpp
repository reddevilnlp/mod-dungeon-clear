/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, or later.
 */

#include "DcNaxx40Thaddius.h"

#include "Ai/Dungeon/DungeonClear/Action/DcActionShared.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Raid/Naxx40/Naxx40Encounter.h"
#include "Ai/Raid/Naxx40/Naxx40Slime.h"
#include "Ai/Raid/Naxx40/Thaddius40.h"
#include "AllMapScript.h"
#include "DcCombatFlag.h"
#include "DcLeaderSignal.h"
#include "DcModuleEnable.h"
#include "DcMovement.h"
#include "DcNaxx40SlimeCrossing.h"
#include "DcPbCompat.h"
#include "DcPlayerbotCompat.h"
#include "DcRezRecovery.h"
#include "DcRun.h"
#include "Group.h"
#include "InstanceScript.h"
#include "Log.h"
#include "Map.h"
#include "MapReference.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "MovementTypedefs.h"
#include "ObjectAccessor.h"
#include "PathGenerator.h"
#include "Player.h"
#include "Timer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace DcNaxx40Thaddius
{
namespace
{
    using Opening = DcRunState::ThaddiusOpening;
    using Phase = Opening::Phase;
    using Exit = DcRunState::ThaddiusExit;
    using ExitStage = Exit::Member::Stage;

    constexpr uint32 GluthEncounter = 2;
    G3D::Vector3 const Camp{3433.0f, -3005.0f, 295.613f};
    std::array<G3D::Vector3, 2> const Holds{{
        {3428.340f, -2958.770f, 309.298f}, {3480.736f, -3011.429f, 309.241f},
    }};
    std::array<G3D::Vector3, 2> const PullPoints{{
        {3442.920f, -2940.690f, 312.021f}, {3498.890f, -2996.340f, 312.021f},
    }};

    G3D::Vector3 const BossPlatform{3513.840f, -2926.550f, 302.914f};
    G3D::Vector3 const ExitLip{3499.381f, -2940.581f, 303.855f};
    G3D::Vector3 const ExitLanding{3487.540f, -2952.072f, 292.290f};
    std::array<G3D::Vector3, 14> const ExitRoute{{
        {3483.234f, -2956.251f, 292.290f}, {3476.058f, -2963.215f, 292.308f},
        {3468.882f, -2970.179f, 292.294f}, {3461.705f, -2977.143f, 292.299f},
        {3448.070f, -2990.375f, 292.293f}, {3446.994f, -2991.420f, 292.766f},
        {3446.635f, -2991.768f, 292.949f}, {3445.917f, -2992.464f, 293.412f},
        {3444.482f, -2993.857f, 294.761f}, {3443.047f, -2995.250f, 296.110f},
        {3442.329f, -2995.947f, 296.451f}, {3439.459f, -2998.732f, 297.320f},
        {3435.871f, -3002.214f, 296.179f}, Camp,
    }};
    constexpr std::size_t ExitDryIndex = 7;
    constexpr unsigned ExitWaveSize = 5;
    constexpr uint32 ExitMotionLimit = 14500;
    constexpr uint32 ExitProgressLimit = 30000;
    constexpr uint32 ExitDryMotionLimit = 8000;
    constexpr float ExitJumpApex = 3.5f;
    constexpr float ExitGroundClearance = 0.06f;

    InstanceScript* GetInstance(Player* bot)
    {
        return bot && IsNaxx40Map(bot->FindMap()) ? bot->GetInstanceScript() : nullptr;
    }

    bool Near(Player const& player, G3D::Vector3 const& point, float radius)
    {
        return player.GetExactDist2dSq(point.x, point.y) <= radius * radius &&
            std::abs(player.GetPositionZ() - point.z) < 2.0f;
    }

    bool OwnSpline(Player const& player, DcRunState const& run)
    {
        return run.naxx40ThaddiusSpline && !player.movespline->Finalized() &&
            player.movespline->GetId() == run.naxx40ThaddiusSpline;
    }

    PlayerbotAI* DrivingAI(Player& player, DcRunState const& local)
    {
        Player* owner = ObjectAccessor::GetPlayer(player, local.naxx40ThaddiusOwner);
        if (!owner || !player.GetGroup() || !owner->IsInMap(&player) || owner->GetGroup() != player.GetGroup() ||
            !player.InSamePhase(owner))
            return nullptr;
        PlayerbotAI* ai = GET_PLAYERBOT_AI(owner);
        if (!ai)
            return nullptr;
        DcRunState const& run = DcRun::Of(ai);
        Opening const& opening = run.naxx40ThaddiusOpening;
        if (!run.enabled || run.paused || opening.instanceId != player.GetInstanceId() ||
            opening.group != player.GetGroup()->GetGUID())
            return nullptr;
        return ai;
    }

    bool CanTravel(PlayerbotAI& ai)
    {
        Player const* player = ai.GetBot();
        return ai.CanMove() && !player->GetVehicle() && !player->GetTransport() && !player->IsFlying() &&
            !player->IsFalling() && !player->HasUnitFlag(UNIT_FLAG_DISABLE_MOVE);
    }

    Movement::PointsArray DryPath(Player& player, G3D::Vector3 const& target)
    {
        if (!DcNaxx40SlimeCrossing::IsDryCampGround(player, target))
            return {};
        PathGenerator path(&player);
        path.SetExcludeFlags(NAV_WATER | NAV_MAGMA | NAV_SLIME | NAV_GROUND_STEEP);
        // Preserve the height changes over the crest on each ramp.
        if (!path.CalculatePath(target.x, target.y, target.z) || path.GetPathType() != PATHFIND_NORMAL ||
            path.GetPath().size() < 2 || (path.GetActualEndPosition() - target).length() > 1.2f ||
            path.getPathLength() > 110.0f)
            return {};
        Movement::PointsArray points = path.GetPath();
        points.front() = {player.GetPositionX(), player.GetPositionY(), player.GetPositionZ()};
        points.back() = target;
        for (std::size_t i = 1; i < points.size(); ++i)
        {
            auto const& a = points[i - 1];
            auto const& b = points[i];
            if (!player.GetMap()->isInLineOfSight(a.x, a.y, a.z + 0.85f, b.x, b.y, b.z + 0.85f,
                player.GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing))
                return {};
        }
        // The core recomputes two-point escort paths. Preserve the validated leg.
        if (points.size() == 2)
            points.insert(points.begin() + 1, (points.front() + points.back()) * 0.5f);
        return points;
    }

    bool Walk(PlayerbotAI& ai, Movement::PointsArray& path)
    {
        if (path.empty() || !DcMovement::SplinePath(&ai, path, MovementPriority::MOVEMENT_NORMAL, FORCED_MOVEMENT_RUN))
            return false;
        DcRun::Of(&ai).naxx40ThaddiusSpline = ai.GetBot()->movespline->GetId();
        return true;
    }

    void MoveTo(PlayerbotAI& ai, G3D::Vector3 const& target, uint32 now)
    {
        DcRunState& run = DcRun::Of(&ai);
        Player& member = *ai.GetBot();
        if (!CanTravel(ai) || OwnSpline(member, run) || now - run.naxx40ThaddiusLastMove < 1400)
            return;
        run.naxx40ThaddiusLastMove = now;
        Movement::PointsArray path = DryPath(member, target);
        Walk(ai, path);
    }

    void Release(Player& member, DcRunState& local, bool handoff)
    {
        if (!handoff && OwnSpline(member, local) && !member.IsFalling())
            DcMovement::StopBot(&member, DcMovement::Stop::Hold);
        if (handoff)
            DcMovement::ClearMovementWait(&member);
        local.naxx40ThaddiusOwner.Clear();
        local.naxx40ThaddiusSpline = 0;
        local.naxx40ThaddiusLastMove = 0;
    }

    void Drive(PlayerbotAI& ai, InstanceScript& instance, uint32 now)
    {
        Player& owner = *ai.GetBot();
        Opening& opening = DcRun::Of(&ai).naxx40ThaddiusOpening;
        AiObjectContext* context = ai.GetAiObjectContext();
        if (opening.instanceId != owner.GetInstanceId() || opening.group != owner.GetGroup()->GetGUID())
        {
            opening = {};
            opening.instanceId = owner.GetInstanceId();
            opening.group = owner.GetGroup()->GetGUID();
            DcActionShared::ClearStall(context);
            LOG_INFO("playerbots.dungeonclear", "[DC:{}] Thaddius opening armed, instance {}",
                owner.GetName(), opening.instanceId);
        }
        if (now - opening.lastUpdate < 250)
            return;
        opening.lastUpdate = now;
        opening.clearingTrash = DcCombatFlag::AnyPartyEngagement(&owner) ||
            DcActionShared::PullOwnsTheTank(&owner, context, "Thaddius opening");
        if (opening.clearingTrash)
        {
            DcActionShared::SetPhase(context, "Thaddius: clearing trash before the split");
            return;
        }

        std::array<Player*, 2> tanks{};
        std::array<unsigned, 2> assigned{}, ready{}, heals{};
        unsigned campReady = 0;
        unsigned bots = 0;
        unsigned selfbots = 0;
        for (GroupReference* ref = owner.GetGroup()->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || !member->IsAlive() || !member->IsInWorld() || !member->IsInMap(&owner) ||
                !member->InSamePhase(&owner))
                continue;
            Thaddius40::OpeningAssignment const assignment = Thaddius40::AssignOpening(member);
            uint8 const side = assignment.side;
            if (assignment.tank)
                tanks[side] = member;
            PlayerbotAI* memberAI = GET_PLAYERBOT_AI(member);
            bool const atHold = Near(*member, Holds[side], assignment.tank ? 2.0f : 3.0f);
            bool const atCamp = Near(*member, Camp, 5.0f) || atHold;
            if (memberAI)
            {
                ++bots;
                selfbots += DcPlayerbotCompat::IsSelfBot(member);
                campReady += atCamp;
                ++assigned[side];
                ready[side] += atHold;
                DcRunState& local = DcRun::Of(memberAI);
                if (local.naxx40ThaddiusOwner != owner.GetGUID())
                {
                    local.naxx40ThaddiusOwner = owner.GetGUID();
                    if (!member->IsFalling())
                        DcMovement::StopBot(member, DcMovement::Stop::Hold);
                }
                if (opening.phase == Phase::Camp && !atCamp)
                    MoveTo(*memberAI, Camp, now);
                else if (opening.phase == Phase::Platforms && !atHold)
                    MoveTo(*memberAI, Holds[side], now);
                else if (opening.phase == Phase::Pull && assignment.tank &&
                    !Near(*member, PullPoints[side], 2.0f))
                    MoveTo(*memberAI, PullPoints[side], now);
            }
            if (atHold && PlayerbotAI::IsHeal(member))
                ++heals[side];
        }

        if (opening.phase == Phase::Camp && bots && campReady == bots)
        {
            opening.phase = Phase::Platforms;
            LOG_INFO("playerbots.dungeonclear", "[DC:{}] Thaddius camp ready, splitting {} bots ({} selfbots): {}/{}",
                owner.GetName(), bots, selfbots, assigned[0], assigned[1]);
        }

        auto const report = [&]
        {
            if (now - opening.lastReport < 10000)
                return;
            opening.lastReport = now;
            LOG_INFO("playerbots.dungeonclear",
                "[DC:{}] Thaddius opening status phase={} camp={}/{} sides={}/{}:{}/{} healers={}/{} tanks={}/{}",
                owner.GetName(), static_cast<unsigned>(opening.phase), campReady, bots,
                ready[0], assigned[0], ready[1], assigned[1], heals[0], heals[1], bool(tanks[0]), bool(tanks[1]));
        };
        report();
        if (opening.phase == Phase::Platforms && tanks[0] && tanks[1] &&
            Near(*tanks[0], Holds[0], 2.0f) && Near(*tanks[1], Holds[1], 2.0f) &&
            heals[0] && heals[1] && ready == assigned &&
            instance.GetCreature(Thaddius40::StalaggData) && instance.GetCreature(Thaddius40::FeugenData))
        {
            if (now - DcRun::Of(&ai).naxx40ThaddiusLastMove < 1000)
                return;
            std::array<Movement::PointsArray, 2> paths;
            for (uint8 side = 0; side < tanks.size(); ++side)
                if (PlayerbotAI* tankAI = GET_PLAYERBOT_AI(tanks[side]))
                {
                    if (!CanTravel(*tankAI) || OwnSpline(*tanks[side], DcRun::Of(tankAI)))
                        return;
                    paths[side] = DryPath(*tanks[side], PullPoints[side]);
                    if (paths[side].empty())
                    {
                        DcRun::Of(&ai).naxx40ThaddiusLastMove = now;
                        DcActionShared::SetPhase(context, "Thaddius: a tank has no dry path to its add");
                        LOG_INFO("playerbots.dungeonclear", "[DC:{}] Thaddius: tank on side {} has no dry pull path",
                            owner.GetName(), side);
                        return;
                    }
                }
            opening.phase = Phase::Pull;
            for (uint8 side = 0; side < tanks.size(); ++side)
                if (PlayerbotAI* tankAI = GET_PLAYERBOT_AI(tanks[side]))
                    Walk(*tankAI, paths[side]);
            LOG_INFO("playerbots.dungeonclear",
                "[DC:{}] Thaddius paired pull, Stalagg {}/{} ({} heal), Feugen {}/{} ({} heal)",
                owner.GetName(), ready[0], assigned[0], heals[0], ready[1], assigned[1], heals[1]);
        }

        DcActionShared::SetPhase(context, opening.phase == Phase::Camp ? "Thaddius: reuniting at the fork" :
            opening.phase == Phase::Platforms ? "Thaddius: preparing both sides" : "Thaddius: tanks pulling both adds");
    }

    G3D::Vector3 Location(Player const& player)
    {
        return {player.GetPositionX(), player.GetPositionY(), player.GetPositionZ()};
    }

    bool ExitActive(Player const& owner, DcRunState const& run)
    {
        Exit const& exit = run.naxx40ThaddiusExit;
        return run.enabled && !run.paused && !exit.complete && !exit.members.empty() &&
            owner.GetGroup() && exit.group == owner.GetGroup()->GetGUID() && exit.instanceId == owner.GetInstanceId();
    }

    bool ExitCampHold(Player const& owner, DcRunState const& run)
    {
        Exit const& exit = run.naxx40ThaddiusExit;
        return run.enabled && exit.complete && owner.GetGroup() && exit.group == owner.GetGroup()->GetGUID() &&
            exit.instanceId == owner.GetInstanceId() && Near(owner, Camp, 6.0f);
    }

    bool ClearExitLine(Player const& bot, G3D::Vector3 const& from, G3D::Vector3 const& to, float lift)
    {
        return bot.GetMap()->isInLineOfSight(from.x, from.y, from.z + lift, to.x, to.y, to.z + lift,
            bot.GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing);
    }

    bool CheckExitGround(Player const& bot, G3D::Vector3 const& point, bool& wet)
    {
        PositionFullTerrainStatus terrain;
        bot.GetMap()->GetFullTerrainStatusForPosition(bot.GetPhaseMask(), point.x, point.y, point.z + 0.75f,
            bot.GetCollisionHeight(), terrain);
        if (std::abs(point.z - terrain.floorZ) > 0.45f)
            return false;
        LiquidData const& liquid = terrain.liquidInfo;
        wet = liquid.Status != LIQUID_MAP_NO_WATER && liquid.Level > terrain.floorZ + 0.02f;
        return !wet || (liquid.Entry == Naxx40SlimeLiquidId && liquid.Level - terrain.floorZ < 0.65f);
    }

    Movement::PointsArray ExitLeg(Player const& bot, bool crossing)
    {
        Movement::PointsArray points{Location(bot)};
        std::size_t begin = 0;
        if (!crossing)
        {
            bool wet = false;
            if (!CheckExitGround(bot, points.front(), wet) || wet)
                return {};
            float best = 0.8f * 0.8f;
            for (std::size_t i = ExitDryIndex + 1; i < ExitRoute.size(); ++i)
            {
                G3D::Vector3 const& from = ExitRoute[i - 1];
                G3D::Vector3 const step = ExitRoute[i] - from;
                float const fraction = std::clamp((points.front() - from).dot(step) / step.squaredLength(), 0.0f, 1.0f);
                float const distance = (points.front() - from - step * fraction).squaredLength();
                if (distance <= best)
                {
                    best = distance;
                    begin = i;
                }
            }
            if (!begin)
                return {};
        }
        std::size_t const end = crossing ? ExitDryIndex + 1 : ExitRoute.size();
        bool dry = !crossing;
        for (std::size_t i = begin; i < end; ++i)
        {
            G3D::Vector3 goal = ExitRoute[i];
            goal.z += ExitGroundClearance;
            G3D::Vector3 const from = points.back();
            if (!ClearExitLine(bot, from, goal, 0.2f) || !ClearExitLine(bot, from, goal, 0.85f))
                return {};
            G3D::Vector3 const step = goal - from;
            int const samples = std::max(1, static_cast<int>(std::ceil(step.length() / 0.5f)));
            for (int sample = 1; sample <= samples; ++sample)
            {
                bool wet = false;
                if (!CheckExitGround(bot, from + step * (static_cast<float>(sample) / samples), wet) || (dry && wet))
                    return {};
                dry |= !wet;
            }
            points.push_back(goal);
        }
        if (points.size() == 2)
            points.insert(points.begin() + 1, (points.front() + points.back()) * 0.5f);
        return dry ? points : Movement::PointsArray{};
    }

    bool ExitSpline(Player const& bot, Exit::Member const& member)
    {
        return member.spline && !bot.movespline->Finalized() && bot.movespline->GetId() == member.spline;
    }

    bool LaunchExitJump(PlayerbotAI& ai, Exit::Member& member, uint32 now)
    {
        Player& bot = *ai.GetBot();
        G3D::Vector3 const start = Location(bot);
        G3D::Vector3 landing = ExitLanding;
        landing.z += ExitGroundClearance;
        bool wet = false;
        if (!CheckExitGround(bot, landing, wet) || !wet ||
            bot.GetMotionMaster()->GetMotionSlotType(MOTION_SLOT_CONTROLLED) != NULL_MOTION_TYPE)
            return false;

        // Recheck the surveyed arc from the member's actual position, including dynamic collisions.
        G3D::Vector3 previous = start;
        constexpr int Samples = 40;
        for (int i = 1; i <= Samples; ++i)
        {
            float const t = static_cast<float>(i) / Samples;
            G3D::Vector3 point = start + (landing - start) * t;
            point.z += 4.0f * ExitJumpApex * t * (1.0f - t);
            if (!ClearExitLine(bot, previous, point, 0.2f) || !ClearExitLine(bot, previous, point, 0.85f))
                return false;
            previous = point;
        }

        if (bot.IsNonMeleeSpellCast(true))
            bot.CastStop();
        uint32 const before = bot.movespline->GetId();
        bot.GetMotionMaster()->Clear();
        bot.GetMotionMaster()->MoveJump(landing.x, landing.y, landing.z, bot.GetSpeed(MOVE_RUN),
            std::sqrt(2.0f * Movement::gravity * ExitJumpApex));
        if (bot.movespline->Finalized() || bot.movespline->GetId() == before)
            return false;

        member.stage = ExitStage::Jump;
        member.spline = bot.movespline->GetId();
        member.departure = now;
        member.lastAttempt = now;
        DcPbCompat::RecordMovement(
            ai.GetAiObjectContext()->GetValue<LastMovement&>(DcKey::Stock::LastMovement)->Get(),
            bot.GetMapId(), landing.x, landing.y, landing.z, bot.GetOrientation(), bot.movespline->Duration(),
            MovementPriority::MOVEMENT_FORCED);
        LOG_INFO("playerbots.dungeonclear",
            "[DC:{}] Thaddius exit jump instance {} selfbot={} hp={}/{} run={:.2f} swim={:.2f} waterWalk={}",
            bot.GetName(), bot.GetInstanceId(), DcPlayerbotCompat::IsSelfBot(&bot), bot.GetHealth(), bot.GetMaxHealth(),
            bot.GetSpeed(MOVE_RUN), bot.GetSpeed(MOVE_SWIM), bot.HasWaterWalkAura());
        return true;
    }

    void FailExitMember(Player& bot, Exit::Member& member, char const* reason)
    {
        member.stage = ExitStage::Failed;
        LOG_INFO("playerbots.dungeonclear",
            "[DC:{}] Thaddius exit member blocked instance {} at ({:.2f},{:.2f},{:.2f}): {}",
            bot.GetName(), bot.GetInstanceId(), bot.GetPositionX(), bot.GetPositionY(), bot.GetPositionZ(), reason);
    }

    bool LaunchExitRamp(PlayerbotAI& ai, Exit::Member& member)
    {
        Player& bot = *ai.GetBot();
        auto path = ExitLeg(bot, false);
        if (path.empty() || !DcMovement::SplinePath(&ai, path, MovementPriority::MOVEMENT_FORCED, FORCED_MOVEMENT_RUN))
            return false;
        member.spline = bot.movespline->GetId();
        // Arrival can be processed before the final auras and movement triggers
        // update. Keep the checked dry spline through that transition.
        AuthorizeNaxx40SlimeCrossing(ai);
        return true;
    }

    bool DriveExitMember(PlayerbotAI& ai, Exit::Member& member, uint32 now)
    {
        Player& bot = *ai.GetBot();
        if (member.stage != ExitStage::Approach &&
            now - member.lastAttempt >= (member.stage == ExitStage::Crossing ? ExitMotionLimit : ExitDryMotionLimit))
        {
            FailExitMember(bot, member, "movement timed out");
            return false;
        }
        if (member.stage == ExitStage::Crossing || member.stage == ExitStage::Jump)
        {
            if (bot.HasAura(28801) && !member.wetSince)
            {
                member.wetSince = now;
                LOG_INFO("playerbots.dungeonclear", "[DC:{}] Thaddius exit slime entered instance {} hp={}/{}",
                    bot.GetName(), bot.GetInstanceId(), bot.GetHealth(), bot.GetMaxHealth());
            }
            if (member.stage == ExitStage::Crossing && ExitSpline(bot, member) &&
                !member.renewed && now - member.lastAttempt >= 6500 && now - member.lastAttempt < 7500)
            {
                // The generic crossing authorization is capped at eight seconds. This one extension
                // belongs to the same still-running, checked spline through the surveyed shallow pool.
                member.renewed = true;
                AuthorizeNaxx40SlimeCrossing(ai);
            }
        }
        if (ExitSpline(bot, member) || !CanTravel(ai))
            return false;
        if (member.stage == ExitStage::Approach)
        {
            if (Near(bot, ExitLip, 0.65f))
                return bot.GetHealthPct() >= 85.0f;
            if (now - member.lastAttempt >= 1400)
            {
                member.lastAttempt = now;
                auto path = DryPath(bot, ExitLip);
                if (!path.empty() && DcMovement::SplinePath(&ai, path, MovementPriority::MOVEMENT_NORMAL, FORCED_MOVEMENT_RUN))
                    member.spline = bot.movespline->GetId();
            }
            return false;
        }
        if (member.stage == ExitStage::Jump)
        {
            if (!Near(bot, ExitLanding, 0.8f) || std::abs(bot.GetPositionZ() - ExitLanding.z) > 0.6f)
            {
                FailExitMember(bot, member, "jump did not reach the surveyed landing");
                return false;
            }
            auto path = ExitLeg(bot, true);
            float const speed = std::min(bot.GetSpeed(MOVE_RUN), bot.GetSpeed(MOVE_SWIM));
            if (path.empty() || speed <= 0.0f || 59.0f * 1000.0f > speed * ExitMotionLimit ||
                !DcMovement::SplinePath(&ai, path, MovementPriority::MOVEMENT_FORCED, FORCED_MOVEMENT_RUN) ||
                bot.movespline->Duration() > static_cast<int32>(ExitMotionLimit))
            {
                FailExitMember(bot, member, "slime path was blocked or exceeded its travel budget");
                return false;
            }
            member.stage = ExitStage::Crossing;
            member.spline = bot.movespline->GetId();
            member.lastAttempt = now;
            AuthorizeNaxx40SlimeCrossing(ai);
            return false;
        }
        if (member.stage == ExitStage::Crossing)
        {
            bool wet = false;
            if (!Near(bot, ExitRoute[ExitDryIndex], 0.8f) || !CheckExitGround(bot, Location(bot), wet) || wet)
            {
                FailExitMember(bot, member, "crossing spline ended before the dry ramp");
                return false;
            }
            LOG_INFO("playerbots.dungeonclear",
                "[DC:{}] Thaddius exit ramp reached instance {} crossingMs={} slimeMs={} hp={}/{}",
                bot.GetName(), bot.GetInstanceId(), now - member.lastAttempt, member.wetSince ? now - member.wetSince : 0,
                bot.GetHealth(), bot.GetMaxHealth());
            member.stage = ExitStage::Ramp;
            member.lastAttempt = now;
            if (!LaunchExitRamp(ai, member))
                FailExitMember(bot, member, "dry ramp path was blocked");
            return false;
        }
        if (member.stage == ExitStage::Ramp)
        {
            if (Near(bot, Camp, 1.5f) && DcNaxx40SlimeCrossing::IsDryCampGround(bot, Location(bot)))
            {
                member.stage = ExitStage::Arrived;
                DcMovement::ClearMovementWait(&bot);
                LOG_INFO("playerbots.dungeonclear", "[DC:{}] Thaddius exit arrived instance {} totalMs={} hp={}/{}",
                    bot.GetName(), bot.GetInstanceId(), now - member.departure, bot.GetHealth(), bot.GetMaxHealth());
            }
            else if (member.rampRetries < 2 && LaunchExitRamp(ai, member))
            {
                ++member.rampRetries;
                LOG_INFO("playerbots.dungeonclear", "[DC:{}] Thaddius exit ramp resumed instance {} retry={}",
                    bot.GetName(), bot.GetInstanceId(), member.rampRetries);
            }
            else
                FailExitMember(bot, member, "ramp spline ended outside the safe continuation");
        }
        return false;
    }

    void DriveExit(PlayerbotAI& ai, uint32 now)
    {
        Player& owner = *ai.GetBot();
        DcRunState& run = DcRun::Of(&ai);
        Exit& exit = run.naxx40ThaddiusExit;
        if (now - exit.lastUpdate < 100)
            return;
        exit.lastUpdate = now;

        std::vector<std::pair<Player*, Exit::Member*>> ready;
        unsigned moving = 0;
        unsigned arrived = 0;
        unsigned failed = 0;
        bool const recover = DcRezRecovery::IsPending(&owner) || DcCombatFlag::AnyPartyEngagement(&owner);
        for (Exit::Member& record : exit.members)
        {
            Player* bot = ObjectAccessor::GetPlayer(owner, record.guid);
            PlayerbotAI* memberAI = bot && bot->IsInMap(&owner) && bot->GetGroup() == owner.GetGroup() &&
                bot->InSamePhase(&owner) ? GET_PLAYERBOT_AI(bot) : nullptr;
            if (record.stage != ExitStage::Arrived && record.stage != ExitStage::Failed && (!memberAI || !bot->IsAlive()))
            {
                record.stage = ExitStage::Failed;
                LOG_INFO("playerbots.dungeonclear", "[DC:{}] Thaddius exit participant unavailable instance {} guid={}",
                    owner.GetName(), exit.instanceId, record.guid.ToString());
            }
            if (record.stage != ExitStage::Arrived && record.stage != ExitStage::Failed && memberAI)
            {
                ExitStage const before = record.stage;
                if ((before != ExitStage::Approach || !recover) && DriveExitMember(*memberAI, record, now))
                    ready.emplace_back(bot, &record);
                if (before != record.stage)
                    exit.lastProgress = now;
            }
            arrived += record.stage == ExitStage::Arrived;
            failed += record.stage == ExitStage::Failed;
            moving += record.stage == ExitStage::Jump || record.stage == ExitStage::Crossing || record.stage == ExitStage::Ramp;
        }
        if (arrived == exit.members.size())
        {
            exit.complete = true;
            DcActionShared::SetPhase(ai.GetAiObjectContext(), "Thaddius: waiting at the dry split camp");
            LOG_INFO("playerbots.dungeonclear", "[DC:{}] Thaddius exit complete instance {} arrived={}/{} waves={}",
                owner.GetName(), exit.instanceId, arrived, exit.members.size(), exit.wave);
            return;
        }
        if (!moving && !recover && (failed || now - exit.lastProgress >= ExitProgressLimit))
        {
            run.paused = true;
            DcActionShared::SetPhase(ai.GetAiObjectContext(), "Thaddius: exit blocked");
            LOG_INFO("playerbots.dungeonclear",
                "[DC:{}] Thaddius exit blocked instance {} arrived={}/{} failed={} staged={}",
                owner.GetName(), exit.instanceId, arrived, exit.members.size(), failed, ready.size());
            return;
        }
        if (!moving && !failed && !recover && ready.size() >= std::min<std::size_t>(ExitWaveSize, exit.members.size() - arrived))
        {
            // Give each wave a healer while reserving the other healers for later waves.
            auto const healer = std::ranges::find_if(ready, [](auto const& candidate) { return candidate.second->healer; });
            if (healer != ready.end())
            {
                std::iter_swap(ready.begin(), healer);
                std::partition(ready.begin() + 1, ready.end(), [](auto const& candidate) { return !candidate.second->healer; });
            }
            unsigned launched = 0;
            unsigned heals = 0;
            for (auto const& [bot, record] : ready)
            {
                if (launched >= ExitWaveSize)
                    break;
                if (!LaunchExitJump(*GET_PLAYERBOT_AI(bot), *record, now))
                {
                    FailExitMember(*bot, *record, "surveyed jump did not launch");
                    break;
                }
                ++launched;
                heals += record->healer;
            }
            if (launched)
            {
                exit.lastProgress = now;
                LOG_INFO("playerbots.dungeonclear", "[DC:{}] Thaddius exit wave {} instance {} bots={} healers={}",
                    owner.GetName(), ++exit.wave, exit.instanceId, launched, heals);
            }
        }
        if (now - exit.lastReport >= 5000)
        {
            exit.lastReport = now;
            LOG_INFO("playerbots.dungeonclear", "[DC:{}] Thaddius exit status instance {} arrived={}/{} moving={} ready={} failed={}",
                owner.GetName(), exit.instanceId, arrived, exit.members.size(), moving, ready.size(), failed);
        }
    }

    Exit::Member const* GetExitMember(Player& bot, DcRunState const& local)
    {
        if (local.naxx40ThaddiusExitOwner.IsEmpty())
            return nullptr;
        Player* owner = ObjectAccessor::GetPlayer(bot, local.naxx40ThaddiusExitOwner);
        PlayerbotAI* ownerAI = owner && bot.IsInMap(owner) && bot.GetGroup() == owner->GetGroup() &&
            bot.InSamePhase(owner) ? GET_PLAYERBOT_AI(owner) : nullptr;
        if (!ownerAI || (!ExitActive(*owner, DcRun::Of(ownerAI)) && !ExitCampHold(*owner, DcRun::Of(ownerAI))))
            return nullptr;
        Exit const& exit = DcRun::Of(ownerAI).naxx40ThaddiusExit;
        auto const record = std::ranges::find_if(exit.members, [&](Exit::Member const& record)
        {
            return record.guid == bot.GetGUID() && record.stage != ExitStage::Failed;
        });
        return record == exit.members.end() ? nullptr : &*record;
    }

    class ThaddiusMapScript final : public AllMapScript
    {
    public:
        ThaddiusMapScript() : AllMapScript("DungeonClearNaxx40Thaddius", {ALLMAPHOOK_ON_MAP_UPDATE}) {}

        void OnMapUpdate(Map* map, uint32) override
        {
            if (!IsNaxx40Map(map) || !DcModule::IsEnabled())
                return;
            uint32 const now = getMSTime();
            for (auto const& ref : map->GetPlayers())
            {
                Player* player = ref.GetSource();
                if (!player)
                    continue;
                PlayerbotAI* ai = GET_PLAYERBOT_AI(player);
                if (!ai)
                    continue;
                DcRunState& run = DcRun::Of(ai);
                InstanceScript* instance = player->GetInstanceScript();
                if (!run.naxx40ThaddiusExitOwner.IsEmpty() && !GetExitMember(*player, run))
                    run.naxx40ThaddiusExitOwner.Clear();
                bool const defeated = instance && instance->IsBossDone(Thaddius40::Encounter);
                if (defeated && ExitActive(*player, run))
                    DriveExit(*ai, now);
                bool const handoff = instance && instance->GetBossState(Thaddius40::Encounter) == IN_PROGRESS;
                if (!handoff && !defeated)
                    run.naxx40ThaddiusPortalCredit = {};
                if (!run.naxx40ThaddiusOwner.IsEmpty())
                {
                    PlayerbotAI* driver = DrivingAI(*player, run);
                    if (handoff || !driver || DcRun::Of(driver).naxx40ThaddiusOpening.clearingTrash)
                        Release(*player, run, handoff);
                }
                if (!run.enabled || run.paused || !player->IsAlive() || !player->GetGroup() ||
                    !instance || defeated)
                {
                    run.naxx40ThaddiusOpening = {};
                    continue;
                }
                if (handoff)
                {
                    Opening const& opening = run.naxx40ThaddiusOpening;
                    if (opening.instanceId)
                    {
                        // Only a live opening owned by this DC run grants portal priority.
                        if (opening.phase == Phase::Pull && opening.instanceId == player->GetInstanceId() &&
                            opening.group == player->GetGroup()->GetGUID())
                            run.naxx40ThaddiusPortalCredit = {opening.group, opening.instanceId};
                        LOG_INFO("playerbots.dungeonclear", "[DC:{}] Thaddius opening handed off to Playerbots",
                            player->GetName());
                        DcActionShared::SetPhase(ai->GetAiObjectContext(), "");
                        run.naxx40ThaddiusOpening = {};
                    }
                    continue;
                }
                if (!BlocksGeneric(player, Thaddius40::ThaddiusEntry) ||
                    DcLeaderSignal::FindRunOwner(player) != player)
                {
                    run.naxx40ThaddiusOpening = {};
                    continue;
                }
                auto const next = ai->GetAiObjectContext()
                    ->GetValue<std::optional<DungeonBossInfo>>(DcKey::NextDungeonBoss)->Get();
                if (next && next->entry == Thaddius40::ThaddiusEntry)
                    Drive(*ai, *instance, now);
                else
                    run.naxx40ThaddiusOpening = {};
            }
        }
    };
}

bool CanUseWingPortal(Player const& owner, DcRunState const& run)
{
    Group const* const group = owner.GetGroup();
    auto const& credit = run.naxx40ThaddiusPortalCredit;
    if (!group || credit.instanceId != owner.GetInstanceId() || credit.group != group->GetGUID() ||
        (run.naxx40ThaddiusExit.instanceId == owner.GetInstanceId() &&
         run.naxx40ThaddiusExit.group == group->GetGUID()))
        return false;

    for (GroupReference const* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* const member = ref->GetSource();
        if (!member || !member->IsAlive() || !GET_PLAYERBOT_AI(member))
            continue;
        if (!member->IsInWorld() || !member->IsInMap(&owner) || !member->InSamePhase(&owner) ||
            member->IsFalling() || !Near(*member, BossPlatform, 34.0f) ||
            !DcNaxx40SlimeCrossing::IsDryCampGround(*member, Location(*member)))
            return false;
    }
    return true;
}

bool BeginExit(PlayerbotAI* ai)
{
    Player* const owner = ai ? ai->GetBot() : nullptr;
    InstanceScript* const instance = GetInstance(owner);
    if (!instance || !instance->IsBossDone(Thaddius40::Encounter) || instance->IsEncounterInProgress() ||
        !DcModule::IsEnabled() || !owner->GetGroup() || !owner->IsAlive() || DcLeaderSignal::FindRunOwner(owner) != owner)
        return false;

    DcRunState& run = DcRun::Of(ai);
    if (!run.enabled || run.paused)
        return false;
    if (run.naxx40ThaddiusExit.instanceId == owner->GetInstanceId() &&
        run.naxx40ThaddiusExit.group == owner->GetGroup()->GetGUID())
        return !run.naxx40ThaddiusExit.complete || ExitCampHold(*owner, run);
    if (!Near(*owner, BossPlatform, 32.0f))
        return false;
    if (DcRezRecovery::IsPending(owner) || DcCombatFlag::AnyPartyEngagement(owner))
        return true;

    Exit exit;
    unsigned selfbots = 0;
    for (GroupReference* ref = owner->GetGroup()->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || !member->IsAlive() || !member->IsInWorld() || !member->IsInMap(owner) ||
            !member->InSamePhase(owner) || !Near(*member, BossPlatform, 34.0f) ||
            !DcNaxx40SlimeCrossing::IsDryCampGround(*member, Location(*member)))
            continue;
        PlayerbotAI* memberAI = GET_PLAYERBOT_AI(member);
        if (!memberAI)
            continue;
        exit.members.push_back({member->GetGUID(), PlayerbotAI::IsHeal(member)});
        selfbots += DcPlayerbotCompat::IsSelfBot(member);
        DcRun::Of(memberAI).naxx40ThaddiusExitOwner = owner->GetGUID();
        if (!member->IsFalling())
            DcMovement::StopBot(member, DcMovement::Stop::Hold);
    }
    if (exit.members.empty())
        return false;

    exit.group = owner->GetGroup()->GetGUID();
    exit.instanceId = owner->GetInstanceId();
    exit.lastProgress = getMSTime();
    run.naxx40ThaddiusExit = std::move(exit);
    DcActionShared::ClearStall(ai->GetAiObjectContext());
    DcActionShared::SetPhase(ai->GetAiObjectContext(), "Thaddius: returning to the split camp");
    LOG_INFO("playerbots.dungeonclear", "[DC:{}] Thaddius exit armed instance {} bots={} selfbots={}",
        owner->GetName(), run.naxx40ThaddiusExit.instanceId, run.naxx40ThaddiusExit.members.size(), selfbots);
    return true;
}

bool BlocksGeneric(Player* bot, uint32 nextBoss)
{
    InstanceScript* instance = GetInstance(bot);
    if (!instance || instance->IsEncounterInProgress())
        return false;
    if (instance->IsBossDone(Thaddius40::Encounter))
    {
        PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
        return ai && (ExitActive(*bot, DcRun::Of(ai)) || ExitCampHold(*bot, DcRun::Of(ai)) ||
            GetExitMember(*bot, DcRun::Of(ai)));
    }
    return nextBoss == Thaddius40::ThaddiusEntry && instance->IsBossDone(GluthEncounter) &&
        bot->GetExactDist2dSq(Camp.x, Camp.y) < 70.0f * 70.0f;
}

bool OwnsMovement(PlayerbotAI* ai)
{
    if (!ai || ai->GetBot()->GetMapId() != 533 || ai->GetBot()->IsInCombat())
        return false;
    Player& bot = *ai->GetBot();
    DcRunState const& local = DcRun::Of(ai);
    if (GetExitMember(bot, local))
        return true;
    PlayerbotAI* driver = local.naxx40ThaddiusOwner.IsEmpty() ? nullptr : DrivingAI(bot, local);
    return driver && !DcRun::Of(driver).naxx40ThaddiusOpening.clearingTrash &&
        BlocksGeneric(driver->GetBot(), Thaddius40::ThaddiusEntry);
}

bool InExitTransition(PlayerbotAI* ai)
{
    if (!ai || !IsNaxx40Map(ai->GetBot()->FindMap()))
        return false;
    Exit::Member const* record = GetExitMember(*ai->GetBot(), DcRun::Of(ai));
    return record && (record->stage == ExitStage::Jump || record->stage == ExitStage::Ramp);
}

void RegisterMapUpdate()
{
    new ThaddiusMapScript();
}
}
