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

#include "libavutil/intreadwrite.h"

#include "avio_internal.h"
#include "rtpdec_formats.h"

#define AGGRE_HEADER_SIZE 1
#define AV1_RTP_FLAG_Z 0x80
#define AV1_RTP_FLAG_Y 0x40
#define AV1_RTP_FLAG_N 0x04
#define AV1_RTP_FLAG_W1 0x10

struct PayloadContext {
    AVIOContext *buf;
    uint32_t     timestamp;
};

static av_cold int av1_init(AVFormatContext *ctx, int st_index,
                            PayloadContext *data)
{
    av_log(ctx, AV_LOG_WARNING,
           "RTP/AV1 support is still experimental\n");

    return 0;
}

static int av1_handle_packet(AVFormatContext *ctx, PayloadContext *rtp_av1_ctx,
                             AVStream *st, AVPacket *pkt, uint32_t *timestamp,
                             const uint8_t *buf, int len, uint16_t seq,
                             int flags)
{
    uint8_t frag_Z, frag_Y, obu_ele_num, first_packet;
    uint8_t first_fragment = 0, last_fragment = 0;
    int res = 0;
    /* drop data of previous packets in case of non-continuous (lossy) packet stream */
    if (rtp_av1_ctx->buf && rtp_av1_ctx->timestamp != *timestamp)
        ffio_free_dyn_buf(&rtp_av1_ctx->buf);

    /* sanity check for size of input packet: 1 byte payload at least */
    if (len < AGGRE_HEADER_SIZE + 1) {
        av_log(ctx, AV_LOG_ERROR, "Too short RTP/AV1 packet, got %d bytes\n", len);
        return AVERROR_INVALIDDATA;
    }

    /**
     *  type  Z | Y     meaning
     *   0    0 | 0     full packet
     *   1    0 | 1     the last pkt is a frag, if there is another pkts, they are completed
     *   2    1 | 0     the first pkt is a frag, if there is another pkts, they are completed
     *   3    1 | 1     the first and the last pkt are frags, if there is only one obu,
     *                  then the rtp_payload belongs to a large obu
     *
     *  0 1 2 3 4 5 6 7
     * +-+-+-+-+-+-+-+-+
     * |Z|Y| W |N|-|-|-|
     * +-+-+-+-+-+-+-+-+
     * Z: set to 1 if the first OBU element is an OBU fragment that is a continuation of
     *          an OBU fragment from the previous packet, 0 otherwise.
     * Y: set to 1 if the last OBU element is an OBU fragment that will continue in the
     *          next packet, 0 otherwise.
     * W: two bit field that describes the number of OBU elements in the packet.
     * N: set to 1 if the packet is the first packet of a coded video sequence, 0 otherwise.
     * Note: if N equals 1 then Z must equal 0.
    */

    frag_Z = !!(buf[0] & AV1_RTP_FLAG_Z);
    frag_Y = !!(buf[0] & AV1_RTP_FLAG_Y);
    obu_ele_num = (buf[0] & 0x30) >> 4;
    first_packet = !!(buf[0] & AV1_RTP_FLAG_N);
    first_fragment = !frag_Z && frag_Y;
    last_fragment = frag_Z && !frag_Y;

    av_log(ctx, AV_LOG_DEBUG, "pkt=%p, len=%d, AGGR=%d, first_frag=%d, last_frag=%d\n", pkt, len, buf[0], first_fragment, last_fragment);

    if (!frag_Z && !frag_Y && obu_ele_num == 0) {
        if ((res = av_new_packet(pkt, len - AGGRE_HEADER_SIZE)) < 0)
            return res;
        memcpy(pkt->data, buf + 1, len - AGGRE_HEADER_SIZE);
        pkt->stream_index = st->index;
        return res;
    }

    /* start frame buffering with new dynamic buffer */
    if (!rtp_av1_ctx->buf) {
        /* sanity check: a new frame should have started */
        if (first_fragment) {
            res = avio_open_dyn_buf(&rtp_av1_ctx->buf);
            if (res < 0)
                return res;
            /* update the timestamp in the frame packet with the one from the RTP packet */
            rtp_av1_ctx->timestamp = *timestamp;
        } else {
            /* frame not started yet, need more packets */
            return AVERROR(EAGAIN);
        }
    }

    /* write the fragment to the dyn. buffer */
    avio_write(rtp_av1_ctx->buf, buf + 1, len - AGGRE_HEADER_SIZE);

    /* do we need more fragments? */
    if (!last_fragment)
        return AVERROR(EAGAIN);

    /* close frame buffering and create resulting A/V packet */
    res = ff_rtp_finalize_packet(pkt, &rtp_av1_ctx->buf, st->index);
    if (res < 0)
        return res;

    return 0;
}

static void av1_close_context(PayloadContext *av1)
{
    ffio_free_dyn_buf(&av1->buf);
}
const RTPDynamicProtocolHandler ff_av1_dynamic_handler = {
    .enc_name         = "AV1",
    .codec_type       = AVMEDIA_TYPE_VIDEO,
    .codec_id         = AV_CODEC_ID_AV1,
    .priv_data_size   = sizeof(PayloadContext),
    .init             = av1_init,
    .close            = av1_close_context,
    .parse_packet     = av1_handle_packet,
};