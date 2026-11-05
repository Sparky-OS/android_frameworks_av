/*
 * Copyright (C) 2018 The Android Open Source Project
 * Copyright 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>
 * SPDX-License-Identifier: Apache-2.0
 *
 * Mesh delta decoding and the coordinate/vertex/index bounds follow Android
 * Media3 ProjectionDecoder (c78c4cf017ea0014dba706d61935a523c8be54b0).
 * The owning extractor also checks CRC, all wrapper bounds, reserved fields,
 * finite coordinates, pose ranges and padding between every vertex list.
 * Format: google/spatial-media, docs/spherical-video-v2-rfc.md, "Mesh Box".
 */
#include "StereoProjection.h"
#include <cmath>
#include <cstring>
#include <utility>
#include <zlib.h>

namespace android::stereo {
namespace {
constexpr size_t kBudget = 32 * 1024 * 1024;
constexpr uint32_t fourcc(char a, char b, char c, char d) {
    return uint32_t(a) << 24 | uint32_t(b) << 16 | uint32_t(c) << 8 | uint32_t(d);
}
uint32_t be32(const uint8_t *p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
struct Box { uint32_t type; const uint8_t *data; size_t size; };
struct Boxes {
    const uint8_t *data; size_t size, at = 0; bool invalid = false;
    bool next(Box *box) {
        if (size - at < 8) return false; // allowed trailing extension bytes
        uint64_t length = be32(data + at); size_t header = 8;
        const uint32_t type = be32(data + at + 4);
        if (length == 1) {
            if (size - at < 16) { invalid = true; return false; }
            length = uint64_t(be32(data + at + 8)) << 32 | be32(data + at + 12);
            header = 16;
        } else if (!length) length = size - at;
        if (length < header || length > size - at) { invalid = true; return false; }
        *box = {type, data + at + header, size_t(length) - header};
        at += length;
        return true;
    }
};
struct Bits {
    const uint8_t *data; size_t size, at = 0;
    bool read(unsigned n, uint32_t *value) {
        if (n > 32 || n > size * 8 - at) return false;
        uint32_t v = 0;
        for (unsigned i = 0; i < n; ++i, ++at) v = (v << 1) | ((data[at / 8] >> (7 - at % 8)) & 1);
        *value = v; return true;
    }
    void align() { at = (at + 7) & ~size_t(7); }
};
bool word(std::vector<uint8_t> *out, uint32_t value) {
    if (out->size() > kBudget - 4) return false;
    for (int i = 3; i >= 0; --i) out->push_back(value >> (8 * i));
    return true;
}
unsigned indexBits(uint32_t count) {
    unsigned n = 0; uint64_t limit = uint64_t(count) * 2;
    while ((uint64_t(1) << n) < limit) ++n;
    return n;
}
int64_t zigzag(uint32_t value) { return int64_t(value >> 1) ^ -int64_t(value & 1); }

bool mesh(const Box &box, std::vector<uint8_t> *out, const char **error) {
    Bits bits{box.data, box.size}; uint32_t count;
    if (!bits.read(32, &count) || !count || count > 10000 || count > box.size / 4) return false;
    std::vector<uint32_t> coordinates(count);
    for (auto &coordinate : coordinates) {
        if (!bits.read(32, &coordinate)) return false;
        float value; static_assert(sizeof(value) == sizeof(coordinate));
        memcpy(&value, &coordinate, sizeof(value));
        if (!std::isfinite(value)) { *error = "Non-finite mesh coordinate"; return false; }
    }
    uint32_t vertices;
    if (!bits.read(32, &vertices) || !vertices || vertices > 32000 || !word(out, vertices)) return false;
    int64_t previous[5] = {};
    for (uint32_t v = 0; v < vertices; ++v) for (int axis = 0; axis < 5; ++axis) {
        uint32_t encoded;
        if (!bits.read(indexBits(count), &encoded)) return false;
        const int64_t index = previous[axis] + zigzag(encoded);
        if (index < 0 || uint64_t(index) >= coordinates.size()) return false;
        previous[axis] = index;
        if (!word(out, coordinates[index])) return false;
    }
    bits.align(); uint32_t lists;
    if (!bits.read(32, &lists) || !lists || lists > box.size / 6 || !word(out, lists)) return false;
    for (uint32_t list = 0; list < lists; ++list) {
        uint32_t texture, mode, indices;
        if (!bits.read(8, &texture) || !bits.read(8, &mode) || !bits.read(32, &indices)) return false;
        if (texture || mode > 2) { *error = "Reserved mesh texture or triangle mode"; return false; }
        if (!indices || indices > 128000 || !word(out, texture) || !word(out, mode) || !word(out, indices)) return false;
        int64_t index = 0;
        for (uint32_t i = 0; i < indices; ++i) {
            uint32_t encoded;
            if (!bits.read(indexBits(vertices), &encoded)) return false;
            index += zigzag(encoded);
            if (index < 0 || uint64_t(index) >= vertices || !word(out, static_cast<uint32_t>(index))) return false;
        }
        bits.align();
    }
    return true;
}

bool meshProjection(const Box &box, std::vector<std::vector<uint8_t>> *meshes, const char **error) {
    if (box.size < 12 || be32(box.data) != 0) { *error = "Unknown mesh projection version/flags"; return false; }
    const uint32_t crc = crc32(crc32(0L, Z_NULL, 0), box.data + 8, static_cast<uInt>(box.size - 8));
    if (crc != be32(box.data + 4)) { *error = "Mesh projection CRC mismatch"; return false; }
    const uint32_t encoding = be32(box.data + 8);
    const uint8_t *data = box.data + 12; size_t size = box.size - 12;
    std::vector<uint8_t> inflated;
    if (encoding == fourcc('d','f','l','8')) {
        z_stream stream{}; stream.next_in = const_cast<Bytef*>(data); stream.avail_in = static_cast<uInt>(size);
        if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) return false;
        int status = Z_OK;
        while (status == Z_OK) {
            uint8_t chunk[4096]; stream.next_out = chunk; stream.avail_out = sizeof(chunk);
            status = inflate(&stream, Z_NO_FLUSH);
            const size_t produced = sizeof(chunk) - stream.avail_out;
            if (produced > kBudget - inflated.size()) { status = Z_MEM_ERROR; break; }
            inflated.insert(inflated.end(), chunk, chunk + produced);
        }
        inflateEnd(&stream);
        if (status != Z_STREAM_END) { *error = "Invalid or oversized raw-deflate mesh projection"; return false; }
        data = inflated.data(); size = inflated.size();
    } else if (encoding != fourcc('r','a','w',' ')) {
        *error = "Unknown mesh projection compression"; return false;
    }
    Boxes boxes{data, size}; Box child;
    while (boxes.next(&child)) if (child.type == fourcc('m','e','s','h')) {
        if (meshes->size() == 2) { *error = "More than two projection meshes"; return false; }
        meshes->emplace_back();
        if (!mesh(child, &meshes->back(), error)) return false;
    }
    return !boxes.invalid && !meshes->empty();
}
} // namespace

bool decodeStereoProjection(const uint8_t *data, size_t size,
                            std::vector<uint8_t> *descriptor, const char **error) {
    descriptor->clear(); *error = "Invalid spherical mesh projection";
    if (!data || size > kBudget) return false;
    Boxes sv3d{data, size}; Box child, projection{}; unsigned headers = 0, projections = 0;
    while (sv3d.next(&child)) {
        if (child.type == fourcc('s','v','h','d')) {
            if (++headers != 1 || child.size < 5 || be32(child.data) != 0
                    || !memchr(child.data + 4, 0, child.size - 4)) return false;
        } else if (child.type == fourcc('p','r','o','j')) { projection = child; ++projections; }
    }
    if (sv3d.invalid || headers != 1 || projections != 1) return false;
    Boxes proj{projection.data, projection.size}; uint32_t pose[3]{}; unsigned poses = 0, types = 0;
    std::vector<std::vector<uint8_t>> meshes;
    while (proj.next(&child)) {
        if (child.type == fourcc('p','r','h','d')) {
            if (++poses != 1 || child.size < 16 || be32(child.data) != 0) return false;
            for (int i = 0; i < 3; ++i) {
                pose[i] = be32(child.data + 4 + 4 * i);
                const int64_t value = int32_t(pose[i]), bound = (i == 1 ? 90 : 180) * 65536;
                if (value < -bound || value > bound) { *error = "Projection pose outside its specified range"; return false; }
            }
        } else if (child.type == fourcc('m','s','h','p')) {
            if (++types != 1 || !meshProjection(child, &meshes, error)) return false;
        } else if (child.type == fourcc('e','q','u','i') || child.type == fourcc('c','b','m','p')) {
            ++types; *error = "Projection is not a stereo mesh";
        }
    }
    if (proj.invalid || poses != 1 || types != 1 || meshes.empty()) return false;
    std::vector<uint8_t> out;
    word(&out, 1);
    for (uint32_t value : pose) word(&out, value);
    word(&out, static_cast<uint32_t>(meshes.size()));
    for (const auto &bytes : meshes) {
        if (bytes.size() > kBudget - out.size()) return false;
        out.insert(out.end(), bytes.begin(), bytes.end());
    }
    *descriptor = std::move(out); *error = nullptr; return true;
}
} // namespace android::stereo
