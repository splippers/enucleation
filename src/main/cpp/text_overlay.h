#pragma once
#include <GLES3/gl3.h>
#include "eye_controller.h"

class TextOverlay {
public:
    bool init();
    void destroy();
    void render(uint32_t width, uint32_t height, EyeMode mode) const;

private:
    GLuint prog_     = 0;
    GLuint vao_      = 0;
    GLuint font_tex_ = 0;
    GLint  loc_res_  = -1;
    GLint  loc_mode_ = -1;
    GLint  loc_font_ = -1;
    bool   ready_    = false;
};
