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
#include <map>
#include <optional>
#include <tuple>
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

// The decoder supplies every picture, including skipped/non-output pictures.
// A scope is an explicit sequence and layer/view identity, never a timestamp.
class FramePackingHistory {
public:
    using Scope = std::pair<uint64_t, uint32_t>;
    void remember(Scope scope, int64_t poc, const std::optional<FramePacking> &message) {
        auto &pictures = mScopes[scope];
        auto inserted = pictures.emplace(poc, message);
        if (!inserted.second && message) inserted.first->second = message;
    }
    std::optional<FramePacking> resolve(Scope scope, int64_t poc) const {
        auto found = mScopes.find(scope);
        if (found == mScopes.end()) return std::nullopt;
        const auto &pictures = found->second;
        auto current = pictures.find(poc);
        if (current == pictures.end()) return std::nullopt;
        uint64_t following = 0;
        for (auto at = std::make_reverse_iterator(std::next(current));
             at != pictures.rend(); ++at, ++following) {
            if (!at->second) continue;
            FramePacking result = *at->second;
            if (result.cancel || (following && !result.persistence)) return std::nullopt;
            if (result.type == 5) result.currentFrameIsFrame0 ^= following & 1;
            return result;
        }
        return std::nullopt;
    }
    // Call only after a picture leaves the decoder in output order. Earlier
    // pictures can no longer be emitted, so preserve just their resolved state.
    // No fixed bound on HEVC non-output history is assumed.
    void retireThrough(Scope scope, int64_t poc) {
        auto found = mScopes.find(scope);
        if (found == mScopes.end() || !found->second.count(poc)) return;
        auto value = resolve(scope, poc);
        auto &pictures = found->second;
        pictures.erase(pictures.begin(), pictures.lower_bound(poc));
        pictures[poc] = value;
        for (auto it = mScopes.begin(); it != mScopes.end();) {
            if (it->first.first < scope.first) it = mScopes.erase(it);
            else ++it;
        }
    }
    void reset() { mScopes.clear(); }

private:
    std::map<Scope, std::map<int64_t, std::optional<FramePacking>>> mScopes;
};

}  // namespace stereo
}  // namespace android
#endif
