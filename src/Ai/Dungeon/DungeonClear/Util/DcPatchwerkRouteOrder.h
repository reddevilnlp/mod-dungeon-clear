/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, or later.
 */

#ifndef DUNGEONCLEAR_PATCHWERK_ROUTE_ORDER_H
#define DUNGEONCLEAR_PATCHWERK_ROUTE_ORDER_H

#include <cstddef>
#include <span>

namespace DcPatchwerkRouteOrder
{
    // Resnapping the tank can retire navigation points. Every intervening
    // objective or crossing must have been independently confirmed.
    constexpr bool CanResumeAt(std::size_t current, std::size_t candidate, std::span<bool const> completed)
    {
        if (current >= completed.size() || candidate >= completed.size() || candidate < current)
            return false;
        for (std::size_t section = current; section < candidate; ++section)
            if (!completed[section])
                return false;
        return true;
    }

    enum class CampResult
    {
        ClearTrash,
        Approach,
        Gather,
        Prepare,
    };

    constexpr CampResult CampDecision(bool linkedTrashKnownClear, bool atCamp, bool raidReady)
    {
        if (!linkedTrashKnownClear)
            return CampResult::ClearTrash;
        if (!atCamp)
            return CampResult::Approach;
        return raidReady ? CampResult::Prepare : CampResult::Gather;
    }
}

#endif
