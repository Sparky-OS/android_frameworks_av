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

#ifndef ANDROID_STEREO_FRAME_PACKING_H_
#define ANDROID_STEREO_FRAME_PACKING_H_

#include <stddef.h>
#include <stdint.h>
#include <media/stagefright/foundation/StereoLayout.h>

namespace android {
namespace stereo {

enum class ParseResult { VALID, IGNORED, MALFORMED };

// The syntax fields are retained independently of the normalized packing.
// Lifetime and layer scope belong to the decoder's picture/output-order path.
struct FramePacking {
    Packing packing = {MONO, UNSPECIFIED};
    uint32_t id = 0;
    uint32_t type = 0;
    uint32_t interpretation = 0;
    uint32_t quincunx = 0;
    uint32_t spatialFlipping = 0;
    uint32_t frame0Flipped = 0;
    uint32_t fieldViews = 0;
    uint32_t currentFrameIsFrame0 = 0;
    uint32_t frame0SelfContained = 0;
    uint32_t frame1SelfContained = 0;
    uint32_t grid[4] = {};
    uint32_t repetitionPeriod = 0;
    uint32_t persistence = 0;
    uint32_t upsampledAspectRatio = 0;
    bool cancel = false;
};

// Input is exactly one unescaped SEI payload, without NAL or payload headers.
// A failed/ignored parse leaves *result unchanged.
ParseResult parseFramePacking(const uint8_t *data, size_t size, bool hevc,
                              FramePacking *result);

}  // namespace stereo
}  // namespace android
#endif
