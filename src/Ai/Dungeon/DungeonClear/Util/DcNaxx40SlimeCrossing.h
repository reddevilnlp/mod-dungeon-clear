/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, or later.
 */

#ifndef DUNGEONCLEAR_NAXX40_SLIME_CROSSING_H
#define DUNGEONCLEAR_NAXX40_SLIME_CROSSING_H

#include "MoveSplineInitArgs.h"

class Player;
class PlayerbotAI;
struct Position;

namespace DcNaxx40SlimeCrossing
{
    inline constexpr float RouteLookahead = 36.0f;
    inline constexpr float PatchwerkFirstBankX = 3088.4287f;
    inline constexpr float PatchwerkFirstBankY = -3219.822f;
    inline constexpr float PatchwerkFirstBankZ = 294.21243f;

    // Planned points must start at or ahead of the bot and be in route order.
    // Takes over only at the near bank of a crossing already present in that route.
    bool TryCross(PlayerbotAI* ai, Movement::PointsArray const& planned);

    // For direct pursuit/follow, check the corresponding normal path to the target first.
    bool TryCrossTowards(PlayerbotAI* ai, G3D::Vector3 const& destination);

    // Cheap near-bank predicate before building a route for TryCross.
    bool IsNearBank(Player const* bot);

    // The exact checked crossing spline is still running. Leave it uninterrupted.
    bool CrossingActive(Player* bot);

    inline G3D::Vector3 const GrobbulusBankA{3317.6309f, -3241.0579f, 294.0635f};
    inline G3D::Vector3 const GrobbulusBankB{3318.1392f, -3255.6616f, 293.78555f};
    bool TryGrobbulusCrossing(PlayerbotAI* ai);
    bool IsDryCampGround(Player const& bot, G3D::Vector3 const& point);
    Movement::PointsArray BuildSurveyedDryLeg(Player* bot, G3D::Vector3 const& end);
    Movement::PointsArray BuildDryCampPath(Player* bot, Position const& destination);

    // Local Patchwerk rejoin: tests short floor-following detours to a dry point.
    // Only a character already on a shallow slime edge may use the short exit prefix.
    Movement::PointsArray BuildPatchwerkRecoveryPath(Player* bot, Position const& destination);
}

#endif
