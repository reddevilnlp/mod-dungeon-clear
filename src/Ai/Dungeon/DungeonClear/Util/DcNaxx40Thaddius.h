/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, or later.
 */

#ifndef DUNGEONCLEAR_NAXX40_THADDIUS_H
#define DUNGEONCLEAR_NAXX40_THADDIUS_H

#include "Define.h"

class Player;
class PlayerbotAI;
struct DcRunState;

namespace DcNaxx40Thaddius
{
    bool CanUseWingPortal(Player const& owner, DcRunState const& run);
    bool BeginExit(PlayerbotAI* ai);
    bool InExitTransition(PlayerbotAI* ai);
    bool BlocksGeneric(Player* bot, uint32 nextBoss);
    bool OwnsMovement(PlayerbotAI* ai);
    void RegisterMapUpdate();
}

#endif
