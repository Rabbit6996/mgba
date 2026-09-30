/* Copyright (c) 2013-2026 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GBA_SIO_NETLINK_H
#define GBA_SIO_NETLINK_H

#include <mgba-util/common.h>

CXX_GUARD_START

#include <mgba/core/timing.h>
#include <mgba/internal/gba/sio.h>
#include <mgba-util/socket.h>

/*
 * Two-player link cable emulation over TCP.
 *
 * One side hosts (player 1, the clock owner in multiplayer mode) and the other
 * side joins (player 2). All socket calls happen on one small helper thread;
 * the emulation thread only exchanges messages with it through queues, so a
 * slow or misbehaving network can stall a game but never freeze it.
 *
 * Synchronization is deliberately looser than the local lockstep driver:
 *  - A transfer started by the host is sent to the partner right away and the
 *    host only blocks when the transfer completes, if the reply has not
 *    arrived yet. The partner answers with whatever it currently has queued in
 *    its send register, which is what games such as Pokemon Ruby/Sapphire/
 *    Emerald/FireRed/LeafGreen expect from a real cable.
 *  - The partner replays the host's timing: it executes the transfer as many
 *    cycles after its previous transfer as the host waited, so its game gets
 *    the same time to queue the next word as it would on hardware.
 *  - While both sides are in the same serial mode, neither side may run more
 *    than two frames of emulated time ahead of the other, so games do not time
 *    out waiting for their partner. If the partner pauses its emulator, this
 *    side stalls instead of breaking the session.
 */

#define GBA_NETLINK_DEFAULT_PORT 5738
#define GBA_NETLINK_PROTOCOL_VERSION 1
#define GBA_NETLINK_MESSAGE_SIZE 16
#define GBA_NETLINK_ERROR_LENGTH 64

enum GBASIONetLinkState {
	GBA_NETLINK_IDLE = 0,
	GBA_NETLINK_LISTENING,
	GBA_NETLINK_HANDSHAKE,
	GBA_NETLINK_CONNECTED,
	GBA_NETLINK_ERROR,
};

enum GBASIONetLinkWaitReason {
	GBA_NETLINK_WAIT_PARTNER_BUSY = 0,
	GBA_NETLINK_WAIT_PARTNER_PAUSED,
};

struct GBASIONetLink;
// Called roughly every 50 ms while emulation is stalled waiting for the
// partner for more than a quarter second. Frontends can draw a notice and poll
// input here; returning false disconnects the link.
typedef bool (*GBASIONetLinkWaitCallback)(struct GBASIONetLink*, void* context, enum GBASIONetLinkWaitReason reason, int64_t waitedMicros);

struct GBASIONetLinkMessage {
	uint8_t type;
	uint8_t arg8;
	uint16_t arg16;
	uint32_t data;
	uint32_t extra;
	uint32_t extra2;
};

struct GBASIONetLinkStats {
	uint32_t transfers;
	uint32_t replyWaits;
	uint32_t driftWaits;
	uint32_t missedReplies;
	uint64_t waitMicros;
};

struct GBASIONetLink {
	struct GBASIODriver d;
	struct mTimingEvent event;
	bool attached;

	enum GBASIONetLinkState state;
	// Sockets live on a helper thread (see netlink.c) so emulation never
	// makes a socket call that could block.
	struct GBASIONetLinkIO* io;
	bool helloSent;
	int playerId;
	char error[GBA_NETLINK_ERROR_LENGTH];
	char localGame[5];
	char peerGame[5];
	int64_t lastReceive;
	int timeoutMs;
	int64_t connectStart;

	GBASIONetLinkWaitCallback waitCallback;
	void* waitContext;

	uint8_t rxBuffer[GBA_NETLINK_MESSAGE_SIZE * 32];
	size_t rxFill;

	int localMode;
	int peerMode;
	bool localPaused;
	bool peerPaused;

	uint16_t nextSeq;
	bool awaitingReply;
	uint16_t pendingSeq;
	bool replyReceived;
	uint32_t replyData;
	uint32_t sentData;
	int sentMode;
	bool finishValid;
	int32_t lastFinishTime;

	bool remoteTransfer;
	int remoteMode;
	bool completionValid;
	int32_t expectedCompletion;
	bool remotePending;
	struct GBASIONetLinkMessage remoteMessage;
	struct mTimingEvent remoteEvent;
	uint16_t multiData[4];
	uint32_t normalData;

	bool engaged;
	int32_t lastEventTime;
	int32_t quantumCycles;
	uint32_t localQuanta;
	uint32_t peerQuanta;

	struct GBASIONetLinkStats stats;
};

void GBASIONetLinkCreate(struct GBASIONetLink* link);
void GBASIONetLinkDestroy(struct GBASIONetLink* link);

// Start waiting for a partner. The link must be attached to a core before the
// partner can exchange data, but connecting works with or without a core.
bool GBASIONetLinkHost(struct GBASIONetLink* link, uint16_t port);
// Connect to a hosting partner. This does not block: the connection and the
// handshake finish in GBASIONetLinkUpdate (or during emulation).
bool GBASIONetLinkConnect(struct GBASIONetLink* link, const char* address, uint16_t port);
void GBASIONetLinkDisconnect(struct GBASIONetLink* link);

// Service the connection outside of emulation, e.g. from a "waiting for
// partner" screen. Waits at most timeoutMs for network activity.
void GBASIONetLinkUpdate(struct GBASIONetLink* link, int timeoutMs);

// Tell the partner that emulation is paused (menus, etc.) so it does not wait
// on us. Call with false before emulation resumes.
void GBASIONetLinkSetPaused(struct GBASIONetLink* link, bool paused);

// Make sure the periodic driver event is scheduled. Loading a savestate clears
// all timing events, so frontends should call this once per frame.
void GBASIONetLinkEnsureScheduled(struct GBASIONetLink* link);

enum GBASIONetLinkState GBASIONetLinkGetState(const struct GBASIONetLink* link);
const char* GBASIONetLinkGetError(const struct GBASIONetLink* link);

// Diagnostics: append timestamped lines to a file. Each line is written with
// its own open/close so it survives the system being switched off hard.
void GBASIONetLinkSetLogFile(const char* path);
void GBASIONetLinkLog(const char* format, ...);
// The most recent line logged, for showing on screen
const char* GBASIONetLinkLastLog(void);

bool GBASIONetLinkParseAddress(const char* text, struct Address* address);
bool GBASIONetLinkGetLocalAddress(char* out, size_t outLength);

CXX_GUARD_END

#endif
