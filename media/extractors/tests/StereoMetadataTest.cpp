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

#include <gtest/gtest.h>
#include <MatroskaExtractor.h>
#include <algorithm>
#include <cstring>
#include <vector>

namespace android {
namespace {
using Bytes = std::vector<uint8_t>;

void append(Bytes &out, const Bytes &data) {
    out.insert(out.end(), data.begin(), data.end());
}

Bytes number(uint64_t value) {
    Bytes data;
    do {
        data.insert(data.begin(), value & 255);
        value >>= 8;
    } while (value);
    return data;
}

Bytes element(uint32_t id, const Bytes &data) {
    Bytes out = number(id);
    size_t width = 1;
    while (data.size() >= (uint64_t(1) << (7 * width)) - 1) ++width;
    uint64_t size = data.size() | (uint64_t(1) << (7 * width));
    Bytes encoded(width);
    for (size_t i = 0; i < width; ++i) {
        encoded[width - 1 - i] = size & 255;
        size >>= 8;
    }
    append(out, encoded);
    append(out, data);
    return out;
}

Bytes matroska(int64_t stereoMode) {
    Bytes header;
    for (auto pair : {std::pair<uint32_t, uint64_t>(0x4286, 1), {0x42f7, 1},
                      {0x42f2, 4}, {0x42f3, 8}, {0x4287, 4}, {0x4285, 2}}) {
        append(header, element(pair.first, number(pair.second)));
    }
    append(header, element(0x4282, {'m', 'a', 't', 'r', 'o', 's', 'k', 'a'}));
    Bytes file = element(0x1a45dfa3, header);
    Bytes video = element(0xb0, number(16));
    append(video, element(0xba, number(16)));
    if (stereoMode >= 0) append(video, element(0x53b8, number(stereoMode)));
    Bytes track = element(0xd7, number(1));
    append(track, element(0x73c5, number(1)));
    append(track, element(0x83, number(1)));
    append(track, element(0x86, {'V', '_', 'V', 'P', '8'}));
    append(track, element(0xe0, video));
    Bytes info = element(0x2ad7b1, number(1000000));
    append(info, element(0x4d80, {'t', 'e', 's', 't'}));
    append(info, element(0x5741, {'t', 'e', 's', 't'}));
    Bytes segment = element(0x1549a966, info);
    append(segment, element(0x1654ae6b, element(0xae, track)));
    Bytes cluster = element(0xe7, number(0));
    append(cluster, element(0xa3, {0x81, 0, 0, 0x80, 0}));
    append(segment, element(0x1f43b675, cluster));
    append(file, element(0x18538067, segment));
    return file;
}

class TestExtractor : public MatroskaExtractor {
public:
    explicit TestExtractor(DataSourceHelper *source) : MatroskaExtractor(source) {}
    ~TestExtractor() override = default;
};

void check(int64_t mode, int expectedLayout, int expectedOrder) {
    Bytes file = matroska(mode);
    CDataSource source = {};
    source.handle = &file;
    source.readAt = [](void *handle, off64_t offset, void *data, size_t size) -> ssize_t {
        const Bytes &bytes = *static_cast<Bytes *>(handle);
        if (offset < 0) return -1;
        if (uint64_t(offset) >= bytes.size()) return 0;
        size = std::min(size, bytes.size() - size_t(offset));
        memcpy(data, bytes.data() + offset, size);
        return size;
    };
    source.getSize = [](void *handle, off64_t *size) -> status_t {
        *size = static_cast<Bytes *>(handle)->size();
        return OK;
    };
    source.flags = [](void *) -> uint32_t { return 0; };
    source.getUri = [](void *, char *, size_t) { return false; };
    TestExtractor extractor(new DataSourceHelper(&source));
    ASSERT_EQ(1u, extractor.countTracks());
    AMediaFormat *format = AMediaFormat_new();
    ASSERT_EQ(AMEDIA_OK, extractor.getTrackMetaData(format, 0, 0));
    int32_t layout = -1, order = -1;
    EXPECT_EQ(expectedLayout >= 0, AMediaFormat_getInt32(format, "stereo-layout", &layout));
    EXPECT_EQ(expectedLayout, layout);
    EXPECT_EQ(expectedLayout >= 0,
              AMediaFormat_getInt32(format, "stereo-view-order", &order));
    EXPECT_EQ(expectedOrder, order);
    int64_t raw = -1;
    EXPECT_EQ(mode >= 0, AMediaFormat_getInt64(format, "stereo-matroska-mode", &raw));
    EXPECT_EQ(mode, raw);
    AMediaFormat_delete(format);
}

class MatroskaStereoMetadataTest : public testing::TestWithParam<int> {};

TEST_P(MatroskaStereoMetadataTest, AssignedModeReachesTrackFormat) {
    // Independent expectations from RFC 9559 Table 5 and IANA's 2026-10-26 registry.
    constexpr int layouts[] = {0, 1, 2, 2, 3, 3, 5, 5, 4, 4, 8, 1, 9, 10, 10,
                               12, 12, 13, 13, 14, 15};
    constexpr int orders[] = {0, 1, 2, 1, 2, 1, 2, 1, 2, 1, 0, 2, 0, 1, 2,
                              3, 4, 3, 4, 0, 0};
    check(GetParam(), layouts[GetParam()], orders[GetParam()]);
}
INSTANTIATE_TEST_SUITE_P(AllAssigned, MatroskaStereoMetadataTest, testing::Range(0, 21));

TEST(MatroskaStereoMetadataTest, AbsentIsNotExplicitMono) { check(-1, -1, -1); }
TEST(MatroskaStereoMetadataTest, UnassignedStaysAbsent) { check(21, -1, -1); }
TEST(MatroskaStereoMetadataTest, LargeUnassignedStaysAbsent) { check(0x7fffffff, -1, -1); }

}  // namespace
}  // namespace android
