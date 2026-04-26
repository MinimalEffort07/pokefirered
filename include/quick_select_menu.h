#ifndef GUARD_QUICK_SELECT_MENU_H
#define GUARD_QUICK_SELECT_MENU_H

#include "global.h"

/*
 * Quick Select Menu
 *
 * Replaces the single registered-item system. Pressing SELECT in the
 * overworld opens a heptagonal (7-position) radial overlay with two pages:
 *
 *   Page 0 — HM moves:  shows the 7 usable HMs (Flash, Cut, Fly, Strength,
 *            Surf, Rock Smash, Waterfall).  Each slot is bright if the player
 *            owns the HM item AND has the required badge, greyed otherwise.
 *            Using an HM no longer requires teaching it to a party pokemon;
 *            the lead pokemon performs the field animation.
 *
 *   Page 1 — Registered items:  up to 7 items registered from the bag menu.
 *
 * Navigation:  D-pad moves between heptagon positions, A confirms, B closes,
 * R switches page.
 */

/* REGISTERED_ITEMS_MAX is defined in global.h (next to the save struct
 * field it sizes) so that global.h doesn't need to include this header. */

bool8 OpenQuickSelectMenu(void);
bool8 QuickSelect_IsItemRegistered(u16 itemId);
bool8 QuickSelect_HasEmptyItemSlot(void);
void QuickSelect_RegisterItem(u16 itemId);
void QuickSelect_UnregisterItem(u16 itemId);
void Special_SetSlavelessHmSilhouette(void);

#endif /* GUARD_QUICK_SELECT_MENU_H */
