/* Copyright (c) 2013-2026 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gba/sio/netlink.h>

#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/io.h>
#include <mgba-util/threading.h>

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

/* ---- Network I/O thread ----
 *
 * Every socket call happens here. The emulation thread talks to this thread
 * through two byte queues guarded by a mutex; it never touches a socket, so
 * it cannot freeze on one. The context is shared and reference-counted: if a
 * socket call ever hangs, the emulation side just drops its reference, reports
 * an error and moves on; the thread cleans up whenever the call returns.
 */

#define IO_QUEUE_SIZE 0x4000
#define IO_ACCEPT_POLL_MS 50
// While data is flowing, check the socket this often and wake up at once for
// anything to send; when the link is quiet, sleep in poll() for longer.
#define IO_ACTIVE_WAIT_US 200
#define IO_IDLE_POLL_MS 4
#define IO_ACTIVE_WINDOW_US 250000

enum GBASIONetLinkIOStatus {
	IO_STARTING = 0,
	IO_CONNECTED,
	IO_FAILED,
};

struct GBASIONetLinkIO {
	Mutex mutex;
	Condition cond; // signaled when something arrives (for emulation)
	Condition outCond; // signaled when something is queued to send
	int refs;
	bool quit;

	bool host;
	uint16_t port;
	uint32_t address;

	enum GBASIONetLinkIOStatus status;
	char error[GBA_NETLINK_ERROR_LENGTH];

	uint8_t in[IO_QUEUE_SIZE];
	size_t inFill;
	uint8_t out[IO_QUEUE_SIZE];
	size_t outFill;
	uint8_t pending[IO_QUEUE_SIZE];
	size_t pendingFill;
};

static void _condWait(Condition* cond, Mutex* mutex, int timeoutMs) {
#ifdef __3DS__
	// mgba-util's 3DS ConditionWaitTimed converts milliseconds incorrectly
	CondVar_WaitTimeout(cond, mutex, timeoutMs * 1000000LL);
#else
	ConditionWaitTimed(cond, mutex, timeoutMs);
#endif
}

static void _condWaitMicros(Condition* cond, Mutex* mutex, int timeoutUs) {
#ifdef __3DS__
	CondVar_WaitTimeout(cond, mutex, timeoutUs * 1000LL);
#elif defined(USE_PTHREADS)
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_nsec += timeoutUs * 1000L;
	ts.tv_sec += ts.tv_nsec / 1000000000L;
	ts.tv_nsec %= 1000000000L;
	pthread_cond_timedwait(cond, mutex, &ts);
#else
	ConditionWaitTimed(cond, mutex, timeoutUs >= 1000 ? timeoutUs / 1000 : 1);
#endif
}

static void _ioRelease(struct GBASIONetLinkIO* io) {
	MutexLock(&io->mutex);
	int refs = --io->refs;
	MutexUnlock(&io->mutex);
	if (!refs) {
		MutexDeinit(&io->mutex);
		ConditionDeinit(&io->cond);
		ConditionDeinit(&io->outCond);
		free(io);
	}
}

static void _ioSetFailed(struct GBASIONetLinkIO* io, const char* reason, int code) {
	MutexLock(&io->mutex);
	if (io->status != IO_FAILED) {
		io->status = IO_FAILED;
		if (code) {
			snprintf(io->error, sizeof(io->error), "%s (code %d)", reason, code);
		} else {
			snprintf(io->error, sizeof(io->error), "%s", reason);
		}
	}
	ConditionWake(&io->cond);
	MutexUnlock(&io->mutex);
}

static bool _ioShouldQuit(struct GBASIONetLinkIO* io) {
	MutexLock(&io->mutex);
	bool quit = io->quit;
	MutexUnlock(&io->mutex);
	return quit;
}

static ssize_t _socketSend(Socket sock, const void* buffer, size_t size) {
#ifdef NETLINK_USE_POLL
	int flags = MSG_DONTWAIT;
#ifdef MSG_NOSIGNAL
	// A partner that already hung up must not kill the process (SIGPIPE)
	flags |= MSG_NOSIGNAL;
#endif
	return send(sock, buffer, size, flags);
#else
	return SocketSend(sock, buffer, size);
#endif
}

static Socket _ioOpen(struct GBASIONetLinkIO* io) {
	if (io->host) {
		Socket listener = SocketOpenTCP(io->port, NULL);
		if (SOCKET_FAILED(listener)) {
			_ioSetFailed(io, "Could not open the link port", SocketError());
			return INVALID_SOCKET;
		}
		if (SOCKET_FAILED(SocketListen(listener, 1))) {
			int code = SocketError();
			SocketClose(listener);
			_ioSetFailed(io, "Could not listen on the link port", code);
			return INVALID_SOCKET;
		}
		SocketSetBlocking(listener, false);
		Socket sock = INVALID_SOCKET;
		while (!_ioShouldQuit(io)) {
			if (!_socketReady(listener, false, IO_ACCEPT_POLL_MS)) {
				continue;
			}
			sock = SocketAccept(listener, NULL);
			if (!SOCKET_FAILED(sock)) {
				break;
			}
		}
		SocketClose(listener);
		return sock;
	}

	struct Address destination = {
		.version = IPV4,
		.ipv4 = io->address,
	};
	// A plain blocking connect: we are on our own thread, and non-blocking
	// connects are unreliable on some systems (e.g. the 3DS)
	Socket sock = SocketConnectTCP(io->port, &destination);
	if (SOCKET_FAILED(sock)) {
		_ioSetFailed(io, "Could not reach the host", SocketError());
	}
	return sock;
}

static THREAD_ENTRY _ioThread(void* context) {
	struct GBASIONetLinkIO* io = context;
	Socket sock = _ioOpen(io);
	if (!SOCKET_FAILED(sock)) {
		SocketSetBlocking(sock, false);
		SocketSetTCPPush(sock, 1);
		MutexLock(&io->mutex);
		if (io->status == IO_STARTING) {
			io->status = IO_CONNECTED;
		}
		ConditionWake(&io->cond);
		MutexUnlock(&io->mutex);
	}

	uint8_t rx[1024];
	int64_t lastActivity = _nowMicros();
	while (!SOCKET_FAILED(sock)) {
		MutexLock(&io->mutex);
		bool quit = io->quit;
		if (io->outFill && io->pendingFill < sizeof(io->pending)) {
			size_t take = sizeof(io->pending) - io->pendingFill;
			if (take > io->outFill) {
				take = io->outFill;
			}
			memcpy(&io->pending[io->pendingFill], io->out, take);
			memmove(io->out, &io->out[take], io->outFill - take);
			io->outFill -= take;
			io->pendingFill += take;
		}
		MutexUnlock(&io->mutex);

		if (io->pendingFill) {
			lastActivity = _nowMicros();
			ssize_t sent = _socketSend(sock, io->pending, io->pendingFill);
			if (sent > 0) {
				memmove(io->pending, &io->pending[sent], io->pendingFill - sent);
				io->pendingFill -= sent;
			} else if (sent < 0 && !SocketWouldBlock()) {
				_ioSetFailed(io, "Lost connection to partner", SocketError());
				break;
			}
		}
		if (quit) {
			// Best effort to get a queued goodbye out; don't wait for anything
			break;
		}

		bool active = io->pendingFill || _nowMicros() - lastActivity < IO_ACTIVE_WINDOW_US;
		if (!_socketReady(sock, false, active ? 0 : IO_IDLE_POLL_MS)) {
			if (active && !io->pendingFill) {
				// Nothing to read right now: sleep briefly, but wake up at
				// once if emulation queues something to send
				MutexLock(&io->mutex);
				if (!io->outFill && !io->quit) {
					_condWaitMicros(&io->outCond, &io->mutex, IO_ACTIVE_WAIT_US);
				}
				MutexUnlock(&io->mutex);
			}
			continue;
		}
		ssize_t received = _recv(sock, rx, sizeof(rx));
		if (received == 0) {
			_ioSetFailed(io, "Partner disconnected", 0);
			break;
		}
		if (received < 0) {
			if (!SocketWouldBlock()) {
				_ioSetFailed(io, "Lost connection to partner", SocketError());
				break;
			}
			continue;
		}
		lastActivity = _nowMicros();
		MutexLock(&io->mutex);
		bool overflow = io->inFill + received > sizeof(io->in);
		if (!overflow) {
			memcpy(&io->in[io->inFill], rx, received);
			io->inFill += received;
			ConditionWake(&io->cond);
		}
		MutexUnlock(&io->mutex);
		if (overflow) {
			_ioSetFailed(io, "Receive queue overflowed", 0);
			break;
		}
	}
	if (!SOCKET_FAILED(sock)) {
		SocketClose(sock);
	}
	_ioRelease(io);
	THREAD_EXIT(0);
}

static bool _ioStart(struct GBASIONetLink* link, bool host, uint32_t address, uint16_t port) {
	struct GBASIONetLinkIO* io = calloc(1, sizeof(*io));
	if (!io) {
		return false;
	}
	MutexInit(&io->mutex);
	ConditionInit(&io->cond);
	ConditionInit(&io->outCond);
	io->refs = 2;
	io->host = host;
	io->address = address;
	io->port = port;
	io->status = IO_STARTING;

	bool started = false;
#ifdef __3DS__
	// Prefer the New 3DS extra core, then the system core, then our own core.
	// Higher priority than emulation, but it sleeps in poll() nearly always.
	static const int cores[] = { 2, 1, -2 };
	size_t i;
	for (i = 0; i < sizeof(cores) / sizeof(*cores) && !started; ++i) {
		started = threadCreate(_ioThread, io, 0x4000, 0x18, cores[i], true) != NULL;
	}
#else
	Thread thread;
	started = !ThreadCreate(&thread, _ioThread, io);
#ifdef USE_PTHREADS
	if (started) {
		pthread_detach(thread);
	}
#elif defined(_WIN32)
	if (started) {
		CloseHandle(thread);
	}
#endif
#endif
	if (!started) {
		MutexDeinit(&io->mutex);
		ConditionDeinit(&io->cond);
		ConditionDeinit(&io->outCond);
		free(io);
		return false;
	}
	link->io = io;
	return true;
}

static void _ioStop(struct GBASIONetLink* link) {
	struct GBASIONetLinkIO* io = link->io;
	if (!io) {
		return;
	}
	link->io = NULL;
	MutexLock(&io->mutex);
	io->quit = true;
	ConditionWake(&io->outCond);
	MutexUnlock(&io->mutex);
	_ioRelease(io);
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
	_ioStop(link);
	link->rxFill = 0;
	link->helloSent = false;
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
	struct GBASIONetLinkIO* io = link->io;
	if (!io) {
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
	MutexLock(&io->mutex);
	bool queued = io->outFill + GBA_NETLINK_MESSAGE_SIZE <= sizeof(io->out);
	if (queued) {
		_encode(&message, &io->out[io->outFill]);
		io->outFill += GBA_NETLINK_MESSAGE_SIZE;
		ConditionWake(&io->outCond);
	}
	MutexUnlock(&io->mutex);
	if (!queued) {
		_fail(link, "Sending to partner timed out");
	}
	return queued;
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

// Handle whatever the I/O thread received, waiting up to timeoutMs for
// something to arrive. Fails the link if the I/O thread reported an error.
static void _pump(struct GBASIONetLink* link, int timeoutMs) {
	struct GBASIONetLinkIO* io = link->io;
	while (io && link->io == io) {
		MutexLock(&io->mutex);
		if (!io->inFill && io->status != IO_FAILED && timeoutMs > 0) {
			_condWait(&io->cond, &io->mutex, timeoutMs);
		}
		timeoutMs = 0;
		size_t take = sizeof(link->rxBuffer) - link->rxFill;
		if (take > io->inFill) {
			take = io->inFill;
		}
		if (take) {
			memcpy(&link->rxBuffer[link->rxFill], io->in, take);
			memmove(io->in, &io->in[take], io->inFill - take);
			io->inFill -= take;
		}
		bool failed = io->status == IO_FAILED && !io->inFill;
		char error[GBA_NETLINK_ERROR_LENGTH];
		if (failed) {
			memcpy(error, io->error, sizeof(error));
		}
		MutexUnlock(&io->mutex);

		if (take) {
			link->rxFill += take;
			link->lastReceive = _nowMicros();
			size_t offset = 0;
			while (link->rxFill - offset >= GBA_NETLINK_MESSAGE_SIZE) {
				struct GBASIONetLinkMessage message;
				_decode(&link->rxBuffer[offset], &message);
				offset += GBA_NETLINK_MESSAGE_SIZE;
				_dispatch(link, &message);
				if (link->io != io) {
					// The link failed or was closed while handling the message
					return;
				}
			}
			if (offset) {
				memmove(link->rxBuffer, &link->rxBuffer[offset], link->rxFill - offset);
				link->rxFill -= offset;
			}
		}
		if (failed) {
			_fail(link, error);
			return;
		}
		if (!take) {
			return;
		}
	}
}

static void _service(struct GBASIONetLink* link, int timeoutMs) {
	struct GBASIONetLinkIO* io = link->io;
	if (!io) {
		return;
	}
	if (link->state == GBA_NETLINK_LISTENING || (link->state == GBA_NETLINK_HANDSHAKE && !link->helloSent)) {
		MutexLock(&io->mutex);
		if (io->status == IO_STARTING && timeoutMs > 0) {
			_condWait(&io->cond, &io->mutex, timeoutMs);
		}
		enum GBASIONetLinkIOStatus status = io->status;
		MutexUnlock(&io->mutex);
		if (status == IO_STARTING) {
			if (link->state == GBA_NETLINK_HANDSHAKE && _nowMicros() - link->connectStart > CONNECT_TIMEOUT_MS * 1000LL) {
				_fail(link, "Could not reach the host (timed out)");
			}
			return;
		}
		if (status == IO_FAILED) {
			_pump(link, 0);
			return;
		}
		link->state = GBA_NETLINK_HANDSHAKE;
		link->helloSent = true;
		link->lastReceive = _nowMicros();
		_sendHello(link);
		timeoutMs = 0;
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
	if (!_ioStart(link, true, 0, port)) {
		_fail(link, "Could not start the network thread");
		return false;
	}
	link->state = GBA_NETLINK_LISTENING;
	link->error[0] = '\0';
	// The port is opened on the I/O thread; give it a moment to report problems
	_service(link, 100);
	return link->state == GBA_NETLINK_LISTENING || link->state == GBA_NETLINK_HANDSHAKE;
}

bool GBASIONetLinkConnect(struct GBASIONetLink* link, const char* address, uint16_t port) {
	GBASIONetLinkDisconnect(link);
	link->playerId = 1;
	struct Address destination;
	if (!GBASIONetLinkParseAddress(address, &destination)) {
		_fail(link, "Invalid address");
		return false;
	}
	if (!_ioStart(link, false, destination.ipv4, port)) {
		_fail(link, "Could not start the network thread");
		return false;
	}
	link->state = GBA_NETLINK_HANDSHAKE;
	link->error[0] = '\0';
	link->connectStart = _nowMicros();
	link->lastReceive = link->connectStart;
	return true;
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
