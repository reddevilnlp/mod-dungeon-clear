/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 */

#include "DcNaxx40WingPortal.h"

#include "Ai/Dungeon/DungeonClear/Action/DcActionShared.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"
#include "Ai/Dungeon/DungeonClear/DcApproachState.h"
#include "Ai/Raid/Naxx40/Naxx40Encounter.h"
#include "AllMapScript.h"
#include "ChunkedPathfinder.h"
#include "DcCombatFlag.h"
#include "DcLeaderSignal.h"
#include "DcModuleEnable.h"
#include "DcMovement.h"
#include "DcNaxx40SlimeCrossing.h"
#include "DcPlayerbotCompat.h"
#include "DcRezRecovery.h"
#include "DcRun.h"
#include "DungeonPathFollower.h"
#include "GameObject.h"
#include "Group.h"
#include "InstanceScript.h"
#include "Log.h"
#include "Map.h"
#include "MapReference.h"
#include "MoveSpline.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Player.h"
#include "Playerbots.h"
#include "Timer.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <span>

namespace DcNaxx40WingPortal
{
namespace
{
    using Transit = DcRunState::Naxx40PortalTransit;

    constexpr uint32 LoathebEncounter = 5;
    constexpr uint32 MaexxnaEncounter = 8;
    constexpr uint32 ThaddiusEncounter = 9;
    constexpr uint32 HorsemenEncounter = 12;
    constexpr uint32 SapphironEntry = 351018;
    constexpr uint32 KelThuzadEntry = 351019;
    constexpr uint32 MaexxnaPortalEntry = 181575;
    constexpr uint32 LoathebPortalEntry = 181577;
    constexpr uint32 HorsemenPortalEntry = 181578;
    constexpr uint32 HubTrigger = 4156;
    constexpr uint32 RetryDelayMs = 1500;
    constexpr uint32 ProgressLimitMs = 75000;
    constexpr float WingWalkRadius = 110.0f;

    G3D::Vector3 const HubLanding{3005.51f, -3434.64f, 304.195f};
    G3D::Vector3 const HubTriggerCenter{3006.090f, -3434.170f, 306.195f};
    G3D::Vector3 const FrostwyrmLanding{3498.300f, -5349.490f, 144.968f};
    G3D::Vector3 const HorsemenPortalFloor{2493.020f, -2921.780f, 241.440f};
    // The ramp climbs mostly after the floor turns. A single straight Detour leg loses this grade.
    std::array<G3D::Vector3, 3> const MaexxnaRamp{{
        {3498.667f, -3926.623f, 299.38f},
        {3477.333f, -3935.084f, 301.80f},
        {3465.753f, -3939.677f, 306.90f},
    }};
    // The mesh smooths the height across the raised grate in Loatheb's room.
    std::array<G3D::Vector3, 3> const LoathebApproach{{
        {2909.000f, -3971.000f, 273.633f},
        {2909.000f, -3997.400f, 274.155f},
        {2909.000f, -4025.020f, 273.691f},
    }};

    // Detour can smooth a straight leg across the central dais even when the
    // endpoints are on the lower floor. Keep these surveyed legs on its skirts.
    std::array<G3D::Vector3, 6> const HorsemenWest{{
        {2519.000f, -2995.000f, 241.290f},
        {2501.500f, -2986.500f, 241.285f},
        {2484.000f, -2978.000f, 241.280f},
        {2488.500f, -2958.250f, 241.280f},
        {2490.760f, -2940.015f, 241.360f},
        HorsemenPortalFloor,
    }};
    std::array<G3D::Vector3, 8> const HorsemenEast{{
        {2576.250f, -2955.500f, 241.310f},
        {2561.000f, -2947.000f, 241.280f},
        {2555.500f, -2930.500f, 241.280f},
        {2550.000f, -2914.000f, 241.280f},
        {2536.800f, -2896.600f, 241.280f},
        {2517.800f, -2896.600f, 241.280f},
        {2505.410f, -2909.190f, 241.360f},
        HorsemenPortalFloor,
    }};
    std::array<G3D::Vector3, 4> const HorsemenCenter{{
        {2535.000f, -2941.000f, 241.480f},
        {2525.000f, -2921.000f, 241.277f},
        {2515.000f, -2916.000f, 241.277f},
        HorsemenPortalFloor,
    }};
    // The straight mesh loses the grade from the raised dais to the room floor.
    std::array<G3D::Vector3, 3> const HorsemenDaisWest{{
        {2514.000f, -2938.000f, 245.192f},
        {2508.000f, -2937.000f, 241.283f},
        HorsemenPortalFloor,
    }};
    std::array<G3D::Vector3, 3> const HorsemenDaisEast{{
        {2538.000f, -2949.500f, 244.853f},
        {2540.500f, -2945.500f, 241.269f},
        HorsemenCenter.front(),
    }};

    struct WingPortal
    {
        uint32 encounter;
        uint32 gameObjectData;
        uint32 entry;
        std::array<uint32, 3> bosses;
        G3D::Vector3 floor;
        std::array<G3D::Vector3, 3> const* surveyedApproach = nullptr;
    };

    // Individual Progression's GO data keys; the destinations below are dry floor within use range.
    std::array<WingPortal, 3> const WingPortals{{
        {MaexxnaEncounter, 201, MaexxnaPortalEntry, {351009, 351007, 351006}, MaexxnaRamp.back(), &MaexxnaRamp},
        {LoathebEncounter, 200, LoathebPortalEntry, {351008, 351005, 351020}, LoathebApproach.back(), &LoathebApproach},
        {HorsemenEncounter, 203, HorsemenPortalEntry, {351036, 351035, 351037}, HorsemenPortalFloor},
    }};

    G3D::Vector3 Location(Player const& player)
    {
        return {player.GetPositionX(), player.GetPositionY(), player.GetPositionZ()};
    }

    bool Near(Player const& player, G3D::Vector3 const& point, float radius)
    {
        return (Location(player) - point).squaredLength() <= radius * radius;
    }

    bool InHorsemenRoom(Player const& player)
    {
        G3D::Vector3 const point = Location(player);
        // The room gate is at Y -3017.17. Do not arm from the outer corridor.
        return point.x >= 2450.0f && point.x <= 2630.0f && point.y >= -3016.0f && point.y <= -2880.0f &&
            point.z >= 239.0f && point.z <= 248.0f;
    }

    bool WingsDone(InstanceScript const& instance)
    {
        return instance.IsBossDone(LoathebEncounter) && instance.IsBossDone(MaexxnaEncounter) &&
            instance.IsBossDone(ThaddiusEncounter) && instance.IsBossDone(HorsemenEncounter);
    }

    bool Active(Player const& owner, DcRunState const& run)
    {
        Transit const& transfer = run.naxx40PortalTransit;
        return run.enabled && !transfer.members.empty() && owner.GetGroup() &&
            owner.GetGroup()->GetGUID() == transfer.group && owner.GetInstanceId() == transfer.instanceId;
    }

    bool SameGroup(Player const& owner, Player const& member)
    {
        return member.IsInWorld() && member.IsInMap(&owner) && member.GetInstanceId() == owner.GetInstanceId() &&
            member.InSamePhase(&owner) && member.GetGroup() == owner.GetGroup();
    }

    WingPortal const* FindWingPortal(uint32 entry)
    {
        for (auto const& wing : WingPortals)
            if (wing.entry == entry)
                return &wing;
        return nullptr;
    }

    uint32 ChoosePortal(Player const& owner, InstanceScript const& instance, uint32 nextBoss)
    {
        for (auto const& wing : WingPortals)
            if (instance.IsBossDone(wing.encounter) && (Near(owner, wing.floor, 75.0f) ||
                (wing.entry == HorsemenPortalEntry && InHorsemenRoom(owner) && Near(owner, wing.floor, WingWalkRadius))) &&
                std::ranges::find(wing.bosses, nextBoss) == wing.bosses.end())
                return wing.entry;

        if ((nextBoss == SapphironEntry || nextBoss == KelThuzadEntry) &&
            Near(owner, HubLanding, 25.0f) && WingsDone(instance))
            return HubTrigger;
        return 0;
    }

    bool MoveDry(PlayerbotAI& ai, G3D::Vector3 const& destination)
    {
        Player& bot = *ai.GetBot();
        auto points = DcNaxx40SlimeCrossing::BuildDryCampPath(&bot, {destination.x, destination.y, destination.z});
        for (std::size_t i = 1; i < points.size(); ++i)
        {
            auto const& from = points[i - 1];
            auto const& to = points[i];
            if (!bot.GetMap()->isInLineOfSight(from.x, from.y, from.z + 0.85f, to.x, to.y, to.z + 0.85f,
                    bot.GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing))
                return false;
        }
        return DcMovement::SplinePath(&ai, points, MovementPriority::MOVEMENT_NORMAL, FORCED_MOVEMENT_RUN);
    }

    bool WalkSurveyedApproach(PlayerbotAI& ai, std::array<G3D::Vector3, 3> const& approach)
    {
        Player* bot = ai.GetBot();
        G3D::Vector3 direction = approach.back() - approach.front();
        direction.z = 0.0f;
        direction /= direction.length();
        G3D::Vector3 const start = Location(*bot);
        for (auto point = approach.rbegin(); point != approach.rend(); ++point)
        {
            if ((*point - start).dot(direction) <= 0.65f)
                continue;

            auto path = DcNaxx40SlimeCrossing::BuildSurveyedDryLeg(bot, *point);
            if (!path.empty())
                return DcMovement::SplinePath(&ai, path, MovementPriority::MOVEMENT_NORMAL, FORCED_MOVEMENT_RUN);
        }
        return (approach.front() - start).dot(direction) > 0.65f && MoveDry(ai, approach.front());
    }

    bool WalkHorsemenDais(PlayerbotAI& ai)
    {
        Player& bot = *ai.GetBot();
        if (!InHorsemenRoom(bot) || bot.GetPositionZ() < 243.0f)
            return false;

        if (WalkSurveyedApproach(ai, HorsemenDaisWest) || WalkSurveyedApproach(ai, HorsemenDaisEast))
            return true;

        // A bot stopped by the eastern lip can be slightly below the WMO floor.
        // Follow the sampled grade for this short first step onto the east route.
        G3D::Vector3 const end = HorsemenDaisEast.front();
        if (!Near(bot, end, 2.0f) || Near(bot, end, 0.65f))
            return false;
        constexpr float probeLift = 1.0f;
        G3D::Vector3 const start = Location(bot);
        if (!bot.GetMap()->isInLineOfSight(start.x, start.y, start.z + probeLift, end.x, end.y, end.z + probeLift,
                bot.GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing))
            return false;

        G3D::Vector3 const leg = end - start;
        int const samples = static_cast<int>(std::ceil(leg.length() / 0.25f));
        Movement::PointsArray path{start};
        for (int index = 0; index <= samples; ++index)
        {
            G3D::Vector3 point = start + leg * (static_cast<float>(index) / samples);
            PositionFullTerrainStatus terrain;
            bot.GetMap()->GetFullTerrainStatusForPosition(bot.GetPhaseMask(), point.x, point.y,
                point.z + probeLift, bot.GetCollisionHeight(), terrain);
            if (terrain.floorZ < point.z - 0.5f || terrain.floorZ > point.z + 0.9f)
                return false;
            point.z = terrain.floorZ + 0.05f;
            if (!DcNaxx40SlimeCrossing::IsDryCampGround(bot, point))
                return false;
            if (index)
                path.push_back(point);
        }
        return DcMovement::SplinePath(&ai, path, MovementPriority::MOVEMENT_NORMAL, FORCED_MOVEMENT_RUN);
    }

    bool WalkHorsemenApproach(PlayerbotAI& ai)
    {
        Player* bot = ai.GetBot();
        if (!InHorsemenRoom(*bot))
            return false;

        std::array<std::span<G3D::Vector3 const>, 3> const routes{HorsemenWest, HorsemenEast, HorsemenCenter};
        G3D::Vector3 const start = Location(*bot);
        Movement::PointsArray bestPath;
        float bestLength = std::numeric_limits<float>::max();
        for (auto const route : routes)
        {
            float remaining = 0.0f;
            for (std::size_t index = route.size(); index-- > 0;)
            {
                if (index + 1 < route.size())
                    remaining += (route[index + 1] - route[index]).length();
                float const approach = (route[index] - start).length();
                float const length = approach + remaining;
                if (approach <= 0.65f || length >= bestLength)
                    continue;

                auto path = DcNaxx40SlimeCrossing::BuildSurveyedDryLeg(bot, route[index]);
                if (!path.empty())
                {
                    bestLength = length;
                    bestPath = std::move(path);
                }
            }
        }
        if (bestPath.empty() || !DcMovement::SplinePath(&ai, bestPath, MovementPriority::MOVEMENT_NORMAL, FORCED_MOVEMENT_RUN))
            return false;

        G3D::Vector3 const& end = bestPath.back();
        LOG_DEBUG("playerbots.dungeonclear", "[DC:{}] Naxx40 Horsemen portal skirt -> ({:.2f},{:.2f},{:.2f})",
            bot->GetName(), end.x, end.y, end.z);
        return true;
    }

    void UsePortal(Player& bot, GameObject const& go)
    {
        if (go.GetGoState() != GO_STATE_ACTIVE || go.HasGameObjectFlag(GO_FLAG_NOT_SELECTABLE))
            return;

        WorldPacket packet(CMSG_GAMEOBJ_USE);
        packet << go.GetGUID();
        packet.rpos(0);
        bot.GetSession()->HandleGameObjectUseOpcode(packet);
    }

    void ClearMovementAfterPortal(PlayerbotAI& ai)
    {
        DcMovement::ClearMovementWait(ai.GetBot());
        AiObjectContext* context = ai.GetAiObjectContext();
        context->GetValue<DcApproachState&>(DcKey::ApproachState)->Get().Reset();
        context->GetValue<uint32>(DcKey::CurrentHop)->Set(0u);
        context->GetValue<ChunkedPathfinder::Result&>(DcKey::LongPath)->Reset();
        context->GetValue<DungeonFollowerState&>(DcKey::FollowerState)->Get() = DungeonFollowerState{};
    }

    void DriveMember(PlayerbotAI& ai, Transit::Member& member, GameObject* go, G3D::Vector3 const& destination,
                     WingPortal const* wing, bool horsemenFallback, bool mayUse, uint32 now)
    {
        Player& bot = *ai.GetBot();
        bool const hub = !wing;
        auto const& spline = *bot.movespline;
        if (!bot.IsAlive() || bot.IsInCombat() || bot.IsBeingTeleported() || bot.IsFalling() || !ai.CanMove() ||
            !Near(bot, destination, hub ? 45.0f : WingWalkRadius) ||
            (member.spline && !spline.Finalized() && spline.GetId() == member.spline) ||
            now - member.lastAttempt < RetryDelayMs)
            return;

        member.lastAttempt = now;
        if (hub && Near(bot, HubTriggerCenter, 5.75f))
        {
            if (mayUse)
            {
                WorldPacket packet(CMSG_AREATRIGGER);
                packet << uint32(HubTrigger);
                packet.rpos(0);
                bot.GetSession()->HandleAreaTriggerOpcode(packet);
            }
            return;
        }
        if (!hub && go && go->IsWithinDistInMap(&bot, go->GetInteractionDistance()))
        {
            if (mayUse)
                UsePortal(bot, *go);
            return;
        }
        auto const* approach = wing ? wing->surveyedApproach : nullptr;
        if (approach ? WalkSurveyedApproach(ai, *approach) :
            MoveDry(ai, destination) || (horsemenFallback && (WalkHorsemenDais(ai) || WalkHorsemenApproach(ai))))
            member.spline = spline.GetId();
    }

    void Drive(PlayerbotAI& ai, InstanceScript& instance, uint32 now)
    {
        Player& owner = *ai.GetBot();
        DcRunState& run = DcRun::Of(&ai);
        Transit& transfer = run.naxx40PortalTransit;
        if (run.paused)
        {
            transfer.lastUpdate = 0;
            return;
        }
        if (!transfer.lastUpdate)
            transfer.lastProgress = now;
        if (now - transfer.lastUpdate < 200)
            return;
        transfer.lastUpdate = now;

        bool const hub = transfer.portalEntry == HubTrigger;
        WingPortal const* wing = FindWingPortal(transfer.portalEntry);
        if (!hub && !wing)
            return;
        GameObject* go = wing ? instance.GetGameObject(wing->gameObjectData) : nullptr;
        if (go && go->GetEntry() != transfer.portalEntry)
            go = nullptr;
        bool const allWingsDone = WingsDone(instance);
        bool const available = hub ? allWingsDone : instance.IsBossDone(wing->encounter);
        bool const ready = available && !instance.IsEncounterInProgress() &&
            !DcCombatFlag::AnyPartyEngagement(&owner) && !DcRezRecovery::IsPending(&owner);
        bool const horsemenFallback = wing && wing->entry == HorsemenPortalEntry && InHorsemenRoom(owner);
        G3D::Vector3 const& destination = hub ? HubLanding : wing->floor;
        unsigned arrived = 0;
        unsigned inUseRange = 0;
        Transit::Member* ownerMember = nullptr;
        for (Transit::Member& member : transfer.members)
        {
            Player* bot = ObjectAccessor::GetPlayer(owner, member.guid);
            PlayerbotAI* memberAI = bot && SameGroup(owner, *bot) ? GET_PLAYERBOT_AI(bot) : nullptr;
            if (!memberAI)
                continue;

            bool const reached = !bot->IsBeingTeleported() &&
                (hub ? Near(*bot, FrostwyrmLanding, 12.0f) :
                    Near(*bot, HubLanding, 12.0f) || (allWingsDone && Near(*bot, FrostwyrmLanding, 12.0f)));
            if (reached)
            {
                ++arrived;
                if (!member.arrived)
                {
                    member.arrived = true;
                    transfer.lastProgress = now;
                    ClearMovementAfterPortal(*memberAI);
                }
                continue;
            }
            member.arrived = false;
            inUseRange += hub ? Near(*bot, HubTriggerCenter, 5.75f) :
                go && go->IsWithinDistInMap(bot, go->GetInteractionDistance());
            if (bot == &owner)
                ownerMember = &member;
            else if (ready)
                DriveMember(*memberAI, member, go, destination, wing, horsemenFallback, true, now);
        }

        if (arrived == transfer.members.size())
        {
            LOG_INFO("playerbots.dungeonclear", "[DC:{}] Naxx40 portal {} complete instance {} arrived={}",
                owner.GetName(), transfer.portalEntry, transfer.instanceId, arrived);
            transfer = {};
            DcActionShared::SetPhase(ai.GetAiObjectContext(), "");
            return;
        }

        if (ready && ownerMember)
            DriveMember(ai, *ownerMember, go, destination, wing, horsemenFallback, arrived + 1 == transfer.members.size(), now);

        if (ready && now - transfer.lastProgress >= ProgressLimitMs)
        {
            run.paused = true;
            DcActionShared::SetPhase(ai.GetAiObjectContext(), "Naxxramas: waiting for the wing portal");
            G3D::Vector3 const position = Location(owner);
            LOG_WARN("playerbots.dungeonclear",
                "[DC:{}] Naxx40 portal {} stalled instance {} arrived={}/{} inUseRange={} goState={} selectable={} owner=({:.2f},{:.2f},{:.2f})",
                owner.GetName(), transfer.portalEntry, transfer.instanceId, arrived, transfer.members.size(), inUseRange,
                go ? int(go->GetGoState()) : -1, go && !go->HasGameObjectFlag(GO_FLAG_NOT_SELECTABLE),
                position.x, position.y, position.z);
        }
        if (!ready)
            transfer.lastProgress = now;
    }

    class PortalMapScript final : public AllMapScript
    {
    public:
        PortalMapScript() : AllMapScript("DungeonClearNaxx40WingPortal", {ALLMAPHOOK_ON_MAP_UPDATE}) {}

        void OnMapUpdate(Map* map, uint32) override
        {
            if (!IsNaxx40Map(map) || !DcModule::IsEnabled())
                return;
            uint32 const now = getMSTime();
            for (auto const& reference : map->GetPlayers())
            {
                Player* owner = reference.GetSource();
                PlayerbotAI* ai = owner ? GET_PLAYERBOT_AI(owner) : nullptr;
                if (ai && Active(*owner, DcRun::Of(ai)))
                    if (InstanceScript* instance = owner->GetInstanceScript())
                        Drive(*ai, *instance, now);
            }
        }
    };
}

bool Begin(PlayerbotAI* ai, uint32 nextBoss)
{
    Player* owner = ai ? ai->GetBot() : nullptr;
    if (!owner || !IsNaxx40Map(owner->FindMap()) || !owner->GetGroup() ||
        DcLeaderSignal::FindRunOwner(owner) != owner)
        return false;
    InstanceScript* instance = owner->GetInstanceScript();
    if (!instance)
        return false;
    DcRunState& run = DcRun::Of(ai);
    if (Active(*owner, run))
        return true;
    uint32 const portal = ChoosePortal(*owner, *instance, nextBoss);
    if (!run.enabled || run.paused || !portal)
        return false;
    if (instance->IsEncounterInProgress() || DcCombatFlag::AnyPartyEngagement(owner) || DcRezRecovery::IsPending(owner))
        return true;

    Transit transfer;
    transfer.group = owner->GetGroup()->GetGUID();
    transfer.instanceId = owner->GetInstanceId();
    transfer.portalEntry = portal;
    unsigned selfbots = 0;
    for (GroupReference* reference = owner->GetGroup()->GetFirstMember(); reference; reference = reference->next())
    {
        Player* member = reference->GetSource();
        if (!member || !SameGroup(*owner, *member))
            continue;
        if (PlayerbotAI* memberAI = GET_PLAYERBOT_AI(member))
        {
            transfer.members.push_back({member->GetGUID()});
            selfbots += DcPlayerbotCompat::IsSelfBot(member);
            DcRun::Of(memberAI).naxx40PortalOwner = owner->GetGUID();
            if (member->IsAlive() && !member->IsFalling())
                DcMovement::StopBot(member, DcMovement::Stop::Hold);
        }
    }
    if (transfer.members.empty())
        return false;

    LOG_INFO("playerbots.dungeonclear", "[DC:{}] Naxx40 portal {} armed instance {} bots={} selfbots={}",
        owner->GetName(), portal, transfer.instanceId, transfer.members.size(), selfbots);
    run.naxx40PortalTransit = std::move(transfer);
    DcActionShared::ClearStall(ai->GetAiObjectContext());
    DcActionShared::SetPhase(ai->GetAiObjectContext(), portal == HubTrigger ?
        "Naxxramas: going to Frostwyrm Lair" : "Naxxramas: returning to the hub");
    return true;
}

bool OwnsMovement(PlayerbotAI* ai)
{
    Player* bot = ai ? ai->GetBot() : nullptr;
    if (!bot || !bot->IsAlive() || bot->IsInCombat() || !IsNaxx40Map(bot->FindMap()) || !DcModule::IsEnabled())
        return false;
    ObjectGuid const guid = DcRun::Of(ai).naxx40PortalOwner;
    Player* owner = guid.IsEmpty() ? nullptr : ObjectAccessor::GetPlayer(*bot, guid);
    PlayerbotAI* ownerAI = owner && SameGroup(*owner, *bot) ? GET_PLAYERBOT_AI(owner) : nullptr;
    return ownerAI && Active(*owner, DcRun::Of(ownerAI));
}

void RegisterMapUpdate()
{
    new PortalMapScript();
}
}
