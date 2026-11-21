/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, or later.
 */

#include "Ai/Dungeon/DungeonClear/Action/DcActionShared.h"
#include "Ai/Dungeon/DungeonClear/Data/DcNaxx40Gothik.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonEventRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonRosterBuilders.h"
#include "Ai/Dungeon/DungeonClear/Data/SealedEncounterRegistry.h"
#include "Ai/Dungeon/DungeonClear/Overrides/ObjectiveHookRegistry.h"
#include "Ai/Dungeon/DungeonClear/Util/DcCombatFlag.h"
#include "Ai/Dungeon/DungeonClear/Util/DcDifficulty.h"
#include "Ai/Dungeon/DungeonClear/Util/DcMovement.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRezRecovery.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRun.h"

#include "Creature.h"
#include "Event.h"
#include "Group.h"
#include "InstanceScript.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Playerbots.h"
#include "ServerFacade.h"
#include "SharedDefines.h"

#include <utility>
#include <vector>

namespace
{
    using namespace DcNaxx40Gothik;
    using Result = ObjectiveArriveResult;

    constexpr uint32 BossSlot = 11;
    constexpr uint32 BossGuidSlot = 105;
    constexpr uint32 GOTHIK_PREP_EVENT = 53320;
    constexpr uint32 HookId = 53320;
    constexpr int32 PreparationOrder = 65;
    constexpr uint32 Shoot = 3018;
    constexpr char BossName[] = "Gothik the Harvester";
    constexpr char PreparationName[] = "Gothik living-side preparation";

    // The raid stays on the dry living-side floor below the balcony.
    constexpr float PullX = 2659.0f;
    constexpr float PullY = -3387.0f;
    constexpr float PullZ = 267.685f;
    constexpr float ArrivalRadius = 1.5f;

    InstanceScript* Naxx40Instance(Player* bot)
    {
        if (!bot || bot->GetMapId() != MapId || DcDifficulty::Of(bot->FindMap()) != DcDiffKey::Raid(2))
            return nullptr;
        return bot->GetInstanceScript();
    }

    bool LivingSideReady(Player* tank)
    {
        SealedEncounterRow const* room = SealedEncounterRegistry::Find(MapId, Entry);
        if (!room)
            return false;

        Group* group = tank->GetGroup();
        if (!group)
            return true;

        for (Group::MemberSlot const& slot : group->GetMemberSlots())
        {
            Player* member = ObjectAccessor::GetPlayer(*tank, slot.guid);
            if (!member || !member->IsAlive() || !member->IsInMap(tank))
                continue;
            if (member->IsBeingTeleported() || !member->InSamePhase(tank) ||
                !SealedEncounterRegistry::InSealedRoom(*room, member->GetPositionX(),
                    member->GetPositionY(), member->GetPositionZ()))
                return false;
        }
        return true;
    }

    Result ShootFromLivingSide(Player* tank, AiObjectContext* context, DungeonBossInfo const&)
    {
        InstanceScript* instance = Naxx40Instance(tank);
        if (!instance)
            return Result::Blocked;

        // Leave the anchor pending while the boss is engaged. The raid engine
        // drives the fight; only a kill completes the anchor, so a reset rearms it.
        EncounterState const state = instance->GetBossState(BossSlot);
        if (state == DONE)
            return Result::Done;
        if (state == IN_PROGRESS || DcRun::Of(context).paused)
            return Result::Running;
        if (tank->GetExactDist(PullX, PullY, PullZ) > ArrivalRadius)
            return Result::Restart;

        PlayerbotAI* ai = GET_PLAYERBOT_AI(tank);
        Creature* boss = ObjectAccessor::GetCreature(*tank, instance->GetGuidData(BossGuidSlot));
        if (!ai || !boss || boss->GetEntry() != Entry || !boss->IsAlive())
            return Result::Running;

        DcMovement::StopBot(tank, DcMovement::Stop::HardPin);
        if (DcCombatFlag::AnyPartyEngagement(tank) || DcRezRecovery::IsPending(tank) ||
            !LivingSideReady(tank) || DcActionShared::PauseAtBoss(ai, boss, BossName, Event()))
            return Result::Running;

        if (DcRun::Of(context).Throttled(DcThrottle::StationaryBossOpener, 3000))
            return Result::Running;

        auto const shot = DcActionShared::ResolveRangedWeaponPull(ai, tank);
        if (!shot || shot->spellId != Shoot)
        {
            LOG_DEBUG("playerbots.dungeonclear", "[DC:{}] Gothik opener needs a bow, gun or crossbow with ammunition",
                      tank->GetName());
            return Result::Running;
        }

        float const distance = tank->GetExactDist(boss);
        if (!tank->IsValidAttackTarget(boss) || distance < shot->minRange ||
            distance > shot->maxRange || !tank->IsWithinLOSInMap(boss))
            return Result::Running;

        tank->SetSelection(boss->GetGUID());
        if (!tank->HasInArc(CAST_ANGLE_IN_FRONT, boss))
            ServerFacade::instance().SetFacingTo(tank, boss);
        if (ai->CastSpell(shot->spellId, boss))
            LOG_INFO("playerbots.dungeonclear", "[DC:{}] Naxx40 Gothik ranged tag from ({:.2f},{:.2f},{:.2f}), spell {} at {:.1f}yd",
                     tank->GetName(), tank->GetPositionX(), tank->GetPositionY(), tank->GetPositionZ(),
                     shot->spellId, distance);
        return Result::Running;
    }
}

bool DcNaxx40Gothik::AwaitingOpener(Player* bot)
{
    InstanceScript* instance = Naxx40Instance(bot);
    if (!instance)
        return false;
    EncounterState const state = instance->GetBossState(BossSlot);
    return state != IN_PROGRESS && state != DONE;
}

void RegisterNaxx40GothikEvents(std::vector<DungeonEvent>& out)
{
    out.push_back(EventBuilder(533, GOTHIK_PREP_EVENT, PreparationName)
        .Anchored(PreparationOrder)
        .ForDifficulty(DcDiffKey::Raid(2))
        .Persistent()
        .StepsOwnMovement()
        .MoveTo(PullX, PullY, PullZ, ArrivalRadius)
        .Custom(DC_HOOK_RAID_MUSTER)
        .Custom(HookId).Timeout(300000)
        .Build());
}

void RegisterNaxx40GothikRoster(std::vector<BossRosterPatch>& out)
{
    BossRosterPatch patch;
    patch.mapId = 533;
    patch.exactDifficulty = DcDiffKey::Raid(2);
    DungeonBossInfo preparation = DcRoster::MakeObjective(
        DcRoster::OBJ(20), 7, 533, PreparationName,
        PullX, PullY, PullZ, 5.0f, 0, 0, GOTHIK_PREP_EVENT, PreparationOrder);
    preparation.doneBossStateIndex = BossSlot;
    preparation.preparesBossEntry = Entry;
    patch.add.push_back(std::move(preparation));
    out.push_back(std::move(patch));
}

void RegisterNaxx40GothikHooks(ObjectiveHookRegistry::HookTable& out)
{
    ObjectiveHookRegistry::AddHook(out, HookId, &ShootFromLivingSide);
}
