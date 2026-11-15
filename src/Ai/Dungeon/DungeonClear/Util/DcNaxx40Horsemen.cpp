/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 */

#include "DcNaxx40Horsemen.h"

#include "Ai/Dungeon/DungeonClear/Action/DcActionShared.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Data/SealedEncounterRegistry.h"
#include "Ai/Dungeon/DungeonClear/Settings/DcSettings.h"
#include "Ai/Raid/Naxx40/FourHorsemen40.h"
#include "Ai/Raid/Naxx40/Naxx40Encounter.h"
#include "Creature.h"
#include "DcMovement.h"
#include "DcNaxx40SlimeCrossing.h"
#include "DcRaidMuster.h"
#include "DcRezRecovery.h"
#include "Group.h"
#include "InstanceScript.h"
#include "Log.h"
#include "MoveSpline.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"

namespace
{
    using FourHorsemen40::Preparation;

    constexpr uint32 MograineEntry = 351037;
    constexpr uint32 BlaumeuxEntry = 351040;
    constexpr uint32 Encounter = 12;
    Position const SplitCamp(2555.9f, -2996.4f, 241.4f);
    Position const PullPoint(2516.0f, -2983.0f, 241.276f);
    constexpr float ApproachRange = 15.0f;

    bool BeforePull(Player const& bot)
    {
        if (bot.IsInCombat() || !IsNaxx40Map(bot.FindMap()))
            return false;
        InstanceScript const* instance = bot.GetInstanceScript();
        return instance && instance->GetBossState(Encounter) == NOT_STARTED && !instance->IsEncounterInProgress();
    }

    bool WalkDry(PlayerbotAI& ai, Position const& destination)
    {
        Player* const tank = ai.GetBot();
        G3D::Vector3 const end(destination.GetPositionX(), destination.GetPositionY(), destination.GetPositionZ());
        if (DcMovement::OwnsRunningEscort(tank) && (tank->movespline->FinalDestination() - end).squaredLength() < 1.0f)
            return true;

        auto path = DcNaxx40SlimeCrossing::BuildDryCampPath(tank, destination);
        if (path.empty())
            return false;
        DcMovement::ClearMovementWait(tank);
        return DcMovement::SplinePath(&ai, path, MovementPriority::MOVEMENT_COMBAT, FORCED_MOVEMENT_RUN);
    }

    bool RoomMusterReady(Player* tank)
    {
        SealedEncounterRow const* room = SealedEncounterRegistry::Find(tank->GetMapId(), MograineEntry);
        Group* group = tank->GetGroup();
        if (!room || !group)
            return false;

        uint32 living = 0;
        uint32 inside = 0;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || !member->IsInWorld() || !member->IsAlive() || !member->IsInMap(tank) ||
                !member->InSamePhase(tank) || !GET_PLAYERBOT_AI(member))
                continue;
            ++living;
            if (SealedEncounterRegistry::InSealedRoom(*room, member->GetPositionX(),
                    member->GetPositionY(), member->GetPositionZ()))
                ++inside;
            else if (PlayerbotAI::IsTank(member) || PlayerbotAI::IsHeal(member))
                return false;
        }
        return living && inside * 100 >= living * DcSettings::GetUInt(tank, "RaidReadyQuorumPct");
    }
}

bool DcNaxx40Horsemen::IsObjective(Player const* bot, uint32 entry)
{
    return bot && entry == MograineEntry && IsNaxx40Map(bot->FindMap());
}

bool DcNaxx40Horsemen::IsEncounterMob(Player const& bot, uint32 entry)
{
    return entry >= MograineEntry && entry <= BlaumeuxEntry && IsNaxx40Map(bot.FindMap());
}

std::optional<bool> DcNaxx40Horsemen::Stage(PlayerbotAI& ai, DungeonBossInfo const& next)
{
    Player* bot = ai.GetBot();
    if (!IsObjective(bot, next.entry))
        return std::nullopt;
    if (!BeforePull(*bot))
        return false;

    auto const current = FourHorsemen40::GetOpeningPreparation(ai);
    AiObjectContext* const context = ai.GetAiObjectContext();
    if (current == Preparation::Inactive)
    {
        // The approach enters the split camp before the boss can body-aggro.
        // Also allow a resumed run already past the camp to arrange a fresh split.
        if (bot->GetExactDist(&SplitCamp) > ApproachRange)
        {
            if (bot->GetPositionY() <= SplitCamp.GetPositionY() || bot->GetPositionZ() < 239.0f ||
                bot->GetPositionZ() > 248.0f)
                return std::nullopt;
            if (WalkDry(ai, SplitCamp))
                return true;
            DcMovement::StopBot(bot, DcMovement::Stop::HardPin);
            DcActionShared::StallDungeonClear(&ai, "Four Horsemen: waiting for a dry path back to the split camp.");
            return false;
        }

        DcMovement::StopBot(bot, DcMovement::Stop::HardPin);
        DcMovement::ClearMovementWait(bot);
        if (!RoomMusterReady(bot))
        {
            DcActionShared::SetPhase(context, "waiting for the raid inside the Four Horsemen gate");
            return false;
        }
        if (DcRezRecovery::IsPending(bot) || DcRaidMuster::Holds(bot, &ai, context, next))
            return false;
    }

    auto const preparation = FourHorsemen40::PrepareOpening(ai);
    DcActionShared::ClearStall(context);
    DcActionShared::SetPhase(context, preparation == Preparation::Ready
            ? "Four Horsemen groups ready" : "staging the Four Horsemen groups");
    return true;
}

bool DcNaxx40Horsemen::OwnsMovement(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    return bot && BeforePull(*bot) && FourHorsemen40::GetOpeningPreparation(ai) != Preparation::Inactive;
}

bool DcNaxx40Horsemen::Ready(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    return bot && BeforePull(*bot) && FourHorsemen40::GetOpeningPreparation(ai) == Preparation::Ready;
}

bool DcNaxx40Horsemen::ConfirmReady(PlayerbotAI& ai)
{
    return Ready(ai) && FourHorsemen40::PrepareOpening(ai) == Preparation::Ready;
}

std::optional<bool> DcNaxx40Horsemen::TryRangedPull(PlayerbotAI& ai, Creature& boss)
{
    Player& tank = *ai.GetBot();
    auto const opener = DcActionShared::ResolvePullSpell(&ai, &tank);
    if (!opener)
        return std::nullopt;

    if (tank.GetExactDist(&PullPoint) > 1.0f)
    {
        if (WalkDry(ai, PullPoint))
            return true;
        DcMovement::StopBot(&tank, DcMovement::Stop::HardPin);
        DcActionShared::StallDungeonClear(&ai, "Four Horsemen: waiting for a dry path to the pull point.");
        return false;
    }

    float const distance = tank.GetExactDist(&boss);
    if (distance < opener->minRange || distance > opener->maxRange || !tank.IsWithinLOSInMap(&boss))
        return std::nullopt;

    if (tank.IsNonMeleeSpellCast(false))
        return true;
    DcMovement::StopBot(&tank, DcMovement::Stop::HardPin);
    tank.SetSelection(boss.GetGUID());
    tank.SetFacingToObject(&boss);
    if (!ai.CastSpell(opener->spellId, &boss))
        return std::nullopt;

    DcActionShared::ClearStall(ai.GetAiObjectContext());
    LOG_INFO("playerbots.dungeonclear", "[DC:{}] Four Horsemen: ranged tag on {} from {:.1f}yd",
        tank.GetName(), boss.GetName(), distance);
    return true;
}
