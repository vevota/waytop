#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "overlay.h"
#include "protocols/layer-shell-client-protocol.h"

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

struct overlay *overlay_create(const char *socket, int width, int height,
                               enum anchor_pos pos, int margin) {
    struct overlay *ov = calloc(1, sizeof(*ov));
    ov->width = width;

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

    ov->surface = wl_compositor_create_surface(ov->compositor);

    ov->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        ov->layer_shell, ov->surface, NULL,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "wl-overlay");

    zwlr_layer_surface_v1_add_listener(ov->layer_surface,
                                       &layer_surface_listener, ov);

    uint32_t anchor = 0;
    switch (pos) {
    case ANCHOR_BOTTOM_RIGHT:
        anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                 ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
        break;
    case ANCHOR_BOTTOM_LEFT:
        anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                 ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
        break;
    case ANCHOR_TOP_RIGHT:
        anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                 ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
        break;
    case ANCHOR_TOP_LEFT:
        anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                 ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
        break;
    }
    zwlr_layer_surface_v1_set_anchor(ov->layer_surface, anchor);

    int t = 0, b = 0, l = 0, r = 0;
    if (anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP) t = margin;
    if (anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM) b = margin;
    if (anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT) l = margin;
    if (anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT) r = margin;
    zwlr_layer_surface_v1_set_margin(ov->layer_surface, t, r, b, l);

    zwlr_layer_surface_v1_set_keyboard_interactivity(
        ov->layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);

    zwlr_layer_surface_v1_set_size(ov->layer_surface, width, height);
    zwlr_layer_surface_v1_set_exclusive_zone(ov->layer_surface, -1);

    struct wl_region *empty = wl_compositor_create_region(ov->compositor);
    wl_surface_set_input_region(ov->surface, empty);
    wl_region_destroy(empty);

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

EGLDisplay overlay_get_egl_display(struct overlay *ov) {
    return ov->egl_display;
}

void overlay_set_position(struct overlay *ov, int x, int y) {
    uint32_t anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                      ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
    zwlr_layer_surface_v1_set_anchor(ov->layer_surface, anchor);
    zwlr_layer_surface_v1_set_margin(ov->layer_surface, y, 0, 0, x);
}
