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

#include <media/stagefright/foundation/ABitReader.h>
#include <media/stagefright/foundation/StereoFramePacking.h>

namespace android {
namespace stereo {
namespace {
bool readUE(ABitReader &bits, uint32_t *value) {
    uint32_t bit, suffix;
    size_t zeros = 0;
    do {
        if (!bits.getBitsGraceful(1, &bit)) return false;
        if (bit) break;
        if (++zeros > 31) return false;
    } while (true);
    if (!bits.getBitsGraceful(zeros, &suffix)) return false;
    *value = ((uint32_t(1) << zeros) - 1) + suffix;
    return true;
}
}  // namespace

ParseResult parseFramePacking(const uint8_t *data, size_t size, bool hevc,
                              FramePacking *result) {
    if (data == nullptr || result == nullptr || size == 0) return ParseResult::MALFORMED;
    ABitReader bits(data, size);
    FramePacking parsed;
    uint32_t cancel, ignored;
    if (!readUE(bits, &parsed.id)) return ParseResult::MALFORMED;
    // H.264 D.2.26 / H.265 D.3.16: reserved usages are ignored.
    if ((parsed.id >= 256 && parsed.id <= 511) || parsed.id >= 0x80000000u) {
        return ParseResult::IGNORED;
    }
    if (!bits.getBitsGraceful(1, &cancel)) return ParseResult::MALFORMED;
    parsed.cancel = cancel;
    if (!cancel) {
        if (!bits.getBitsGraceful(7, &parsed.type)
                || !bits.getBitsGraceful(1, &parsed.quincunx)
                || !bits.getBitsGraceful(6, &parsed.interpretation)
                || !bits.getBitsGraceful(1, &parsed.spatialFlipping)
                || !bits.getBitsGraceful(1, &parsed.frame0Flipped)
                || !bits.getBitsGraceful(1, &parsed.fieldViews)
                || !bits.getBitsGraceful(1, &parsed.currentFrameIsFrame0)
                || !bits.getBitsGraceful(1, &parsed.frame0SelfContained)
                || !bits.getBitsGraceful(1, &parsed.frame1SelfContained)) {
            return ParseResult::MALFORMED;
        }
        if (!parsed.quincunx && parsed.type != 5) {
            for (auto &position : parsed.grid) {
                if (!bits.getBitsGraceful(4, &position)) return ParseResult::MALFORMED;
            }
        }
        if (!bits.getBitsGraceful(8, &ignored)) return ParseResult::MALFORMED;
        if (hevc) {
            if (!bits.getBitsGraceful(1, &parsed.persistence)) return ParseResult::MALFORMED;
        } else {
            if (!readUE(bits, &parsed.repetitionPeriod)) return ParseResult::MALFORMED;
            if (parsed.repetitionPeriod > 16384) return ParseResult::MALFORMED;
            parsed.persistence = parsed.repetitionPeriod != 0;
        }
    }
    if (hevc) {
        if (!bits.getBitsGraceful(1, &parsed.upsampledAspectRatio)) return ParseResult::MALFORMED;
    } else {
        // The extension flag and any following extension data are ignored.
        if (!bits.getBitsGraceful(1, &ignored)) return ParseResult::MALFORMED;
    }
    if (!cancel) {
        // H.264 Table D-9 assigns 0..7; H.265 Table D.8 assigns ONLY 3..5.
        if (parsed.type > 7 || (hevc && (parsed.type < 3 || parsed.type > 5))) {
            return ParseResult::IGNORED;
        }
        if (parsed.interpretation > 2 && parsed.type != 6) return ParseResult::IGNORED;
        if ((parsed.type == 0 && !parsed.quincunx)
                || (parsed.type >= 5 && parsed.quincunx)) return ParseResult::MALFORMED;
        constexpr Layout layouts[] = {CHECKERBOARD, COLUMN_INTERLEAVED, ROW_INTERLEAVED,
                                      SIDE_BY_SIDE, TOP_BOTTOM, TEMPORAL_INTERLEAVED,
                                      MONO, TILED};
        parsed.packing.layout = layouts[parsed.type];
        parsed.packing.order = parsed.type == 6 ? UNSPECIFIED
                : static_cast<ViewOrder>(parsed.interpretation);
    }
    *result = parsed;
    return ParseResult::VALID;
}

}  // namespace stereo
}  // namespace android
