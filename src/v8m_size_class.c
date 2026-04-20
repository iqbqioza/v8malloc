/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Size-class reverse-lookup table. The class id returned by
 * v8m_size_class() indexes this array to recover the exact byte size
 * the class serves. Values are pinned by the design doc
 * (.claude/docs/size-classes.md §8.5) and cross-checked against the
 * branchless formula in test_size_class.
 */

#include <stdint.h>

#include "v8m_size_class.h"

const uint32_t v8m_class_to_size[V8M_NUM_SIZE_CLASSES] = {
    /* Tiny: classes 0..7, 8B spacing */
    8,
    16,
    24,
    32,
    40,
    48,
    56,
    64,
    /* Small low: classes 8..11, 16B spacing */
    80,
    96,
    112,
    128,
    /* Small mid-low: classes 12..15, 32B spacing */
    160,
    192,
    224,
    256,
    /* Small mid: classes 16..19, 64B spacing */
    320,
    384,
    448,
    512,
    /* Small mid-high: classes 20..23, 128B spacing */
    640,
    768,
    896,
    1024,
    /* Small high: classes 24..27, 256B spacing */
    1280,
    1536,
    1792,
    2048,
    /* Small max: classes 28..31, 512B spacing */
    2560,
    3072,
    3584,
    4096,
    /* Medium: classes 32..37, one class per power-of-two */
    8192,
    16384,
    32768,
    65536,
    131072,
    262144,
    /* Large: classes 38..40, extent-managed */
    524288,
    1048576,
    2097152,
};
