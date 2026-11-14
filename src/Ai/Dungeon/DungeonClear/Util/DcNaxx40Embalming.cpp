/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, or later.
 */

#include "DcNaxx40Embalming.h"

#include "Ai/Raid/Naxx40/Naxx40Encounter.h"
#include "Ai/Raid/RaidClear/Common/WayOut.h"
#include "CombatManager.h"
#include "DcEngageGeometry.h"
#include "DcLeaderSignal.h"
#include "DcPullContext.h"
#include "DcRun.h"
#include "DcTickMemo.h"
#include "Group.h"
#include "Player.h"
#include "Playerbots.h"
#include "Timer.h"

namespace
{
    constexpr uint32 EmbalmingSlime = 351026;
    constexpr float CloudMargin = 11.0f;
    Position const SouthCamp(3137.1f, -3268.5f, 294.374f);
    Position const SouthRetreat(3152.0f, -3270.0f, 294.559f);

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
                hold.authored = pull.camp.GetExactDist(&SouthCamp) < 1.0f;
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
    return camp.GetExactDist(&SouthCamp) < 1.0f ? SouthRetreat : camp;
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
        return SouthRetreat;
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
