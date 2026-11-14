/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, or later.
 */

#include "DcNaxx40Thaddius.h"

#include "Ai/Dungeon/DungeonClear/Action/DcActionShared.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Raid/Naxx40/Naxx40Encounter.h"
#include "Ai/Raid/Naxx40/Thaddius40.h"
#include "AllMapScript.h"
#include "DcCombatFlag.h"
#include "DcLeaderSignal.h"
#include "DcModuleEnable.h"
#include "DcMovement.h"
#include "DcNaxx40SlimeCrossing.h"
#include "DcPlayerbotCompat.h"
#include "DcRun.h"
#include "Group.h"
#include "InstanceScript.h"
#include "Log.h"
#include "MapReference.h"
#include "MoveSpline.h"
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

    constexpr uint32 GluthEncounter = 2;
    G3D::Vector3 const Camp{3433.0f, -3005.0f, 295.613f};
    std::array<G3D::Vector3, 2> const Holds{{
        {3428.340f, -2958.770f, 309.298f}, {3480.736f, -3011.429f, 309.241f},
    }};
    std::array<G3D::Vector3, 2> const PullPoints{{
        {3442.920f, -2940.690f, 312.021f}, {3498.890f, -2996.340f, 312.021f},
    }};

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
                bool const handoff = instance && instance->GetBossState(Thaddius40::Encounter) == IN_PROGRESS;
                if (!run.naxx40ThaddiusOwner.IsEmpty())
                {
                    PlayerbotAI* driver = DrivingAI(*player, run);
                    if (handoff || !driver || DcRun::Of(driver).naxx40ThaddiusOpening.clearingTrash)
                        Release(*player, run, handoff);
                }
                if (!run.enabled || run.paused || !player->IsAlive() || !player->GetGroup() ||
                    !instance || instance->IsBossDone(Thaddius40::Encounter))
                {
                    run.naxx40ThaddiusOpening = {};
                    continue;
                }
                if (handoff)
                {
                    if (run.naxx40ThaddiusOpening.instanceId)
                    {
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

bool BlocksGeneric(Player* bot, uint32 nextBoss)
{
    if (nextBoss != Thaddius40::ThaddiusEntry)
        return false;
    InstanceScript* instance = GetInstance(bot);
    return instance && instance->IsBossDone(GluthEncounter) && !instance->IsEncounterInProgress() &&
        !instance->IsBossDone(Thaddius40::Encounter) && bot->GetExactDist2dSq(Camp.x, Camp.y) < 70.0f * 70.0f;
}

bool OwnsMovement(PlayerbotAI* ai)
{
    if (!ai || ai->GetBot()->GetMapId() != 533 || ai->GetBot()->IsInCombat())
        return false;
    Player& bot = *ai->GetBot();
    DcRunState const& local = DcRun::Of(ai);
    PlayerbotAI* driver = local.naxx40ThaddiusOwner.IsEmpty() ? nullptr : DrivingAI(bot, local);
    return driver && !DcRun::Of(driver).naxx40ThaddiusOpening.clearingTrash &&
        BlocksGeneric(driver->GetBot(), Thaddius40::ThaddiusEntry);
}

void RegisterMapUpdate()
{
    new ThaddiusMapScript();
}
}
