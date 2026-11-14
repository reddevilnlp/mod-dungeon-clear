/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, or later.
 */

#include "Ai/Dungeon/DungeonClear/Util/DcPatchwerkRouteOrder.h"
#include "gtest/gtest.h"

#include <array>

namespace
{
    using DcPatchwerkRouteOrder::CanResumeAt;
    using DcPatchwerkRouteOrder::CampDecision;
    using DcPatchwerkRouteOrder::CampResult;

    TEST(DcPatchwerkRouteOrder, NeverSkipsTheFirstPendingRequirement)
    {
        constexpr std::array completed{true, true, false, true, false};
        EXPECT_TRUE(CanResumeAt(0, 2, completed));
        EXPECT_FALSE(CanResumeAt(0, 3, completed));
        EXPECT_FALSE(CanResumeAt(0, 4, completed));
    }

    TEST(DcPatchwerkRouteOrder, CanResumeAPreviouslyCompletedPassage)
    {
        constexpr std::array completed{true, true, true, false, false};
        EXPECT_TRUE(CanResumeAt(1, 1, completed));
        EXPECT_TRUE(CanResumeAt(1, 3, completed));
        EXPECT_FALSE(CanResumeAt(3, 1, completed));
        EXPECT_FALSE(CanResumeAt(1, completed.size(), completed));
        EXPECT_FALSE(CanResumeAt(0, 0, {}));
    }

    TEST(DcPatchwerkRouteOrder, CampPositionCannotStandInForClearedTrash)
    {
        EXPECT_EQ(CampDecision(false, false, false), CampResult::ClearTrash);
        EXPECT_EQ(CampDecision(false, true, true), CampResult::ClearTrash);
    }

    TEST(DcPatchwerkRouteOrder, CampRequiresDryArrivalAndGatherBeforePreparation)
    {
        EXPECT_EQ(CampDecision(true, false, false), CampResult::Approach);
        EXPECT_EQ(CampDecision(true, false, true), CampResult::Approach);
        EXPECT_EQ(CampDecision(true, true, false), CampResult::Gather);
        EXPECT_EQ(CampDecision(true, true, true), CampResult::Prepare);
    }
}
