/*
 * Copyright (C) 2013 Sebastian Thorarensen <sebth@naju.se>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"
#include "audio_parser.h"
#include "decoder_api.h"
#include "tag_handler.h"

#include <assert.h>
#include <dumb.h>
#include <glib.h>

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "dumb"

enum {
	DUMB_FRAME_SIZE = 4096
};

static unsigned loop_count;
static unsigned current_loop;

static bool
dumb_decoder_init(const struct config_param *param)
{
	loop_count = config_get_block_unsigned(param, "loop_count", 1);
	dumb_register_stdfiles();
	return true;
}

static DUH *
dumb_decoder_load(const char *path_fs)
{
	DUH *retval;
	char *path_fs_lower = g_utf8_strdown(path_fs, -1);

	if (g_str_has_suffix(path_fs_lower, ".it")) {
		retval = dumb_load_it(path_fs);
	} else if (g_str_has_suffix(path_fs_lower, ".xm")) {
		retval = dumb_load_xm(path_fs);
	} else if (g_str_has_suffix(path_fs_lower, ".s3m")) {
		retval = dumb_load_s3m(path_fs);
	} else if (g_str_has_suffix(path_fs_lower, ".mod")) {
		retval = dumb_load_mod(path_fs);
	} else {
		g_warning("unknown file extension\n");
		retval = NULL;
	}

	g_free(path_fs_lower);
	return retval;
}

static void
dumb_decoder_get_audio_format(struct audio_format *audio_format)
{
	const struct config_param *param =
		config_get_param(CONF_AUDIO_OUTPUT_FORMAT);
	GError *error = NULL;

	if (param != NULL && audio_format_parse(audio_format, param->value,
			true, &error)) {
		/* DUMB currently has a maximum of 2 channels. */
		if (audio_format->channels > 2)
			audio_format->channels = 2;
		/* duh_render only supports 8 or 16 bits. */
		if (audio_format->format != SAMPLE_FORMAT_S8 &&
				audio_format->format != SAMPLE_FORMAT_S16)
			audio_format->format = SAMPLE_FORMAT_S16;
	} else  {
		/*
                 * Resonable default if the user hasn't specified any
                 * output format.
                 */
		audio_format_init(audio_format, 48000, SAMPLE_FORMAT_S16, 2);
	}
	assert(audio_format_valid(audio_format));
}

static int
dumb_decoder_callback_loop(G_GNUC_UNUSED void *data)
{
	return ++current_loop > loop_count;
}

static void
dumb_decoder_install_callbacks(DUH_SIGRENDERER *sigrenderer)
{
	DUMB_IT_SIGRENDERER *it_sr = duh_get_it_sigrenderer(sigrenderer);

	/* 0 means loop forever. */
	if (loop_count != 0) {
		current_loop = 1;
		dumb_it_set_loop_callback(it_sr, dumb_decoder_callback_loop,
				NULL);
	}

	/* Prevent modules with speed 0 from playing forever. */
	dumb_it_set_xm_speed_zero_callback(it_sr, dumb_it_callback_terminate,
			NULL);
}

static DUH_SIGRENDERER *
dumb_decoder_get_sigrenderer(DUH *duh, int channels, long pos)
{
	DUH_SIGRENDERER *sigrenderer = duh_start_sigrenderer(duh, 0, channels,
			pos);
	/* It's safe to pass NULL here, no need for checking. */
	dumb_decoder_install_callbacks(sigrenderer);
	return sigrenderer;
}

static void
dumb_decoder_seek(struct decoder *decoder, DUH *duh, int channels,
		DUH_SIGRENDERER **sigrenderer)
{
	long pos = decoder_seek_where(decoder) * 65536;
	DUH_SIGRENDERER *new_sr = dumb_decoder_get_sigrenderer(duh, channels,
			pos);

	if (new_sr) {
		duh_end_sigrenderer(*sigrenderer);
		*sigrenderer = new_sr;
		decoder_command_finished(decoder);
	} else {
		decoder_seek_error(decoder);
	}
}

static void
dumb_decoder_file_decode(struct decoder *decoder, const char *path_fs)
{
	DUH *duh;
	struct audio_format audio_format;
	DUH_SIGRENDERER *sigrenderer;
	char buffer[DUMB_FRAME_SIZE];
	enum decoder_command cmd = DECODE_COMMAND_NONE;
	int bits;
	int factor;
	long length;

	duh = dumb_decoder_load(path_fs);
	if (!duh) {
		g_warning("could not load file\n");
		return;
	}

	dumb_decoder_get_audio_format(&audio_format);
	sigrenderer = dumb_decoder_get_sigrenderer(duh, audio_format.channels,
			0);
	if (!sigrenderer) {
		g_warning("could not decode stream\n");
		unload_duh(duh);
		return;
	}

	decoder_initialized(decoder, &audio_format, true,
			duh_get_length(duh) / 65536);
	/* format is either S8 or S16. */
	bits = audio_format.format == SAMPLE_FORMAT_S8 ? 8 : 16;
	factor = bits / 8 * audio_format.channels;
	do {
		/* duh_render uses sizes in samples, not in chars. */
		length = duh_render(sigrenderer, bits, false, 1,
				65536.0f / audio_format.sample_rate,
				sizeof(buffer) / factor, buffer) * factor;
		cmd = decoder_data(decoder, NULL, buffer, length, 0);
		if (cmd == DECODE_COMMAND_SEEK)
			dumb_decoder_seek(decoder, duh, audio_format.channels,
					&sigrenderer);
	} while (length == sizeof(buffer) && cmd != DECODE_COMMAND_STOP);

	duh_end_sigrenderer(sigrenderer);
	unload_duh(duh);
}

static bool
dumb_decoder_scan_file(const char *path_fs, const struct tag_handler *handler,
		void *handler_ctx)
{
	const char *title;
	DUH *duh = dumb_decoder_load(path_fs);

	if (!duh)
		return false;

	tag_handler_invoke_duration(handler, handler_ctx,
			duh_get_length(duh) / 65536);
	title = duh_get_tag(duh, "TITLE");
	if (title)
		tag_handler_invoke_tag(handler, handler_ctx, TAG_TITLE, title);

	unload_duh(duh);
	return true;
}

static const char *const dumb_decoder_suffixes[] = {
	"it", "mod", "s3m", "xm", NULL
};

const struct decoder_plugin dumb_decoder_plugin = {
	.name = "dumb",
	.init = dumb_decoder_init,
	.finish = dumb_exit,
	.file_decode = dumb_decoder_file_decode,
	.scan_file = dumb_decoder_scan_file,
	.suffixes = dumb_decoder_suffixes,
};
