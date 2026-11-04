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
#include <media/stagefright/foundation/StereoFramePacking.h>
#include <vector>

namespace android {
namespace stereo {
namespace {
class Writer {
public:
    std::vector<uint8_t> bytes;
    size_t count = 0;
    void bits(uint32_t value, size_t width) {
        for (size_t i = width; i > 0; --i) {
            if ((count & 7) == 0) bytes.push_back(0);
            bytes.back() |= ((value >> (i - 1)) & 1) << (7 - (count++ & 7));
        }
    }
    void ue(uint32_t value) {
        uint64_t code = uint64_t(value) + 1;
        size_t width = 0;
        for (auto n = code; n; n >>= 1) ++width;
        bits(0, width - 1);
        bits(code, width);
    }
};
std::vector<uint8_t> payload(bool hevc, uint32_t type, uint32_t order = 1,
                             uint32_t id = 0, bool cancel = false,
                             uint32_t period = 1, bool extras = false) {
    Writer w;
    w.ue(id);
    w.bits(cancel, 1);
    if (!cancel) {
        w.bits(type, 7);
        w.bits(type == 0, 1);
        w.bits(order, 6);
        for (int i = 0; i < 6; ++i) w.bits(extras, 1);
        if (type != 0 && type != 5) {
            for (int i = 1; i <= 4; ++i) w.bits(extras ? i : 0, 4);
        }
        w.bits(0, 8);
        if (hevc) w.bits(period != 0, 1); else w.ue(period);
    }
    w.bits(extras, 1);
    if (w.count & 7) {
        w.bits(1, 1);
        while (w.count & 7) w.bits(0, 1);
    }
    return w.bytes;
}
ParseResult parse(const std::vector<uint8_t> &data, bool hevc, FramePacking *out) {
    return parseFramePacking(data.data(), data.size(), hevc, out);
}
class StereoFramePackingTest : public testing::TestWithParam<int> {};
TEST_P(StereoFramePackingTest, AvcCompleteVocabularyAndOrders) {
    constexpr int expected[] = {3, 4, 5, 1, 2, 6, 0, 7};
    for (int order = 0; order < 3; ++order) {
        FramePacking f;
        ASSERT_EQ(ParseResult::VALID, parse(payload(false, GetParam(), order), false, &f));
        EXPECT_EQ(expected[GetParam()], f.packing.layout);
        EXPECT_EQ(GetParam() == 6 ? 0 : order, f.packing.order);
    }
}
TEST_P(StereoFramePackingTest, HevcDoesNotImportAvcReservedTypes) {
    FramePacking f;
    bool assigned = GetParam() >= 3 && GetParam() <= 5;
    EXPECT_EQ(assigned ? ParseResult::VALID : ParseResult::IGNORED,
              parse(payload(true, GetParam(), 2), true, &f));
    if (assigned) EXPECT_EQ(RIGHT_FIRST, f.packing.order);
}
INSTANTIATE_TEST_SUITE_P(AllAvcTypes, StereoFramePackingTest, testing::Range(0, 8));
TEST(StereoFramePackingTest, CancellationBothSyntaxes) {
    for (bool hevc : {false, true}) {
        FramePacking f;
        ASSERT_EQ(ParseResult::VALID, parse(payload(hevc, 0, 0, 17, true), hevc, &f));
        EXPECT_TRUE(f.cancel);
        EXPECT_EQ(17u, f.id);
    }
}
TEST(StereoFramePackingTest, AllByteTruncationsLeaveDestinationUntouched) {
    for (bool hevc : {false, true}) {
        auto data = payload(hevc, 3, 1, 255, false, 16384, true);
        for (size_t size = 0; size < data.size(); ++size) {
            FramePacking f;
            f.id = 1234;
            EXPECT_EQ(ParseResult::MALFORMED, parseFramePacking(data.data(), size, hevc, &f));
            EXPECT_EQ(1234u, f.id);
        }
    }
}
TEST(StereoFramePackingTest, ReservedUsageAndInterpretationIgnored) {
    for (bool hevc : {false, true}) {
        for (uint32_t id : {256u, 511u, 0x80000000u}) {
            FramePacking f;
            f.id = 1234;
            EXPECT_EQ(ParseResult::IGNORED, parse(payload(hevc, 3, 1, id), hevc, &f));
            EXPECT_EQ(1234u, f.id);
        }
        FramePacking f;
        EXPECT_EQ(ParseResult::IGNORED, parse(payload(hevc, 3, 63), hevc, &f));
        EXPECT_EQ(ParseResult::IGNORED, parse(payload(hevc, 127), hevc, &f));
    }
}
TEST(StereoFramePackingTest, RepetitionIsNotAFrameCountExpiry) {
    for (uint32_t period : {0u, 1u, 2u, 16384u}) {
        FramePacking f;
        ASSERT_EQ(ParseResult::VALID, parse(payload(false, 3, 1, 0, false, period), false, &f));
        EXPECT_EQ(period, f.repetitionPeriod);
        EXPECT_EQ(period != 0, f.persistence != 0);
    }
    FramePacking f;
    EXPECT_EQ(ParseResult::MALFORMED, parse(payload(false, 3, 1, 0, false, 16385), false, &f));
}
TEST(StereoFramePackingTest, RetainsIndependentGeometryFields) {
    for (bool hevc : {false, true}) {
        FramePacking f;
        ASSERT_EQ(ParseResult::VALID, parse(payload(hevc, 3, 2, 512, false, 1, true), hevc, &f));
        EXPECT_EQ(512u, f.id);
        EXPECT_EQ(1u, f.spatialFlipping);
        EXPECT_EQ(1u, f.frame0Flipped);
        EXPECT_EQ(1u, f.fieldViews);
        EXPECT_EQ(1u, f.frame0SelfContained);
        EXPECT_EQ(1u, f.frame1SelfContained);
        for (int i = 0; i < 4; ++i) EXPECT_EQ(uint32_t(i + 1), f.grid[i]);
        EXPECT_EQ(hevc ? 1u : 0u, f.upsampledAspectRatio);
    }
}
TEST(StereoFramePackingTest, InvalidSamplingCombinationsAreMalformed) {
    auto checker = payload(false, 0);
    checker[1] &= ~0x40;
    FramePacking f;
    EXPECT_EQ(ParseResult::MALFORMED, parse(checker, false, &f));
    for (bool hevc : {false, true}) {
        auto temporal = payload(hevc, 5);
        temporal[1] |= 0x40;
        EXPECT_EQ(ParseResult::MALFORMED, parse(temporal, hevc, &f));
    }
}
TEST(StereoFramePackingTest, GolombOverflowIsMalformed) {
    std::vector<uint8_t> zeros(16, 0);
    FramePacking f;
    EXPECT_EQ(ParseResult::MALFORMED, parse(zeros, false, &f));
}
TEST(StereoFramePackingHistoryTest, DecodeOrderDoesNotChangeEarlierOutput) {
    FramePackingHistory history;
    FramePacking a, b;
    ASSERT_EQ(ParseResult::VALID, parse(payload(false, 3), false, &a));
    ASSERT_EQ(ParseResult::VALID, parse(payload(false, 4), false, &b));
    history.remember({1, 0}, 0, a);
    history.remember({1, 0}, 6, b);
    history.remember({1, 0}, 2, std::nullopt);
    history.remember({1, 0}, 4, std::nullopt);
    ASSERT_TRUE(history.resolve({1, 0}, 2));
    EXPECT_EQ(SIDE_BY_SIDE, history.resolve({1, 0}, 2)->packing.layout);
    EXPECT_EQ(SIDE_BY_SIDE, history.resolve({1, 0}, 4)->packing.layout);
    EXPECT_EQ(TOP_BOTTOM, history.resolve({1, 0}, 6)->packing.layout);
}
TEST(StereoFramePackingHistoryTest, SkippedCancellationAndDifferentIdReplace) {
    FramePackingHistory history;
    FramePacking a, cancel;
    ASSERT_EQ(ParseResult::VALID, parse(payload(false, 3), false, &a));
    ASSERT_EQ(ParseResult::VALID, parse(payload(false, 0, 0, 9, true), false, &cancel));
    history.remember({1, 0}, 0, a);
    history.remember({1, 0}, 4, std::nullopt);
    history.remember({1, 0}, 2, cancel); // Skipped: never resolved for display.
    EXPECT_FALSE(history.resolve({1, 0}, 4));
    EXPECT_TRUE(history.resolve({1, 0}, 0));
}
TEST(StereoFramePackingHistoryTest, NonPersistentDoesNotReviveOlderArrangement) {
    FramePackingHistory history;
    FramePacking a, b;
    ASSERT_EQ(ParseResult::VALID, parse(payload(true, 3), true, &a));
    ASSERT_EQ(ParseResult::VALID, parse(payload(true, 4, 1, 2, false, 0), true, &b));
    history.remember({1, 0}, 0, a);
    history.remember({1, 0}, 1, b);
    history.remember({1, 0}, 2, std::nullopt);
    EXPECT_EQ(TOP_BOTTOM, history.resolve({1, 0}, 1)->packing.layout);
    EXPECT_FALSE(history.resolve({1, 0}, 2));
}
TEST(StereoFramePackingHistoryTest, NonOutputHistoryIsNotCappedAt64) {
    FramePackingHistory history;
    FramePacking a;
    ASSERT_EQ(ParseResult::VALID, parse(payload(true, 3), true, &a));
    history.remember({1, 0}, 0, a);
    for (int poc = 100; poc >= 1; --poc) history.remember({1, 0}, poc, std::nullopt);
    ASSERT_TRUE(history.resolve({1, 0}, 17));
    EXPECT_EQ(SIDE_BY_SIDE, history.resolve({1, 0}, 17)->packing.layout);
}
TEST(StereoFramePackingHistoryTest, RetirementPreservesPhaseAndFutureChanges) {
    FramePackingHistory history;
    FramePacking a, b;
    ASSERT_EQ(ParseResult::VALID, parse(payload(false, 5, 1, 0, false, 1, true), false, &a));
    ASSERT_EQ(ParseResult::VALID, parse(payload(false, 4), false, &b));
    history.remember({1, 0}, 0, a);
    history.remember({1, 0}, 2, std::nullopt);
    history.remember({1, 0}, 4, std::nullopt);
    history.remember({1, 0}, 6, b);
    history.retireThrough({1, 0}, 2);
    EXPECT_EQ(1u, history.resolve({1, 0}, 4)->currentFrameIsFrame0);
    EXPECT_EQ(TOP_BOTTOM, history.resolve({1, 0}, 6)->packing.layout);
}
TEST(StereoFramePackingHistoryTest, TemporalPhaseCountsSkippedPicturesAndScopes) {
    FramePackingHistory history;
    FramePacking a;
    ASSERT_EQ(ParseResult::VALID, parse(payload(false, 5, 1, 0, false, 1, true), false, &a));
    history.remember({1, 0}, 0, a);
    history.remember({1, 0}, 4, std::nullopt);
    history.remember({1, 0}, 2, std::nullopt); // Skipped, still the other constituent.
    EXPECT_EQ(1u, history.resolve({1, 0}, 4)->currentFrameIsFrame0);
    EXPECT_FALSE(history.resolve({2, 0}, 4));
    EXPECT_FALSE(history.resolve({1, 1}, 4));
    history.reset();
    EXPECT_FALSE(history.resolve({1, 0}, 4));
}
}  // namespace
}  // namespace stereo
}  // namespace android
