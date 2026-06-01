#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <GLES2/gl2.h>

#include "overlay.h"
#include "player.h"

struct app {
    struct overlay *ov;
    struct player *pl;
    int wakeup_fd;
    int frame_done;
    int running;
};

static void frame_done_cb(void *data, struct wl_callback *cb, uint32_t time) {
    (void)time;
    struct app *app = data;
    app->frame_done = 1;
    wl_callback_destroy(cb);
}

static const struct wl_callback_listener frame_listener = {
    .done = frame_done_cb,
};

static void print_usage(const char *name) {
    fprintf(stderr,
        "usage: %s [options] <url|file>\n"
        "\n"
        "options:\n"
        "  -s WxH         size (default: 480x270)\n"
        "  -p POS         position: br (default), bl, tr, tl\n"
        "  -m N           margin from edge (default: 16)\n"
        "  -h             show this help\n"
        "\n"
        "example:\n"
        "  %s -s 640x360 -p br https://www.youtube.com/watch?v=dQw4w9WgXcQ\n",
        name, name);
}

static int parse_size(const char *s, int *w, int *h) {
    int n = sscanf(s, "%dx%d", w, h);
    if (n != 2 || *w <= 0 || *h <= 0) return -1;
    return 0;
}

static enum anchor_pos parse_pos(const char *s) {
    if (strcmp(s, "br") == 0) return ANCHOR_BOTTOM_RIGHT;
    if (strcmp(s, "bl") == 0) return ANCHOR_BOTTOM_LEFT;
    if (strcmp(s, "tr") == 0) return ANCHOR_TOP_RIGHT;
    if (strcmp(s, "tl") == 0) return ANCHOR_TOP_LEFT;
    fprintf(stderr, "invalid position '%s', using bottom-right\n", s);
    return ANCHOR_BOTTOM_RIGHT;
}

int main(int argc, char **argv) {
    int width = 480;
    int height = 270;
    int margin = 16;
    enum anchor_pos pos = ANCHOR_BOTTOM_RIGHT;
    const char *url = NULL;

    int i;
    for (i = 1; i < argc; i++) {
        if (argv[i][0] != '-') {
            url = argv[i];
            break;
        }
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            if (parse_size(argv[++i], &width, &height) < 0) {
                fprintf(stderr, "invalid size '%s'\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            pos = parse_pos(argv[++i]);
        } else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            margin = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown option '%s'\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (!url) {
        fprintf(stderr, "no url specified\n");
        print_usage(argv[0]);
        return 1;
    }

    struct app app = {0};

    app.ov = overlay_create(NULL, width, height, pos, margin);
    if (!app.ov) {
        fprintf(stderr, "failed to create overlay\n");
        return 1;
    }

    app.wakeup_fd = eventfd(0, EFD_NONBLOCK);
    if (app.wakeup_fd < 0) {
        perror("eventfd");
        overlay_destroy(app.ov);
        return 1;
    }

    app.pl = player_create(url, width, height);
    if (!app.pl) {
        fprintf(stderr, "failed to create player\n");
        close(app.wakeup_fd);
        overlay_destroy(app.ov);
        return 1;
    }

    player_set_wakeup_fd(app.pl, &app.wakeup_fd);
    app.running = 1;

    overlay_make_current(app.ov);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    struct wl_callback *cb = wl_surface_frame(app.ov->surface);
    wl_callback_add_listener(cb, &frame_listener, &app);
    overlay_swap_buffers(app.ov);

    while (app.running) {
        mpv_event *ev;
        while (player_poll_event(app.pl, &ev)) {
            if (ev->event_id == MPV_EVENT_SHUTDOWN ||
                ev->event_id == MPV_EVENT_END_FILE) {
                app.running = 0;
            }
        }

        int mpv_ready = player_update(app.pl);

        if (app.frame_done && mpv_ready) {
            overlay_make_current(app.ov);
            player_render(app.pl);

            struct wl_callback *cb = wl_surface_frame(app.ov->surface);
            wl_callback_add_listener(cb, &frame_listener, &app);
            overlay_swap_buffers(app.ov);
            app.frame_done = 0;
        }

        wl_display_flush(app.ov->display);

        struct pollfd fds[2];
        memset(fds, 0, sizeof(fds));
        fds[0].fd = wl_display_get_fd(app.ov->display);
        fds[0].events = POLLIN;
        fds[1].fd = app.wakeup_fd;
        fds[1].events = POLLIN;

        poll(fds, 2, -1);

        if (fds[0].revents & POLLIN)
            wl_display_dispatch(app.ov->display);

        if (fds[1].revents & POLLIN) {
            uint64_t val;
            read(app.wakeup_fd, &val, sizeof(val));
        }

        if (app.ov->closed)
            app.running = 0;
    }

    player_destroy(app.pl);
    close(app.wakeup_fd);
    overlay_destroy(app.ov);

    return 0;
}
