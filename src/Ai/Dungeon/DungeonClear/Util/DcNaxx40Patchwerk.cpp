/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, or later.
 */

#include "DcNaxx40Patchwerk.h"

#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"
#include "Ai/Dungeon/DungeonClear/DcPullContext.h"
#include "Ai/Dungeon/DungeonClear/Action/DcActionShared.h"
#include "Ai/Dungeon/DungeonClear/Settings/DcSettings.h"
#include "Ai/Raid/Naxx40/Naxx40Encounter.h"
#include "Ai/Raid/Naxx40/Patchwerk40.h"
#include "AttackersValue.h"
#include "Creature.h"
#include "CreatureGroups.h"
#include "DcEngageGeometry.h"
#include "DcLeaderSignal.h"
#include "DcMovement.h"
#include "DcNaxx40Embalming.h"
#include "DcNaxx40SlimeCrossing.h"
#include "DcPullPlanner.h"
#include "DcRun.h"
#include "DcStatusPublisher.h"
#include "DcTargeting.h"
#include "DungeonClearMath.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "MoveSpline.h"
#include "PathGenerator.h"
#include "Player.h"
#include "Playerbots.h"
#include "Timer.h"
#include "WorldSession.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace DcNaxx40Patchwerk
{
namespace
{
    constexpr float Floor = 294.1f;

    struct Checkpoint
    {
        float x;
        float y;
        float radius = 0.0f;
        bool crossing = false;
        float z = Floor;
        ObjectGuid::LowType formation = 0;
        ObjectGuid::LowType patrol = 0;
        Position const* camp = nullptr;
    };

    constexpr float Arrive = 2.0f;
    constexpr float LocalPullRange = 42.0f;
    constexpr float FinalPullRange = 55.0f;
    constexpr float CampRadius = 3.0f;
    using Patchwerk40Position::RaidCamp;
    using Patchwerk40Position::PullCampX;
    using Patchwerk40Position::PullCampY;
    using Patchwerk40Position::PullCampZ;
    constexpr float ShotBankX = 3101.8f;
    constexpr float ShotBankY = -3117.0f;
    constexpr float ShotBankZ = 294.064f;
    Position const& Camp = Patchwerk40Position::PullCamp;
    Position const ShotBank(ShotBankX, ShotBankY, ShotBankZ);
    Position const PatrolStop(3107.4f, -3145.3f, 294.063f);
    std::array<Position, 2> const AssistWait = {{
        {3101.3f, -3105.0f, 294.0702f},
        {3106.0f, -3105.0f, 294.0698f},
    }};
    std::array<Position, 2> const AssistIntercept = {{
        {3101.3f, -3107.3f, 294.0692f},
        {3106.0f, -3107.3f, 294.0686f},
    }};
    constexpr float FormationPositionRadius = 1.0f;
    constexpr float BossArrivalRadius = 9.0f;
    constexpr float AssistAdvanceRadius = 13.0f;
    constexpr uint32 AssistHeadStartMs = 3000;
    constexpr uint8 PreparationOverrideIcon = 4; // Moon on the pull tank, as for C'Thun.

    bool IsHeadlessFollower(Player const* member)
    {
        return member && member->GetSession() && member->GetSession()->IsHeadless();
    }

    bool IsManagedFollower(Player const* member, Player const* tank)
    {
        return IsHeadlessFollower(member) && member != tank && member->IsAlive() && member->IsInMap(tank);
    }

    std::optional<std::size_t> AssistSlot(Player* member)
    {
        for (std::size_t slot = 0; slot < AssistWait.size(); ++slot)
            if (PlayerbotAI::IsAssistTankOfIndex(member, static_cast<uint8>(slot), true))
                return slot;
        return std::nullopt;
    }

    std::array<Player*, AssistWait.size() + 1> PatrolTanks(Player* tank)
    {
        std::array<Player*, AssistWait.size() + 1> tanks{tank};
        if (Group* const group = tank->GetGroup())
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* const member = ref->GetSource();
                if (!member || member == tank || !member->IsAlive() || !member->IsInMap(tank))
                    continue;
                if (std::optional<std::size_t> const slot = AssistSlot(member))
                    tanks[*slot + 1] = member;
            }
        return tanks;
    }

    PlayerbotAI* TankPreparationAI(Player* tank)
    {
        return IsHeadlessFollower(tank) ? GET_PLAYERBOT_AI(tank) : nullptr;
    }

    void AddMissing(std::string& list, std::string const& requirement)
    {
        if (!list.empty())
            list += ", ";
        list += requirement;
    }

    enum class PreparationResult
    {
        Ready,
        Waiting,
        Overridden
    };

    PreparationResult PreparePatrolTanks(PlayerbotAI* leaderAI, AiObjectContext* context, bool atShot)
    {
        using namespace Patchwerk40TankSupplies;
        Player* const leader = leaderAI->GetBot();
        Group* const group = leader->GetGroup();
        bool const marked = group && group->isRaidGroup() && group->GetTargetIcon(PreparationOverrideIcon) ==
                            leader->GetGUID();
        auto const tanks = PatrolTanks(leader);
        std::array<bool, AssistWait.size() + 1> needsCamp{};
        std::array<bool, AssistWait.size() + 1> shieldActive{};
        std::string missing;
        bool campReady = true;
        bool shotReady = true;
        for (std::size_t index = 0; index < tanks.size(); ++index)
        {
            Player* const tank = tanks[index];
            if (!tank)
            {
                AddMissing(missing, "off-tank " + std::to_string(index) + " unavailable");
                continue;
            }
            bool const flask = HasTimeRemaining(*tank, FlaskAura, MinimumFlaskMs);
            bool const food = HasTimeRemaining(*tank, FoodAura, MinimumFoodMs);
            bool const eating = HasTimeRemaining(*tank, EatingAura, 0);
            shieldActive[index] = HasTimeRemaining(*tank, StoneshieldAura, MinimumStoneshieldMs);
            uint32 const needStoneshield = shieldActive[index] ? 1 : 2;
            PlayerbotAI* const tankAI = TankPreparationAI(tank);
            std::string needed;
            if (!flask && !tank->HasItemCount(FlaskItem, 1))
                AddMissing(needed, "Flask of the Titans");
            else if (!flask && !tankAI)
                AddMissing(needed, "use Flask of the Titans");
            if (!food && !eating && !tank->HasItemCount(FoodItem, 1))
                AddMissing(needed, "Chimaerok Chops");
            else if (!food && !eating && !tankAI)
                AddMissing(needed, "eat Chimaerok Chops");
            if (!tank->HasItemCount(StoneshieldItem, needStoneshield))
                AddMissing(needed, std::to_string(needStoneshield) + " Greater Stoneshield in bags");
            else if (atShot && !shieldActive[index] && !tankAI)
                AddMissing(needed, "use Greater Stoneshield");
            if (!needed.empty())
                AddMissing(missing, tank->GetName() + " (" + needed + ")");

            needsCamp[index] = !flask || !food;
            campReady = campReady && !needsCamp[index];
            shotReady = shotReady && shieldActive[index];
        }

        bool const override = marked && !missing.empty();
        for (std::size_t index = 0; index < tanks.size(); ++index)
        {
            PlayerbotAI* const tankAI = TankPreparationAI(tanks[index]);
            if (!tankAI)
                continue;
            if (atShot)
            {
                bool const publishShot = (campReady && missing.empty()) || (override && shieldActive[index]);
                SetPatchwerk40TankPreparation(*tankAI, publishShot ? Patchwerk40TankPreparation::Shot
                                                                  : Patchwerk40TankPreparation::Idle);
            }
            else if (!atShot && needsCamp[index])
                SetPatchwerk40TankPreparation(*tankAI, override ? Patchwerk40TankPreparation::Idle
                                                               : Patchwerk40TankPreparation::Camp);
        }

        DcRunState& run = DcRun::Of(context);
        std::string& previous = run.patchwerkSweep.preparationWarning;
        if (!missing.empty() && !override)
        {
            if (missing != previous)
            {
                previous = missing;
                std::string const warning = "Patchwerk waiting for supplies: " + missing +
                    ". Mark the pull tank with Moon to continue this pull anyway.";
                LOG_INFO("playerbots.dungeonclear", "[DC:{}] {}", leader->GetName(), warning);
                DcStatusPublisher::SendAddonMessage(leaderAI, "CHAT\t" + warning);
            }
            DcActionShared::SetPhase(context, "Patchwerk waiting for tank supplies: " + missing);
            DcRunProgress::Stamp(run.progress, getMSTime());
            return PreparationResult::Waiting;
        }
        previous.clear();
        if (campReady && (!atShot || shotReady) && missing.empty())
            return PreparationResult::Ready;
        if (override)
            return PreparationResult::Overridden;

        DcActionShared::SetPhase(context, atShot && campReady ? "Patchwerk tanks applying Greater Stoneshield"
                                                             : "Patchwerk tanks preparing flask and food");
        DcRunProgress::Stamp(run.progress, getMSTime());
        return PreparationResult::Waiting;
    }

    DcPullContext const* ReadPatrolPull(Player* bot, Player*& tank)
    {
        if (!bot || !IsNaxx40Map(bot->FindMap()))
            return nullptr;
        tank = DcLeaderSignal::FindLeaderTank(bot);
        if (!tank || !tank->IsAlive())
            return nullptr;
        PlayerbotAI* const tankAI = GET_PLAYERBOT_AI(tank);
        if (!tankAI)
            return nullptr;
        AiObjectContext* const context = tankAI->GetAiObjectContext();
        DcPullContext const& pull = context->GetValue<DcPullContext&>(DcKey::PullContext)->Get();
        return DcRun::Of(context).enabled && IsPatrolPull(pull) ? &pull : nullptr;
    }

    bool HoldRaidAfterArrival(Player* bot, Player* tank, DcPullContext const& pull)
    {
        // Give the assist tanks time to attack in melee before DPS close in.
        if (pull.phase != DcPullPhase::Engage || getMSTimeDiff(pull.phaseSince, getMSTime()) >= AssistHeadStartMs)
            return false;
        Creature const* const boss = tank->GetMap()->GetCreature(pull.pullTarget);
        return boss && boss->IsAlive() && boss->IsInCombat() && PatrolBossAtCamp(*boss) && !AssistSlot(bot);
    }

    constexpr ObjectGuid::LowType EastGolem = 361150;
    constexpr ObjectGuid::LowType WestGolem = 361151;
    constexpr ObjectGuid::LowType GolemPackLeader = 361155;
    constexpr Checkpoint ApproachAnchor{3125.0f, -3246.0f, 0.0f, false, 294.096f};
    constexpr Checkpoint LowerRoomAnchor{3104.0f, -3305.0f, 0.0f, false, 293.7f};
    constexpr float ApproachArrival = 4.0f;
    Position const SouthWestCamp(3017.3f, -3214.8f, 294.0628f);
    Position const NorthWestCamp(3016.0f, -3194.0f, 294.0628f);
    Position const NorthCentreCamp(3051.21f, -3148.60f, 294.0628f);
    Position const NorthRoomCamp(3074.79f, -3119.06f, 294.0628f);

    // Follow the white line on the dry rim. Reach the two far-west packs
    // and the north room from short detours before waiting at the white camp.
    constexpr std::array<Checkpoint, 25> WhiteRoute = {{
        {3122.35f, -3224.93f, 29.0f, false, Floor, 0, 361323},
        {3094.5f, -3219.9f},
        {3088.43f, -3219.82f},
        {3066.52f, -3224.18f, 0.0f, true},
        {3052.70f, -3227.90f},
        {3038.50f, -3231.40f, 29.0f, true, 294.063f, 0, 0, &SouthWestCamp},
        {3022.0f, -3224.2f, 34.0f, false, Floor, 361909, 0, &SouthWestCamp},
        {3015.5f, -3221.7f, 34.0f, false, Floor, 361909, 0, &SouthWestCamp},
        {3012.0f, -3206.0f, 38.0f, false, Floor, 361918, 0, &SouthWestCamp},
        {3010.0f, -3200.5f, 38.0f, false, Floor, 361918, 0, &SouthWestCamp},
        {3012.0f, -3206.0f, 35.0f, false, Floor, 0, 0, &SouthWestCamp},
        {3012.0f, -3188.0f, 24.0f, false, Floor, 0, 0, &NorthWestCamp},
        {3022.0f, -3188.0f, 35.0f, false, 294.0628f, 0, 361325, &NorthWestCamp},
        {3038.0f, -3175.0f, 48.0f, false, 294.0628f, 361923, 0, &NorthWestCamp},
        {3029.0f, -3143.0f, 22.0f, false, Floor, 361923, 0, &NorthCentreCamp},
        {3051.21f, -3148.60f},
        {3055.69f, -3138.59f, 48.0f, false, 294.0667f, 361932, 0, &NorthCentreCamp},
        {3058.55f, -3132.20f, 35.0f, false, Floor, 361932, 0, &NorthCentreCamp},
        {3065.05f, -3129.7f, 35.0f, false, Floor, 361932, 0, &NorthCentreCamp},
        {3074.79f, -3119.06f, 25.0f},
        {3088.19f, -3112.46f, 48.0f, false, 294.0675f, 361941, 0, &NorthRoomCamp},
        {3095.37f, -3108.93f, 29.0f, false, Floor, 361941, 0, &NorthRoomCamp},
        {3092.4f, -3097.2f, 35.0f, false, Floor, 361941, 0, &NorthRoomCamp},
        {3095.37f, -3108.93f},
        {PullCampX, PullCampY, 0.0f, false, PullCampZ},
    }};
    constexpr std::size_t EastBank = 2;
    constexpr std::size_t WestBank = 3;
    constexpr std::size_t OuterBank = 5;
    constexpr std::size_t WestFirstPull = OuterBank + 1;
    constexpr std::size_t WestGolemStand = 10;
    constexpr std::size_t WestGolemFallback = WestGolemStand + 1;
    constexpr std::size_t WestTrashExit = 12;
    constexpr std::size_t NorthwestFallback = 14;
    constexpr std::size_t RouteSize = WhiteRoute.size();
    constexpr std::size_t CampCheckpoint = RouteSize - 1;

    bool IsWestGolemCheckpoint(std::size_t checkpoint)
    {
        return checkpoint == WestGolemStand || checkpoint == WestGolemFallback;
    }

    bool IsWestGolem(ObjectGuid::LowType spawn)
    {
        return spawn >= 361326 && spawn <= 361329;
    }

    Checkpoint const& RoutePoint(std::size_t index)
    {
        return WhiteRoute[index];
    }

    Creature* FindLiveSpawn(Player& tank, ObjectGuid::LowType spawnId)
    {
        auto const [first, last] = tank.GetMap()->GetCreatureBySpawnIdStore().equal_range(spawnId);
        for (auto it = first; it != last; ++it)
        {
            Creature* const creature = it->second;
            if (creature->IsInWorld() && creature->IsAlive() && tank.InSamePhase(creature))
                return creature;
        }
        return nullptr;
    }

    bool HasGolemPack(Player& tank)
    {
        return FindLiveSpawn(tank, GolemPackLeader) || FindLiveSpawn(tank, EastGolem) ||
            FindLiveSpawn(tank, WestGolem);
    }

    bool NeedsLowerRoomPull(Player& tank)
    {
        return tank.GetPositionY() >= -3330.0f && tank.GetPositionY() <= -3242.0f &&
            std::abs(tank.GetPositionZ() - LowerRoomAnchor.z) <= 2.5f && HasGolemPack(tank);
    }

    bool IsLinkedTrash(Creature const& creature)
    {
        switch (creature.GetEntry())
        {
            case 351021: // Patchwork Golem
            case 351023: // Mad Scientist
            case 351024: // Living Monstrosity
            case 351025: // Surgical Assistant
                return true;
            case 351022: // Bile Retcher
            case 351029: // Sludge Belcher
            {
                CreatureData const* spawn = creature.GetCreatureData();
                return spawn && spawn->posY > -3258.0f;
            }
            default:
                return false;
        }
    }

    bool CanRun(Player* tank, AiObjectContext* context, uint32 entry)
    {
        return tank && context && entry == Naxx40PatchwerkEntry && IsNaxx40Map(tank->FindMap()) &&
               DcRun::Of(context).enabled && DcSettings::GetBool(tank, "ClearRoomBeforeBoss");
    }

    bool AtDryCamp(Player const& tank)
    {
        return tank.GetExactDist(&Camp) <= CampRadius &&
            DcNaxx40SlimeCrossing::IsDryCampGround(tank, {tank.GetPositionX(), tank.GetPositionY(),
                                                        tank.GetPositionZ()});
    }

    bool NearPatrolStop(Creature const& boss)
    {
        return !boss.IsInCombat() && boss.GetExactDist(&PatrolStop) <= 3.0f;
    }

    bool IsDryPath(Player* tank, Movement::PointsArray const& points)
    {
        for (std::size_t i = 1; i < points.size(); ++i)
        {
            G3D::Vector3 const leg = points[i] - points[i - 1];
            int const samples = std::max(1, static_cast<int>(std::ceil(leg.length() / 0.5f)));
            for (int sample = 0; sample <= samples; ++sample)
            {
                G3D::Vector3 const p = points[i - 1] + leg * (static_cast<float>(sample) / samples);
                PositionFullTerrainStatus terrain;
                tank->GetMap()->GetFullTerrainStatusForPosition(tank->GetPhaseMask(), p.x, p.y, p.z,
                                                               tank->GetCollisionHeight(), terrain);
                if (std::fabs(p.z - terrain.floorZ) > 2.0f ||
                    (terrain.liquidInfo.Status != LIQUID_MAP_NO_WATER && terrain.liquidInfo.Level > terrain.floorZ))
                    return false;
            }
        }
        return true;
    }

    Movement::PointsArray BuildSurveyedDryLeg(Player* tank, G3D::Vector3 const& end)
    {
        G3D::Vector3 const start(tank->GetPositionX(), tank->GetPositionY(), tank->GetPositionZ());
        // The navmesh can smooth over a dry lip. Use the surveyed leg only
        // when the actual position still gives us a clear, dry line to it.
        if (!tank->GetMap()->isInLineOfSight(start.x, start.y, start.z + 0.75f, end.x, end.y, end.z + 0.75f,
                                             tank->GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS,
                                             VMAP::ModelIgnoreFlags::Nothing))
            return {};

        Movement::PointsArray points{start, (start + end) * 0.5f, end};
        return IsDryPath(tank, points) ? points : Movement::PointsArray{};
    }

    Movement::PointsArray BuildSweepCampLeg(Player* tank, Position const& camp)
    {
        auto points = DcNaxx40SlimeCrossing::BuildDryCampPath(tank, camp);
        if (points.empty() && tank->GetExactDist(&camp) <= 32.0f)
            points = BuildSurveyedDryLeg(tank, {camp.GetPositionX(), camp.GetPositionY(), camp.GetPositionZ()});
        return points;
    }

    bool MoveDry(PlayerbotAI* ai, Position const& destination,
                 MovementPriority priority = MovementPriority::MOVEMENT_NORMAL)
    {
        Player* const tank = ai->GetBot();
        G3D::Vector3 const target(destination.GetPositionX(), destination.GetPositionY(), destination.GetPositionZ());
        if (DcMovement::OwnsRunningEscort(tank) && (tank->movespline->FinalDestination() - target).length() <= 0.6f)
            return true;
        if (!ai->CanMove() || tank->IsFalling())
            return false;

        PathGenerator path(tank);
        path.SetUseStraightPath(true);
        if (!path.CalculatePath(destination.GetPositionX(), destination.GetPositionY(), destination.GetPositionZ()) ||
            path.GetPathType() != PATHFIND_NORMAL || path.GetPath().size() < 2 ||
            path.getPathLength() > 18.0f)
            return false;
        Movement::PointsArray points = path.GetPath();
        if ((points.back() - target).length() > 0.6f)
            return false;
        points.front() = G3D::Vector3(tank->GetPositionX(), tank->GetPositionY(), tank->GetPositionZ());
        if (!IsDryPath(tank, points))
            return false;
        if (points.size() == 2)
            points.insert(points.begin() + 1, (points.front() + points.back()) * 0.5f);
        ForcedMovement const forced = priority == MovementPriority::MOVEMENT_COMBAT
            ? FORCED_MOVEMENT_RUN : FORCED_MOVEMENT_NONE;
        return DcMovement::SplinePath(ai, points, priority, forced);
    }

    bool IsAtEntrance(Player const& tank)
    {
        return std::fabs(tank.GetPositionZ() - Floor) <= 4.0f &&
               (tank.GetExactDist2d(RoutePoint(EastBank).x, RoutePoint(EastBank).y) <= 23.0f ||
                tank.GetExactDist2d(RoutePoint(WestBank).x, RoutePoint(WestBank).y) <= 20.0f ||
                (tank.GetPositionY() >= -3240.0f && tank.GetPositionY() <= -3214.0f &&
                 tank.GetPositionX() >= 3103.0f && tank.GetPositionX() <= 3134.0f));
    }

    bool AtApproachAnchor(Player const& tank)
    {
    return tank.GetExactDist(ApproachAnchor.x, ApproachAnchor.y, ApproachAnchor.z) < ApproachArrival &&
        std::abs(tank.GetPositionZ() - ApproachAnchor.z) < 1.5f;
    }

    bool AtSweepEntrance(Player const& tank)
    {
        return IsAtEntrance(tank) || AtApproachAnchor(tank);
    }

    bool IsNorthOfSlime(Player const& tank)
    {
        return std::abs(tank.GetPositionZ() - Floor) < 1.5f &&
            tank.GetPositionX() >= 3075.0f && tank.GetPositionX() <= 3113.0f &&
            tank.GetPositionY() >= -3119.0f && tank.GetPositionY() <= -3080.0f;
    }

    bool WestRecoveryArea(Player const& tank)
    {
        float const x = tank.GetPositionX();
        float const y = tank.GetPositionY();
        if (std::abs(tank.GetPositionZ() - Floor) >= 1.5f)
            return false;
        // The southern arm starts beyond the landing of the second crossing.
        float const eastEdge = y <= -3205.0f ? 3041.0f : 3062.0f;
        bool const west = x >= 2977.0f && x <= eastEdge && y >= -3250.0f && y <= -3100.0f;
        bool const northJoin = x > 3062.0f && x <= 3075.0f && y >= -3140.0f && y <= -3100.0f;
        bool const north = x >= 3075.0f && x <= 3115.0f && y >= -3122.0f && y <= -3080.0f;
        return (west || northJoin || north) &&
            DcNaxx40SlimeCrossing::IsDryCampGround(tank, {x, y, tank.GetPositionZ()});
    }

    bool InsideWestApproach(Player const& tank)
    {
        return std::abs(tank.GetPositionZ() - Floor) < 2.0f &&
            tank.GetPositionX() >= 3080.0f && tank.GetPositionX() <= 3250.0f &&
            tank.GetPositionY() >= -3195.0f && tank.GetPositionY() <= -3075.0f;
    }

    // All the linked spawns are in these two grids. On a fresh handoff a
    // missing creature in the map's live store must not mean "already dead"
    // simply because its grid was never visited.
    void LoadLinkedTrashGrids(Player& tank)
    {
        Map* const map = tank.GetMap();
        for (float const y : {-3385.0f, PullCampY})
            if (!map->IsGridLoaded(PullCampX, y))
                map->LoadGrid(PullCampX, y);
    }

    std::size_t WhiteRouteEntry(Player const& tank)
    {
        if (tank.GetPositionX() < 3073.0f)
            return WestBank + 1;
        return tank.GetPositionX() < 3103.0f ? EastBank : 0;
    }

    bool ClearsPatrol(Player& tank, Creature* boss, Movement::PointsArray const& points)
    {
        if (!boss || boss->IsInCombat())
            return true;
        float const radius = DcEngageGeometry::RoomAggroSphereRadius(&tank, boss);
        for (std::size_t i = 1; i < points.size(); ++i)
            if (DungeonClearMath::DistSqToSegment2D(boss->GetPositionX(), boss->GetPositionY(),
                    points[i - 1].x, points[i - 1].y, points[i].x, points[i].y) < radius * radius)
                return false;
        return true;
    }

    bool UsesWestBackbone(std::size_t index)
    {
        Checkpoint const& point = RoutePoint(index);
        return index != WestGolemFallback && index != NorthwestFallback &&
            !(point.formation && RoutePoint(index - 1).formation == point.formation);
    }

    std::optional<std::size_t> FindWestRejoin(Player& tank, Creature* boss)
    {
        std::optional<std::size_t> nearest;
        float bestDistance = 40.0f;
        for (std::size_t i = WestFirstPull; i <= CampCheckpoint; ++i)
        {
            if (!UsesWestBackbone(i))
                continue;
            Checkpoint const& point = RoutePoint(i);
            float const distance = tank.GetExactDist(point.x, point.y, point.z);
            if (distance >= bestDistance)
                continue;
            if (distance > Arrive)
            {
                auto const path = DcNaxx40SlimeCrossing::BuildDryCampPath(&tank, {point.x, point.y, point.z});
                if (path.empty() || !ClearsPatrol(tank, boss, path))
                    continue;
            }
            nearest = i;
            bestDistance = distance;
        }
        return nearest;
    }

    SweepResult MoveToWhiteCheckpoint(PlayerbotAI* ai, std::size_t checkpoint, Checkpoint const& point,
                                      Creature* avoidBoss = nullptr, bool dryOnly = false)
    {
        Player* const tank = ai->GetBot();
        G3D::Vector3 destination(point.x, point.y, point.z);
        if (tank->GetExactDist(point.x, point.y, point.z) <= Arrive)
            return SweepResult::Waiting;

        if (DcMovement::OwnsRunningEscort(tank))
            return SweepResult::Moving;
        if (!dryOnly && point.crossing && DcNaxx40SlimeCrossing::TryCross(ai, {destination}))
            return SweepResult::Moving;

        Movement::PointsArray points;
        if (point.crossing)
        {
            Checkpoint const& bank = RoutePoint(checkpoint - 1);
            float const distanceToBank = tank->GetExactDist(bank.x, bank.y, bank.z);
            if (distanceToBank <= Arrive)
                return SweepResult::Blocked;
            // Combat can carry the tank away from the landing before Advance records arrival.
            if (tank->GetExactDist(point.x, point.y, point.z) < distanceToBank)
                points = DcNaxx40SlimeCrossing::BuildDryCampPath(tank, Position(point.x, point.y, point.z));
            destination = G3D::Vector3(bank.x, bank.y, bank.z);
        }
        if (dryOnly)
            points = BuildSweepCampLeg(tank, {point.x, point.y, point.z});
        else if (points.empty())
        {
            PathGenerator path(tank);
            if (path.CalculatePath(destination.x, destination.y, destination.z) &&
                path.GetPathType() == PATHFIND_NORMAL && path.GetPath().size() >= 2 &&
                (path.GetActualEndPosition() - destination).length() <= Arrive)
            {
                points = path.GetPath();
                points.front() = G3D::Vector3(tank->GetPositionX(), tank->GetPositionY(), tank->GetPositionZ());
                if (!IsDryPath(tank, points))
                    points.clear();
            }
            bool const surveyed = !point.crossing;
            if (points.empty() && surveyed && tank->GetExactDist(destination.x, destination.y, destination.z) <= 30.0f)
                points = BuildSurveyedDryLeg(tank, destination);
        }
        if (points.empty() || !ClearsPatrol(*tank, avoidBoss, points))
            return SweepResult::Blocked;

        if (points.size() == 2)
            points.insert(points.begin() + 1, (points.front() + points.back()) * 0.5f);
        return DcMovement::SplinePath(ai, points, MovementPriority::MOVEMENT_NORMAL)
            ? SweepResult::Moving : SweepResult::Blocked;
    }

    bool IsLiveEmbalmingSlime(Player const& tank, Creature const& creature)
    {
        return creature.IsInWorld() && creature.IsAlive() && tank.InSamePhase(&creature) &&
            DcNaxx40Embalming::IsSlime(&creature);
    }

    bool HasLiveEmbalmingSlimes(Player const& tank)
    {
        for (auto const& [id, creature] : tank.GetMap()->GetCreatureBySpawnIdStore())
            if (IsLiveEmbalmingSlime(tank, *creature))
                return true;
        return false;
    }

    std::vector<ObjectGuid> LinkedTrash(Player& tank, Checkpoint const* point = nullptr, bool golems = false)
    {
        std::vector<ObjectGuid> trash;
        for (auto const& spawn : tank.GetMap()->GetCreatureBySpawnIdStore())
        {
            Creature* const creature = spawn.second;
            if (!creature->IsInWorld() || !creature->IsAlive() || !tank.InSamePhase(creature) ||
                !IsLinkedTrash(*creature))
                continue;
            if (point && point->formation &&
                (!creature->GetFormation() || creature->GetFormation()->GetId() != point->formation))
                continue;
            if (point && point->patrol && creature->GetSpawnId() != point->patrol)
                continue;
            if (golems && !IsWestGolem(creature->GetSpawnId()))
                continue;
            if (point && point->crossing &&
                creature->GetSpawnId() != 361324 && creature->GetSpawnId() != 361325)
                continue;
            if (point && (creature->GetExactDist2d(point->x, point->y) > point->radius ||
                          tank.GetExactDist2d(creature) > LocalPullRange ||
                          (!point->camp && (point->patrol || point->crossing) &&
                           !tank.IsWithinLOSInMap(creature)) ||
                          !AttackersValue::IsPossibleTarget(creature, &tank) ||
                          (!point->camp && !DcEngageGeometry::IsEngageReachable(&tank, creature, true))))
                continue;
            trash.push_back(creature->GetGUID());
        }
        return trash;
    }

    bool HasLiveFormation(Player& tank, ObjectGuid::LowType formation)
    {
        for (auto const& [id, creature] : tank.GetMap()->GetCreatureBySpawnIdStore())
            if (creature->IsInWorld() && creature->IsAlive() && tank.InSamePhase(creature) &&
                creature->GetFormation() && creature->GetFormation()->GetId() == formation)
                return true;
        return false;
    }

    SweepResult HoldRoute(PlayerbotAI* ai, std::string const& reason)
    {
        std::string const message = "Patchwerk: " + reason;
        AiObjectContext* const context = ai->GetAiObjectContext();
        if (context->GetValue<std::string&>(DcKey::StallReason)->Get() != message)
            LOG_INFO("playerbots.dungeonclear", "[DC:{}] {} at ({:.1f},{:.1f},{:.1f})",
                ai->GetBot()->GetName(), message, ai->GetBot()->GetPositionX(),
                ai->GetBot()->GetPositionY(), ai->GetBot()->GetPositionZ());
        DcActionShared::StallDungeonClear(ai, message);
        return SweepResult::Blocked;
    }

    std::vector<ObjectGuid> ReachableSurvivors(Player& tank, AiObjectContext* context)
    {
        Creature* const boss = DcTargeting::GetLiveBoss(&tank, context, Naxx40PatchwerkEntry);
        float const safe = boss ? DcEngageGeometry::RoomAggroSphereRadius(&tank, boss) : 0.0f;
        std::vector<ObjectGuid> out;
        for (ObjectGuid const& guid : LinkedTrash(tank))
        {
            Creature* const trash = tank.GetMap()->GetCreature(guid);
            if (trash && tank.GetExactDist(trash) <= FinalPullRange &&
                !(trash->GetPositionX() >= 3115.0f && trash->GetPositionY() < -3119.0f) &&
                (!boss || boss->GetExactDist2d(trash) > safe) && AttackersValue::IsPossibleTarget(trash, &tank) &&
                !DcEngageGeometry::ClosedDoorBetween(&tank, trash->GetPositionX(), trash->GetPositionY(),
                                                    trash->GetPositionZ()) &&
                DcEngageGeometry::IsEngageReachable(&tank, trash, true))
                out.push_back(guid);
        }
        return out;
    }
}

bool BlocksBossPull(Player* tank, AiObjectContext* context, uint32 bossEntry)
{
    return CanRun(tank, context, bossEntry) && !AtPatrolCamp(tank, context);
}

bool UsesPatrolCamp(Player* tank, AiObjectContext* context, uint32 bossEntry)
{
    return CanRun(tank, context, bossEntry);
}

bool SetApproachAnchor(Player* tank, AiObjectContext* context, DungeonBossInfo& next)
{
    if (!CanRun(tank, context, next.entry))
        return false;
    auto const& anchor = !SweepInProgress(tank, context, next.entry) && NeedsLowerRoomPull(*tank)
        ? LowerRoomAnchor : ApproachAnchor;
    next.x = anchor.x;
    next.y = anchor.y;
    next.z = anchor.z;
    return true;
}

bool AtEmbalmingHandoff(Player* tank, AiObjectContext* context, uint32 bossEntry)
{
    return CanRun(tank, context, bossEntry) && AtApproachAnchor(*tank) &&
        !SweepInProgress(tank, context, bossEntry) && !NeedsLowerRoomPull(*tank) &&
        HasLiveEmbalmingSlimes(*tank);
}

bool DeferEmbalmingUntilLowerEast(Player* tank, AiObjectContext* context, uint32 bossEntry, Unit const* target)
{
    return CanRun(tank, context, bossEntry) && tank->GetPositionY() <= -3242.0f &&
        !SweepInProgress(tank, context, bossEntry) && DcNaxx40Embalming::IsSlime(target) && HasGolemPack(*tank);
}

Creature* FindEmbalmingPull(PlayerbotAI* ai, ObjectGuid excluded)
{
    Player* const tank = ai->GetBot();
    auto const opener = DcActionShared::ResolvePullSpell(ai, tank);
    if (!opener)
        return nullptr;

    float nearestDistance = opener->maxRange;
    Creature* nearest = nullptr;
    for (auto const& [id, creature] : tank->GetMap()->GetCreatureBySpawnIdStore())
    {
        if (creature->GetGUID() == excluded || !IsLiveEmbalmingSlime(*tank, *creature))
            continue;
        float const range = tank->GetExactDist(creature);
        if (range >= opener->minRange && range <= nearestDistance &&
            AttackersValue::IsPossibleTarget(creature, tank) && tank->IsWithinLOSInMap(creature))
        {
            nearest = creature;
            nearestDistance = range;
        }
    }
    return nearest;
}

std::optional<Position> SweepPullCamp(Player* tank, AiObjectContext* context, uint32 bossEntry)
{
    if (!SweepInProgress(tank, context, bossEntry))
        return std::nullopt;
    auto const& sweep = DcRun::Of(context).patchwerkSweep;
    if (!sweep.reachedCheckpoint || sweep.returningToWestStart || sweep.checkpoint >= CampCheckpoint)
        return std::nullopt;
    Checkpoint const& point = RoutePoint(sweep.checkpoint);
    if (!point.camp || tank->GetExactDist(point.x, point.y, point.z) > Arrive)
        return std::nullopt;
    return *point.camp;
}

bool ValidateSweepCamp(Player* tank, AiObjectContext* context, Position const& camp)
{
    auto const path = BuildSweepCampLeg(tank, camp);
    Creature* const boss = DcTargeting::GetLiveBoss(tank, context, Naxx40PatchwerkEntry);
    return !path.empty() && ClearsPatrol(*tank, boss, path);
}

bool DriveSweepReturn(PlayerbotAI* ai, Position const& camp)
{
    Player* const tank = ai->GetBot();
    if (DcMovement::OwnsRunningEscort(tank))
        return true;
    auto path = BuildSweepCampLeg(tank, camp);
    return !path.empty() && DcMovement::SplinePath(ai, path, MovementPriority::MOVEMENT_COMBAT, FORCED_MOVEMENT_RUN);
}

void HoldForEmbalming(PlayerbotAI* ai)
{
    Player* const tank = ai->GetBot();
    AiObjectContext* const context = ai->GetAiObjectContext();
    DcMovement::StopBot(tank, DcMovement::Stop::Hold);
    DcActionShared::SetPhase(context, "waiting for the Embalming Slimes pull");
    if (!DcTargeting::GetPullTarget(ai))
        HoldRoute(ai, "Embalming Slimes remain, but no usable ranged shot from the dry approach.");
    else
        DcActionShared::ClearStall(context);
}

void NoteClearedLowerEast(Player* tank, AiObjectContext* context, uint32 bossEntry)
{
    if (!CanRun(tank, context, bossEntry) || HasGolemPack(*tank))
        return;
    auto& sweep = DcRun::Of(context).patchwerkSweep;
    if (sweep.reportedEastClear)
        return;
    sweep.reportedEastClear = true;
    LOG_INFO("playerbots.dungeonclear", "[DC:{}] Patchwerk: formation 361155 cleared before Embalming Slimes",
        tank->GetName());
}

bool AtPatrolCamp(Player* tank, AiObjectContext* context)
{
    return SweepInProgress(tank, context, Naxx40PatchwerkEntry) &&
        DcRun::Of(context).patchwerkSweep.checkpoint == RouteSize && AtDryCamp(*tank) && LinkedTrash(*tank).empty();
}

bool IsPatrolPull(DcPullContext const& pull)
{
    return pull.bossPullback && pull.pullTarget.GetEntry() == Naxx40PatchwerkEntry;
}

bool IsPatrolManeuverActive(Player* bot)
{
    Player* tank = nullptr;
    DcPullContext const* const pull = ReadPatrolPull(bot, tank);
    return pull && DcLeaderSignal::IsPullPhaseHolding(static_cast<uint32>(pull->phase));
}

bool HasPatrolPosition(Player* bot)
{
    if (!IsHeadlessFollower(bot))
        return false;
    Player* tank = nullptr;
    DcPullContext const* const pull = ReadPatrolPull(bot, tank);
    return pull && IsManagedFollower(bot, tank) &&
           (DcLeaderSignal::IsPullPhaseHolding(static_cast<uint32>(pull->phase)) ||
            HoldRaidAfterArrival(bot, tank, *pull));
}

std::optional<Position> PatrolPosition(Player* bot)
{
    if (!IsHeadlessFollower(bot))
        return std::nullopt;
    Player* tank = nullptr;
    DcPullContext const* const pull = ReadPatrolPull(bot, tank);
    if (!pull || !IsManagedFollower(bot, tank))
        return std::nullopt;
    if (!DcLeaderSignal::IsPullPhaseHolding(static_cast<uint32>(pull->phase)))
        return HoldRaidAfterArrival(bot, tank, *pull) ? std::optional<Position>(RaidCamp) : std::nullopt;
    if (std::optional<std::size_t> const slot = AssistSlot(bot))
    {
        Creature const* const boss = tank->GetMap()->GetCreature(pull->pullTarget);
        return pull->phase == DcPullPhase::Returning && boss && boss->GetExactDist(&Camp) <= AssistAdvanceRadius
            ? AssistIntercept[*slot] : AssistWait[*slot];
    }
    return RaidCamp;
}

bool PatrolFormationReady(Player* tank)
{
    if (Group* const group = tank->GetGroup())
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* const member = ref->GetSource();
            if (!IsManagedFollower(member, tank))
                continue;
            std::optional<std::size_t> const slot = AssistSlot(member);
            Position const& position = slot ? AssistWait[*slot] : RaidCamp;
            if (member->GetExactDist(&position) > FormationPositionRadius)
                return false;
        }
    return true;
}

bool PatrolBossAtCamp(Unit const& boss)
{
    return boss.GetExactDist(&Camp) <= BossArrivalRadius;
}

bool DrivePatrolReturn(PlayerbotAI* ai)
{
    return MoveDry(ai, Camp, MovementPriority::MOVEMENT_COMBAT);
}

bool BeginPatrolPull(PlayerbotAI* ai, AiObjectContext* context, Creature* boss)
{
    Player* const tank = ai->GetBot();
    if (!AtPatrolCamp(tank, context) || !boss || !boss->IsAlive() || boss->IsInCombat())
        return false;
    if (PreparePatrolTanks(ai, context, false) == PreparationResult::Waiting || !NearPatrolStop(*boss))
        return false;

    if (!DcActionShared::ResolvePullSpell(ai, tank))
    {
        DcActionShared::StallDungeonClear(ai, "Patchwerk: the tank needs a usable ranged pull.");
        return false;
    }

    DcPullContext& pull = context->GetValue<DcPullContext&>(DcKey::PullContext)->Get();
    uint32 const now = getMSTime();
    pull.PublishCamp(Camp, now);
    pull.bossPullback = true;
    pull.pullTarget = boss->GetGUID();
    pull.Transition(DcPullPhase::Forming, now);
    DcMovement::StopBot(tank, DcMovement::Stop::Hold);
    return true;
}

bool DrivePatrolTag(PlayerbotAI* ai, AiObjectContext* context)
{
    Player* const tank = ai->GetBot();
    DcPullContext& pull = context->GetValue<DcPullContext&>(DcKey::PullContext)->Get();
    Creature* const boss = tank->GetMap()->GetCreature(pull.pullTarget);
    auto const opener = DcActionShared::ResolvePullSpell(ai, tank);
    uint32 const now = getMSTime();
    if (!boss || !boss->IsAlive() || !opener || !NearPatrolStop(*boss) || !LinkedTrash(*tank).empty() ||
        getMSTimeDiff(pull.phaseSince, now) >= 12000u)
    {
        if (pull.tagTarget.IsEmpty())
            for (Player* const member : PatrolTanks(tank))
                if (PlayerbotAI* const tankAI = TankPreparationAI(member))
                    SetPatchwerk40TankPreparation(*tankAI, Patchwerk40TankPreparation::Idle);
        if (tank->GetExactDist(&Camp) > Arrive)
        {
            if (!MoveDry(ai, Camp))
                DcActionShared::StallDungeonClear(ai, "Patchwerk: waiting for a clear path back to camp.");
            return true;
        }
        DcMovement::StopBot(tank, DcMovement::Stop::Hold);
        pull.bossPullback = false;
        pull.pullTarget = ObjectGuid::Empty;
        pull.tagTarget = ObjectGuid::Empty;
        pull.Transition(DcPullPhase::Idle, now);
        return true;
    }

    if (tank->GetExactDist(&ShotBank) > 0.75f)
    {
        if (!MoveDry(ai, ShotBank))
            DcActionShared::StallDungeonClear(ai, "Patchwerk: waiting for the dry firing position.");
        return true;
    }

    DcMovement::StopBot(tank, DcMovement::Stop::Hold);
    float const distance = tank->GetExactDist(boss);
    if (pull.tagTarget.IsEmpty() && PatrolFormationReady(tank) &&
        distance >= opener->minRange && distance <= opener->maxRange &&
        tank->IsWithinLOSInMap(boss))
    {
        PreparationResult const preparation = PreparePatrolTanks(ai, context, true);
        if (preparation == PreparationResult::Waiting)
            return true;
        tank->SetSelection(boss->GetGUID());
        DcActionShared::DcFaceIfNeeded(tank, boss);
        if (ai->CastSpell(opener->spellId, boss))
        {
            if (preparation == PreparationResult::Overridden)
                if (Group* const group = tank->GetGroup())
                {
                    group->SetTargetIcon(PreparationOverrideIcon, tank->GetGUID(), ObjectGuid::Empty);
                    LOG_INFO("playerbots.dungeonclear", "[DC:{}] Patchwerk: Moon override on tank preparation",
                             tank->GetName());
                }
            if (preparation == PreparationResult::Ready)
                LOG_INFO("playerbots.dungeonclear", "[DC:{}] Patchwerk: tank supplies confirmed", tank->GetName());
            pull.tagTarget = boss->GetGUID();
            LOG_INFO("playerbots.dungeonclear", "[DC:{}] Patchwerk: formation ready, shot from the dry bank at {:.1f}yd",
                     tank->GetName(), distance);
        }
    }
    return true;
}

G3D::Vector3 const& WhiteCampPosition()
{
    static G3D::Vector3 const position(PullCampX, PullCampY, PullCampZ);
    return position;
}

G3D::Vector3 const& WhiteCampFiringBank()
{
    static G3D::Vector3 const position(ShotBankX, ShotBankY, ShotBankZ);
    return position;
}

bool CanJoinGrobbulusCampRoute(Player const& tank)
{
    return IsNorthOfSlime(tank) || IsAtEntrance(tank);
}

SweepResult DriveGrobbulusCamp(PlayerbotAI* ai)
{
    Player* const tank = ai->GetBot();
    DcRunState& run = DcRun::Of(ai);
    auto& route = run.naxx40GrobbulusCamp;
    uint32 const instanceId = tank->GetInstanceId();
    constexpr std::size_t GrobbulusLastLeg = 17;
    if (route.instanceId != instanceId)
    {
        if (!CanJoinGrobbulusCampRoute(*tank))
            return SweepResult::Inactive;
        route = {instanceId, IsNorthOfSlime(*tank) ? GrobbulusLastLeg : WhiteRouteEntry(*tank)};
        DcMovement::StopBot(tank, DcMovement::Stop::Hold);
        LOG_INFO("playerbots.dungeonclear", "[DC:{}] Grobbulus: taking the white route via the Patchwerk camp",
                 tank->GetName());
    }

    if (route.reachedCamp)
        return SweepResult::Waiting;

    // Keep the previously tested post-kill route independent of pull positions.
    constexpr std::array<Checkpoint, 15> GrobbulusApproach = {{
        WhiteRoute[0], WhiteRoute[1], WhiteRoute[2], WhiteRoute[3], WhiteRoute[4], WhiteRoute[5],
        {3022.0f, -3224.2f}, {3012.0f, -3206.0f}, {3030.07f, -3211.44f},
        {3030.34f, -3190.17f}, {3039.75f, -3168.46f}, {3051.21f, -3148.60f},
        {3058.55f, -3132.20f}, {3074.79f, -3119.06f}, {3084.0f, -3100.0f},
    }};
    constexpr std::array<Checkpoint, 3> GrobbulusCampLeg = {{
        {3092.4f, -3097.2f}, {3095.37f, -3108.93f}, {PullCampX, PullCampY, 0.0f, false, PullCampZ},
    }};
    std::size_t const leg = route.checkpoint;
    Checkpoint const& point = leg < GrobbulusApproach.size() ? GrobbulusApproach[leg] :
        GrobbulusCampLeg[leg - GrobbulusApproach.size()];
    SweepResult const result = MoveToWhiteCheckpoint(ai, route.checkpoint, point);
    if (result != SweepResult::Waiting)
        return result;
    if (leg + 1 < GrobbulusApproach.size() + GrobbulusCampLeg.size())
    {
        ++route.checkpoint;
        return SweepResult::Moving;
    }

    route.reachedCamp = true;
    DcMovement::StopBot(tank, DcMovement::Stop::Hold);
    return SweepResult::Waiting;
}

bool ShouldStart(Player* tank, AiObjectContext* context, uint32 bossEntry)
{
    return CanRun(tank, context, bossEntry) &&
        (InsideWestApproach(*tank) || WestRecoveryArea(*tank) ||
         (AtSweepEntrance(*tank) && !HasLiveEmbalmingSlimes(*tank)));
}

bool SweepInProgress(Player* tank, AiObjectContext* context, uint32 bossEntry)
{
    if (!CanRun(tank, context, bossEntry))
        return false;
    DcRunState::PatchwerkSweep const& sweep = DcRun::Of(context).patchwerkSweep;
    return sweep.instanceId == tank->GetInstanceId();
}

std::vector<ObjectGuid> LocalTrash(Player* tank, AiObjectContext* context)
{
    if (!CanRun(tank, context, Naxx40PatchwerkEntry) || DcRun::Of(context).paused)
        return {};

    DcRunState::PatchwerkSweep const& sweep = DcRun::Of(context).patchwerkSweep;
    if (sweep.instanceId != tank->GetInstanceId() || sweep.returningToWestStart)
        return {};
    if (sweep.checkpoint >= CampCheckpoint)
        return AtDryCamp(*tank) ? ReachableSurvivors(*tank, context) : std::vector<ObjectGuid>{};
    if (!sweep.reachedCheckpoint)
        return {};

    Checkpoint const& point = RoutePoint(sweep.checkpoint);
    if (tank->GetExactDist(point.x, point.y, point.z) > Arrive)
        return {};
    if (!point.radius)
        return {};
    auto trash = LinkedTrash(*tank, &point, IsWestGolemCheckpoint(sweep.checkpoint));
    if (point.camp)
    {
        PlayerbotAI* const ai = GET_PLAYERBOT_AI(tank);
        auto const opener = DcActionShared::ResolvePullSpell(ai, tank);
        ObjectGuid const abort = context->GetValue<DcPullContext&>(DcKey::PullContext)->Get().abortTarget;
        std::erase_if(trash, [&](ObjectGuid const& guid)
        {
            Creature const* const mob = tank->GetMap()->GetCreature(guid);
            if (!mob || !opener || guid == abort)
                return true;
            float const range = tank->GetExactDist(mob);
            return range < opener->minRange || range > opener->maxRange || !tank->IsWithinLOSInMap(mob);
        });
    }
    return trash;
}

SweepResult Drive(PlayerbotAI* ai, AiObjectContext* context, uint32 bossEntry)
{
    Player* const tank = ai->GetBot();
    if (!CanRun(tank, context, bossEntry))
        return SweepResult::Inactive;

    bool const west = WestRecoveryArea(*tank);
    if (!west && InsideWestApproach(*tank))
        return HoldRoute(ai, "return the raid to the dry west route before continuing.");

    Creature* const boss = DcTargeting::GetLiveBoss(tank, context, Naxx40PatchwerkEntry);
    DcRunState::PatchwerkSweep& sweep = DcRun::Of(context).patchwerkSweep;
    if (sweep.instanceId != tank->GetInstanceId())
    {
        if (!west && (!AtSweepEntrance(*tank) || HasLiveEmbalmingSlimes(*tank)))
            return SweepResult::Inactive;

        LoadLinkedTrashGrids(*tank);
        std::size_t start = 0;
        if (west)
        {
            if (LinkedTrash(*tank).empty() && AtDryCamp(*tank))
                start = CampCheckpoint;
            else
            {
                auto const rejoin = FindWestRejoin(*tank, boss);
                if (!rejoin)
                    return HoldRoute(ai, "no clear dry path to a west route checkpoint.");
                start = *rejoin;
            }
        }
        else
        {
            if (HasGolemPack(*tank))
                return HoldRoute(ai, "lower east formation 361155 still alive; resume from the lower room.");
            start = WhiteRouteEntry(*tank);
        }

        sweep = {};
        sweep.instanceId = tank->GetInstanceId();
        sweep.checkpoint = start;
        sweep.returningToWestStart = west && start > WestFirstPull && !LinkedTrash(*tank).empty();
        DcMovement::StopBot(tank, DcMovement::Stop::Hold);
        LOG_INFO("playerbots.dungeonclear", "[DC:{}] Patchwerk: {} at checkpoint {} ({:.1f},{:.1f},{:.1f})",
            tank->GetName(), west ? "rejoining the west route" : "starting the west trash sweep",
            start, tank->GetPositionX(), tank->GetPositionY(), tank->GetPositionZ());
    }

    if (sweep.checkpoint >= RouteSize)
    {
        if (AtPatrolCamp(tank, context))
            return SweepResult::Waiting;
        sweep.checkpoint = CampCheckpoint;
        sweep.reachedCheckpoint = false;
    }

    if (sweep.returningToWestStart)
    {
        SweepResult const movement = MoveToWhiteCheckpoint(ai, sweep.checkpoint, RoutePoint(sweep.checkpoint), boss,
                                                            /*dryOnly*/ true);
        if (movement == SweepResult::Blocked)
            return HoldRoute(ai, "waiting for a dry return to checkpoint " + std::to_string(sweep.checkpoint) + ".");
        if (movement == SweepResult::Moving)
            return movement;
        if (sweep.checkpoint == WestFirstPull)
            sweep.returningToWestStart = false;
        else
        {
            do
                --sweep.checkpoint;
            while (!UsesWestBackbone(sweep.checkpoint));
            return SweepResult::Moving;
        }
    }

    // Combat can end far from a checkpoint whose pull stand was already
    // reached. Re-acquire the stand before asking what can be pulled from it.
    Checkpoint const& point = RoutePoint(sweep.checkpoint);
    if (sweep.reachedCheckpoint && tank->GetExactDist(point.x, point.y, point.z) > Arrive)
    {
        sweep.reachedCheckpoint = false;
        context->GetValue<std::vector<ObjectGuid>>(DcKey::RoomTrashRemaining)->Reset();
        context->GetValue<ObjectGuid>(DcKey::PullTarget)->Reset();
    }
    if (sweep.checkpoint == CampCheckpoint && AtDryCamp(*tank))
    {
        LoadLinkedTrashGrids(*tank);
        auto const remaining = LinkedTrash(*tank);
        if (!remaining.empty())
        {
            if (!LocalTrash(tank, context).empty())
                return SweepResult::Clearing;
            if (!sweep.reportedSurvivors)
            {
                sweep.reportedSurvivors = true;
                Creature const* nearest = nullptr;
                for (ObjectGuid const& guid : remaining)
                    if (Creature const* mob = tank->GetMap()->GetCreature(guid);
                        mob && (!nearest || tank->GetExactDist(mob) < tank->GetExactDist(nearest)))
                        nearest = mob;
                if (nearest)
                    LOG_INFO("playerbots.dungeonclear",
                        "[DC:{}] Patchwerk: {} linked trash remain; nearest {} spawn {} at ({:.1f},{:.1f},{:.1f})",
                        tank->GetName(), remaining.size(), nearest->GetName(), nearest->GetSpawnId(),
                        nearest->GetPositionX(), nearest->GetPositionY(), nearest->GetPositionZ());
            }
            return HoldRoute(ai, "linked trash still alive outside the safe pull area of the camp.");
        }
        sweep.checkpoint = RouteSize;
        LOG_INFO("playerbots.dungeonclear", "[DC:{}] Patchwerk: linked trash clear, waiting at the white camp",
                 tank->GetName());
        return SweepResult::Waiting;
    }

    if (!sweep.reachedCheckpoint && tank->GetExactDist(point.x, point.y, point.z) > Arrive)
    {
        SweepResult const movement = MoveToWhiteCheckpoint(ai, sweep.checkpoint, point, boss);
        return movement == SweepResult::Blocked
            ? HoldRoute(ai, "waiting for safe movement to checkpoint " + std::to_string(sweep.checkpoint) + ".")
            : movement;
    }

    if (point.crossing && sweep.checkpoint != sweep.lastCrossingReported)
    {
        sweep.lastCrossingReported = sweep.checkpoint;
        LOG_INFO("playerbots.dungeonclear", "[DC:{}] Patchwerk: arrived at crossing {} ({:.1f},{:.1f},{:.1f})",
            tank->GetName(), sweep.checkpoint == WestBank ? 1 : 2, point.x, point.y, point.z);
    }

    if (point.radius > 0.0f)
    {
        if (!sweep.reachedCheckpoint)
        {
            sweep.reachedCheckpoint = true;
            DcMovement::StopBot(tank, DcMovement::Stop::Hold);
            context->GetValue<std::vector<ObjectGuid>>(DcKey::RoomTrashRemaining)->Reset();
            context->GetValue<ObjectGuid>(DcKey::PullTarget)->Reset();
        }
        auto const& targets = context->GetValue<std::vector<ObjectGuid>>(DcKey::RoomTrashRemaining)->Get();
        if (!targets.empty())
            return SweepResult::Clearing;
        if (point.patrol && FindLiveSpawn(*tank, point.patrol))
            return HoldRoute(ai, "waiting for patrol spawn " + std::to_string(point.patrol) +
                " to reach the dry pull position.");
        else if (IsWestGolemCheckpoint(sweep.checkpoint))
        {
            bool live = false;
            for (ObjectGuid::LowType spawn = 361326; spawn <= 361329; ++spawn)
                if (FindLiveSpawn(*tank, spawn))
                    live = true;
            if (live && sweep.checkpoint == WestGolemFallback)
                return HoldRoute(ai, "western Golems remain outside the firing position.");
            if (!live)
                sweep.checkpoint = WestTrashExit - 1;
        }
        else if (point.formation)
        {
            bool const live = HasLiveFormation(*tank, point.formation);
            bool const hasFallback = RoutePoint(sweep.checkpoint + 1).formation == point.formation;
            if (live && !hasFallback)
                return HoldRoute(ai, "formation " + std::to_string(point.formation) +
                    " still alive outside its firing position.");
            if (!live)
                LOG_INFO("playerbots.dungeonclear", "[DC:{}] Patchwerk: checkpoint {} formation {} clear",
                    tank->GetName(), sweep.checkpoint, point.formation);
            while (!live && RoutePoint(sweep.checkpoint + 1).formation == point.formation)
                ++sweep.checkpoint;
        }
    }

    ++sweep.checkpoint;
    sweep.reachedCheckpoint = false;
    context->GetValue<std::vector<ObjectGuid>>(DcKey::RoomTrashRemaining)->Reset();
    context->GetValue<ObjectGuid>(DcKey::PullTarget)->Reset();
    return SweepResult::Moving;
}
}
