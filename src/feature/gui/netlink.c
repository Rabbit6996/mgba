/* Copyright (c) 2013-2026 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "feature/gui/netlink.h"

#include "feature/gui/gui-runner.h"

#ifdef ENABLE_NETLINK
#include <mgba/core/core.h>
#include <mgba/core/input.h>
#include <mgba/gba/interface.h>
#include <mgba/internal/gba/sio/netlink.h>
#include <mgba-util/gui/font.h>
#include <mgba-util/gui/menu.h>
#include <mgba-util/socket.h>
#include <mgba-util/vfs.h>

#include <stdarg.h>

#ifndef _MSC_VER
#include <sys/time.h>
#endif

#define NOTICE_DURATION_US 5000000
#define CANCEL_HOLD_US 1500000
#define CONNECTED_NOTICE_US 1200000

#define COLOR_TITLE 0xFFFFFFFF
#define COLOR_TEXT 0xFFD0D0D0
#define COLOR_HINT 0xFF909090
#define COLOR_ERROR 0xFF8080FF

enum {
	LINK_ITEM_HOST = 1,
	LINK_ITEM_JOIN,
	LINK_ITEM_DISCONNECT,
	LINK_ITEM_BACK,
};

struct mGUINetLink {
	struct GBASIONetLink link;
	bool socketsReady;
	bool attached;
	bool menuPaused;
	bool systemPaused;
	enum GBASIONetLinkState lastState;
	int64_t noticeUntil;
	uint32_t noticeColor;
	char notice[96];
	char status[96];
	int64_t cancelHeldSince;

	// Diagnostic log on the SD card: <config dir>/netlink.log
	struct VFile* log;
	int64_t logStart;
	bool waiting;
	unsigned frames;
};

static int64_t _now(void) {
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

static void _logf(struct mGUINetLink* netlink, const char* format, ...) {
	if (!netlink->log) {
		char path[PATH_MAX];
		mCoreConfigDirectory(path, sizeof(path));
		strncat(path, PATH_SEP "netlink.log", sizeof(path) - strlen(path) - 1);
		netlink->log = VFileOpen(path, O_CREAT | O_WRONLY | O_APPEND);
		if (!netlink->log) {
			return;
		}
		netlink->logStart = _now();
		const char* header = "---- mGBA Wi-Fi link session ----\n";
		netlink->log->write(netlink->log, header, strlen(header));
	}
	char line[256];
	int64_t t = _now() - netlink->logStart;
	int length = snprintf(line, sizeof(line), "[%4u.%03us] ", (unsigned) (t / 1000000), (unsigned) (t / 1000 % 1000));
	va_list args;
	va_start(args, format);
	length += vsnprintf(&line[length], sizeof(line) - length - 1, format, args);
	va_end(args);
	if (length > (int) sizeof(line) - 2) {
		length = sizeof(line) - 2;
	}
	line[length++] = '\n';
	netlink->log->write(netlink->log, line, length);
	netlink->log->sync(netlink->log, NULL, 0);
}

static void _logStats(struct mGUINetLink* netlink, const char* what) {
	const struct GBASIONetLink* link = &netlink->link;
	_logf(netlink, "%s: state=%i player=%i mode=%i peerMode=%i peerPaused=%i transfers=%u replyWaits=%u driftWaits=%u missed=%u waited=%.2fs",
	      what, link->state, link->playerId + 1, link->localMode, link->peerMode, link->peerPaused,
	      link->stats.transfers, link->stats.replyWaits, link->stats.driftWaits, link->stats.missedReplies,
	      link->stats.waitMicros / 1e6);
}

static bool _isGBA(struct mGUIRunner* runner) {
	return runner->core && runner->core->platform(runner->core) == mPLATFORM_GBA;
}

static struct mGUINetLink* _get(struct mGUIRunner* runner) {
	return runner->netlink;
}

static const char* _keyName(struct mGUIRunner* runner, int guiInput) {
	if (!runner->keySources || !runner->keySources[0].id) {
		return NULL;
	}
	int key = mInputQueryBinding(&runner->params.keyMap, runner->keySources[0].id, guiInput);
	if (key < 0 || (size_t) key >= runner->keySources[0].nKeys) {
		return NULL;
	}
	return runner->keySources[0].keyNames[key];
}

static uint16_t _port(struct mGUIRunner* runner) {
	unsigned port = GBA_NETLINK_DEFAULT_PORT;
	if (!mCoreConfigGetUIntValue(&runner->config, "netlink.port", &port) || !port || port > 0xFFFF) {
		port = GBA_NETLINK_DEFAULT_PORT;
	}
	return port;
}

static void _setNotice(struct mGUINetLink* netlink, uint32_t color, const char* text) {
	strncpy(netlink->notice, text, sizeof(netlink->notice) - 1);
	netlink->notice[sizeof(netlink->notice) - 1] = '\0';
	netlink->noticeColor = color;
	netlink->noticeUntil = _now() + NOTICE_DURATION_US;
}

static void _drawScreen(struct mGUIRunner* runner, const char* title, const char* const* lines, const uint32_t* colors, size_t nLines) {
	struct GUIParams* params = &runner->params;
	unsigned lineHeight = GUIFontHeight(params->font);
	params->drawStart();
	if (runner->drawFrame) {
		runner->drawFrame(runner, true);
	}
	if (params->guiPrepare) {
		params->guiPrepare();
	}
	int y = (params->height - (nLines + 2) * lineHeight) / 2 + lineHeight;
	GUIFontPrint(params->font, params->width / 2, y, GUI_ALIGN_HCENTER, COLOR_TITLE, title);
	y += lineHeight * 2;
	size_t i;
	for (i = 0; i < nLines; ++i) {
		if (lines[i] && lines[i][0]) {
			GUIFontPrint(params->font, params->width / 2, y, GUI_ALIGN_HCENTER, colors ? colors[i] : COLOR_TEXT, lines[i]);
		}
		y += lineHeight;
	}
	if (params->guiFinish) {
		params->guiFinish();
	}
	params->drawEnd();
}

static bool _stillRunning(struct mGUIRunner* runner) {
	return !runner->running || runner->running(runner);
}

// Show a message until the user presses a button (or the app is closing)
static void _showMessage(struct mGUIRunner* runner, const char* title, const char* message) {
	char hint[64];
	const char* back = _keyName(runner, GUI_INPUT_SELECT);
	snprintf(hint, sizeof(hint), "Press %s to continue", back ? back : "any button");
	const char* lines[] = { message, "", hint };
	const uint32_t colors[] = { COLOR_TEXT, COLOR_TEXT, COLOR_HINT };
	GUIInvalidateKeys(&runner->params);
	while (_stillRunning(runner)) {
		_drawScreen(runner, title, lines, colors, 3);
		uint32_t keys = 0;
		GUIPollInput(&runner->params, &keys, NULL);
		if (keys & ((1 << GUI_INPUT_SELECT) | (1 << GUI_INPUT_BACK) | (1 << GUI_INPUT_CANCEL))) {
			break;
		}
	}
	GUIInvalidateKeys(&runner->params);
}

static bool _waitCallback(struct GBASIONetLink* link, void* context, enum GBASIONetLinkWaitReason reason, int64_t waited) {
	UNUSED(link);
	struct mGUIRunner* runner = context;
	struct mGUINetLink* netlink = _get(runner);
	if (!netlink->waiting) {
		netlink->waiting = true;
		_logStats(netlink, reason == GBA_NETLINK_WAIT_PARTNER_PAUSED ? "waiting (partner paused)" : "waiting (partner slow)");
	}
	if (!_stillRunning(runner)) {
		return false;
	}
	uint32_t held = 0;
	GUIPollInput(&runner->params, NULL, &held);
	int64_t now = _now();
	bool cancelHeld = held & ((1 << GUI_INPUT_BACK) | (1 << GUI_INPUT_CANCEL));
	if (!cancelHeld) {
		netlink->cancelHeldSince = 0;
	} else if (!netlink->cancelHeldSince) {
		netlink->cancelHeldSince = now;
	} else if (now - netlink->cancelHeldSince >= CANCEL_HOLD_US) {
		netlink->cancelHeldSince = 0;
		_logf(netlink, "user cancelled the wait after %.1fs", waited / 1e6);
		return false;
	}

	char waitedText[48];
	snprintf(waitedText, sizeof(waitedText), "Waiting... (%u s)", (unsigned) (waited / 1000000));
	char hint[64];
	const char* back = _keyName(runner, GUI_INPUT_BACK);
	snprintf(hint, sizeof(hint), "Hold %s to disconnect", back ? back : "Back");
	const char* lines[] = {
		reason == GBA_NETLINK_WAIT_PARTNER_PAUSED ? "Your partner paused their game." : "Your partner is not keeping up.",
		waitedText,
		"",
		hint,
	};
	const uint32_t colors[] = { COLOR_TEXT, COLOR_TEXT, COLOR_TEXT, COLOR_HINT };
	_drawScreen(runner, "Link cable", lines, colors, 4);
	return true;
}

static struct mGUINetLink* _ensure(struct mGUIRunner* runner) {
	if (!_isGBA(runner)) {
		return NULL;
	}
	struct mGUINetLink* netlink = runner->netlink;
	if (!netlink) {
		netlink = calloc(1, sizeof(*netlink));
		if (!netlink) {
			return NULL;
		}
		GBASIONetLinkCreate(&netlink->link);
		netlink->link.waitCallback = _waitCallback;
		netlink->link.waitContext = runner;
		netlink->lastState = GBA_NETLINK_IDLE;
		runner->netlink = netlink;
	}
	if (!netlink->socketsReady) {
		SocketSubsystemInit();
		netlink->socketsReady = true;
	}
	return netlink;
}

static void _attach(struct mGUIRunner* runner, struct mGUINetLink* netlink) {
	if (netlink->attached) {
		return;
	}
	runner->core->setPeripheral(runner->core, mPERIPH_GBA_LINK_PORT, &netlink->link.d);
	netlink->attached = true;
	GBASIONetLinkSetPaused(&netlink->link, netlink->menuPaused || netlink->systemPaused);
}

static void _detach(struct mGUIRunner* runner, struct mGUINetLink* netlink) {
	if (!netlink->attached) {
		return;
	}
	runner->core->setPeripheral(runner->core, mPERIPH_GBA_LINK_PORT, NULL);
	netlink->attached = false;
}

static void _disconnect(struct mGUIRunner* runner, struct mGUINetLink* netlink) {
	if (GBASIONetLinkGetState(&netlink->link) != GBA_NETLINK_IDLE) {
		_logStats(netlink, "disconnecting");
	}
	GBASIONetLinkDisconnect(&netlink->link);
	netlink->lastState = GBA_NETLINK_IDLE;
	_detach(runner, netlink);
}

// Returns true once the link is up; false if cancelled or failed
static bool _waitForPartner(struct mGUIRunner* runner, struct mGUINetLink* netlink, const char* title, const char* const* info, size_t nInfo) {
	char hint[64];
	const char* back = _keyName(runner, GUI_INPUT_BACK);
	snprintf(hint, sizeof(hint), "Press %s to cancel", back ? back : "Back");
	const char* lines[8];
	uint32_t colors[8];
	size_t i;
	for (i = 0; i < nInfo && i < 6; ++i) {
		lines[i] = info[i];
		colors[i] = COLOR_TEXT;
	}
	lines[i] = "";
	colors[i] = COLOR_TEXT;
	++i;
	lines[i] = hint;
	colors[i] = COLOR_HINT;
	++i;

	GUIInvalidateKeys(&runner->params);
	while (true) {
		if (!_stillRunning(runner)) {
			_disconnect(runner, netlink);
			return false;
		}
		GBASIONetLinkUpdate(&netlink->link, 10);
		enum GBASIONetLinkState state = GBASIONetLinkGetState(&netlink->link);
		if (state == GBA_NETLINK_CONNECTED) {
			break;
		}
		if (state == GBA_NETLINK_ERROR || state == GBA_NETLINK_IDLE) {
			_logf(netlink, "connecting failed: %s", GBASIONetLinkGetError(&netlink->link));
			char error[GBA_NETLINK_ERROR_LENGTH + 32];
			snprintf(error, sizeof(error), "%s.%s", GBASIONetLinkGetError(&netlink->link),
			         netlink->link.playerId ? " Is player 1 waiting?" : "");
			_disconnect(runner, netlink);
			_showMessage(runner, "Link failed", error);
			return false;
		}
		uint32_t keys = 0;
		GUIPollInput(&runner->params, &keys, NULL);
		if (keys & ((1 << GUI_INPUT_BACK) | (1 << GUI_INPUT_CANCEL))) {
			_logf(netlink, "user cancelled connecting");
			_disconnect(runner, netlink);
			GUIInvalidateKeys(&runner->params);
			return false;
		}
		_drawScreen(runner, title, lines, colors, i);
	}

	netlink->lastState = GBA_NETLINK_CONNECTED;
	netlink->frames = 0;
	_logf(netlink, "connected as player %i, my game %s, partner game %s", netlink->link.playerId + 1, netlink->link.localGame, netlink->link.peerGame);
	char partner[64];
	snprintf(partner, sizeof(partner), "You are player %i. Partner game: %s", netlink->link.playerId + 1, netlink->link.peerGame);
	const char* done[] = { partner };
	int64_t until = _now() + CONNECTED_NOTICE_US;
	while (_now() < until && _stillRunning(runner)) {
		GBASIONetLinkUpdate(&netlink->link, 10);
		_drawScreen(runner, "Connected!", done, NULL, 1);
	}
	GUIInvalidateKeys(&runner->params);
	return true;
}

static void _host(struct mGUIRunner* runner, struct mGUINetLink* netlink) {
	uint16_t port = _port(runner);
	_logf(netlink, "hosting on port %u", port);
	if (!GBASIONetLinkHost(&netlink->link, port)) {
		_logf(netlink, "hosting failed: %s", GBASIONetLinkGetError(&netlink->link));
		char error[GBA_NETLINK_ERROR_LENGTH + 8];
		snprintf(error, sizeof(error), "%s.", GBASIONetLinkGetError(&netlink->link));
		GBASIONetLinkDisconnect(&netlink->link);
		_showMessage(runner, "Link failed", error);
		return;
	}
	_attach(runner, netlink);

	char address[32];
	char addressLine[64];
	if (GBASIONetLinkGetLocalAddress(address, sizeof(address))) {
		_logf(netlink, "local address %s", address);
		snprintf(addressLine, sizeof(addressLine), "Your address: %s", address);
	} else {
		snprintf(addressLine, sizeof(addressLine), "Could not determine your address. Is Wi-Fi on?");
	}
	char portLine[32];
	snprintf(portLine, sizeof(portLine), "Port: %u", port);
	const char* info[] = {
		addressLine,
		portLine,
		"",
		"On the other system, choose",
		"\"Join a session\" and enter this address.",
	};
	_waitForPartner(runner, netlink, "Waiting for player 2...", info, 5);
}

static void _join(struct mGUIRunner* runner, struct mGUINetLink* netlink) {
	if (!runner->params.getText) {
		_showMessage(runner, "Link failed", "No on-screen keyboard is available.");
		return;
	}
	struct GUIKeyboardParams keyboard;
	GUIKeyboardParamsInit(&keyboard);
	strncpy(keyboard.title, "Address of player 1 (e.g. 192.168.1.20)", sizeof(keyboard.title) - 1);
	const char* last = mCoreConfigGetValue(&runner->config, "netlink.host");
	if (last) {
		strncpy(keyboard.result, last, sizeof(keyboard.result) - 1);
	}
	keyboard.maxLen = 15;
	if (runner->params.getText(&keyboard) != GUI_KEYBOARD_DONE) {
		return;
	}
	struct Address address;
	if (!GBASIONetLinkParseAddress(keyboard.result, &address)) {
		_showMessage(runner, "Link failed", "That is not a valid address (e.g. 192.168.1.20).");
		return;
	}
	mCoreConfigSetValue(&runner->config, "netlink.host", keyboard.result);

	uint16_t port = _port(runner);
	char target[64];
	snprintf(target, sizeof(target), "%.40s, port %u", keyboard.result, port);
	const char* connecting[] = { target };
	_logf(netlink, "joining %s", target);
	_drawScreen(runner, "Connecting...", connecting, NULL, 1);
	if (!GBASIONetLinkConnect(&netlink->link, keyboard.result, port)) {
		char error[GBA_NETLINK_ERROR_LENGTH + 32];
		snprintf(error, sizeof(error), "%s. Is player 1 waiting?", GBASIONetLinkGetError(&netlink->link));
		GBASIONetLinkDisconnect(&netlink->link);
		_showMessage(runner, "Link failed", error);
		return;
	}
	_attach(runner, netlink);
	_waitForPartner(runner, netlink, "Connecting...", connecting, 1);
}

static const char* _statusText(struct mGUINetLink* netlink) {
	if (!netlink) {
		return "Status: not connected";
	}
	switch (GBASIONetLinkGetState(&netlink->link)) {
	case GBA_NETLINK_CONNECTED:
		snprintf(netlink->status, sizeof(netlink->status), "Status: player %i, partner game %s", netlink->link.playerId + 1, netlink->link.peerGame);
		break;
	case GBA_NETLINK_LISTENING:
	case GBA_NETLINK_HANDSHAKE:
		snprintf(netlink->status, sizeof(netlink->status), "Status: connecting");
		break;
	case GBA_NETLINK_ERROR:
		snprintf(netlink->status, sizeof(netlink->status), "Status: %s", GBASIONetLinkGetError(&netlink->link));
		break;
	case GBA_NETLINK_IDLE:
	default:
		snprintf(netlink->status, sizeof(netlink->status), "Status: not connected");
		break;
	}
	return netlink->status;
}

bool mGUINetLinkAvailable(struct mGUIRunner* runner) {
	return _isGBA(runner);
}

void mGUINetLinkShowMenu(struct mGUIRunner* runner) {
	struct mGUINetLink* netlink = _ensure(runner);
	if (!netlink) {
		return;
	}
	struct GUIMenu menu = {
		.title = "Link cable (Wi-Fi)",
		.index = 0,
		.background = &runner->background.d,
	};
	GUIMenuItemListInit(&menu.items, 0);
	while (_stillRunning(runner)) {
		GUIMenuItemListClear(&menu.items);
		menu.subtitle = _statusText(netlink);
		enum GBASIONetLinkState state = GBASIONetLinkGetState(&netlink->link);
		if (state == GBA_NETLINK_CONNECTED) {
			*GUIMenuItemListAppend(&menu.items) = (struct GUIMenuItem) { .title = "Disconnect", .data = GUI_V_U(LINK_ITEM_DISCONNECT) };
		} else {
			*GUIMenuItemListAppend(&menu.items) = (struct GUIMenuItem) { .title = "Host a session (player 1)", .data = GUI_V_U(LINK_ITEM_HOST) };
			*GUIMenuItemListAppend(&menu.items) = (struct GUIMenuItem) { .title = "Join a session (player 2)", .data = GUI_V_U(LINK_ITEM_JOIN) };
		}
		*GUIMenuItemListAppend(&menu.items) = (struct GUIMenuItem) { .title = "Back", .data = GUI_V_U(LINK_ITEM_BACK) };
		if (menu.index >= GUIMenuItemListSize(&menu.items)) {
			menu.index = 0;
		}

		struct GUIMenuItem* item;
		enum GUIMenuExitReason reason = GUIShowMenu(&runner->params, &menu, &item);
		if (reason != GUI_MENU_EXIT_ACCEPT || !GUIVariantIsUInt(item->data)) {
			break;
		}
		unsigned choice = item->data.v.u;
		if (choice == LINK_ITEM_BACK) {
			break;
		}
		switch (choice) {
		case LINK_ITEM_HOST:
			_host(runner, netlink);
			break;
		case LINK_ITEM_JOIN:
			_join(runner, netlink);
			break;
		case LINK_ITEM_DISCONNECT:
			_disconnect(runner, netlink);
			break;
		}
		if (GBASIONetLinkGetState(&netlink->link) == GBA_NETLINK_CONNECTED) {
			// Straight back to the game once connected
			break;
		}
	}
	GUIMenuItemListDeinit(&menu.items);
}

void mGUINetLinkFrame(struct mGUIRunner* runner) {
	struct mGUINetLink* netlink = _get(runner);
	if (!netlink || !netlink->attached) {
		return;
	}
	GBASIONetLinkEnsureScheduled(&netlink->link);
	enum GBASIONetLinkState state = GBASIONetLinkGetState(&netlink->link);
	if (netlink->waiting) {
		netlink->waiting = false;
		_logStats(netlink, "wait over");
	}
	++netlink->frames;
	if (state == GBA_NETLINK_CONNECTED && (netlink->frames == 60 || netlink->frames % 1800 == 0)) {
		_logStats(netlink, "running");
	}
	if (state != netlink->lastState) {
		if (netlink->lastState == GBA_NETLINK_CONNECTED && state == GBA_NETLINK_ERROR) {
			_logf(netlink, "link lost: %s", GBASIONetLinkGetError(&netlink->link));
			_logStats(netlink, "at loss");
			char text[GBA_NETLINK_ERROR_LENGTH + 16];
			snprintf(text, sizeof(text), "Link lost: %s", GBASIONetLinkGetError(&netlink->link));
			_setNotice(netlink, COLOR_ERROR, text);
		}
		netlink->lastState = state;
	}
	if (state == GBA_NETLINK_ERROR || state == GBA_NETLINK_IDLE) {
		// Nothing more to do this session; stop polling the network
		_detach(runner, netlink);
	}
}

static void _applyPause(struct mGUINetLink* netlink) {
	GBASIONetLinkSetPaused(&netlink->link, netlink->menuPaused || netlink->systemPaused);
}

void mGUINetLinkSetMenuPaused(struct mGUIRunner* runner, bool paused) {
	struct mGUINetLink* netlink = _get(runner);
	if (!netlink) {
		return;
	}
	netlink->menuPaused = paused;
	_applyPause(netlink);
}

void mGUINetLinkSetSystemPaused(struct mGUIRunner* runner, bool paused) {
	struct mGUINetLink* netlink = _get(runner);
	if (!netlink) {
		return;
	}
	netlink->systemPaused = paused;
	_applyPause(netlink);
}

bool mGUINetLinkDrawStatus(struct mGUIRunner* runner) {
	struct mGUINetLink* netlink = _get(runner);
	if (!netlink) {
		return false;
	}
	struct GUIParams* params = &runner->params;
	unsigned lineHeight = GUIFontHeight(params->font);
	bool drew = false;
	if (netlink->attached && GBASIONetLinkGetState(&netlink->link) == GBA_NETLINK_CONNECTED) {
		// Small status line: player, partner's serial mode and transfer count
		char status[48];
		snprintf(status, sizeof(status), "Link P%i m%i/%i #%u", netlink->link.playerId + 1,
		         netlink->link.localMode, netlink->link.peerMode, (unsigned) netlink->link.stats.transfers);
		GUIFontPrint(params->font, 0, params->height - lineHeight / 2, GUI_ALIGN_LEFT, 0x7FFFFFFF, status);
		drew = true;
	}
	if (netlink->noticeUntil) {
		if (_now() < netlink->noticeUntil) {
			GUIFontPrint(params->font, params->width / 2, params->height - lineHeight / 2, GUI_ALIGN_HCENTER, netlink->noticeColor, netlink->notice);
			drew = true;
		} else {
			netlink->noticeUntil = 0;
		}
	}
	return drew;
}

bool mGUINetLinkWantsOSD(struct mGUIRunner* runner) {
	struct mGUINetLink* netlink = _get(runner);
	if (!netlink) {
		return false;
	}
	return netlink->noticeUntil || (netlink->attached && GBASIONetLinkGetState(&netlink->link) == GBA_NETLINK_CONNECTED);
}

void mGUINetLinkGameUnloading(struct mGUIRunner* runner) {
	struct mGUINetLink* netlink = _get(runner);
	if (!netlink) {
		return;
	}
	_disconnect(runner, netlink);
	if (netlink->socketsReady) {
		SocketSubsystemDeinit();
	}
	if (netlink->log) {
		netlink->log->close(netlink->log);
	}
	free(netlink);
	runner->netlink = NULL;
}

#else

bool mGUINetLinkAvailable(struct mGUIRunner* runner) {
	UNUSED(runner);
	return false;
}

void mGUINetLinkShowMenu(struct mGUIRunner* runner) {
	UNUSED(runner);
}

void mGUINetLinkFrame(struct mGUIRunner* runner) {
	UNUSED(runner);
}

void mGUINetLinkSetMenuPaused(struct mGUIRunner* runner, bool paused) {
	UNUSED(runner);
	UNUSED(paused);
}

void mGUINetLinkSetSystemPaused(struct mGUIRunner* runner, bool paused) {
	UNUSED(runner);
	UNUSED(paused);
}

bool mGUINetLinkDrawStatus(struct mGUIRunner* runner) {
	UNUSED(runner);
	return false;
}

bool mGUINetLinkWantsOSD(struct mGUIRunner* runner) {
	UNUSED(runner);
	return false;
}

void mGUINetLinkGameUnloading(struct mGUIRunner* runner) {
	UNUSED(runner);
}

#endif
