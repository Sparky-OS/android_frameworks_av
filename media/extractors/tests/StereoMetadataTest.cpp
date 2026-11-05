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
#include "StereoMetadataTestUtils.h"
#include <MatroskaExtractor.h>
#include <algorithm>
#include <cstring>
#include <vector>
#include <memory>

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

Bytes matroska(int64_t stereoMode, const Bytes &extraTracks = {}, uint64_t uid = 1,
               bool forceStereoMode = false, const Bytes &blockPayload = {},
               const Bytes &tags = {}, bool tagsAfterCluster = false, bool unknownCluster = false) {
    Bytes header;
    for (auto pair : {std::pair<uint32_t, uint64_t>(0x4286, 1), {0x42f7, 1},
                      {0x42f2, 4}, {0x42f3, 8}, {0x4287, 4}, {0x4285, 2}}) {
        append(header, element(pair.first, number(pair.second)));
    }
    append(header, element(0x4282, {'m', 'a', 't', 'r', 'o', 's', 'k', 'a'}));
    Bytes file = element(0x1a45dfa3, header);
    Bytes video = element(0xb0, number(16));
    append(video, element(0xba, number(16)));
    if (stereoMode >= 0 || forceStereoMode) append(video, element(0x53b8, number(stereoMode)));
    Bytes track = element(0xd7, number(1));
    append(track, element(0x73c5, number(uid)));
    append(track, element(0x83, number(1)));
    append(track, element(0x86, {'V', '_', 'V', 'P', '8'}));
    append(track, element(0xe0, video));
    Bytes info = element(0x2ad7b1, number(1000000));
    append(info, element(0x4d80, {'t', 'e', 's', 't'}));
    append(info, element(0x5741, {'t', 'e', 's', 't'}));
    Bytes segment = element(0x1549a966, info);
    Bytes tracks = element(0xae, track);
    append(tracks, extraTracks);
    append(segment, element(0x1654ae6b, tracks));
    if (!tagsAfterCluster) append(segment, tags);
    Bytes cluster = element(0xe7, number(0));
    append(cluster, element(0xa3, blockPayload.empty() ? Bytes{0x81, 0, 0, 0x80, 0} : blockPayload));
    if (unknownCluster) {
        append(segment, number(0x1f43b675));
        segment.push_back(0xff);
        append(segment, cluster);
    } else append(segment, element(0x1f43b675, cluster));
    if (tagsAfterCluster) append(segment, tags);
    append(file, element(0x18538067, segment));
    return file;
}

class TestExtractor : public MatroskaExtractor {
public:
    explicit TestExtractor(DataSourceHelper *source) : MatroskaExtractor(source) {}
    ~TestExtractor() override = default;
};

CDataSource makeSource(Bytes &file) {
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
    return source;
}

// A small C extractor buffer group, so these tests read real demuxed samples.
class SampleBuffers {
    struct Buffer {
        Bytes bytes;
        size_t offset = 0, length = 0;
        AMediaFormat *meta = AMediaFormat_new();
        CMediaBuffer c{};
        explicit Buffer(size_t size) : bytes(size) {
            c.handle = this;
            c.release = [](void *) {};
            c.data = [](void *h) -> void* { return static_cast<Buffer*>(h)->bytes.data(); };
            c.size = [](void *h) { return static_cast<Buffer*>(h)->bytes.size(); };
            c.range_offset = [](void *h) { return static_cast<Buffer*>(h)->offset; };
            c.range_length = [](void *h) { return static_cast<Buffer*>(h)->length; };
            c.set_range = [](void *h, size_t o, size_t n) {
                auto *b = static_cast<Buffer*>(h); b->offset = o; b->length = n;
            };
            c.meta_data = [](void *h) { return static_cast<Buffer*>(h)->meta; };
        }
        ~Buffer() { AMediaFormat_delete(meta); }
    };
    size_t defaultSize = 1024;
    std::vector<std::unique_ptr<Buffer>> buffers;
public:
    CMediaBufferGroup c{};
    SampleBuffers() {
        c.handle = this;
        c.init = [](void *h, size_t, size_t size, size_t) {
            static_cast<SampleBuffers*>(h)->defaultSize = size; return true;
        };
        c.add_buffer = [](void *h, size_t size) { static_cast<SampleBuffers*>(h)->defaultSize = size; };
        c.has_buffers = [](void *) { return true; };
        c.acquire_buffer = [](void *h, CMediaBuffer **out, bool, size_t size) -> media_status_t {
            auto *p = static_cast<SampleBuffers*>(h);
            p->buffers.push_back(std::make_unique<Buffer>(std::max(size, p->defaultSize)));
            *out = &p->buffers.back()->c; return AMEDIA_OK;
        };
    }
};

void checkLaceSamples(int64_t mode, int count, bool laced, bool expected) {
    Bytes payload = {0x81, 0, 0, uint8_t(laced ? 0x84 : 0x80)};
    if (laced) payload.push_back(count - 1); // fixed-size lacing
    for (int i = 0; i < count; ++i) payload.push_back(0x20 + i);
    Bytes file = matroska(mode, {}, 1, false, payload);
    CDataSource source = makeSource(file);
    TestExtractor extractor(new DataSourceHelper(&source));
    auto *track = wrap(extractor.getTrack(0));
    ASSERT_NE(nullptr, track);
    SampleBuffers buffers;
    ASSERT_EQ(AMEDIA_OK, track->start(track->data, &buffers.c));
    int64_t previousBlock = -1;
    for (int i = 0; i < count; ++i) {
        CMediaBuffer *buffer = nullptr;
        ASSERT_EQ(AMEDIA_OK, track->read(track->data, &buffer, 0, 0));
        ASSERT_NE(nullptr, buffer);
        AMediaFormat *format = buffer->meta_data(buffer->handle);
        int64_t block = -1;
        int32_t index = -1, total = -1, layout = -1, order = -1;
        EXPECT_EQ(expected, AMediaFormat_getInt64(format, "stereo-block-id", &block));
        EXPECT_EQ(expected, AMediaFormat_getInt32(format, "stereo-lace-index", &index));
        EXPECT_EQ(expected, AMediaFormat_getInt32(format, "stereo-lace-count", &total));
        if (expected) {
            EXPECT_GE(block, 0);
            if (i) EXPECT_EQ(previousBlock, block);
            previousBlock = block;
            EXPECT_EQ(i, index); EXPECT_EQ(count, total);
            ASSERT_TRUE(AMediaFormat_getInt32(format, "stereo-layout", &layout));
            ASSERT_TRUE(AMediaFormat_getInt32(format, "stereo-view-order", &order));
            EXPECT_EQ(10, layout); EXPECT_EQ(mode == 13 ? 1 : 2, order);
        }
        EXPECT_EQ(size_t(1), buffer->range_length(buffer->handle));
        EXPECT_EQ(0x20 + i, static_cast<uint8_t*>(buffer->data(buffer->handle))[0]);
        buffer->release(buffer->handle);
    }
    EXPECT_EQ(AMEDIA_OK, track->stop(track->data));
    track->free(track->data); free(track);
}
TEST(StereoMetadataTest, LacedViewsRetainBlockAndConstituentIdentity) {
    checkLaceSamples(13, 2, true, true);
}
TEST(StereoMetadataTest, LacedViewsRetainRightFirstOrder) {
    checkLaceSamples(14, 2, true, true);
}
TEST(StereoMetadataTest, IncompleteLacedViewBlockIsNotInventedPair) {
    checkLaceSamples(13, 1, false, true);
}
TEST(StereoMetadataTest, ExtraLacedConstituentCountIsPreserved) {
    checkLaceSamples(13, 3, true, true);
}
TEST(StereoMetadataTest, OrdinaryLacingDoesNotBecomeStereo) {
    checkLaceSamples(-1, 2, true, false);
}

void check(int64_t mode, int expectedLayout, int expectedOrder, bool forceStereoMode = false) {
    Bytes file = matroska(mode, {}, 1, forceStereoMode);
    CDataSource source = makeSource(file);
    TestExtractor extractor(new DataSourceHelper(&source));
    ASSERT_EQ(1u, extractor.countTracks());
    AMediaFormat *format = AMediaFormat_new();
    ASSERT_EQ(AMEDIA_OK, extractor.getTrackMetaData(format, 0, 0));
    bridgeStereoFormat(format);
    int32_t layout = -1, order = -1;
    EXPECT_EQ(expectedLayout >= 0, AMediaFormat_getInt32(format, "stereo-layout", &layout));
    EXPECT_EQ(expectedLayout, layout);
    EXPECT_EQ(expectedLayout >= 0,
              AMediaFormat_getInt32(format, "stereo-view-order", &order));
    EXPECT_EQ(expectedOrder, order);
    int64_t raw = -1;
    EXPECT_EQ(mode >= 0 || forceStereoMode,
              AMediaFormat_getInt64(format, "stereo-matroska-mode", &raw));
    EXPECT_EQ(mode, raw);
    AMediaFormat_delete(format);
}

Bytes stereoTag(const char *name, const char *value, const Bytes &targets = {}) {
    Bytes simple = element(0x45a3, Bytes(name, name + strlen(name)));
    append(simple, element(0x4487, Bytes(value, value + strlen(value))));
    Bytes tag = element(0x63c0, targets);
    append(tag, element(0x67c8, simple));
    return element(0x7373, tag);
}

void checkTags(const Bytes &tagEntries, uint64_t uid,
               const std::vector<std::pair<uint32_t, std::pair<std::string, std::string>>> &expected,
               bool afterCluster = false, bool unknownCluster = false, bool multipleTags = false) {
    Bytes file = matroska(15, {}, uid, false, {},
                         multipleTags ? tagEntries : element(0x1254c367, tagEntries),
                         afterCluster, unknownCluster);
    CDataSource source = makeSource(file);
    TestExtractor extractor(new DataSourceHelper(&source));
    ASSERT_EQ(1u, extractor.countTracks());
    AMediaFormat *format = AMediaFormat_new();
    ASSERT_EQ(AMEDIA_OK, extractor.getTrackMetaData(format, 0, 0));
    bridgeStereoFormat(format);
    Bytes expectedBytes;
    append(expectedBytes, {0, 0, 0, 1});
    uint32_t count = expected.size();
    append(expectedBytes, {uint8_t(count >> 24), uint8_t(count >> 16), uint8_t(count >> 8), uint8_t(count)});
    auto u32 = [&](uint32_t n) {
        append(expectedBytes, {uint8_t(n >> 24), uint8_t(n >> 16), uint8_t(n >> 8), uint8_t(n)});
    };
    for (const auto &entry : expected) {
        u32(entry.first); u32(entry.second.first.size()); u32(entry.second.second.size());
        expectedBytes.insert(expectedBytes.end(), entry.second.first.begin(), entry.second.first.end());
        expectedBytes.insert(expectedBytes.end(), entry.second.second.begin(), entry.second.second.end());
    }
    void *data = nullptr; size_t size = 0;
    ASSERT_EQ(!expected.empty(), AMediaFormat_getBuffer(format, "stereo-matroska-tags", &data, &size));
    if (!expected.empty()) {
        ASSERT_EQ(expectedBytes.size(), size);
        EXPECT_EQ(0, memcmp(expectedBytes.data(), data, size));
    }
    AMediaFormat_delete(format);
}

TEST(StereoMetadataTest, DepthTagRetainsGlobalScopeAndRawConvention) {
    checkTags(stereoTag("STEREO3D_DEPTH", "farness zero=0.25 range=120"), 1,
              {{1, {"STEREO3D_DEPTH", "farness zero=0.25 range=120"}}});
}
TEST(StereoMetadataTest, DepthTagTracksUseFullUnsignedUid) {
    checkTags(stereoTag("STEREO3D_DEPTH", "metric near=1 far=9000",
                       element(0x63c5, number(UINT64_MAX))), UINT64_MAX,
              {{2, {"STEREO3D_DEPTH", "metric near=1 far=9000"}}});
}
TEST(StereoMetadataTest, ForeignTrackDepthTagIsNotGlobal) {
    checkTags(stereoTag("STEREO3D_DEPTH", "farness", element(0x63c5, number(2))), 1, {});
}
TEST(StereoMetadataTest, ZeroTrackTargetDepthTagAppliesToAllTracks) {
    checkTags(stereoTag("STEREO3D_DEPTH", "inverseZ", element(0x63c5, number(0))), 1,
              {{1, {"STEREO3D_DEPTH", "inverseZ"}}});
}
TEST(StereoMetadataTest, ChapterEditionAndAttachmentDepthTagsAreNotTrackWide) {
    for (uint32_t kind : {0x63c4, 0x63c6, 0x63c9}) {
        checkTags(stereoTag("STEREO3D_DEPTH", "farness", element(kind, number(1))), 1, {});
    }
}
TEST(StereoMetadataTest, ConflictingDepthMarksRemainDistinctAtTheirScope) {
    Bytes entries = stereoTag("STEREO3D_DEPTH", "nearness");
    append(entries, stereoTag("STEREO3D_DEPTH", "farness", element(0x63c5, number(1))));
    append(entries, stereoTag("STEREO3D_DEPTH", "inverseZ", element(0x63c5, number(1))));
    checkTags(entries, 1, {{1, {"STEREO3D_DEPTH", "nearness"}},
                         {2, {"STEREO3D_DEPTH", "farness"}},
                         {2, {"STEREO3D_DEPTH", "inverseZ"}}});
}
TEST(StereoMetadataTest, EmptyDepthMarkRemainsPresentAndDoesNotBecomeDefault) {
    checkTags(stereoTag("STEREO3D_DEPTH", ""), 1, {{1, {"STEREO3D_DEPTH", ""}}});
}
TEST(StereoMetadataTest, DepthAndLayoutTagsAfterClusterAreAvailableBeforeDecode) {
    Bytes entries = stereoTag("STEREO3D_LAYOUT", "depthSideBySideDepthFirst");
    append(entries, stereoTag("STEREO3D_DEPTH", "rangeInverse near=0.1 far=50"));
    checkTags(entries, 1, {{1, {"STEREO3D_LAYOUT", "depthSideBySideDepthFirst"}},
                         {1, {"STEREO3D_DEPTH", "rangeInverse near=0.1 far=50"}}}, true);
}

TEST(StereoMetadataTest, PostClusterDepthTagsFollowUnknownSizeClusterBoundary) {
    checkTags(stereoTag("STEREO3D_DEPTH", "farness"), 1,
              {{1, {"STEREO3D_DEPTH", "farness"}}}, true, true);
}
TEST(StereoMetadataTest, MultipleTopLevelTagsAreAllPreserved) {
    Bytes tags = element(0x1254c367, stereoTag("STEREO3D_DEPTH", "nearness"));
    append(tags, element(0x1254c367, stereoTag("STEREO3D_DEPTH", "farness")));
    checkTags(tags, 1, {{1, {"STEREO3D_DEPTH", "nearness"}},
                        {1, {"STEREO3D_DEPTH", "farness"}}}, false, false, true);
}
TEST(StereoMetadataTest, TargetScopeSurvivesTagArrayGrowth) {
    Bytes tags;
    std::vector<std::pair<uint32_t, std::pair<std::string, std::string>>> expected;
    for (int i = 0; i < 12; ++i) {
        append(tags, stereoTag("STEREO3D_DEPTH", "farness", element(0x63c5, number(1))));
        expected.push_back({2, {"STEREO3D_DEPTH", "farness"}});
    }
    checkTags(tags, 1, expected);
}

void checkHouseLayout(const Bytes &entries, int64_t mode, int expectedLayout, int expectedOrder) {
    Bytes file = matroska(mode, {}, 1, false, {}, element(0x1254c367, entries));
    CDataSource source = makeSource(file);
    TestExtractor extractor(new DataSourceHelper(&source));
    ASSERT_EQ(1u, extractor.countTracks());
    AMediaFormat *format = AMediaFormat_new();
    ASSERT_EQ(AMEDIA_OK, extractor.getTrackMetaData(format, 0, 0));
    bridgeStereoFormat(format);
    int32_t layout = -1, order = -1;
    EXPECT_EQ(expectedLayout >= 0, AMediaFormat_getInt32(format, "stereo-layout", &layout));
    EXPECT_EQ(expectedLayout, layout);
    EXPECT_EQ(expectedLayout >= 0, AMediaFormat_getInt32(format, "stereo-view-order", &order));
    EXPECT_EQ(expectedOrder, order);
    AMediaFormat_delete(format);
}
TEST(StereoMetadataTest, EveryHouseDepthLayoutKeepsItsExactMeaning) {
    const char *marks[] = {"depthSideBySidePictureFirst", "depthSideBySideDepthFirst",
        "depthTopBottomPictureFirst", "depthTopBottomDepthFirst", "depthGraphicsTopBottom",
        "depthBackgroundQuarters", "depthSeparate"};
    const int layout[] = {12,12,13,13,14,15,16}, order[] = {3,4,3,4,3,3,3};
    for (size_t i = 0; i < 7; ++i) checkHouseLayout(stereoTag("STEREO3D_LAYOUT", marks[i]), -1, layout[i], order[i]);
}
TEST(StereoMetadataTest, HouseMarkNeverOverridesNativeStereoMode) {
    checkHouseLayout(stereoTag("STEREO3D_LAYOUT", "depthSideBySideDepthFirst"), 15, 12, 3);
}
TEST(StereoMetadataTest, UnknownHouseLayoutDoesNotInventPacking) {
    checkHouseLayout(stereoTag("STEREO3D_LAYOUT", "futureDepthLayout"), -1, -1, -1);
}
TEST(StereoMetadataTest, ConflictingHouseLayoutsDoNotInventPacking) {
    Bytes entries = stereoTag("STEREO3D_LAYOUT", "depthSideBySidePictureFirst");
    append(entries, stereoTag("STEREO3D_LAYOUT", "depthTopBottomPictureFirst"));
    checkHouseLayout(entries, -1, -1, -1);
}
TEST(StereoMetadataTest, TrackLayoutMarkOverridesGlobalMark) {
    Bytes entries = stereoTag("STEREO3D_LAYOUT", "depthSideBySidePictureFirst");
    append(entries, stereoTag("STEREO3D_LAYOUT", "depthTopBottomDepthFirst", element(0x63c5, number(1))));
    checkHouseLayout(entries, -1, 13, 4);
}
TEST(StereoMetadataTest, NonDepthHouseVocabularyIsRetained) {
    const char *names[] = {"sideBySide", "sideBySideQuincunx", "topBottom", "checkerboard",
        "rowInterleaved", "columnInterleaved", "frameSequential", "blockLaced", "multiView"};
    const int layout[] = {1,1,2,3,5,4,6,10,11};
    for (size_t i = 0; i < 9; ++i) for (int order = 1; order <= 2; ++order) {
        std::string mark = std::string(names[i]) + (order == 1 ? "LeftFirst" : "RightFirst");
        checkHouseLayout(stereoTag("STEREO3D_LAYOUT", mark.c_str()), -1, layout[i], order);
    }
    checkHouseLayout(stereoTag("STEREO3D_LAYOUT", "mono"), -1, 0, 0);
    checkHouseLayout(stereoTag("STEREO3D_LAYOUT", "anaglyphCyanRed"), -1, 8, 1);
    checkHouseLayout(stereoTag("STEREO3D_LAYOUT", "anaglyphRedLeft"), -1, 8, 1);
    checkHouseLayout(stereoTag("STEREO3D_LAYOUT", "anaglyphRedRight"), -1, 8, 2);
    checkHouseLayout(stereoTag("STEREO3D_LAYOUT", "anaglyphGreenMagenta"), -1, 9, 1);
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
TEST(MatroskaStereoMetadataTest, UnsignedHighBitDoesNotRejectTrack) {
    check(INT64_MIN, -1, -1, true);
}
TEST(MatroskaStereoMetadataTest, UnsignedAllOnesIsNotMissing) {
    check(-1, -1, -1, true);
}


Bytes videoTrack(uint64_t numberValue, uint64_t uid, const Bytes &operation = {}) {
    Bytes track = element(0xd7, number(numberValue));
    append(track, element(0x73c5, number(uid)));
    append(track, element(0x83, number(1)));
    append(track, element(0x86, {'V', '_', 'V', 'P', '8'}));
    Bytes video = element(0xb0, number(16));
    append(video, element(0xba, number(16)));
    append(track, element(0xe0, video));
    append(track, operation);
    return element(0xae, track);
}

Bytes plane(uint64_t uid, uint64_t role) {
    Bytes fields = element(0xe5, number(uid));
    append(fields, element(0xe6, number(role)));
    return element(0xe4, fields);
}

Bytes group(const Bytes &planes) { return element(0xe2, element(0xe3, planes)); }

uint64_t readInteger(const uint8_t *data, size_t width) {
    uint64_t value = 0;
    for (size_t i = 0; i < width; ++i) value = (value << 8) | data[i];
    return value;
}

void checkGroup(uint64_t role, bool missingReference = false) {
    // UIDs are unsigned 64-bit, not TrackNumber or signed MediaFormat integers.
    constexpr uint64_t first = UINT64_C(0xfedcba9876543210), second = UINT64_MAX;
    Bytes planes = plane(first, 0);
    append(planes, plane(second, role));
    Bytes extra = videoTrack(2, missingReference ? 123 : second);
    append(extra, videoTrack(3, 456, group(planes)));
    Bytes file = matroska(-1, extra, first);
    CDataSource source = makeSource(file);
    TestExtractor extractor(new DataSourceHelper(&source));
    ASSERT_EQ(2u, extractor.countTracks()); // the virtual track is not a codec input
    for (size_t i = 0; i < (missingReference ? 1u : 2u); ++i) {
        AMediaFormat *format = AMediaFormat_new();
        ASSERT_EQ(AMEDIA_OK, extractor.getTrackMetaData(format, i, 0));
        bridgeStereoFormat(format);
        int32_t layout;
        EXPECT_FALSE(AMediaFormat_getInt32(format, "stereo-layout", &layout));
        void *raw;
        size_t size;
        ASSERT_TRUE(AMediaFormat_getBuffer(format, "stereo-track-uid", &raw, &size));
        ASSERT_EQ(8u, size);
        EXPECT_EQ(i ? second : first, readInteger(static_cast<uint8_t *>(raw), size));
        ASSERT_TRUE(AMediaFormat_getBuffer(format, "stereo-track-groups", &raw, &size));
        ASSERT_EQ(56u, size);
        const uint8_t *bytes = static_cast<uint8_t *>(raw);
        EXPECT_EQ(1u, readInteger(bytes, 4));
        EXPECT_EQ(1u, readInteger(bytes + 4, 4));
        EXPECT_EQ(456u, readInteger(bytes + 8, 8));
        EXPECT_EQ(missingReference || role > 5 ? UINT32_MAX : role >= 3 ? 16u : 11u,
                  readInteger(bytes + 16, 4));
        EXPECT_EQ(2u, readInteger(bytes + 20, 4));
        EXPECT_EQ(first, readInteger(bytes + 24, 8));
        EXPECT_EQ(0u, readInteger(bytes + 32, 8));
        EXPECT_EQ(second, readInteger(bytes + 40, 8));
        EXPECT_EQ(role, readInteger(bytes + 48, 8));
        AMediaFormat_delete(format);
    }
}

TEST(MatroskaStereoMetadataTest, AllPlaneRolesAndUnsignedUids) {
    for (uint64_t role = 0; role <= 5; ++role) checkGroup(role);
}

TEST(MatroskaStereoMetadataTest, UnknownPlaneRoleRetainedWithoutGuessing) {
    checkGroup(6);
    checkGroup(UINT64_MAX);
}

TEST(MatroskaStereoMetadataTest, MissingPlaneReferenceIsNotARecognizedGroup) {
    checkGroup(1, true);
}

void checkMalformedGroup(const Bytes &operation) {
    Bytes file = matroska(-1, videoTrack(2, 456, operation));
    CDataSource source = makeSource(file);
    TestExtractor extractor(new DataSourceHelper(&source));
    EXPECT_EQ(0u, extractor.countTracks());
}

TEST(MatroskaStereoMetadataTest, PlaneRequiresUidAndRole) {
    checkMalformedGroup(group(element(0xe4, element(0xe5, number(1)))));
    checkMalformedGroup(group(element(0xe4, element(0xe6, number(0)))));
    checkMalformedGroup(group(plane(0, 0)));
    checkMalformedGroup(group({}));
}

TEST(MatroskaStereoMetadataTest, DuplicatePlaneFieldsAndOperationsFail) {
    Bytes fields = element(0xe5, number(1));
    append(fields, element(0xe5, number(2)));
    append(fields, element(0xe6, number(0)));
    checkMalformedGroup(group(element(0xe4, fields)));
    Bytes operation = group(plane(1, 0));
    Bytes duplicate = operation;
    append(operation, duplicate);
    checkMalformedGroup(operation);
    Bytes combine = element(0xe3, plane(1, 0));
    duplicate = combine;
    append(combine, duplicate);
    checkMalformedGroup(element(0xe2, combine));
}

TEST(MatroskaStereoMetadataTest, PlaneIntegerCannotExceedEightBytes) {
    Bytes fields = element(0xe5, Bytes(9, 1));
    append(fields, element(0xe6, number(0)));
    checkMalformedGroup(group(element(0xe4, fields)));
}

}  // namespace
}  // namespace android
