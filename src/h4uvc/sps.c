// SPDX-License-Identifier: GPL-2.0
/*
 * Low-delay SPS rewrite for the HERO4 preview's H.264.
 *
 * The camera's SPS says max_num_reorder_frames = 1 (VUI bitstream_restriction),
 * but the preview has only I and P slices. A decoder that trusts the SPS holds
 * every frame back by one (33 ms at 30 fps) in case it needs reordering. This
 * rewrites the SPS with max_num_reorder_frames = 0 and max_dec_frame_buffering =
 * max_num_ref_frames, everything else bit for bit.
 */
#include "sps.h"

#include <string.h>

struct bits {
	const uint8_t *p;
	size_t len, pos;	/* in bits */
	int err;
};

static unsigned get1(struct bits *b)
{
	if (b->pos >= b->len) {
		b->err = 1;
		return 0;
	}
	unsigned v = (b->p[b->pos >> 3] >> (7 - (b->pos & 7))) & 1;
	b->pos++;
	return v;
}

static unsigned getn(struct bits *b, int n)
{
	unsigned v = 0;
	while (n--)
		v = v << 1 | get1(b);
	return v;
}

static unsigned ue(struct bits *b)
{
	int zeros = 0;
	while (!get1(b) && !b->err && zeros < 32)
		zeros++;
	return ((1u << zeros) - 1) + getn(b, zeros);
}

struct out {
	uint8_t *p;
	size_t cap, pos;	/* in bits */
	int err;
};

static void put1(struct out *o, unsigned v)
{
	if (o->pos >= o->cap * 8) {
		o->err = 1;
		return;
	}
	if (v)
		o->p[o->pos >> 3] |= 0x80 >> (o->pos & 7);
	o->pos++;
}

static void putn(struct out *o, unsigned v, int n)
{
	while (n--)
		put1(o, (v >> n) & 1);
}

static void put_ue(struct out *o, unsigned v)
{
	unsigned x = v + 1;
	int n = 0;
	while (x >> n > 1)
		n++;
	putn(o, 0, n);
	putn(o, x, n + 1);
}

static void hrd(struct bits *b)
{
	unsigned cpb = ue(b);
	getn(b, 8);
	for (unsigned i = 0; i <= cpb && !b->err; i++) {
		ue(b);
		ue(b);
		get1(b);
	}
	getn(b, 20);
}

/* RBSP (no emulation prevention) of an SPS, NAL header byte included. Returns
 * the RBSP length of the rewrite, or 0 if there is nothing to change or the
 * SPS can't be parsed. */
static size_t rewrite_rbsp(const uint8_t *in, size_t len, uint8_t *out, size_t cap)
{
	struct bits b = {in, len * 8, 8, 0};
	unsigned profile = getn(&b, 8);
	getn(&b, 16);
	ue(&b);
	if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 ||
	    profile == 83 || profile == 86 || profile == 118 || profile == 128 || profile == 138 ||
	    profile == 139 || profile == 134 || profile == 135)
		return 0;	/* not the camera's Main profile: leave it alone */
	ue(&b);				/* log2_max_frame_num_minus4 */
	unsigned poc = ue(&b);
	if (poc == 0)
		ue(&b);
	else if (poc == 1)
		return 0;
	unsigned refs = ue(&b);
	get1(&b);
	ue(&b);
	ue(&b);
	if (!get1(&b))			/* frame_mbs_only_flag */
		get1(&b);
	get1(&b);
	if (get1(&b)) {			/* frame_cropping_flag */
		ue(&b);
		ue(&b);
		ue(&b);
		ue(&b);
	}
	if (!get1(&b))			/* vui_parameters_present_flag */
		return 0;
	if (get1(&b) && getn(&b, 8) == 255)
		getn(&b, 32);
	if (get1(&b))
		get1(&b);
	if (get1(&b)) {
		getn(&b, 4);
		if (get1(&b))
			getn(&b, 24);
	}
	if (get1(&b)) {
		ue(&b);
		ue(&b);
	}
	if (get1(&b)) {
		getn(&b, 32);
		getn(&b, 32);
		get1(&b);
	}
	int nal = get1(&b);
	if (nal)
		hrd(&b);
	int vcl = get1(&b);
	if (vcl)
		hrd(&b);
	if (nal || vcl)
		get1(&b);
	get1(&b);			/* pic_struct_present_flag */
	if (!get1(&b))			/* bitstream_restriction_flag */
		return 0;
	get1(&b);
	ue(&b);
	ue(&b);
	ue(&b);
	ue(&b);
	size_t at = b.pos;		/* max_num_reorder_frames starts here */
	unsigned reorder = ue(&b), dpb = ue(&b);
	if (b.err || (reorder == 0 && dpb == refs))
		return 0;

	/* Copy everything before, write the two new values, then the RBSP
	 * trailing bits (nothing follows the VUI in an SPS). */
	struct out o = {out, cap, 0, 0};
	memset(out, 0, cap);
	struct bits c = {in, len * 8, 0, 0};
	while (c.pos < at)
		put1(&o, get1(&c));
	put_ue(&o, 0);
	put_ue(&o, refs ? refs : 1);
	put1(&o, 1);
	while (o.pos & 7)
		put1(&o, 0);
	return o.err ? 0 : o.pos / 8;
}

size_t sps_low_delay(const uint8_t *nal, size_t len, uint8_t *out, size_t cap)
{
	uint8_t rbsp[256], fixed[256];
	size_t n = 0, zeros = 0;

	if (len < 4 || len > sizeof(rbsp) || (nal[0] & 0x1f) != 7)
		return 0;
	for (size_t i = 0; i < len; i++) {	/* drop emulation prevention bytes */
		if (zeros >= 2 && nal[i] == 3) {
			zeros = 0;
			continue;
		}
		zeros = nal[i] ? 0 : zeros + 1;
		rbsp[n++] = nal[i];
	}
	size_t m = rewrite_rbsp(rbsp, n, fixed, sizeof(fixed));
	if (!m)
		return 0;
	size_t o = 0;
	zeros = 0;
	for (size_t i = 0; i < m; i++) {	/* and put them back */
		if (zeros >= 2 && fixed[i] <= 3) {
			if (o >= cap)
				return 0;
			out[o++] = 3;
			zeros = 0;
		}
		if (o >= cap)
			return 0;
		out[o++] = fixed[i];
		zeros = fixed[i] ? 0 : zeros + 1;
	}
	return o;
}
