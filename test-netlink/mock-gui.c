/* Headless mGUI frontend for testing the Wi-Fi link menus end to end.
 *
 * It implements the few platform hooks a GUI port provides (font, drawing,
 * input, keyboard), records the text each frame "draws", and drives the menus
 * with a small scripted agent that reads the screen like a user would.
 *
 * mock-gui host <rom>
 * mock-gui join <rom>
 */
#include "feature/gui/gui-runner.h"
#include "feature/gui/netlink.h"

#include <mgba/core/core.h>
#include <mgba/core/log.h>
#include <mgba/internal/gba/sio/netlink.h>
#include <mgba-util/gui.h>
#include <mgba-util/gui/font.h>
#include <mgba-util/gui/menu.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#define MOCK_INPUT 0x4D4F434B
#define MAX_LINES 32
#define GLYPH_W 6
#define GLYPH_H 12

static const char* role;
static bool isHost;

struct Line {
	int y;
	int minX;
	char text[128];
	size_t len;
};

static struct Line lines[MAX_LINES];
static size_t nLines;
static int pointerY = -1000;
static char screen[4096];
static char lastPrinted[4096];

static struct mGUIRunner* gRunner;
static int phase = 0;
static int64_t phaseStart;
static int pauseCount = 0;
static bool paramsWritten = false;
static bool running = true;
static uint32_t pendingKeys = 0;
static int idlePolls = 0;
static int frames = 0;
static uint32_t doneFrame = 0;
static uint32_t results[9];
static bool sawPartnerPausedNotice = false;

static int64_t now(void) {
	struct timeval tv;
	gettimeofday(&tv, 0);
	return tv.tv_sec * 1000000LL + tv.tv_usec;
}

static void _log(struct mLogger* logger, int category, enum mLogLevel level, const char* format, va_list args) {
	(void) logger;
	if (!(level & (mLOG_WARN | mLOG_ERROR | mLOG_FATAL | mLOG_INFO)) || category == -1) {
		return;
	}
	const char* name = mLogCategoryName(category);
	if (level == mLOG_INFO && strcmp(name, "GBA Serial I/O")) {
		return;
	}
	fprintf(stderr, "[%s]   log[%s]: ", role, name);
	vfprintf(stderr, format, args);
	fprintf(stderr, "\n");
}
static struct mLogger logger = { .log = _log };

/* ---- Font stubs: capture glyphs as text lines ---- */
struct GUIFont {
	int dummy;
};

struct GUIFont* GUIFontCreate(void) {
	return calloc(1, sizeof(struct GUIFont));
}

void GUIFontDestroy(struct GUIFont* font) {
	free(font);
}

unsigned GUIFontHeight(const struct GUIFont* font) {
	(void) font;
	return GLYPH_H;
}

unsigned GUIFontGlyphWidth(const struct GUIFont* font, uint32_t glyph) {
	(void) font;
	(void) glyph;
	return GLYPH_W;
}

void GUIFontIconMetrics(const struct GUIFont* font, enum GUIIcon icon, unsigned* w, unsigned* h) {
	(void) font;
	(void) icon;
	if (w) {
		*w = GLYPH_W;
	}
	if (h) {
		*h = GLYPH_H;
	}
}

void GUIFontDrawGlyph(struct GUIFont* font, int x, int y, uint32_t color, uint32_t glyph) {
	(void) font;
	(void) color;
	size_t i;
	for (i = 0; i < nLines; ++i) {
		if (lines[i].y == y && x >= lines[i].minX + (int) lines[i].len * GLYPH_W - 1 && x <= lines[i].minX + (int) lines[i].len * GLYPH_W + GLYPH_W * 2) {
			break;
		}
	}
	if (i == nLines) {
		if (nLines == MAX_LINES) {
			return;
		}
		++nLines;
		lines[i].y = y;
		lines[i].minX = x;
		lines[i].len = 0;
	}
	if (lines[i].len < sizeof(lines[i].text) - 1) {
		lines[i].text[lines[i].len++] = glyph < 0x80 ? (char) glyph : '?';
		lines[i].text[lines[i].len] = '\0';
	}
}

void GUIFontDrawIcon(struct GUIFont* font, int x, int y, enum GUIAlignment align, enum GUIOrientation orient, uint32_t color, enum GUIIcon icon) {
	(void) font;
	(void) x;
	(void) align;
	(void) orient;
	(void) color;
	if (icon == GUI_ICON_POINTER) {
		pointerY = y;
	}
}

void GUIFontDrawIconSize(struct GUIFont* font, int x, int y, int w, int h, uint32_t color, enum GUIIcon icon) {
	(void) font;
	(void) x;
	(void) y;
	(void) w;
	(void) h;
	(void) color;
	(void) icon;
}

/* ---- Screen capture ---- */
static void _drawStart(void) {
	nLines = 0;
	pointerY = -1000;
}

static int _lineCompare(const void* a, const void* b) {
	const struct Line* la = a;
	const struct Line* lb = b;
	if (la->y != lb->y) {
		return la->y - lb->y;
	}
	return la->minX - lb->minX;
}

static const char* _selected(void) {
	size_t i;
	for (i = 0; i < nLines; ++i) {
		if (lines[i].y == pointerY || (lines[i].y > pointerY - 4 && lines[i].y < pointerY + 4)) {
			return lines[i].text;
		}
	}
	return "";
}

static void _drawEnd(void) {
	qsort(lines, nLines, sizeof(lines[0]), _lineCompare);
	screen[0] = '\0';
	size_t i;
	for (i = 0; i < nLines; ++i) {
		if (strstr(lines[i].text, "fps")) {
			continue;
		}
		strncat(screen, lines[i].text, sizeof(screen) - strlen(screen) - 2);
		strncat(screen, lines[i].y == pointerY ? " <\n" : "\n", sizeof(screen) - strlen(screen) - 1);
	}
	if (strstr(screen, "Waiting... (")) {
		// Don't reprint the countdown every time
		char* waited = strstr(screen, "Waiting... (");
		if (!strncmp(lastPrinted, screen, waited - screen)) {
			return;
		}
	}
	if (strcmp(screen, lastPrinted) && screen[0]) {
		strcpy(lastPrinted, screen);
		printf("[%s] ---- screen ----\n", role);
		char copy[4096];
		strcpy(copy, screen);
		char* line = strtok(copy, "\n");
		while (line) {
			printf("[%s] | %s\n", role, line);
			line = strtok(NULL, "\n");
		}
		fflush(stdout);
	}
	if (strstr(screen, "Your partner paused their game.")) {
		sawPartnerPausedNotice = true;
	}
}

static bool _onScreen(const char* text) {
	return strstr(screen, text) != NULL;
}

static void _setPhase(int p) {
	phase = p;
	phaseStart = now();
	printf("[%s] >> phase %d\n", role, p);
	fflush(stdout);
}

/* ---- Scripted agent: decides which GUI keys are "pressed" ---- */
static uint32_t _navigateTo(const char* target) {
	const char* selected = _selected();
	if (!strcmp(selected, target)) {
		return 1 << GUI_INPUT_SELECT;
	}
	return 1 << GUI_INPUT_DOWN;
}

static uint32_t _agent(void) {
	// Alternate pressed/released so each press is a fresh edge
	if (pendingKeys) {
		uint32_t keys = pendingKeys;
		pendingKeys = 0;
		return keys;
	}
	if (++idlePolls % 2) {
		return 0;
	}
	if (now() - phaseStart > 60000000) {
		printf("[%s] WATCHDOG: stuck in phase %d\n", role, phase);
		exit(3);
	}
	if (_onScreen("Link failed")) {
		// e.g. the host is not listening yet; dismiss and try again
		usleep(200000);
		return 1 << GUI_INPUT_SELECT;
	}
	if (_onScreen("Game Paused")) {
		if (phase == 0) {
			return _navigateTo("Link cable (Wi-Fi)");
		}
		if (phase == 3) {
			// Mid-session pause: stay in the menu for two seconds
			if (now() - phaseStart < 2000000) {
				return 0;
			}
			if (!strcmp(_selected(), "Unpause")) {
				_setPhase(4);
			}
			return _navigateTo("Unpause");
		}
		if (phase == 5) {
			return _navigateTo("Exit game");
		}
		return _navigateTo("Unpause");
	}
	if (_onScreen("Link cable (Wi-Fi)")) {
		if (phase == 0) {
			_setPhase(1);
		}
		if (phase == 1) {
			return _navigateTo(isHost ? "Host a session (player 1)" : "Join a session (player 2)");
		}
		return 1 << GUI_INPUT_BACK;
	}
	return 0;
}

static uint32_t _pollInput(const struct mInputMap* map) {
	(void) map;
	usleep(500);
	return _agent();
}

static enum GUIKeyboardStatus _getText(struct GUIKeyboardParams* keyboard) {
	printf("[%s] keyboard: '%s' (prefilled '%s') -> typing 127.0.0.1\n", role, keyboard->title, keyboard->result);
	strcpy(keyboard->result, "127.0.0.1");
	return GUI_KEYBOARD_DONE;
}

/* ---- Runner hooks ---- */
static mColor videoBuffer[256 * 224];

static void _setup(struct mGUIRunner* runner) {
	runner->core->setVideoBuffer(runner->core, videoBuffer, 256);
	runner->core->opts.skipBios = true;
}

static void _drawFrame(struct mGUIRunner* runner, bool faded) {
	(void) runner;
	(void) faded;
}

static uint16_t _pollGameInput(struct mGUIRunner* runner) {
	(void) runner;
	return 0;
}

static void _prepareForFrame(struct mGUIRunner* runner) {
	struct mCore* core = runner->core;
	++frames;
	if ((phase == 0 || phase == 1) && frames % 60 == 20) {
		// Open the pause menu
		pendingKeys = 1 << GUI_INPUT_CANCEL;
	}
	if (phase == 1 && runner->netlink && GBASIONetLinkGetState((struct GBASIONetLink*) runner->netlink) == GBA_NETLINK_CONNECTED) {
		_setPhase(2);
	}
	if (phase >= 2 && !paramsWritten) {
		core->busWrite32(core, 0x02000104, 1500); // transfers
		core->busWrite32(core, 0x02000108, 1000); // gap
		core->busWrite32(core, 0x0200010C, 0); // multiplayer mode
		core->busWrite32(core, 0x02000110, 50); // child delay
		core->busWrite32(core, 0x02000100, 0x50415241);
		paramsWritten = true;
	}
	uint32_t completed = core->busRead32(core, 0x02000010);
	if (phase == 2 && completed >= (isHost ? 300u : 700u)) {
		// Open the pause menu mid-session to exercise pausing
		printf("[%s] pausing mid-session after %u transfers\n", role, completed);
		_setPhase(3);
		++pauseCount;
		pendingKeys = 1 << GUI_INPUT_CANCEL;
	}
	if (phase == 4 && core->busRead32(core, 0x02000008)) {
		if (!doneFrame) {
			doneFrame = frames;
		}
		if (frames - doneFrame > 60) {
			int i;
			for (i = 0; i < 9; ++i) {
				results[i] = core->busRead32(core, 0x02000000 + i * 4);
			}
			_setPhase(5);
			pendingKeys = 1 << GUI_INPUT_CANCEL;
		}
	}
}

static bool _running(struct mGUIRunner* runner) {
	(void) runner;
	return running;
}

int main(int argc, char** argv) {
	if (argc < 3) {
		fprintf(stderr, "usage: %s host|join rom\n", argv[0]);
		return 1;
	}
	role = argv[1];
	isHost = !strcmp(role, "host");
	mLogSetDefaultLogger(&logger);
	setvbuf(stdout, NULL, _IOLBF, 0);

	static const char* keyNames[] = { "A", "B", "X", "Up", "Down", "Left", "Right" };
	struct GUIFont* font = GUIFontCreate();
	struct mGUIRunner runner = {
		.params = {
			320, 240,
			font, "/",
			_drawStart, _drawEnd,
			_pollInput, NULL,
			NULL,
			NULL, NULL,
			_getText,
		},
		.keySources = (struct GUIInputKeys[]) {
			{
				.name = "Mock",
				.id = MOCK_INPUT,
				.keyNames = keyNames,
				.nKeys = 7,
			},
			{ .id = 0 }
		},
		.setup = _setup,
		.prepareForFrame = _prepareForFrame,
		.drawFrame = _drawFrame,
		.pollGameInput = _pollGameInput,
		.running = _running,
	};
	gRunner = &runner;
	runner.autosave.running = true;
	MutexInit(&runner.autosave.mutex);
	ConditionInit(&runner.autosave.cond);
	ThreadCreate(&runner.autosave.thread, mGUIAutosaveThread, &runner.autosave);

	char port[32];
	snprintf(port, sizeof(port), "mock-%s", role);
	mGUIInit(&runner, port);
	mLogSetDefaultLogger(&logger);
	mInputBindKey(&runner.params.keyMap, MOCK_INPUT, 0, GUI_INPUT_SELECT);
	mInputBindKey(&runner.params.keyMap, MOCK_INPUT, 1, GUI_INPUT_BACK);
	mInputBindKey(&runner.params.keyMap, MOCK_INPUT, 2, GUI_INPUT_CANCEL);
	mCoreConfigSetDefaultIntValue(&runner.config, "skipBios", 1);
	mCoreConfigSetDefaultIntValue(&runner.config, "autoload", 0);
	mCoreConfigSetDefaultIntValue(&runner.config, "autosave", 0);

	// Start by opening the pause menu once the game runs
	pendingKeys = 1 << GUI_INPUT_CANCEL;
	phaseStart = now();
	mGUIRun(&runner, argv[2]);

	MutexLock(&runner.autosave.mutex);
	runner.autosave.running = false;
	ConditionWake(&runner.autosave.cond);
	MutexUnlock(&runner.autosave.mutex);
	ThreadJoin(&runner.autosave.thread);
	mGUIDeinit(&runner);

	printf("[%s] RESULT role=%s done=%u completed=%u errors=%u first@%u got=%08X want=%08X pauses=%d sawPartnerPaused=%d phase=%d\n",
	       role, results[3] ? "child" : "parent", results[2], results[4], results[5], results[6], results[7], results[8],
	       pauseCount, sawPartnerPausedNotice, phase);
	return results[2] == 1 && results[5] == 0 && results[4] == 1500 ? 0 : 2;
}
