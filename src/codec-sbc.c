/*
 * BlueALSA - codec-sbc.c
 * Copyright (c) 2016-2021 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "codec-sbc.h"

#include <errno.h>
#include <stdbool.h>

#include <glib.h>
#include <sbc/sbc.h>

#include "shared/a2dp-codecs.h"
#include "shared/log.h"

/**
 * Get the optimum bit-pool for given parameters.
 *
 * The bit-pool values were chosen based on the A2DP specification
 * recommendations.
 *
 * @param conf A2DP SBC configuration.
 * @param quality Target quality level.
 * @return SBC bitpool value. */
uint8_t sbc_a2dp_get_bitpool(const a2dp_sbc_t *conf, unsigned int quality) {

	static const uint8_t bitpool_mono_44100[] = {
		[SBC_QUALITY_LOW] = SBC_BITPOOL_LQ_MONO_44100,
		[SBC_QUALITY_MEDIUM] = SBC_BITPOOL_MQ_MONO_44100,
		[SBC_QUALITY_HIGH] = SBC_BITPOOL_HQ_MONO_44100,
	};

	static const uint8_t bitpool_mono_48000[] = {
		[SBC_QUALITY_LOW] = SBC_BITPOOL_LQ_MONO_48000,
		[SBC_QUALITY_MEDIUM] = SBC_BITPOOL_MQ_MONO_48000,
		[SBC_QUALITY_HIGH] = SBC_BITPOOL_HQ_MONO_48000,
	};

	static const uint8_t bitpool_stereo_44100[] = {
		[SBC_QUALITY_LOW] = SBC_BITPOOL_LQ_JOINT_STEREO_44100,
		[SBC_QUALITY_MEDIUM] = SBC_BITPOOL_MQ_JOINT_STEREO_44100,
		[SBC_QUALITY_HIGH] = SBC_BITPOOL_HQ_JOINT_STEREO_44100,
	};

	static const uint8_t bitpool_stereo_48000[] = {
		[SBC_QUALITY_LOW] = SBC_BITPOOL_LQ_JOINT_STEREO_48000,
		[SBC_QUALITY_MEDIUM] = SBC_BITPOOL_MQ_JOINT_STEREO_48000,
		[SBC_QUALITY_HIGH] = SBC_BITPOOL_HQ_JOINT_STEREO_48000,
	};

	uint8_t bitpool = SBC_MIN_BITPOOL;

	if (quality == SBC_QUALITY_XQ ||
			quality == SBC_QUALITY_XQPLUS) {
		/* Check whether XQ/XQ+ is possible. If not,
		 * downgrade to high quality. */
		if (conf->frequency == SBC_SAMPLING_FREQ_44100 &&
				conf->channel_mode == SBC_CHANNEL_MODE_DUAL_CHANNEL &&
				conf->block_length == SBC_BLOCK_LENGTH_16 &&
				conf->subbands == SBC_SUBBANDS_8 &&
				conf->allocation_method == SBC_ALLOCATION_LOUDNESS)
			bitpool = quality == SBC_QUALITY_XQ ? 38 : 47;
		else {
			warn("Unable to use SBC %s, downgrading to high quality",
					quality == SBC_QUALITY_XQ ? "XQ" : "XQ+");
			quality = SBC_QUALITY_HIGH;
		}
	}

	if (quality < SBC_QUALITY_XQ)
		switch (conf->frequency) {
		case SBC_SAMPLING_FREQ_16000:
		case SBC_SAMPLING_FREQ_32000:
			bitpool = bitpool_stereo_44100[quality];
			break;
		case SBC_SAMPLING_FREQ_44100:
			switch (conf->channel_mode) {
			case SBC_CHANNEL_MODE_MONO:
			case SBC_CHANNEL_MODE_DUAL_CHANNEL:
				bitpool = bitpool_mono_44100[quality];
				break;
			case SBC_CHANNEL_MODE_STEREO:
			case SBC_CHANNEL_MODE_JOINT_STEREO:
				bitpool = bitpool_stereo_44100[quality];
				break;
			}
			break;
		case SBC_SAMPLING_FREQ_48000:
			switch (conf->channel_mode) {
			case SBC_CHANNEL_MODE_MONO:
			case SBC_CHANNEL_MODE_DUAL_CHANNEL:
				bitpool = bitpool_mono_48000[quality];
				break;
			case SBC_CHANNEL_MODE_STEREO:
			case SBC_CHANNEL_MODE_JOINT_STEREO:
				bitpool = bitpool_stereo_48000[quality];
				break;
			}
		}

	/* Clamp selected bit-pool value to supported range. */
	return MIN(MAX(conf->min_bitpool, bitpool), conf->max_bitpool);
}

#if ENABLE_FASTSTREAM
/**
 * Initialize SBC audio codec for A2DP FastStream connection.
 *
 * @param sbc SBC structure which shall be initialized.
 * @param flags SBC initialization flags.
 * @param conf A2DP FastStream configuration blob.
 * @param size Size of the configuration blob.
 * @param voice It true, SBC will be initialized for voice direction.
 * @return This function returns 0 on success or a negative error value
 *   in case of SBC audio codec initialization failure. */
int sbc_init_a2dp_faststream(sbc_t *sbc, unsigned long flags,
		const void *conf, size_t size, bool voice) {

	const a2dp_faststream_t *a2dp = conf;
	if (size != sizeof(*a2dp))
		return -EINVAL;

	int rv;
	if ((rv = sbc_init(sbc, flags)) != 0)
		return rv;

	sbc->blocks = SBC_BLK_16;
	sbc->subbands = SBC_SB_8;
	sbc->allocation = SBC_AM_LOUDNESS;

	if (voice) {

		if (!(a2dp->direction & FASTSTREAM_DIRECTION_VOICE))
			goto fail;

		switch (a2dp->frequency_voice) {
		case FASTSTREAM_SAMPLING_FREQ_VOICE_16000:
			sbc->frequency = SBC_FREQ_16000;
			break;
		default:
			goto fail;
		}

		sbc->mode = SBC_MODE_MONO;
		sbc->bitpool = 32;

	}
	else {

		if (!(a2dp->direction & FASTSTREAM_DIRECTION_MUSIC))
			goto fail;

		switch (a2dp->frequency_music) {
		case FASTSTREAM_SAMPLING_FREQ_MUSIC_44100:
			sbc->frequency = SBC_FREQ_44100;
			break;
		case FASTSTREAM_SAMPLING_FREQ_MUSIC_48000:
			sbc->frequency = SBC_FREQ_48000;
			break;
		default:
			goto fail;
		}

		sbc->mode = SBC_MODE_JOINT_STEREO;
		sbc->bitpool = 29;

	}

	return 0;

fail:
	sbc_finish(sbc);
	return -EINVAL;
}
#endif

#if ENABLE_MSBC
/**
 * Reinitialize SBC audio codec for mSBC mode.
 *
 * @param sbc SBC structure which shall be reinitialized.
 * @param flags SBC initialization flags.
 * @return This function returns 0 on success or a negative error value
 *   in case of SBC audio codec initialization failure. */
int sbc_reinit_msbc(sbc_t *sbc, unsigned long flags) {
	sbc_finish(sbc);
	return sbc_init_msbc(sbc, flags);
}
#endif

/**
 * Get string representation of the SBC encode/decode error.
 *
 * @param err The encode/decode error code.
 * @return Human-readable string. */
const char *sbc_strerror(int err) {
	if (err >= 0)
		return "Success";
	switch (err) {
	case -1:
		return "Bitstream corrupted";
	case -2:
		return "Invalid sync-word";
	case -3:
		return "Invalid CRC";
	case -4:
	case -5:
		return "Bitpool out of range";
	case -EINVAL:
		return "Invalid argument";
	case -ENOSPC:
		return "No space for output data";
	default:
		debug("Unknown SBC error code: %d", err);
		return "Unknown error";
	}
}

#if DEBUG
void sbc_print_internals(const sbc_t *sbc) {

	const char *mode[] = { "Mono", "DualChannel", "Stereo", "JointStereo" };
	const char *allocation[] = { "SNR", "Loudness" };
	const unsigned int frequency[] = { 16000, 32000, 44100, 48000 };
	const unsigned int br = 8 * sbc_get_frame_length((sbc_t *)sbc) * frequency[sbc->frequency] /
		((sbc->subbands + 1) * 4) / ((sbc->blocks + 1) * 4);

	debug("SBC setup: %u Hz %s allocation=%s blocks=%u sub-bands=%u bit-pool=%u => %u bps",
			frequency[sbc->frequency],
			mode[sbc->mode],
			allocation[sbc->allocation],
			(sbc->blocks + 1) * 4,
			(sbc->subbands + 1) * 4,
			sbc->bitpool,
			br);

}
#endif
