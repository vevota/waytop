#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include <GLES2/gl2.h>

#include "ui.h"
#include "overlay.h"
#include "player.h"

#define UI_SEEK_H 6

static const char *vert_src =
    "attribute vec2 pos;\n"
    "uniform vec2 scale;\n"
    "void main() {\n"
    "  gl_Position = vec4(pos * scale + vec2(-1.0, 1.0), 0.0, 1.0);\n"
    "}";

static const char *frag_src =
    "precision mediump float;\n"
    "uniform vec4 color;\n"
    "void main() { gl_FragColor = color; }";

static GLuint prog;
static int prog_ok;

static GLint u_color, u_scale;
static GLint a_pos;

static void ui_init(void) {
    if (prog_ok) return;

    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vert_src, NULL);
    glCompileShader(vs);
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &frag_src, NULL);
    glCompileShader(fs);

    prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);

    GLint ok;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[256];
        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        fprintf(stderr, "ui shader link: %s\n", log);
        return;
    }

    u_color = glGetUniformLocation(prog, "color");
    u_scale = glGetUniformLocation(prog, "scale");
    a_pos = glGetAttribLocation(prog, "pos");
    prog_ok = 1;
}

static void draw_rect(float x, float y, float w, float h, float r, float g, float b, float a) {
    if (!prog_ok) return;
    glUseProgram(prog);

    float verts[] = {
        x,   y+h,
        x,   y,
        x+w, y+h,
        x+w, y,
    };

    glUniform4f(u_color, r, g, b, a);
    glVertexAttribPointer(a_pos, 2, GL_FLOAT, GL_FALSE, 0, verts);
    glEnableVertexAttribArray(a_pos);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(a_pos);
}

static void draw_tri(float x1, float y1, float x2, float y2, float x3, float y3,
                     float r, float g, float b, float a) {
    if (!prog_ok) return;
    glUseProgram(prog);

    float verts[] = {x1, y1, x2, y2, x3, y3};
    glUniform4f(u_color, r, g, b, a);
    glVertexAttribPointer(a_pos, 2, GL_FLOAT, GL_FALSE, 0, verts);
    glEnableVertexAttribArray(a_pos);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisableVertexAttribArray(a_pos);
}

void ui_draw(struct overlay *ov, struct player *pl, int unlocked) {
    int w = ov->width;
    int h = ov->height;
    if (w < 1 || h < 1) return;

    if (!unlocked) return;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    uint64_t delta = now - ov->hover_ns;
    static int dc;
    if ((dc++ % 60) == 0)
        fprintf(stderr, "hover: delta=%lluns ns=%llu timeout=%llu\n",
                (unsigned long long)delta,
                (unsigned long long)ov->hover_ns,
                (unsigned long long)UI_HOVER_TIMEOUT);
    if (delta > UI_HOVER_TIMEOUT)
        return;

    ui_init();

    glViewport(0, 0, w, h);

    float sx = 2.0f / w;
    float sy = -2.0f / h;
    glUniform2f(u_scale, sx, sy);

    int bar_y = h - UI_BAR_H;
    int btn_top = bar_y + (UI_BAR_H - UI_BTN_S) / 2;

    draw_rect(0, bar_y, w, UI_BAR_H, 0, 0, 0, 0.7f);

    if (!pl) return;

    int pause_flag = 0;
    mpv_get_property(pl->mpv, "pause", MPV_FORMAT_FLAG, &pause_flag);
    double pos = 0, dur = 1;
    mpv_get_property(pl->mpv, "time-pos", MPV_FORMAT_DOUBLE, &pos);
    mpv_get_property(pl->mpv, "duration", MPV_FORMAT_DOUBLE, &dur);
    if (dur <= 0) dur = 1;
    if (pos < 0) pos = 0;

    float c = 0.8f;

    int btn_left = UI_MARGIN;

    if (pause_flag) {
        float cx = btn_left + 8;
        float cy = btn_top + UI_BTN_S / 2.0f;
        float half = UI_BTN_S / 3.0f;
        draw_tri(cx, cy - half, cx, cy + half, cx + half * 1.2f, cy, c, c, c, 1);
    } else {
        draw_rect(btn_left + 6,  btn_top + 4,  5, UI_BTN_S - 8, c, c, c, 1);
        draw_rect(btn_left + 16, btn_top + 4,  5, UI_BTN_S - 8, c, c, c, 1);
    }

    int seek_l = btn_left + UI_BTN_S + UI_MARGIN;
    int seek_r = w - UI_MARGIN - (UI_SEEK_S + UI_MARGIN) * 2;
    int seek_y = bar_y + (UI_BAR_H - UI_SEEK_H) / 2;
    int seek_w = seek_r - seek_l;

    if (seek_w > 0) {
        draw_rect(seek_l, seek_y, seek_w, UI_SEEK_H, 0.3f, 0.3f, 0.3f, 1);
        int prog_w = (int)(seek_w * (pos / dur));
        if (prog_w > 0)
            draw_rect(seek_l, seek_y, prog_w, UI_SEEK_H, 0.9f, 0.2f, 0.2f, 1);

        int dot_x = seek_l + prog_w;
        int dot_r = UI_SEEK_H + 2;
        draw_rect(dot_x - dot_r/2, seek_y - 1, dot_r, UI_SEEK_H + 2, c, c, c, 1);
    }

    int btn_y = bar_y + (UI_BAR_H - UI_SEEK_S) / 2;

    int rw_x = seek_r + UI_MARGIN;
    draw_rect(rw_x, btn_y, UI_SEEK_S, UI_SEEK_S, 0.2f, 0.2f, 0.2f, 1);
    draw_tri(rw_x + 20, btn_y + UI_SEEK_S/2,
             rw_x + 5,  btn_y + 5,
             rw_x + 5,  btn_y + UI_SEEK_S - 5, c, c, c, 1);
    draw_tri(rw_x + 27, btn_y + UI_SEEK_S/2,
             rw_x + 12, btn_y + 5,
             rw_x + 12, btn_y + UI_SEEK_S - 5, c, c, c, 1);

    int ff_x = rw_x + UI_SEEK_S + UI_MARGIN;
    draw_rect(ff_x, btn_y, UI_SEEK_S, UI_SEEK_S, 0.2f, 0.2f, 0.2f, 1);
    draw_tri(ff_x + 5,  btn_y + UI_SEEK_S/2,
             ff_x + 20, btn_y + 5,
             ff_x + 20, btn_y + UI_SEEK_S - 5, c, c, c, 1);
    draw_tri(ff_x + 12, btn_y + UI_SEEK_S/2,
             ff_x + 27, btn_y + 5,
             ff_x + 27, btn_y + UI_SEEK_S - 5, c, c, c, 1);
}

int ui_hit_play(struct overlay *ov, int x, int y) {
    int bar_y = ov->height - UI_BAR_H;
    if (y < bar_y || y >= ov->height) return 0;
    int btn_left = UI_MARGIN;
    return x >= btn_left && x < btn_left + UI_BTN_S + UI_MARGIN;
}

int ui_hit_seek(struct overlay *ov, int x, int y, int *out_pos) {
    int bar_y = ov->height - UI_BAR_H;
    if (y < bar_y || y >= ov->height) return 0;
    int seek_l = UI_MARGIN + UI_BTN_S + UI_MARGIN;
    int seek_r = ov->width - UI_MARGIN - (UI_SEEK_S + UI_MARGIN) * 2;
    if (x < seek_l || x >= seek_r) return 0;
    *out_pos = seek_l;
    return 1;
}

int ui_hit_rewind(struct overlay *ov, int x, int y) {
    int bar_y = ov->height - UI_BAR_H;
    if (y < bar_y || y >= ov->height) return 0;
    int seek_r = ov->width - UI_MARGIN - (UI_SEEK_S + UI_MARGIN) * 2;
    int btn_x = seek_r + UI_MARGIN;
    return x >= btn_x && x < btn_x + UI_SEEK_S;
}

int ui_hit_forward(struct overlay *ov, int x, int y) {
    int bar_y = ov->height - UI_BAR_H;
    if (y < bar_y || y >= ov->height) return 0;
    int seek_r = ov->width - UI_MARGIN - (UI_SEEK_S + UI_MARGIN) * 2;
    int btn_x = seek_r + UI_MARGIN + UI_SEEK_S + UI_MARGIN;
    return x >= btn_x && x < btn_x + UI_SEEK_S;
}
