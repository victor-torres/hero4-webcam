// SPDX-License-Identifier: GPL-2.0
#include "ts-demux.h"

#include <stdlib.h>
#include <string.h>

#define STREAM_TYPE_H264 0x1b
#define STREAM_TYPE_AAC_ADTS 0x0f

void ts_demux_init(struct ts_demux *d, ts_pes_cb cb, void *opaque)
{
	memset(d, 0, sizeof(*d));
	d->cb = cb;
	d->opaque = opaque;
	ts_demux_reset(d);
}

void ts_demux_free(struct ts_demux *d)
{
	for (int i = 0; i < TS_STREAMS; i++) {
		free(d->s[i].buf);
		d->s[i].buf = NULL;
		d->s[i].cap = 0;
	}
}

void ts_demux_reset(struct ts_demux *d)
{
	d->pmt_pid = -1;
	for (int i = 0; i < TS_STREAMS; i++) {
		struct ts_pes *s = &d->s[i];
		s->pid = -1;
		s->len = 0;
		s->in_pes = false;
		s->cc = -1;
		s->damaged = false;
	}
}

static void append(struct ts_pes *s, const uint8_t *p, size_t n)
{
	if (s->len + n > s->cap) {
		size_t cap = s->cap ? s->cap * 2 : 64 * 1024;
		while (cap < s->len + n)
			cap *= 2;
		uint8_t *buf = realloc(s->buf, cap);
		if (!buf)
			return;
		s->buf = buf;
		s->cap = cap;
	}
	memcpy(s->buf + s->len, p, n);
	s->len += n;
}

void ts_demux_flush(struct ts_demux *d, enum ts_stream st)
{
	struct ts_pes *s = &d->s[st];
	if (s->in_pes && s->len) {
		d->cb(d->opaque, st, s->buf, s->len, s->start_ns, s->pts, s->damaged);
		s->damaged = false;
	}
	s->in_pes = false;
	s->len = 0;
}

/* PSI section starting at the pointer field; returns NULL if it doesn't fit. */
static const uint8_t *section(const uint8_t *p, size_t n, size_t *len)
{
	if (n < 1 || 1u + p[0] + 3 > n)
		return NULL;
	const uint8_t *s = p + 1 + p[0];
	size_t sl = ((s[1] & 0x0f) << 8 | s[2]) + 3;
	if (s + sl > p + n || sl < 12)
		return NULL;
	*len = sl;
	return s;
}

static void parse_pat(struct ts_demux *d, const uint8_t *p, size_t n)
{
	size_t sl;
	const uint8_t *s = section(p, n, &sl);
	if (!s || s[0] != 0x00)
		return;
	for (size_t i = 8; i + 4 <= sl - 4; i += 4) {
		int program = s[i] << 8 | s[i + 1];
		if (program) {
			d->pmt_pid = (s[i + 2] & 0x1f) << 8 | s[i + 3];
			return;
		}
	}
}

static void set_pid(struct ts_pes *s, int pid)
{
	if (pid != s->pid) {
		s->len = 0;
		s->in_pes = false;
		s->cc = -1;
	}
	s->pid = pid;
}

static void parse_pmt(struct ts_demux *d, const uint8_t *p, size_t n)
{
	size_t sl;
	const uint8_t *s = section(p, n, &sl);
	if (!s || s[0] != 0x02)
		return;
	size_t i = 12 + ((s[10] & 0x0f) << 8 | s[11]);
	while (i + 5 <= sl - 4) {
		int type = s[i], pid = (s[i + 1] & 0x1f) << 8 | s[i + 2];
		if (type == STREAM_TYPE_H264)
			set_pid(&d->s[TS_VIDEO], pid);
		else if (type == STREAM_TYPE_AAC_ADTS)
			set_pid(&d->s[TS_AUDIO], pid);
		i += 5 + ((s[i + 3] & 0x0f) << 8 | s[i + 4]);
	}
}

static int64_t read_pts(const uint8_t *p)
{
	return (int64_t)(p[0] & 0x0e) << 29 | p[1] << 22 | (p[2] & 0xfe) << 14 | p[3] << 7 | p[4] >> 1;
}

int ts_demux_packet(struct ts_demux *d, const uint8_t *pkt, uint64_t now_ns)
{
	if (pkt[0] != 0x47)
		return -1;
	bool pusi = pkt[1] & 0x40;
	int pid = (pkt[1] & 0x1f) << 8 | pkt[2];
	int afc = (pkt[3] >> 4) & 3;
	size_t off = 4;
	if (afc & 2)
		off += 1 + pkt[4];
	if (!(afc & 1) || off >= 188)
		return -1;
	const uint8_t *p = pkt + off;
	size_t n = 188 - off;

	if (pid == 0 && pusi) {
		parse_pat(d, p, n);
		return -1;
	}
	if (pid == d->pmt_pid && pusi) {
		parse_pmt(d, p, n);
		return -1;
	}

	int st = pid == d->s[TS_VIDEO].pid ? TS_VIDEO : pid == d->s[TS_AUDIO].pid ? TS_AUDIO : -1;
	if (st < 0)
		return -1;
	struct ts_pes *s = &d->s[st];

	/* The counter advances on every packet with payload; a jump means lost packets. */
	int cc = pkt[3] & 0x0f;
	if (s->cc >= 0 && cc != ((s->cc + 1) & 0x0f)) {
		s->damaged = true;
		s->lost++;
		s->last_cc_gap = (cc - s->cc - 1) & 0x0f;
	}
	s->cc = cc;

	if (pusi) {
		ts_demux_flush(d, st);
		/* PES header: 00 00 01, stream id, length, 2 flag bytes, header length. */
		if (n < 9 || p[0] || p[1] || p[2] != 1 || 9u + p[8] > n)
			return st;
		s->in_pes = true;
		s->start_ns = now_ns;
		s->pts = (p[7] & 0x80) && p[8] >= 5 ? read_pts(p + 9) : -1;
		append(s, p + 9 + p[8], n - 9 - p[8]);
	} else if (s->in_pes) {
		append(s, p, n);
	}
	return st;
}
