#ifndef UI_H
#define UI_H

struct player;
struct overlay;

#define UI_BAR_H 40
#define UI_BTN_S 36
#define UI_MARGIN 8

void ui_draw(struct overlay *ov, struct player *pl, int unlocked);
int ui_hit_play(struct overlay *ov, int x, int y);
int ui_hit_seek(struct overlay *ov, int x, int y, int *out_pos);

#endif
