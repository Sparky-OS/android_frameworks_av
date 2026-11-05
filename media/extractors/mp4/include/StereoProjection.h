/*
 * Copyright 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ANDROID_STEREO_PROJECTION_H_
#define ANDROID_STEREO_PROJECTION_H_
#include <stddef.h>
#include <stdint.h>
#include <vector>

namespace android::stereo {
// Spherical Video V2 sv3d body to a typed, container-independent mesh descriptor.
// The caller retains the original bytes separately, including unsupported fields.
bool decodeStereoProjection(const uint8_t *data, size_t size,
                            std::vector<uint8_t> *descriptor, const char **error);
}
#endif
