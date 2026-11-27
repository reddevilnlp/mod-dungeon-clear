/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, or later.
 */

#include "DcNaxx40SpiderQuarter.h"

#include "Ai/Dungeon/DungeonClear/Data/ScriptedPullRegistry.h"
#include "ObjectGuid.h"
#include "Position.h"

#include <utility>

void DcNaxx40SpiderQuarter::AppendStages(std::vector<ScriptedPullStage>& stages)
{
    // Anub's in-place approach replaces orders 8 through 12.
    uint32 order = 13;
    auto add = [&](uint32 boss, char const* name, Position const& camp, Position const& stand,
                   Position const& arm, float armRadius, std::vector<uint32> entries,
                   std::vector<ObjectGuid::LowType> spawns,
                   std::optional<std::array<float, 3>> alternateCamp = std::nullopt)
    {
        ScriptedPullStage stage;
        stage.mapId = 533;
        stage.bossEntry = boss;
        stage.order = order++;
        stage.name = name;
        camp.GetPosition(stage.campX, stage.campY, stage.campZ);
        stand.GetPosition(stage.standX, stage.standY, stage.standZ);
        arm.GetPosition(stage.armX, stage.armY, stage.armZ);
        stage.armRadius = armRadius;
        stage.entries = std::move(entries);
        stage.spawnIds = std::move(spawns);
        stage.alternateCamp = alternateCamp;
        stage.quarantineOtherPacks = false;
        stages.push_back(std::move(stage));
    };

    Position const entrance(3059.3f, -3479.2f, 295.256f);
    Position const westStand(3076.0f, -3505.0f, 287.080f);
    std::vector<uint32> const mixed = {351010, 351011, 351012};
    std::vector<ObjectGuid::LowType> const westEntry = {361187, 361188, 361189, 361190, 361205, 361206};

    Position const westCamp(3082.2f, -3515.0f, 287.079f);
    Position const westArm(3113.0f, -3530.0f, 287.1f);
    Position const upperCamp(3095.4f, -3544.7f, 287.0794f);
    Position const spinnerArm(3133.0f, -3566.0f, 286.0f);
    Position const spinnerCamp(3154.1f, -3572.9f, 284.9804f);
    add(Faerlina, "Naxx40 - Faerlina west mixed pack 361189", entrance, westStand,
        {3104.0f, -3503.0f, 290.0f}, 59.0f, mixed, westEntry);
    add(Faerlina, "Naxx40 - Faerlina west Skitterers 361781", westCamp,
        {3097.9f, -3550.5f, 288.163f}, westArm, 47.0f, {351013},
        {361776, 361777, 361778, 361779, 361780, 361781, 361782, 361783, 361784, 361785});
    add(Faerlina, "Naxx40 - Faerlina upper Skitterer patrol 361858", upperCamp,
        {3127.0f, -3571.0f, 287.0813f}, {3106.0f, -3543.0f, 287.1f}, 48.0f, {351013},
        {361850, 361851, 361852, 361853, 361855, 361856, 361857, 361858},
        std::array{3089.4f, -3544.7f, 287.079f});
    add(Faerlina, "Naxx40 - Faerlina east Spinners 361297", spinnerCamp,
        {3176.7f, -3597.9f, 280.3066f}, spinnerArm, 46.0f, {351011, 351014},
        {361297, 361299, 361300, 361877, 361878, 361879, 361880});
    add(Faerlina, "Naxx40 - Faerlina west Spinners 361298", spinnerCamp,
        {3146.0f, -3594.0f, 287.0775f}, spinnerArm, 46.0f, {351011, 351014},
        {361298, 361301, 361302, 361881, 361882, 361883, 361884});

    add(Faerlina, "Naxx40 - Faerlina upper landing Skitterers 361872", {3225.1f, -3651.6f, 287.075f},
        {3221.0f, -3660.0f, 287.079f}, {3211.0f, -3636.0f, 285.0f}, 33.0f, {351013},
        {361866, 361867, 361868, 361869, 361870, 361871, 361872, 361873, 361874});
    Position const landing(3232.7f, -3660.2f, 287.089f);
    Position const landingArm(3246.0f, -3671.0f, 284.0f);
    Position const rampTop(3246.0f, -3684.0f, 287.080f);
    Position const lowerCamp(3278.0f, -3694.0f, 280.050f);
    Position const rampArm(3274.0f, -3692.0f, 282.0f);

    add(Faerlina, "Naxx40 - Faerlina landing Necro Stalkers 361304", landing,
        {3237.0f, -3678.0f, 287.090f}, landingArm, 34.0f, {16453}, {361303, 361304});
    add(Faerlina, "Naxx40 - Faerlina landing Skitterers 361899", landing,
        {3254.0f, -3681.0f, 287.882f}, landingArm, 34.0f, {351013},
        {361897, 361898, 361899, 361900, 361901, 361902, 361903, 361904});
    add(Faerlina, "Naxx40 - Faerlina middle ramp Skitterers 361309", rampTop,
        {3285.9f, -3698.6f, 277.058f}, rampArm, 36.0f, {351013},
        {361306, 361307, 361308, 361309, 361885, 361886, 361887, 361888});
    add(Faerlina, "Naxx40 - Faerlina lower ramp Necro Stalkers 361876", rampTop,
        {3278.0f, -3698.0f, 279.251f}, rampArm, 36.0f, {16453}, {361875, 361876});
    add(Faerlina, "Naxx40 - Faerlina lower Skitterer patrol 361791", lowerCamp,
        {3296.0f, -3697.0f, 274.820f}, rampArm, 36.0f, {351013},
        {361786, 361787, 361788, 361789, 361790, 361791, 361792, 361793});

    std::vector<uint32> const casters = {351016, 351017};
    add(Faerlina, "Naxx40 - Faerlina south Acolytes 361798", lowerCamp,
        {3316.7f, -3693.2f, 262.135f}, {3286.0f, -3695.0f, 278.0f}, 40.0f, casters,
        {361794, 361795, 361796, 361797, 361798, 361799, 361800, 361801});
    add(Faerlina, "Naxx40 - Faerlina north Acolytes 361838", lowerCamp,
        {3315.9f, -3693.4f, 262.567f}, {3286.0f, -3695.0f, 278.0f}, 40.0f, casters,
        {361834, 361835, 361836, 361837, 361838, 361839, 361840, 361841});
    add(Faerlina, "Naxx40 - Faerlina centre Acolytes 361826", {3310.5f, -3693.8f, 265.153f},
        {3326.7f, -3679.9f, 259.083f}, {3299.0f, -3693.0f, 270.0f}, 28.0f, casters,
        {361826, 361827, 361828, 361829, 361830, 361831, 361832, 361833});
    Position const roomCamp(3326.0f, -3681.4f, 259.083f);
    Position const roomArm(3320.0f, -3686.0f, 262.0f);
    add(Faerlina, "Naxx40 - Faerlina inner Acolytes 361818", roomCamp,
        {3340.8f, -3660.7f, 259.083f}, roomArm, 24.0f, casters,
        {361818, 361819, 361820, 361821, 361822, 361823, 361824, 361825});
    add(Faerlina, "Naxx40 - Faerlina rear Acolytes 361811", roomCamp,
        {3341.8f, -3674.5f, 259.077f}, roomArm, 24.0f, casters,
        {361810, 361811, 361812, 361813, 361814, 361815, 361816, 361817});

    // This pack covers the side of Faerlina's room. Arm it only if the exit
    // actually follows that side, after the boss objective has moved on.
    add(Maexxna, "Naxx40 - Maexxna room exit Acolytes 361802", roomCamp,
        {3349.0f, -3688.0f, 259.079f}, {3350.0f, -3678.0f, 259.08f}, 15.0f, casters,
        {361802, 361803, 361804, 361805, 361806, 361807, 361808, 361809});
    add(Maexxna, "Naxx40 - Maexxna east bridge approach 361590", {3248.5f, -3750.4f, 279.248f},
        {3219.9f, -3767.2f, 272.797f}, {3254.0f, -3744.0f, 279.240f}, 16.0f, {351010, 351015},
        {361590, 361859, 361860, 361861, 361862});
    add(Maexxna, "Naxx40 - Maexxna west bridge approach 361591", {3211.4f, -3772.1f, 273.490f},
        {3177.1f, -3779.8f, 273.950f}, {3225.0f, -3765.0f, 273.5f}, 27.0f, {351010, 351015},
        {361305, 361591, 361863, 361864, 361865});

    // The Acolytes in the shared tunnel stayed at range when pulled downhill.
    // Leave those casters to the in-place corridor fight; these melee packs dragged back.
    add(Maexxna, "Naxx40 - Maexxna upper tunnel 361956", {3218.6f, -3873.8f, 281.5f},
        {3248.2f, -3875.0f, 294.7f}, {3237.0f, -3876.0f, 289.0f}, 22.0f, {16448, 351078},
        {361954, 361955, 361956, 361957, 361958});
    add(Maexxna, "Naxx40 - Maexxna upper tunnel 362009", {3270.8f, -3869.1f, 294.7f},
        {3305.6f, -3870.9f, 294.7f}, {3270.8f, -3869.1f, 294.7f}, 18.0f, {16448, 351078},
        {362007, 362008, 362009, 362010, 362011});
}
