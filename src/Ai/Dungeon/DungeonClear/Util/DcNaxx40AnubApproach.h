/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, or later.
 */

#ifndef DUNGEONCLEAR_NAXX40_ANUB_APPROACH_H
#define DUNGEONCLEAR_NAXX40_ANUB_APPROACH_H

#include "Define.h"

class AiObjectContext;
class Player;
class PlayerbotAI;
struct Position;

namespace DcNaxx40AnubApproach
{
    bool AppliesTo(Player const* tank, uint32 boss);
    bool OwnsApproach(Player* tank, AiObjectContext* context, uint32 boss);
    Position const* ActiveCampHold(Player* tank, AiObjectContext* context);
    bool Drive(PlayerbotAI* ai);
}

#endif
