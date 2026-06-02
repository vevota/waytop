#ifndef UI_H
#define UI_H

struct player;
struct overlay;

#define UI_BAR_H 40
#define UI_BTN_S 36
#define UI_MARGIN 8

#define UI_SEEK_S 30
#define UI_HOVER_TIMEOUT 2000000000ULL

void ui_draw(struct overlay *ov, struct player *pl, int unlocked);
int ui_hit_play(struct overlay *ov, int x, int y);
int ui_hit_seek(struct overlay *ov, int x, int y, int *out_pos);
int ui_hit_rewind(struct overlay *ov, int x, int y);
int ui_hit_forward(struct overlay *ov, int x, int y);

#endif
