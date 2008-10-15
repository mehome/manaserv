/*
 *  The Mana World Server
 *  Copyright 2004 The Mana World Development Team
 *
 *  This file is part of The Mana World.
 *
 *  The Mana World is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  any later version.
 *
 *  The Mana World is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with The Mana World; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *  $Id$
 */

#include <cassert>

#include "game-server/state.hpp"

#include "defines.h"
#include "point.h"
#include "game-server/accountconnection.hpp"
#include "game-server/gamehandler.hpp"
#include "game-server/inventory.hpp"
#include "game-server/item.hpp"
#include "game-server/itemmanager.hpp"
#include "game-server/map.hpp"
#include "game-server/mapcomposite.hpp"
#include "game-server/mapmanager.hpp"
#include "game-server/monster.hpp"
#include "game-server/npc.hpp"
#include "game-server/trade.hpp"
#include "net/messageout.hpp"
#include "scripting/script.hpp"
#include "utils/logger.h"

enum
{
    EVENT_REMOVE = 0,
    EVENT_INSERT,
    EVENT_WARP
};

/**
 * Event expected to happen at next update.
 */
struct DelayedEvent
{
    unsigned short type, x, y;
    MapComposite *map;
};

typedef std::map< Object *, DelayedEvent > DelayedEvents;

/**
 * List of delayed events.
 */
static DelayedEvents delayedEvents;

/**
 * Updates object states on the map.
 */
static void updateMap(MapComposite *map)
{
    // 1. update object status.
    std::vector< Thing * > const &things = map->getEverything();
    for (std::vector< Thing * >::const_iterator i = things.begin(),
         i_end = things.end(); i != i_end; ++i)
    {
        (*i)->update();
    }

    // 2. run scripts.
    if (Script *s = map->getScript())
    {
        s->update();
    }

    // 3. perform actions.
    for (MovingObjectIterator i(map->getWholeMapIterator()); i; ++i)
    {
        (*i)->perform();
    }

    // 4. move objects around and update zones.
    for (MovingObjectIterator i(map->getWholeMapIterator()); i; ++i)
    {
        (*i)->move();
    }
    map->update();
}

/**
 * Sets message fields describing character look.
 */
static void serializeLooks(Character *ch, MessageOut &msg, bool full)
{
    Possessions const &poss = ch->getPossessions();
    static int const nb_slots = 4;
    static int const slots[nb_slots] =
        { EQUIP_FIGHT1_SLOT, EQUIP_HEAD_SLOT, EQUIP_TORSO_SLOT, EQUIP_LEGS_SLOT };

    // Bitmask describing the changed entries.
    int changed = (1 << nb_slots) - 1;
    if (!full)
    {
        // TODO: do not assume the whole equipment changed, when an update is asked for.
        changed = (1 << nb_slots) - 1;
    }

    int items[nb_slots];
    // Partially build both kinds of packet, to get their sizes.
    int mask_full = 0, mask_diff = 0;
    int nb_full = 0, nb_diff = 0;
    for (int i = 0; i < nb_slots; ++i)
    {
        int id = poss.equipment[slots[i]];
        ItemClass *eq;
        items[i] = id && (eq = ItemManager::getItem(id)) ? eq->getSpriteID() : 0;
        if (changed & (1 << i))
        {
            // Skip slots that have not changed, when sending an update.
            ++nb_diff;
            mask_diff |= 1 << i;
        }
        if (items[i])
        {
            /* If we are sending the whole equipment, only filled slots have to
               be accounted for, as the other ones will be automatically cleared. */
            ++nb_full;
            mask_full |= 1 << i;
        }
    }

    // Choose the smaller payload.
    if (nb_full <= nb_diff) full = true;

    /* Bitmask enumerating the sent slots.
       Setting the upper bit tells the client to clear the slots beforehand. */
    int mask = full ? mask_full | (1 << 7) : mask_diff;

    msg.writeByte(mask);
    for (int i = 0; i < nb_slots; ++i)
    {
        if (mask & (1 << i)) msg.writeShort(items[i]);
    }
}

/**
 * Informs a player of what happened around the character.
 */
static void informPlayer(MapComposite *map, Character *p)
{
    MessageOut moveMsg(GPMSG_BEINGS_MOVE);
    MessageOut damageMsg(GPMSG_BEINGS_DAMAGE);
    Point pold = p->getOldPosition(), ppos = p->getPosition();
    int pid = p->getPublicID(), pflags = p->getUpdateFlags();

    // Inform client about activities of other beings near its character
    for (MovingObjectIterator i(map->getAroundCharacterIterator(p, AROUND_AREA)); i; ++i)
    {
        MovingObject *o = *i;

        Point oold = o->getOldPosition(), opos = o->getPosition();
        int otype = o->getType();
        int oid = o->getPublicID(), oflags = o->getUpdateFlags();
        int flags = 0;

        // Check if the character p and the moving object o are around.
        bool wereInRange = pold.inRangeOf(oold, AROUND_AREA) &&
                           !((pflags | oflags) & UPDATEFLAG_NEW_ON_MAP);
        bool willBeInRange = ppos.inRangeOf(opos, AROUND_AREA);

        if (!wereInRange && !willBeInRange)
        {
            // Nothing to report: o and p are far away from each other.
            continue;
        }


        if (wereInRange && willBeInRange)
        {
            // Send attack messages.
            if ((oflags & UPDATEFLAG_ATTACK) && oid != pid)
            {
                MessageOut AttackMsg(GPMSG_BEING_ATTACK);
                AttackMsg.writeShort(oid);
                AttackMsg.writeByte(o->getDirection());
                AttackMsg.writeByte(static_cast< Being * >(o)->getAttackType());
                gameHandler->sendTo(p, AttackMsg);
            }

            // Send action change messages.
            if ((oflags & UPDATEFLAG_ACTIONCHANGE))
            {
                MessageOut ActionMsg(GPMSG_BEING_ACTION_CHANGE);
                ActionMsg.writeShort(oid);
                ActionMsg.writeByte(static_cast< Being * >(o)->getAction());
                gameHandler->sendTo(p, ActionMsg);
            }

            // Send looks change messages.
            if (oflags & UPDATEFLAG_LOOKSCHANGE)
            {
                MessageOut LooksMsg(GPMSG_BEING_LOOKS_CHANGE);
                LooksMsg.writeShort(oid);
                serializeLooks(static_cast< Character * >(o), LooksMsg, false);
                gameHandler->sendTo(p, LooksMsg);
            }

            // Send direction change messages.
            if (oflags & UPDATEFLAG_DIRCHANGE)
            {
                MessageOut DirMsg(GPMSG_BEING_DIR_CHANGE);
                DirMsg.writeShort(oid);
                DirMsg.writeByte(o->getDirection());
                gameHandler->sendTo(p, DirMsg);
            }

            // Send damage messages.
            if (o->canFight())
            {
                Being *victim = static_cast< Being * >(o);
                Hits const &hits = victim->getHitsTaken();
                for (Hits::const_iterator j = hits.begin(),
                     j_end = hits.end(); j != j_end; ++j)
                {
                    damageMsg.writeShort(oid);
                    damageMsg.writeShort(*j);
                }
            }

            if (oold == opos)
            {
                // o does not move, nothing more to report.
                continue;
            }
        }

        if (!willBeInRange)
        {
            // o is no longer visible from p. Send leave message.
            MessageOut leaveMsg(GPMSG_BEING_LEAVE);
            leaveMsg.writeShort(oid);
            gameHandler->sendTo(p, leaveMsg);
            continue;
        }

        if (!wereInRange)
        {
            // o is now visible by p. Send enter message.
            flags |= MOVING_POSITION;
            flags |= MOVING_DESTINATION;

            MessageOut enterMsg(GPMSG_BEING_ENTER);
            enterMsg.writeByte(otype);
            enterMsg.writeShort(oid);
            enterMsg.writeByte(static_cast< Being *>(o)->getAction());
            enterMsg.writeShort(opos.x); // aren't these two lines redundand considering
            enterMsg.writeShort(opos.y); // that a MOVING_POSITION message is following?
            switch (otype)
            {
                case OBJECT_CHARACTER:
                {
                    Character *q = static_cast< Character * >(o);
                    enterMsg.writeString(q->getName());
                    enterMsg.writeByte(q->getHairStyle());
                    enterMsg.writeByte(q->getHairColor());
                    enterMsg.writeByte(q->getGender());
                    serializeLooks(q, enterMsg, true);
                } break;

                case OBJECT_MONSTER:
                {
                    Monster *q = static_cast< Monster * >(o);
                    enterMsg.writeShort(q->getSpecy()->getType());
                    enterMsg.writeString(q->getName());
                } break;

                case OBJECT_NPC:
                {
                    NPC *q = static_cast< NPC * >(o);
                    enterMsg.writeShort(q->getNPC());
                    enterMsg.writeString(q->getName());
                } break;

                default:
                    assert(false); // TODO
            }
            gameHandler->sendTo(p, enterMsg);
        }

        /* At this point, either o has entered p's range, either o is
           moving inside p's range. Report o's movements. */

        Point odst = o->getDestination();
        if (opos != odst)
        {
            flags |= MOVING_POSITION;
            if (oflags & UPDATEFLAG_NEW_DESTINATION)
            {
                flags |= MOVING_DESTINATION;
            }
        }
        else
        {
            // No need to synchronize on the very last step.
            flags |= MOVING_DESTINATION;
        }

        // Send move messages.
        moveMsg.writeShort(oid);
        moveMsg.writeByte(flags);
        if (flags & MOVING_POSITION)
        {
            moveMsg.writeCoordinates(opos.x / 32, opos.y / 32);
            moveMsg.writeByte(o->getSpeed() / 10);
        }
        if (flags & MOVING_DESTINATION)
        {
            moveMsg.writeShort(odst.x);
            moveMsg.writeShort(odst.y);
        }
    }

    // Do not send a packet if nothing happened in p's range.
    if (moveMsg.getLength() > 2)
        gameHandler->sendTo(p, moveMsg);

    if (damageMsg.getLength() > 2)
        gameHandler->sendTo(p, damageMsg);

    // Inform client about status change.
    p->sendStatus();

    // Inform client about items on the ground around its character
    MessageOut itemMsg(GPMSG_ITEMS);
    for (FixedObjectIterator i(map->getAroundCharacterIterator(p, AROUND_AREA)); i; ++i)
    {
        assert((*i)->getType() == OBJECT_ITEM);
        Item *o = static_cast< Item * >(*i);
        Point opos = o->getPosition();
        int oflags = o->getUpdateFlags();
        bool willBeInRange = ppos.inRangeOf(opos, AROUND_AREA);
        bool wereInRange = pold.inRangeOf(opos, AROUND_AREA) &&
                           !((pflags | oflags) & UPDATEFLAG_NEW_ON_MAP);

        if (willBeInRange ^ wereInRange)
        {
            if (oflags & UPDATEFLAG_NEW_ON_MAP)
            {
                /* Send a specific message to the client when an item appears
                   out of nowhere, so that a sound/animation can be performed. */
                MessageOut appearMsg(GPMSG_ITEM_APPEAR);
                appearMsg.writeShort(o->getItemClass()->getDatabaseID());
                appearMsg.writeShort(opos.x);
                appearMsg.writeShort(opos.y);
                gameHandler->sendTo(p, appearMsg);
            }
            else
            {
                itemMsg.writeShort(willBeInRange ? o->getItemClass()->getDatabaseID() : 0);
                itemMsg.writeShort(opos.x);
                itemMsg.writeShort(opos.y);
            }
        }
    }

    // Do not send a packet if nothing happened in p's range.
    if (itemMsg.getLength() > 2)
        gameHandler->sendTo(p, itemMsg);
}

#ifndef NDEBUG
static bool dbgLockObjects;
#endif

void GameState::update()
{
#   ifndef NDEBUG
    dbgLockObjects = true;
#   endif

    // Update game state (update AI, etc.)
    MapManager::Maps const &maps = MapManager::getMaps();
    for (MapManager::Maps::const_iterator m = maps.begin(), m_end = maps.end(); m != m_end; ++m)
    {
        MapComposite *map = m->second;
        if (!map->isActive())
        {
            continue;
        }

        updateMap(map);

        for (CharacterIterator p(map->getWholeMapIterator()); p; ++p)
        {
            informPlayer(map, *p);
        }

        for (ObjectIterator i(map->getWholeMapIterator()); i; ++i)
        {
            Object *o = *i;
            o->clearUpdateFlags();
            if (o->canFight())
            {
                static_cast< Being * >(o)->clearHitsTaken();
            }
        }
    }

#   ifndef NDEBUG
    dbgLockObjects = false;
#   endif

    // Take care of events that were delayed because of their side effects.
    for (DelayedEvents::iterator i = delayedEvents.begin(),
         i_end = delayedEvents.end(); i != i_end; ++i)
    {
        DelayedEvent const &e = i->second;
        Object *o = i->first;
        switch (e.type)
        {
            case EVENT_REMOVE:
                remove(o);
                if (o->getType() == OBJECT_CHARACTER)
                {
                    Character *ch = static_cast< Character * >(o);
                    ch->disconnected();
                    gameHandler->kill(ch);
                }
                delete o;
                break;

            case EVENT_INSERT:
                insertSafe(o);
                break;

            case EVENT_WARP:
                assert(o->getType() == OBJECT_CHARACTER);
                warp(static_cast< Character * >(o), e.map, e.x, e.y);
                break;
        }
    }
    delayedEvents.clear();
}

bool GameState::insert(Thing *ptr)
{
    assert(!dbgLockObjects);
    MapComposite *map = ptr->getMap();
    assert(map && map->isActive());

    /* Non-visible objects have neither position nor public ID, so their
       insertion cannot fail. Take care of them first. */
    if (!ptr->isVisible())
    {
        map->insert(ptr);
        ptr->inserted();
        return true;
    }

    // Check that coordinates are actually valid.
    Object *obj = static_cast< Object * >(ptr);
    Map *mp = map->getMap();
    Point pos = obj->getPosition();
    if (pos.x / 32 >= (unsigned)mp->getWidth() ||
        pos.y / 32 >= (unsigned)mp->getHeight())
    {
        LOG_ERROR("Tried to insert an object at position " << pos.x << ','
                  << pos.y << " outside map " << map->getID() << '.');
        // Set an arbitrary small position.
        pos = Point(100, 100);
        obj->setPosition(pos);
    }

    if (!map->insert(obj))
    {
        // The map is overloaded. No room to add a new object.
        LOG_ERROR("Too many objects on map " << map->getID() << '.');
        return false;
    }

    obj->inserted();

    obj->raiseUpdateFlags(UPDATEFLAG_NEW_ON_MAP);
    if (obj->getType() != OBJECT_CHARACTER) return true;

    /* Since the player does not know yet where in the world its character is,
       we send a map-change message, even if it is the first time it
       connects to this server. */
    MessageOut mapChangeMessage(GPMSG_PLAYER_MAP_CHANGE);
    mapChangeMessage.writeString(map->getName());
    mapChangeMessage.writeShort(pos.x);
    mapChangeMessage.writeShort(pos.y);
    gameHandler->sendTo(static_cast< Character * >(obj), mapChangeMessage);
    return true;
}

bool GameState::insertSafe(Thing *ptr)
{
    if (insert(ptr)) return true;
    delete ptr;
    return false;
}

void GameState::remove(Thing *ptr)
{
    assert(!dbgLockObjects);
    MapComposite *map = ptr->getMap();

    ptr->removed();

    if (ptr->canMove())
    {
        if (ptr->getType() == OBJECT_CHARACTER)
        {
            static_cast< Character * >(ptr)->cancelTransaction();
        }

        MovingObject *obj = static_cast< MovingObject * >(ptr);
        MessageOut msg(GPMSG_BEING_LEAVE);
        msg.writeShort(obj->getPublicID());
        Point objectPos = obj->getPosition();

        for (CharacterIterator p(map->getAroundObjectIterator(obj, AROUND_AREA)); p; ++p)
        {
            if (*p != obj && objectPos.inRangeOf((*p)->getPosition(), AROUND_AREA))
            {
                gameHandler->sendTo(*p, msg);
            }
        }
    }
    else if (ptr->getType() == OBJECT_ITEM)
    {
        Item *obj = static_cast< Item * >(ptr);
        Point pos = obj->getPosition();
        MessageOut msg(GPMSG_ITEMS);
        msg.writeShort(0);
        msg.writeShort(pos.x);
        msg.writeShort(pos.y);

        for (CharacterIterator p(map->getAroundObjectIterator(obj, AROUND_AREA)); p; ++p)
        {
            if (pos.inRangeOf((*p)->getPosition(), AROUND_AREA))
            {
                gameHandler->sendTo(*p, msg);
            }
        }
    }

    map->remove(ptr);
}

void GameState::warp(Character *ptr, MapComposite *map, int x, int y)
{
    remove(ptr);
    ptr->setMap(map);
    ptr->setPosition(Point(x, y));
    ptr->clearDestination();
    /* Force update of persistent data on map change, so that
       characters can respawn at the start of the map after a death or
       a disconnection. */
    accountHandler->sendCharacterData(ptr);

    if (map->isActive())
    {
        if (!insert(ptr))
        {
            ptr->disconnected();
            gameHandler->kill(ptr);
            delete ptr;
        }
    }
    else
    {
        MessageOut msg(GAMSG_REDIRECT);
        msg.writeLong(ptr->getDatabaseID());
        accountHandler->send(msg);
        gameHandler->prepareServerChange(ptr);
    }
}

/**
 * Enqueues an event. It will be executed at end of update.
 */
static void enqueueEvent(Object *ptr, DelayedEvent const &e)
{
    std::pair< DelayedEvents::iterator, bool > p =
        delayedEvents.insert(std::make_pair(ptr, e));
    // Delete events take precedence over other events.
    if (!p.second && e.type == EVENT_REMOVE)
    {
        p.first->second.type = EVENT_REMOVE;
    }
}

void GameState::enqueueInsert(Object *ptr)
{
    DelayedEvent e = { EVENT_INSERT, 0, 0, 0 };
    enqueueEvent(ptr, e);
}

void GameState::enqueueRemove(Object *ptr)
{
    DelayedEvent e = { EVENT_REMOVE, 0, 0, 0 };
    enqueueEvent(ptr, e);
}

void GameState::enqueueWarp(Character *ptr, MapComposite *m, int x, int y)
{
    DelayedEvent e = { EVENT_WARP, x, y, m };
    enqueueEvent(ptr, e);
}

void GameState::sayAround(Object *obj, std::string const &text)
{
    Point speakerPosition = obj->getPosition();

    for (CharacterIterator i(obj->getMap()->getAroundObjectIterator(obj, AROUND_AREA)); i; ++i)
    {
        if (speakerPosition.inRangeOf((*i)->getPosition(), AROUND_AREA))
        {
            sayTo(*i, obj, text);
        }
    }
}

void GameState::sayTo(Object *destination, Object *source, std::string const &text)
{
    if (destination->getType() != OBJECT_CHARACTER) return; //only characters will read it anyway

    MessageOut msg(GPMSG_SAY);
    if (source == NULL) {
        msg.writeShort(0);
    } else if (!source->canMove()) {
        msg.writeShort(65535);
    } else {
        msg.writeShort(static_cast< MovingObject * >(source)->getPublicID());
    }
    msg.writeString(text);

    gameHandler->sendTo(static_cast< Character * >(destination), msg);
}
