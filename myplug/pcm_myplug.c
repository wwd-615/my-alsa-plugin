#include <syslog.h>
#include <stdio.h>
#include <sys/poll.h>
#include <syslog.h>

#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>


struct myplug_info {
	snd_pcm_extplug_t ext;
	int my_own_data;
};

static int myplug_init(snd_pcm_extplug_t *ext)
{
	syslog(LOG_NOTICE, "[alsa mypulg debug] %s enter\n", __func__);
	return 0;
}

static snd_pcm_sframes_t myplug_write(snd_pcm_extplug_t *ext,
				const snd_pcm_channel_area_t *dst_areas,
				snd_pcm_uframes_t dst_offset,
				const snd_pcm_channel_area_t *src_areas,
				snd_pcm_uframes_t src_offset,
				snd_pcm_uframes_t size)
{
	// syslog(LOG_NOTICE, "[alsa mypulg debug] %s enter\n", __func__);
	// int i;

	// for (i = 0; i < 50; i++) {
	// 	syslog(LOG_NOTICE, "myplug = 0x%x\n", *(((unsigned int *)(src_areas->addr)) + i));
	// }

	snd_pcm_area_copy(dst_areas, dst_offset, src_areas, src_offset, size, SND_PCM_FORMAT_S16);

	return size;
}

static int myplug_close(snd_pcm_extplug_t *ext)
{
	syslog(LOG_NOTICE, "[alsa mypulg debug] %s enter\n", __func__);
	return 0;
}

static const snd_pcm_extplug_callback_t my_own_callback = {
	.init = myplug_init,
	.close = myplug_close,
	.transfer = myplug_write,
};

SND_PCM_PLUGIN_DEFINE_FUNC(myplug)
{
	snd_config_iterator_t i, next;
	snd_config_t *slave = NULL;
	struct myplug_info *myplug;
	int err;

	syslog(LOG_NOTICE, "%s enter\n", __func__);
	snd_config_for_each(i, next, conf) {
		snd_config_t *n = snd_config_iterator_entry(i);
		const char *id;
		if (snd_config_get_id(n, &id) < 0)
			continue;
		if (strcmp(id, "comment") == 0 || strcmp(id, "type") == 0)
			continue;
		if (strcmp(id, "slave") == 0) {
			slave = n;
			continue;
		}
		if (strcmp(id, "my_own_parameter") == 0) {
			continue;
		}
		SNDERR("Unknown field %s", id);
		return -EINVAL;
	}

	if (! slave) {
		SNDERR("No slave defined for myplug");
		return -EINVAL;
	}

	myplug = calloc(1, sizeof(*myplug));
	if (myplug == NULL)
		return -ENOMEM;

	myplug->ext.version = SND_PCM_EXTPLUG_VERSION;
	myplug->ext.name = "My Own Plugin";
	myplug->ext.callback = &my_own_callback;
	myplug->ext.private_data = myplug;

	err = snd_pcm_extplug_create(&myplug->ext, name, root, slave, stream, mode);
	if (err < 0) {
		free(myplug);
		return err;
	}

	*pcmp = myplug->ext.pcm;
	return 0;
}

SND_PCM_PLUGIN_SYMBOL(myplug);
