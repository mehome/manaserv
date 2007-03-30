/*
 *  The Mana World Server
 *  Copyright 2004 The Mana World Development Team
 *
 *  This file is part of The Mana World.
 *
 *  The Mana World  is free software; you can redistribute  it and/or modify it
 *  under the terms of the GNU General  Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or any later version.
 *
 *  The Mana  World is  distributed in  the hope  that it  will be  useful, but
 *  WITHOUT ANY WARRANTY; without even  the implied warranty of MERCHANTABILITY
 *  or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 *  more details.
 *
 *  You should  have received a  copy of the  GNU General Public  License along
 *  with The Mana  World; if not, write to the  Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 *  $Id: controller.cpp 3191 2007-03-15 23:47:13Z crush_tmw $
 */

#include "game-server/monster.hpp"

#include "utils/logger.h"

Monster::Monster():
    Being(OBJECT_MONSTER, 65535),
    mCountDown(0)
{
    mAttributes.resize(NB_ATTRIBUTES_CONTROLLED, 1); // TODO: fill with the real attributes
}

void Monster::update()
{
    /* Temporary "AI" behaviour that is purely artificial and not at all
     * intelligent.
     */
    if (mCountDown == 0)
    {
        if (mAction != DEAD)
        {
            Point randomPos( rand() % 320 + 720,
                                rand() % 320 + 840 );
            setDestination(randomPos);
            mCountDown = 10 + rand() % 10;

            LOG_DEBUG("Setting new random destination " << randomPos.x << ","
                      << randomPos.y << " for being " << getPublicID());
        }
        else
        {
            raiseUpdateFlags(UPDATEFLAG_REMOVE);
        }
    }
    else
    {
        mCountDown--;
    }
}

void Monster::die()
{
    mCountDown = 50; //sets remove time to 5 seconds
    Being::die();
}

WeaponStats Monster::getWeaponStats()
{

    WeaponStats weaponStats;

    /*
     * TODO: This should all be set by the monster database
     */
    weaponStats.piercing = 1;
    weaponStats.element = ELEMENT_NEUTRAL;
    weaponStats.skill = MONSTER_SKILL_WEAPON;

    return weaponStats;
}

void Monster::calculateDerivedAttributes()
{
    Being::calculateDerivedAttributes();
    /*
     * Do any monster specific attribute calculation here
     */
}