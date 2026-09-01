/*
 *			GPAC - Multimedia Framework C SDK
 *
 *  This file is part of GPAC / libx264 H.264 encoder filter - talks
 *  directly to libx264's native API (x264_encoder_open/encode/close),
 *  unlike filters/ffmpeg-x264 which goes through the full FFmpeg
 *  libavcodec/libavformat/libavutil stack to reach the same library.
 *  Much smaller footprint, and input caps match the raw YUV420
 *  (GF_PIXEL_YUV) output already produced by this repo's video
 *  decoders (libvpx, libaom, libde265, libmpeg2) directly - no
 *  intermediate pixel format negotiation needed.
 */

#include <gpac/filters.h>
#include <gpac/constants.h>
#include <gpac/mpeg4_odf.h>
#include <string.h>

#include <x264.h>

typedef struct
{
	/* opts */
	u32 bitrate;
	const char *preset;

	GF_FilterPid *ipid, *opid;
	u32 width, height, pixel_format, stride, stride_uv, nb_planes, uv_height;
	GF_Fraction fps;

	x264_t *encoder;
	Bool in_fmt_negotiate;
	Bool header_sent;
} GF_X264EncCtx;

static GF_Err x264enc_send_config(GF_X264EncCtx *ctx)
{
	x264_nal_t *nals;
	int nb_nals, i, size;
	GF_AVCConfig *cfg;
	u8 *dsi;
	u32 dsi_size;
	GF_Err e;

	size = x264_encoder_headers(ctx->encoder, &nals, &nb_nals);
	if (size < 0)
	{
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[X264Enc] Failed to retrieve SPS/PPS headers\n"));
		return GF_IO_ERR;
	}

	cfg = gf_odf_avc_cfg_new();
	cfg->configurationVersion = 1;
	cfg->nal_unit_size = 4;
	cfg->AVCProfileIndication = 66;
	cfg->profile_compatibility = 0;
	cfg->AVCLevelIndication = 30;

	for (i = 0; i < nb_nals; i++)
	{
		x264_nal_t *nal = &nals[i];
		GF_AVCConfigSlot *slot;
		/* nal->p_payload is length-prefixed (b_annexb=0): skip the 4-byte
		 * size field and NAL header byte is the first byte of the RBSP */
		u8 *nal_start = nal->p_payload + 4;
		u32 nal_size = (u32)(nal->i_payload - 4);

		if ((nal->i_type == NAL_SPS) && (nal_size >= 4))
		{
			cfg->AVCProfileIndication = nal_start[1];
			cfg->profile_compatibility = nal_start[2];
			cfg->AVCLevelIndication = nal_start[3];
		}

		slot = (GF_AVCConfigSlot *)gf_malloc(sizeof(GF_AVCConfigSlot));
		memset(slot, 0, sizeof(GF_AVCConfigSlot));
		slot->size = (u16)nal_size;
		slot->data = (u8 *)gf_malloc(nal_size);
		memcpy(slot->data, nal_start, nal_size);

		if (nal->i_type == NAL_SPS)
			gf_list_add(cfg->sequenceParameterSets, slot);
		else if (nal->i_type == NAL_PPS)
			gf_list_add(cfg->pictureParameterSets, slot);
		else
		{
			gf_free(slot->data);
			gf_free(slot);
		}
	}

	dsi = NULL;
	dsi_size = 0;
	e = gf_odf_avc_cfg_write(cfg, &dsi, &dsi_size);
	gf_odf_avc_cfg_del(cfg);
	if (e != GF_OK)
	{
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[X264Enc] Failed to write avcC config\n"));
		return e;
	}

	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_DECODER_CONFIG, &PROP_DATA_NO_COPY(dsi, dsi_size));

	ctx->header_sent = GF_TRUE;
	return GF_OK;
}

static GF_Err x264enc_setup(GF_Filter *filter, GF_X264EncCtx *ctx)
{
	x264_param_t param;

	if (ctx->encoder)
	{
		x264_encoder_close(ctx->encoder);
		ctx->encoder = NULL;
	}

	if (x264_param_default_preset(&param, ctx->preset, "zerolatency") < 0)
	{
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[X264Enc] Unknown preset \"%s\", falling back to veryfast\n", ctx->preset));
		x264_param_default_preset(&param, "veryfast", "zerolatency");
	}

	param.i_width = ctx->width;
	param.i_height = ctx->height;
	param.i_fps_num = ctx->fps.num > 0 ? (u32)ctx->fps.num : 25;
	param.i_fps_den = ctx->fps.den ? ctx->fps.den : 1;
	param.i_timebase_num = 1;
	param.i_timebase_den = param.i_fps_num;
	param.b_vfr_input = 1;

	param.rc.i_rc_method = X264_RC_ABR;
	param.rc.i_bitrate = ctx->bitrate;

	/* no B-frames: keeps decoding order == display order, avoiding the
	 * need to reorder output packets - the encoder already introduces
	 * some lookahead/ratecontrol delay regardless, drained at EOS */
	param.i_bframe = 0;

	/* length-prefixed NALs (4-byte size), not Annex-B startcodes: this
	 * is the format isobmff_1 (and any GF_CODECID_AVC consumer) expects
	 * for muxing, matching the avcC config built in x264enc_send_config */
	param.b_annexb = 0;
	param.b_repeat_headers = 0;

	ctx->encoder = x264_encoder_open(&param);
	if (!ctx->encoder)
	{
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[X264Enc] Failed to open encoder\n"));
		return GF_IO_ERR;
	}

	ctx->header_sent = GF_FALSE;
	return x264enc_send_config(ctx);
}

static GF_Err x264enc_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	const GF_PropertyValue *prop;
	GF_X264EncCtx *ctx = (GF_X264EncCtx *)gf_filter_get_udta(filter);

	if (is_remove)
	{
		if (ctx->opid)
		{
			gf_filter_pid_remove(ctx->opid);
			ctx->opid = NULL;
		}
		if (ctx->encoder)
		{
			x264_encoder_close(ctx->encoder);
			ctx->encoder = NULL;
		}
		ctx->ipid = NULL;
		return GF_OK;
	}
	if (!gf_filter_pid_check_caps(pid))
		return GF_NOT_SUPPORTED;

	ctx->ipid = pid;

	if (!ctx->opid)
	{
		ctx->opid = gf_filter_pid_new(filter);
	}
	/* copy properties at init or reconfig */
	gf_filter_pid_copy_properties(ctx->opid, ctx->ipid);
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CODECID, &PROP_UINT(GF_CODECID_AVC));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STRIDE, NULL);
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STRIDE_UV, NULL);
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_UNFRAMED, NULL);

	gf_filter_set_name(filter, "encx264:libx264");

	prop = gf_filter_pid_get_property(pid, GF_PROP_PID_WIDTH);
	if (!prop)
		return GF_OK;
	ctx->width = prop->value.uint;

	prop = gf_filter_pid_get_property(pid, GF_PROP_PID_HEIGHT);
	if (!prop)
		return GF_OK;
	ctx->height = prop->value.uint;

	prop = gf_filter_pid_get_property(pid, GF_PROP_PID_PIXFMT);
	if (!prop)
		return GF_OK;
	ctx->pixel_format = prop->value.uint;

	if (ctx->pixel_format != GF_PIXEL_YUV)
	{
		gf_filter_pid_negotiate_property(pid, GF_PROP_PID_PIXFMT, &PROP_UINT(GF_PIXEL_YUV));
		ctx->in_fmt_negotiate = GF_TRUE;
		return GF_OK;
	}
	ctx->in_fmt_negotiate = GF_FALSE;

	prop = gf_filter_pid_get_property(pid, GF_PROP_PID_FPS);
	if (prop)
		ctx->fps = prop->value.frac;
	else
	{
		ctx->fps.num = 25;
		ctx->fps.den = 1;
	}

	gf_pixel_get_size_info(ctx->pixel_format, ctx->width, ctx->height, NULL, &ctx->stride, &ctx->stride_uv, &ctx->nb_planes, &ctx->uv_height);

	return x264enc_setup(filter, ctx);
}

static GF_Err x264enc_send_nals(GF_X264EncCtx *ctx, x264_nal_t *nals, int nb_nals, x264_picture_t *pic_out)
{
	int i;
	u32 total_size = 0;
	u8 *output;
	GF_FilterPacket *dst_pck;

	if (nb_nals <= 0)
		return GF_OK;

	for (i = 0; i < nb_nals; i++)
		total_size += (u32)nals[i].i_payload;

	dst_pck = gf_filter_pck_new_alloc(ctx->opid, total_size, &output);
	if (!dst_pck)
		return GF_OUT_OF_MEM;

	total_size = 0;
	for (i = 0; i < nb_nals; i++)
	{
		memcpy(output + total_size, nals[i].p_payload, nals[i].i_payload);
		total_size += (u32)nals[i].i_payload;
	}

	gf_filter_pck_set_cts(dst_pck, (u64)pic_out->i_pts);
	gf_filter_pck_set_dts(dst_pck, (u64)pic_out->i_dts);
	gf_filter_pck_set_sap(dst_pck, pic_out->b_keyframe ? GF_FILTER_SAP_1 : GF_FILTER_SAP_NONE);

	gf_filter_pck_send(dst_pck);
	return GF_OK;
}

static GF_Err x264enc_process(GF_Filter *filter)
{
	GF_X264EncCtx *ctx = (GF_X264EncCtx *)gf_filter_get_udta(filter);
	GF_FilterPacket *pck;
	x264_picture_t pic_in, pic_out;
	x264_nal_t *nals;
	int nb_nals, ret;
	const u8 *in_data;
	u32 size;

	if (ctx->in_fmt_negotiate)
		return GF_OK;

	pck = gf_filter_pid_get_packet(ctx->ipid);
	if (!pck)
	{
		if (gf_filter_pid_is_eos(ctx->ipid))
		{
			/* drain delayed frames before signaling EOS */
			while (ctx->encoder && x264_encoder_delayed_frames(ctx->encoder) > 0)
			{
				ret = x264_encoder_encode(ctx->encoder, &nals, &nb_nals, NULL, &pic_out);
				if (ret < 0)
					break;
				x264enc_send_nals(ctx, nals, nb_nals, &pic_out);
			}
			gf_filter_pid_set_eos(ctx->opid);
			return GF_EOS;
		}
		return GF_OK;
	}

	if (!ctx->encoder)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_SERVICE_ERROR;
	}

	in_data = (const u8 *)gf_filter_pck_get_data(pck, &size);
	if (!in_data)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_IO_ERR;
	}

	x264_picture_init(&pic_in);
	pic_in.img.i_csp = X264_CSP_I420;
	pic_in.img.i_plane = 3;

	pic_in.img.plane[0] = (uint8_t *)in_data;
	pic_in.img.i_stride[0] = (int)ctx->stride;
	pic_in.img.plane[1] = (uint8_t *)in_data + ctx->stride * ctx->height;
	pic_in.img.i_stride[1] = (int)ctx->stride_uv;
	pic_in.img.plane[2] = pic_in.img.plane[1] + ctx->stride_uv * ctx->uv_height;
	pic_in.img.i_stride[2] = (int)ctx->stride_uv;

	pic_in.i_pts = (int64_t)gf_filter_pck_get_cts(pck);

	ret = x264_encoder_encode(ctx->encoder, &nals, &nb_nals, &pic_in, &pic_out);
	gf_filter_pid_drop_packet(ctx->ipid);

	if (ret < 0)
	{
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[X264Enc] Encoding failed\n"));
		return GF_NON_COMPLIANT_BITSTREAM;
	}

	return x264enc_send_nals(ctx, nals, nb_nals, &pic_out);
}

static void x264enc_finalize(GF_Filter *filter)
{
	GF_X264EncCtx *ctx = (GF_X264EncCtx *)gf_filter_get_udta(filter);
	if (ctx->encoder)
		x264_encoder_close(ctx->encoder);
}

static const GF_FilterCapability X264EncCaps[] =
	{
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_VISUAL),
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_CODECID, GF_CODECID_RAW),
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_PIXFMT, GF_PIXEL_YUV),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_VISUAL),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_CODECID, GF_CODECID_AVC),
};

#define OFFS(_n) #_n, offsetof(GF_X264EncCtx, _n)
static GF_FilterArgs X264EncArgs[] =
	{
		{OFFS(bitrate), "target bitrate in kbps", GF_PROP_UINT, "1000", NULL, GF_FS_ARG_HINT_ADVANCED},
		{OFFS(preset), "libx264 speed preset", GF_PROP_STRING, "veryfast",
		 "ultrafast|superfast|veryfast|faster|fast|medium|slow|slower|veryslow", GF_FS_ARG_HINT_ADVANCED},
		{0}};

GF_FilterRegister X264EncRegister = {
	.name = "encx264",
	GF_FS_SET_DESCRIPTION("H.264/AVC encoder (native libx264)")
		GF_FS_SET_HELP("This filter encodes a raw YUV420 video PID to H.264/AVC by calling libx264's "
					   "native API directly (no FFmpeg dependency, unlike ffmpeg-x264).")
			.private_size = sizeof(GF_X264EncCtx),
	.args = X264EncArgs,
	SETCAPS(X264EncCaps),
	.configure_pid = x264enc_configure_pid,
	.process = x264enc_process,
	.finalize = x264enc_finalize,
};

const GF_FilterRegister * EMSCRIPTEN_KEEPALIVE encx264_register(GF_FilterSession *session)
{
	return &X264EncRegister;
}

#include "filter_register.h"
__attribute__((constructor))
void register_encx264(void) {
    gf_filter_auto_register("encx264", encx264_register);
}
