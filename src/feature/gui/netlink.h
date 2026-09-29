/* Copyright (c) 2013-2026 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GUI_NETLINK_H
#define GUI_NETLINK_H

#include <mgba-util/common.h>

CXX_GUARD_START

struct mGUIRunner;

// Link cable over Wi-Fi for the GBA core in GUI frontends (3DS, etc.).
// All functions are safe to call when the feature is compiled out or the
// loaded game is not a GBA game; they then do nothing.
bool mGUINetLinkAvailable(struct mGUIRunner*);
void mGUINetLinkShowMenu(struct mGUIRunner*);
void mGUINetLinkFrame(struct mGUIRunner*);
void mGUINetLinkSetMenuPaused(struct mGUIRunner*, bool paused);
void mGUINetLinkSetSystemPaused(struct mGUIRunner*, bool paused);
bool mGUINetLinkWantsOSD(struct mGUIRunner*);
bool mGUINetLinkDrawStatus(struct mGUIRunner*);
void mGUINetLinkGameUnloading(struct mGUIRunner*);

CXX_GUARD_END

#endif
