#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <linux/input-event-codes.h>

#include "overlay.h"
#include "protocols/layer-shell-client-protocol.h"

#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))

static void set_input_regions(struct overlay *ov) {
    if (ov->locked) {
        struct wl_region *empty = wl_compositor_create_region(ov->compositor);
        wl_surface_set_input_region(ov->surface, empty);
        wl_region_destroy(empty);
    } else {
        struct wl_region *full = wl_compositor_create_region(ov->compositor);
        wl_region_add(full, 0, 0, ov->width, ov->height);
        wl_surface_set_input_region(ov->surface, full);
        wl_region_destroy(full);
    }
}

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface, uint32_t ver) {
    (void)ver;
    struct overlay *ov = data;

    if (strcmp(interface, "wl_compositor") == 0) {
        ov->compositor = wl_registry_bind(registry, name,
                                          &wl_compositor_interface, 4);
    } else if (strcmp(interface, "zwlr_layer_shell_v1") == 0) {
        ov->layer_shell = wl_registry_bind(registry, name,
                                           &zwlr_layer_shell_v1_interface, 4);
    } else if (strcmp(interface, "wl_seat") == 0) {
        ov->seat = wl_registry_bind(registry, name,
                                    &wl_seat_interface, 1);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void layer_surface_configure(void *data,
                                    struct zwlr_layer_surface_v1 *ls,
                                    uint32_t serial, uint32_t w, uint32_t h) {
    struct overlay *ov = data;
    ov->width = w;
    ov->height = h;
    ov->configured = 1;
    zwlr_layer_surface_v1_ack_configure(ls, serial);
}

static void layer_surface_closed(void *data,
                                 struct zwlr_layer_surface_v1 *ls) {
    (void)ls;
    struct overlay *ov = data;
    ov->closed = 1;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed = layer_surface_closed,
};

static void pointer_enter(void *data, struct wl_pointer *ptr,
                          uint32_t serial, struct wl_surface *surface,
                          wl_fixed_t sx, wl_fixed_t sy) {
    (void)ptr;
    (void)serial;
    (void)surface;
    struct overlay *ov = data;
    ov->pointer_entered = 1;
    ov->pointer_x = wl_fixed_to_int(sx);
    ov->pointer_y = wl_fixed_to_int(sy);
}

static void pointer_leave(void *data, struct wl_pointer *ptr,
                          uint32_t serial, struct wl_surface *surface) {
    (void)ptr;
    (void)serial;
    (void)surface;
    struct overlay *ov = data;
    ov->pointer_entered = 0;
    ov->drag_active = 0;
    ov->resize_active = 0;
    fprintf(stderr, "pointer: leave\n");
}

#define DRAG_THRESHOLD 5

static void pointer_motion(void *data, struct wl_pointer *ptr,
                           uint32_t time, wl_fixed_t sx, wl_fixed_t sy) {
    (void)ptr;
    (void)time;
    struct overlay *ov = data;
    int nx = wl_fixed_to_int(sx);
    int ny = wl_fixed_to_int(sy);

    if (ov->drag_active) {
        overlay_set_position(ov,
            ov->drag_grab_px + (nx - ov->drag_grab_rx),
            ov->drag_grab_py + (ny - ov->drag_grab_ry));
    } else if (ov->resize_active) {
        int dw = nx - ov->resize_grab_rx;
        int dh = ny - ov->resize_grab_ry;
        int nw = CLAMP(ov->resize_grab_bw + dw, MIN_WIDTH, 9999);
        int nh = CLAMP(ov->resize_grab_bh + dh, MIN_HEIGHT, 9999);
        overlay_resize(ov, nw, nh);
    } else if (ov->press_pending) {
        int dx = nx - ov->press_x;
        int dy = ny - ov->press_y;
        if (dx * dx + dy * dy > DRAG_THRESHOLD * DRAG_THRESHOLD) {
            ov->drag_active = 1;
            ov->press_pending = 0;
            ov->drag_grab_rx = ov->press_x;
            ov->drag_grab_ry = ov->press_y;
            ov->drag_grab_px = ov->pos_x;
            ov->drag_grab_py = ov->pos_y;
            fprintf(stderr, "pointer: drag start at pos %d,%d\n", ov->pos_x, ov->pos_y);
        }
    }

    if (ov->pointer_fn && !ov->locked)
        ov->pointer_fn(ov->pointer_data, nx, ny, 0);

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ov->hover_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    ov->pointer_x = nx;
    ov->pointer_y = ny;
}

static void pointer_button(void *data, struct wl_pointer *ptr,
                           uint32_t serial, uint32_t time,
                           uint32_t button, uint32_t state) {
    (void)ptr;
    (void)serial;
    (void)time;
    struct overlay *ov = data;

    if (button != BTN_LEFT) return;

    fprintf(stderr, "pointer: btn %s locked=%d\n",
            state == WL_POINTER_BUTTON_STATE_PRESSED ? "press" : "release",
            ov->locked);

    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        if (!ov->locked) {
            int in_grip = ov->pointer_x >= ov->width - RESIZE_GRIP_SIZE &&
                          ov->pointer_y >= ov->height - RESIZE_GRIP_SIZE;

            if (in_grip) {
                ov->resize_active = 1;
                ov->resize_grab_rx = ov->pointer_x;
                ov->resize_grab_ry = ov->pointer_y;
                ov->resize_grab_bw = ov->width;
                ov->resize_grab_bh = ov->height;
                fprintf(stderr, "pointer: resize start\n");
            } else {
                ov->press_pending = 1;
                ov->press_x = ov->pointer_x;
                ov->press_y = ov->pointer_y;
                fprintf(stderr, "pointer: press at %d,%d\n", ov->press_x, ov->press_y);
            }

            if (ov->pointer_fn)
                ov->pointer_fn(ov->pointer_data, ov->pointer_x, ov->pointer_y, 1);
        }
    } else {
        ov->press_pending = 0;
        if (ov->drag_active) {
            ov->drag_active = 0;
        } else if (ov->resize_active) {
            ov->resize_active = 0;
        } else if (!ov->locked && ov->pointer_fn) {
            ov->pointer_fn(ov->pointer_data, ov->pointer_x, ov->pointer_y, -1);
        }
    }
}



static void pointer_axis(void *data, struct wl_pointer *ptr,
                         uint32_t time, uint32_t axis, wl_fixed_t value) {
    (void)ptr;
    (void)time;
    struct overlay *ov = data;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL && ov->scroll_fn) {
        ov->scroll_fn(ov->scroll_data, wl_fixed_to_int(value));
    }
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
};

static void seat_capabilities(void *data, struct wl_seat *seat,
                              uint32_t caps) {
    struct overlay *ov = data;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !ov->pointer) {
        ov->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(ov->pointer, &pointer_listener, ov);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && ov->pointer) {
        wl_pointer_destroy(ov->pointer);
        ov->pointer = NULL;
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name) {
    (void)data;
    (void)seat;
    (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

struct overlay *overlay_create(const char *socket, int width, int height,
                               enum anchor_pos pos, int margin) {
    (void)pos;
    struct overlay *ov = calloc(1, sizeof(*ov));
    ov->locked = 1;
    ov->width = width;
    ov->height = height;

    ov->display = wl_display_connect(socket);
    if (!ov->display) {
        fprintf(stderr, "failed to connect to wayland display\n");
        free(ov);
        return NULL;
    }

    struct wl_registry *registry = wl_display_get_registry(ov->display);
    wl_registry_add_listener(registry, &registry_listener, ov);
    wl_display_roundtrip(ov->display);

    if (!ov->compositor || !ov->layer_shell) {
        fprintf(stderr, "missing required wayland globals\n");
        overlay_destroy(ov);
        return NULL;
    }

    if (ov->seat)
        wl_seat_add_listener(ov->seat, &seat_listener, ov);

    ov->surface = wl_compositor_create_surface(ov->compositor);

    ov->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        ov->layer_shell, ov->surface, NULL,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "waytop");

    zwlr_layer_surface_v1_add_listener(ov->layer_surface,
                                       &layer_surface_listener, ov);

    ov->pos_x = margin;
    ov->pos_y = margin;
    uint32_t anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                      ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
    zwlr_layer_surface_v1_set_anchor(ov->layer_surface, anchor);
    zwlr_layer_surface_v1_set_margin(ov->layer_surface, margin, 0, 0, margin);

    zwlr_layer_surface_v1_set_keyboard_interactivity(
        ov->layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);

    zwlr_layer_surface_v1_set_size(ov->layer_surface, width, height);
    zwlr_layer_surface_v1_set_exclusive_zone(ov->layer_surface, -1);

    set_input_regions(ov);

    wl_surface_commit(ov->surface);
    wl_display_roundtrip(ov->display);

    if (ov->closed) {
        fprintf(stderr, "layer surface was closed\n");
        overlay_destroy(ov);
        return NULL;
    }
    if (!ov->configured) {
        fprintf(stderr, "layer surface was not configured\n");
        overlay_destroy(ov);
        return NULL;
    }

    static const EGLint ctx_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE,
    };
    static const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };

    ov->egl_display = eglGetDisplay((EGLNativeDisplayType)ov->display);
    if (ov->egl_display == EGL_NO_DISPLAY) {
        fprintf(stderr, "failed to create egl display\n");
        overlay_destroy(ov);
        return NULL;
    }

    EGLint major, minor;
    if (!eglInitialize(ov->egl_display, &major, &minor)) {
        fprintf(stderr, "failed to initialize egl\n");
        overlay_destroy(ov);
        return NULL;
    }

    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        fprintf(stderr, "failed to bind opengl es api\n");
        overlay_destroy(ov);
        return NULL;
    }

    EGLint count;
    if (!eglChooseConfig(ov->egl_display, config_attribs,
                         &ov->egl_config, 1, &count) || count == 0) {
        fprintf(stderr, "failed to choose egl config\n");
        overlay_destroy(ov);
        return NULL;
    }

    ov->egl_context = eglCreateContext(ov->egl_display, ov->egl_config,
                                       EGL_NO_CONTEXT, ctx_attribs);
    if (ov->egl_context == EGL_NO_CONTEXT) {
        fprintf(stderr, "failed to create egl context\n");
        overlay_destroy(ov);
        return NULL;
    }

    ov->egl_window = wl_egl_window_create(ov->surface, ov->width, ov->height);
    if (!ov->egl_window) {
        fprintf(stderr, "failed to create egl window\n");
        overlay_destroy(ov);
        return NULL;
    }

    ov->egl_surface = eglCreateWindowSurface(ov->egl_display, ov->egl_config,
                                             (EGLNativeWindowType)ov->egl_window, NULL);
    if (ov->egl_surface == EGL_NO_SURFACE) {
        fprintf(stderr, "failed to create egl surface\n");
        overlay_destroy(ov);
        return NULL;
    }

    eglMakeCurrent(ov->egl_display, ov->egl_surface,
                   ov->egl_surface, ov->egl_context);

    return ov;
}

void overlay_destroy(struct overlay *ov) {
    if (!ov) return;

    if (ov->egl_context != EGL_NO_CONTEXT)
        eglDestroyContext(ov->egl_display, ov->egl_context);
    if (ov->egl_surface != EGL_NO_SURFACE)
        eglDestroySurface(ov->egl_display, ov->egl_surface);
    if (ov->egl_window)
        wl_egl_window_destroy(ov->egl_window);
    if (ov->egl_display)
        eglTerminate(ov->egl_display);

    if (ov->pointer)
        wl_pointer_destroy(ov->pointer);
    if (ov->seat)
        wl_seat_destroy(ov->seat);
    if (ov->layer_surface)
        zwlr_layer_surface_v1_destroy(ov->layer_surface);
    if (ov->layer_shell)
        zwlr_layer_shell_v1_destroy(ov->layer_shell);
    if (ov->surface)
        wl_surface_destroy(ov->surface);
    if (ov->compositor)
        wl_compositor_destroy(ov->compositor);
    if (ov->display)
        wl_display_disconnect(ov->display);

    free(ov);
}

int overlay_configured(struct overlay *ov) {
    return ov->configured;
}

int overlay_closed(struct overlay *ov) {
    return ov->closed;
}

void overlay_make_current(struct overlay *ov) {
    eglMakeCurrent(ov->egl_display, ov->egl_surface,
                   ov->egl_surface, ov->egl_context);
}

void overlay_swap_buffers(struct overlay *ov) {
    eglSwapBuffers(ov->egl_display, ov->egl_surface);
}

void overlay_set_position(struct overlay *ov, int x, int y) {
    ov->pos_x = x;
    ov->pos_y = y;
    uint32_t anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                      ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
    zwlr_layer_surface_v1_set_anchor(ov->layer_surface, anchor);
    zwlr_layer_surface_v1_set_margin(ov->layer_surface, y, 0, 0, x);
    wl_surface_commit(ov->surface);
    wl_display_flush(ov->display);
}

EGLDisplay overlay_get_egl_display(struct overlay *ov) {
    return ov->egl_display;
}

void overlay_resize(struct overlay *ov, int width, int height) {
    ov->width = width;
    ov->height = height;
    if (ov->egl_window)
        wl_egl_window_resize(ov->egl_window, width, height, 0, 0);
    zwlr_layer_surface_v1_set_size(ov->layer_surface, width, height);
    set_input_regions(ov);
    wl_surface_commit(ov->surface);
    wl_display_flush(ov->display);
    if (ov->resize_fn)
        ov->resize_fn(ov->resize_data, width, height);
}

void overlay_set_scroll_fn(struct overlay *ov, void (*fn)(void *, int), void *data) {
    ov->scroll_fn = fn;
    ov->scroll_data = data;
}

void overlay_set_resize_fn(struct overlay *ov, void (*fn)(void *, int, int), void *data) {
    ov->resize_fn = fn;
    ov->resize_data = data;
}

void overlay_set_pointer_fn(struct overlay *ov, void (*fn)(void *, int, int, int), void *data) {
    ov->pointer_fn = fn;
    ov->pointer_data = data;
}

void overlay_toggle_locked(struct overlay *ov) {
    ov->locked = !ov->locked;
    fprintf(stderr, "toggle: locked=%d\n", ov->locked);
    set_input_regions(ov);
    wl_surface_commit(ov->surface);
    wl_display_roundtrip(ov->display);
}

int overlay_is_locked(struct overlay *ov) {
    return ov->locked;
}
