#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <dirent.h>
#include <GLES2/gl2.h>

#include "overlay.h"
#include "player.h"

struct app {
    struct overlay *ov;
    struct player *pl;
    int wakeup_fd;
    int cmd_fd;
    int frame_done;
    int running;
    char socket_path[128];
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
        "  run overlay:\n"
        "    %s -s 640x360 https://youtube.com/watch?v=...\n"
        "\n"
        "  control a running instance:\n"
        "    %s -c toggle\n"
        "    %s -c \"pos 100 50\"\n"
        "    %s -c \"size 640x360\"\n"
        "\n"
        "options:\n"
        "  -s WxH         size (default: 480x270)\n"
        "  -m N           margin from top-left (default: 16)\n"
        "  -c CMD         send command to running instance\n"
        "  -h             show this help\n",
        name, name, name, name, name);
}

static int ctl_send(const char *cmd) {
    DIR *dir = opendir("/tmp");
    if (!dir) return -1;

    struct dirent *e;
    int ret = -1;
    while ((e = readdir(dir))) {
        if (strncmp(e->d_name, "waytop-", 7) != 0) continue;
        if (strlen(e->d_name) + 6 > sizeof(((struct sockaddr_un *)0)->sun_path))
            continue;

        char path[256];
        snprintf(path, sizeof(path), "/tmp/%s", e->d_name);

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        memcpy(addr.sun_path, path, strlen(path) + 1);

        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) continue;

        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            close(fd);
            continue;
        }

        write(fd, cmd, strlen(cmd));
        write(fd, "\n", 1);
        close(fd);
        ret = 0;
        break;
    }
    closedir(dir);
    return ret;
}

static int parse_size(const char *s, int *w, int *h) {
    int n = sscanf(s, "%dx%d", w, h);
    if (n != 2 || *w <= 0 || *h <= 0) return -1;
    return 0;
}

static void on_resize(void *data, int w, int h) {
    player_set_size((struct player *)data, w, h);
}

static void on_motion(void *data, int x, int y) {
    struct player *pl = data;
    char xs[16], ys[16];
    snprintf(xs, sizeof(xs), "%d", x);
    snprintf(ys, sizeof(ys), "%d", y);
    const char *args[] = {"mouse", xs, ys, NULL};
    mpv_command_async(pl->mpv, 0, args);
}

static void on_scroll(void *data, int value) {
    struct player *pl = data;
    double vol;
    mpv_get_property(pl->mpv, "volume", MPV_FORMAT_DOUBLE, &vol);
    vol += value > 0 ? -5.0 : 5.0;
    if (vol < 0.0) vol = 0.0;
    if (vol > 150.0) vol = 150.0;
    mpv_set_property(pl->mpv, "volume", MPV_FORMAT_DOUBLE, &vol);
}

static int setup_cmd_socket(char *path, size_t pathlen) {
    int n = snprintf(path, pathlen, "/tmp/waytop-%d.sock", getpid());
    if (n < 0 || (size_t)n >= pathlen) return -1;

    unlink(path);

    if (strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path))
        return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, strlen(path) + 1);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(fd, 4) < 0) {
        close(fd);
        return -1;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    return fd;
}

static void handle_cmd(struct app *app, const char *cmd) {
    int x, y, w, h;
    float fs;
    fprintf(stderr, "cmd: [%s]\n", cmd);
    if (strcmp(cmd, "quit") == 0) {
        app->running = 0;
    } else if (strcmp(cmd, "toggle") == 0) {
        overlay_toggle_locked(app->ov);
    } else if (sscanf(cmd, "pos %d %d", &x, &y) == 2) {
        overlay_set_position(app->ov, x, y);
    } else if (sscanf(cmd, "size %dx%d", &w, &h) == 2 && w > 0 && h > 0) {
        overlay_resize(app->ov, w, h);
        player_set_size(app->pl, w, h);
    } else if (sscanf(cmd, "seek %f", &fs) == 1) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%+.0f", fs);
        const char *args[] = {"seek", buf, "relative", NULL};
        player_cmd(app->pl, args);
    } else if (strcmp(cmd, "play") == 0 || strcmp(cmd, "pause") == 0) {
        const char *args[] = {"cycle", "pause", NULL};
        player_cmd(app->pl, args);
    } else {
        fprintf(stderr, "cmd: unknown\n");
    }
}

static void process_cmd_client(struct app *app, int client) {
    int flags = fcntl(client, F_GETFL, 0);
    fcntl(client, F_SETFL, flags | O_NONBLOCK);

    char buf[256];
    int n = read(client, buf, sizeof(buf) - 1);
    close(client);

    if (n > 0) {
        buf[n] = '\0';
        char *nl = strchr(buf, '\n');
        if (nl) *nl = '\0';
        handle_cmd(app, buf);
    }
}

int main(int argc, char **argv) {
    int width = 480;
    int height = 270;
    int margin = 16;
    const char *url = NULL;
    const char *ctl_cmd = NULL;

    int i;
    for (i = 1; i < argc; i++) {
        if (argv[i][0] != '-') {
            url = argv[i];
            break;
        }
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            ctl_cmd = argv[++i];
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            if (parse_size(argv[++i], &width, &height) < 0) {
                fprintf(stderr, "invalid size '%s'\n", argv[i]);
                return 1;
            }
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

    if (ctl_cmd) {
        if (ctl_send(ctl_cmd) < 0)
            fprintf(stderr, "no running wl-overlay instance found\n");
        return 0;
    }

    if (!url) {
        fprintf(stderr, "no url specified\n");
        print_usage(argv[0]);
        return 1;
    }

    struct app app = {0};

    app.ov = overlay_create(NULL, width, height, ANCHOR_TOP_LEFT, margin);
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

    app.cmd_fd = setup_cmd_socket(app.socket_path, sizeof(app.socket_path));
    if (app.cmd_fd < 0) {
        fprintf(stderr, "failed to create command socket\n");
        close(app.wakeup_fd);
        overlay_destroy(app.ov);
        return 1;
    }

    fprintf(stderr, "socket: %s\n", app.socket_path);

    app.pl = player_create(url, width, height);
    if (!app.pl) {
        fprintf(stderr, "failed to create player\n");
        close(app.cmd_fd);
        unlink(app.socket_path);
        close(app.wakeup_fd);
        overlay_destroy(app.ov);
        return 1;
    }

    player_set_wakeup_fd(app.pl, &app.wakeup_fd);
    overlay_set_scroll_fn(app.ov, on_scroll, app.pl);
    overlay_set_resize_fn(app.ov, on_resize, app.pl);
    overlay_set_motion_fn(app.ov, on_motion, app.pl);
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
            if (ev->event_id == MPV_EVENT_SHUTDOWN)
                app.running = 0;
        }

        int mpv_ready = player_update(app.pl);

        if (mpv_ready) {
            overlay_make_current(app.ov);
            player_render(app.pl);

            struct wl_callback *cb = wl_surface_frame(app.ov->surface);
            wl_callback_add_listener(cb, &frame_listener, &app);
            overlay_swap_buffers(app.ov);
            app.frame_done = 0;
        }

        wl_display_flush(app.ov->display);

        struct pollfd fds[3];
        memset(fds, 0, sizeof(fds));
        fds[0].fd = wl_display_get_fd(app.ov->display);
        fds[0].events = POLLIN;
        fds[1].fd = app.wakeup_fd;
        fds[1].events = POLLIN;
        fds[2].fd = app.cmd_fd;
        fds[2].events = POLLIN;

        poll(fds, 3, 16);

        if (fds[0].revents & POLLIN)
            wl_display_dispatch(app.ov->display);

        if (fds[1].revents & POLLIN) {
            uint64_t val;
            read(app.wakeup_fd, &val, sizeof(val));
        }

        if (fds[2].revents & POLLIN) {
            int client = accept(app.cmd_fd, NULL, NULL);
            if (client >= 0) {
                int flags = fcntl(client, F_GETFL, 0);
                fcntl(client, F_SETFL, flags | O_NONBLOCK);
                process_cmd_client(&app, client);
            }
        }

        if (app.ov->closed)
            app.running = 0;
    }

    player_destroy(app.pl);
    close(app.cmd_fd);
    unlink(app.socket_path);
    close(app.wakeup_fd);
    overlay_destroy(app.ov);

    return 0;
}
