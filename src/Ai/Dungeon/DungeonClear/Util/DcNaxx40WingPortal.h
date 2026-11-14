/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 */

#ifndef DUNGEONCLEAR_NAXX40_WING_PORTAL_H
#define DUNGEONCLEAR_NAXX40_WING_PORTAL_H

#include "Define.h"

class PlayerbotAI;

namespace DcNaxx40WingPortal
{
bool Begin(PlayerbotAI* ai, uint32 nextBoss);
bool OwnsMovement(PlayerbotAI* ai);
void RegisterMapUpdate();
}

#endif
