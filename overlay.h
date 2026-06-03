#ifndef OVERLAY_H
#define OVERLAY_H

#include <stdint.h>
#include <time.h>
#include <EGL/egl.h>
#include <wayland-client.h>
#include <wayland-egl.h>

#define DRAG_HANDLE_HEIGHT 40
#define RESIZE_GRIP_SIZE 40
#define MIN_WIDTH 100
#define MIN_HEIGHT 56

struct overlay {
    struct wl_display *display;
    struct wl_compositor *compositor;
    struct wl_surface *surface;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct zwlr_layer_surface_v1 *layer_surface;

    struct wl_egl_window *egl_window;
    EGLDisplay egl_display;
    EGLContext egl_context;
    EGLSurface egl_surface;
    EGLConfig egl_config;

    struct wl_seat *seat;
    struct wl_pointer *pointer;

    int pos_x, pos_y;
    int width;
    int height;
    int configured;
    int closed;

    int pointer_entered;
    int pointer_x, pointer_y;

    int press_pending;
    int press_x, press_y;
    int drag_active;
    int drag_grab_rx, drag_grab_ry;
    int drag_grab_px, drag_grab_py;

    int resize_active;
    int resize_grab_rx, resize_grab_ry;
    int resize_grab_bw, resize_grab_bh;

    void (*scroll_fn)(void *, int);
    void *scroll_data;

    void (*resize_fn)(void *, int, int);
    void *resize_data;

    void (*pointer_fn)(void *, int, int, int);
    void *pointer_data;

    uint64_t hover_ns;
    int locked;

    float aspect;
};

enum anchor_pos {
    ANCHOR_BOTTOM_RIGHT,
    ANCHOR_BOTTOM_LEFT,
    ANCHOR_TOP_RIGHT,
    ANCHOR_TOP_LEFT,
};

struct overlay *overlay_create(const char *socket, int width, int height,
                               enum anchor_pos pos, int margin);
void overlay_destroy(struct overlay *ov);
int overlay_configured(struct overlay *ov);
int overlay_closed(struct overlay *ov);
void overlay_make_current(struct overlay *ov);
void overlay_swap_buffers(struct overlay *ov);
void overlay_set_position(struct overlay *ov, int x, int y);
void overlay_resize(struct overlay *ov, int width, int height);
void overlay_set_aspect(struct overlay *ov, float ar);
void overlay_set_scroll_fn(struct overlay *ov, void (*fn)(void *, int), void *data);
void overlay_set_resize_fn(struct overlay *ov, void (*fn)(void *, int, int), void *data);
void overlay_set_pointer_fn(struct overlay *ov, void (*fn)(void *, int, int, int), void *data);
void overlay_toggle_locked(struct overlay *ov);
int overlay_is_locked(struct overlay *ov);
EGLDisplay overlay_get_egl_display(struct overlay *ov);

#endif
