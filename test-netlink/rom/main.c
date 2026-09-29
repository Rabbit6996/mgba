#include <stdint.h>

#define REG16(x) (*(volatile uint16_t*) (0x04000000 + (x)))
#define REG_SIODATA32_LO 0x120
#define REG_SIODATA32_HI 0x122
#define REG_SIOMULTI0 0x120
#define REG_SIOMULTI1 0x122
#define REG_SIOCNT 0x128
#define REG_SIOMLT_SEND 0x12A
#define REG_RCNT 0x134
#define REG_IF 0x202

struct Params {
	volatile uint32_t magic;
	volatile uint32_t count;
	volatile uint32_t gap;
	volatile uint32_t mode; // 0 = multi, 1 = normal32
	volatile uint32_t childDelay;
};

struct Results {
	volatile uint32_t magic;
	volatile uint32_t started;
	volatile uint32_t done;
	volatile uint32_t role; // 0 parent, 1 child
	volatile uint32_t completed;
	volatile uint32_t errors;
	volatile uint32_t firstError;
	volatile uint32_t firstErrorGot;
	volatile uint32_t firstErrorWant;
};

#define PARAMS ((struct Params*) 0x02000100)
#define RESULTS ((struct Results*) 0x02000000)

static void spin(uint32_t n) {
	while (n--) {
		__asm__ volatile("");
	}
}

static void error(uint32_t index, uint32_t got, uint32_t want) {
	if (!RESULTS->errors) {
		RESULTS->firstError = index;
		RESULTS->firstErrorGot = got;
		RESULTS->firstErrorWant = want;
	}
	++RESULTS->errors;
}

static void multiParent(uint32_t count, uint32_t gap) {
	uint32_t i;
	for (i = 0; i < count; ++i) {
		REG16(REG_SIOMLT_SEND) = 0x1000 + i;
		REG16(REG_SIOCNT) |= 0x80;
		while (REG16(REG_SIOCNT) & 0x80);
		uint16_t own = REG16(REG_SIOMULTI0);
		uint16_t other = REG16(REG_SIOMULTI1);
		if (own != (uint16_t) (0x1000 + i)) {
			error(i, own, 0x1000 + i);
		}
		if (other != (uint16_t) (0x2000 + i)) {
			error(i, other, 0x2000 + i);
		}
		RESULTS->completed = i + 1;
		spin(gap);
	}
}

static void multiChild(uint32_t count, uint32_t delay) {
	uint32_t k = 0;
	REG16(REG_SIOMLT_SEND) = 0x2000;
	while (k < count) {
		while (!(REG16(REG_IF) & 0x80));
		REG16(REG_IF) = 0x80;
		uint16_t parent = REG16(REG_SIOMULTI0);
		uint16_t own = REG16(REG_SIOMULTI1);
		if (parent != (uint16_t) (0x1000 + k)) {
			error(k, parent, 0x1000 + k);
		}
		if (own != (uint16_t) (0x2000 + k)) {
			error(k, own, 0x2000 + k);
		}
		++k;
		spin(delay);
		REG16(REG_SIOMLT_SEND) = 0x2000 + k;
		RESULTS->completed = k;
	}
}

static void normalParent(uint32_t count, uint32_t gap) {
	uint32_t i;
	REG16(REG_SIOCNT) = 0x5001; // normal32, internal clock, IRQ
	for (i = 0; i < count; ++i) {
		uint32_t out = 0xA0000000 | i;
		REG16(REG_SIODATA32_LO) = out;
		REG16(REG_SIODATA32_HI) = out >> 16;
		REG16(REG_SIOCNT) |= 0x80;
		while (REG16(REG_SIOCNT) & 0x80);
		uint32_t in = REG16(REG_SIODATA32_LO) | (REG16(REG_SIODATA32_HI) << 16);
		if (in != (0xB0000000 | i)) {
			error(i, in, 0xB0000000 | i);
		}
		RESULTS->completed = i + 1;
		spin(gap);
	}
}

static void normalChild(uint32_t count, uint32_t delay) {
	uint32_t k = 0;
	REG16(REG_SIOCNT) = 0x5000; // normal32, external clock, IRQ
	REG16(REG_SIODATA32_LO) = 0x0000;
	REG16(REG_SIODATA32_HI) = 0xB000;
	REG16(REG_SIOCNT) |= 0x80;
	while (k < count) {
		while (!(REG16(REG_IF) & 0x80));
		REG16(REG_IF) = 0x80;
		uint32_t in = REG16(REG_SIODATA32_LO) | (REG16(REG_SIODATA32_HI) << 16);
		if (in != (0xA0000000 | k)) {
			error(k, in, 0xA0000000 | k);
		}
		++k;
		spin(delay);
		uint32_t out = 0xB0000000 | k;
		REG16(REG_SIODATA32_LO) = out;
		REG16(REG_SIODATA32_HI) = out >> 16;
		REG16(REG_SIOCNT) |= 0x80;
		RESULTS->completed = k;
	}
}

int main(void) {
	while (PARAMS->magic != 0x50415241);
	RESULTS->magic = 0x52534C54;
	REG16(REG_RCNT) = 0;
	REG16(REG_SIOCNT) = 0x6003; // multi, 115200 baud, IRQ
	// Wait for both sides to be in multiplayer mode
	while (!(REG16(REG_SIOCNT) & 0x8));
	spin(20000);
	uint32_t role = (REG16(REG_SIOCNT) >> 2) & 1;
	RESULTS->role = role;
	RESULTS->started = 1;
	REG16(REG_IF) = 0x80;
	if (PARAMS->mode == 0) {
		if (role) {
			multiChild(PARAMS->count, PARAMS->childDelay);
		} else {
			// Give the child time to get ready
			spin(50000);
			multiParent(PARAMS->count, PARAMS->gap);
		}
	} else {
		if (role) {
			normalChild(PARAMS->count, PARAMS->childDelay);
		} else {
			spin(50000);
			normalParent(PARAMS->count, PARAMS->gap);
		}
	}
	RESULTS->done = 1;
	while (1);
	return 0;
}
