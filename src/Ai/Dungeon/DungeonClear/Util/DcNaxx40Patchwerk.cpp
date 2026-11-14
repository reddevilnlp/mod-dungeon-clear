/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, or later.
 */

#include "DcNaxx40Patchwerk.h"

#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"
#include "Ai/Dungeon/DungeonClear/DcPullContext.h"
#include "Ai/Dungeon/DungeonClear/Action/DcActionShared.h"
#include "Ai/Dungeon/DungeonClear/Settings/DcSettings.h"
#include "Ai/Dungeon/DungeonClear/Data/ScriptedPullRegistry.h"
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
#include "DcPatchwerkRouteOrder.h"
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
        {3094.5f, -3219.9f, 0.0f, false, 294.114f},
        {DcNaxx40SlimeCrossing::PatchwerkFirstBankX, DcNaxx40SlimeCrossing::PatchwerkFirstBankY,
            0.0f, false, DcNaxx40SlimeCrossing::PatchwerkFirstBankZ},
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
    constexpr std::size_t FirstBankApproach = 1;
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

    enum SectionId : std::size_t
    {
        Gallery, LowerRoom, Doorway, Embalming, FirstCrossing, BetweenCrossings, SecondCrossing,
        Southwest, WestWall, WestGolems, Northwest, NorthCentre, NorthRoom, WhiteCamp, SectionCount
    };

    struct Section
    {
        char const* name;
        std::size_t first;
        std::size_t last;
        std::array<ObjectGuid::LowType, 2> formations{};
        ObjectGuid::LowType patrol = 0;
    };

    // Pull geometry in the upper rooms is owned by ScriptedPullRegistry.
    // The white route owns the measured walk, crossing and pull positions below.
    constexpr std::array<Section, SectionCount> Sections{{
        {"gallery", 0, 0, {361146}},
        {"lower-room", 0, 0, {361154, 361155}},
        {"doorway", 0, 0, {361161}},
        {"embalming-slimes", 0, 0},
        {"first-crossing", 0, WestBank, {}, 361323},
        {"between-crossings", WestBank + 1, OuterBank - 1},
        {"second-crossing", OuterBank, OuterBank},
        {"southwest", WestFirstPull, 7, {361909}},
        {"west-wall", 8, 9, {361918}},
        {"west-golems", WestGolemStand, WestGolemFallback},
        {"northwest", WestTrashExit, NorthwestFallback, {361923}, 361325},
        {"north-centre", 15, 18, {361932}},
        {"north-room", 19, CampCheckpoint - 1, {361941}},
        {"white-camp", CampCheckpoint, CampCheckpoint},
    }};

    SectionId SectionFor(std::size_t checkpoint)
    {
        for (std::size_t i = FirstCrossing; i < WhiteCamp; ++i)
            if (checkpoint <= Sections[i].last)
                return static_cast<SectionId>(i);
        return WhiteCamp;
    }

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

    bool HasLowerEastPack(Player& tank)
    {
        return FindLiveSpawn(tank, GolemPackLeader) || FindLiveSpawn(tank, EastGolem) ||
            FindLiveSpawn(tank, WestGolem);
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

    bool BetweenCrossingBanks(Player const& tank)
    {
        return std::abs(tank.GetPositionZ() - Floor) < 1.5f &&
            tank.GetPositionX() >= 3042.0f && tank.GetPositionX() <= 3068.0f &&
            tank.GetPositionY() >= -3233.0f && tank.GetPositionY() <= -3206.0f;
    }

    bool BeyondFirstLanding(Player const& member)
    {
        return BetweenCrossingBanks(member) && member.GetPositionX() <= RoutePoint(WestBank).x;
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

    bool ClearsPatrol(Player& tank, Creature* boss, Movement::PointsArray const& points, bool withdraw = false)
    {
        if (!boss || boss->IsInCombat())
            return true;
        float const radius = DcEngageGeometry::RoomAggroSphereRadius(&tank, boss);
        float const radiusSq = radius * radius;
        for (std::size_t i = 1; i < points.size(); ++i)
        {
            auto const& start = points[i - 1];
            auto const& end = points[i];
            float const closest = DungeonClearMath::DistSqToSegment2D(boss->GetPositionX(), boss->GetPositionY(),
                start.x, start.y, end.x, end.y);
            if (closest >= radiusSq)
                continue;
            if (!withdraw || closest + 0.01f < boss->GetExactDist2dSq(start.x, start.y))
                return false;
        }
        return !withdraw || boss->GetExactDist2dSq(points.back().x, points.back().y) >= radiusSq;
    }

    bool UsesWestBackbone(std::size_t index)
    {
        Checkpoint const& point = RoutePoint(index);
        return index != WestGolemFallback && index != NorthwestFallback &&
            !(point.formation && RoutePoint(index - 1).formation == point.formation);
    }

    bool OnFarBank(Player& tank, std::size_t landing)
    {
        Checkpoint const& bank = RoutePoint(landing - 1);
        Checkpoint const& end = RoutePoint(landing);
        if (WestRecoveryArea(tank) || (landing == WestBank && BeyondFirstLanding(tank)))
            return true;
        if (tank.GetExactDist(end.x, end.y, end.z) <= Arrive)
            return DcNaxx40SlimeCrossing::IsDryCampGround(tank,
                {tank.GetPositionX(), tank.GetPositionY(), tank.GetPositionZ()});
        if (tank.GetExactDist(end.x, end.y, end.z) > 32.0f ||
            tank.GetExactDist(end.x, end.y, end.z) >= tank.GetExactDist(bank.x, bank.y, bank.z))
            return false;
        return !BuildSweepCampLeg(&tank, {end.x, end.y, end.z}).empty();
    }

    bool MemberAtBank(Player& member, std::size_t landing)
    {
        if (WestRecoveryArea(member))
            return true;
        Checkpoint const& end = RoutePoint(landing);
        Checkpoint const& start = RoutePoint(landing - 1);
        bool const beyond = landing == WestBank && BeyondFirstLanding(member);
        bool const nearby = member.GetExactDist(end.x, end.y, end.z) <= 12.0f &&
            member.GetExactDist(end.x, end.y, end.z) < member.GetExactDist(start.x, start.y, start.z);
        return (beyond || nearby) && DcNaxx40SlimeCrossing::IsDryCampGround(member,
            {member.GetPositionX(), member.GetPositionY(), member.GetPositionZ()});
    }

    bool RaidAtBank(Player& tank, std::size_t landing)
    {
        Group* const group = tank.GetGroup();
        if (!group)
            return true;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* const member = ref->GetSource();
            if (!member || member == &tank || !member->IsAlive() || !member->IsInMap(&tank) ||
                !GET_PLAYERBOT_AI(member))
                continue;
            if (!MemberAtBank(*member, landing))
                return false;
        }
        return true;
    }

    SweepResult MoveToWhiteCheckpoint(PlayerbotAI* ai, std::size_t checkpoint, Checkpoint const& point,
                                      Creature* avoidBoss = nullptr, bool dryOnly = false, bool recover = false)
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
        bool const tryDetour = points.empty() && recover && (!point.crossing || dryOnly);
        if (tryDetour)
            points = DcNaxx40SlimeCrossing::BuildPatchwerkRecoveryPath(tank,
                {destination.x, destination.y, destination.z});
        if (points.empty() || !ClearsPatrol(*tank, avoidBoss, points, recover || dryOnly))
            return SweepResult::Blocked;

        if (points.size() == 2)
            points.insert(points.begin() + 1, (points.front() + points.back()) * 0.5f);
        if (!DcMovement::SplinePath(ai, points, MovementPriority::MOVEMENT_NORMAL))
            return SweepResult::Blocked;
        if (tryDetour)
        {
            char const* name = Sections[SectionFor(checkpoint)].name;
            LOG_INFO("playerbots.dungeonclear", "[DC:{}] Patchwerk: route rejoin {} -> {} (dry detour to checkpoint {})",
                tank->GetName(), name, name, checkpoint);
        }
        return SweepResult::Moving;
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

    bool HasWesternGolems(Player& tank)
    {
        for (ObjectGuid::LowType spawn = 361326; spawn <= 361329; ++spawn)
            if (FindLiveSpawn(tank, spawn))
                return true;
        return false;
    }

    bool SectionHasTrash(Player& tank, SectionId section)
    {
        if (section == Embalming)
            return HasLiveEmbalmingSlimes(tank);
        if (section == WestGolems)
            return HasWesternGolems(tank);

        Section const& row = Sections[section];
        if (row.patrol && FindLiveSpawn(tank, row.patrol))
            return true;
        for (ObjectGuid::LowType formation : row.formations)
            if (formation && HasLiveFormation(tank, formation))
                return true;
        return section == LowerRoom && HasLowerEastPack(tank);
    }

    SectionId EntrySection(Player& tank)
    {
        for (std::size_t i = Gallery; i <= Embalming; ++i)
            if (SectionHasTrash(tank, static_cast<SectionId>(i)))
                return static_cast<SectionId>(i);
        return FirstCrossing;
    }

    Checkpoint EntryAnchor(Player& tank, SectionId section)
    {
        int32 order = 0;
        switch (section)
        {
            case LowerRoom:
                if (HasLowerEastPack(tank))
                    return LowerRoomAnchor;
                if (HasLiveFormation(tank, 361154))
                    return {3097.2f, -3292.6f, 0.0f, false, 294.69f};
                break;
            case Doorway:
                order = 3;
                break;
            case Gallery:
                break;
            default:
                return ApproachAnchor;
        }

        if (ScriptedPullStage const* stage = ScriptedPullRegistry::Find(tank.GetMapId(), order))
            return {stage->HasArmAnchor() ? stage->armX : stage->campX,
                    stage->HasArmAnchor() ? stage->armY : stage->campY, 0.0f, false,
                    stage->HasArmAnchor() ? stage->armZ : stage->campZ};
        return ApproachAnchor;
    }

    void ReportSection(Player& tank, DcRunState::PatchwerkSweep& sweep, SectionId section)
    {
        if (sweep.reportedSection == section)
            return;
        sweep.reportedSection = section;
        LOG_INFO("playerbots.dungeonclear", "[DC:{}] Patchwerk: route section {} started",
            tank.GetName(), Sections[section].name);
    }

    void CompleteSection(Player& tank, DcRunState::PatchwerkSweep& sweep, SectionId section)
    {
        uint32 const flag = 1u << section;
        if (sweep.completedSections & flag)
            return;
        ReportSection(tank, sweep, section);
        sweep.completedSections |= flag;
        LOG_INFO("playerbots.dungeonclear", "[DC:{}] Patchwerk: route section {} completed",
            tank.GetName(), Sections[section].name);
    }

    void ResetRouteTargets(AiObjectContext* context)
    {
        context->GetValue<std::vector<ObjectGuid>>(DcKey::RoomTrashRemaining)->Reset();
        context->GetValue<ObjectGuid>(DcKey::PullTarget)->Reset();
    }

    bool RaidNearCamp(Player& tank)
    {
        Group* const group = tank.GetGroup();
        if (!group)
            return true;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* const member = ref->GetSource();
            if (member && member != &tank && member->IsAlive() && member->IsInMap(&tank) &&
                GET_PLAYERBOT_AI(member))
            {
                if (member->GetExactDist(&RaidCamp) > 10.0f ||
                    !DcNaxx40SlimeCrossing::IsDryCampGround(*member,
                        {member->GetPositionX(), member->GetPositionY(), member->GetPositionZ()}))
                    return false;
            }
        }
        return true;
    }

    std::optional<std::size_t> WestRecoveryGoal(Player& tank)
    {
        for (std::size_t i = Southwest; i < WhiteCamp; ++i)
        {
            SectionId const section = static_cast<SectionId>(i);
            if (SectionHasTrash(tank, section))
                return Sections[section].first;
        }
        return std::nullopt;
    }

    std::optional<std::size_t> FindDryWestAnchor(Player& tank, Creature* boss)
    {
        std::optional<std::size_t> nearest;
        float best = 32.0f;
        for (std::size_t i = OuterBank; i <= CampCheckpoint; ++i)
        {
            if (!UsesWestBackbone(i))
                continue;
            Checkpoint const& point = RoutePoint(i);
            float const distance = tank.GetExactDist(point.x, point.y, point.z);
            if (distance >= best)
                continue;
            if (distance > Arrive)
            {
                auto const path = BuildSweepCampLeg(&tank, {point.x, point.y, point.z});
                if (path.empty() || !ClearsPatrol(tank, boss, path, true))
                    continue;
            }
            nearest = i;
            best = distance;
        }
        return nearest;
    }

    bool TryWestRejoin(Player& tank, AiObjectContext* context, Creature* boss, DcRunState::PatchwerkSweep& sweep)
    {
        if (!WestRecoveryArea(tank))
            return false;

        LoadLinkedTrashGrids(tank);
        // Slimes on the opposite bank can respawn after a completed crossing.
        // Closing the boss gate still requires every linked pack to be clear.
        if (EntrySection(tank) < Embalming)
            return false;
        std::optional<std::size_t> const pending = WestRecoveryGoal(tank);
        std::optional<std::size_t> entry;
        if (!pending && tank.GetExactDist(&Camp) <= 15.0f)
        {
            auto const path = BuildSweepCampLeg(&tank, Camp);
            if (AtDryCamp(tank) || (!path.empty() && ClearsPatrol(tank, boss, path, true)))
                entry = CampCheckpoint;
        }
        if (!entry)
            entry = FindDryWestAnchor(tank, boss);
        if (!entry)
            return false;

        std::size_t const goal = pending.value_or(CampCheckpoint);
        std::size_t const old = sweep.checkpoint;
        std::array<bool, SectionCount> completed{};
        for (std::size_t i = Gallery; i < FirstCrossing; ++i)
            completed[i] = !SectionHasTrash(tank, static_cast<SectionId>(i));
        completed[SecondCrossing] = RaidAtBank(tank, OuterBank);
        completed[FirstCrossing] = completed[SecondCrossing];
        completed[BetweenCrossings] = completed[SecondCrossing];
        for (std::size_t i = Southwest; i < WhiteCamp; ++i)
            completed[i] = !SectionHasTrash(tank, static_cast<SectionId>(i));

        // The closest connection is a navigation choice. It cannot mark a
        // crossing or a pull complete, and may lead back to the first live pack.
        if (!completed[SecondCrossing])
        {
            sweep.checkpoint = OuterBank;
            sweep.recoveryGoal.reset();
        }
        else if (*entry > goal)
        {
            sweep.checkpoint = *entry;
            sweep.recoveryGoal = goal;
        }
        else
        {
            sweep.checkpoint = std::max(*entry, WestFirstPull);
            sweep.recoveryGoal.reset();
        }

        for (std::size_t i = 0; i < SectionFor(std::min(sweep.checkpoint, goal)); ++i)
        {
            if (DcPatchwerkRouteOrder::CanResumeAt(i, i + 1, completed))
                CompleteSection(tank, sweep, static_cast<SectionId>(i));
        }
        sweep.reachedCheckpoint = false;
        sweep.followerAnchor.reset();
        ResetRouteTargets(context);
        LOG_INFO("playerbots.dungeonclear", "[DC:{}] Patchwerk: route rejoin {} -> {} (checkpoint {} to {}, goal {})",
            tank.GetName(), Sections[SectionFor(old)].name, Sections[SectionFor(goal)].name, old, sweep.checkpoint,
            goal);
        if (tank.GetExactDist(&Camp) <= 15.0f && !LinkedTrash(tank).empty())
            LOG_INFO("playerbots.dungeonclear", "[DC:{}] Patchwerk: route waiting white-camp: linked trash remains",
                tank.GetName());
        return true;
    }

    bool TryRejoinAtSecondLanding(Player& tank, AiObjectContext* context, Creature* boss,
                                  DcRunState::PatchwerkSweep& sweep)
    {
        Checkpoint const& bank = RoutePoint(OuterBank);
        Position const landing(bank.x, bank.y, bank.z);
        // After combat a tank can be on the dry lip just outside WestRecoveryArea.
        // A short, dry connection must lead west to the landing without recrossing.
        if (tank.GetPositionX() >= 3042.0f || tank.GetExactDist(&landing) > 8.0f)
            return false;
        if (!OnFarBank(tank, OuterBank) || !RaidAtBank(tank, WestBank) || SectionHasTrash(tank, FirstCrossing))
            return false;
        if (tank.GetExactDist(&landing) > Arrive)
        {
            auto const path = BuildSweepCampLeg(&tank, landing);
            if (path.empty() || !ClearsPatrol(tank, boss, path, true))
                return false;
        }

        std::size_t const old = sweep.checkpoint;
        CompleteSection(tank, sweep, FirstCrossing);
        CompleteSection(tank, sweep, BetweenCrossings);
        sweep.checkpoint = OuterBank;
        sweep.reachedCheckpoint = false;
        sweep.followerAnchor = landing;
        ResetRouteTargets(context);
        LOG_INFO("playerbots.dungeonclear", "[DC:{}] Patchwerk: route rejoin checkpoint {} to {} at the second landing",
            tank.GetName(), old, OuterBank);
        return true;
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

void ObserveEntry(Player* tank, AiObjectContext* context, uint32 bossEntry)
{
    if (!CanRun(tank, context, bossEntry) || SweepInProgress(tank, context, bossEntry))
        return;
    LoadLinkedTrashGrids(*tank);
    auto& sweep = DcRun::Of(context).patchwerkSweep;
    SectionId const pending = EntrySection(*tank);
    for (std::size_t i = Gallery; i < pending; ++i)
    {
        SectionId const section = static_cast<SectionId>(i);
        if (sweep.completedSections & (1u << section))
            continue;
        ReportSection(*tank, sweep, section);
        CompleteSection(*tank, sweep, section);
    }
    if (pending < FirstCrossing)
        ReportSection(*tank, sweep, pending);
}

bool EntryPullDue(Player& tank, uint32 order)
{
    SectionId const section = EntrySection(tank);
    if (section == LowerRoom)
        return order == 1 || order == 2;
    return (section == Gallery && order == 0) || (section == Doorway && order == 3);
}

bool SetApproachAnchor(Player* tank, AiObjectContext* context, DungeonBossInfo& next)
{
    if (!CanRun(tank, context, next.entry))
        return false;
    Checkpoint const anchor = SweepInProgress(tank, context, next.entry)
        ? ApproachAnchor : EntryAnchor(*tank, EntrySection(*tank));
    next.x = anchor.x;
    next.y = anchor.y;
    next.z = anchor.z;
    return true;
}

bool AtEmbalmingHandoff(Player* tank, AiObjectContext* context, uint32 bossEntry)
{
    if (!CanRun(tank, context, bossEntry) || SweepInProgress(tank, context, bossEntry))
        return false;
    auto const& pull = context->GetValue<DcPullContext&>(DcKey::PullContext)->Get();
    return (pull.naxx40RangedStand && pull.phase != DcPullPhase::Idle) ||
        (AtApproachAnchor(*tank) && EntrySection(*tank) == Embalming);
}

bool DeferEmbalmingUntilLowerEast(Player* tank, AiObjectContext* context, uint32 bossEntry, Unit const* target)
{
    return CanRun(tank, context, bossEntry) && tank->GetPositionY() <= -3242.0f &&
        !SweepInProgress(tank, context, bossEntry) && DcNaxx40Embalming::IsSlime(target) &&
        EntrySection(*tank) < Embalming;
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
    if (!sweep.reachedCheckpoint || sweep.recoveryGoal || sweep.checkpoint >= CampCheckpoint)
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
    if (!CanRun(tank, context, bossEntry) || HasLowerEastPack(*tank))
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
    if (!CanRun(tank, context, bossEntry))
        return false;
    return InsideWestApproach(*tank) || WestRecoveryArea(*tank) ||
        ((AtSweepEntrance(*tank) || BetweenCrossingBanks(*tank)) && EntrySection(*tank) == FirstCrossing);
}

bool SweepInProgress(Player* tank, AiObjectContext* context, uint32 bossEntry)
{
    if (!CanRun(tank, context, bossEntry))
        return false;
    DcRunState::PatchwerkSweep const& sweep = DcRun::Of(context).patchwerkSweep;
    return sweep.instanceId == tank->GetInstanceId();
}

bool HasRouteFollowerAnchor(Player* tank, AiObjectContext* context, uint32 bossEntry)
{
    if (!SweepInProgress(tank, context, bossEntry))
        return false;
    auto const& run = DcRun::Of(context);
    auto const& pull = context->GetValue<DcPullContext&>(DcKey::PullContext)->Get();
    return !run.paused && run.patchwerkSweep.followerAnchor && pull.phase == DcPullPhase::Idle;
}

bool DriveRouteFollower(PlayerbotAI* ai, Player* tank)
{
    Player* const member = ai->GetBot();
    if (!IsNaxx40Map(member->FindMap()) || !member->IsInMap(tank) || member->IsInCombat())
        return false;
    PlayerbotAI* const tankAI = GET_PLAYERBOT_AI(tank);
    if (!tankAI)
        return false;
    AiObjectContext* const context = tankAI->GetAiObjectContext();
    auto const next = context->GetValue<std::optional<DungeonBossInfo>>(DcKey::NextDungeonBoss)->Get();
    auto const& run = DcRun::Of(context);
    auto const& sweep = run.patchwerkSweep;
    if (!next || !HasRouteFollowerAnchor(tank, context, next->entry))
        return false;

    Position anchor = *sweep.followerAnchor;
    bool const crossing = sweep.checkpoint == WestBank || sweep.checkpoint == OuterBank;
    bool shallowExit = false;
    if (DcRun::Of(ai).Throttled(DcThrottle::Naxx40PatchwerkFollower, 500))
        return true;
    if (crossing)
    {
        if (MemberAtBank(*member, sweep.checkpoint))
            return true;
        Checkpoint const& departure = RoutePoint(sweep.checkpoint - 1);
        Checkpoint const& landing = RoutePoint(sweep.checkpoint);
        if (sweep.checkpoint == WestBank && BeyondFirstLanding(*member))
        {
            Checkpoint const& dry = RoutePoint(WestBank + 1);
            anchor.Relocate(dry.x, dry.y, dry.z);
            shallowExit = true;
        }
        else
        {
            if (anchor.GetPositionX() == landing.x && DcNaxx40SlimeCrossing::IsNearBank(member) &&
                DcNaxx40SlimeCrossing::TryCross(ai, {{landing.x, landing.y, landing.z}}))
                return true;
            if (!OnFarBank(*member, sweep.checkpoint))
                anchor.Relocate(departure.x, departure.y, departure.z);
        }
    }

    G3D::Vector3 const destination(anchor.GetPositionX(), anchor.GetPositionY(), anchor.GetPositionZ());
    if (DcMovement::OwnsRunningEscort(member) &&
        (member->movespline->FinalDestination() - destination).squaredLength() <= 1.0f)
        return true;
    if (member->GetMotionMaster()->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE)
        DcMovement::StopBot(member, DcMovement::Stop::Hold);
    if (member->GetExactDist(&anchor) <= Arrive || !ai->CanMove() || member->IsFalling())
        return true;
    auto path = BuildSweepCampLeg(member, anchor);
    if (path.empty() && shallowExit)
        path = DcNaxx40SlimeCrossing::BuildPatchwerkRecoveryPath(member, anchor);
    Checkpoint const& bankApproach = RoutePoint(FirstBankApproach);
    if (path.empty() && crossing && sweep.checkpoint == WestBank && !shallowExit &&
        destination.x == RoutePoint(EastBank).x && member->GetPositionX() > bankApproach.x)
    {
        path = BuildSweepCampLeg(member, {bankApproach.x, bankApproach.y, bankApproach.z});
    }
    Creature* const boss = DcTargeting::GetLiveBoss(tank, context, Naxx40PatchwerkEntry);
    if (!path.empty() && ClearsPatrol(*member, boss, path, true))
        DcMovement::SplinePath(ai, path, MovementPriority::MOVEMENT_NORMAL);
    return true;
}

std::vector<ObjectGuid> LocalTrash(Player* tank, AiObjectContext* context)
{
    if (!CanRun(tank, context, Naxx40PatchwerkEntry) || DcRun::Of(context).paused)
        return {};

    DcRunState::PatchwerkSweep const& sweep = DcRun::Of(context).patchwerkSweep;
    if (sweep.instanceId != tank->GetInstanceId() || sweep.recoveryGoal)
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
    bool const fresh = sweep.instanceId != tank->GetInstanceId();
    if (fresh)
    {
        LoadLinkedTrashGrids(*tank);
        bool const between = BetweenCrossingBanks(*tank);
        if (!west && (EntrySection(*tank) < FirstCrossing || (!AtSweepEntrance(*tank) && !between)))
            return SweepResult::Inactive;
        ObserveEntry(tank, context, bossEntry);
        sweep.checkpoint = 0;
        sweep.reachedCheckpoint = false;
        sweep.recoveryGoal.reset();
        if (west && !TryWestRejoin(*tank, context, boss, sweep))
            return HoldRoute(ai, "route waiting southwest: no dry rejoin or earlier trash remains");
        if (!west && OnFarBank(*tank, WestBank))
        {
            sweep.checkpoint = WestBank;
            Checkpoint const& bank = RoutePoint(WestBank);
            sweep.followerAnchor = Position(bank.x, bank.y, bank.z);
        }
        sweep.instanceId = tank->GetInstanceId();
        DcMovement::StopBot(tank, DcMovement::Stop::Hold);
        LOG_INFO("playerbots.dungeonclear", "[DC:{}] Patchwerk: {} at checkpoint {} ({:.1f},{:.1f},{:.1f})",
            tank->GetName(), west ? "rejoining the west route" : "starting the west trash sweep",
            sweep.checkpoint, tank->GetPositionX(), tank->GetPositionY(), tank->GetPositionZ());
    }

    if (sweep.checkpoint >= RouteSize)
    {
        if (AtPatrolCamp(tank, context))
            return SweepResult::Waiting;
        sweep.checkpoint = CampCheckpoint;
        sweep.reachedCheckpoint = false;
    }

    // A combat or manual move can leave us on a different part of the route.
    // Reconcile before asking the mover for the old point again.
    Checkpoint const& expected = RoutePoint(sweep.checkpoint);
    uint32 const now = getMSTime();
    bool const mayProbe = !sweep.nextRecoveryProbe ||
        getMSTimeDiff(sweep.nextRecoveryProbe, now) >= 1800;
    if (!sweep.recoveryGoal && mayProbe && sweep.checkpoint >= WestBank && sweep.checkpoint < OuterBank &&
        !DcMovement::OwnsRunningEscort(tank))
    {
        if (west && RaidAtBank(*tank, OuterBank) && !SectionHasTrash(*tank, FirstCrossing) &&
            TryWestRejoin(*tank, context, boss, sweep))
            return SweepResult::Moving;
        if (TryRejoinAtSecondLanding(*tank, context, boss, sweep))
            return SweepResult::Moving;
    }
    if (!sweep.recoveryGoal && west && mayProbe && !DcMovement::OwnsRunningEscort(tank) &&
        tank->GetExactDist(expected.x, expected.y, expected.z) > 35.0f)
    {
        sweep.nextRecoveryProbe = now;
        if (!TryWestRejoin(*tank, context, boss, sweep))
            return HoldRoute(ai, "route waiting west: cannot rejoin without crossing gosma or bypassing trash");
    }

    if (sweep.recoveryGoal)
    {
        Checkpoint const& recovery = RoutePoint(sweep.checkpoint);
        SweepResult const movement = MoveToWhiteCheckpoint(ai, sweep.checkpoint, recovery, boss, true, mayProbe);
        if (movement == SweepResult::Blocked)
        {
            if (mayProbe)
                sweep.nextRecoveryProbe = now;
            return HoldRoute(ai, "route waiting " + std::string(Sections[SectionFor(sweep.checkpoint)].name) +
                ": no dry return to the pending pack");
        }
        if (movement == SweepResult::Moving)
            return movement;
        if (sweep.checkpoint == *sweep.recoveryGoal)
            sweep.recoveryGoal.reset();
        else
        {
            do
                --sweep.checkpoint;
            while (sweep.checkpoint > *sweep.recoveryGoal && !UsesWestBackbone(sweep.checkpoint));
            return SweepResult::Moving;
        }
    }

    // Combat can end far from a checkpoint whose pull stand was already
    // reached. Re-acquire the stand before asking what can be pulled from it.
    Checkpoint const& point = RoutePoint(sweep.checkpoint);
    SectionId const section = SectionFor(sweep.checkpoint);
    ReportSection(*tank, sweep, section);
    if (sweep.reachedCheckpoint && tank->GetExactDist(point.x, point.y, point.z) > Arrive)
    {
        sweep.reachedCheckpoint = false;
        ResetRouteTargets(context);
    }
    if (sweep.checkpoint == CampCheckpoint && AtDryCamp(*tank))
    {
        LoadLinkedTrashGrids(*tank);
        auto const remaining = LinkedTrash(*tank);
        auto const campDecision = DcPatchwerkRouteOrder::CampDecision(remaining.empty(), true, RaidNearCamp(*tank));
        if (campDecision == DcPatchwerkRouteOrder::CampResult::ClearTrash)
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
            if (mayProbe)
            {
                sweep.nextRecoveryProbe = now;
                if (WestRecoveryGoal(*tank) && TryWestRejoin(*tank, context, boss, sweep))
                    return SweepResult::Moving;
            }
            return HoldRoute(ai, "route waiting white-camp: linked trash still alive outside the safe pull area");
        }
        sweep.followerAnchor = RaidCamp;
        if (campDecision == DcPatchwerkRouteOrder::CampResult::Gather)
            return HoldRoute(ai, "route waiting white-camp: gathering the raid at the dry camp");
        sweep.checkpoint = RouteSize;
        CompleteSection(*tank, sweep, WhiteCamp);
        LOG_INFO("playerbots.dungeonclear", "[DC:{}] Patchwerk: linked trash clear, waiting at the white camp",
                 tank->GetName());
        return SweepResult::Waiting;
    }

    bool const atFarBank = point.crossing && OnFarBank(*tank, sweep.checkpoint);
    if (section >= NorthRoom && !sweep.followerAnchor)
        sweep.followerAnchor = NorthRoomCamp;
    if (!sweep.reachedCheckpoint && tank->GetExactDist(point.x, point.y, point.z) > Arrive && !atFarBank)
    {
        SweepResult const movement = MoveToWhiteCheckpoint(ai, sweep.checkpoint, point, boss, false, mayProbe);
        if (movement != SweepResult::Blocked)
            return movement;
        if (mayProbe)
            sweep.nextRecoveryProbe = now;
        return HoldRoute(ai, "route waiting " + std::string(Sections[section].name) +
            ": no safe movement to checkpoint " + std::to_string(sweep.checkpoint));
    }

    if (point.crossing)
    {
        sweep.followerAnchor = Position(point.x, point.y, point.z);
        if (!RaidAtBank(*tank, sweep.checkpoint))
            return HoldRoute(ai, "route waiting " + std::string(Sections[section].name) +
                ": waiting for the raid on the destination bank");
        if (tank->GetExactDist(point.x, point.y, point.z) > Arrive && point.radius > 0.0f &&
            !LinkedTrash(*tank, &point).empty())
        {
            SweepResult const movement = MoveToWhiteCheckpoint(ai, sweep.checkpoint, point, boss, true, mayProbe);
            if (movement != SweepResult::Blocked)
                return movement;
            if (mayProbe)
                sweep.nextRecoveryProbe = now;
            return HoldRoute(ai, "route waiting second-crossing: no dry approach to the landing pull");
        }
        if (sweep.checkpoint != sweep.lastCrossingReported)
        {
            sweep.lastCrossingReported = sweep.checkpoint;
            LOG_INFO("playerbots.dungeonclear", "[DC:{}] Patchwerk: arrived at crossing {} ({:.1f},{:.1f},{:.1f})",
                tank->GetName(), sweep.checkpoint == WestBank ? 1 : 2, point.x, point.y, point.z);
        }
    }

    if (point.radius > 0.0f)
    {
        if (!sweep.reachedCheckpoint)
        {
            sweep.reachedCheckpoint = true;
            DcMovement::StopBot(tank, DcMovement::Stop::Hold);
            ResetRouteTargets(context);
        }
        auto const& targets = context->GetValue<std::vector<ObjectGuid>>(DcKey::RoomTrashRemaining)->Get();
        if (!targets.empty())
            return SweepResult::Clearing;
        if (point.patrol && FindLiveSpawn(*tank, point.patrol))
            return HoldRoute(ai, "route waiting " + std::string(Sections[section].name) + ": patrol spawn " +
                std::to_string(point.patrol) +
                " to reach the dry pull position.");
        else if (IsWestGolemCheckpoint(sweep.checkpoint))
        {
            bool const live = HasWesternGolems(*tank);
            if (live && sweep.checkpoint == WestGolemFallback)
                return HoldRoute(ai, "western Golems remain outside the firing position.");
            if (!live)
                sweep.checkpoint = Sections[section].last;
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

    if (sweep.checkpoint == Sections[section].last && !SectionHasTrash(*tank, section))
        CompleteSection(*tank, sweep, section);
    if (point.crossing)
        sweep.followerAnchor.reset();
    ++sweep.checkpoint;
    if (RoutePoint(sweep.checkpoint).crossing)
        sweep.followerAnchor = Position(point.x, point.y, point.z);
    sweep.reachedCheckpoint = false;
    ResetRouteTargets(context);
    return SweepResult::Moving;
}
}
