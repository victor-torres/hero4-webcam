// SPDX-License-Identifier: GPL-2.0
#pragma once

#include <stddef.h>
#include <stdint.h>

/* Rewrite an H.264 SPS NAL unit (header byte first, no start code) so that the
 * decoder doesn't hold frames for reordering: max_num_reorder_frames = 0,
 * max_dec_frame_buffering = max_num_ref_frames. Returns the new NAL's length in
 * out, or 0 if it can't be parsed or needs no change. */
size_t sps_low_delay(const uint8_t *nal, size_t len, uint8_t *out, size_t cap);
