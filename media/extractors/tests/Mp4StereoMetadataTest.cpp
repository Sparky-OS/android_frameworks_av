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
#include <media/stagefright/foundation/ABase.h>
#include <SampleTable.h>
#include <MPEG4Extractor.h>
#include <algorithm>
#include <cstring>
#include <vector>

namespace android {
namespace {
using Bytes = std::vector<uint8_t>;

void append(Bytes &out, const Bytes &data) {
    out.insert(out.end(), data.begin(), data.end());
}
void put32(Bytes &data, size_t offset, uint32_t value) {
    for (size_t i = 0; i < 4; ++i) data[offset + i] = value >> (24 - 8 * i);
}
Bytes words(std::initializer_list<uint32_t> values) {
    Bytes out(values.size() * 4);
    size_t offset = 0;
    for (auto value : values) { put32(out, offset, value); offset += 4; }
    return out;
}
Bytes box(const char *type, const Bytes &payload) {
    Bytes out(8);
    put32(out, 0, payload.size() + 8);
    memcpy(out.data() + 4, type, 4);
    append(out, payload);
    return out;
}
Bytes movie(const Bytes &extensions, bool wrongParent = false) {
    Bytes entry(78);
    entry[7] = 1;
    entry[25] = entry[27] = 16;
    entry[41] = 1;
    entry[75] = 24;
    entry[76] = entry[77] = 255;
    append(entry, box("d263", Bytes(7)));
    if (!wrongParent) append(entry, extensions);
    Bytes stsd = words({0, 1});
    append(stsd, box("s263", entry));
    Bytes stbl = box("stsd", stsd);
    append(stbl, box("stco", words({0, 1, 8})));
    append(stbl, box("stsc", words({0, 1, 1, 1, 1})));
    append(stbl, box("stsz", words({0, 1, 1})));
    append(stbl, box("stts", words({0, 1, 1, 1})));
    if (wrongParent) append(stbl, extensions);
    Bytes mdhd = words({0, 0, 0, 1000, 1, 0});
    Bytes mdia = box("mdhd", mdhd);
    append(mdia, box("minf", box("stbl", stbl)));
    Bytes tkhd(84);
    put32(tkhd, 12, 1);
    put32(tkhd, 40, 0x10000);
    put32(tkhd, 56, 0x10000);
    put32(tkhd, 72, 0x40000000);
    put32(tkhd, 76, 16 << 16);
    put32(tkhd, 80, 16 << 16);
    Bytes trak = box("tkhd", tkhd);
    append(trak, box("mdia", mdia));
    Bytes file = box("mdat", {0});
    append(file, box("moov", box("trak", trak)));
    return file;
}
class TestExtractor : public MPEG4Extractor {
public:
    explicit TestExtractor(DataSourceHelper *source) : MPEG4Extractor(source) {}
    ~TestExtractor() override = default;
};
void check(const Bytes &extensions, int expectedLayout, int expectedOrder,
           int rawMode, bool valid = true, bool wrongParent = false) {
    Bytes file = movie(extensions, wrongParent);
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
    ASSERT_EQ(valid ? 1u : 0u, extractor.countTracks());
    if (!valid) return;
    AMediaFormat *format = AMediaFormat_new();
    ASSERT_EQ(AMEDIA_OK, extractor.getTrackMetaData(format, 0, 0));
    int32_t layout = -1, order = -1, raw = -1;
    EXPECT_EQ(expectedLayout >= 0, AMediaFormat_getInt32(format, "stereo-layout", &layout));
    EXPECT_EQ(expectedLayout, layout);
    EXPECT_EQ(expectedOrder >= 0, AMediaFormat_getInt32(format, "stereo-view-order", &order));
    EXPECT_EQ(expectedOrder, order);
    EXPECT_EQ(rawMode >= 0, AMediaFormat_getInt32(format, "stereo-st3d-mode", &raw));
    EXPECT_EQ(rawMode, raw);
    AMediaFormat_delete(format);
}
class Mp4StereoMetadataTest : public testing::TestWithParam<int> {};
TEST_P(Mp4StereoMetadataTest, AssignedSt3dMode) {
    constexpr int layouts[] = {0, 2, 1, 17, 1};
    constexpr int orders[] = {0, 1, 1, 0, 2};
    check(box("st3d", {0, 0, 0, 0, uint8_t(GetParam())}),
          layouts[GetParam()], orders[GetParam()], GetParam());
}
INSTANTIATE_TEST_SUITE_P(AllAssigned, Mp4StereoMetadataTest, testing::Range(0, 5));
TEST(Mp4StereoMetadataTest, Absent) { check({}, -1, -1, -1); }
TEST(Mp4StereoMetadataTest, ReservedMode) { check(box("st3d", {0,0,0,0,255}), -1,-1,255); }
TEST(Mp4StereoMetadataTest, FutureVersion) { check(box("st3d", {1,0,0,0,2}), -1,-1,-1); }
TEST(Mp4StereoMetadataTest, ReservedFlags) { check(box("st3d", {0,0,0,1,2}), -1,-1,-1); }
TEST(Mp4StereoMetadataTest, Truncated) { check(box("st3d", {0,0,0,0}), -1,-1,-1,false); }
TEST(Mp4StereoMetadataTest, Oversized) { check(box("st3d", {0,0,0,0,2,0}), -1,-1,-1,false); }
TEST(Mp4StereoMetadataTest, Duplicate) {
    Bytes boxes = box("st3d", {0,0,0,0,1});
    append(boxes, box("st3d", {0,0,0,0,2}));
    check(boxes, -1,-1,-1,false);
}
TEST(Mp4StereoMetadataTest, DuplicateAfterUnknownVersion) {
    Bytes boxes = box("st3d", {1,0,0,0,1});
    append(boxes, box("st3d", {0,0,0,0,2}));
    check(boxes, -1,-1,-1,false);
}
TEST(Mp4StereoMetadataTest, WrongParent) {
    check(box("st3d", {0,0,0,0,2}), -1,-1,-1,true,true);
}
}  // namespace
}  // namespace android
