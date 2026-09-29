/* Test harness: run one side of a network link session headless.
 *
 * netlink-test host <rom> [count gap mode childDelay [port]]
 * netlink-test join <rom> <address> [count gap mode childDelay [port]]
 */
#include <mgba/core/core.h>
#include <mgba/core/config.h>
#include <mgba/gba/interface.h>
#include <mgba/internal/gba/sio/netlink.h>

#include <mgba/core/log.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

static void _log(struct mLogger* logger, int category, enum mLogLevel level, const char* format, va_list args) {
	(void) logger;
	if (category == -1) {
		return;
	}
	if (getenv("DEBUG_SIO") && !strcmp(mLogCategoryName(category), "GBA Serial I/O") && (level & mLOG_DEBUG)) {
		fprintf(stderr, "  dbg: ");
		vfprintf(stderr, format, args);
		fprintf(stderr, "\n");
		return;
	}
	if (!(level & (mLOG_WARN | mLOG_ERROR | mLOG_FATAL | mLOG_INFO))) {
		return;
	}
	if (level == mLOG_INFO && strcmp(mLogCategoryName(category), "GBA Serial I/O")) {
		return;
	}
	fprintf(stderr, "  log[%s]: ", mLogCategoryName(category));
	vfprintf(stderr, format, args);
	fprintf(stderr, "\n");
}

static struct mLogger logger = { .log = _log };

static int64_t now(void) {
	struct timeval tv;
	gettimeofday(&tv, 0);
	return tv.tv_sec * 1000000LL + tv.tv_usec;
}

int main(int argc, char** argv) {
	if (argc < 3) {
		fprintf(stderr, "usage: %s host|join rom [address] [count gap mode childDelay [port]]\n", argv[0]);
		return 1;
	}
	bool host = !strcmp(argv[1], "host");
	const char* rom = argv[2];
	int arg = 3;
	const char* address = NULL;
	if (!host) {
		address = argv[arg++];
	}
	uint32_t count = argc > arg ? strtoul(argv[arg++], 0, 0) : 500;
	uint32_t gap = argc > arg ? strtoul(argv[arg++], 0, 0) : 3000;
	uint32_t mode = argc > arg ? strtoul(argv[arg++], 0, 0) : 0;
	uint32_t childDelay = argc > arg ? strtoul(argv[arg++], 0, 0) : 50;
	int port = argc > arg ? atoi(argv[arg++]) : GBA_NETLINK_DEFAULT_PORT;
	int maxFrames = getenv("MAX_FRAMES") ? atoi(getenv("MAX_FRAMES")) : 20000;
	int pauseAt = getenv("PAUSE_AT") ? atoi(getenv("PAUSE_AT")) : -1;
	int quitAt = getenv("QUIT_AT") ? atoi(getenv("QUIT_AT")) : -1;
	bool throttle = getenv("THROTTLE") != NULL;

	mLogSetDefaultLogger(&logger);
	struct mCore* core = mCoreFind(rom);
	if (!core || !core->init(core)) {
		fprintf(stderr, "could not create core\n");
		return 1;
	}
	mCoreInitConfig(core, NULL);
	core->opts.skipBios = true;
	core->opts.useBios = false;
	mCoreConfigSetDefaultIntValue(&core->config, "skipBios", 1);
	mCoreLoadConfig(core);
	static mColor video[256 * 224];
	core->setVideoBuffer(core, video, 256);
	if (!mCoreLoadFile(core, rom)) {
		fprintf(stderr, "could not load rom\n");
		return 1;
	}
	core->reset(core);

	struct GBASIONetLink link;
	GBASIONetLinkCreate(&link);
	core->setPeripheral(core, mPERIPH_GBA_LINK_PORT, &link.d);

	if (host) {
		if (!GBASIONetLinkHost(&link, port)) {
			fprintf(stderr, "host failed: %s\n", GBASIONetLinkGetError(&link));
			return 1;
		}
	} else if (!GBASIONetLinkConnect(&link, address, port)) {
		fprintf(stderr, "connect failed: %s\n", GBASIONetLinkGetError(&link));
		return 1;
	}
	int64_t start = now();
	while (GBASIONetLinkGetState(&link) != GBA_NETLINK_CONNECTED) {
		GBASIONetLinkUpdate(&link, 100);
		if (GBASIONetLinkGetState(&link) == GBA_NETLINK_ERROR || now() - start > 15000000) {
			fprintf(stderr, "handshake failed: %s\n", GBASIONetLinkGetError(&link));
			return 1;
		}
	}
	fprintf(stderr, "[%s] connected, partner game %s\n", argv[1], link.peerGame);

	core->busWrite32(core, 0x02000104, count);
	core->busWrite32(core, 0x02000108, gap);
	core->busWrite32(core, 0x0200010C, mode);
	core->busWrite32(core, 0x02000110, childDelay);
	core->busWrite32(core, 0x02000100, 0x50415241);

	start = now();
	int frame;
	for (frame = 0; frame < maxFrames; ++frame) {
		if (frame == quitAt) {
			fprintf(stderr, "[%s] quitting at frame %d\n", argv[1], frame);
			GBASIONetLinkDisconnect(&link);
			return 0;
		}
		if (frame == pauseAt) {
			fprintf(stderr, "[%s] pausing for 3s at frame %d\n", argv[1], frame);
			GBASIONetLinkSetPaused(&link, true);
			int64_t pauseStart = now();
			while (now() - pauseStart < 3000000) {
				GBASIONetLinkUpdate(&link, 50);
			}
			GBASIONetLinkSetPaused(&link, false);
		}
		GBASIONetLinkEnsureScheduled(&link);
		core->runFrame(core);
		if (throttle) {
			// Emulate a frame limiter: never run faster than ~59.73 fps
			int64_t target = start + (int64_t) ((frame + 1) * 16742.7);
			int64_t wait = target - now();
			if (wait > 0) {
				usleep(wait);
			}
		}
		if (core->busRead32(core, 0x02000008)) {
			// Keep running briefly so the partner can finish its last transfer
			int linger;
			for (linger = 0; linger < 30; ++linger) {
				GBASIONetLinkEnsureScheduled(&link);
				core->runFrame(core);
			}
			break;
		}
	}
	int64_t elapsed = now() - start;
	uint32_t role = core->busRead32(core, 0x0200000C);
	uint32_t completed = core->busRead32(core, 0x02000010);
	uint32_t errors = core->busRead32(core, 0x02000014);
	printf("[%s] role=%s done=%u completed=%u/%u errors=%u", argv[1], role ? "child" : "parent",
	       core->busRead32(core, 0x02000008), completed, count, errors);
	if (errors) {
		printf(" first@%u got=%08X want=%08X", core->busRead32(core, 0x02000018),
		       core->busRead32(core, 0x0200001C), core->busRead32(core, 0x02000020));
	}
	printf(" frames=%d wall=%.2fs fps=%.1f state=%d err='%s'\n", frame, elapsed / 1e6, frame / (elapsed / 1e6),
	       GBASIONetLinkGetState(&link), GBASIONetLinkGetError(&link));
	printf("[%s] stats: transfers=%u replyWaits=%u driftWaits=%u missedReplies=%u waited=%.2fs\n", argv[1],
	       link.stats.transfers, link.stats.replyWaits, link.stats.driftWaits, link.stats.missedReplies,
	       link.stats.waitMicros / 1e6);
	GBASIONetLinkDisconnect(&link);
	core->setPeripheral(core, mPERIPH_GBA_LINK_PORT, NULL);
	core->deinit(core);
	return errors || completed != count ? 2 : 0;
}
