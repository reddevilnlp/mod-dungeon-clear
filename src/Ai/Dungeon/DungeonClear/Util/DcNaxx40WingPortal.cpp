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
    constexpr uint32 HubTrigger = 4156;
    constexpr uint32 RetryDelayMs = 1500;
    constexpr uint32 ProgressLimitMs = 75000;

    G3D::Vector3 const HubLanding{3005.51f, -3434.64f, 304.195f};
    G3D::Vector3 const HubTriggerCenter{3006.090f, -3434.170f, 306.195f};
    G3D::Vector3 const FrostwyrmLanding{3498.300f, -5349.490f, 144.968f};

    struct WingPortal
    {
        uint32 encounter;
        uint32 gameObjectData;
        uint32 entry;
        std::array<uint32, 3> bosses;
        G3D::Vector3 floor;
    };

    // Individual Progression's GO data keys; the destinations below are dry floor within use range.
    std::array<WingPortal, 3> const WingPortals{{
        {MaexxnaEncounter, 201, 181575, {351009, 351007, 351006}, {3465.753f, -3939.677f, 307.196f}},
        {LoathebEncounter, 200, 181577, {351008, 351005, 351020}, {2909.000f, -4025.020f, 273.691f}},
        {HorsemenEncounter, 203, 181578, {351036, 351035, 351037}, {2493.020f, -2921.780f, 241.440f}},
    }};

    G3D::Vector3 Location(Player const& player)
    {
        return {player.GetPositionX(), player.GetPositionY(), player.GetPositionZ()};
    }

    bool Near(Player const& player, G3D::Vector3 const& point, float radius)
    {
        return (Location(player) - point).squaredLength() <= radius * radius;
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
            if (instance.IsBossDone(wing.encounter) && Near(owner, wing.floor, 75.0f) &&
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
                     bool hub, bool mayUse, uint32 now)
    {
        Player& bot = *ai.GetBot();
        auto const& spline = *bot.movespline;
        if (!bot.IsAlive() || bot.IsInCombat() || bot.IsBeingTeleported() || bot.IsFalling() || !ai.CanMove() ||
            !Near(bot, destination, hub ? 45.0f : 110.0f) ||
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
        if (MoveDry(ai, destination))
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
        G3D::Vector3 const& destination = hub ? HubLanding : wing->floor;
        unsigned arrived = 0;
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
            if (bot == &owner)
                ownerMember = &member;
            else if (ready)
                DriveMember(*memberAI, member, go, destination, hub, true, now);
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
            DriveMember(ai, *ownerMember, go, destination, hub, arrived + 1 == transfer.members.size(), now);

        if (ready && now - transfer.lastProgress >= ProgressLimitMs)
        {
            run.paused = true;
            DcActionShared::SetPhase(ai.GetAiObjectContext(), "Naxxramas: waiting for the wing portal");
            LOG_WARN("playerbots.dungeonclear", "[DC:{}] Naxx40 portal {} stalled instance {} arrived={}/{}",
                owner.GetName(), transfer.portalEntry, transfer.instanceId, arrived, transfer.members.size());
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
