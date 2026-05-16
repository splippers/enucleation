#include "text_overlay.h"
#include <android/log.h>
#include <cstring>

#define LOG_TAG "MonoView/Overlay"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ---------------------------------------------------------------------------
// Font data — 5×7 bitmap, row-major.
// kFontData[row * 27 + charIdx], where charIdx: A=0..Z=25, space=26.
// Each byte = 5-bit row pattern, bit4=leftmost pixel.
// ---------------------------------------------------------------------------
static const uint8_t kFontData[7 * 27] = {
    // row 0 (top) — A  B     C     D     E     F     G     H     I     J     K     L     M     N     O     P     Q     R     S     T     U     V     W     X     Y     Z    sp
    0x0E,0x1E,0x0E,0x1E,0x1F,0x1F,0x0E,0x11,0x1F,0x0F,0x11,0x10,0x11,0x11,0x0E,0x1E,0x0E,0x1E,0x0E,0x1F,0x11,0x11,0x11,0x11,0x11,0x1F,0x00,
    // row 1
    0x11,0x11,0x11,0x11,0x10,0x10,0x11,0x11,0x04,0x01,0x12,0x10,0x1B,0x19,0x11,0x11,0x11,0x11,0x11,0x04,0x11,0x11,0x11,0x11,0x11,0x01,0x00,
    // row 2
    0x11,0x11,0x10,0x11,0x10,0x10,0x10,0x11,0x04,0x01,0x14,0x10,0x15,0x15,0x11,0x11,0x11,0x11,0x10,0x04,0x11,0x11,0x11,0x0A,0x0A,0x02,0x00,
    // row 3
    0x1F,0x1E,0x10,0x11,0x1E,0x1E,0x17,0x1F,0x04,0x01,0x18,0x10,0x15,0x13,0x11,0x1E,0x11,0x1E,0x0E,0x04,0x11,0x11,0x15,0x04,0x04,0x04,0x00,
    // row 4
    0x11,0x11,0x10,0x11,0x10,0x10,0x11,0x11,0x04,0x11,0x14,0x10,0x11,0x11,0x11,0x10,0x15,0x14,0x01,0x04,0x11,0x11,0x15,0x0A,0x04,0x08,0x00,
    // row 5
    0x11,0x11,0x11,0x11,0x10,0x10,0x11,0x11,0x04,0x11,0x12,0x10,0x11,0x11,0x11,0x10,0x12,0x12,0x11,0x04,0x11,0x0A,0x1B,0x11,0x04,0x10,0x00,
    // row 6 (bottom)
    0x11,0x1E,0x0E,0x1E,0x1F,0x10,0x0F,0x11,0x1F,0x0E,0x11,0x1F,0x11,0x11,0x0E,0x10,0x0D,0x11,0x0E,0x04,0x0E,0x04,0x11,0x11,0x04,0x1F,0x00,
};

// ---------------------------------------------------------------------------
// Shaders
// ---------------------------------------------------------------------------
static const char* kVertSrc = R"glsl(
#version 300 es
void main() {
    // Fullscreen quad via gl_VertexID, no vertex buffer needed.
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
uniform int        u_mode;   // 0=both 1=left-only 2=right-only
uniform usampler2D u_font;   // 27 wide x 7 tall, GL_R8UI

out vec4 fragColor;

// Packed ASCII for the three label strings.
// ALL_TEXT[TEXT_OFF[m] .. TEXT_OFF[m]+TEXT_LEN[m]-1]
const int ALL_TEXT[36] = int[36](
    66,79,84,72,32,69,89,69,83,              // "BOTH EYES"      [0..8]
    76,69,70,84,32,69,89,69,32,79,78,76,89,  // "LEFT EYE ONLY"  [9..21]
    82,73,71,72,84,32,69,89,69,32,79,78,76,89// "RIGHT EYE ONLY" [22..35]
);
const int TEXT_OFF[3] = int[3](0, 9, 22);
const int TEXT_LEN[3] = int[3](9, 13, 14);

int fontIdx(int ascii) {
    return (ascii >= 65 && ascii <= 90) ? ascii - 65 : 26;
}

bool fontPixel(int fi, int col, int row) {
    uint bits = texelFetch(u_font, ivec2(fi, row), 0).r;
    return (bits & (0x10u >> uint(col))) != 0u;
}

void main() {
    const float SCALE  = 8.0;
    const float CHAR_W = 5.0 * SCALE;
    const float CHAR_H = 7.0 * SCALE;
    const float GAP    = 2.0 * SCALE;
    const float PAD    = 2.0 * SCALE;

    int m   = clamp(u_mode, 0, 2);
    int len = TEXT_LEN[m];
    int off = TEXT_OFF[m];

    float totalW = float(len) * (CHAR_W + GAP) - GAP;
    float startX = (u_res.x - totalW) * 0.5;
    float startY = u_res.y * 0.88;   // bottom of text block, ~12% from top

    float bgL = startX - PAD;
    float bgR = startX + totalW + PAD;
    float bgB = startY - PAD;
    float bgT = startY + CHAR_H + PAD;

    vec2 fc = gl_FragCoord.xy;
    if (fc.x < bgL || fc.x > bgR || fc.y < bgB || fc.y > bgT) {
        fragColor = vec4(0.0);
        return;
    }

    fragColor = vec4(0.0, 0.0, 0.0, 0.78);  // dark pill background

    float relX = fc.x - startX;
    float relY = fc.y - startY;

    if (relX >= 0.0 && relY >= 0.0 && relY < CHAR_H) {
        int   slot = int(relX / (CHAR_W + GAP));
        float locX = relX - float(slot) * (CHAR_W + GAP);

        if (slot < len && locX < CHAR_W) {
            int col = clamp(int(locX / SCALE), 0, 4);
            // row 0 = top of glyph; GL y=0 is bottom, so invert.
            int row = clamp(6 - int(relY / SCALE), 0, 6);

            int ascii = ALL_TEXT[off + slot];
            if (fontPixel(fontIdx(ascii), col, row)) {
                // White for both, blue tint for left, amber for right.
                if (m == 0)      fragColor = vec4(0.95, 0.95, 1.00, 1.0);
                else if (m == 1) fragColor = vec4(0.65, 0.85, 1.00, 1.0);
                else             fragColor = vec4(1.00, 0.82, 0.60, 1.0);
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

    loc_res_  = glGetUniformLocation(prog_, "u_res");
    loc_mode_ = glGetUniformLocation(prog_, "u_mode");
    loc_font_ = glGetUniformLocation(prog_, "u_font");

    // Empty VAO — vertex shader uses gl_VertexID only.
    glGenVertexArrays(1, &vao_);

    // Font texture: 27 wide × 7 tall, GL_R8UI.
    glGenTextures(1, &font_tex_);
    glBindTexture(GL_TEXTURE_2D, font_tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8UI,
                 27, 7, 0,
                 GL_RED_INTEGER, GL_UNSIGNED_BYTE,
                 kFontData);
    // Integer textures must use NEAREST filtering.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    ready_ = true;
    LOGI("TextOverlay ready");
    return true;
}

void TextOverlay::destroy() {
    if (font_tex_) { glDeleteTextures(1, &font_tex_);     font_tex_ = 0; }
    if (vao_)      { glDeleteVertexArrays(1, &vao_);       vao_      = 0; }
    if (prog_)     { glDeleteProgram(prog_);               prog_     = 0; }
    ready_ = false;
}

void TextOverlay::render(uint32_t width, uint32_t height, EyeMode mode) const {
    if (!ready_) return;

    // Save state we'll clobber.
    GLint prevProg = 0, prevVao = 0, prevTex = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM,    &prevProg);
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
    glUniform1i(loc_font_, 0);
    glBindTexture(GL_TEXTURE_2D, font_tex_);

    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // Restore state.
    glBindVertexArray((GLuint)prevVao);
    glUseProgram((GLuint)prevProg);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    if (!wasBlend)     glDisable(GL_BLEND);
    if (wasDepthTest)  glEnable(GL_DEPTH_TEST);
}
