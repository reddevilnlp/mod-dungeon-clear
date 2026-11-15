/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, or later.
 */

#ifndef DUNGEONCLEAR_NAXX40_LOATHEB_TRANSIT_H
#define DUNGEONCLEAR_NAXX40_LOATHEB_TRANSIT_H

class PlayerbotAI;

namespace DcNaxx40LoathebTransit
{
    bool IsCrossing(PlayerbotAI* ai);
    bool HoldsProgression(PlayerbotAI* ai);
    void RegisterMapUpdate();
}

#endif
