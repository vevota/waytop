#ifndef PLAYER_H
#define PLAYER_H

#include <mpv/client.h>
#include <mpv/render_gl.h>
#include <EGL/egl.h>

struct player {
    mpv_handle *mpv;
    mpv_render_context *render_ctx;
    int width;
    int height;
    int *wakeup_fd;
};

struct player *player_create(const char *url, int width, int height);
void player_set_wakeup_fd(struct player *pl, int *fd);
void player_destroy(struct player *pl);
void player_set_size(struct player *pl, int width, int height);
void player_render(struct player *pl);
int player_update(struct player *pl);
int player_poll_event(struct player *pl, mpv_event **ev);

#endif
