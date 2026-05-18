#pragma once
#include <GLES3/gl3.h>
#include "eye_controller.h"

class TextOverlay {
public:
    bool init();
    void destroy();

    // Dynamic text overlay: mode label on line 1, opacity/brightness readout on line 2.
    // is_preferred: star badge shown when the user's learned preferred mode is active.
    // edge_on: "EDGE" badge + contrast boost active.
    // brightness: -0.5..0.5 (0 = default); shows BRIGHT / DIM badge when != 0.
    void render(uint32_t width, uint32_t height,
                EyeMode mode, float opacity,
                bool passthrough_active, bool is_preferred,
                bool edge_on, float brightness = 0.0f) const;

private:
    static int charIdx(char c);

    GLuint prog_        = 0;
    GLuint vao_         = 0;
    GLuint font_tex_    = 0;
    GLint  loc_res_     = -1;
    GLint  loc_mode_    = -1;  // 0/1/2 — drives label colour only
    GLint  loc_chars_   = -1;  // int[32]: line-1 char indices
    GLint  loc_len_     = -1;  // line-1 char count
    GLint  loc_chars2_  = -1;  // int[16]: line-2 char indices
    GLint  loc_len2_    = -1;  // line-2 char count (0 = hidden)
    GLint  loc_font_    = -1;
    bool   ready_       = false;
};
