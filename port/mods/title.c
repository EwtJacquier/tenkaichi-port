// Mod: the title screen without its bottom line ("(C) BIRD STUDIO/SHUEISHA, TOEI ANIMATION. License
// coordinated by FUNimation Entertainment. All Rights Reserved."): a texture of its own (512x32, the
// texture pack's key fc6c12b4cd3bf663), drawn at the screen's bottom; made see-through as it is
// decoded, so neither it nor a replacement shows. DBZ_MOD_TITLE=0: shown.
#include <stdlib.h>
#include <string.h>

#include "rt.h"
#include "texpack.h"

#define LICENSE_KEY 0xfc6c12b4cd3bf663ull

void rt_texture_decoded(uint32_t *px, int w, int h) {
	if (w != 512 || h != 32) return;
	static int on = -1;
	if (on < 0) on = !getenv("DBZ_MOD_TITLE") || atoi(getenv("DBZ_MOD_TITLE")) != 0;
	if (!on || texpack_key(px, w, w, h) != LICENSE_KEY) return;
	memset(px, 0, sizeof *px * (size_t)w * h);
	static int logged;
	if (!logged++) rt_log("title: the license line hidden");
}
