#include "text_overlay.h"
#include <android/log.h>
#include <cstring>
#include <cstdio>

#define LOG_TAG "MonoView/Overlay"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ---------------------------------------------------------------------------
// Font data — 5×7 bitmap, row-major, 40 characters.
// Index map:
//   0-25  = A-Z
//   26    = space
//   27-36 = 0-9
//   37    = %
//   38    = :
//   39    = * (preferred-mode badge)
//
// Storage: kFontData[row * 40 + charIdx], row 0 = top of glyph.
// Each byte: 5-bit row pattern, bit4 = leftmost pixel column.
// ---------------------------------------------------------------------------
static const uint8_t kFontData[7 * 40] = {
    // row 0 (top) ─ A..Z, sp, 0..9, %, :, *
    0x0E,0x1E,0x0E,0x1E,0x1F,0x1F,0x0E,0x11,0x1F,0x0F,
    0x11,0x10,0x11,0x11,0x0E,0x1E,0x0E,0x1E,0x0E,0x1F,
    0x11,0x11,0x11,0x11,0x11,0x1F, 0x00,
    /*0-9*/ 0x0E,0x04,0x0E,0x1F,0x02,0x1F,0x06,0x1F,0x0E,0x0E,
    /*% : **/ 0x11, 0x00, 0x00,
    // row 1
    0x11,0x11,0x11,0x11,0x10,0x10,0x11,0x11,0x04,0x01,
    0x12,0x10,0x1B,0x19,0x11,0x11,0x11,0x11,0x11,0x04,
    0x11,0x11,0x11,0x11,0x11,0x01, 0x00,
    0x11,0x0C,0x11,0x02,0x06,0x10,0x08,0x01,0x11,0x11,
    0x09, 0x04, 0x0A,
    // row 2
    0x11,0x11,0x10,0x11,0x10,0x10,0x10,0x11,0x04,0x01,
    0x14,0x10,0x15,0x15,0x11,0x11,0x11,0x11,0x10,0x04,
    0x11,0x11,0x11,0x0A,0x0A,0x02, 0x00,
    0x13,0x04,0x01,0x04,0x0A,0x1E,0x10,0x02,0x11,0x11,
    0x02, 0x00, 0x1F,
    // row 3
    0x1F,0x1E,0x10,0x11,0x1E,0x1E,0x17,0x1F,0x04,0x01,
    0x18,0x10,0x15,0x13,0x11,0x1E,0x11,0x1E,0x0E,0x04,
    0x11,0x11,0x15,0x04,0x04,0x04, 0x00,
    0x15,0x04,0x02,0x02,0x12,0x01,0x1E,0x04,0x0E,0x0F,
    0x04, 0x00, 0x0E,
    // row 4
    0x11,0x11,0x10,0x11,0x10,0x10,0x11,0x11,0x04,0x11,
    0x14,0x10,0x11,0x11,0x11,0x10,0x15,0x14,0x01,0x04,
    0x11,0x11,0x15,0x0A,0x04,0x08, 0x00,
    0x19,0x04,0x04,0x01,0x1F,0x01,0x11,0x08,0x11,0x01,
    0x08, 0x00, 0x1F,
    // row 5
    0x11,0x11,0x11,0x11,0x10,0x10,0x11,0x11,0x04,0x11,
    0x12,0x10,0x11,0x11,0x11,0x10,0x12,0x12,0x11,0x04,
    0x11,0x0A,0x1B,0x11,0x04,0x10, 0x00,
    0x11,0x04,0x08,0x11,0x02,0x11,0x11,0x08,0x11,0x02,
    0x13, 0x04, 0x0A,
    // row 6 (bottom)
    0x11,0x1E,0x0E,0x1E,0x1F,0x10,0x0F,0x11,0x1F,0x0E,
    0x11,0x1F,0x11,0x11,0x0E,0x10,0x0D,0x11,0x0E,0x04,
    0x0E,0x04,0x11,0x11,0x04,0x1F, 0x00,
    0x0E,0x0E,0x1F,0x0E,0x02,0x0E,0x0E,0x08,0x0E,0x0C,
    0x11, 0x00, 0x00,
};

// ---------------------------------------------------------------------------
// Shader source
// ---------------------------------------------------------------------------
static const char* kVertSrc = R"glsl(
#version 300 es
void main() {
    vec2 pos = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
)glsl";

static const char* kFragSrc = R"glsl(
#version 300 es
precision mediump float;
precision highp   int;
precision highp   usampler2D;

uniform vec2       u_res;
uniform int        u_mode;       // 0=both 1=left 2=right — colour selection only
uniform int        u_chars[32];  // line-1 char indices into the 40-char font table
uniform int        u_len;        // line-1 length
uniform int        u_chars2[16]; // line-2 char indices
uniform int        u_len2;       // line-2 length (0 = hidden)
uniform usampler2D u_font;       // 40 wide × 7 tall, GL_R8UI

out vec4 fragColor;

bool fontPixel(int fi, int col, int row) {
    uint bits = texelFetch(u_font, ivec2(fi, row), 0).r;
    return (bits & (0x10u >> uint(col))) != 0u;
}

void main() {
    const float SCALE    = 8.0;
    const float CHAR_W   = 5.0 * SCALE;
    const float CHAR_H   = 7.0 * SCALE;
    const float GAP      = 2.0 * SCALE;
    const float PAD      = 2.0 * SCALE;
    const float LINE_SEP = 6.0 * SCALE;

    vec2 fc = gl_FragCoord.xy;

    // ── Line 1 geometry ───────────────────────────────────────────────────
    float totalW1 = float(u_len) * (CHAR_W + GAP) - GAP;
    float startX1 = (u_res.x - totalW1) * 0.5;
    float startY1 = u_res.y * 0.88;

    float bgL1 = startX1 - PAD, bgR1 = startX1 + totalW1 + PAD;
    float bgB1 = startY1 - PAD, bgT1 = startY1 + CHAR_H + PAD;
    bool  inBG1 = fc.x >= bgL1 && fc.x <= bgR1 && fc.y >= bgB1 && fc.y <= bgT1;

    // ── Line 2 geometry (optional) ────────────────────────────────────────
    float totalW2 = float(u_len2) * (CHAR_W + GAP) - GAP;
    float startX2 = (u_res.x - totalW2) * 0.5;
    float startY2 = bgB1 - LINE_SEP - CHAR_H;
    float bgL2 = startX2 - PAD, bgR2 = startX2 + totalW2 + PAD;
    float bgB2 = startY2 - PAD, bgT2 = startY2 + CHAR_H + PAD;
    bool  inBG2 = u_len2 > 0 &&
                  fc.x >= bgL2 && fc.x <= bgR2 && fc.y >= bgB2 && fc.y <= bgT2;

    if (!inBG1 && !inBG2) { fragColor = vec4(0.0); return; }

    // Dark pill background
    fragColor = vec4(0.0, 0.0, 0.0, 0.80);

    // ── Colour palette per mode ───────────────────────────────────────────
    vec4 fg1;
    if      (u_mode == 1) fg1 = vec4(0.65, 0.85, 1.00, 1.0); // left  = blue
    else if (u_mode == 2) fg1 = vec4(1.00, 0.82, 0.60, 1.0); // right = amber
    else                  fg1 = vec4(0.95, 0.95, 1.00, 1.0); // both  = white
    vec4 fg2 = vec4(0.60, 1.00, 0.70, 1.0); // green for line-2 (opacity readout)

    // ── Render line 1 ─────────────────────────────────────────────────────
    if (inBG1) {
        float relX = fc.x - startX1;
        float relY = fc.y - startY1;
        if (relX >= 0.0 && relY >= 0.0 && relY < CHAR_H) {
            int   slot = int(relX / (CHAR_W + GAP));
            float locX = relX - float(slot) * (CHAR_W + GAP);
            if (slot < u_len && locX < CHAR_W) {
                int col = clamp(int(locX / SCALE), 0, 4);
                int row = clamp(6 - int(relY / SCALE), 0, 6);
                if (fontPixel(u_chars[slot], col, row)) fragColor = fg1;
            }
        }
    }

    // ── Render line 2 ─────────────────────────────────────────────────────
    if (inBG2) {
        float relX = fc.x - startX2;
        float relY = fc.y - startY2;
        if (relX >= 0.0 && relY >= 0.0 && relY < CHAR_H) {
            int   slot = int(relX / (CHAR_W + GAP));
            float locX = relX - float(slot) * (CHAR_W + GAP);
            if (slot < u_len2 && locX < CHAR_W) {
                int col = clamp(int(locX / SCALE), 0, 4);
                int row = clamp(6 - int(relY / SCALE), 0, 6);
                if (fontPixel(u_chars2[slot], col, row)) fragColor = fg2;
            }
        }
    }
}
)glsl";

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static GLuint compile_shader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[512];
        glGetShaderInfoLog(s, sizeof(buf), nullptr, buf);
        LOGE("Shader compile error: %s", buf);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint link_program(GLuint vert, GLuint frag) {
    GLuint p = glCreateProgram();
    glAttachShader(p, vert);
    glAttachShader(p, frag);
    glLinkProgram(p);
    glDetachShader(p, vert);
    glDetachShader(p, frag);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char buf[512];
        glGetProgramInfoLog(p, sizeof(buf), nullptr, buf);
        LOGE("Program link error: %s", buf);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

// Map an ASCII char to the 40-char font table index.
int TextOverlay::charIdx(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a';   // caps-only font
    if (c == ' ')              return 26;
    if (c >= '0' && c <= '9') return 27 + (c - '0');
    if (c == '%')              return 37;
    if (c == ':')              return 38;
    if (c == '*')              return 39;          // preferred badge
    return 26; // fallback = space
}

// ---------------------------------------------------------------------------
// TextOverlay
// ---------------------------------------------------------------------------
bool TextOverlay::init() {
    GLuint vert = compile_shader(GL_VERTEX_SHADER,   kVertSrc);
    GLuint frag = compile_shader(GL_FRAGMENT_SHADER, kFragSrc);
    if (!vert || !frag) {
        if (vert) glDeleteShader(vert);
        if (frag) glDeleteShader(frag);
        return false;
    }

    prog_ = link_program(vert, frag);
    glDeleteShader(vert);
    glDeleteShader(frag);
    if (!prog_) return false;

    loc_res_    = glGetUniformLocation(prog_, "u_res");
    loc_mode_   = glGetUniformLocation(prog_, "u_mode");
    loc_chars_  = glGetUniformLocation(prog_, "u_chars");
    loc_len_    = glGetUniformLocation(prog_, "u_len");
    loc_chars2_ = glGetUniformLocation(prog_, "u_chars2");
    loc_len2_   = glGetUniformLocation(prog_, "u_len2");
    loc_font_   = glGetUniformLocation(prog_, "u_font");

    glGenVertexArrays(1, &vao_);

    // Font texture: 40 wide × 7 tall, GL_R8UI
    glGenTextures(1, &font_tex_);
    glBindTexture(GL_TEXTURE_2D, font_tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8UI,
                 40, 7, 0,
                 GL_RED_INTEGER, GL_UNSIGNED_BYTE,
                 kFontData);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    ready_ = true;
    LOGI("TextOverlay ready (40-char font, dynamic text)");
    return true;
}

void TextOverlay::destroy() {
    if (font_tex_) { glDeleteTextures(1, &font_tex_);   font_tex_ = 0; }
    if (vao_)      { glDeleteVertexArrays(1, &vao_);    vao_      = 0; }
    if (prog_)     { glDeleteProgram(prog_);             prog_     = 0; }
    ready_ = false;
}

void TextOverlay::render(uint32_t width, uint32_t height,
                         EyeMode mode, float opacity,
                         bool passthrough_active, bool is_preferred,
                         bool edge_on) const {
    if (!ready_) return;

    // ── Build line 1: mode label + optional badges ─────────────────────
    int chars1[32] = {};
    int len1 = 0;

    auto addStr1 = [&](const char* s) {
        for (; *s && len1 < 32; ++s)
            chars1[len1++] = charIdx(*s);
    };

    if      (mode == EyeMode::LeftOnly)  addStr1("LEFT ONLY");
    else if (mode == EyeMode::RightOnly) addStr1("RIGHT ONLY");
    else                                 addStr1("BOTH EYES");

    if (is_preferred && mode != EyeMode::Both) {
        chars1[len1++] = charIdx(' ');
        chars1[len1++] = charIdx('*');  // star = preferred-mode badge
    }
    if (edge_on) {
        addStr1(" EDGE");
    }

    // ── Build line 2: opacity readout when passthrough active ─────────
    int chars2[16] = {};
    int len2 = 0;

    if (passthrough_active && mode != EyeMode::Both) {
        auto addStr2 = [&](const char* s) {
            for (; *s && len2 < 16; ++s)
                chars2[len2++] = charIdx(*s);
        };
        addStr2("OPC:");
        chars2[len2++] = charIdx(' ');
        int pct = (int)(opacity * 100.0f + 0.5f);
        if (pct > 100) pct = 100;
        if (pct >= 100) {
            chars2[len2++] = charIdx('1');
            chars2[len2++] = charIdx('0');
            chars2[len2++] = charIdx('0');
        } else if (pct >= 10) {
            chars2[len2++] = charIdx('0' + pct / 10);
            chars2[len2++] = charIdx('0' + pct % 10);
        } else {
            chars2[len2++] = charIdx('0' + pct);
        }
        chars2[len2++] = charIdx('%');
    }

    // ── OpenGL draw ───────────────────────────────────────────────────
    GLint prevProg = 0, prevVao = 0, prevTex = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM,      &prevProg);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    GLboolean wasBlend     = glIsEnabled(GL_BLEND);
    GLboolean wasDepthTest = glIsEnabled(GL_DEPTH_TEST);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);

    glUseProgram(prog_);
    glUniform2f(loc_res_,  (float)width, (float)height);
    glUniform1i(loc_mode_, (int)mode);
    glUniform1iv(loc_chars_,  32, chars1);
    glUniform1i(loc_len_,  len1);
    glUniform1iv(loc_chars2_, 16, chars2);
    glUniform1i(loc_len2_, len2);
    glUniform1i(loc_font_, 0);
    glBindTexture(GL_TEXTURE_2D, font_tex_);

    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // Restore state
    glBindVertexArray((GLuint)prevVao);
    glUseProgram((GLuint)prevProg);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    if (!wasBlend)    glDisable(GL_BLEND);
    if (wasDepthTest) glEnable(GL_DEPTH_TEST);
}
