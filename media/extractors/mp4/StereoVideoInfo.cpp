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

#define LOG_TAG "StereoVideoInfo"
#include "StereoVideoInfo.h"

#include <media/stagefright/foundation/ByteUtils.h>
#include <utils/Log.h>
#include <vector>

namespace android {
namespace {
constexpr uint32_t code(char a, char b, char c, char d) {
    return uint32_t(a) << 24 | uint32_t(b) << 16 | uint32_t(c) << 8 | uint32_t(d);
}
constexpr uint32_t kVexu = code('v','e','x','u'), kEyes = code('e','y','e','s');
constexpr uint32_t kPack = code('p','a','c','k'), kMust = code('m','u','s','t');
constexpr uint32_t kStri = code('s','t','r','i'), kHero = code('h','e','r','o');
constexpr uint32_t kPkin = code('p','k','i','n'), kFree = code('f','r','e','e');
struct Box {
    uint32_t type;
    const uint8_t *data;
    size_t size;
    bool understood = false;
};

bool split(const uint8_t *data, size_t size, std::vector<Box> *boxes) {
    while (size) {
        if (size < 8) return false;
        uint64_t length = U32_AT(data);
        size_t header = 8;
        if (length == 1) {
            if (size < 16) return false;
            length = U64_AT(data + 8);
            header = 16;
        } else if (!length) {
            length = size;
        }
        if (length < header || length > size) return false;
        boxes->push_back({U32_AT(data + 4), data + header, size_t(length - header), false});
        data += length;
        size -= length;
    }
    return true;
}

bool recognized(uint32_t parent, uint32_t type) {
    if (type == kMust || type == kFree) return true;
    if (parent == kVexu) return type == kEyes || type == kPack;
    if (parent == kEyes) return type == kStri || type == kHero;
    if (parent == kPack) return type == kPkin;
    return false;
}

bool hierarchy(const uint8_t *data, size_t size, uint32_t parent, StereoVideoInfo *info) {
    std::vector<Box> boxes;
    if (!split(data, size, &boxes)) return false;
    StereoVideoInfo local;
    const Box *must = nullptr;
    bool hasStri = false;
    for (auto &box : boxes) {
        if (box.type != kFree && recognized(parent, box.type)) {
            size_t occurrences = 0;
            for (const auto &other : boxes) occurrences += other.type == box.type;
            if (occurrences > 1) return false;
        }
        if (box.type == kMust) {
            if (box.size < 4 || U32_AT(box.data) != 0 || box.size % 4) return false;
            must = &box;
            box.understood = true;
        } else if (box.type == kFree) {
            box.understood = true;
        } else if (parent == kVexu && (box.type == kEyes || box.type == kPack)) {
            StereoVideoInfo child;
            box.understood = hierarchy(box.data, box.size, box.type, &child);
            if (box.understood) {
                if (box.type == kEyes) {
                    local.viewFlags = child.viewFlags;
                    local.hero = child.hero;
                } else {
                    local.packing = child.packing;
                }
            } else {
                ALOGW("Ignoring invalid optional vexu subtree %08x unless required", box.type);
            }
        } else if (parent == kEyes && box.type == kStri) {
            hasStri = true;
            box.understood = box.size == 5 && U32_AT(box.data) == 0 && !(box.data[4] & 0xf0);
            if (!box.understood) return false; // stri is intrinsically mandatory
            local.viewFlags = box.data[4];
        } else if (parent == kEyes && box.type == kHero) {
            box.understood = box.size == 5 && U32_AT(box.data) == 0 && box.data[4] <= 2;
            if (box.understood) local.hero = box.data[4];
        } else if (parent == kPack && box.type == kPkin) {
            box.understood = box.size == 8 && U32_AT(box.data) == 0;
            if (box.understood) {
                local.packing = U32_AT(box.data + 4);
                box.understood = local.packing == 0 || local.packing == code('s','i','d','e')
                        || local.packing == code('o','v','e','r');
                if (!box.understood) local.packing = -1;
            }
        }
    }
    if (parent == kEyes && !hasStri) return false;
    if (must) {
        bool requiresStri = false;
        for (size_t i = 4; i < must->size; i += 4) {
            uint32_t type = U32_AT(must->data + i);
            if (!type) continue;
            if (!recognized(parent, type) || type == kFree) return false;
            requiresStri |= type == kStri;
            for (const auto &box : boxes) {
                if (box.type == type && !box.understood) return false;
            }
            // The specification allows absent known boxes named by must.
        }
        if (parent == kEyes && !requiresStri) return false;
    }
    *info = local;
    return true;
}
} // namespace

bool parseStereoVideoInfo(const uint8_t *data, size_t size, StereoVideoInfo *info) {
    return hierarchy(data, size, kVexu, info);
}
} // namespace android
