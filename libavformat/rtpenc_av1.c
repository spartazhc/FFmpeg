/*
 * RTP parser for AV1 payload format (draft) - experimental
 * Copyright (c) 2020 Zhong Hongcheng <hongcheng.zhong@intel.com>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "rtpenc.h"
#include "libavcodec/av1.h"
#include "libavcodec/av1_parse.h"

static const size_t kMaximumLeb128Size = 8;
static const uint64_t kMaximumLeb128Value = 0xFFFFFFFFFFFFFF;  // 2 ^ 56 - 1
static uint8_t firstPacketReceived = 0;

#define AGGRE_HEADER_SIZE 1
#define AV1_RTP_FLAG_Z 0x80
#define AV1_RTP_FLAG_Y 0x40
#define AV1_RTP_FLAG_N 0x08
#define AV1_RTP_FLAG_W1 0x10

#define AV1_RTP_FLAG_NONE 0

static size_t eb_aom_uleb_size_in_bytes(uint64_t value) {
    size_t size = 0;
    do {
        ++size;
    } while ((value >>= 7) != 0);
    return size;
}

static int eb_aom_uleb_encode(uint64_t value, size_t available, uint8_t *coded_value,
    size_t *coded_size) {
    const size_t leb_size = eb_aom_uleb_size_in_bytes(value);
    if (value > kMaximumLeb128Value || leb_size > kMaximumLeb128Size ||
        leb_size > available || !coded_value || !coded_size) {
        return -1;
    }

    for (size_t i = 0; i < leb_size; ++i) {
        uint8_t byte = value & 0x7f;
        value >>= 7;

        if (value != 0) byte |= 0x80;  // Signal that more bytes follow.

        *(coded_value + i) = byte;
    }

    *coded_size = leb_size;
    return 0;
}
/**
 *  0 1 2 3 4 5 6 7
 * +-+-+-+-+-+-+-+-+
 * |Z|Y| W |N|-|-|-|
 * +-+-+-+-+-+-+-+-+
 * Z: set to 1 if the first OBU element is an OBU fragment that is a continuation of an OBU fragment from the previous packet, 0 otherwise.
 * Y: set to 1 if the last OBU element is an OBU fragment that will continue in the next packet, 0 otherwise.
 * W: two bit field that describes the number of OBU elements in the packet.
 * N: set to 1 if the packet is the first packet of a coded video sequence, 0 otherwise. Note: if N equals 1 then Z must equal 0.
 */

static void update_aggregate_hdr(AVFormatContext *s1, uint8_t flag, uint8_t clear)
{
    RTPMuxContext *s = s1->priv_data;
    if (clear) {
        s->buf[0] = 0;
    }
    s->buf[0] |= flag;
}

static void flush_buffered(AVFormatContext *s1, int last)
{
    RTPMuxContext *s = s1->priv_data;
    if (s->buf_ptr != s->buf) {
        ff_rtp_send_data(s1, s->buf, s->buf_ptr - s->buf, last);
    }
    s->buf_ptr = s->buf;
}

/* simplified version: only split obu which can not put into one rtp_payload */
static void obu_send(AVFormatContext *s1, const uint8_t *buf, int size, int last)
{
    uint8_t first;
    size_t obu_ele_siz;
    // uint8_t *obu_ele_hdr, header_size;
    RTPMuxContext *s = s1->priv_data;
    // obu_ele_hdr = (uint8_t *)malloc(sizeof(obu_ele_hdr) * 2);
    if (size <= 0)
        return;
    // header_size = obu_ele_siz + AGGRE_HEADER_SIZE;

    av_log(s1, AV_LOG_DEBUG, "Sending OBU Type: %x of len %d M=%d\n", buf[0] & 0x7F, size, last);

    if (size + AGGRE_HEADER_SIZE <= s->max_payload_size) {
        update_aggregate_hdr(s1, AV1_RTP_FLAG_NONE, 1);
        if (!firstPacketReceived) {
            update_aggregate_hdr(s1, AV1_RTP_FLAG_N, 1);
            firstPacketReceived = 1;
        }
        memcpy(&s->buf[AGGRE_HEADER_SIZE], buf, size);
        ff_rtp_send_data(s1, s->buf, size + AGGRE_HEADER_SIZE, last);
    } else {
        av_log(s1, AV_LOG_DEBUG, "OBU size %d > %d\n", size, s->max_payload_size);
        first = 1;
        while (size + AGGRE_HEADER_SIZE > s->max_payload_size) {
            /* first rtp_payload should not have AV1_RTP_FLAG_Z */
            if (first == 1) {
                update_aggregate_hdr(s1, AV1_RTP_FLAG_Y | AV1_RTP_FLAG_W1, 1);
                first = 0;
            } else {
                update_aggregate_hdr(s1, AV1_RTP_FLAG_Z, 0);
            }
            memcpy(&s->buf[AGGRE_HEADER_SIZE], buf, s->max_payload_size - AGGRE_HEADER_SIZE);
            ff_rtp_send_data(s1, s->buf, s->max_payload_size, 0);
            buf  += s->max_payload_size - AGGRE_HEADER_SIZE;
            size -= s->max_payload_size - AGGRE_HEADER_SIZE;
        }
        update_aggregate_hdr(s1, AV1_RTP_FLAG_Z, 1);
        memcpy(&s->buf[AGGRE_HEADER_SIZE], buf, size);
        ff_rtp_send_data(s1, s->buf, size + AGGRE_HEADER_SIZE, last);
    }
    // free(obu_ele_hdr);
}

void ff_rtp_send_av1(AVFormatContext *s1, const uint8_t *buf, int size)
{
    RTPMuxContext *s = s1->priv_data;
    int64_t obu_size;
    int start_pos, type, temporal_id, spatial_id;


    s->timestamp = s->cur_timestamp;
    s->buf_ptr   = s->buf;

    while (size > 0) {
        int len = parse_obu_header(buf, size, &obu_size, &start_pos,
                                   &type, &temporal_id, &spatial_id);
        if (type == AV1_OBU_TEMPORAL_DELIMITER) {
            size -= len;
            buf  += len;
            len = parse_obu_header(buf, size, &obu_size, &start_pos,
                                   &type, &temporal_id, &spatial_id);
        }
        obu_send(s1, buf, len, size == len);
        size -= len;
        buf  += len;
    }
    flush_buffered(s1, 1);
}