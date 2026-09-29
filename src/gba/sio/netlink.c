/* Copyright (c) 2013-2026 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gba/sio/netlink.h>

#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/io.h>

#ifndef _MSC_VER
#include <sys/time.h>
#endif
#if !defined(_WIN32) && !defined(GEKKO)
#include <poll.h>
#define NETLINK_USE_POLL
#endif

#define DRIVER_ID 0x4B4C4E6D // "mNLK"
#define NETLINK_MAGIC 0x4B4C4E6D

// Emulated cycles between drift checkpoints (a quarter of a frame)
#define TICK_INTERVAL (VIDEO_TOTAL_LENGTH / 4)
// How many checkpoints one side may run ahead of the other (two frames)
#define MAX_LEAD 8

// How often the driver looks at the network, in emulated cycles
#define POLL_SECONDARY 4096
#define POLL_PRIMARY 8192
#define POLL_CONNECTED 32768
#define POLL_WAITING (VIDEO_TOTAL_LENGTH / 2)
#define POLL_IDLE VIDEO_TOTAL_LENGTH

#define NO_GAP 0xFFFFFFFF

#define DEFAULT_TIMEOUT_MS 30000
#define PAUSED_TIMEOUT_MS (10 * 60 * 1000)
#define WAIT_NOTICE_US 250000
#define WAIT_CALLBACK_US 50000
#define HANDSHAKE_TIMEOUT_MS 10000
#define CONNECT_TIMEOUT_MS 10000

enum {
	MSG_HELLO = 1,
	MSG_MODE = 2,
	MSG_XFER = 3,
	MSG_REPLY = 4,
	MSG_TICK = 5,
	MSG_BYE = 6,
};

static bool _driverInit(struct GBASIODriver* driver);
static void _driverDeinit(struct GBASIODriver* driver);
static void _driverReset(struct GBASIODriver* driver);
static uint32_t _driverId(const struct GBASIODriver* driver);
static void _driverSetMode(struct GBASIODriver* driver, enum GBASIOMode mode);
static bool _driverHandlesMode(struct GBASIODriver* driver, enum GBASIOMode mode);
static int _driverConnectedDevices(struct GBASIODriver* driver);
static int _driverDeviceId(struct GBASIODriver* driver);
static uint16_t _driverWriteSIOCNT(struct GBASIODriver* driver, uint16_t value);
static uint16_t _driverWriteRCNT(struct GBASIODriver* driver, uint16_t value);
static bool _driverStart(struct GBASIODriver* driver);
static void _driverFinishMultiplayer(struct GBASIODriver* driver, uint16_t data[4]);
static uint8_t _driverFinishNormal8(struct GBASIODriver* driver);
static uint32_t _driverFinishNormal32(struct GBASIODriver* driver);

static void _event(struct mTiming* timing, void* context, uint32_t cyclesLate);
static void _service(struct GBASIONetLink* link, int timeoutMs);

static int64_t _nowMicros(void) {
#ifndef _MSC_VER
	struct timeval tv;
	gettimeofday(&tv, 0);
	return tv.tv_sec * 1000000LL + tv.tv_usec;
#else
	struct timespec ts;
	timespec_get(&ts, TIME_UTC);
	return ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
#endif
}

// Don't rely on O_NONBLOCK alone: on some systems (e.g. the 3DS) a socket
// can end up blocking anyway, which would freeze emulation. Only read once the
// socket says data is there, and ask for a non-blocking read on top.
static bool _socketReady(Socket sock, bool write, int timeoutMs) {
	if (SOCKET_FAILED(sock)) {
		return false;
	}
#ifdef NETLINK_USE_POLL
	struct pollfd pfd;
	memset(&pfd, 0, sizeof(pfd));
	pfd.fd = sock;
	pfd.events = write ? POLLOUT : POLLIN;
	int result = poll(&pfd, 1, timeoutMs);
	if (result <= 0) {
		return false;
	}
	return pfd.revents & (pfd.events | POLLHUP | POLLERR);
#else
	Socket ready = sock;
	int result;
	if (write) {
		result = SocketPoll(1, NULL, &ready, NULL, timeoutMs);
	} else {
		result = SocketPoll(1, &ready, NULL, NULL, timeoutMs);
	}
	return result > 0 && !SOCKET_FAILED(ready);
#endif
}

static ssize_t _recv(Socket sock, void* buffer, size_t size) {
#ifdef NETLINK_USE_POLL
	return recv(sock, buffer, size, MSG_DONTWAIT);
#else
	return SocketRecv(sock, buffer, size);
#endif
}

static uint8_t _encodeMode(int mode) {
	if (mode < 0 || mode > 0xFE) {
		return 0xFF;
	}
	return mode;
}

static int _decodeMode(uint8_t mode) {
	if (mode == 0xFF) {
		return -1;
	}
	return mode;
}

static bool _isDataMode(int mode) {
	return mode == GBA_SIO_MULTI || mode == GBA_SIO_NORMAL_8 || mode == GBA_SIO_NORMAL_32;
}

static struct GBASIO* _sio(struct GBASIONetLink* link) {
	if (!link->attached) {
		return NULL;
	}
	return link->d.p;
}

static void _closeSockets(struct GBASIONetLink* link) {
	if (!SOCKET_FAILED(link->sock)) {
		SocketClose(link->sock);
		link->sock = INVALID_SOCKET;
	}
	if (!SOCKET_FAILED(link->listener)) {
		SocketClose(link->listener);
		link->listener = INVALID_SOCKET;
	}
	link->rxFill = 0;
}

static void _executeRemoteTransfer(struct GBASIONetLink* link, const struct GBASIONetLinkMessage* message);

static void _resetSession(struct GBASIONetLink* link) {
	if (link->remotePending) {
		// The partner's data already arrived, so let the transfer complete on
		// our side even though the partner will never see our answer.
		link->remotePending = false;
		if (link->attached && link->d.p) {
			mTimingDeschedule(&link->d.p->p->timing, &link->remoteEvent);
			_executeRemoteTransfer(link, &link->remoteMessage);
		}
	}
	link->peerMode = -1;
	link->peerPaused = false;
	link->peerGame[0] = '\0';
	link->awaitingReply = false;
	link->replyReceived = false;
	link->completionValid = false;
	link->finishValid = false;
	link->engaged = false;
	link->quantumCycles = 0;
	link->localQuanta = 0;
	link->peerQuanta = 0;
}

static void _updateSioBits(struct GBASIONetLink* link) {
	struct GBASIO* sio = _sio(link);
	if (!sio || sio->mode != GBA_SIO_MULTI) {
		return;
	}
	bool connected = link->state == GBA_NETLINK_CONNECTED;
	int id = connected ? link->playerId : 0;
	bool ready = true;
	if (connected) {
		ready = link->peerMode == GBA_SIO_MULTI;
	}
	sio->siocnt = GBASIOMultiplayerSetId(sio->siocnt, id);
	sio->siocnt = GBASIOMultiplayerSetSlave(sio->siocnt, id || !connected);
	sio->siocnt = GBASIOMultiplayerSetReady(sio->siocnt, ready);
	sio->rcnt = GBASIORegisterRCNTSetSd(sio->rcnt, ready);
	sio->rcnt = GBASIORegisterRCNTSetSi(sio->rcnt, !!id);
}

static void _updateEngaged(struct GBASIONetLink* link) {
	bool engaged = link->state == GBA_NETLINK_CONNECTED &&
	               link->attached &&
	               link->localMode == link->peerMode &&
	               _isDataMode(link->localMode);
	if (engaged && !link->engaged) {
		link->quantumCycles = 0;
		link->localQuanta = 0;
		link->peerQuanta = 0;
	}
	link->engaged = engaged;
}

static void _fail(struct GBASIONetLink* link, const char* reason) {
	mLOG(GBA_SIO, WARN, "Network link: %s", reason);
	_closeSockets(link);
	link->state = GBA_NETLINK_ERROR;
	strncpy(link->error, reason, sizeof(link->error) - 1);
	link->error[sizeof(link->error) - 1] = '\0';
	_resetSession(link);
	_updateSioBits(link);
}

static void _failCode(struct GBASIONetLink* link, const char* reason, int code) {
	char text[GBA_NETLINK_ERROR_LENGTH];
	snprintf(text, sizeof(text), "%s (code %d)", reason, code);
	_fail(link, text);
}

static void _encode(const struct GBASIONetLinkMessage* message, uint8_t* buffer) {
	buffer[0] = message->type;
	buffer[1] = message->arg8;
	STORE_16LE(message->arg16, 2, buffer);
	STORE_32LE(message->data, 4, buffer);
	STORE_32LE(message->extra, 8, buffer);
	STORE_32LE(message->extra2, 12, buffer);
}

static void _decode(const uint8_t* buffer, struct GBASIONetLinkMessage* message) {
	message->type = buffer[0];
	message->arg8 = buffer[1];
	LOAD_16LE(message->arg16, 2, buffer);
	LOAD_32LE(message->data, 4, buffer);
	LOAD_32LE(message->extra, 8, buffer);
	LOAD_32LE(message->extra2, 12, buffer);
}

static bool _sendFull(struct GBASIONetLink* link, uint8_t type, uint8_t arg8, uint16_t arg16, uint32_t data, uint32_t extra, uint32_t extra2) {
	if (SOCKET_FAILED(link->sock)) {
		return false;
	}
	struct GBASIONetLinkMessage message = {
		.type = type,
		.arg8 = arg8,
		.arg16 = arg16,
		.data = data,
		.extra = extra,
		.extra2 = extra2,
	};
	uint8_t buffer[GBA_NETLINK_MESSAGE_SIZE];
	_encode(&message, buffer);

	size_t sent = 0;
	int64_t start = _nowMicros();
	while (sent < sizeof(buffer)) {
		ssize_t result = SocketSend(link->sock, &buffer[sent], sizeof(buffer) - sent);
		if (result > 0) {
			sent += result;
			continue;
		}
		if (result < 0 && SocketWouldBlock()) {
			if (_nowMicros() - start > link->timeoutMs * 1000LL) {
				_fail(link, "Sending to partner timed out");
				return false;
			}
			_socketReady(link->sock, true, 20);
			continue;
		}
		_fail(link, "Lost connection to partner");
		return false;
	}
	return true;
}

static bool _send(struct GBASIONetLink* link, uint8_t type, uint8_t arg8, uint16_t arg16, uint32_t data, uint32_t extra) {
	return _sendFull(link, type, arg8, arg16, data, extra, 0);
}

static void _sendMode(struct GBASIONetLink* link) {
	if (link->state != GBA_NETLINK_CONNECTED) {
		return;
	}
	int mode = link->attached ? link->localMode : -1;
	_send(link, MSG_MODE, _encodeMode(mode), link->localPaused, 0, 0);
}

static void _sendHello(struct GBASIONetLink* link) {
	uint32_t game;
	LOAD_32LE(game, 0, link->localGame);
	_send(link, MSG_HELLO, GBA_NETLINK_PROTOCOL_VERSION, link->playerId, NETLINK_MAGIC, game);
}

static void _prepareSocket(Socket sock) {
	SocketSetBlocking(sock, false);
	SocketSetTCPPush(sock, 1);
}

static void _handleHello(struct GBASIONetLink* link, const struct GBASIONetLinkMessage* message) {
	if (link->state != GBA_NETLINK_HANDSHAKE) {
		return;
	}
	if (message->data != NETLINK_MAGIC) {
		_fail(link, "Partner is not an mGBA link");
		return;
	}
	if (message->arg8 != GBA_NETLINK_PROTOCOL_VERSION) {
		_fail(link, "Partner uses a different mGBA link version");
		return;
	}
	if (message->arg16 == link->playerId) {
		_fail(link, "Both sides picked the same player");
		return;
	}
	_resetSession(link);
	size_t i;
	STORE_32LE(message->extra, 0, link->peerGame);
	for (i = 0; i < 4; ++i) {
		if (link->peerGame[i] < 0x20 || link->peerGame[i] > 0x7E) {
			link->peerGame[i] = '?';
		}
	}
	link->peerGame[4] = '\0';
	link->state = GBA_NETLINK_CONNECTED;
	link->error[0] = '\0';
	mLOG(GBA_SIO, INFO, "Network link established as player %i (partner game %s)", link->playerId + 1, link->peerGame);
	_sendMode(link);
	_updateSioBits(link);
	_updateEngaged(link);
}

static void _executeRemoteTransfer(struct GBASIONetLink* link, const struct GBASIONetLinkMessage* message) {
	int mode = _decodeMode(message->arg8);
	uint32_t reply = 0xFFFFFFFF;
	struct GBASIO* sio = _sio(link);
	if (sio && (int) sio->mode == mode && _isDataMode(mode)) {
		struct GBA* gba = sio->p;
		if (mTimingIsScheduled(&gba->timing, &sio->completeEvent)) {
			// The previous transfer has not completed on our side yet. Finish
			// it now so its data is not lost.
			mTimingDeschedule(&gba->timing, &sio->completeEvent);
			sio->completeEvent.callback(&gba->timing, sio, 0);
		}
		switch (mode) {
		case GBA_SIO_MULTI:
			reply = gba->memory.io[GBA_REG(SIOMLT_SEND)];
			link->multiData[0] = message->data;
			link->multiData[1] = reply;
			link->multiData[2] = 0xFFFF;
			link->multiData[3] = 0xFFFF;
			sio->siocnt = GBASIOMultiplayerFillBusy(sio->siocnt);
			sio->rcnt = GBASIORegisterRCNTClearSc(sio->rcnt);
			break;
		case GBA_SIO_NORMAL_8:
			reply = gba->memory.io[GBA_REG(SIODATA8)] & 0xFF;
			link->normalData = message->data & 0xFF;
			sio->siocnt = GBASIONormalFillStart(sio->siocnt);
			break;
		case GBA_SIO_NORMAL_32:
			reply = gba->memory.io[GBA_REG(SIODATA32_LO)];
			reply |= gba->memory.io[GBA_REG(SIODATA32_HI)] << 16;
			link->normalData = message->data;
			sio->siocnt = GBASIONormalFillStart(sio->siocnt);
			break;
		}
		link->remoteTransfer = true;
		link->remoteMode = mode;
		int32_t cycles = message->extra;
		if (cycles <= 0 || cycles > VIDEO_TOTAL_LENGTH) {
			cycles = GBASIOTransferCycles(mode, sio->siocnt, 1);
		}
		mTimingSchedule(&gba->timing, &sio->completeEvent, cycles);
		link->expectedCompletion = mTimingCurrentTime(&gba->timing) + cycles;
		link->completionValid = true;
		++link->stats.transfers;
	}
	_send(link, MSG_REPLY, _encodeMode(mode), message->arg16, reply, 0);
}

static void _flushRemoteTransfer(struct GBASIONetLink* link) {
	if (!link->remotePending) {
		return;
	}
	link->remotePending = false;
	struct GBASIO* sio = _sio(link);
	if (sio) {
		mTimingDeschedule(&sio->p->timing, &link->remoteEvent);
	}
	_executeRemoteTransfer(link, &link->remoteMessage);
}

static void _dropRemoteTransfer(struct GBASIONetLink* link) {
	if (!link->remotePending) {
		return;
	}
	link->remotePending = false;
	struct GBASIO* sio = _sio(link);
	if (sio) {
		mTimingDeschedule(&sio->p->timing, &link->remoteEvent);
	}
	if (link->state == GBA_NETLINK_CONNECTED) {
		_send(link, MSG_REPLY, link->remoteMessage.arg8, link->remoteMessage.arg16, 0xFFFFFFFF, 0);
	}
}

static void _remoteEvent(struct mTiming* timing, void* context, uint32_t cyclesLate) {
	UNUSED(timing);
	UNUSED(cyclesLate);
	struct GBASIONetLink* link = context;
	if (!link->remotePending) {
		return;
	}
	link->remotePending = false;
	_executeRemoteTransfer(link, &link->remoteMessage);
}

static void _scheduleRemoteTransfer(struct GBASIONetLink* link, const struct GBASIONetLinkMessage* message) {
	struct GBASIO* sio = _sio(link);
	// The host tells us how long it waited after the previous transfer finished
	// before starting this one. Replay that gap on our own timeline so our game
	// gets the same time to react (e.g. queue the next word) as on hardware.
	if (sio && link->completionValid && message->extra2 != NO_GAP) {
		struct mTiming* timing = &sio->p->timing;
		int32_t target = link->expectedCompletion + (int32_t) message->extra2;
		int32_t delta = target - mTimingCurrentTime(timing);
		if (delta > 0 && delta <= VIDEO_TOTAL_LENGTH) {
			link->remoteMessage = *message;
			link->remotePending = true;
			mTimingDeschedule(timing, &link->remoteEvent);
			mTimingSchedule(timing, &link->remoteEvent, delta);
			return;
		}
	}
	_executeRemoteTransfer(link, message);
}

static void _resumeRemoteTransfer(struct GBASIONetLink* link) {
	// Reschedule a transfer that is pending but lost its timing event (it was
	// held while paused, or a savestate load cleared the event queue)
	struct GBASIO* sio = _sio(link);
	if (!link->remotePending || (sio && mTimingIsScheduled(&sio->p->timing, &link->remoteEvent))) {
		return;
	}
	struct GBASIONetLinkMessage message = link->remoteMessage;
	link->remotePending = false;
	_scheduleRemoteTransfer(link, &message);
}

static void _handleRemoteTransfer(struct GBASIONetLink* link, const struct GBASIONetLinkMessage* message) {
	_flushRemoteTransfer(link);
	if (link->localPaused) {
		// Emulation is paused (e.g. a menu is open); hold the transfer until it
		// resumes. The host keeps waiting for our reply in the meantime.
		link->remoteMessage = *message;
		link->remotePending = true;
		return;
	}
	_scheduleRemoteTransfer(link, message);
}

static void _dispatch(struct GBASIONetLink* link, const struct GBASIONetLinkMessage* message) {
	switch (message->type) {
	case MSG_HELLO:
		_handleHello(link, message);
		break;
	case MSG_MODE:
		if (link->state != GBA_NETLINK_CONNECTED) {
			break;
		}
		link->peerMode = _decodeMode(message->arg8);
		link->peerPaused = message->arg16 & 1;
		mLOG(GBA_SIO, DEBUG, "Network link: partner mode %i%s", link->peerMode, link->peerPaused ? " (paused)" : "");
		_updateSioBits(link);
		_updateEngaged(link);
		break;
	case MSG_XFER:
		if (link->state != GBA_NETLINK_CONNECTED || link->playerId == 0) {
			break;
		}
		_handleRemoteTransfer(link, message);
		break;
	case MSG_REPLY:
		if (link->state != GBA_NETLINK_CONNECTED || link->playerId != 0) {
			break;
		}
		if (link->awaitingReply && message->arg16 == link->pendingSeq) {
			link->replyReceived = true;
			link->replyData = message->data;
		} else {
			++link->stats.missedReplies;
		}
		break;
	case MSG_TICK:
		if (link->state == GBA_NETLINK_CONNECTED && message->arg8 && link->engaged) {
			link->peerQuanta = message->data;
		}
		break;
	case MSG_BYE:
		_fail(link, "Partner disconnected");
		break;
	default:
		_fail(link, "Received garbage from partner");
		break;
	}
}

static void _pump(struct GBASIONetLink* link, int timeoutMs) {
	if (!_socketReady(link->sock, false, timeoutMs > 0 ? timeoutMs : 0)) {
		return;
	}
	while (!SOCKET_FAILED(link->sock)) {
		ssize_t received = _recv(link->sock, &link->rxBuffer[link->rxFill], sizeof(link->rxBuffer) - link->rxFill);
		if (received == 0) {
			_fail(link, "Partner disconnected");
			return;
		}
		if (received < 0) {
			if (!SocketWouldBlock()) {
				_failCode(link, "Lost connection to partner", SocketError());
			}
			return;
		}
		link->rxFill += received;
		link->lastReceive = _nowMicros();

		size_t offset = 0;
		while (link->rxFill - offset >= GBA_NETLINK_MESSAGE_SIZE) {
			struct GBASIONetLinkMessage message;
			_decode(&link->rxBuffer[offset], &message);
			offset += GBA_NETLINK_MESSAGE_SIZE;
			_dispatch(link, &message);
			if (SOCKET_FAILED(link->sock)) {
				return;
			}
		}
		if (offset) {
			memmove(link->rxBuffer, &link->rxBuffer[offset], link->rxFill - offset);
			link->rxFill -= offset;
		}
		if (!_socketReady(link->sock, false, 0)) {
			return;
		}
	}
}

static void _service(struct GBASIONetLink* link, int timeoutMs) {
	if (link->state == GBA_NETLINK_LISTENING) {
		if (!_socketReady(link->listener, false, timeoutMs)) {
			return;
		}
		Socket sock = SocketAccept(link->listener, NULL);
		if (SOCKET_FAILED(sock)) {
			return;
		}
		SocketClose(link->listener);
		link->listener = INVALID_SOCKET;
		_prepareSocket(sock);
		link->sock = sock;
		link->state = GBA_NETLINK_HANDSHAKE;
		link->lastReceive = _nowMicros();
		_sendHello(link);
		timeoutMs = 0;
	}
	if (link->state == GBA_NETLINK_HANDSHAKE && link->connecting) {
		if (_socketReady(link->sock, true, timeoutMs)) {
			int error = 0;
			socklen_t length = sizeof(error);
			if (getsockopt(link->sock, SOL_SOCKET, SO_ERROR, (char*) &error, &length) != 0) {
				// Can't tell; a failed connection will show up on the first read
				error = 0;
			}
			if (error) {
				_failCode(link, "Could not reach the host", error);
				return;
			}
			link->connecting = false;
			_prepareSocket(link->sock);
			link->lastReceive = _nowMicros();
			_sendHello(link);
			timeoutMs = 0;
		} else if (_nowMicros() - link->connectStart > CONNECT_TIMEOUT_MS * 1000LL) {
			_fail(link, "Could not reach the host");
			return;
		} else {
			return;
		}
	}
	if (link->state == GBA_NETLINK_HANDSHAKE) {
		_pump(link, timeoutMs);
		if (link->state == GBA_NETLINK_HANDSHAKE && _nowMicros() - link->lastReceive > HANDSHAKE_TIMEOUT_MS * 1000LL) {
			_fail(link, "Partner did not answer");
		}
	} else if (link->state == GBA_NETLINK_CONNECTED) {
		_pump(link, timeoutMs);
	}
}

static bool _waitFor(struct GBASIONetLink* link, bool (*done)(struct GBASIONetLink*)) {
	int64_t start = _nowMicros();
	int64_t lastCallback = start;
	while (!done(link)) {
		if (link->state != GBA_NETLINK_CONNECTED) {
			return false;
		}
		int64_t now = _nowMicros();
		int64_t lastActivity = link->lastReceive > start ? link->lastReceive : start;
		int64_t timeout = (link->peerPaused ? PAUSED_TIMEOUT_MS : link->timeoutMs) * 1000LL;
		if (now - lastActivity > timeout) {
			_fail(link, "Partner stopped responding");
			return false;
		}
		if (link->waitCallback && now - start >= WAIT_NOTICE_US && now - lastCallback >= WAIT_CALLBACK_US) {
			lastCallback = now;
			enum GBASIONetLinkWaitReason reason = link->peerPaused ? GBA_NETLINK_WAIT_PARTNER_PAUSED : GBA_NETLINK_WAIT_PARTNER_BUSY;
			if (!link->waitCallback(link, link->waitContext, reason, now - start)) {
				_send(link, MSG_BYE, 0, 0, 0, 0);
				_fail(link, "Link cancelled");
				return false;
			}
			continue;
		}
		_pump(link, 20);
	}
	link->stats.waitMicros += _nowMicros() - start;
	return true;
}

static bool _replyArrived(struct GBASIONetLink* link) {
	return link->replyReceived;
}

static bool _driftResolved(struct GBASIONetLink* link) {
	// Never block while a partner transfer is scheduled on our timeline: the
	// partner is waiting for our reply and cannot advance until it gets it.
	return !link->engaged || link->remotePending || (int32_t) (link->localQuanta - link->peerQuanta) <= MAX_LEAD;
}

static int32_t _nextInterval(struct GBASIONetLink* link) {
	switch (link->state) {
	case GBA_NETLINK_CONNECTED:
		if (link->engaged) {
			return link->playerId ? POLL_SECONDARY : POLL_PRIMARY;
		}
		return POLL_CONNECTED;
	case GBA_NETLINK_LISTENING:
	case GBA_NETLINK_HANDSHAKE:
		return POLL_WAITING;
	default:
		return POLL_IDLE;
	}
}

static void _event(struct mTiming* timing, void* context, uint32_t cyclesLate) {
	UNUSED(cyclesLate);
	struct GBASIONetLink* link = context;
	int32_t now = mTimingCurrentTime(timing);
	int32_t elapsed = now - link->lastEventTime;
	link->lastEventTime = now;
	if (elapsed < 0 || elapsed > TICK_INTERVAL * 2) {
		// Probably a savestate was loaded; don't count this period
		elapsed = 0;
	}

	_service(link, 0);
	if (link->state == GBA_NETLINK_CONNECTED) {
		_updateEngaged(link);
		if (link->engaged) {
			link->quantumCycles += elapsed;
			while (link->quantumCycles >= TICK_INTERVAL && link->state == GBA_NETLINK_CONNECTED) {
				link->quantumCycles -= TICK_INTERVAL;
				++link->localQuanta;
				_send(link, MSG_TICK, 1, 0, link->localQuanta, 0);
			}
			if (!_driftResolved(link)) {
				++link->stats.driftWaits;
				_waitFor(link, _driftResolved);
			}
		}
	}
	mTimingSchedule(timing, &link->event, _nextInterval(link));
}

void GBASIONetLinkCreate(struct GBASIONetLink* link) {
	memset(link, 0, sizeof(*link));
	link->d.init = _driverInit;
	link->d.deinit = _driverDeinit;
	link->d.reset = _driverReset;
	link->d.driverId = _driverId;
	link->d.setMode = _driverSetMode;
	link->d.handlesMode = _driverHandlesMode;
	link->d.connectedDevices = _driverConnectedDevices;
	link->d.deviceId = _driverDeviceId;
	link->d.writeSIOCNT = _driverWriteSIOCNT;
	link->d.writeRCNT = _driverWriteRCNT;
	link->d.start = _driverStart;
	link->d.finishMultiplayer = _driverFinishMultiplayer;
	link->d.finishNormal8 = _driverFinishNormal8;
	link->d.finishNormal32 = _driverFinishNormal32;
	link->event.context = link;
	link->event.callback = _event;
	link->event.name = "GBA SIO Network Link";
	link->event.priority = 0x80;
	link->remoteEvent.context = link;
	link->remoteEvent.callback = _remoteEvent;
	link->remoteEvent.name = "GBA SIO Network Link Transfer";
	link->remoteEvent.priority = 0x80;
	link->listener = INVALID_SOCKET;
	link->sock = INVALID_SOCKET;
	link->localMode = -1;
	link->timeoutMs = DEFAULT_TIMEOUT_MS;
	memcpy(link->localGame, "????", 5);
	_resetSession(link);
}

void GBASIONetLinkDestroy(struct GBASIONetLink* link) {
	GBASIONetLinkDisconnect(link);
}

bool GBASIONetLinkHost(struct GBASIONetLink* link, uint16_t port) {
	GBASIONetLinkDisconnect(link);
	link->playerId = 0;
	Socket listener = SocketOpenTCP(port, NULL);
	if (SOCKET_FAILED(listener)) {
		_failCode(link, "Could not open the link port", SocketError());
		return false;
	}
	if (SOCKET_FAILED(SocketListen(listener, 1))) {
		SocketClose(listener);
		_fail(link, "Could not listen on the link port");
		return false;
	}
	SocketSetBlocking(listener, false);
	link->listener = listener;
	link->state = GBA_NETLINK_LISTENING;
	link->error[0] = '\0';
	return true;
}

static bool _connectInProgress(void) {
#ifdef _WIN32
	return WSAGetLastError() == WSAEWOULDBLOCK;
#else
	return errno == EINPROGRESS || errno == EALREADY || errno == EWOULDBLOCK || errno == EAGAIN;
#endif
}

bool GBASIONetLinkConnect(struct GBASIONetLink* link, const char* address, uint16_t port) {
	GBASIONetLinkDisconnect(link);
	link->playerId = 1;
	struct Address destination;
	if (!GBASIONetLinkParseAddress(address, &destination)) {
		_fail(link, "Invalid address");
		return false;
	}
#if defined(GEKKO) || defined(__3DS__)
	// Plain blocking connect: non-blocking connects are not reliable here
	Socket sock = SocketConnectTCP(port, &destination);
	if (SOCKET_FAILED(sock)) {
		_failCode(link, "Could not reach the host", SocketError());
		return false;
	}
	_prepareSocket(sock);
	link->sock = sock;
	link->state = GBA_NETLINK_HANDSHAKE;
	link->error[0] = '\0';
	link->lastReceive = _nowMicros();
	_sendHello(link);
#else
	Socket sock = SocketCreate(false, IPPROTO_TCP);
	if (SOCKET_FAILED(sock)) {
		_failCode(link, "Could not create a socket", SocketError());
		return false;
	}
	SocketSetBlocking(sock, false);
	struct sockaddr_in info;
	memset(&info, 0, sizeof(info));
	info.sin_family = AF_INET;
	info.sin_port = htons(port);
	info.sin_addr.s_addr = htonl(destination.ipv4);
	int result = connect(sock, (const struct sockaddr*) &info, sizeof(info));
	link->sock = sock;
	link->state = GBA_NETLINK_HANDSHAKE;
	link->error[0] = '\0';
	link->lastReceive = _nowMicros();
	if (result == 0) {
		_prepareSocket(sock);
		_sendHello(link);
	} else if (_connectInProgress()) {
		link->connecting = true;
		link->connectStart = _nowMicros();
	} else {
		_failCode(link, "Could not reach the host", SocketError());
		return false;
	}
#endif
	return link->state == GBA_NETLINK_HANDSHAKE;
}

void GBASIONetLinkDisconnect(struct GBASIONetLink* link) {
	if (link->state == GBA_NETLINK_CONNECTED) {
		_send(link, MSG_BYE, 0, 0, 0, 0);
	}
	_closeSockets(link);
	link->state = GBA_NETLINK_IDLE;
	link->error[0] = '\0';
	_resetSession(link);
	_updateSioBits(link);
}

void GBASIONetLinkUpdate(struct GBASIONetLink* link, int timeoutMs) {
	_service(link, timeoutMs);
}

void GBASIONetLinkSetPaused(struct GBASIONetLink* link, bool paused) {
	if (link->localPaused == paused) {
		return;
	}
	if (paused) {
		// Handle whatever already arrived while emulation can still act on it.
		// A scheduled partner transfer stays scheduled; emulated time does not
		// advance while paused, so it fires at the right moment afterwards.
		_service(link, 0);
		link->localPaused = true;
		_sendMode(link);
	} else {
		link->localPaused = false;
		link->lastReceive = _nowMicros();
		_sendMode(link);
		// A transfer that arrived while we were paused was held back
		_resumeRemoteTransfer(link);
		_service(link, 0);
	}
}

void GBASIONetLinkEnsureScheduled(struct GBASIONetLink* link) {
	struct GBASIO* sio = _sio(link);
	if (!sio) {
		return;
	}
	struct mTiming* timing = &sio->p->timing;
	if (!mTimingIsScheduled(timing, &link->event)) {
		link->lastEventTime = mTimingCurrentTime(timing);
		link->awaitingReply = false;
		link->remoteTransfer = false;
		mTimingSchedule(timing, &link->event, _nextInterval(link));
	}
	if (!link->localPaused) {
		_resumeRemoteTransfer(link);
	}
}

enum GBASIONetLinkState GBASIONetLinkGetState(const struct GBASIONetLink* link) {
	return link->state;
}

const char* GBASIONetLinkGetError(const struct GBASIONetLink* link) {
	return link->error;
}

bool GBASIONetLinkParseAddress(const char* text, struct Address* address) {
	unsigned parts[4];
	int part = 0;
	unsigned value = 0;
	bool digits = false;
	const char* c;
	for (c = text; ; ++c) {
		if (*c >= '0' && *c <= '9') {
			value = value * 10 + (*c - '0');
			if (value > 255) {
				return false;
			}
			digits = true;
		} else if (*c == '.' || *c == '\0' || *c == ' ') {
			if (!digits || part >= 4) {
				return false;
			}
			parts[part] = value;
			++part;
			value = 0;
			digits = false;
			if (*c != '.') {
				break;
			}
		} else {
			return false;
		}
	}
	for (; *c == ' '; ++c);
	if (part != 4 || *c) {
		return false;
	}
	address->version = IPV4;
	address->ipv4 = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
	return true;
}

bool GBASIONetLinkGetLocalAddress(char* out, size_t outLength) {
	uint32_t ip = 0;
#ifdef __3DS__
	ip = ntohl(gethostid());
#elif !defined(GEKKO)
	Socket sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (SOCKET_FAILED(sock)) {
		return false;
	}
	struct sockaddr_in remote;
	memset(&remote, 0, sizeof(remote));
	remote.sin_family = AF_INET;
	remote.sin_port = htons(53);
	remote.sin_addr.s_addr = htonl(0x08080808);
	if (connect(sock, (const struct sockaddr*) &remote, sizeof(remote)) == 0) {
		struct sockaddr_in local;
		socklen_t length = sizeof(local);
		if (getsockname(sock, (struct sockaddr*) &local, &length) == 0) {
			ip = ntohl(local.sin_addr.s_addr);
		}
	}
	SocketCloseQuiet(sock);
#endif
	if (!ip) {
		return false;
	}
	snprintf(out, outLength, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
	return true;
}

static bool _driverInit(struct GBASIODriver* driver) {
	struct GBASIONetLink* link = (struct GBASIONetLink*) driver;
	link->attached = true;
	struct GBA* gba = driver->p->p;
	if (gba->memory.rom) {
		const struct GBACartridge* cart = (const struct GBACartridge*) gba->memory.rom;
		memcpy(link->localGame, &cart->id, 4);
		link->localGame[4] = '\0';
	}
	link->localMode = driver->p->mode;
	link->awaitingReply = false;
	link->remoteTransfer = false;
	link->lastEventTime = mTimingCurrentTime(&gba->timing);
	mTimingDeschedule(&gba->timing, &link->event);
	mTimingSchedule(&gba->timing, &link->event, _nextInterval(link));
	_sendMode(link);
	_updateSioBits(link);
	_updateEngaged(link);
	return true;
}

static void _driverDeinit(struct GBASIODriver* driver) {
	struct GBASIONetLink* link = (struct GBASIONetLink*) driver;
	if (!link->attached) {
		return;
	}
	_dropRemoteTransfer(link);
	mTimingDeschedule(&driver->p->p->timing, &link->event);
	link->attached = false;
	link->awaitingReply = false;
	link->remoteTransfer = false;
	_sendMode(link);
	_updateEngaged(link);
}

static void _driverReset(struct GBASIODriver* driver) {
	struct GBASIONetLink* link = (struct GBASIONetLink*) driver;
	_dropRemoteTransfer(link);
	link->awaitingReply = false;
	link->remoteTransfer = false;
	link->completionValid = false;
	link->finishValid = false;
	GBASIONetLinkEnsureScheduled(link);
}

static uint32_t _driverId(const struct GBASIODriver* driver) {
	UNUSED(driver);
	return DRIVER_ID;
}

static void _driverSetMode(struct GBASIODriver* driver, enum GBASIOMode mode) {
	struct GBASIONetLink* link = (struct GBASIONetLink*) driver;
	if (link->localMode == (int) mode) {
		return;
	}
	link->localMode = mode;
	_sendMode(link);
	_updateSioBits(link);
	_updateEngaged(link);
}

static bool _driverHandlesMode(struct GBASIODriver* driver, enum GBASIOMode mode) {
	UNUSED(driver);
	UNUSED(mode);
	return true;
}

static int _driverConnectedDevices(struct GBASIODriver* driver) {
	struct GBASIONetLink* link = (struct GBASIONetLink*) driver;
	return link->state == GBA_NETLINK_CONNECTED ? 1 : 0;
}

static int _driverDeviceId(struct GBASIODriver* driver) {
	struct GBASIONetLink* link = (struct GBASIONetLink*) driver;
	return link->state == GBA_NETLINK_CONNECTED ? link->playerId : 0;
}

static uint16_t _driverWriteSIOCNT(struct GBASIODriver* driver, uint16_t value) {
	UNUSED(driver);
	return value;
}

static uint16_t _driverWriteRCNT(struct GBASIODriver* driver, uint16_t value) {
	UNUSED(driver);
	return value;
}

static bool _driverStart(struct GBASIODriver* driver) {
	struct GBASIONetLink* link = (struct GBASIONetLink*) driver;
	struct GBASIO* sio = driver->p;
	int mode = sio->mode;
	if (link->state != GBA_NETLINK_CONNECTED || !_isDataMode(mode)) {
		return true;
	}
	if (link->playerId != 0) {
		// The partner owns the clock; our side completes when its transfer arrives
		return false;
	}
	if (link->peerMode != mode) {
		return true;
	}
	struct GBA* gba = sio->p;
	uint32_t data;
	switch (mode) {
	case GBA_SIO_MULTI:
		data = gba->memory.io[GBA_REG(SIOMLT_SEND)];
		break;
	case GBA_SIO_NORMAL_8:
		data = gba->memory.io[GBA_REG(SIODATA8)] & 0xFF;
		break;
	default:
		data = gba->memory.io[GBA_REG(SIODATA32_LO)];
		data |= gba->memory.io[GBA_REG(SIODATA32_HI)] << 16;
		break;
	}
	++link->nextSeq;
	link->pendingSeq = link->nextSeq;
	link->awaitingReply = true;
	link->replyReceived = false;
	link->sentData = data;
	link->sentMode = mode;
	++link->stats.transfers;
	uint32_t gap = NO_GAP;
	if (link->finishValid) {
		int32_t since = mTimingCurrentTime(&gba->timing) - link->lastFinishTime;
		if (since >= 0 && since <= VIDEO_TOTAL_LENGTH) {
			gap = since;
		}
	}
	_sendFull(link, MSG_XFER, _encodeMode(mode), link->pendingSeq, data, GBASIOTransferCycles(mode, sio->siocnt, 1), gap);
	return true;
}

static bool _collectReply(struct GBASIONetLink* link, int mode, uint32_t* reply) {
	if (!link->awaitingReply || link->sentMode != mode) {
		link->awaitingReply = false;
		return false;
	}
	if (!link->replyReceived) {
		++link->stats.replyWaits;
		_waitFor(link, _replyArrived);
	}
	link->awaitingReply = false;
	struct GBASIO* sio = _sio(link);
	if (sio) {
		link->lastFinishTime = mTimingCurrentTime(&sio->p->timing);
		link->finishValid = true;
	}
	if (!link->replyReceived) {
		return false;
	}
	*reply = link->replyData;
	return true;
}

static void _driverFinishMultiplayer(struct GBASIODriver* driver, uint16_t data[4]) {
	struct GBASIONetLink* link = (struct GBASIONetLink*) driver;
	if (link->remoteTransfer) {
		link->remoteTransfer = false;
		if (link->remoteMode == GBA_SIO_MULTI) {
			memcpy(data, link->multiData, sizeof(link->multiData));
			return;
		}
	}
	memset(data, 0xFF, sizeof(uint16_t) * 4);
	uint32_t reply;
	if (link->awaitingReply) {
		data[0] = link->sentData;
		if (_collectReply(link, GBA_SIO_MULTI, &reply)) {
			data[1] = reply;
		}
		return;
	}
	data[_driverDeviceId(driver)] = driver->p->p->memory.io[GBA_REG(SIOMLT_SEND)];
}

static uint8_t _driverFinishNormal8(struct GBASIODriver* driver) {
	struct GBASIONetLink* link = (struct GBASIONetLink*) driver;
	if (link->remoteTransfer) {
		link->remoteTransfer = false;
		if (link->remoteMode == GBA_SIO_NORMAL_8) {
			return link->normalData;
		}
	}
	uint32_t reply;
	if (_collectReply(link, GBA_SIO_NORMAL_8, &reply)) {
		return reply;
	}
	return 0xFF;
}

static uint32_t _driverFinishNormal32(struct GBASIODriver* driver) {
	struct GBASIONetLink* link = (struct GBASIONetLink*) driver;
	if (link->remoteTransfer) {
		link->remoteTransfer = false;
		if (link->remoteMode == GBA_SIO_NORMAL_32) {
			return link->normalData;
		}
	}
	uint32_t reply;
	if (_collectReply(link, GBA_SIO_NORMAL_32, &reply)) {
		return reply;
	}
	return 0xFFFFFFFF;
}
