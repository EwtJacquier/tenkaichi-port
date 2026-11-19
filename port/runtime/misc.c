// Smaller system functions: the clocks' frequencies (scePower), the language and buttons
// (sceImpose), the display's line counter, the controller's idle threshold, dates and ticks
// (sceRtc: microseconds since 0001-01-01), the console's id.
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "hle.h"
#include "rt.h"

// ---- scePower: the clocks the game asks for (it waits until the PLL reports them); the speed here
// doesn't change ----------------------------------------------------------------------------------
static uint32_t pll_mhz = 222, cpu_mhz = 222, bus_mhz = 111;  // the PSP's at boot
void hle_scePowerGetCpuClockFrequencyInt(CPU *c) { RET(cpu_mhz); }
void hle_scePowerGetPllClockFrequencyInt(CPU *c) { RET(pll_mhz); }
void hle_scePowerGetBusClockFrequencyInt(CPU *c) { RET(bus_mhz); }
void hle_scePowerSetClockFrequency350(CPU *c) {  // (pll, cpu, bus)
	if (A0 < 19 || A0 > 333 || A1 < 1 || A1 > 333 || A2 < 1 || A2 > 167) {
		RET(0x80000107u);  // SCE_ERROR_INVALID_VALUE
		return;
	}
	pll_mhz = A0;
	cpu_mhz = A1;
	bus_mhz = A2;
	RET(0);
}

// ---- sceImpose: (u32 *language, u32 *buttons): English, cross confirms (as GetSystemParamInt) -----
void hle_sceImposeGetLanguageMode(CPU *c) {
	if (A0) M32(A0) = 1;
	if (A1) M32(A1) = 1;
	RET(0);
}

// ---- the display's current line (286 a frame, from the last vblank: kernel.c) --------------------
uint32_t rt_current_hcount(void);
void hle_sceDisplayGetCurrentHcount(CPU *c) { RET(rt_current_hcount()); }

void hle_sceCtrlSetIdleCancelThreshold(CPU *c) { RET(0); }

// ---- sceRtc ----------------------------------------------------------------------------------------
// ScePspDateTime: u16 year, month, day, hour, minute, second; u32 microsecond.
#define TICKS_PER_DAY 86400000000ull

static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {  // days since 1970-01-01
	y -= m <= 2;
	int64_t era = (y >= 0 ? y : y - 399) / 400;
	unsigned yoe = (unsigned)(y - era * 400);
	unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + (int64_t)doe - 719468;
}
static void civil_from_days(int64_t z, int *y, unsigned *m, unsigned *d) {
	z += 719468;
	int64_t era = (z >= 0 ? z : z - 146096) / 146097;
	unsigned doe = (unsigned)(z - era * 146097);
	unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	int64_t yy = (int64_t)yoe + era * 400;
	unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	unsigned mp = (5 * doy + 2) / 153;
	*d = doy - (153 * mp + 2) / 5 + 1;
	*m = mp < 10 ? mp + 3 : mp - 9;
	*y = (int)(yy + (*m <= 2));
}
static const int64_t EPOCH_DAYS = 719162;  // 0001-01-01 to 1970-01-01

static uint64_t date_to_tick(uint32_t p) {
	int64_t days = days_from_civil(M16(p), M16(p + 2), M16(p + 4)) + EPOCH_DAYS;
	return (uint64_t)days * TICKS_PER_DAY + ((uint64_t)M16(p + 6) * 3600 + M16(p + 8) * 60 + M16(p + 10)) * 1000000ull + M32(p + 12);
}
static void tick_to_date(uint64_t t, uint32_t p) {
	int y;
	unsigned m, d;
	civil_from_days((int64_t)(t / TICKS_PER_DAY) - EPOCH_DAYS, &y, &m, &d);
	uint64_t r = t % TICKS_PER_DAY;
	M16(p) = (uint16_t)y;
	M16(p + 2) = (uint16_t)m;
	M16(p + 4) = (uint16_t)d;
	M16(p + 6) = (uint16_t)(r / 3600000000ull);
	M16(p + 8) = (uint16_t)(r / 60000000ull % 60);
	M16(p + 10) = (uint16_t)(r / 1000000ull % 60);
	M32(p + 12) = (uint32_t)(r % 1000000ull);
}
static uint64_t rd64(uint32_t a) { return M32(a) | (uint64_t)M32(a + 4) << 32; }
static void wr64(uint32_t a, uint64_t v) {
	M32(a) = (uint32_t)v;
	M32(a + 4) = (uint32_t)(v >> 32);
}

void hle_sceRtcGetDaysInMonth(CPU *c) {  // (year, month)
	static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	uint32_t y = A0, m = A1;
	if (m < 1 || m > 12) {
		RET(0x80000102u);
		return;
	}
	bool leap = (y % 4 == 0 && y % 100) || y % 400 == 0;
	RET(days[m - 1] + (m == 2 && leap));
}
void hle_sceRtcGetDayOfWeek(CPU *c) {  // (year, month, day): 0 = Sunday
	int64_t d = days_from_civil(A0, A1, A2);  // 1970-01-01 was a Thursday
	RET((uint32_t)(((d % 7) + 7 + 4) % 7));
}
void hle_sceRtcGetTick(CPU *c) {  // (const date *, u64 *tick)
	wr64(A1, date_to_tick(A0));
	RET(0);
}
void hle_sceRtcSetTick(CPU *c) {  // (date *, const u64 *tick)
	tick_to_date(rd64(A1), A0);
	RET(0);
}
void hle_sceRtcGetTime_t(CPU *c) {  // (const date *, u32 *time_t)
	M32(A1) = (uint32_t)((date_to_tick(A0) - (uint64_t)EPOCH_DAYS * TICKS_PER_DAY) / 1000000ull);
	RET(0);
}
void hle_sceRtcGetCurrentClock(CPU *c) {  // (date *, int timezone minutes): UTC moved by the zone
	struct timeval tv;
	rt_wallclock(&tv);
	uint64_t t = ((uint64_t)tv.tv_sec + (uint64_t)EPOCH_DAYS * 86400ull) * 1000000ull + (uint64_t)tv.tv_usec;
	t += (int64_t)(int32_t)A1 * 60000000ll;
	tick_to_date(t, A0);
	RET(0);
}
void hle_sceRtcTickAddDays(CPU *c) {  // (u64 *dest, const u64 *src, int days)
	wr64(A0, rd64(A1) + (int64_t)(int32_t)A2 * (int64_t)TICKS_PER_DAY);
	RET(0);
}
void hle_sceRtcTickAddHours(CPU *c) {  // (u64 *dest, const u64 *src, int hours)
	wr64(A0, rd64(A1) + (int64_t)(int32_t)A2 * 3600000000ll);
	RET(0);
}
void hle_sceRtcTickAddMinutes(CPU *c) {  // (u64 *dest, const u64 *src, s64 minutes in a2:a3)
	int64_t n = (int64_t)(A2 | (uint64_t)A3 << 32);
	wr64(A0, rd64(A1) + n * 60000000ll);
	RET(0);
}
void hle_sceRtcTickAddSeconds(CPU *c) {  // (u64 *dest, const u64 *src, s64 seconds in a2:a3)
	int64_t n = (int64_t)(A2 | (uint64_t)A3 << 32);
	wr64(A0, rd64(A1) + n * 1000000ll);
	RET(0);
}

// ---- sceOpenPSID: the console's id (16 bytes), the same on every run ------------------------------
void hle_sceOpenPSIDGetOpenPSID(CPU *c) {
	static const uint8_t id[16] = {0x10, 0x02, 0xa3, 0x44, 0x13, 0xf5, 0x93, 0xb0, 0xcc, 0x6e, 0xd1, 0x32, 0x27, 0x85, 0x0f, 0x9d};
	memcpy(MP(A0), id, 16);
	RET(0);
}

void hle_scePowerCheckWlanCoexistenceClock(CPU *c) { RET(1); }  // a PSP-2000/3000: 333 MHz with the wireless on
void hle_scePower_469989ad(CPU *c) { hle_scePowerSetClockFrequency350(c); }  // the same (pll, cpu, bus)
void hle_sceImposeSetUMDPopup(CPU *c) { RET(0); }
