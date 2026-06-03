#ifndef UI_H
#define UI_H

struct player;
struct overlay;

#define UI_BAR_H 40
#define UI_BTN_S 36
#define UI_MARGIN 8

#define UI_SEEK_S 30
#define UI_HOVER_TIMEOUT 999999999999ULL

void ui_draw(struct overlay *ov, struct player *pl, int unlocked);
int ui_hit_play(struct overlay *ov, int x, int y);
int ui_hit_seek(struct overlay *ov, int x, int y, int *out_pos);
int ui_hit_rewind(struct overlay *ov, int x, int y);
int ui_hit_forward(struct overlay *ov, int x, int y);
int ui_hit_prev(struct overlay *ov, int x, int y);
int ui_hit_next(struct overlay *ov, int x, int y);
int ui_hit_subs(struct overlay *ov, int x, int y);

#endif
