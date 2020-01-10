#include "rtpenc.h"
#include "libavcodec/av1.h"
#include "libavcodec/av1_parse.h"

static const size_t kMaximumLeb128Size = 8;
static const uint64_t kMaximumLeb128Value = 0xFFFFFFFFFFFFFF;  // 2 ^ 56 - 1
static uint8_t firstPacketReceived = 0;

#define AGGRE_HEADER_SIZE 1
#define AV1_RTP_FLAG_Z 0x80
#define AV1_RTP_FLAG_Y 0x40
#define AV1_RTP_FLAG_N 0x04
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
        memset(s->buf, 0, sizeof(uint8_t));
    }
    s->buf[0] &= flag;
    if (s->buffered_nals > 3) {
        s->buf[0] &= 0x30;
    }
}

static void flush_buffered(AVFormatContext *s1, int last)
{
    RTPMuxContext *s = s1->priv_data;
    if (s->buf_ptr != s->buf) {
        ff_rtp_send_data(s1, s->buf, s->buf_ptr - s->buf, last);
    }
    s->buf_ptr = s->buf;
    s->buffered_nals = 0;
}

static void obu_send(AVFormatContext *s1, const uint8_t *buf, int size, int last)
{
    RTPMuxContext *s = s1->priv_data;

    uint8_t *obu_ele_hdr;
    size_t obu_ele_siz;
    int header_size;
    obu_ele_hdr = (uint8_t *)malloc(sizeof(obu_ele_hdr) * 2);
    // if (size <= 0)
    //     return AVERROR_INVALIDDATA;
    eb_aom_uleb_encode(size, sizeof(size), obu_ele_hdr, &obu_ele_siz);


    av_log(s1, AV_LOG_DEBUG, "Sending OBU Type: %x of len %d M=%d\n", buf[0] & 0x7F, size, last);

    int buffered_size = (int)(s->buf_ptr - s->buf);
    header_size = buffered_size > 0 ? obu_ele_siz : (obu_ele_siz + AGGRE_HEADER_SIZE);

    // 直接可以放到前一个包里
    if (buffered_size + size + header_size <= s->max_payload_size) {
        if (buffered_size == 0) {
            s->buf_ptr++;
        }
        update_aggregate_hdr(s1, AV1_RTP_FLAG_NONE, 0);//?
        memcpy(s->buf_ptr, obu_ele_hdr, obu_ele_siz);
        memcpy(s->buf_ptr + obu_ele_siz, buf, size);
        s->buf_ptr += (size + obu_ele_siz);
        // s->buffered_nals++;
        // will flush in next obu_send or at end of code
    } else {
        av_log(s1, AV_LOG_DEBUG, "OBU size %d > %d\n", size, s->max_payload_size);
        // 可以先把前一个塞满
        if (buffered_size > 0) {
            int size_to_fill = s->max_payload_size - buffered_size - AGGRE_HEADER_SIZE;

            if (size_to_fill > 127) {
                size_to_fill -= 2;
            } else {
                size_to_fill--;
            }
            eb_aom_uleb_encode(size_to_fill, sizeof(size_to_fill), obu_ele_hdr, &obu_ele_siz);

            update_aggregate_hdr(s1, AV1_RTP_FLAG_Y, 0);

            memcpy(s->buf_ptr, obu_ele_hdr, obu_ele_siz);
            memcpy(s->buf_ptr + obu_ele_siz, buf, size_to_fill);
            buf  += size_to_fill;
            size -= size_to_fill;
            flush_buffered(s1, 0);
        }

        // 再处理剩下的部分
        while (size + AGGRE_HEADER_SIZE > s->max_payload_size) {
            update_aggregate_hdr(s1, AV1_RTP_FLAG_Z | AV1_RTP_FLAG_Y | AV1_RTP_FLAG_W1, 1);
            memcpy(&s->buf[AGGRE_HEADER_SIZE], buf, s->max_payload_size - AGGRE_HEADER_SIZE);
            ff_rtp_send_data(s1, s->buf, s->max_payload_size, 0);
            buf  += s->max_payload_size - AGGRE_HEADER_SIZE;
            size -= s->max_payload_size - AGGRE_HEADER_SIZE;
        }
        update_aggregate_hdr(s1, AV1_RTP_FLAG_Z, 1);
        eb_aom_uleb_encode(size, sizeof(size), obu_ele_hdr, &obu_ele_siz);
        header_size = obu_ele_siz + AGGRE_HEADER_SIZE;
        memcpy(s->buf + AGGRE_HEADER_SIZE, obu_ele_hdr, obu_ele_siz);
        memcpy(s->buf + header_size, buf, size);
        ff_rtp_send_data(s1, s->buf, size + header_size, last);
    }
    free(obu_ele_hdr);
}

void ff_rtp_send_av1(AVFormatContext *s1, const uint8_t *buf, int size)
{
    // const uint8_t *r, *end = buf + size;
    RTPMuxContext *s = s1->priv_data;

    s->timestamp = s->cur_timestamp;
    s->buf_ptr   = s->buf;

    int64_t obu_size;
    int start_pos, type, temporal_id, spatial_id;

    if (!firstPacketReceived) {
        update_aggregate_hdr(s1, AV1_RTP_FLAG_N, 1);
    }
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