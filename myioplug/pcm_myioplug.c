/*
 * Simple ALSA ioplug plugin implementation.
 *
 * This plugin forwards playback to a real slave PCM device and reads capture
 * from a real slave PCM device.
 *
 * Example config:
 * myioplug {
 *   type myioplug
 *   slave {
 *     pcm "hw:0,1"
 *   }
 * }
 */

#include <stdlib.h>
#include <string.h>
#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>
#include <alsa/pcm_plugin.h>

#define ARRAY_SIZE(ary) (sizeof(ary) / sizeof((ary)[0]))

struct myioplug {
	snd_pcm_ioplug_t io;
	snd_pcm_t *slave;
	snd_pcm_uframes_t slave_buffer_size;
};

static int myioplug_close(snd_pcm_ioplug_t *io)
{
	struct myioplug *plug = io->private_data;
	if (plug->slave)
		snd_pcm_close(plug->slave);
	free(plug);
	return 0;
}

static snd_pcm_sframes_t myioplug_pointer(snd_pcm_ioplug_t *io)
{
	struct myioplug *plug = io->private_data;
	snd_pcm_sframes_t avail;

	if (!plug->slave)
		return 0;

	avail = snd_pcm_avail(plug->slave);
	if (avail < 0)
		return 0;  /* Clamp to 0 on error */

	if (plug->slave_buffer_size == 0)
		return 0;

	return (snd_pcm_sframes_t)(plug->slave_buffer_size - avail);
}

static int myioplug_prepare(snd_pcm_ioplug_t *io)
{
	struct myioplug *plug = io->private_data;
	return snd_pcm_prepare(plug->slave);
}

static int myioplug_start(snd_pcm_ioplug_t *io)
{
	struct myioplug *plug = io->private_data;

	if (snd_pcm_state(plug->slave) == SND_PCM_STATE_RUNNING)
		return 0;

	return snd_pcm_start(plug->slave);
}

static int myioplug_stop(snd_pcm_ioplug_t *io)
{
	struct myioplug *plug = io->private_data;
	return snd_pcm_drop(plug->slave);
}

static int myioplug_drain(snd_pcm_ioplug_t *io)
{
	struct myioplug *plug = io->private_data;
	if (io->stream == SND_PCM_STREAM_PLAYBACK)
		return snd_pcm_drain(plug->slave);
	return 0;
}

static snd_pcm_sframes_t myioplug_transfer(snd_pcm_ioplug_t *io,
										   const snd_pcm_channel_area_t *areas,
										   snd_pcm_uframes_t offset,
										   snd_pcm_uframes_t size)
{
	struct myioplug *plug = io->private_data;
	char *buf = (char *)areas->addr + (areas->first + areas->step * offset) / 8;
	snd_pcm_state_t state;
	int err;

	if (!plug->slave)
		return -EBADFD;

	/* Check slave PCM state */
	state = snd_pcm_state(plug->slave);
	SNDERR("myioplug_transfer: slave state = %d", state);
	if (state == SND_PCM_STATE_OPEN) {
		SNDERR("myioplug_transfer: preparing slave");
		err = snd_pcm_prepare(plug->slave);
		if (err < 0) {
			SNDERR("myioplug_transfer: prepare failed: %d", err);
			return err;
		}
	} else if (state == SND_PCM_STATE_XRUN) {
		SNDERR("myioplug_transfer: xrun detected, recovering slave");
		err = snd_pcm_recover(plug->slave, -EPIPE, 1);
		if (err < 0) {
			SNDERR("myioplug_transfer: recover failed: %d", err);
			return err;
		}
		state = snd_pcm_state(plug->slave);
		SNDERR("myioplug_transfer: state after recover = %d", state);
	} else if (state != SND_PCM_STATE_RUNNING && state != SND_PCM_STATE_PREPARED) {
		SNDERR("myioplug_transfer: invalid state %d", state);
		return -EBADFD;
	}

	if (io->stream == SND_PCM_STREAM_PLAYBACK) {
		snd_pcm_sframes_t res = snd_pcm_writei(plug->slave, buf, size);
		if (res < 0) {
			SNDERR("myioplug_transfer: writei failed: %ld", res);
			err = snd_pcm_recover(plug->slave, res, 1);
			if (err < 0) {
				SNDERR("myioplug_transfer: recover failed: %d", err);
				return err;
			}
			SNDERR("myioplug_transfer: recovered, retrying write");
			res = snd_pcm_writei(plug->slave, buf, size);
			if (res < 0) {
				SNDERR("myioplug_transfer: retry writei failed: %ld", res);
				return res;
			}
			SNDERR("myioplug_transfer: retry res = %ld", res);
			return res;
		}

		SNDERR("myioplug_transfer: res = %ld", res);
		return res;
	}

	return snd_pcm_readi(plug->slave, buf, size);
}

static int myioplug_hw_params(snd_pcm_ioplug_t *io, snd_pcm_hw_params_t *params)
{
	struct myioplug *plug = io->private_data;
	snd_pcm_hw_params_t *hw_params;
	snd_pcm_uframes_t period = io->period_size;
	snd_pcm_uframes_t buffer = io->buffer_size;
	int err;

	if (!plug->slave)
		return -EINVAL;

	err = snd_pcm_hw_params_malloc(&hw_params);
	if (err < 0)
		return err;

	err = snd_pcm_hw_params_any(plug->slave, hw_params);
	if (err < 0)
		goto out;
	err = snd_pcm_hw_params_set_access(plug->slave, hw_params,
									   SND_PCM_ACCESS_RW_INTERLEAVED);
	if (err < 0)
		goto out;
	err = snd_pcm_hw_params_set_format(plug->slave, hw_params, io->format);
	if (err < 0)
		goto out;
	err = snd_pcm_hw_params_set_channels(plug->slave, hw_params, io->channels);
	if (err < 0)
		goto out;
	err = snd_pcm_hw_params_set_rate(plug->slave, hw_params, io->rate, 0);
	if (err < 0)
		goto out;
	err = snd_pcm_hw_params_set_period_size_near(plug->slave, hw_params, &period, 0);
	if (err < 0)
		goto out;
	err = snd_pcm_hw_params_set_buffer_size_near(plug->slave, hw_params, &buffer);
	if (err < 0)
		goto out;
	err = snd_pcm_hw_params(plug->slave, hw_params);
	if (err < 0)
		goto out;

	plug->slave_buffer_size = buffer;

 out:
	snd_pcm_hw_params_free(hw_params);
	return err;
}

static int myioplug_hw_free(snd_pcm_ioplug_t *io)
{
	struct myioplug *plug = io->private_data;
	if (!plug->slave)
		return 0;
	return snd_pcm_hw_free(plug->slave);
}

static int myioplug_sw_params(snd_pcm_ioplug_t *io, snd_pcm_sw_params_t *params)
{
	struct myioplug *plug = io->private_data;
	snd_pcm_sw_params_t *sparams;
	snd_pcm_uframes_t avail_min, start_threshold;
	snd_pcm_uframes_t len;

	snd_pcm_sw_params_get_avail_min(params, &avail_min);
	snd_pcm_sw_params_get_start_threshold(params, &start_threshold);

	len = avail_min;
	len += (int)plug->slave_buffer_size - (int)io->buffer_size;
	if ((int)len < 0)
		avail_min = 1;
	else
		avail_min = len;

	snd_pcm_sw_params_alloca(&sparams);
	snd_pcm_sw_params_current(plug->slave, sparams);
	snd_pcm_sw_params_set_avail_min(plug->slave, sparams, avail_min);
	snd_pcm_sw_params_set_start_threshold(plug->slave, sparams,
						start_threshold);

	return snd_pcm_sw_params(plug->slave, sparams);
}

static const snd_pcm_ioplug_callback_t myioplug_callback = {
	.start = myioplug_start,
	.stop = myioplug_stop,
	.transfer = myioplug_transfer,
	.pointer = myioplug_pointer,
	.close = myioplug_close,
	.hw_params = myioplug_hw_params,
	.hw_free = myioplug_hw_free,
	.sw_params = myioplug_sw_params,
	.prepare = myioplug_prepare,
	.drain = myioplug_drain,
};

static int get_slave_pcm_name(snd_config_t *slave_conf, const char **pcm_name)
{
	snd_config_iterator_t i, next;

	if (snd_config_get_string(slave_conf, pcm_name) >= 0)
		return 0;
	if (snd_config_get_type(slave_conf) != SND_CONFIG_TYPE_COMPOUND)
		return -EINVAL;

	snd_config_for_each(i, next, slave_conf) {
		snd_config_t *n = snd_config_iterator_entry(i);
		const char *id;
		if (snd_config_get_id(n, &id) < 0)
			continue;
		if (strcmp(id, "pcm") == 0) {
			if (snd_config_get_string(n, pcm_name) >= 0)
				return 0;
			return -EINVAL;
		}
	}
	return -EINVAL;
}

SND_PCM_PLUGIN_DEFINE_FUNC(myioplug)
{
	snd_config_iterator_t i, next;
	snd_config_t *slave_conf = NULL;
	const char *slave_pcm = NULL;
	struct myioplug *plug;
	int err;

	if (stream != SND_PCM_STREAM_PLAYBACK && stream != SND_PCM_STREAM_CAPTURE)
		return -EINVAL;

	snd_config_for_each(i, next, conf) {
		snd_config_t *n = snd_config_iterator_entry(i);
		const char *id;
		if (snd_config_get_id(n, &id) < 0)
			continue;
		if (strcmp(id, "comment") == 0 || strcmp(id, "type") == 0 || strcmp(id, "hint") == 0)
			continue;
		if (strcmp(id, "slave") == 0) {
			slave_conf = n;
			continue;
		}
		if (strcmp(id, "slavepcm") == 0) {
			if (snd_config_get_string(n, &slave_pcm) < 0) {
				SNDERR("myioplug slavepcm must be a string");
				return -EINVAL;
			}
			continue;
		}
		SNDERR("Unknown field %s", id);
		return -EINVAL;
	}

	if (!slave_conf && !slave_pcm) {
		SNDERR("No slave defined for myioplug");
		return -EINVAL;
	}

	if (slave_conf && !slave_pcm) {
		err = get_slave_pcm_name(slave_conf, &slave_pcm);
		if (err < 0) {
			SNDERR("No valid slave pcm found");
			return err;
		}
	}

	plug = calloc(1, sizeof(*plug));
	if (!plug)
		return -ENOMEM;

	err = snd_pcm_open(&plug->slave, slave_pcm, stream, mode);
	if (err < 0) {
		SNDERR("Cannot open slave pcm %s", slave_pcm);
		free(plug);
		return err;
	}

	plug->io.version = SND_PCM_IOPLUG_VERSION;
	plug->io.name = "ALSA myioplug";
	plug->io.callback = &myioplug_callback;
	plug->io.private_data = plug;
	plug->io.mmap_rw = 0;

	err = snd_pcm_ioplug_create(&plug->io, name, stream, mode);
	if (err < 0)
		goto error;

	{
		unsigned int access_list[] = { SND_PCM_ACCESS_RW_INTERLEAVED,
									   SND_PCM_ACCESS_MMAP_INTERLEAVED };
		err = snd_pcm_ioplug_set_param_list(&plug->io, SND_PCM_IOPLUG_HW_ACCESS,
											ARRAY_SIZE(access_list), access_list);
		if (err < 0)
			goto error;
	}

	{
		unsigned int format_list[] = { SND_PCM_FORMAT_S16_LE,
									   SND_PCM_FORMAT_S16_BE,
									   SND_PCM_FORMAT_FLOAT_LE,
									   SND_PCM_FORMAT_FLOAT_BE };
		err = snd_pcm_ioplug_set_param_list(&plug->io, SND_PCM_IOPLUG_HW_FORMAT,
											ARRAY_SIZE(format_list), format_list);
		if (err < 0)
			goto error;
	}

	err = snd_pcm_ioplug_set_param_minmax(&plug->io, SND_PCM_IOPLUG_HW_CHANNELS,
										  1, 2);
	if (err < 0)
		goto error;

	err = snd_pcm_ioplug_set_param_minmax(&plug->io, SND_PCM_IOPLUG_HW_RATE,
										  8000, 48000);
	if (err < 0)
		goto error;

	{
		unsigned int period_list[] = { 4096, 8192, 16384, 32768, 65536 };
		err = snd_pcm_ioplug_set_param_list(&plug->io, SND_PCM_IOPLUG_HW_PERIOD_BYTES,
											ARRAY_SIZE(period_list), period_list);
		if (err < 0)
			goto error;
	}

	{
		unsigned int buffer_list[] = { 8192, 16384, 32768, 65536, 131072, 262144 };
		err = snd_pcm_ioplug_set_param_list(&plug->io, SND_PCM_IOPLUG_HW_BUFFER_BYTES,
											ARRAY_SIZE(buffer_list), buffer_list);
		if (err < 0)
			goto error;
	}

	*pcmp = plug->io.pcm;
	return 0;

error:
	if (plug->io.pcm)
		snd_pcm_ioplug_delete(&plug->io);
	snd_pcm_close(plug->slave);
	free(plug);
	return err;
}

SND_PCM_PLUGIN_SYMBOL(myioplug);
