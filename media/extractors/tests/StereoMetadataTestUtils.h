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

#ifndef STEREO_METADATA_TEST_UTILS_H_
#define STEREO_METADATA_TEST_UTILS_H_

#include <gtest/gtest.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkMediaFormatPriv.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/AMessage.h>

namespace android {
// Utils.h also pulls in device-only audio Binder headers.
status_t convertMessageToMetaData(const sp<AMessage> &, sp<MetaData> &);
status_t convertMetaDataToMessage(const sp<MetaData> &, sp<AMessage> *);
// Exercise the C extractor wrapper and NuMediaExtractor format conversions.
inline void bridgeStereoFormat(AMediaFormat *format) {
    sp<AMessage> message, returned;
    AMediaFormat_getFormat(format, &message);
    sp<MetaData> metadata = new MetaData;
    ASSERT_EQ(OK, convertMessageToMetaData(message, metadata));
    ASSERT_EQ(OK, convertMetaDataToMessage(metadata, &returned));
    for (size_t i = 0; i < message->countEntries(); ++i) {
        AMessage::Type type;
        const char *name = message->getEntryNameAt(i, &type);
        if (strncmp(name, "stereo-", 7)) continue;
        SCOPED_TRACE(name);
        switch (type) {
            case AMessage::kTypeInt32: {
                int32_t before, after;
                ASSERT_TRUE(message->findInt32(name, &before));
                ASSERT_TRUE(returned->findInt32(name, &after));
                EXPECT_EQ(before, after);
                break;
            }
            case AMessage::kTypeInt64: {
                int64_t before, after;
                ASSERT_TRUE(message->findInt64(name, &before));
                ASSERT_TRUE(returned->findInt64(name, &after));
                EXPECT_EQ(before, after);
                break;
            }
            case AMessage::kTypeString: {
                AString before, after;
                ASSERT_TRUE(message->findString(name, &before));
                ASSERT_TRUE(returned->findString(name, &after));
                EXPECT_EQ(before, after);
                break;
            }
            case AMessage::kTypeBuffer: {
                sp<ABuffer> before, after;
                ASSERT_TRUE(message->findBuffer(name, &before));
                ASSERT_TRUE(returned->findBuffer(name, &after));
                ASSERT_EQ(before->size(), after->size());
                if (before->size()) EXPECT_EQ(0, memcmp(before->data(), after->data(), before->size()));
                break;
            }
            default: FAIL() << "Unhandled stereo descriptor type " << type;
        }
    }
    AMediaFormat *converted = AMediaFormat_fromMsg(&returned);
    ASSERT_EQ(AMEDIA_OK, AMediaFormat_copy(format, converted));
    AMediaFormat_delete(converted);
}
} // namespace android
#endif
