#pragma once
#include <cstdint>

enum class EyeMode : uint8_t { Both, LeftOnly, RightOnly };

// eye: 0 = left, 1 = right
inline bool eye_is_active(EyeMode mode, int eye) {
    switch (mode) {
        case EyeMode::Both:      return true;
        case EyeMode::LeftOnly:  return eye == 0;
        case EyeMode::RightOnly: return eye == 1;
    }
    return true;
}

inline EyeMode cycle_mode(EyeMode m) {
    switch (m) {
        case EyeMode::Both:      return EyeMode::LeftOnly;
        case EyeMode::LeftOnly:  return EyeMode::RightOnly;
        case EyeMode::RightOnly: return EyeMode::Both;
    }
    return EyeMode::Both;
}

inline const char* mode_name(EyeMode m) {
    switch (m) {
        case EyeMode::Both:      return "Both";
        case EyeMode::LeftOnly:  return "Left only";
        case EyeMode::RightOnly: return "Right only";
    }
    return "Unknown";
}
