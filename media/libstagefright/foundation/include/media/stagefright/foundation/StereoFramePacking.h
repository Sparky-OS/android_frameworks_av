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
#include <vector>
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
        auto &messages = mScopes[scope][poc];
        if (!message) return;
        if (message->cancel) {
            messages.clear();
            messages.push_back(*message);
            return;
        }
        for (auto &entry : messages) {
            if (!entry.cancel && entry.id == message->id) { entry = *message; return; }
        }
        messages.push_back(*message);
    }
    std::vector<FramePacking> resolveAll(Scope scope, int64_t poc) const {
        auto found = mScopes.find(scope);
        if (found == mScopes.end() || !found->second.count(poc)) return {};
        std::map<uint32_t, std::pair<FramePacking, uint64_t>> active;
        uint64_t ordinal = 0;
        for (const auto &[at, messages] : found->second) {
            if (at > poc) break;
            ++ordinal;
            for (const auto &message : messages) {
                if (message.cancel) active.clear();
                else active[message.id] = {message, ordinal};
            }
        }
        std::vector<FramePacking> result;
        for (const auto &[id, entry] : active) {
            auto value = entry.first;
            const uint64_t following = ordinal - entry.second;
            if (following && !value.persistence) continue;
            if (value.type == 5) value.currentFrameIsFrame0 ^= following & 1;
            result.push_back(value);
        }
        return result;
    }
    std::optional<FramePacking> resolve(Scope scope, int64_t poc) const {
        auto values = resolveAll(scope, poc);
        // Distinct application usages may coexist. Never guess which usage a
        // caller means; the scalar convenience key requires a unique descriptor.
        return values.size() == 1 ? std::optional<FramePacking>(values[0]) : std::nullopt;
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
                || previous->id != current->id) return std::nullopt;
        return at->first;
    }
    // Retain all independent usages at the retirement boundary. Pictures not
    // output still take part in temporal phase and pair identity.
    void retireThrough(Scope scope, int64_t poc) {
        auto found = mScopes.find(scope);
        if (found == mScopes.end() || !found->second.count(poc)) return;
        auto values = resolveAll(scope, poc);
        auto &pictures = found->second;
        pictures.erase(pictures.begin(), pictures.lower_bound(poc));
        pictures[poc] = std::move(values);
        for (auto it = mScopes.begin(); it != mScopes.end();) {
            if (it->first.first < scope.first) it = mScopes.erase(it);
            else ++it;
        }
    }
    void reset() { mScopes.clear(); }

private:
    std::map<Scope, std::map<int64_t, std::vector<FramePacking>>> mScopes;
};

}  // namespace stereo
}  // namespace android
#endif
