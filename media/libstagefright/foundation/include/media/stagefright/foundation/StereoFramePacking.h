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
#include <media/stagefright/foundation/StereoLayout.h>

namespace android {
namespace stereo {

enum class ParseResult { VALID, IGNORED, MALFORMED };

// The syntax fields are retained independently of the normalized packing.
// Lifetime and layer scope belong to the decoder's picture/output-order path.
struct FramePacking {
    Packing packing = {MONO, UNSPECIFIED};
    uint32_t payloadType = 45;
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

ParseResult parseSegmentedRect(const uint8_t *data, size_t size, FramePacking *result);

// The decoder supplies every picture, including skipped/non-output pictures.
// A scope is an explicit sequence and layer/view identity, never a timestamp.
class FramePackingHistory {
public:
    using Scope = std::pair<uint64_t, uint32_t>;
    void remember(Scope scope, int64_t poc, const std::optional<FramePacking> &message) {
        auto &event = mScopes[scope][poc];
        if (message) event = *message;
    }
    std::optional<FramePacking> resolve(Scope scope, int64_t poc) const {
        auto found = mScopes.find(scope);
        if (found == mScopes.end() || !found->second.count(poc)) return std::nullopt;
        const FramePacking *latest = nullptr;
        uint64_t following = 0;
        for (const auto &[at, event] : found->second) {
            if (at > poc) break;
            if (event) { latest = &*event; following = 0; }
            else if (latest) ++following;
        }
        if (!latest || latest->cancel || (following && !latest->persistence)) return std::nullopt;
        auto value = *latest;
        if (value.type == 5) value.currentFrameIsFrame0 ^= following & 1;
        return value;
    }
    std::optional<int64_t> pairStartPoc(Scope scope, int64_t poc) const {
        auto current = resolve(scope, poc);
        if (!current || current->type != 5) return std::nullopt;
        if (current->currentFrameIsFrame0) return poc;
        const auto &pictures = mScopes.at(scope);
        auto at = pictures.find(poc);
        if (at == pictures.begin()) return std::nullopt;
        --at;
        auto previous = resolve(scope, at->first);
        if (!previous || previous->type != 5 || !previous->currentFrameIsFrame0
                || previous->packing.order != current->packing.order) return std::nullopt;
        return at->first;
    }
    // Retain the current viewpoint's arrangement and phase at the boundary.
    // Skipped/non-output pictures still take part in temporal pair identity.
    void retireThrough(Scope scope, int64_t poc) {
        auto found = mScopes.find(scope);
        if (found == mScopes.end() || !found->second.count(poc)) return;
        auto value = resolve(scope, poc);
        auto &pictures = found->second;
        pictures.erase(pictures.begin(), pictures.lower_bound(poc));
        pictures[poc] = value;
        for (auto it = mScopes.begin(); it != mScopes.end();) {
            if (it->first.second == scope.second && it->first.first < scope.first) {
                it = mScopes.erase(it);
            }
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
