/*
* Scalable Video Technology for AV1 encoder library plugin
*
* Copyright (c) 2018 Intel Corporation
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
* License along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/

#include <stdint.h>
#include "EbSvtAv1ErrorCodes.h"
#include "EbSvtAv1Enc.h"

#include "libavutil/common.h"
#include "libavutil/frame.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/avassert.h"

#include "internal.h"
#include "avcodec.h"

typedef enum eos_status {
    EOS_NOT_REACHED = 0,
    EOS_SENT,
    EOS_RECEIVED
}EOS_STATUS;

typedef struct SvtContext {
    AVClass     *class;

    EbSvtAv1EncConfiguration    enc_params;
    EbComponentType            *svt_handle;

    EbBufferHeaderType         *in_buf;
    int                         raw_size;

    AVBufferPool* pool;

    EOS_STATUS eos_flag;

    // User options.
    int hierarchical_level;
    int la_depth;
    int enc_mode;
    int rc_mode;
    int scd;
    int qp;

    int forced_idr;

    int tier;
    int level;
    int profile;

    int base_layer_switch_mode;
} SvtContext;

static const struct {
    EbErrorType    eb_err;
    int            av_err;
    const char     *desc;
} svt_errors[] = {
    { EB_ErrorNone,                             0,              "success"                   },
    { EB_ErrorInsufficientResources,      AVERROR(ENOMEM),      "insufficient resources"    },
    { EB_ErrorUndefined,                  AVERROR(EINVAL),      "undefined error"           },
    { EB_ErrorInvalidComponent,           AVERROR(EINVAL),      "invalid component"         },
    { EB_ErrorBadParameter,               AVERROR(EINVAL),      "bad parameter"             },
    { EB_ErrorDestroyThreadFailed,        AVERROR_EXTERNAL,     "failed to destory thread"  },
    { EB_ErrorSemaphoreUnresponsive,      AVERROR_EXTERNAL,     "semaphore unresponsive"    },
    { EB_ErrorDestroySemaphoreFailed,     AVERROR_EXTERNAL,     "semaphore unresponsive"    },
    { EB_ErrorCreateMutexFailed,          AVERROR_EXTERNAL,     "failed to creat mutex"     },
    { EB_ErrorMutexUnresponsive,          AVERROR_EXTERNAL,     "mutex unresponsive"        },
    { EB_ErrorDestroyMutexFailed,         AVERROR_EXTERNAL,     "failed to destory muxtex"  },
    { EB_NoErrorEmptyQueue,               AVERROR(EAGAIN),      "empty queue"               },
};

static int svt_map_error(EbErrorType eb_err, const char **desc)
{
    int i;

    av_assert0(desc);
    for (i = 0; i < FF_ARRAY_ELEMS(svt_errors); i++) {
        if (svt_errors[i].eb_err == eb_err) {
            *desc = svt_errors[i].desc;
            return svt_errors[i].av_err;
        }
    }
    *desc = "unknown error";
    return AVERROR_UNKNOWN;
}

static int svt_print_error(void *log_ctx, EbErrorType err,
                           const char *error_string)
{
    const char *desc;
    int ret;
    ret = svt_map_error(err, &desc);
    av_log(log_ctx, AV_LOG_ERROR, "%s: %s (0x%x)\n", error_string, desc, err);
    return ret;
}

static void free_buffer(SvtContext *svt_enc)
{
    if (svt_enc->in_buf) {
        EbSvtIOFormat *in_data = (EbSvtIOFormat *)svt_enc->in_buf->p_buffer;
        av_freep(&in_data);
        av_freep(&svt_enc->in_buf);
    }
    av_buffer_pool_uninit(&svt_enc->pool);
}

static int alloc_buffer(EbSvtAv1EncConfiguration *config, SvtContext *svt_enc)
{
    const int    pack_mode_10bit   =
        (config->encoder_bit_depth > 8) && (config->compressed_ten_bit_format == 0) ? 1 : 0;
    const size_t luma_size_8bit    =
        config->source_width * config->source_height * (1 << pack_mode_10bit);
    const size_t luma_size_10bit   =
        (config->encoder_bit_depth > 8 && pack_mode_10bit == 0) ? luma_size_8bit : 0;

    EbSvtIOFormat *in_data;

    svt_enc->raw_size = (luma_size_8bit + luma_size_10bit) * 3 / 2;

    // allocate buffer for in and out
    svt_enc->in_buf           = av_mallocz(sizeof(*svt_enc->in_buf));
    if (!svt_enc->in_buf)
        return AVERROR(ENOMEM);

    svt_enc->in_buf->p_buffer  = (unsigned char *)av_mallocz(sizeof(*in_data));
    if (!svt_enc->in_buf->p_buffer)
        return AVERROR(ENOMEM);

    svt_enc->in_buf->size        = sizeof(*svt_enc->in_buf);
    svt_enc->in_buf->p_app_private  = NULL;

    svt_enc->pool = av_buffer_pool_init(svt_enc->raw_size, NULL);
    if (!svt_enc->pool) {
        return AVERROR(ENOMEM);
    }

    return 0;

}

static int config_enc_params(EbSvtAv1EncConfiguration *param,
                             AVCodecContext *avctx)
{
    SvtContext *svt_enc = avctx->priv_data;
    const AVPixFmtDescriptor *desc;
    int ret;

    param->source_width     = avctx->width;
    param->source_height    = avctx->height;

    desc = av_pix_fmt_desc_get(avctx->pix_fmt);
    param->encoder_bit_depth = desc->comp[0].depth;
    av_log(avctx, AV_LOG_DEBUG , "Encoder %d bits depth input\n", param->encoder_bit_depth);

    if (desc->log2_chroma_w == 1 && desc->log2_chroma_h == 1)
        param->encoder_color_format   = EB_YUV420;
    else if (desc->log2_chroma_w == 1 && desc->log2_chroma_h == 0)
        param->encoder_color_format   = EB_YUV422;
    else if (!desc->log2_chroma_w && !desc->log2_chroma_h)
        param->encoder_color_format   = EB_YUV444;
    else {
        av_log(avctx, AV_LOG_ERROR , "Unsupported pixel format\n");
        return AVERROR(EINVAL);
    }
    av_log(avctx, AV_LOG_DEBUG , "Encoder color format is %d \n", param->encoder_color_format);

    param->profile = svt_enc->profile;

    if ((param->encoder_color_format == EB_YUV422 || param->encoder_bit_depth > 10)
         && param->profile != PROFESSIONAL_PROFILE ) {
        av_log(avctx, AV_LOG_WARNING, "Force to be professional profile \n");
        param->profile = PROFESSIONAL_PROFILE;
    } else if (param->encoder_color_format == EB_YUV444 && param->profile != HIGH_PROFILE) {
        av_log(avctx, AV_LOG_WARNING, "Force to be high profile \n");
        param->profile = HIGH_PROFILE;
    }

    // Update param from options
    param->hierarchical_levels     = svt_enc->hierarchical_level;
    param->enc_mode                = svt_enc->enc_mode;
    param->tier                   = svt_enc->tier;
    param->level                  = svt_enc->level;
    param->rate_control_mode        = svt_enc->rc_mode;
    param->scene_change_detection   = svt_enc->scd;
    param->base_layer_switch_mode    = svt_enc->base_layer_switch_mode;
    param->qp                     = svt_enc->qp;


    param->target_bit_rate          = avctx->bit_rate;
    if (avctx->gop_size > 0)
        param->intra_period_length  = avctx->gop_size - 1;

    if (avctx->framerate.num > 0 && avctx->framerate.den > 0) {
        param->frame_rate_numerator     = avctx->framerate.num;
        param->frame_rate_denominator   = avctx->framerate.den * avctx->ticks_per_frame;
    } else {
        param->frame_rate_numerator     = avctx->time_base.den;
        param->frame_rate_denominator   = avctx->time_base.num * avctx->ticks_per_frame;
    }

    if (param->rate_control_mode) {
        param->max_qp_allowed       = avctx->qmax;
        param->min_qp_allowed       = avctx->qmin;
    }

    param->intra_refresh_type       = svt_enc->forced_idr + 1;

    if (svt_enc->la_depth != -1)
        param->look_ahead_distance  = svt_enc->la_depth;

    return 0;
}

static void read_in_data(const AVFrame *frame,
                         EbBufferHeaderType *header_ptr)
{
    EbSvtIOFormat *in_data = (EbSvtIOFormat *)header_ptr->p_buffer;
    const AVPixFmtDescriptor *desc;
    int i, bytes_shift, plane_h;

    desc = av_pix_fmt_desc_get(frame->format);
    bytes_shift = desc->comp[0].depth > 8 ? 1 : 0;

    in_data->luma = frame->data[0];
    in_data->cb   = frame->data[1];
    in_data->cr   = frame->data[2];

    in_data->y_stride  = AV_CEIL_RSHIFT(frame->linesize[0], bytes_shift);
    in_data->cb_stride = AV_CEIL_RSHIFT(frame->linesize[1], bytes_shift);
    in_data->cr_stride = AV_CEIL_RSHIFT(frame->linesize[2], bytes_shift);

    for (i = 0; i < desc->nb_components; i++) {
        plane_h = frame->height;
        if (i > 0)
            plane_h = AV_CEIL_RSHIFT(plane_h, desc->log2_chroma_h);
        header_ptr->n_filled_len += frame->linesize[i] * plane_h;
    }
}

static av_cold int eb_enc_init(AVCodecContext *avctx)
{
    SvtContext   *svt_enc = avctx->priv_data;
    EbErrorType svt_ret;
    int ret;

    svt_enc->eos_flag = EOS_NOT_REACHED;

    svt_ret = eb_init_handle(&svt_enc->svt_handle, svt_enc, &svt_enc->enc_params);
    if (svt_ret != EB_ErrorNone) {
        return svt_print_error(avctx, svt_ret, "Error init encoder handle");
    }

    ret = config_enc_params(&svt_enc->enc_params, avctx);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Error configure encoder parameters\n");
        return ret;
    }

    svt_ret = eb_svt_enc_set_parameter(svt_enc->svt_handle, &svt_enc->enc_params);
    if (svt_ret != EB_ErrorNone) {
        return svt_print_error(avctx, svt_ret, "Error setting encoder parameters");
    }

    svt_ret = eb_init_encoder(svt_enc->svt_handle);
    if (svt_ret != EB_ErrorNone) {
        eb_deinit_handle(svt_enc->svt_handle);
        svt_enc->svt_handle = NULL;
        return svt_print_error(avctx, svt_ret, "Error init encoder");
    }

    if (avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER) {
        EbBufferHeaderType *headerPtr = NULL;

        svt_ret = eb_svt_enc_stream_header(svt_enc->svt_handle, &headerPtr);
        if (svt_ret != EB_ErrorNone) {
            return svt_print_error(avctx, svt_ret, "Error when build stream header");
        }

        avctx->extradata_size = headerPtr->n_filled_len;
        avctx->extradata = av_mallocz(avctx->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!avctx->extradata) {
            av_log(avctx, AV_LOG_ERROR,
                   "Cannot allocate AV1 header of size %d.\n", avctx->extradata_size);
            return AVERROR(ENOMEM);
        }

        memcpy(avctx->extradata, headerPtr->p_buffer, avctx->extradata_size);

        svt_ret = eb_svt_release_enc_stream_header(headerPtr);
        if (svt_ret != EB_ErrorNone) {
            return svt_print_error(avctx, svt_ret, "Error when destroy stream header");
        }
    }

    ret = alloc_buffer(&svt_enc->enc_params, svt_enc);
    if (ret < 0)
        return ret;
    return 0;

}

static int eb_send_frame(AVCodecContext *avctx, const AVFrame *frame)
{
    SvtContext           *svt_enc = avctx->priv_data;
    EbBufferHeaderType  *headerPtr = svt_enc->in_buf;

    if (!frame) {
        EbBufferHeaderType headerPtrLast;
        headerPtrLast.n_alloc_len   = 0;
        headerPtrLast.n_filled_len  = 0;
        headerPtrLast.n_tick_count  = 0;
        headerPtrLast.p_app_private = NULL;
        headerPtrLast.p_buffer     = NULL;
        headerPtrLast.flags      = EB_BUFFERFLAG_EOS;

        eb_svt_enc_send_picture(svt_enc->svt_handle, &headerPtrLast);
        svt_enc->eos_flag = EOS_SENT;
        av_log(avctx, AV_LOG_DEBUG, "Finish sending frames!!!\n");
        return 0;
    }

    read_in_data(frame, headerPtr);

    headerPtr->flags       = 0;
    headerPtr->p_app_private  = NULL;
    headerPtr->pts          = frame->pts;

    eb_svt_enc_send_picture(svt_enc->svt_handle, headerPtr);

    return 0;
}

static int eb_receive_packet(AVCodecContext *avctx, AVPacket *pkt)
{
    SvtContext  *svt_enc = avctx->priv_data;
    EbBufferHeaderType *headerPtr;
    EbErrorType svt_ret;
    int ret = 0, pict_type;
    AVBufferRef* ref;

    if (svt_enc->eos_flag == EOS_RECEIVED)
        return AVERROR_EOF;

    svt_ret = eb_svt_get_packet(svt_enc->svt_handle, &headerPtr, svt_enc->eos_flag);
    if (svt_ret == EB_NoErrorEmptyQueue)
        return AVERROR(EAGAIN);

    ref = av_buffer_pool_get(svt_enc->pool);
    if (!ref) {
        av_log(avctx, AV_LOG_ERROR, "Failed to allocate output packet.\n");
        eb_svt_release_out_buffer(&headerPtr);
        return AVERROR(ENOMEM);
    }
    pkt->buf = ref;
    pkt->data = ref->data;

    memcpy(pkt->data, headerPtr->p_buffer, headerPtr->n_filled_len);
    pkt->size = headerPtr->n_filled_len;
    pkt->pts  = headerPtr->pts;
    pkt->dts  = headerPtr->dts;
    if (headerPtr->pic_type == EB_AV1_KEY_PICTURE) {
        pkt->flags |= AV_PKT_FLAG_KEY;
        pict_type = AV_PICTURE_TYPE_I;
    } else if (headerPtr->pic_type == EB_AV1_INTRA_ONLY_PICTURE) {
        pict_type = AV_PICTURE_TYPE_I;
    } else if (headerPtr->pic_type == EB_AV1_INVALID_PICTURE) {
        pict_type = AV_PICTURE_TYPE_NONE;
    } else
        pict_type = AV_PICTURE_TYPE_P;

    if (headerPtr->pic_type == EB_AV1_NON_REF_PICTURE)
        pkt->flags |= AV_PKT_FLAG_DISPOSABLE;

    if (headerPtr->flags & EB_BUFFERFLAG_EOS)
        svt_enc->eos_flag = EOS_RECEIVED;

    ff_side_data_set_encoder_stats(pkt, headerPtr->qp * FF_QP2LAMBDA, NULL, 0, pict_type);

    eb_svt_release_out_buffer(&headerPtr);

    return ret;
}

static av_cold int eb_enc_close(AVCodecContext *avctx)
{
    SvtContext *svt_enc = avctx->priv_data;

    if (svt_enc) {
        if (svt_enc->svt_handle) {
            eb_deinit_encoder(svt_enc->svt_handle);
            eb_deinit_handle(svt_enc->svt_handle);
        }

        free_buffer(svt_enc);
        svt_enc = NULL;
    }
    return 0;
}

#define OFFSET(x) offsetof(SvtContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "hielevel", "Hierarchical prediction levels setting", OFFSET(hierarchical_level),
      AV_OPT_TYPE_INT, { .i64 = 4 }, 3, 4, VE , "hielevel"},
        { "3level", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 3 },  INT_MIN, INT_MAX, VE, "hielevel" },
        { "4level", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 4 },  INT_MIN, INT_MAX, VE, "hielevel" },

    { "la_depth", "Look ahead distance [0, 120]", OFFSET(la_depth),
      AV_OPT_TYPE_INT, { .i64 = -1 }, -1, 120, VE },

    { "preset", "Encoding preset [0, 8]",
      OFFSET(enc_mode), AV_OPT_TYPE_INT, { .i64 = MAX_ENC_PRESET }, 0, MAX_ENC_PRESET, VE },

    { "profile", "Set profile restrictions", OFFSET(profile), AV_OPT_TYPE_INT, { .i64 = MAIN_PROFILE}, MAIN_PROFILE, PROFESSIONAL_PROFILE, VE, "profile" },
    { "main" , NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MAIN_PROFILE}, INT_MIN, INT_MAX,     VE, "profile" },
    { "high" , NULL, 0, AV_OPT_TYPE_CONST, { .i64 = HIGH_PROFILE}, INT_MIN, INT_MAX,     VE, "profile" },
    { "professional", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = PROFESSIONAL_PROFILE }, INT_MIN, INT_MAX,     VE, "profile" },

    { "tier", "Set tier (general_tier_flag)", OFFSET(tier),
      AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, VE, "tier" },
        { "main", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 0 }, 0, 0, VE, "tier" },
        { "high", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 1 }, 0, 0, VE, "tier" },

    { "level", "Set level (level_idc)", OFFSET(level),
      AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 0x1f, VE, "level" },

#define LEVEL(name, value) name, NULL, 0, AV_OPT_TYPE_CONST, \
      { .i64 = value }, 0, 0, VE, "level"
        { LEVEL("2.0", 20) },
        { LEVEL("2.1", 21) },
        { LEVEL("2.2", 22) },
        { LEVEL("2.3", 23) },
        { LEVEL("3.0", 30) },
        { LEVEL("3.1", 31) },
        { LEVEL("3.2", 32) },
        { LEVEL("3.3", 33) },
        { LEVEL("4.0", 40) },
        { LEVEL("4.1", 41) },
        { LEVEL("4.2", 42) },
        { LEVEL("4.3", 43) },
        { LEVEL("5.0", 50) },
        { LEVEL("5.1", 51) },
        { LEVEL("5.2", 52) },
        { LEVEL("5.3", 53) },
        { LEVEL("6.0", 60) },
        { LEVEL("6.1", 61) },
        { LEVEL("6.2", 62) },
        { LEVEL("6.3", 63) },
        { LEVEL("7.0", 70) },
        { LEVEL("7.1", 71) },
        { LEVEL("7.2", 72) },
        { LEVEL("7.3", 73) },
#undef LEVEL

    { "rc", "Bit rate control mode", OFFSET(rc_mode),
      AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 3, VE , "rc"},
        { "cqp", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 0 },  INT_MIN, INT_MAX, VE, "rc" },
        { "vbr", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 2 },  INT_MIN, INT_MAX, VE, "rc" },
        { "cvbr", NULL, 0, AV_OPT_TYPE_CONST,{ .i64 = 3 },  INT_MIN, INT_MAX, VE, "rc" },

    { "qp", "QP value for intra frames", OFFSET(qp),
      AV_OPT_TYPE_INT, { .i64 = 50 }, 0, 63, VE },

    { "sc_detection", "Scene change detection", OFFSET(scd),
      AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, VE },

    { "bl_mode", "Random Access Prediction Structure type setting", OFFSET(base_layer_switch_mode),
      AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, VE },

    { "forced-idr", "If forcing keyframes, force them as IDR frames.", OFFSET(forced_idr),
      AV_OPT_TYPE_BOOL,   { .i64 = 0 }, 0, 1, VE },

    {NULL},
};

static const AVClass class = {
    .class_name = "libsvt_av1",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const AVCodecDefault eb_enc_defaults[] = {
    { "b",         "7M"    },
    { "g",         "-2"    },
    { "qmin",      "0"     },
    { "qmax",      "63"    },
    { NULL },
};

AVCodec ff_libsvt_av1_encoder = {
    .name           = "libsvt_av1",
    .long_name      = NULL_IF_CONFIG_SMALL("SVT-AV1(Scalable Video Technology for AV1) encoder"),
    .priv_data_size = sizeof(SvtContext),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_AV1,
    .init           = eb_enc_init,
    .send_frame     = eb_send_frame,
    .receive_packet = eb_receive_packet,
    .close          = eb_enc_close,
    .capabilities   = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AUTO_THREADS,
    .pix_fmts       = (const enum AVPixelFormat[]){ AV_PIX_FMT_YUV420P,
                                                    AV_PIX_FMT_YUV420P10,
                                                    AV_PIX_FMT_NONE },
    .priv_class     = &class,
    .defaults       = eb_enc_defaults,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
    .wrapper_name   = "libsvt_av1",
};
