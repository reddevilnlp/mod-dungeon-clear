/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, or later.
 */

#include "DcNaxx40Razuvious.h"

#include "Ai/Dungeon/DungeonClear/Action/DcActionShared.h"
#include "Ai/Raid/Naxx40/Naxx40Encounter.h"
#include "Ai/Raid/Naxx40/Razuvious40.h"
#include "Creature.h"
#include "DcLeaderSignal.h"
#include "DcMovement.h"
#include "DcNaxx40SlimeCrossing.h"
#include "DcRun.h"
#include "Group.h"
#include "InstanceScript.h"
#include "Log.h"
#include "MoveSpline.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Playerbots.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Timer.h"

#include <algorithm>
#include <cmath>
#include <list>

namespace DcNaxx40Razuvious
{
namespace
{
    constexpr uint32 Razuvious = 351036;
    constexpr uint32 Understudy = 351084;
    constexpr uint32 MindControl = 605;
    constexpr uint32 BoneBarrier = 29061;
    constexpr uint32 RazuviousEncounter = 10;
    constexpr uint32 PriestSetupMs = 18000;
    constexpr uint32 TankHoldMs = 12000;
    constexpr uint32 StageTimeoutMs = 8000;
    G3D::Vector3 const Camp{2801.0f, -3104.0f, 268.012f};
    G3D::Vector3 const PriestStand{2800.0f, -3102.0f, 267.685f};

    using Opening = DcRunState::RazuviousOpening;

    G3D::Vector3 Location(WorldObject const& object)
    {
        return {object.GetPositionX(), object.GetPositionY(), object.GetPositionZ()};
    }

    bool Near(WorldObject const& object, G3D::Vector3 const& point, float radius)
    {
        return (Location(object) - point).squaredMagnitude() <= radius * radius;
    }

    bool Walk(PlayerbotAI& ai, G3D::Vector3 const& destination, bool surveyed)
    {
        Player* bot = ai.GetBot();
        if (DcMovement::OwnsRunningEscort(bot) && bot->movespline &&
            (bot->movespline->FinalDestination() - destination).squaredMagnitude() < 1.0f)
            return true;
        auto path = surveyed
            ? DcNaxx40SlimeCrossing::BuildSurveyedDryLeg(bot, destination)
            : DcNaxx40SlimeCrossing::BuildDryCampPath(bot, {destination.x, destination.y, destination.z});
        return DcMovement::SplinePath(&ai, path, MovementPriority::MOVEMENT_COMBAT, FORCED_MOVEMENT_RUN);
    }

    bool ClearOfAggro(Player const& player, G3D::Vector3 const& point, Creature const& enemy)
    {
        float const radius = enemy.GetAggroRange(&player) + enemy.m_CombatDistance + 0.35f;
        return !enemy.IsAlive() || !Near(enemy, point, radius);
    }

    bool IsControlPriest(Player* player, Player const& tank)
    {
        return player && player->IsAlive() && player->GetGroup() == tank.GetGroup() &&
            player->IsInMap(&tank) && tank.InSamePhase(player) &&
            player->getClass() == CLASS_PRIEST && player->HasSpell(MindControl) && GET_PLAYERBOT_AI(player);
    }

    bool TryControl(PlayerbotAI& ai, Creature& target, Creature& boss, std::list<Creature*> const& studies)
    {
        Player& priest = *ai.GetBot();
        if (Unit* charm = priest.GetCharm())
            return charm->GetEntry() == Understudy && DirectRazuvious40Understudy(ai, boss);
        if (priest.IsNonMeleeSpellCast(false))
            return true;

        SpellInfo const* spell = sSpellMgr->GetSpellInfo(MindControl);
        if (!spell || !target.IsAlive() || target.IsCharmed())
            return false;
        float const reach = priest.GetSpellMaxRangeForTarget(&target, spell) +
            priest.GetCombatReach() + target.GetCombatReach();
        if (!Near(target, PriestStand, reach - 0.2f))
            return false;

        G3D::Vector3 const start = Location(priest);
        // This leg starts at the camp and ends at the surveyed casting spot. Check
        // both endpoints and the segment against the actual NPCs before moving.
        G3D::Vector3 const leg = PriestStand - start;
        auto const clearsLeg = [&](Creature const& enemy)
        {
            float const length = leg.squaredMagnitude();
            float const t = length > 0.0f
                ? std::clamp((Location(enemy) - start).dot(leg) / length, 0.0f, 1.0f) : 0.0f;
            return ClearOfAggro(priest, start + leg * t, enemy);
        };
        if (!clearsLeg(boss) || std::ranges::any_of(studies, [&](Creature const* study) { return !clearsLeg(*study); }))
            return false;

        if (!Near(priest, PriestStand, 0.5f))
            return Walk(ai, PriestStand, true);
        if (!priest.IsWithinLOSInMap(&target) || !priest.IsWithinCombatRange(&target,
            priest.GetSpellMaxRangeForTarget(&target, spell)))
            return false;
        DcMovement::StopBot(&priest, DcMovement::Stop::HardPin);
        priest.SetFacingToObject(&target);
        return ai.CanCastSpell(MindControl, &target) && ai.CastSpell(MindControl, &target);
    }
}

bool UsesOpening(PlayerbotAI* ai, uint32 nextBoss)
{
    Player* bot = ai ? ai->GetBot() : nullptr;
    if (nextBoss != Razuvious || !bot || !IsNaxx40Map(bot->FindMap()) ||
        !Near(*bot, Camp, 36.0f) || std::abs(bot->GetPositionZ() - Camp.z) > 8.0f)
        return false;
    InstanceScript const* instance = bot->GetInstanceScript();
    return instance && !instance->IsBossDone(RazuviousEncounter) && !instance->IsEncounterInProgress() &&
        !DcRun::Of(ai).naxx40RazuviousOpening.useNormalPull;
}

bool AtCamp(Player const& bot)
{
    return Near(bot, Camp, 1.6f);
}

bool Stage(PlayerbotAI* ai)
{
    Player& tank = *ai->GetBot();
    Opening& opening = DcRun::Of(ai).naxx40RazuviousOpening;
    if (AtCamp(tank))
        DcMovement::StopBot(&tank, DcMovement::Stop::Hold);
    else if (Walk(*ai, Camp, false))
        opening.stageBlocked = 0;
    else
    {
        DcMovement::StopBot(&tank, DcMovement::Stop::HardPin);
        uint32 const now = getMSTime();
        if (!opening.stageBlocked)
            opening.stageBlocked = now;
        if (getMSTimeDiff(opening.stageBlocked, now) >= StageTimeoutMs)
        {
            opening.useNormalPull = true;
            LOG_WARN("playerbots.dungeonclear",
                "[DC:{}] Razuvious opening: camp path unavailable, using the ordinary approach", tank.GetName());
            return false;
        }
    }
    DcActionShared::ClearStall(ai->GetAiObjectContext());
    DcActionShared::SetPhase(ai->GetAiObjectContext(), "staging before Instructor Razuvious");
    return true;
}

bool Open(PlayerbotAI* ai, Creature& boss)
{
    Player& tank = *ai->GetBot();
    if (!AtCamp(tank))
        return false;
    Opening& opening = DcRun::Of(ai).naxx40RazuviousOpening;
    uint32 const now = getMSTime();
    if (opening.committed && getMSTimeDiff(opening.committed, now) < TankHoldMs)
        return true;
    if (opening.committed)
        opening = {};
    if (!opening.started)
        opening.started = now;
    opening.boss = boss.GetGUID();

    std::list<Creature*> studies;
    tank.GetCreatureListWithEntryInGrid(studies, Understudy, 60.0f);
    Creature* target = nullptr;
    for (Creature* study : studies)
        if (study->IsAlive() && (!target ||
            (Location(*study) - PriestStand).squaredMagnitude() < (Location(*target) - PriestStand).squaredMagnitude()))
            target = study;

    Player* priest = opening.priest.IsEmpty() ? nullptr : ObjectAccessor::GetPlayer(tank, opening.priest);
    if (!IsControlPriest(priest, tank))
    {
        priest = nullptr;
        opening.priest.Clear();
        if (Group* group = tank.GetGroup())
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!IsControlPriest(member, tank) || !Near(*member, Camp, 10.0f))
                    continue;
                if (!priest || PlayerbotAI::IsHeal(priest))
                    priest = member;
            }
    }
    if (IsControlPriest(priest, tank) && target && getMSTimeDiff(opening.started, now) < PriestSetupMs)
    {
        opening.priest = priest->GetGUID();
        if (TryControl(*GET_PLAYERBOT_AI(priest), *target, boss, studies))
            return true;
    }

    // If there is no valid Mind Control cast from the stand, tag the study from
    // camp so the raid and healers are already behind the tank when it arrives.
    DcMovement::StopBot(&tank, DcMovement::Stop::HardPin);
    if (auto const tag = DcActionShared::ResolvePullSpell(ai, &tank); tag && target)
    {
        float const distance = tank.GetExactDist(target);
        if (distance >= tag->minRange && distance <= tag->maxRange && tank.IsWithinLOSInMap(target))
        {
            tank.SetFacingToObject(target);
            if (ai->CastSpell(tag->spellId, target))
            {
                opening.committed = now;
                opening.priest.Clear();
                LOG_INFO("playerbots.dungeonclear",
                    "[DC:{}] Razuvious opening: ranged tag from camp at {:.1f}yd", tank.GetName(), distance);
                return true;
            }
        }
    }
    if (getMSTimeDiff(opening.started, now) < PriestSetupMs)
        return true;

    opening.priest.Clear();
    opening.useNormalPull = true;
    LOG_WARN("playerbots.dungeonclear",
        "[DC:{}] Razuvious opening: no control or ranged tag, using the normal pull from camp", tank.GetName());
    return false;
}

bool HoldsPosition(PlayerbotAI* ai)
{
    Player* bot = ai ? ai->GetBot() : nullptr;
    if (!bot || !IsNaxx40Map(bot->FindMap()) || !Near(*bot, Camp, 20.0f))
        return false;

    if (bot->getClass() == CLASS_PRIEST && !bot->IsInCombat())
    {
        Player* owner = DcLeaderSignal::FindRunOwner(bot);
        PlayerbotAI* ownerAI = owner && owner->IsInMap(bot) ? GET_PLAYERBOT_AI(owner) : nullptr;
        if (ownerAI)
        {
            auto const& leaderRun = DcRun::Of(ownerAI);
            if (leaderRun.enabled && !leaderRun.paused && leaderRun.naxx40RazuviousOpening.priest == bot->GetGUID())
                return true;
        }
    }

    auto const& run = DcRun::Of(ai);
    auto const& opening = run.naxx40RazuviousOpening;
    if (!run.enabled || run.paused)
        return false;
    uint32 const now = getMSTime();
    bool const tagged = opening.committed && getMSTimeDiff(opening.committed, now) < TankHoldMs;
    bool const controlling = !opening.priest.IsEmpty() && opening.started &&
        getMSTimeDiff(opening.started, now) < PriestSetupMs + TankHoldMs;
    if (!tagged && !controlling)
        return false;
    Creature* boss = ObjectAccessor::GetCreature(*bot, opening.boss);
    Unit* victim = boss ? boss->GetVictim() : nullptr;
    return boss && boss->IsAlive() && (!victim || victim->GetEntry() != Understudy || !victim->HasAura(BoneBarrier));
}
}
