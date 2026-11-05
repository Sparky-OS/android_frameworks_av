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
#include <media/stagefright/foundation/ABase.h>
#include <media/stagefright/foundation/ByteUtils.h>
#include <SampleTable.h>
#include <MPEG4Extractor.h>
#include <algorithm>
#include <cstring>
#include <vector>
#include <zlib.h>

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
Bytes movie(const Bytes &extensions, bool wrongParent = false, bool restricted = false,
            const Bytes &sampleExtensions = {}, uint32_t sampleCount = 1) {
    Bytes entry(78);
    entry[7] = 1;
    entry[25] = entry[27] = 16;
    entry[41] = 1;
    entry[75] = 24;
    entry[76] = entry[77] = 255;
    append(entry, box("d263", Bytes(7)));
    if (!wrongParent) append(entry, extensions);
    Bytes stsd = words({0, 1});
    append(stsd, box(restricted ? "resv" : "s263", entry));
    Bytes stbl = box("stsd", stsd);
    append(stbl, box("stco", words({0, 1, 8})));
    append(stbl, box("stsc", words({0, 1, 1, sampleCount, 1})));
    append(stbl, box("stsz", words({0, 1, sampleCount})));
    append(stbl, box("stts", words({0, 1, sampleCount, 1})));
    if (wrongParent) append(stbl, extensions);
    append(stbl, sampleExtensions);
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
           int rawMode, bool valid = true, bool wrongParent = false,
           int64_t stviScheme = -1, int quincunx = -1, bool restricted = false,
           const Bytes &sampleExtensions = {}, uint32_t sampleCount = 1,
           const Bytes &expectedRuns = {}, int presence = -1, int additional = -1,
           int reversed = -1, int hero = -1, int64_t vexuPacking = -1) {
    Bytes file = movie(extensions, wrongParent, restricted, sampleExtensions, sampleCount);
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
    bridgeStereoFormat(format);
    int32_t layout = -1, order = -1, raw = -1;
    EXPECT_EQ(expectedLayout >= 0, AMediaFormat_getInt32(format, "stereo-layout", &layout));
    EXPECT_EQ(expectedLayout, layout);
    EXPECT_EQ(expectedOrder >= 0, AMediaFormat_getInt32(format, "stereo-view-order", &order));
    EXPECT_EQ(expectedOrder, order);
    EXPECT_EQ(rawMode >= 0, AMediaFormat_getInt32(format, "stereo-st3d-mode", &raw));
    EXPECT_EQ(rawMode, raw);
    int64_t actualScheme = -1;
    EXPECT_EQ(stviScheme >= 0,
              AMediaFormat_getInt64(format, "stereo-stvi-scheme", &actualScheme));
    EXPECT_EQ(stviScheme, actualScheme);
    int32_t actualQuincunx = -1;
    EXPECT_EQ(quincunx >= 0,
              AMediaFormat_getInt32(format, "stereo-quincunx", &actualQuincunx));
    EXPECT_EQ(quincunx, actualQuincunx);
    void *runs;
    size_t runsSize;
    EXPECT_EQ(!expectedRuns.empty(),
              AMediaFormat_getBuffer(format, "stereo-sample-layouts", &runs, &runsSize));
    if (!expectedRuns.empty()) {
        ASSERT_EQ(expectedRuns.size(), runsSize);
        EXPECT_EQ(0, memcmp(expectedRuns.data(), runs, runsSize));
    }
    for (auto field : {std::pair<const char *, int32_t>("stereo-view-presence", presence),
                       {"stereo-additional-views", additional}, {"stereo-views-reversed", reversed},
                       {"stereo-hero-view", hero}}) {
        int32_t value = -1;
        EXPECT_EQ(field.second >= 0, AMediaFormat_getInt32(format, field.first, &value));
        EXPECT_EQ(field.second, value);
    }
    int64_t packing = -1;
    EXPECT_EQ(vexuPacking >= 0, AMediaFormat_getInt64(format, "stereo-vexu-packing", &packing));
    EXPECT_EQ(vexuPacking, packing);
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
Bytes stviBody(uint32_t scheme, const Bytes &indication) {
    Bytes body = words({0, 3, scheme, uint32_t(indication.size())});
    append(body, indication);
    return body;
}
Bytes restriction(const Bytes &body) {
    Bytes rinf = box("frma", {'s','2','6','3'});
    append(rinf, box("schm", words({0, 0x73747669, 0})));
    append(rinf, box("schi", box("stvi", body)));
    return box("rinf", rinf);
}
void checkStvi(uint32_t scheme, const Bytes &indication, int layout, int order,
               int quincunx = -1) {
    check(restriction(stviBody(scheme, indication)), layout, order, -1,
          true, false, scheme, quincunx, true);
}
TEST(Mp4StereoMetadataTest, StviAvcVocabularyHasUnspecifiedOrder) {
    constexpr int layouts[] = {3,4,5,1,2,6,0,7};
    for (uint32_t mode = 0; mode < 8; ++mode) {
        checkStvi(1, words({mode}), layouts[mode], 0);
    }
    checkStvi(1, words({8}), -1, -1);
}
TEST(Mp4StereoMetadataTest, StviMpeg2Vocabulary) {
    checkStvi(2, words({3}), 1, 1, 0);
    checkStvi(2, words({4}), 2, 1, 0);
    checkStvi(2, words({8}), 0, 0, 0);
    checkStvi(2, words({0x23}), 1, 1, 1);
    checkStvi(2, words({5}), -1, -1);
}
TEST(Mp4StereoMetadataTest, StviSvafVocabularyAndBothOrders) {
    constexpr int layouts[] = {1,4,6,11,2,1,2};
    for (uint8_t mode = 0; mode < 7; ++mode) {
        checkStvi(3, {mode, 0}, layouts[mode], 2);
        checkStvi(3, {mode, 1}, layouts[mode], 1);
    }
    checkStvi(3, {7, 1}, -1, -1);
}
TEST(Mp4StereoMetadataTest, StviCicpWithImplicitAndExplicitInterpretation) {
    constexpr int layouts[] = {3,4,5,1,2,6,0};
    for (uint8_t mode = 0; mode < 7; ++mode) {
        for (uint8_t q : {0, 1}) {
            checkStvi(4, {mode, q}, layouts[mode], mode == 6 ? 0 : 1, q);
            for (uint8_t order : {0, 1, 2}) {
                checkStvi(5, {mode, q, order}, layouts[mode], mode == 6 ? 0 : order, q);
            }
        }
    }
    checkStvi(4, {7, 0}, -1, -1);
    checkStvi(5, {7, 0, 1}, -1, -1);
    checkStvi(5, {3, 0, 3}, -1, -1);
}
TEST(Mp4StereoMetadataTest, StviUnknownSchemePreservesRawOnly) {
    checkStvi(6, {3, 0, 1}, -1, -1);
    checkStvi(0xffffffffu, {}, -1, -1);
}
TEST(Mp4StereoMetadataTest, StviIncorrectKnownLengths) {
    for (uint32_t scheme : {1,2,3,4,5}) {
        check(restriction(stviBody(scheme, {3})), -1,-1,-1,false,false,-1,-1,true);
    }
}
TEST(Mp4StereoMetadataTest, StviLengthCannotCrossItsBox) {
    Bytes body = stviBody(4, {3, 0});
    put32(body, 12, 0xffffffffu);
    check(restriction(body), -1,-1,-1,false,false,-1,-1,true);
}
TEST(Mp4StereoMetadataTest, StviReservedBitsAreNotViewOrder) {
    check(restriction(stviBody(3, {0, 2})), -1,-1,-1,false,false,-1,-1,true);
    check(restriction(stviBody(4, {3, 2})), -1,-1,-1,false,false,-1,-1,true);
    check(restriction(stviBody(5, {3, 2, 1})), -1,-1,-1,false,false,-1,-1,true);
}
TEST(Mp4StereoMetadataTest, StviOptionalChildDoesNotChangeIndicationLength) {
    Bytes body = stviBody(5, {3, 1, 2});
    append(body, box("free", {1,2,3}));
    check(restriction(body), 1,2,-1,true,false,5,1,true);
}
TEST(Mp4StereoMetadataTest, StviAndSt3dConflictFailsInEitherOrder) {
    Bytes first = restriction(stviBody(5, {3, 0, 2}));
    Bytes second = box("st3d", {0,0,0,0,2});
    Bytes boxes = first;
    append(boxes, second);
    check(boxes, -1,-1,-1,false,false,-1,-1,true);
    append(second, first);
    check(second, -1,-1,-1,false,false,-1,-1,true);
}
TEST(Mp4StereoMetadataTest, StviAndSt3dAgree) {
    Bytes boxes = restriction(stviBody(5, {3, 0, 1}));
    append(boxes, box("st3d", {0,0,0,0,2}));
    check(boxes, 1,1,2,true,false,5,0,true);
}

Bytes svmi(uint8_t composition, uint8_t first, const std::vector<std::pair<uint32_t, uint8_t>> &runs) {
    Bytes data = {0, 0, 0, 0, composition, first};
    append(data, words({uint32_t(runs.size() - 1)}));
    for (const auto &run : runs) {
        append(data, words({run.first}));
        data.push_back(run.second);
    }
    return box("svmi", data);
}

void checkSvmi(const Bytes &data, uint32_t samples, int layout, int order,
               const Bytes &expected, bool valid = true) {
    check({}, layout, order, -1, valid, false, -1, -1, false, data, samples, expected);
}

TEST(Mp4StereoMetadataTest, SvmiAllCompositionsAndOrders) {
    constexpr uint32_t layouts[] = {1, 4, 6, 11, 2, 1, 2};
    for (uint8_t composition = 0; composition <= 6; ++composition) {
        for (uint8_t first : {0, 1}) {
            uint32_t order = first ? 1 : 2;
            checkSvmi(svmi(composition, first, {{1, 1}}), 1, layouts[composition], order,
                      words({1, 1, 1, layouts[composition], order}));
        }
    }
}

TEST(Mp4StereoMetadataTest, SvmiMixedRunsDoNotClaimOneTrackWideLayout) {
    checkSvmi(svmi(0, 1, {{2, 1}, {3, 0}, {4, 1}}), 9, -1, -1,
              words({1, 3, 2, 1, 1, 3, 0, 0, 4, 1, 1}));
}

TEST(Mp4StereoMetadataTest, SvmiAllMonoIsExplicitMono) {
    checkSvmi(svmi(0, 1, {{2, 0}}), 2, 0, 0, words({1, 1, 2, 0, 0}));
}

TEST(Mp4StereoMetadataTest, SvmiUnknownCompositionPreservesMonoRuns) {
    checkSvmi(svmi(7, 1, {{1, 1}, {1, 0}}), 2, -1, -1,
              words({1, 2, 1, UINT32_MAX, 0, 1, 0, 0}));
}

TEST(Mp4StereoMetadataTest, SvmiRunsMustCoverSamplesAndBeNonempty) {
    checkSvmi(svmi(0, 1, {{2, 1}}), 1, -1, -1, {}, false);
    checkSvmi(svmi(0, 1, {{1, 1}}), 2, -1, -1, {}, false);
    checkSvmi(svmi(0, 1, {{0, 1}}), 1, -1, -1, {}, false);
}

TEST(Mp4StereoMetadataTest, SvmiReservedBitsAreNotOrderOrStereo) {
    checkSvmi(svmi(0, 3, {{1, 1}}), 1, -1, -1, {}, false);
    checkSvmi(svmi(0, 1, {{1, 3}}), 1, -1, -1, {}, false);
}

TEST(Mp4StereoMetadataTest, SvmiEveryTruncationAndOversizeFails) {
    Bytes data = {0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1};
    for (size_t length = 0; length < data.size(); ++length) {
        checkSvmi(box("svmi", Bytes(data.begin(), data.begin() + length)), 1, -1, -1, {}, false);
    }
    data.push_back(0);
    checkSvmi(box("svmi", data), 1, -1, -1, {}, false);
    put32(data, 6, UINT32_MAX);
    checkSvmi(box("svmi", data), 1, -1, -1, {}, false);
}

TEST(Mp4StereoMetadataTest, SvmiDuplicateFails) {
    Bytes data = svmi(0, 1, {{1, 1}});
    Bytes duplicate = data;
    append(data, duplicate);
    checkSvmi(data, 1, -1, -1, {}, false);
}

TEST(Mp4StereoMetadataTest, SvmiFutureVersionIsNotGuessed) {
    checkSvmi(box("svmi", {1, 0, 0, 0, 0, 1, 0, 0, 0, 0}), 1, -1, -1, {});
}

TEST(Mp4StereoMetadataTest, SvmiOutsideSampleTableIgnored) {
    check(svmi(0, 1, {{1, 1}}), -1, -1, -1);
}

TEST(Mp4StereoMetadataTest, SvmiConflictingStaticArrangementFails) {
    check(box("st3d", {0, 0, 0, 0, 1}), -1, -1, -1, false, false, -1, -1, false,
          svmi(0, 1, {{1, 1}}));
}

TEST(Mp4StereoMetadataTest, SvmiMixedRunsOverrideConsistentStaticArrangement) {
    check(box("st3d", {0, 0, 0, 0, 2}), -1, -1, 2, true, false, -1, -1, false,
          svmi(0, 1, {{1, 1}, {1, 0}}), 2, words({1, 2, 1, 1, 1, 1, 0, 0}));
}


constexpr uint32_t kSide = 0x73696465, kOver = 0x6f766572;
constexpr uint32_t kEyes = 0x65796573, kPack = 0x7061636b, kStri = 0x73747269;
constexpr uint32_t kPkin = 0x706b696e, kHero = 0x6865726f, kUnknown = 0x78787878;
Bytes must(std::initializer_list<uint32_t> types) {
    Bytes data = words({0});
    append(data, words(types));
    return box("must", data);
}
Bytes eyes(uint8_t flags, const Bytes &extra = {}) {
    Bytes data = box("stri", {0, 0, 0, 0, flags});
    append(data, extra);
    return box("eyes", data);
}
Bytes pack(uint32_t kind, const Bytes &extra = {}) {
    Bytes data = box("pkin", words({0, kind}));
    append(data, extra);
    return box("pack", data);
}
void checkVexu(const Bytes &body, int layout, int order, bool valid = true,
               int flags = -1, int hero = -1, int64_t kind = -1) {
    check(box("vexu", body), layout, order, -1, valid, false, -1, -1, false, {}, 1, {},
          flags < 0 ? -1 : flags & 3, flags < 0 ? -1 : (flags >> 2) & 1,
          flags < 0 ? -1 : (flags >> 3) & 1, hero, kind);
}

TEST(Mp4StereoMetadataTest, VexuBothPackingsAndViewOrders) {
    for (uint32_t kind : {kSide, kOver}) {
        for (uint8_t flags : {3, 11}) {
            Bytes body = eyes(flags, must({kStri}));
            append(body, pack(kind, must({kPkin})));
            append(body, must({kEyes, kPack}));
            checkVexu(body, kind == kSide ? 1 : 2, flags == 3 ? 1 : 2, true, flags, -1, kind);
        }
    }
}

TEST(Mp4StereoMetadataTest, VexuNoPackingInferredFromViewPresence) {
    for (uint8_t flags = 0; flags < 16; ++flags) {
        bool mono = !(flags & 7);
        checkVexu(eyes(flags), mono ? 0 : -1, mono ? 0 : -1, true, flags);
    }
}

TEST(Mp4StereoMetadataTest, VexuPackingDoesNotImplyStereoViewMembership) {
    checkVexu(pack(kSide), -1, -1, true, -1, -1, kSide);
    Bytes body = eyes(7);
    append(body, pack(kSide));
    checkVexu(body, -1, -1, true, 7, -1, kSide);
}

TEST(Mp4StereoMetadataTest, VexuHeroViewRetainedWithoutChoosingIt) {
    for (uint8_t hero = 0; hero <= 2; ++hero) {
        checkVexu(eyes(3, box("hero", {0, 0, 0, 0, hero})), -1, -1, true, 3, hero);
    }
}

TEST(Mp4StereoMetadataTest, VexuEmptyAndFreeSpaceAreValid) {
    checkVexu({}, -1, -1);
    checkVexu(box("free", Bytes(17)), -1, -1);
}

TEST(Mp4StereoMetadataTest, VexuUnknownOptionalDoesNotInvalidateKnownViews) {
    Bytes body = eyes(3);
    append(body, box("xxxx", {1, 2, 3}));
    checkVexu(body, -1, -1, true, 3);
}

TEST(Mp4StereoMetadataTest, VexuUnknownRequiredFailsEvenWhenAbsent) {
    checkVexu(must({kUnknown}), -1, -1, false);
    Bytes body = must({kUnknown});
    append(body, box("xxxx", {}));
    checkVexu(body, -1, -1, false);
}

TEST(Mp4StereoMetadataTest, VexuRequiredSubtreeFailurePropagatesOnlyIfRequired) {
    Bytes body = eyes(3, must({kStri, kUnknown}));
    checkVexu(body, -1, -1); // optional eyes subtree contributes no partial semantics
    append(body, must({kEyes}));
    checkVexu(body, -1, -1, false);
}

TEST(Mp4StereoMetadataTest, VexuMustAllowsZeroAndAbsentKnownBoxes) {
    checkVexu(must({0, kPack}), -1, -1);
}

TEST(Mp4StereoMetadataTest, VexuStriIsMandatoryAndItsReservedBitsInvalidateEyes) {
    for (const Bytes &bad : {box("eyes", {}), eyes(0x13),
                            box("eyes", box("stri", {1, 0, 0, 0, 3}))}) {
        checkVexu(bad, -1, -1);
        Bytes required = must({kEyes});
        append(required, bad);
        checkVexu(required, -1, -1, false);
    }
}

TEST(Mp4StereoMetadataTest, VexuEyesMustNamesStri) {
    Bytes body = eyes(3, must({kHero}));
    checkVexu(body, -1, -1);
    append(body, must({kEyes}));
    checkVexu(body, -1, -1, false);
}

TEST(Mp4StereoMetadataTest, VexuUnknownRequiredPackingFails) {
    Bytes body = pack(kUnknown, must({kPkin}));
    append(body, must({kPack}));
    checkVexu(body, -1, -1, false);
}

TEST(Mp4StereoMetadataTest, VexuBoxOrderDoesNotChangeMeaning) {
    Bytes body = must({kEyes, kPack});
    append(body, pack(kSide, must({kPkin})));
    Bytes children = must({kStri});
    append(children, box("hero", {0, 0, 0, 0, 2}));
    append(children, box("stri", {0, 0, 0, 0, 11}));
    append(body, box("eyes", children));
    checkVexu(body, 1, 2, true, 11, 2, kSide);
}

TEST(Mp4StereoMetadataTest, VexuMalformedChildrenAndDuplicateBoxesFail) {
    checkVexu({0, 0, 0, 8}, -1, -1, false);
    checkVexu(words({1000, kEyes}), -1, -1, false);
    checkVexu(box("must", {0, 0, 0}), -1, -1, false);
    Bytes body = eyes(3);
    append(body, eyes(3));
    checkVexu(body, -1, -1, false);
    Bytes duplicate = box("vexu", {});
    append(duplicate, box("vexu", {}));
    check(duplicate, -1, -1, -1, false);
}

TEST(Mp4StereoMetadataTest, VexuExtendedSizeAndToEndBoxes) {
    Bytes child = box("stri", {0, 0, 0, 0, 3});
    Bytes body = words({1, kEyes, 0, uint32_t(16 + child.size())});
    append(body, child);
    checkVexu(body, -1, -1, true, 3);
    body = words({0, kEyes});
    append(body, child);
    checkVexu(body, -1, -1, true, 3);
}

CDataSource projectionSource(Bytes &file) {
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
class ProjectionBits {
public:
    Bytes bytes;
    size_t bit = 0;
    void put(uint32_t value, unsigned count) {
        for (unsigned n = count; n > 0; --n, ++bit) {
            if (bit / 8 == bytes.size()) bytes.push_back(0);
            bytes[bit / 8] |= ((value >> (n - 1)) & 1) << (7 - bit % 8);
        }
    }
    void align() { while (bit % 8) put(0, 1); }
};
uint32_t floatWord(float value) { uint32_t bits; memcpy(&bits, &value, 4); return bits; }
Bytes projectionMesh(uint8_t mode = 0, bool badIndex = false, bool multipleLists = false) {
    const float coordinates[] = {-1, 0, 1};
    ProjectionBits bits; bits.put(3, 32);
    for (float c : coordinates) bits.put(floatWord(c), 32);
    bits.put(4, 32);
    const int vertices[4][5] = {{0,0,0,1,1},{2,0,0,2,1},{2,2,0,2,2},{0,2,0,1,2}};
    int previous[5] = {};
    for (const auto &v : vertices) for (int i = 0; i < 5; ++i) {
        const int delta = v[i] - previous[i]; previous[i] = v[i];
        bits.put(delta >= 0 ? delta * 2 : -delta * 2 - 1, 3);
    }
    bits.align(); bits.put(multipleLists ? 2 : 1, 32);
    for (unsigned list = 0; list < (multipleLists ? 2u : 1u); ++list) {
        bits.put(0, 8); bits.put(mode, 8); bits.put(3, 32);
        bits.put(0, 3); bits.put(2, 3); bits.put(badIndex ? 6 : 2, 3);
        bits.align();
    }
    return box("mesh", bits.bytes);
}
Bytes spherical(const Bytes &meshes, bool compressed = false, bool badCrc = false,
                int32_t yaw = 0, bool header = true) {
    Bytes payload = meshes;
    if (compressed) {
        z_stream stream{};
        EXPECT_EQ(Z_OK, deflateInit2(&stream, 1, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY));
        Bytes encoded(compressBound(payload.size()));
        stream.next_in = payload.data(); stream.avail_in = static_cast<uInt>(payload.size());
        stream.next_out = encoded.data(); stream.avail_out = static_cast<uInt>(encoded.size());
        EXPECT_EQ(Z_STREAM_END, deflate(&stream, Z_FINISH));
        encoded.resize(stream.total_out); deflateEnd(&stream); payload = encoded;
    }
    Bytes encoded = compressed ? Bytes{'d','f','l','8'} : Bytes{'r','a','w',' '}; append(encoded, payload);
    uint32_t crc = crc32(crc32(0L, Z_NULL, 0), encoded.data(), static_cast<uInt>(encoded.size()));
    Bytes mshp = words({0, crc ^ uint32_t(badCrc)}); append(mshp, encoded);
    Bytes proj = box("prhd", words({0, uint32_t(yaw), 0, 0})); append(proj, box("mshp", mshp));
    Bytes body;
    if (header) { Bytes svhd = words({0}); append(svhd, {'t','e','s','t',0}); append(body, box("svhd", svhd)); }
    append(body, box("proj", proj)); return body;
}
void checkProjection(const Bytes &body, bool recognized, int meshCount = 2, int32_t yaw = 0) {
    Bytes extensions = box("st3d", {0,0,0,0,3}); append(extensions, box("sv3d", body));
    Bytes file = movie(extensions); CDataSource source = projectionSource(file);
    TestExtractor extractor(new DataSourceHelper(&source));
    ASSERT_EQ(1u, extractor.countTracks());
    AMediaFormat *format = AMediaFormat_new();
    ASSERT_EQ(AMEDIA_OK, extractor.getTrackMetaData(format, 0, 0)); bridgeStereoFormat(format);
    int32_t status = -1, layout = -1;
    ASSERT_TRUE(AMediaFormat_getInt32(format, "stereo-layout", &layout)); EXPECT_EQ(17, layout);
    ASSERT_TRUE(AMediaFormat_getInt32(format, "stereo-projection-status", &status)); EXPECT_EQ(recognized, status);
    void *raw = nullptr; size_t rawSize = 0;
    ASSERT_TRUE(AMediaFormat_getBuffer(format, "stereo-sv3d-data", &raw, &rawSize));
    ASSERT_EQ(body.size(), rawSize); EXPECT_EQ(0, memcmp(body.data(), raw, rawSize));
    void *data = nullptr; size_t size = 0;
    EXPECT_EQ(recognized, AMediaFormat_getBuffer(format, "stereo-projection-meshes", &data, &size));
    if (recognized) {
        ASSERT_GE(size, size_t(20));
        const auto *bytes = static_cast<const uint8_t*>(data);
        EXPECT_EQ(1u, uint32_t(bytes[3])); EXPECT_EQ(uint32_t(yaw), U32_AT(bytes + 4));
        EXPECT_EQ(uint32_t(meshCount), U32_AT(bytes + 16));
        EXPECT_EQ(4u, U32_AT(bytes + 20)); EXPECT_EQ(floatWord(-1), U32_AT(bytes + 24));
    } else {
        const char *error = nullptr;
        ASSERT_TRUE(AMediaFormat_getString(format, "stereo-projection-error", &error));
        EXPECT_NE(nullptr, error); EXPECT_NE('\0', error[0]);
    }
    AMediaFormat_delete(format);
}
TEST(Mp4StereoMetadataTest, CustomMeshRawDescriptorCrossesFormatBridge) {
    Bytes meshes = projectionMesh(); append(meshes, projectionMesh()); checkProjection(spherical(meshes), true);
}
TEST(Mp4StereoMetadataTest, CustomMeshRawDeflateIsDecoded) {
    Bytes meshes = projectionMesh(); append(meshes, projectionMesh()); checkProjection(spherical(meshes, true), true);
}
TEST(Mp4StereoMetadataTest, CustomMeshAllTriangleModesRemainTyped) {
    for (uint8_t mode : {0,1,2}) {
        Bytes meshes = projectionMesh(mode); append(meshes, projectionMesh(mode)); checkProjection(spherical(meshes), true);
    }
}
TEST(Mp4StereoMetadataTest, CustomMeshListPaddingIsRespected) {
    Bytes meshes = projectionMesh(0,false,true); append(meshes, projectionMesh(2,false,true));
    checkProjection(spherical(meshes), true);
}
TEST(Mp4StereoMetadataTest, CustomMeshPoseIsPreserved) {
    Bytes meshes = projectionMesh(); append(meshes, projectionMesh()); checkProjection(spherical(meshes,false,false,90*65536),true,2,90*65536);
}
TEST(Mp4StereoMetadataTest, CustomMeshBadCrcIsExplicit) {
    Bytes meshes = projectionMesh(); append(meshes, projectionMesh()); checkProjection(spherical(meshes,false,true),false);
}
TEST(Mp4StereoMetadataTest, CustomMeshOutOfRangeIndexIsRejected) {
    Bytes meshes = projectionMesh(0,true); append(meshes, projectionMesh()); checkProjection(spherical(meshes),false);
}
TEST(Mp4StereoMetadataTest, CustomMeshReservedTriangleModeIsRejected) {
    Bytes meshes = projectionMesh(3); append(meshes, projectionMesh()); checkProjection(spherical(meshes),false);
}
TEST(Mp4StereoMetadataTest, CustomMeshMissingHeaderIsNotDefaulted) {
    Bytes meshes = projectionMesh(); append(meshes, projectionMesh()); checkProjection(spherical(meshes,false,false,0,false),false);
}
TEST(Mp4StereoMetadataTest, CustomMeshThirdEyeIsRejected) {
    Bytes meshes = projectionMesh(); append(meshes, projectionMesh()); append(meshes, projectionMesh()); checkProjection(spherical(meshes),false);
}
TEST(Mp4StereoMetadataTest, CustomMeshSingleMeshRemainsOneMesh) { checkProjection(spherical(projectionMesh()),true,1); }
TEST(Mp4StereoMetadataTest, CustomMeshUnknownBoxesDoNotEraseKnownData) {
    Bytes meshes = box("futr", {1,2,3}); append(meshes,projectionMesh()); append(meshes,projectionMesh());
    checkProjection(spherical(meshes),true);
}
TEST(Mp4StereoMetadataTest, CustomMeshInvalidPoseIsRejected) {
    Bytes meshes = projectionMesh(); append(meshes, projectionMesh()); checkProjection(spherical(meshes,false,false,181*65536),false);
}
TEST(Mp4StereoMetadataTest, CustomMeshTruncationDoesNotBecomeMono) {
    Bytes meshes = projectionMesh(); append(meshes, projectionMesh()); Bytes body = spherical(meshes); body.pop_back(); checkProjection(body,false);
}

}  // namespace
}  // namespace android
