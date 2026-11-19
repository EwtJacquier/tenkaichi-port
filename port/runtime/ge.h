// GE renderer interface (ge.c).
#pragma once
#include <GLES3/gl3.h>
#include <stdbool.h>
#include <stdint.h>

void ge_init(void);
void ge_command(uint32_t cmd, uint32_t data, uint32_t addr);
// BOUNDINGBOX: whether any of the next count vertices can be on screen (the GE's test; BJUMP
// skips the drawing that follows when not). The vertex (or index) address moves past them.
bool ge_bounding_box(uint32_t count);
// Texture showing the displayed framebuffer; *src_w x *src_h is its visible 480x272 area in texels.
GLuint ge_display_texture(uint32_t fb_addr, int stride, int fmt, int *src_w, int *src_h);
void ge_unbind_target(void);
void ge_draw_stats(uint64_t *draws, uint64_t *batches);
uint64_t ge_draws_total(void);  // draws since the start
void ge_hitch(uint64_t frame_us);  // DBZ_HITCH=ms: where a slow frame's time went
int ge_render_scale(void);
void ge_set_render_scale(int n);  // 1..6, takes effect immediately
void ge_set_tex_filter(int mode);
int ge_tex_filter(void);  // the game's own textures: 0 GE's own filtering, 1 xBR, 2 sharp pixels
void ge_set_ui_filter(int mode);  // 2D interface filter: 0 GE's own, 1 xBR, 2 bicubic
void ge_forget_textures(void);  // every texture looked up and drawn again (texture pack, text drawing)
void ge_frame_mark(void);  // called at sceDisplaySetFrameBuf
bool ge_dump_finished(void);  // a debug capture just ended
extern uint32_t ge_offset_addr;
