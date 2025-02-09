/*
 * Copyright (c) 2017 Ronald S. Bultje <rsbultje@gmail.com>
 * Copyright (c) 2017 Ashish Pratap Singh <ashk43712@gmail.com>
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

 /**
  * @file
  * Calculate the VMAF between two input videos.
  */


#include <libvmaf.h>
#include "libavutil/avstring.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "avfilter.h"
#include "drawutils.h"
#include "formats.h"
#include "framesync.h"
#include "internal.h"
#include "video.h"
#include <time.h> //Leo


typedef struct LIBVMAFContext {
	const AVClass* class;
	FFFrameSync fs;
	char* model_path;
	char* log_path;
	char* log_fmt;
	int enable_transform;
	int phone_model;
	int psnr;
	int ssim;
	int ms_ssim;
	char* pool;
	int n_threads;
	int n_subsample;
	int enable_conf_interval;
	char* model_cfg;
	char* feature_cfg;
	VmafContext* vmaf;
	VmafModel** model;
	unsigned model_cnt;
	unsigned frame_cnt;
	unsigned bpc;
	} LIBVMAFContext;




#define OFFSET(x) offsetof(LIBVMAFContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM

static const AVOption libvmaf_options[] = {
	{"model_path",  "use model='path=...'.",                                            OFFSET(model_path), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 1, FLAGS | AV_OPT_FLAG_DEPRECATED},
	{"log_path",  "Set the file path to be used to write log.",                         OFFSET(log_path), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 1, FLAGS},
	{"log_fmt",  "Set the format of the log (csv, json, xml, or sub).",                 OFFSET(log_fmt), AV_OPT_TYPE_STRING, {.str = "xml"}, 0, 1, FLAGS},
	{"enable_transform",  "use model='enable_transform=true'.",                         OFFSET(enable_transform), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, FLAGS | AV_OPT_FLAG_DEPRECATED},
	{"phone_model",  "use model='enable_transform=true'.",                              OFFSET(phone_model), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, FLAGS | AV_OPT_FLAG_DEPRECATED},
	{"psnr",  "use feature='name=psnr'.",                                               OFFSET(psnr), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, FLAGS | AV_OPT_FLAG_DEPRECATED},
	{"ssim",  "use feature='name=float_ssim'.",                                         OFFSET(ssim), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, FLAGS | AV_OPT_FLAG_DEPRECATED},
	{"ms_ssim",  "use feature='name=float_ms_ssim'.",                                   OFFSET(ms_ssim), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, FLAGS | AV_OPT_FLAG_DEPRECATED},
	{"pool",  "Set the pool method to be used for computing vmaf.",                     OFFSET(pool), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 1, FLAGS},
	{"n_threads", "Set number of threads to be used when computing vmaf.",              OFFSET(n_threads), AV_OPT_TYPE_INT, {.i64 = 0}, 0, UINT_MAX, FLAGS},
	{"n_subsample", "Set interval for frame subsampling used when computing vmaf.",     OFFSET(n_subsample), AV_OPT_TYPE_INT, {.i64 = 1}, 1, UINT_MAX, FLAGS},
	{"enable_conf_interval",  "model='enable_conf_interval=true'.",                     OFFSET(enable_conf_interval), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, FLAGS | AV_OPT_FLAG_DEPRECATED},
	{"model",  "Set the model to be used for computing vmaf.",                          OFFSET(model_cfg), AV_OPT_TYPE_STRING, {.str = "version=vmaf_v0.6.1"}, 0, 1, FLAGS},
	{"feature",  "Set the feature to be used for computing vmaf.",                      OFFSET(feature_cfg), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 1, FLAGS},
	{ NULL }
	};

FRAMESYNC_DEFINE_CLASS(libvmaf, LIBVMAFContext, fs);

static enum VmafPixelFormat pix_fmt_map(enum AVPixelFormat av_pix_fmt)
	{
	switch (av_pix_fmt) {
			case AV_PIX_FMT_YUV420P:
			case AV_PIX_FMT_YUV420P10LE:
			case AV_PIX_FMT_YUV420P12LE:
			case AV_PIX_FMT_YUV420P16LE:
				return VMAF_PIX_FMT_YUV420P;
			case AV_PIX_FMT_YUV422P:
			case AV_PIX_FMT_YUV422P10LE:
			case AV_PIX_FMT_YUV422P12LE:
			case AV_PIX_FMT_YUV422P16LE:
				return VMAF_PIX_FMT_YUV422P;
			case AV_PIX_FMT_YUV444P:
			case AV_PIX_FMT_YUV444P10LE:
			case AV_PIX_FMT_YUV444P12LE:
			case AV_PIX_FMT_YUV444P16LE:
				return VMAF_PIX_FMT_YUV444P;
			default:
				return VMAF_PIX_FMT_UNKNOWN;
		}
	}

/*The bpc variable is a value that indicates the number of bits per color channel (bits per component) in the video stream.
The > operator is a comparison operator that checks if the value of bpc is greater than 8. If bpc is greater than 8,
bytes_per_value is assigned a value of 2, indicating that each color channel value will take up two bytes of memory.
If bpc is less than or equal to 8, bytes_per_value is assigned a value of 1, indicating that each color channel value will
take up one byte of memory.*/

static int copy_picture_data(AVFrame* src, VmafPicture* dst, unsigned bpc)
	{
	const int bytes_per_value = bpc > 8 ? 2 : 1;
	int err = vmaf_picture_alloc(dst, pix_fmt_map(src->format), bpc,
		src->width, src->height);
	if (err)
		return AVERROR(ENOMEM);

	for (unsigned i = 0; i < 3; i++) {
		uint8_t* src_data = src->data[i];
		uint8_t* dst_data = dst->data[i];
		for (unsigned j = 0; j < dst->h[i]; j++) {
			memcpy(dst_data, src_data, bytes_per_value * dst->w[i]);
			src_data += src->linesize[i];
			dst_data += dst->stride[i];
			}
		}

	return 0;
	}

/*The function starts by getting the input frames from the FFFrameSync object fs, which contains the reference and distorted frames.
If there is no reference frame or if the filter has been disabled, the function returns immediately.

Then, it creates VmafPicture structures pic_ref and pic_dist and copies the pixel data from the reference and distorted frames to these structures using the copy_picture_data function. This function is responsible for copying the frame data to the appropriate buffer based on the pixel format and bit depth. If there is an error during the copy, the function returns with an appropriate error code.

After both pictures have been allocated and their data copied, the function calls vmaf_read_pictures with the pic_ref and pic_dist structures and the current frame number s->frame_cnt. This function calculates and stores the VMAF scores for the reference and distorted pictures in the internal VMAF state. If there is an error during this operation, the function returns with an appropriate error code.

Overall, the do_vmaf function reads in the input frames, copies their data to VmafPicture structures, and calculates the VMAF score for these pictures using the vmaf_read_pictures function. The resulting VMAF scores are stored in the internal VMAF state for later use.*/


static int do_vmaf(FFFrameSync* fs)
	{
	AVFilterContext* ctx = fs->parent;
	LIBVMAFContext* s = ctx->priv;
	VmafPicture pic_ref, pic_dist;
	AVFrame* ref, * dist;
	int err = 0;

	int ret = ff_framesync_dualinput_get(fs, &dist, &ref);
	if (ret < 0)
		return ret;
	if (ctx->is_disabled || !ref)
		return ff_filter_frame(ctx->outputs[0], dist);

	err = copy_picture_data(ref, &pic_ref, s->bpc);
	if (err) {
		av_log(s, AV_LOG_ERROR, "problem during vmaf_picture_alloc.\n");
		return AVERROR(ENOMEM);
		}

	err = copy_picture_data(dist, &pic_dist, s->bpc);
	if (err) {
		av_log(s, AV_LOG_ERROR, "problem during vmaf_picture_alloc.\n");
		vmaf_picture_unref(&pic_ref);
		return AVERROR(ENOMEM);
		}

	err = vmaf_read_pictures(s->vmaf, &pic_ref, &pic_dist, s->frame_cnt++);
	if (err) {
		av_log(s, AV_LOG_ERROR, "problem during vmaf_read_poctures.\n");
		return AVERROR(EINVAL);
		}

	/*inserted from*/
	/* This: s->vmaf->feature_collector; does not work here because s is pointer to LIBVMAFContext,
	and s->vmaf should be a VmafContext, but somehow not recognised because ?? it is not defined in libvmaf.h ??
	I don't know, C is not my specialty.
	when you call e.g. vmaf_score_at_index, that is in the Netflix libvmaf, it gets treated as a VmafContext
	which does have a feature_collector property

	BE AWARE:
	If an unsigned int and a (signed) int are used in the same expression, the signed int gets implicitly converted to unsigned.
	This is a rather dangerous feature of the C language, and one you therefore need to be aware of. It may or may not be the 
	cause of your bug. If you want a more detailed answer, you'll have to post some code.

	*/


	for (unsigned x = 0; x < s->model_cnt; x++) {
		double vmaf_score;
		char MyLine[512]; //Zwechon
		if (s->frame_cnt < 10) continue; //it should never get to calling with a negative frame number...
		unsigned MyFrame = s->frame_cnt - 10;
		int err = vmaf_score_at_index(s->vmaf, s->model[x], &vmaf_score, MyFrame);
		if (err) {
			av_log(ctx, AV_LOG_ERROR, "problem in do_vmaf in vf_libvmaf.\n");
			}
		vmaf_get_outputline_sub_Leo(s->vmaf, MyFrame, MyLine);

		av_log(NULL, AV_LOG_INFO, "%s\n", MyLine);
		}
	/*end inserted by Zwechon*/
	return ff_filter_frame(ctx->outputs[0], dist);
	}


static AVDictionary** delimited_dict_parse(char* str, unsigned* cnt)
	{
	AVDictionary** dict = NULL;
	char* str_copy = NULL;
	char* saveptr = NULL;
	unsigned cnt2;
	int err = 0;

	if (!str)
		return NULL;

	cnt2 = 1;
	for (char* p = str; *p; p++) {
		if (*p == '|')
			cnt2++;
		}

	dict = av_calloc(cnt2, sizeof(*dict));
	if (!dict)
		goto fail;

	str_copy = av_strdup(str);
	if (!str_copy)
		goto fail;

	*cnt = 0;
	for (unsigned i = 0; i < cnt2; i++) {
		char* s = av_strtok(i == 0 ? str_copy : NULL, "|", &saveptr);
		if (!s)
			continue;
		err = av_dict_parse_string(&dict[(*cnt)++], s, "=", ":", 0);
		if (err)
			goto fail;
		}

	av_free(str_copy);
	return dict;

fail:
	if (dict) {
		for (unsigned i = 0; i < *cnt; i++) {
			if (dict[i])
				av_dict_free(&dict[i]);
			}
		av_free(dict);
		}

	av_free(str_copy);
	*cnt = 0;
	return NULL;
	}

static int parse_features(AVFilterContext* ctx)
	{
	LIBVMAFContext* s = ctx->priv;
	AVDictionary** dict = NULL;
	unsigned dict_cnt;
	int err = 0;

	if (!s->feature_cfg)
		return 0;

	dict = delimited_dict_parse(s->feature_cfg, &dict_cnt);
	if (!dict) {
		av_log(ctx, AV_LOG_ERROR,
			"could not parse feature config: %s\n", s->feature_cfg);
		return AVERROR(EINVAL);
		}

	for (unsigned i = 0; i < dict_cnt; i++) {
		char* feature_name = NULL;
		VmafFeatureDictionary* feature_opts_dict = NULL;
		const AVDictionaryEntry* e = NULL;

		while (e = av_dict_iterate(dict[i], e)) {
			if (av_stristr(e->key, "name")) {
				feature_name = e->value;
				continue;
				}

			err = vmaf_feature_dictionary_set(&feature_opts_dict, e->key,
				e->value);
			if (err) {
				av_log(ctx, AV_LOG_ERROR,
					"could not set feature option: %s.%s=%s\n",
					feature_name, e->key, e->value);
				goto exit;
				}
			}

		err = vmaf_use_feature(s->vmaf, feature_name, feature_opts_dict);
		if (err) {
			av_log(ctx, AV_LOG_ERROR,
				"problem during vmaf_use_feature: %s\n", feature_name);
			goto exit;
			}
		}

exit:
	for (unsigned i = 0; i < dict_cnt; i++) {
		if (dict[i])
			av_dict_free(&dict[i]);
		}
	av_free(dict);
	return err;
	}
/*
static int parse_features(AVFilterContext* ctx)
	{
	// Get a reference to the filter's private context
	LIBVMAFContext* s = ctx->priv;
	// Create an array of dictionaries for feature configuration options
	AVDictionary** dict = NULL;
	unsigned dict_cnt;
	int err = 0;

	// If no feature configuration options are present, return 0
	if (!s->feature_cfg)
		return 0;

	// Parse the feature configuration options
	dict = delimited_dict_parse(s->feature_cfg, &dict_cnt);
	// If parsing fails, log an error and return an error code
	if (!dict) {
		av_log(ctx, AV_LOG_ERROR,
			"could not parse feature config: %s\n", s->feature_cfg);
		return AVERROR(EINVAL);
		}

	// Loop through each dictionary of feature configuration options
	for (unsigned i = 0; i < dict_cnt; i++) {
		// Initialize variables to store the feature name and options
		char* feature_name = NULL;
		VmafFeatureDictionary* feature_opts_dict = NULL;
		const AVDictionaryEntry* e = NULL;

		// Loop through each key-value pair in the current dictionary
		while (e = av_dict_iterate(dict[i], e)) {
			// If the key contains the string "name", set the feature name
			if (av_stristr(e->key, "name")) {
				feature_name = e->value;
				continue;
				}

			// Set the current key-value pair as a feature option
			err = vmaf_feature_dictionary_set(&feature_opts_dict, e->key,
				e->value);
			// If setting fails, log an error and jump to the "exit" label
			if (err) {
				av_log(ctx, AV_LOG_ERROR,
					"could not set feature option: %s.%s=%s\n",
					feature_name, e->key, e->value);
				goto exit;
				}
			}

		// Use the VMAF library to set the feature name and options
		err = vmaf_use_feature(s->vmaf, feature_name, feature_opts_dict);
		// If setting fails, log an error and jump to the "exit" label
		if (err) {
			av_log(ctx, AV_LOG_ERROR,
				"problem during vmaf_use_feature: %s\n", feature_name);
			goto exit;
			}
		}

	// Free memory used by the feature configuration options dictionaries
exit:
	for (unsigned i = 0; i < dict_cnt; i++) {
		if (dict[i])
			av_dict_free(&dict[i]);
		}
	av_free(dict);
	// Return the error code (if any)
	return err;
	}
*/

static int parse_models(AVFilterContext* ctx)
	{
	LIBVMAFContext* s = ctx->priv;
	AVDictionary** dict;
	unsigned dict_cnt;
	int err = 0;

	if (!s->model_cfg) return 0;

	dict_cnt = 0;
	dict = delimited_dict_parse(s->model_cfg, &dict_cnt);
	if (!dict) {
		av_log(ctx, AV_LOG_ERROR,
			"could not parse model config: %s\n", s->model_cfg);
		return AVERROR(EINVAL);
		}

	s->model_cnt = dict_cnt;
	s->model = av_calloc(s->model_cnt, sizeof(*s->model));
	if (!s->model)
		return AVERROR(ENOMEM);

	for (unsigned i = 0; i < dict_cnt; i++) {
		VmafModelConfig model_cfg = { 0 };
		const AVDictionaryEntry* e = NULL;
		char* version = NULL;
		char* path = NULL;

		while (e = av_dict_iterate(dict[i], e)) {
			if (av_stristr(e->key, "disable_clip")) {
				model_cfg.flags |= av_stristr(e->value, "true") ?
					VMAF_MODEL_FLAG_DISABLE_CLIP : 0;
				continue;
				}

			if (av_stristr(e->key, "enable_transform")) {
				model_cfg.flags |= av_stristr(e->value, "true") ?
					VMAF_MODEL_FLAG_ENABLE_TRANSFORM : 0;
				continue;
				}

			if (av_stristr(e->key, "name")) {
				model_cfg.name = e->value;
				continue;
				}

			if (av_stristr(e->key, "version")) {
				version = e->value;
				continue;
				}

			if (av_stristr(e->key, "path")) {
				path = e->value;
				continue;
				}
			}

		if (version) {
			err = vmaf_model_load(&s->model[i], &model_cfg, version);
			if (err) {
				av_log(ctx, AV_LOG_ERROR,
					"could not load libvmaf model with version: %s\n",
					version);
				goto exit;
				}
			}

		if (path && !s->model[i]) {
			err = vmaf_model_load_from_path(&s->model[i], &model_cfg, path);
			if (err) {
				av_log(ctx, AV_LOG_ERROR,
					"could not load libvmaf model with path: %s\n",
					path);
				goto exit;
				}
			}

		if (!s->model[i]) {
			av_log(ctx, AV_LOG_ERROR,
				"could not load libvmaf model with config: %s\n",
				s->model_cfg);
			goto exit;
			}

		while (e = av_dict_iterate(dict[i], e)) {
			VmafFeatureDictionary* feature_opts_dict = NULL;
			char* feature_opt = NULL;

			char* feature_name = av_strtok(e->key, ".", &feature_opt);
			if (!feature_opt)
				continue;

			err = vmaf_feature_dictionary_set(&feature_opts_dict,
				feature_opt, e->value);
			if (err) {
				av_log(ctx, AV_LOG_ERROR,
					"could not set feature option: %s.%s=%s\n",
					feature_name, feature_opt, e->value);
				err = AVERROR(EINVAL);
				goto exit;
				}

			err = vmaf_model_feature_overload(s->model[i], feature_name,
				feature_opts_dict);
			if (err) {
				av_log(ctx, AV_LOG_ERROR,
					"could not overload feature: %s\n", feature_name);
				err = AVERROR(EINVAL);
				goto exit;
				}
			}
		}

	for (unsigned i = 0; i < s->model_cnt; i++) {
		err = vmaf_use_features_from_model(s->vmaf, s->model[i]);
		if (err) {
			av_log(ctx, AV_LOG_ERROR,
				"problem during vmaf_use_features_from_model\n");
			err = AVERROR(EINVAL);
			goto exit;
			}
		}

exit:
	for (unsigned i = 0; i < dict_cnt; i++) {
		if (dict[i])
			av_dict_free(&dict[i]);
		}
	av_free(dict);
	return err;
	}

static enum VmafLogLevel log_level_map(int log_level)
	{
	switch (log_level) {
			case AV_LOG_QUIET:
				return VMAF_LOG_LEVEL_NONE;
			case AV_LOG_ERROR:
				return VMAF_LOG_LEVEL_ERROR;
			case AV_LOG_WARNING:
				return VMAF_LOG_LEVEL_WARNING;
			case AV_LOG_INFO:
				return VMAF_LOG_LEVEL_INFO;
			case AV_LOG_DEBUG:
				return VMAF_LOG_LEVEL_DEBUG;
			default:
				return VMAF_LOG_LEVEL_INFO;
		}
	}

static int parse_deprecated_options(AVFilterContext* ctx)
	{
	LIBVMAFContext* s = ctx->priv;
	VmafModel* model = NULL;
	VmafModelCollection* model_collection = NULL;
	enum VmafModelFlags flags = VMAF_MODEL_FLAGS_DEFAULT;
	int err = 0;

	VmafModelConfig model_cfg = {
		.name = "vmaf",
		.flags = flags,
		};

	if (s->enable_transform || s->phone_model)
		flags |= VMAF_MODEL_FLAG_ENABLE_TRANSFORM;

	if (!s->model_path)
		goto extra_metrics_only;

	if (s->enable_conf_interval) {
		err = vmaf_model_collection_load_from_path(&model, &model_collection,
			&model_cfg, s->model_path);
		if (err) {
			av_log(ctx, AV_LOG_ERROR,
				"problem loading model file: %s\n", s->model_path);
			goto exit;
			}

		err = vmaf_use_features_from_model_collection(s->vmaf, model_collection);
		if (err) {
			av_log(ctx, AV_LOG_ERROR,
				"problem loading feature extractors from model file: %s\n",
				s->model_path);
			goto exit;
			}
		}
	else {
		err = vmaf_model_load_from_path(&model, &model_cfg, s->model_path);
		if (err) {
			av_log(ctx, AV_LOG_ERROR,
				"problem loading model file: %s\n", s->model_path);
			goto exit;
			}
		err = vmaf_use_features_from_model(s->vmaf, model);
		if (err) {
			av_log(ctx, AV_LOG_ERROR,
				"problem loading feature extractors from model file: %s\n",
				s->model_path);
			goto exit;
			}
		}

extra_metrics_only:
	if (s->psnr) {
		VmafFeatureDictionary* d = NULL;
		vmaf_feature_dictionary_set(&d, "enable_chroma", "false");

		err = vmaf_use_feature(s->vmaf, "psnr", d);
		if (err) {
			av_log(ctx, AV_LOG_ERROR,
				"problem loading feature extractor: psnr\n");
			goto exit;
			}
		}

	if (s->ssim) {
		err = vmaf_use_feature(s->vmaf, "float_ssim", NULL);
		if (err) {
			av_log(ctx, AV_LOG_ERROR,
				"problem loading feature extractor: ssim\n");
			goto exit;
			}
		}

	if (s->ms_ssim) {
		err = vmaf_use_feature(s->vmaf, "float_ms_ssim", NULL);
		if (err) {
			av_log(ctx, AV_LOG_ERROR,
				"problem loading feature extractor: ms_ssim\n");
			goto exit;
			}
		}

exit:
	return err;
	}

static av_cold int init(AVFilterContext* ctx)
	{
	LIBVMAFContext* s = ctx->priv;
	int err = 0;

	VmafConfiguration cfg = {
		.log_level = log_level_map(av_log_get_level()),
		.n_subsample = s->n_subsample,
		.n_threads = s->n_threads,
		};

	err = vmaf_init(&s->vmaf, cfg);
	if (err)
		return AVERROR(EINVAL);

	err = parse_deprecated_options(ctx);
	if (err)
		return err;

	err = parse_models(ctx);
	if (err)
		return err;

	err = parse_features(ctx);
	if (err)
		return err;

	s->fs.on_event = do_vmaf;
	return 0;
	}

static const enum AVPixelFormat pix_fmts[] = {
	AV_PIX_FMT_YUV444P, AV_PIX_FMT_YUV422P, AV_PIX_FMT_YUV420P,
	AV_PIX_FMT_YUV444P10LE, AV_PIX_FMT_YUV422P10LE, AV_PIX_FMT_YUV420P10LE,
	AV_PIX_FMT_NONE
	};

static int config_input_ref(AVFilterLink* inlink)
	{
	AVFilterContext* ctx = inlink->dst;
	LIBVMAFContext* s = ctx->priv;
	const AVPixFmtDescriptor* desc;
	int err = 0;

	if (ctx->inputs[0]->w != ctx->inputs[1]->w) {
		av_log(ctx, AV_LOG_ERROR, "input width must match.\n");
		err |= AVERROR(EINVAL);
		}

	if (ctx->inputs[0]->h != ctx->inputs[1]->h) {
		av_log(ctx, AV_LOG_ERROR, "input height must match.\n");
		err |= AVERROR(EINVAL);
		}

	if (ctx->inputs[0]->format != ctx->inputs[1]->format) {
		av_log(ctx, AV_LOG_ERROR, "input pix_fmt must match.\n");
		err |= AVERROR(EINVAL);
		}

	if (err)
		return err;

	desc = av_pix_fmt_desc_get(inlink->format);
	s->bpc = desc->comp[0].depth;

	return 0;
	}

static int config_output(AVFilterLink* outlink)
	{
	AVFilterContext* ctx = outlink->src;
	LIBVMAFContext* s = ctx->priv;
	AVFilterLink* mainlink = ctx->inputs[0];
	int ret;

	ret = ff_framesync_init_dualinput(&s->fs, ctx);
	if (ret < 0)
		return ret;
	outlink->w = mainlink->w;
	outlink->h = mainlink->h;
	outlink->time_base = mainlink->time_base;
	outlink->sample_aspect_ratio = mainlink->sample_aspect_ratio;
	outlink->frame_rate = mainlink->frame_rate;
	if ((ret = ff_framesync_configure(&s->fs)) < 0)
		return ret;

	return 0;
	}

static int activate(AVFilterContext* ctx)
	{
	LIBVMAFContext* s = ctx->priv;
	return ff_framesync_activate(&s->fs);
	}

static enum VmafOutputFormat log_fmt_map(const char* log_fmt)
	{
	if (log_fmt) {
		if (av_stristr(log_fmt, "xml"))
			return VMAF_OUTPUT_FORMAT_XML;
		if (av_stristr(log_fmt, "json"))
			return VMAF_OUTPUT_FORMAT_JSON;
		if (av_stristr(log_fmt, "csv"))
			return VMAF_OUTPUT_FORMAT_CSV;
		if (av_stristr(log_fmt, "sub"))
			return VMAF_OUTPUT_FORMAT_SUB;
		}

	return VMAF_OUTPUT_FORMAT_XML;
	}

static enum VmafPoolingMethod pool_method_map(const char* pool_method)
	{
	if (pool_method) {
		if (av_stristr(pool_method, "min"))
			return VMAF_POOL_METHOD_MIN;
		if (av_stristr(pool_method, "mean"))
			return VMAF_POOL_METHOD_MEAN;
		if (av_stristr(pool_method, "harmonic_mean"))
			return VMAF_POOL_METHOD_HARMONIC_MEAN;
		}

	return VMAF_POOL_METHOD_MEAN;
	}

static av_cold void uninit(AVFilterContext* ctx)
	{
	LIBVMAFContext* s = ctx->priv;
	int err = 0;

	ff_framesync_uninit(&s->fs);

	if (!s->frame_cnt)
		goto clean_up;

	err = vmaf_read_pictures(s->vmaf, NULL, NULL, 0);
	if (err) {
		av_log(ctx, AV_LOG_ERROR,
			"problem flushing libvmaf context.\n");
		}




	av_log(ctx, AV_LOG_INFO, "Next is a fast part....\n"); //Leo
	clock_t start = clock(); //Leo

	for (unsigned i = 0; i < s->model_cnt; i++) {
		double vmaf_score;
		err = vmaf_score_pooled(s->vmaf, s->model[i], pool_method_map(s->pool),
			&vmaf_score, 0, s->frame_cnt - 1);
		if (err) {
			av_log(ctx, AV_LOG_ERROR,
				"problem getting pooled vmaf score.\n");
			}

		av_log(ctx, AV_LOG_INFO, "Elapsed time: %f seconds\n", (double)(clock() - start) / CLOCKS_PER_SEC); //Leo
		//insert by Leo
	//now that all work has been done we know the total frames, namely s->frame_cnt
	//for the last 10 remaining lines I print them here to the av_log
		av_log(ctx, AV_LOG_INFO, "The last 10 of a total of %u frames\n", s->frame_cnt); //Leo

		char MyLine[512]; //Leo
		unsigned int ffstart;

		if (s->frame_cnt < 10)
			ffstart = 0;
		else
			ffstart = s->frame_cnt - 10;

		for (unsigned int MyFrame = ffstart; MyFrame < s->frame_cnt; MyFrame++) {
			av_log(ctx, AV_LOG_INFO, "Frame %u:\n", MyFrame); //this can be deleted
			int err = vmaf_get_outputline_sub_Leo(s->vmaf, MyFrame, MyLine);
			if (err < 0) {
				av_log(ctx, AV_LOG_ERROR, "Error in vmaf_get_outputline_sub_Leo for frame %u\n", MyFrame);
				continue;
				}
			av_log(NULL, AV_LOG_INFO, "%s\n", MyLine);
			}
		//end insert by Leo




		av_log(ctx, AV_LOG_INFO, "Averaged VMAF score: %f\n", vmaf_score);
		}


	if (s->vmaf) {
		if (s->log_path && !err)
			vmaf_write_output(s->vmaf, s->log_path, log_fmt_map(s->log_fmt));
		}

clean_up:
	if (s->model) {
		for (unsigned i = 0; i < s->model_cnt; i++) {
			if (s->model[i])
				vmaf_model_destroy(s->model[i]);
			}
		av_free(s->model);
		}

	if (s->vmaf)
		vmaf_close(s->vmaf);
	}

static const AVFilterPad libvmaf_inputs[] = {
	{
		.name = "main",
		.type = AVMEDIA_TYPE_VIDEO,
	},{
		.name = "reference",
		.type = AVMEDIA_TYPE_VIDEO,
		.config_props = config_input_ref,
	},
	};

static const AVFilterPad libvmaf_outputs[] = {
	{
		.name = "default",
		.type = AVMEDIA_TYPE_VIDEO,
		.config_props = config_output,
	},
	};

const AVFilter ff_vf_libvmaf = {
	.name = "libvmaf",
	.description = NULL_IF_CONFIG_SMALL("Calculate the VMAF between two video streams."),
	.preinit = libvmaf_framesync_preinit,
	.init = init,
	.uninit = uninit,
	.activate = activate,
	.priv_size = sizeof(LIBVMAFContext),
	.priv_class = &libvmaf_class,
	FILTER_INPUTS(libvmaf_inputs),
	FILTER_OUTPUTS(libvmaf_outputs),
	FILTER_PIXFMTS_ARRAY(pix_fmts),
	};
