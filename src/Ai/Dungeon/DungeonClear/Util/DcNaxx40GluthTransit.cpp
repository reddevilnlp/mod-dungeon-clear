/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, or later.
 */

#include "DcNaxx40GluthTransit.h"

#include "Ai/Dungeon/DungeonClear/Action/DcActionShared.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/DcApproachState.h"
#include "Ai/Raid/Naxx40/Naxx40Encounter.h"
#include "AllMapScript.h"
#include "ChunkedPathfinder.h"
#include "Creature.h"
#include "DcModuleEnable.h"
#include "DcMovement.h"
#include "DcNaxx40SlimeCrossing.h"
#include "DcPlayerbotCompat.h"
#include "DcRaidMuster.h"
#include "DcRun.h"
#include "DungeonPathFollower.h"
#include "Group.h"
#include "InstanceScript.h"
#include "Log.h"
#include "MapReference.h"
#include "MoveSpline.h"
#include "MoveSplineInit.h"
#include "ObjectAccessor.h"
#include "PathGenerator.h"
#include "Player.h"
#include "Playerbots.h"
#include "Timer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <list>

namespace DcNaxx40GluthTransit
{
namespace
{
    constexpr uint32 Gluth = 351004;
    // Encounter slots shared by the Naxxramas instance and the two Naxx40 boss scripts.
    constexpr uint32 GrobbulusEncounter = 1;
    constexpr uint32 GluthEncounter = 2;
    constexpr float RoomTop = 300.0f;
    constexpr uint32 UpdateInterval = 150;
    constexpr uint32 RetryInterval = 1200;
    constexpr uint32 NoProgressTimeout = 90000;
    constexpr std::size_t TunnelMuster = 5;
    constexpr std::size_t TunnelExit = 7;
    std::array<G3D::Vector3, 8> const Route{{
        {3247.7253f, -3306.819f, 292.59506f},
        {3221.333f, -3349.333f, 296.84147f},
        {3216.8f, -3374.4f, 310.75584f},
        {3215.733f, -3374.667f, 312.08761f},
        {3194.933f, -3312.267f, 320.31727f},
        {3168.2822f, -3277.4412f, 318.763f},
        {3168.2822f, -3273.4412f, 318.6679f},
        {3252.5935f, -3189.0203f, 317.9101f},
    }};
    G3D::Vector3 const OverRoom{3254.6836f, -3186.8682f, 319.0f};
    G3D::Vector3 const RoomEntrance{3261.4653f, -3181.4333f, 297.80096f};
    using Transit = DcRunState::GluthTransit;

    G3D::Vector3 Location(Player const& player)
    {
        return {player.GetPositionX(), player.GetPositionY(), player.GetPositionZ()};
    }

    InstanceScript const* Instance(Player const* player)
    {
        return player && IsNaxx40Map(player->FindMap()) ? player->GetInstanceScript() : nullptr;
    }

    bool Near(Player const& player, G3D::Vector3 const& point, float radius)
    {
        return (Location(player) - point).squaredLength() <= radius * radius;
    }

    bool AtRoom(Player const& player)
    {
        return player.GetPositionZ() >= 296.0f && player.GetPositionZ() <= RoomTop &&
            player.GetPositionX() >= 3251.0f && player.GetPositionX() <= 3366.0f &&
            player.GetPositionY() >= -3190.0f && player.GetPositionY() <= -3070.0f;
    }

    bool AtGrobbulus(Player const& player)
    {
        return player.GetPositionZ() >= 290.0f && player.GetPositionZ() <= 295.0f &&
            player.GetPositionX() >= 3189.0f && player.GetPositionX() <= 3337.0f &&
            player.GetPositionY() >= -3340.0f && player.GetPositionY() <= -3258.0f;
    }

    bool Falling(Player const& player)
    {
        return player.GetMotionMaster()->GetCurrentMovementGeneratorType() == EFFECT_MOTION_TYPE;
    }

    bool CanTravel(PlayerbotAI& ai)
    {
        Player* bot = ai.GetBot();
        return ai.CanMove() && !bot->GetVehicle() && !bot->GetTransport() && !bot->IsFlying() &&
            !bot->IsFalling() && !bot->HasUnitFlag(UNIT_FLAG_DISABLE_MOVE);
    }

    Player* GetMember(Player& owner, ObjectGuid guid)
    {
        Player* member = ObjectAccessor::GetPlayer(owner, guid);
        if (!member || !member->IsInMap(&owner) || member->GetGroup() != owner.GetGroup() ||
            !owner.InSamePhase(member) || !GET_PLAYERBOT_AI(member))
            return nullptr;
        return member;
    }

    bool ClearLine(Player const& bot, G3D::Vector3 from, G3D::Vector3 to, float lift)
    {
        return bot.GetMap()->isInLineOfSight(from.x, from.y, from.z + lift, to.x, to.y, to.z + lift,
            bot.GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing);
    }

    bool ClearsClouds(Player const& bot, Movement::PointsArray const& path)
    {
        if (bot.GetPositionZ() > 300.0f)
            return true;
        static std::vector<uint32> const entries{16363, 351089};
        std::list<Creature*> nearby;
        bot.GetCreatureListWithEntryInGrid(nearby, entries, 145.0f);
        for (Creature const* cloud : nearby)
        {
            if (!cloud->IsAlive())
                continue;
            G3D::Vector3 const center{cloud->GetPositionX(), cloud->GetPositionY(), cloud->GetPositionZ()};
            float const radius = cloud->GetCombatReach() + 3.0f;
            for (std::size_t i = 1; i < path.size(); ++i)
            {
                G3D::Vector3 const start = path[i - 1] - center;
                G3D::Vector3 const step = path[i] - path[i - 1];
                if (step.squaredLength() < 0.001f)
                    continue;
                float const fraction = std::clamp(-start.dot(step) / step.squaredLength(), 0.0f, 1.0f);
                float const closest = (start + step * fraction).length();
                if (closest < radius && (start.length() >= radius || closest + 0.15f < start.length()))
                    return false;
                if (i + 1 == path.size() && (path.back() - center).length() < radius)
                    return false;
            }
        }
        return true;
    }

    Movement::PointsArray WalkPath(Player& bot, G3D::Vector3 const& goal, float limit)
    {
        PathGenerator nav(&bot);
        nav.SetExcludeFlags(NAV_WATER | NAV_MAGMA | NAV_SLIME | NAV_GROUND_STEEP);
        if (!nav.CalculatePath(goal.x, goal.y, goal.z) || nav.GetPathType() != PATHFIND_NORMAL ||
            nav.GetPath().size() < 2 || (nav.GetActualEndPosition() - goal).length() > 1.5f ||
            nav.getPathLength() > limit || !DcNaxx40SlimeCrossing::IsDryCampGround(bot, goal))
            return {};

        Movement::PointsArray path = nav.GetPath();
        path.front() = Location(bot);
        path.back() = goal;
        for (std::size_t i = 1; i < path.size(); ++i)
            if (!ClearLine(bot, path[i - 1], path[i], 0.8f))
                return {};
        if (!ClearsClouds(bot, path))
            return {};
        // A two-point escort would ask the core for a new path and discard these checks.
        if (path.size() == 2)
            path.insert(path.begin() + 1, (path.front() + path.back()) * 0.5f);
        return path;
    }

    bool Landed(Player& player, bool dropped)
    {
        if (!AtRoom(player) || Falling(player) ||
            !DcNaxx40SlimeCrossing::IsDryCampGround(player, Location(player)))
            return false;
        if (dropped)
            player.RemoveUnitMovementFlag(MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR);
        return true;
    }

    void DriveDrop(Player& bot, Transit::Member& record)
    {
        G3D::Vector3 const start = Location(bot);
        float const floor = bot.GetMapHeight(OverRoom.x, OverRoom.y, OverRoom.z, true, 30.0f);
        G3D::Vector3 const landing{OverRoom.x, OverRoom.y, floor};
        if (std::abs(floor - RoomEntrance.z) > 0.8f ||
            !DcNaxx40SlimeCrossing::IsDryCampGround(bot, landing) || !ClearLine(bot, landing, OverRoom, 0.1f))
            return;
        if (Near(bot, OverRoom, 0.30f))
        {
            // MoveFall uses the floor at the bot's live column, not the destination.
            float const underBot = bot.GetMapHeight(start.x, start.y, start.z, true, 30.0f);
            if (std::abs(underBot - floor) > 0.5f)
                return;
            bot.GetMotionMaster()->MoveFall();
            record.dropped = Falling(bot);
            return;
        }
        if (!Near(bot, Route.back(), 0.65f) || !ClearLine(bot, start, OverRoom, 0.08f) ||
            !ClearLine(bot, start, OverRoom, bot.GetCollisionHeight()))
            return;
        Movement::MoveSplineInit glide(&bot);
        glide.MoveTo(OverRoom.x, OverRoom.y, OverRoom.z, false);
        glide.SetWalk(false);
        glide.Launch();
    }

    std::size_t InitialCheckpoint(Player& bot)
    {
        if (AtGrobbulus(bot))
            return 0;
        if (Near(bot, OverRoom, 0.3f))
            return Route.size();
        if (Near(bot, Route[0], 3.0f))
            return 0;
        if (bot.GetPositionZ() >= 313.0f)
        {
            // Distinguish the gas tunnel from the upper ramp, which shares its elevation.
            G3D::Vector3 const origin = Location(bot) - Route[6];
            G3D::Vector3 const tunnel = Route.back() - Route[6];
            float const along = origin.dot(tunnel) / tunnel.squaredLength();
            if (along >= 0.0f && along <= 1.02f && (origin - tunnel * along).length() < 8.0f)
                return TunnelExit;
            if (Near(bot, Route[TunnelMuster], 20.0f))
                return TunnelMuster;
        }
        if (bot.GetPositionZ() > 295.0f)
        {
            // On a resumed ramp leg, choose the downstream end of the nearest
            // segment on this floor. Tight vertical bounds avoid the room below.
            float nearest = 12.0f * 12.0f;
            std::size_t next = Route.size() + 1;
            for (std::size_t i = 1; i <= TunnelMuster; ++i)
            {
                G3D::Vector3 const leg = Route[i] - Route[i - 1];
                G3D::Vector3 const offset = Location(bot) - Route[i - 1];
                float const along = std::clamp(offset.dot(leg) / leg.squaredLength(), 0.0f, 1.0f);
                G3D::Vector3 const delta = offset - leg * along;
                if (std::abs(delta.z) < 4.0f && delta.squaredLength() < nearest)
                {
                    nearest = delta.squaredLength();
                    next = i;
                }
            }
            return next;
        }
        return Route.size() + 1;
    }

    void Arm(Player& owner, AiObjectContext* context, Transit& transit, uint32 now)
    {
        transit = {};
        transit.group = owner.GetGroup()->GetGUID();
        transit.instanceId = owner.GetInstanceId();
        transit.lastProgress = now;
        context->GetValue<ChunkedPathfinder::Result&>(DcKey::LongPath)->Reset();
        context->GetValue<DungeonFollowerState&>(DcKey::FollowerState)->Get() = DungeonFollowerState{};
        context->GetValue<DcApproachState&>(DcKey::ApproachState)->Get().Reset();
        context->GetValue<ObjectGuid>(DcKey::EngageTrashTarget)->Set(ObjectGuid::Empty);
        DcActionShared::ClearStall(context);
        for (GroupReference* ref = owner.GetGroup()->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            PlayerbotAI* ai = member && member->IsInMap(&owner) && owner.InSamePhase(member)
                ? GET_PLAYERBOT_AI(member) : nullptr;
            if (!ai)
                continue;
            transit.members.push_back({member->GetGUID(), InitialCheckpoint(*member)});
            DcRun::Of(ai).naxx40GluthTransitOwner = owner.GetGUID();
            if (member->IsAlive() && !AtRoom(*member) && !Falling(*member))
                DcMovement::StopBot(member, DcMovement::Stop::Hold);
        }
        auto rank = [&](ObjectGuid guid)
        {
            Player* member = GetMember(owner, guid);
            if (member && PlayerbotAI::IsMainTank(member))
                return 0;
            if (member && PlayerbotAI::IsHeal(member))
                return 2;
            if (member == &owner || (member && PlayerbotAI::IsAssistTankOfIndex(member, 0, true)))
                return 1;
            return 3;
        };
        std::stable_sort(transit.members.begin(), transit.members.end(),
            [&](Transit::Member const& a, Transit::Member const& b) { return rank(a.guid) < rank(b.guid); });
        LOG_INFO("playerbots.dungeonclear", "[DC:{}] Gluth transit armed for {} bots, instance {}",
            owner.GetName(), transit.members.size(), transit.instanceId);
    }

    void Drive(PlayerbotAI& ai, DcRunState& run, DungeonBossInfo const* next, EncounterState gluth, uint32 now)
    {
        Player& owner = *ai.GetBot();
        AiObjectContext* context = ai.GetAiObjectContext();
        Transit& transit = run.naxx40GluthTransit;
        if (transit.instanceId != owner.GetInstanceId() || transit.group != owner.GetGroup()->GetGUID())
            Arm(owner, context, transit, now);
        if (transit.complete || transit.stopped || now - transit.lastUpdate < UpdateInterval)
            return;
        transit.lastUpdate = now;
        bool const fightStarted = gluth == IN_PROGRESS;
        if (!transit.throughGas && !fightStarted && owner.IsInCombat())
        {
            transit.lastProgress = now;
            return;
        }
        bool const gathered = std::ranges::all_of(transit.members, [&](Transit::Member const& record)
        {
            Player* member = GetMember(owner, record.guid);
            return member && member->IsAlive() && record.checkpoint > 0 && record.checkpoint <= Route.size();
        });
        bool const startingInTunnel = std::ranges::any_of(transit.members, [](Transit::Member const& member)
        {
            return member.checkpoint >= TunnelExit && member.checkpoint <= Route.size();
        });
        if (gathered && !transit.prepared && !fightStarted && !startingInTunnel && next)
        {
            DcActionShared::SetPhase(context, "preparing for Gluth before the pipe");
            DcActionShared::IsBetweenPullsReady(&owner, context);
            transit.lastProgress = now;
            if (DcRaidMuster::Holds(&owner, &ai, context, *next))
                return;
            transit.prepared = true;
        }
        if (fightStarted || startingInTunnel)
            transit.prepared = true;

        bool const staged = gathered && std::ranges::all_of(transit.members, [&](Transit::Member const& record)
        {
            Player* member = GetMember(owner, record.guid);
            return record.checkpoint > TunnelMuster ||
                (record.checkpoint == TunnelMuster && Near(*member, Route[TunnelMuster], 2.0f));
        });
        if (!transit.throughGas && (staged || fightStarted || startingInTunnel))
        {
            transit.throughGas = true;
            LOG_INFO("playerbots.dungeonclear", "[DC:{}] Gluth transit entering Toxic Tunnel", owner.GetName());
        }
        bool tankAcrossLip = fightStarted;
        std::size_t landed = 0;
        for (Transit::Member& record : transit.members)
        {
            Player* member = GetMember(owner, record.guid);
            if (!member || !member->IsAlive())
                continue;
            if (record.landed || Landed(*member, record.dropped))
            {
                ++landed;
                if (!record.landed)
                {
                    record.landed = true;
                    transit.lastProgress = now;
                    LOG_INFO("playerbots.dungeonclear",
                        "[DC:{}] Gluth landed {} selfbot={} at ({:.3f},{:.3f},{:.3f}) hp={}/{}",
                        owner.GetName(), member->GetName(), DcPlayerbotCompat::IsSelfBot(member),
                        member->GetPositionX(), member->GetPositionY(), member->GetPositionZ(),
                        member->GetHealth(), member->GetMaxHealth());
                }
                tankAcrossLip |= &record == &transit.members.front();
                continue;
            }
            if (Falling(*member))
            {
                tankAcrossLip |= &record == &transit.members.front();
                continue;
            }
            if (record.checkpoint > Route.size())
                record.checkpoint = InitialCheckpoint(*member);
            if (record.checkpoint > Route.size() || !member->movespline->Finalized())
                continue;
            if (!transit.throughGas && !fightStarted && member->IsInCombat())
                continue;
            if (record.checkpoint < Route.size())
            {
                float const radius = record.checkpoint == 0 ? 3.0f : 0.5f;
                if (Near(*member, Route[record.checkpoint], radius))
                {
                    ++record.checkpoint;
                    transit.lastProgress = now;
                    record.lastAttempt = 0;
                }
            }
            if ((record.checkpoint > 0 && !transit.prepared) ||
                (record.checkpoint > TunnelMuster && !transit.throughGas) ||
                (record.checkpoint == Route.size() && &record != &transit.members.front() && !tankAcrossLip))
                continue;
            if (record.lastAttempt && now - record.lastAttempt < RetryInterval)
                continue;
            PlayerbotAI* memberAI = GET_PLAYERBOT_AI(member);
            if (!CanTravel(*memberAI))
                continue;
            record.lastAttempt = now;
            if (record.checkpoint == Route.size())
            {
                DriveDrop(*member, record);
                continue;
            }
            Movement::PointsArray path = WalkPath(*member, Route[record.checkpoint],
                record.checkpoint == 0 ? 120.0f : record.checkpoint == TunnelExit ? 140.0f : 90.0f);
            if (!path.empty() && DcMovement::SplinePath(memberAI, path, MovementPriority::MOVEMENT_FORCED,
                FORCED_MOVEMENT_RUN))
            {
                if (record.checkpoint == TunnelExit)
                    LOG_INFO("playerbots.dungeonclear",
                        "[DC:{}] Gluth tunnel departure {} selfbot={} speed={:.2f} hp={}/{}",
                        owner.GetName(), member->GetName(), DcPlayerbotCompat::IsSelfBot(member),
                        member->GetSpeed(MOVE_RUN), member->GetHealth(), member->GetMaxHealth());
            }
        }
        if (landed == transit.members.size())
        {
            transit.complete = true;
            DcActionShared::SetPhase(context, "");
            LOG_INFO("playerbots.dungeonclear", "[DC:{}] Gluth transit complete: {}/{} landed",
                owner.GetName(), landed, transit.members.size());
            return;
        }
        if (now - transit.lastProgress >= NoProgressTimeout)
        {
            if (!transit.throughGas)
                transit.stopped = true;
            transit.lastProgress = now;
            DcActionShared::SetPhase(context, transit.throughGas
                ? "Gluth tunnel blocked; trying to finish the crossing"
                : "Gluth route blocked; regroup and resume");
            for (Transit::Member const& record : transit.members)
                if (!record.landed)
                    LOG_WARN("playerbots.dungeonclear", "[DC:{}] Gluth route blocked at checkpoint {} for {}",
                        owner.GetName(), record.checkpoint, record.guid.ToString());
        }
        else if (transit.prepared)
            DcActionShared::SetPhase(context, transit.throughGas
                ? "crossing the Toxic Tunnel to Gluth" : "taking the ramp to the Toxic Tunnel");
        else if (!gathered)
            DcActionShared::SetPhase(context, "regrouping before the pipe to Gluth");
    }

    Transit const* ActiveTransit(PlayerbotAI& ai)
    {
        Player* bot = ai.GetBot();
        InstanceScript const* instance = Instance(bot);
        if (!instance || !instance->IsBossDone(GrobbulusEncounter) || instance->IsBossDone(GluthEncounter) ||
            !bot->IsAlive())
            return nullptr;
        ObjectGuid const ownerGuid = DcRun::Of(&ai).naxx40GluthTransitOwner;
        Player* owner = ownerGuid.IsEmpty() ? nullptr : ObjectAccessor::GetPlayer(*bot, ownerGuid);
        PlayerbotAI* ownerAI = owner && owner->GetGroup() == bot->GetGroup() ? GET_PLAYERBOT_AI(owner) : nullptr;
        if (!ownerAI)
            return nullptr;
        DcRunState const& run = DcRun::Of(ownerAI);
        Transit const& transit = run.naxx40GluthTransit;
        if (!run.enabled || run.paused || transit.complete || !bot->GetGroup() ||
            transit.instanceId != bot->GetInstanceId() || transit.group != bot->GetGroup()->GetGUID())
            return nullptr;
        return !AtRoom(*bot) || Falling(*bot) ? &transit : nullptr;
    }

    class GluthTransitMapScript final : public AllMapScript
    {
    public:
        GluthTransitMapScript() : AllMapScript("DungeonClearNaxx40GluthTransit", {ALLMAPHOOK_ON_MAP_UPDATE}) {}

        void OnMapUpdate(Map* map, uint32) override
        {
            if (!IsNaxx40Map(map) || !DcModule::IsEnabled())
                return;
            uint32 const now = getMSTime();
            for (auto const& ref : map->GetPlayers())
            {
                Player* owner = ref.GetSource();
                if (!owner || !owner->GetGroup() || !owner->IsAlive())
                    continue;
                PlayerbotAI* ai = GET_PLAYERBOT_AI(owner);
                if (!ai)
                    continue;
                DcRunState& run = DcRun::Of(ai);
                InstanceScript const* instance = owner->GetInstanceScript();
                if (!run.enabled || run.paused || !instance || !instance->IsBossDone(GrobbulusEncounter))
                    continue;
                EncounterState const gluth = instance->GetBossState(GluthEncounter);
                if (gluth == DONE || (instance->IsEncounterInProgress() && gluth != IN_PROGRESS))
                    continue;
                std::optional<DungeonBossInfo> next;
                if (gluth != IN_PROGRESS)
                {
                    next = ai->GetAiObjectContext()->GetValue<std::optional<DungeonBossInfo>>(DcKey::NextDungeonBoss)->Get();
                    if (!next || next->entry != Gluth)
                        continue;
                }
                Drive(*ai, run, next ? &*next : nullptr, gluth, now);
            }
        }
    };
}

bool BlocksGeneric(Player* bot, uint32 nextBoss)
{
    InstanceScript const* instance = nextBoss == Gluth ? Instance(bot) : nullptr;
    return instance && instance->IsBossDone(GrobbulusEncounter) && !instance->IsBossDone(GluthEncounter) &&
        (!AtRoom(*bot) || Falling(*bot));
}

bool OwnsMovement(PlayerbotAI* ai)
{
    Transit const* transit = ai ? ActiveTransit(*ai) : nullptr;
    return transit && (!ai->GetBot()->IsInCombat() || transit->throughGas);
}

bool InGasTransit(PlayerbotAI* ai)
{
    Transit const* transit = ai ? ActiveTransit(*ai) : nullptr;
    return transit && transit->throughGas;
}

void RegisterMapUpdate()
{
    new GluthTransitMapScript();
}
}
