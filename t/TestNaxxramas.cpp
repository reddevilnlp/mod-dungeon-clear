/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

// Authored-data lints for Naxxramas (533): the Four Horsemen roster patch (their
// cast-spell credit hides them from BossSpawnIndex) and the teleports that join
// the wings up (wing portals to the hub, the hub to the Frostwyrm Lair). No world
// state: these pin the bit, the per-tier anchor, the clear order and the event
// shapes the runtime relies on.

#include "gtest/gtest.h"

#include <algorithm>
#include <tuple>
#include <vector>

#include "Ai/Dungeon/DungeonClear/Data/DungeonEventRegistry.h"
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

namespace
{
    // 28444's landing in the hub.
    constexpr float kHubX = 3005.51f, kHubY = -3434.64f, kHubZ = 304.195f;

    // A conditional, non-repeatable wing-portal event: one TeleportParty from the
    // portal to the hub, listed after the wing's last boss.
    void ExpectWingPortal(uint32 id, uint32 panelBoss, DcDifficultyGate gate,
                          float x, float y, float z)
    {
        DungeonEvent const* ev = DungeonEventRegistry::Find(kMap, id);
        ASSERT_NE(ev, nullptr) << "event " << id;
        EXPECT_EQ(ev->activation, EventActivation::Conditional);
        EXPECT_TRUE(static_cast<bool>(ev->condition));
        EXPECT_FALSE(ev->repeatable) << "one trip per wing per run";
        EXPECT_EQ(ev->gate, gate);
        EXPECT_EQ(ev->panelSortAfterBossEntry, panelBoss);
        EXPECT_EQ(ev->panelGatesBossEntry, 0u);
        ASSERT_EQ(ev->steps.size(), 1u);
        EventStep const& s = ev->steps[0];
        EXPECT_EQ(s.kind, EventStepKind::TeleportParty);
        EXPECT_NEAR(s.x, x, 0.01f);
        EXPECT_NEAR(s.y, y, 0.01f);
        EXPECT_NEAR(s.z, z, 0.01f);
        EXPECT_NEAR(s.landX, kHubX, 0.01f);
        EXPECT_NEAR(s.landY, kHubY, 0.01f);
        EXPECT_NEAR(s.landZ, kHubZ, 0.01f);
    }

    std::vector<uint32> ConditionalIds(DcDiffKey key)
    {
        std::vector<uint32> ids;
        for (DungeonEvent const* ev : DungeonEventRegistry::Conditional(kMap, key))
            ids.push_back(ev->id);
        std::sort(ids.begin(), ids.end());
        return ids;
    }
}

TEST(DcNaxxramasTest, WingPortalsTeleportToTheHub)
{
    // Each portal once per tier, listed after the wing boss that tier's roster
    // carries (the panel hides an event whose boss is not in the roster).
    constexpr auto N = DcDifficultyGate::NormalOnly;
    constexpr auto H = DcDifficultyGate::HeroicOnly;
    ExpectWingPortal(1, 15952, N, 3465.16f, -3940.45f, 308.788f);   // Maexxna
    ExpectWingPortal(7, 351006, H, 3465.16f, -3940.45f, 308.788f);
    ExpectWingPortal(2, 16011, N, 2909.0f, -4025.02f, 273.475f);    // Loatheb
    ExpectWingPortal(8, 351020, H, 2909.0f, -4025.02f, 273.475f);
    ExpectWingPortal(3, 15928, N, 3539.02f, -2936.82f, 302.476f);   // Thaddius
    ExpectWingPortal(9, 351000, H, 3539.02f, -2936.82f, 302.476f);
    ExpectWingPortal(4, kRivendare, N, 2493.02f, -2921.78f, 241.193f);  // Horsemen
    ExpectWingPortal(5, kMograine, H, 2493.02f, -2921.78f, 241.193f);
}

class DcNaxxramasLairTest : public ::testing::TestWithParam<std::tuple<uint32, DcDifficultyGate, uint32>>
{
};

TEST_P(DcNaxxramasLairTest, HubTeleportsToTheFrostwyrmLair)
{
    auto const [id, gate, thaddius] = GetParam();
    DungeonEvent const* ev = DungeonEventRegistry::Find(kMap, id);
    ASSERT_NE(ev, nullptr);
    EXPECT_EQ(ev->activation, EventActivation::Conditional);
    EXPECT_TRUE(static_cast<bool>(ev->condition));
    EXPECT_TRUE(ev->repeatable) << "a wipe in the lair sends the raid back to the hub";
    EXPECT_EQ(ev->gate, gate);
    EXPECT_EQ(ev->panelSortAfterBossEntry, thaddius) << "listed just before Sapphiron";
    ASSERT_EQ(ev->steps.size(), 1u);
    EventStep const& s = ev->steps[0];
    EXPECT_EQ(s.kind, EventStepKind::TeleportParty);
    EXPECT_NEAR(s.x, kHubX, 0.01f);
    EXPECT_NEAR(s.y, kHubY, 0.01f);
    EXPECT_NEAR(s.z, kHubZ, 0.01f);
    // 72617's landing.
    EXPECT_NEAR(s.landX, 3498.3f, 0.01f);
    EXPECT_NEAR(s.landY, -5349.49f, 0.01f);
    EXPECT_NEAR(s.landZ, 144.968f, 0.01f);
}

INSTANTIATE_TEST_SUITE_P(DcNaxxramas, DcNaxxramasLairTest,
                         ::testing::Values(std::make_tuple(6u, DcDifficultyGate::NormalOnly, 15928u),
                                           std::make_tuple(10u, DcDifficultyGate::HeroicOnly, 351000u)));

TEST(DcNaxxramasTest, EachTierGetsOneCopyOfEveryEvent)
{
    std::vector<uint32> const normal = {1, 2, 3, 4, 6};
    std::vector<uint32> const forty = {5, 7, 8, 9, 10};
    EXPECT_EQ(ConditionalIds(DcDiffKey::Raid(RAID_DIFFICULTY_10MAN_NORMAL)), normal);
    EXPECT_EQ(ConditionalIds(DcDiffKey::Raid(RAID_DIFFICULTY_25MAN_NORMAL)), normal);
    EXPECT_EQ(ConditionalIds(DcDiffKey::Raid(RAID_DIFFICULTY_10MAN_HEROIC)), forty);
}
