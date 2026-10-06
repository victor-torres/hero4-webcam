// SPDX-License-Identifier: GPL-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Minimal MPEG-TS demuxer: finds the H.264 and AAC streams through PAT/PMT and
 * hands out one PES payload at a time (an Annex B access unit, or an ADTS
 * frame). Other streams (GoPro data) are ignored. */
enum ts_stream { TS_VIDEO, TS_AUDIO, TS_STREAMS };

/* damaged: packets of this stream were lost since its previous PES was handed
 * out, so this one or the one before it is incomplete. pts is in 90 kHz units,
 * -1 if the PES had none. */
typedef void (*ts_pes_cb)(void *opaque, enum ts_stream st, const uint8_t *es, size_t len, uint64_t first_packet_ns,
			  int64_t pts, bool damaged);

struct ts_pes {
	int pid;
	uint8_t *buf;
	size_t len, cap;
	bool in_pes;
	int cc;          /* last continuity counter, -1 if none yet */
	bool damaged;    /* a packet went missing; reported with the next PES */
	uint64_t lost;   /* continuity errors */
	int last_cc_gap; /* packets missing at the last continuity error (mod 16) */
	uint64_t start_ns;
	int64_t pts;
};

struct ts_demux {
	int pmt_pid;
	struct ts_pes s[TS_STREAMS];
	ts_pes_cb cb;
	void *opaque;
};

void ts_demux_init(struct ts_demux *d, ts_pes_cb cb, void *opaque);
void ts_demux_free(struct ts_demux *d);
void ts_demux_reset(struct ts_demux *d);

/* Feed one 188-byte packet received at now_ns. Returns the stream it belongs
 * to, or -1 for anything else. */
int ts_demux_packet(struct ts_demux *d, const uint8_t *pkt, uint64_t now_ns);

/* Emit the PES of one stream collected so far. The receiver calls this on
 * gpStream's end-of-frame flag instead of waiting for the next PES start. */
void ts_demux_flush(struct ts_demux *d, enum ts_stream st);
