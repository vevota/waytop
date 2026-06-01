#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

#include "player.h"

static void *get_proc_address(void *ctx, const char *name) {
    (void)ctx;
    return (void *)eglGetProcAddress(name);
}

static void wakeup_cb(void *data) {
    int *fd = data;
    uint64_t val = 1;
    write(*fd, &val, sizeof(val));
}

struct player *player_create(const char *url, int width, int height) {
    struct player *pl = calloc(1, sizeof(*pl));
    pl->width = width;
    pl->height = height;
    pl->wakeup_fd = NULL;

    pl->mpv = mpv_create();
    if (!pl->mpv) {
        fprintf(stderr, "failed to create mpv handle\n");
        free(pl);
        return NULL;
    }

    mpv_set_option_string(pl->mpv, "vo", "libmpv");
    mpv_set_option_string(pl->mpv, "hwdec", "auto");
    mpv_set_option_string(pl->mpv, "keep-open", "yes");
    mpv_set_option_string(pl->mpv, "audio-display", "no");
    mpv_set_option_string(pl->mpv, "osc", "yes");
    mpv_set_option_string(pl->mpv, "load-scripts", "yes");
    mpv_set_option_string(pl->mpv, "terminal", "no");

    if (mpv_initialize(pl->mpv) < 0) {
        fprintf(stderr, "failed to initialize mpv\n");
        player_destroy(pl);
        return NULL;
    }

    mpv_opengl_init_params gl_init = {
        .get_proc_address = get_proc_address,
        .get_proc_address_ctx = NULL,
    };

    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, (void *)MPV_RENDER_API_TYPE_OPENGL},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init},
        {0}
    };

    if (mpv_render_context_create(&pl->render_ctx, pl->mpv, params) < 0) {
        fprintf(stderr, "failed to create mpv render context\n");
        player_destroy(pl);
        return NULL;
    }

    const char *cmd[] = {"loadfile", url, NULL};
    mpv_command_async(pl->mpv, 0, cmd);

    return pl;
}

void player_set_wakeup_fd(struct player *pl, int *fd) {
    pl->wakeup_fd = fd;
    mpv_set_wakeup_callback(pl->mpv, wakeup_cb, fd);
}

void player_set_size(struct player *pl, int width, int height) {
    pl->width = width;
    pl->height = height;
}

void player_destroy(struct player *pl) {
    if (!pl) return;
    if (pl->render_ctx)
        mpv_render_context_free(pl->render_ctx);
    if (pl->mpv)
        mpv_terminate_destroy(pl->mpv);
    free(pl);
}

void player_render(struct player *pl) {
    mpv_opengl_fbo fbo = {
        .fbo = 0,
        .w = pl->width,
        .h = pl->height,
        .internal_format = 0,
    };
    int flip_y = 1;

    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
        {MPV_RENDER_PARAM_FLIP_Y, &flip_y},
        {0}
    };

    mpv_render_context_render(pl->render_ctx, params);
}

int player_update(struct player *pl) {
    return mpv_render_context_update(pl->render_ctx) & MPV_RENDER_UPDATE_FRAME;
}

int player_poll_event(struct player *pl, mpv_event **ev) {
    *ev = mpv_wait_event(pl->mpv, 0);
    return (*ev)->event_id != MPV_EVENT_NONE;
}

void player_cmd(struct player *pl, const char *args[]) {
    mpv_command_async(pl->mpv, 0, args);
}
