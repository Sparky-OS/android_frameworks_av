/*
 * Copyright 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ANDROID_STEREO_LAYOUT_H_
#define ANDROID_STEREO_LAYOUT_H_

#include <stdint.h>

namespace android {
namespace stereo {

// Draft API values, independent of the container's and composer's numbering.
enum Layout : int32_t {
    MONO = 0,
    SIDE_BY_SIDE = 1,
    TOP_BOTTOM = 2,
    CHECKERBOARD = 3,
    COLUMN_INTERLEAVED = 4,
    ROW_INTERLEAVED = 5,
    TEMPORAL_INTERLEAVED = 6,
    TILED = 7,
    ANAGLYPH_CYAN_RED = 8,
    ANAGLYPH_GREEN_MAGENTA = 9,
    LACED_VIEWS = 10,
    SEPARATE_VIEWS = 11,
    PICTURE_DEPTH_SIDE_BY_SIDE = 12,
    PICTURE_DEPTH_TOP_BOTTOM = 13,
    PICTURE_DEPTH_GRAPHICS = 14,
    PICTURE_DEPTH_BACKGROUND = 15,
    SEPARATE_DEPTH = 16,
    CUSTOM = 17,
    SEGMENTED_RECTANGULAR = 18,
};

enum ViewOrder : int32_t {
    UNSPECIFIED = 0,
    LEFT_FIRST = 1,
    RIGHT_FIRST = 2,
    PICTURE_FIRST = 3,
    DEPTH_FIRST = 4,
};

struct Packing {
    Layout layout;
    ViewOrder order;
};

// RFC 9559, Table 5; IANA Matroska Stereo Modes, 2026-10-26.
inline bool fromMatroska(uint64_t value, Packing *packing) {
    static constexpr Packing kModes[] = {
        {MONO, UNSPECIFIED},
        {SIDE_BY_SIDE, LEFT_FIRST},
        {TOP_BOTTOM, RIGHT_FIRST},
        {TOP_BOTTOM, LEFT_FIRST},
        {CHECKERBOARD, RIGHT_FIRST},
        {CHECKERBOARD, LEFT_FIRST},
        {ROW_INTERLEAVED, RIGHT_FIRST},
        {ROW_INTERLEAVED, LEFT_FIRST},
        {COLUMN_INTERLEAVED, RIGHT_FIRST},
        {COLUMN_INTERLEAVED, LEFT_FIRST},
        {ANAGLYPH_CYAN_RED, UNSPECIFIED},
        {SIDE_BY_SIDE, RIGHT_FIRST},
        {ANAGLYPH_GREEN_MAGENTA, UNSPECIFIED},
        {LACED_VIEWS, LEFT_FIRST},
        {LACED_VIEWS, RIGHT_FIRST},
        {PICTURE_DEPTH_SIDE_BY_SIDE, PICTURE_FIRST},
        {PICTURE_DEPTH_SIDE_BY_SIDE, DEPTH_FIRST},
        {PICTURE_DEPTH_TOP_BOTTOM, PICTURE_FIRST},
        {PICTURE_DEPTH_TOP_BOTTOM, DEPTH_FIRST},
        {PICTURE_DEPTH_GRAPHICS, UNSPECIFIED},
        {PICTURE_DEPTH_BACKGROUND, UNSPECIFIED},
    };
    if (value >= sizeof(kModes) / sizeof(kModes[0])) {
        return false;
    }
    *packing = kModes[value];
    return true;
}

// Google Spherical Video V2, Stereoscopic 3D Video Box.
inline bool fromSt3d(uint8_t value, Packing *packing) {
    static constexpr Packing kModes[] = {
        {MONO, UNSPECIFIED},
        {TOP_BOTTOM, LEFT_FIRST},
        {SIDE_BY_SIDE, LEFT_FIRST},
        {CUSTOM, UNSPECIFIED},
        {SIDE_BY_SIDE, RIGHT_FIRST},
    };
    if (value >= sizeof(kModes) / sizeof(kModes[0])) {
        return false;
    }
    *packing = kModes[value];
    return true;
}

}  // namespace stereo
}  // namespace android

#endif  // ANDROID_STEREO_LAYOUT_H_
