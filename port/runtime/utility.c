// sceUtility: system parameters and the savedata utility (files under ms0:/PSP/SAVEDATA).
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hle.h"
#include "platform.h"
#include "rt.h"

void hle_sceUtilityLoadModule(CPU *c) { RET(0); }
void hle_sceUtilityUnloadModule(CPU *c) { RET(0); }

void hle_sceUtilityGetSystemParamInt(CPU *c) {
	uint32_t v;
	switch (A0) {
	case 2: v = 0; break;    // adhoc channel: automatic
	case 3: v = 0; break;    // wlan power save
	case 4: v = 1; break;    // date format MM/DD/YYYY
	case 5: v = 0; break;    // 24h clock
	case 6: v = 0; break;    // timezone offset (minutes)
	case 7: v = 0; break;    // daylight saving
	case 8: v = 1; break;    // language: English
	case 9: v = 1; break;    // confirm button: cross (US)
	default:
		RET(0x80110103u);
		return;
	}
	M32(A1) = v;
	RET(0);
}

void hle_sceUtilityGetSystemParamString(CPU *c) {
	if (A0 == 1) {  // nickname
		const char *nick = getenv("DBZ_NICK");
		rt_write_cstr(A1, A2, nick ? nick : "Player");
		RET(0);
		return;
	}
	RET(0x80110103u);
}

// ---- savedata ---------------------------------------------------------------------------------
// SceUtilitySavedataParam offsets (PSP SDK layout).
enum {
	SD_RESULT = 0x1C, SD_MODE = 0x30, SD_GAMENAME = 0x3C, SD_SAVENAME = 0x4C, SD_FILENAME = 0x64,
	SD_DATABUF = 0x74, SD_DATABUFSIZE = 0x78, SD_DATASIZE = 0x7C,
	SD_MSFREE = 0x5D0, SD_MSDATA = 0x5D4, SD_UTILDATA = 0x5D8, SD_IDLIST = 0x5F4, SD_FILELIST = 0x5F8, SD_SIZEINFO = 0x5FC,
};
enum { ST_NONE = 0, ST_INIT = 1, ST_RUNNING = 2, ST_FINISHED = 3, ST_SHUTDOWN = 4 };
static int sd_status = ST_NONE;
static uint32_t sd_param;

static void save_dir(char *out, size_t cap) {
	const char *ms0 = getenv("DBZ_MS0");
	snprintf(out, cap, "%s/PSP/SAVEDATA/%.13s%.20s", ms0 ? ms0 : "build/ms0", rt_str(sd_param + SD_GAMENAME),
	         rt_str(sd_param + SD_SAVENAME));
}

// Space on the memory stick: plenty (1 GB free), in 32 KB clusters as the PSP counts.
#define MS_CLUSTER 0x8000u
static void space_text(uint64_t bytes, char out[8]) {  // "1000 KB" / "50 MB", as the system writes it
	char t[16];
	if (bytes < 1024 * 1024) snprintf(t, sizeof t, "%u KB", (unsigned)((bytes + 1023) / 1024));
	else snprintf(t, sizeof t, "%u MB", (unsigned)((bytes + 1024 * 1024 - 1) / (1024 * 1024)));
	memset(out, 0, 8);
	memcpy(out, t, strlen(t) < 7 ? strlen(t) : 7);
}
static void used_info(uint32_t info, uint32_t clusters) {  // SceUtilitySavedataUsedDataInfo
	uint64_t bytes = (uint64_t)clusters * MS_CLUSTER;
	M32(info + 0) = clusters;
	M32(info + 4) = (uint32_t)(bytes / 1024);
	space_text(bytes, (char *)MP(info + 8));
	M32(info + 16) = (uint32_t)(bytes / 1024);
	space_text(bytes, (char *)MP(info + 20));
}
static uint32_t dir_clusters(const char *dir, bool *exists) {
	DIR *d = opendir(dir);
	*exists = d != NULL;
	if (!d) return 0;
	uint32_t n = 0;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		char f[1100];
		struct stat st;
		snprintf(f, sizeof f, "%s/%s", dir, e->d_name);
		if (stat(f, &st) == 0) n += (uint32_t)((st.st_size + MS_CLUSTER - 1) / MS_CLUSTER);
	}
	closedir(d);
	return n;
}
static uint32_t savedata_sizes(uint32_t p, const char *dir) {
	uint32_t ret = 0;
	uint32_t ms_free = M32(p + SD_MSFREE), ms_data = M32(p + SD_MSDATA), util = M32(p + SD_UTILDATA);
	if (ms_free) {  // SceUtilitySavedataMsFreeInfo
		uint64_t free_bytes = 1024ull * 1024 * 1024;
		M32(ms_free + 0) = MS_CLUSTER;
		M32(ms_free + 4) = (uint32_t)(free_bytes / MS_CLUSTER);
		M32(ms_free + 8) = (uint32_t)(free_bytes / 1024);
		space_text(free_bytes, (char *)MP(ms_free + 12));
	}
	if (ms_data) {  // SceUtilitySavedataMsDataInfo: game name[13], pad, save name[20], used info
		char path[800];
		const char *ms0 = getenv("DBZ_MS0");
		const char *save = rt_str(ms_data + 16);
		snprintf(path, sizeof path, "%s/PSP/SAVEDATA/%.13s%.20s", ms0 ? ms0 : "build/ms0", rt_str(ms_data),
		         strcmp(save, "<>") ? save : "");
		bool exists;
		uint32_t n = dir_clusters(path, &exists);
		used_info(ms_data + 36, n);
		if (!exists) ret = 0x801103C7u;  // sizes: no data
	}
	if (util) {  // what this save would take: folder, SFO, the data
		uint32_t n = 2;
		if (rt_str(p + SD_FILENAME)[0]) n += (M32(p + SD_DATASIZE) + MS_CLUSTER - 1) / MS_CLUSTER;
		used_info(util, n);
	}
	(void)dir;
	return ret;
}
static bool name_match(const char *t, const char *re) {  // the system's: '*' any run, '?' any one
	if (!*re) return !*t;
	if (*re == '*') return name_match(t, re + 1) || (*t && name_match(t + 1, re));
	if (!*t) return false;
	if (*re == '?' || *re == *t) return name_match(t + 1, re + 1);
	return false;
}
static void savedata_list(uint32_t p) {  // SceUtilitySavedataIdListInfo: max, result count, entries (0x48 each)
	uint32_t list = M32(p + SD_IDLIST);
	if (!list) return;
	uint32_t max = M32(list), entries = M32(list + 8), n = 0;
	char root[700], want[64];
	const char *ms0 = getenv("DBZ_MS0");
	const char *game = rt_str(p + SD_GAMENAME), *save = rt_str(p + SD_SAVENAME);
	snprintf(root, sizeof root, "%s/PSP/SAVEDATA", ms0 ? ms0 : "build/ms0");
	snprintf(want, sizeof want, "%.13s%.20s", game, strcmp(save, "<>") ? save : "");
	DIR *d = opendir(root);
	struct dirent *e;
	while (d && (e = readdir(d)) && n < max) {
		if (e->d_name[0] == '.' || !name_match(e->d_name, want)) continue;
		uint32_t en = entries + n * 0x48;
		memset(MP(en), 0, 0x48);
		M32(en) = 0x11FF;  // a folder
		for (int i = 0; i < 3; i++) {  // its times: 2009-11-09
			M16(en + 4 + i * 16) = 2009;
			M16(en + 6 + i * 16) = 11;
			M16(en + 8 + i * 16) = 9;
		}
		rt_write_cstr(en + 0x34, 20, e->d_name + strlen(game));
		n++;
	}
	if (d) closedir(d);
	M32(list + 4) = n;
	rt_log("savedata list %s: %u", want, n);
}
static uint32_t savedata_files(uint32_t p, const char *dir) {
	// SceUtilitySavedataFileListInfo: max secure, max normal, max system, secure count, normal count,
	// system count, the three entry arrays (0x40 each: mode, size u64, times, name[16]).
	uint32_t fl = M32(p + SD_FILELIST);
	if (!fl) return 0;
	DIR *d = opendir(dir);
	if (!d) return 0x80110327u;
	uint32_t max_normal = M32(fl + 4), normal = M32(fl + 28), n = 0;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.' || !strcmp(e->d_name, "PARAM.SFO")) continue;
		if (n >= max_normal || !normal) break;
		char f[1100];
		struct stat st;
		snprintf(f, sizeof f, "%s/%s", dir, e->d_name);
		if (stat(f, &st)) continue;
		uint32_t en = normal + n * 0x40;
		memset(MP(en), 0, 0x40);
		M32(en) = 0x21FF;
		M32(en + 8) = (uint32_t)st.st_size;
		rt_write_cstr(en + 0x30, 16, e->d_name);
		n++;
	}
	closedir(d);
	M32(fl + 12) = 0;
	M32(fl + 16) = n;
	M32(fl + 20) = 0;
	return 0;
}

// Mods may change a save as it is read (a player's name in it, ...): (file name, data, size).
__attribute__((weak)) void rt_savedata_loaded(const char *name, uint32_t buf, uint32_t size) {}

static uint32_t savedata_run(void) {
	uint32_t p = sd_param;
	uint32_t mode = M32(p + SD_MODE);
	char dir[700], file[800];
	save_dir(dir, sizeof dir);
	snprintf(file, sizeof file, "%s/%.13s", dir, rt_str(p + SD_FILENAME));
	rt_log("savedata mode=%d game=%s save=%s file=%s", mode, rt_str(p + SD_GAMENAME), rt_str(p + SD_SAVENAME),
	       rt_str(p + SD_FILENAME));
	switch (mode) {
	case 0: case 2: case 4: {  // (auto/list) load
		FILE *f = fopen(file, "rb");
		if (!f) {
			return 0x80110307u;  // load: no data
		}
		size_t n = fread(MP(M32(p + SD_DATABUF)), 1, M32(p + SD_DATABUFSIZE), f);
		fclose(f);
		M32(p + SD_DATASIZE) = (uint32_t)n;
		rt_savedata_loaded(rt_str(p + SD_FILENAME), M32(p + SD_DATABUF), (uint32_t)n);
		return 0;
	}
	case 1: case 3: case 5: {  // (auto/list) save
		mkdir(dir, 0755);
		FILE *f = fopen(file, "wb");
		if (!f) {
			return 0x80110385u;
		}
		fwrite(MP(M32(p + SD_DATABUF)), 1, M32(p + SD_DATASIZE), f);
		fclose(f);
		plat_persist();
		return 0;
	}
	case 15: case 16: {  // read data (silently)
		FILE *f = fopen(file, "rb");
		if (!f) return 0x80110327u;  // read/write: no data
		size_t n = fread(MP(M32(p + SD_DATABUF)), 1, M32(p + SD_DATABUFSIZE), f);
		fclose(f);
		M32(p + SD_DATASIZE) = (uint32_t)n;
		rt_savedata_loaded(rt_str(p + SD_FILENAME), M32(p + SD_DATABUF), (uint32_t)n);
		return 0;
	}
	case 13: case 14: case 17: case 18: {  // make / write data (silently)
		mkdir(dir, 0755);
		FILE *f = fopen(file, "wb");
		if (!f) return 0x80110385u;
		fwrite(MP(M32(p + SD_DATABUF)), 1, M32(p + SD_DATASIZE), f);
		fclose(f);
		plat_persist();
		return 0;
	}
	case 19: case 20:  // erase a file of the save
		if (remove(file) != 0) return 0x80110347u;
		plat_persist();
		return 0;
	case 9: case 10: case 21: {  // delete the save (its folder)
		DIR *d = opendir(dir);
		if (!d) return 0x80110347u;
		struct dirent *e;
		while ((e = readdir(d))) {
			if (e->d_name[0] == '.') continue;
			char f2[1100];
			snprintf(f2, sizeof f2, "%s/%s", dir, e->d_name);
			remove(f2);
		}
		closedir(d);
		rmdir(dir);
		plat_persist();
		return 0;
	}
	case 8:  // sizes
		return savedata_sizes(p, dir);
	case 11:  // list the saves
		savedata_list(p);
		return 0;
	case 12:  // the save's files
		return savedata_files(p, dir);
	case 22: {  // the size the data needs
		uint32_t info = M32(p + SD_SIZEINFO);
		if (info) {
			uint32_t need = 3 + (M32(p + SD_DATASIZE) + 0x7FFF) / 0x8000;  // folder, SFO and data clusters
			M32(info + 0) = need;  // needed clusters
			M32(info + 4) = 0;     // ...
		}
		return 0;
	}
	default:
		rt_log("savedata mode %d not implemented yet", mode);
		return 0;
	}
}

void hle_sceUtilitySavedataInitStart(CPU *c) {
	sd_param = A0;
	sd_status = ST_RUNNING;
	RET(0);
}
void hle_sceUtilitySavedataGetStatus(CPU *c) {
	int s = sd_status;
	if (sd_status == ST_SHUTDOWN) {
		sd_status = ST_NONE;
	}
	RET(s);
}
void hle_sceUtilitySavedataUpdate(CPU *c) {
	if (sd_status == ST_RUNNING) {
		M32(sd_param + SD_RESULT) = savedata_run();
		sd_status = ST_FINISHED;
	}
	RET(0);
}
void hle_sceUtilitySavedataShutdownStart(CPU *c) {
	sd_status = ST_SHUTDOWN;
	RET(0);
}

// ---- the message dialog: answered at once (YES / OK), its text in the log -------------------------
// pspUtilityMsgDialogParams: the common 0x30 bytes (result at 0x1c), mode 0x30 (0 error, 1 text),
// error 0x34, message[512] 0x38, options 0x238, button pressed 0x23c (1 yes/ok, 2 no, 3 back).
static int md_status;
static uint32_t md_param;
void hle_sceUtilityMsgDialogInitStart(CPU *c) {
	md_param = A0;
	if (M32(A0 + 0x30) == 0) rt_log("message dialog: error %08x (size %x)", M32(A0 + 0x34), M32(A0));
	else rt_log("message dialog: \"%.200s\" (options %x)", rt_str(A0 + 0x38), M32(A0 + 0x238));
	md_status = 2;
	RET(0);
}
void hle_sceUtilityMsgDialogUpdate(CPU *c) {
	if (md_status == 2) {
		if (M32(md_param) >= 0x240) M32(md_param + 0x23c) = 1;
		M32(md_param + 0x1c) = 0;
		md_status = 3;
	}
	RET(0);
}
void hle_sceUtilityMsgDialogGetStatus(CPU *c) {
	int s = md_status;
	if (md_status == 4) md_status = 0;
	RET(s);
}
void hle_sceUtilityMsgDialogShutdownStart(CPU *c) {
	md_status = 4;
	RET(0);
}
void hle_sceUtilityMsgDialogAbort(CPU *c) {
	if (md_status == 2) {
		if (M32(md_param) >= 0x240) M32(md_param + 0x23c) = 3;
		md_status = 3;
	}
	RET(0);
}

// ---- the network dialog: only Ad Hoc (joins or makes the group through sceNetAdhocctl) ------------
// pspUtilityNetconfData: the common 0x30 bytes, action 0x30 (2 connect, 4 create, 5 join an Ad Hoc
// group; 0/1/3 an access point), Ad Hoc parameters 0x34 (u8 group name[8], u32 timeout).
static int nc_status;
static uint32_t nc_param;
void hle_sceUtilityNetconfInitStart(CPU *c) {
	nc_param = A0;
	nc_status = 2;
	RET(0);
}
void hle_sceUtilityNetconfUpdate(CPU *c) {
	if (nc_status == 2) {
		uint32_t action = M32(nc_param + 0x30), adhoc = M32(nc_param + 0x34);
		uint32_t result = 0;
		if ((action == 2 || action == 4 || action == 5) && adhoc) {
			CPU call = *c;
			call.r[4] = adhoc;  // the group's name
			hle_sceNetAdhocctlConnect(&call);
			result = call.r[2];
		}
		else {
			rt_log("network dialog: action %u (an access point): not available, Ad Hoc only", action);
			result = 0x80110601u;  // canceled
		}
		M32(nc_param + 0x1c) = result;
		nc_status = 3;
	}
	RET(0);
}
void hle_sceUtilityNetconfGetStatus(CPU *c) {
	int s = nc_status;
	if (nc_status == 4) nc_status = 0;
	RET(s);
}
void hle_sceUtilityNetconfShutdownStart(CPU *c) {
	nc_status = 4;
	RET(0);
}

// ---- the on-screen keyboard: answered at once with the text it was given (or DBZ_NICK for an
// empty one), so a name asked for is the player's. SceUtilityOskParams: the common 0x30 bytes,
// data count 0x30, data 0x34 (SceUtilityOskData: description 0x1c, in text 0x20, out length 0x24,
// out text 0x28, result 0x2c, out limit 0x30; UTF-16 strings).
static int osk_status;
static uint32_t osk_param;
void hle_sceUtilityOskInitStart(CPU *c) {
	osk_param = A0;
	osk_status = 2;
	RET(0);
}
void hle_sceUtilityOskUpdate(CPU *c) {
	if (osk_status == 2) {
		uint32_t n = M32(osk_param + 0x30), d = M32(osk_param + 0x34);
		for (uint32_t k = 0; k < n && k < 4 && rt_valid(d + k * 0x34, 0x34); k++) {
			uint32_t e = d + k * 0x34, in = M32(e + 0x20), out = M32(e + 0x28), len = M32(e + 0x24);
			uint32_t limit = M32(e + 0x30) ? M32(e + 0x30) : len;
			if (!out || !len) continue;
			uint32_t i = 0;
			if (in) for (; i + 1 < len && i < limit && M16(in + 2 * i); i++) M16(out + 2 * i) = M16(in + 2 * i);
			const char *nick = getenv("DBZ_NICK");
			if (!nick || !*nick) nick = "Player";
			rt_log("on-screen keyboard: data %u of %u, in %08x out %08x length %u limit %u", k, n, in, out, len, limit);
			if (!i) for (; i + 1 < len && i < limit && nick[i]; i++) M16(out + 2 * i) = (uint8_t)nick[i];
			M16(out + 2 * i) = 0;
			M32(e + 0x2c) = 2;  // changed
			char s[64];
			uint32_t k2 = 0;
			for (; k2 < 63 && M16(out + 2 * k2); k2++) s[k2] = (char)M16(out + 2 * k2);
			s[k2] = 0;
			rt_log("on-screen keyboard: answered \"%s\" (in text %04x %04x)", s, in ? M16(in) : 0, in ? M16(in + 2) : 0);
		}
		M32(osk_param + 0x1c) = 0;
		osk_status = 3;
	}
	RET(0);
}
void hle_sceUtilityOskGetStatus(CPU *c) {
	int s = osk_status;
	if (osk_status == 4) osk_status = 0;
	RET(s);
}
void hle_sceUtilityOskShutdownStart(CPU *c) {
	osk_status = 4;
	RET(0);
}

// ---- installing the game's data to the memory stick: not done (the disc is read directly and
// fast here); the dialog ends at once as canceled.
static int gi_status;
static uint32_t gi_param;
void hle_sceUtilityGamedataInstallInitStart(CPU *c) {
	gi_param = A0;
	gi_status = 2;
	rt_log("game data install asked: answered canceled (the disc is read directly)");
	RET(0);
}
void hle_sceUtilityGamedataInstallUpdate(CPU *c) {
	if (gi_status == 2) {
		M32(gi_param + 0x1c) = 0x80110601u;  // canceled
		gi_status = 3;
	}
	RET(0);
}
void hle_sceUtilityGamedataInstallGetStatus(CPU *c) {
	int s = gi_status;
	if (gi_status == 4) gi_status = 0;
	RET(s);
}
void hle_sceUtilityGamedataInstallShutdownStart(CPU *c) {
	gi_status = 4;
	RET(0);
}
void hle_sceUtilityGamedataInstallAbort(CPU *c) {
	if (gi_status == 2) gi_status = 3;
	RET(0);
}
