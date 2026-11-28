/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

// Authored-data lints for the Naxxramas (533) roster patch: the Four Horsemen,
// whose cast-spell credit hides them from BossSpawnIndex. No world state: these
// pin the bit, the per-tier anchor and the clear order the runtime relies on.

#include "gtest/gtest.h"

#include <vector>

#include "Ai/Dungeon/DungeonClear/Overrides/BossRosterRegistry.h"
#include "Ai/Dungeon/DungeonClear/Util/DcDifficulty.h"

namespace
{
    constexpr uint32 kMap = 533;
    constexpr uint32 kGothik = 16060;
    constexpr uint32 kPatchwerk = 16028;
    constexpr uint32 kRivendare = 30549;
    constexpr uint32 kMograine = 351037;

    DungeonBossInfo Static(uint32 entry, uint32 index)
    {
        DungeonBossInfo b;
        b.entry = entry;
        b.encounterIndex = index;
        b.mapId = kMap;
        return b;
    }

    // What BossSpawnIndex derives: every encounter but the Horsemen (bit 8).
    std::vector<DungeonBossInfo> DerivedBase()
    {
        return {
            Static(15956, 0), Static(15953, 1), Static(15952, 2), Static(15954, 3),
            Static(15936, 4), Static(16011, 5), Static(16061, 6), Static(kGothik, 7),
            Static(kPatchwerk, 9), Static(15931, 10), Static(15932, 11), Static(15928, 12),
            Static(15989, 13), Static(15990, 14),
        };
    }

    size_t IndexOf(std::vector<DungeonBossInfo> const& v, uint32 entry)
    {
        for (size_t i = 0; i < v.size(); ++i)
            if (v[i].entry == entry)
                return i;
        return v.size();
    }

    // The Horsemen row for `key`: exactly one, anchored on `anchor` at (x, y, z),
    // on bit 8, ordered between Gothik and Patchwerk.
    void ExpectHorsemen(DcDiffKey key, uint32 anchor, uint32 otherAnchor, float x, float y, float z)
    {
        auto const out = BossRosterRegistry::Apply(kMap, key, DerivedBase());
        ASSERT_EQ(out.size(), 15u);
        EXPECT_EQ(IndexOf(out, otherAnchor), out.size()) << "the other tier's horseman leaked in";

        size_t const h = IndexOf(out, anchor);
        ASSERT_LT(h, out.size());
        DungeonBossInfo const& row = out[h];
        EXPECT_EQ(row.kind, DungeonAnchorKind::Boss);
        EXPECT_STREQ(row.name.c_str(), "The Four Horsemen");
        EXPECT_EQ(row.encounterIndex, 8u) << "completion rides the real DBC bit";
        EXPECT_EQ(row.orderOverride, -1) << "bit 8 orders itself";
        EXPECT_EQ(row.doneBossStateIndex, -1);
        EXPECT_EQ(BossOrderKey(row), 8u);
        EXPECT_NEAR(row.x, x, 0.01f);
        EXPECT_NEAR(row.y, y, 0.01f);
        EXPECT_NEAR(row.z, z, 0.01f);

        EXPECT_EQ(IndexOf(out, kGothik) + 1, h) << "Horsemen right after Gothik";
        EXPECT_EQ(h + 1, IndexOf(out, kPatchwerk)) << "Patchwerk right after the Horsemen";
    }
}

TEST(DcNaxxramasTest, FourHorsemenOn10Man)
{
    ExpectHorsemen(DcDiffKey::Raid(RAID_DIFFICULTY_10MAN_NORMAL), kRivendare, kMograine,
                   2524.32f, -2951.28f, 245.633f);
}

TEST(DcNaxxramasTest, FourHorsemenOn25Man)
{
    ExpectHorsemen(DcDiffKey::Raid(RAID_DIFFICULTY_25MAN_NORMAL), kRivendare, kMograine,
                   2524.32f, -2951.28f, 245.633f);
}

TEST(DcNaxxramasTest, FourHorsemenOnIndividualProgression40Man)
{
    // Individual Progression runs its 40-man Naxxramas at raid difficulty 2.
    ExpectHorsemen(DcDiffKey::Raid(RAID_DIFFICULTY_10MAN_HEROIC), kMograine, kRivendare,
                   2519.95f, -2947.47f, 245.64f);
}
