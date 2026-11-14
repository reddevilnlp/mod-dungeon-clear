/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, or later.
 */

#ifndef DUNGEONCLEAR_NAXX40_LIVING_POISON_TIMING_H
#define DUNGEONCLEAR_NAXX40_LIVING_POISON_TIMING_H

#include "MoveSplineInitArgs.h"

#include <array>
#include <cstdint>
#include <vector>

class Player;

namespace DcNaxx40LivingPoisonTiming
{
    // Capture and use on the map thread, in one update. Every bot tested against
    // this snapshot must launch its exact path at the supplied run speed now.
    class Snapshot
    {
    public:
        explicit Snapshot(Player* observer);

        [[nodiscard]] bool IsReady() const { return _ready; }
        [[nodiscard]] bool CanCross(Movement::PointsArray const& path, float runSpeed) const;

    private:
        struct Segment
        {
            G3D::Vector3 start;
            G3D::Vector3 end;
            float startTime;
            float endTime;
        };

        static bool TooClose(Segment const& bot, Segment const& poison);
        static bool NewSummonCouldReach(Segment const& bot, G3D::Vector3 const& spawn, float speed);

        std::vector<Segment> _poison;
        std::array<float, 3> _waveSpeed{};
        uint32_t _capturedAt = 0;
        bool _ready = false;
    };
}

#endif
