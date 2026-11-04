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

#ifndef STEREO_VIDEO_INFO_H_
#define STEREO_VIDEO_INFO_H_

#include <cstddef>
#include <cstdint>

namespace android {
struct StereoVideoInfo {
    int32_t viewFlags = -1;
    int32_t hero = -1;
    int64_t packing = -1;
};

// Apple Video Extended Usage specification 1.9.8, sections 1 and 3.
// False means the top-level required-box contract was not satisfied.
bool parseStereoVideoInfo(const uint8_t *data, size_t size, StereoVideoInfo *info);
}
#endif
