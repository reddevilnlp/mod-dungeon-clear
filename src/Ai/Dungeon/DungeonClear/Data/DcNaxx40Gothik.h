/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, or later.
 */

#ifndef DUNGEONCLEAR_NAXX40_GOTHIK_H
#define DUNGEONCLEAR_NAXX40_GOTHIK_H

#include "Define.h"

class Player;

namespace DcNaxx40Gothik
{
    inline constexpr uint32 MapId = 533;
    inline constexpr uint32 Entry = 351035;

    bool AwaitingOpener(Player* bot);
}

#endif
