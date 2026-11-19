#define _GNU_SOURCE
#include "alsa_bridge.h"

#include <alsa/asoundlib.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <speex/speex_echo.h>
#include <speex/speex_preprocess.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define HFP_RATE 8000
#define HFP_CHANNELS 1
#define HFP_FRAME_SAMPLES 160 /* 20 ms @ 8 kHz = 320 bytes (>= Q6ASM CAPTURE_MIN_PERIOD_SIZE) */
#define HFP_BUFFER_SAMPLES (HFP_FRAME_SAMPLES * 4)
#define AEC_TAIL_SAMPLES 800 /* 100 ms acoustic echo tail */
#define REF_RING_SLOTS 4
#define NLMS_TAPS 64 /* 8 ms dual-mic ambient path span @ 8 kHz */

struct alsa_devs {
	int card;
	int mm1_play;
	int mm2_cap;
	int mm3_play;
	int mm5_cap;
};

struct alsa_bridge {
	struct alsa_bridge_cfg cfg;
	struct alsa_devs devs;

	pthread_mutex_t cfg_lock;
	pthread_mutex_t aec_lock;
	pthread_t dl_thread;
	pthread_t ul_thread;
	_Atomic bool running;
	_Atomic bool stop_req;
	bool mixers_active;

	snd_pcm_t *cap_sco;
	snd_pcm_t *play_spk;
	snd_pcm_t *cap_mic;
	snd_pcm_t *play_sco;

	SpeexEchoState *echo;
	SpeexPreprocessState *prep;
	int16_t ref_ring[REF_RING_SLOTS][HFP_FRAME_SAMPLES];
	int ref_head;
	int ref_tail;
	int ref_count;

	float nlms_w[NLMS_TAPS];
	float nlms_hist[NLMS_TAPS];
	int nlms_pos;
	float bg_ratio;

	char saved_pw_profile[128];
	bool pw_suspended;
};

static int set_mixer_ctrl(snd_ctl_t *ctl, const char *name, long val_int,
			  const char *val_enum)
{
	snd_ctl_elem_id_t *id;
	snd_ctl_elem_info_t *info;
	snd_ctl_elem_value_t *val;
	snd_ctl_elem_type_t type;
	unsigned int count, i;
	int err;

	snd_ctl_elem_id_alloca(&id);
	snd_ctl_elem_info_alloca(&info);
	snd_ctl_elem_value_alloca(&val);

	snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
	snd_ctl_elem_id_set_name(id, name);
	snd_ctl_elem_info_set_id(info, id);

	err = snd_ctl_elem_info(ctl, info);
	if (err < 0) {
		fprintf(stderr, "[alsa] missing control '%s': %s\n", name,
			snd_strerror(err));
		return err;
	}

	snd_ctl_elem_value_set_id(val, id);
	type = snd_ctl_elem_info_get_type(info);
	count = snd_ctl_elem_info_get_count(info);

	if (type == SND_CTL_ELEM_TYPE_BOOLEAN) {
		for (i = 0; i < count; i++)
			snd_ctl_elem_value_set_boolean(val, i, val_int ? 1 : 0);
	} else if (type == SND_CTL_ELEM_TYPE_INTEGER) {
		for (i = 0; i < count; i++)
			snd_ctl_elem_value_set_integer(val, i, val_int);
	} else if (type == SND_CTL_ELEM_TYPE_ENUMERATED) {
		unsigned int items = snd_ctl_elem_info_get_items(info);
		int target = -1;

		if (!val_enum) {
			fprintf(stderr, "[alsa] enum value NULL for '%s'\n", name);
			return -EINVAL;
		}
		for (i = 0; i < items; i++) {
			snd_ctl_elem_info_set_item(info, i);
			if (snd_ctl_elem_info(ctl, info) == 0 &&
			    strcmp(snd_ctl_elem_info_get_item_name(info),
				   val_enum) == 0) {
				target = (int)i;
				break;
			}
		}
		if (target < 0) {
			fprintf(stderr, "[alsa] enum '%s' not valid for '%s'\n",
				val_enum, name);
			return -EINVAL;
		}
		for (i = 0; i < count; i++)
			snd_ctl_elem_value_set_enumerated(val, i,
							  (unsigned int)target);
	} else {
		fprintf(stderr, "[alsa] unsupported type for '%s'\n", name);
		return -EINVAL;
	}

	err = snd_ctl_elem_write(ctl, val);
	if (err < 0)
		fprintf(stderr, "[alsa] write '%s' failed: %s\n", name,
			snd_strerror(err));
	return err;
}

static int find_msm8916_devs(struct alsa_devs *out)
{
	int card = -1;

	memset(out, -1, sizeof(*out));
	while (snd_card_next(&card) >= 0 && card >= 0) {
		char ctl_name[32];
		snd_ctl_t *ctl = NULL;
		snd_ctl_card_info_t *cinfo;

		snprintf(ctl_name, sizeof(ctl_name), "hw:%d", card);
		if (snd_ctl_open(&ctl, ctl_name, 0) < 0)
			continue;

		snd_ctl_card_info_alloca(&cinfo);
		if (snd_ctl_card_info(ctl, cinfo) == 0 &&
		    strcmp(snd_ctl_card_info_get_id(cinfo), "msm8916") == 0) {
			int dev = -1;
			snd_pcm_info_t *pinfo;

			snd_pcm_info_alloca(&pinfo);
			out->card = card;
			while (snd_ctl_pcm_next_device(ctl, &dev) >= 0 &&
			       dev >= 0) {
				const char *pid, *pname;

				snd_pcm_info_set_device(pinfo, (unsigned int)dev);
				snd_pcm_info_set_subdevice(pinfo, 0);

				snd_pcm_info_set_stream(pinfo,
							SND_PCM_STREAM_PLAYBACK);
				if (snd_ctl_pcm_info(ctl, pinfo) == 0) {
					pid = snd_pcm_info_get_id(pinfo);
					pname = snd_pcm_info_get_name(pinfo);
					if ((pid && strstr(pid, "MultiMedia1")) ||
					    (pname && strstr(pname, "MultiMedia1")))
						out->mm1_play = dev;
					else if ((pid && strstr(pid, "MultiMedia3")) ||
						 (pname && strstr(pname, "MultiMedia3")))
						out->mm3_play = dev;
				}

				snd_pcm_info_set_stream(pinfo,
							SND_PCM_STREAM_CAPTURE);
				if (snd_ctl_pcm_info(ctl, pinfo) == 0) {
					pid = snd_pcm_info_get_id(pinfo);
					pname = snd_pcm_info_get_name(pinfo);
					if ((pid && strstr(pid, "MultiMedia2")) ||
					    (pname && strstr(pname, "MultiMedia2")))
						out->mm2_cap = dev;
					else if ((pid && strstr(pid, "MultiMedia5")) ||
						 (pname && strstr(pname, "MultiMedia5")))
						out->mm5_cap = dev;
				}
			}
			snd_ctl_close(ctl);
			break;
		}
		snd_ctl_close(ctl);
	}

	if (out->card < 0 || out->mm1_play < 0 || out->mm2_cap < 0 ||
	    out->mm3_play < 0 || out->mm5_cap < 0) {
		fprintf(stderr,
			"[alsa] missing msm8916 PCMs (card=%d mm1=%d mm2=%d mm3=%d mm5=%d)\n",
			out->card, out->mm1_play, out->mm2_cap, out->mm3_play,
			out->mm5_cap);
		return -ENODEV;
	}
	return 0;
}

static bool pulse_socket_alive(void)
{
	const char *xdg = getenv("XDG_RUNTIME_DIR");
	char path[256];
	struct stat st;

	if (!xdg || !xdg[0])
		return false;
	snprintf(path, sizeof(path), "%s/pulse/native", xdg);
	return stat(path, &st) == 0;
}

static void pipewire_suspend_card(struct alsa_bridge *br)
{
	FILE *fp;
	char line[256];

	br->pw_suspended = false;
	snprintf(br->saved_pw_profile, sizeof(br->saved_pw_profile),
		 "HiFi (Mic1, Speaker)");

	if (!pulse_socket_alive())
		return;

	fp = popen(
		"pactl list cards 2>/dev/null | awk '/Name: alsa_card\\.platform-7702000\\.sound/{f=1} f && /Active Profile:/{sub(/.*Active Profile: /,\"\"); print; exit}'",
		"r");
	if (fp) {
		if (fgets(line, sizeof(line), fp)) {
			size_t len = strlen(line);
			while (len > 0 &&
			       (line[len - 1] == '\n' || line[len - 1] == '\r'))
				line[--len] = '\0';
			if (len > 0)
				snprintf(br->saved_pw_profile,
					 sizeof(br->saved_pw_profile), "%s",
					 line);
		}
		pclose(fp);
	}

	if (system("pactl set-card-profile alsa_card.platform-7702000.sound off >/dev/null 2>&1") ==
	    0) {
		br->pw_suspended = true;
		fprintf(stderr,
			"[pw] suspended alsa_card.platform-7702000.sound (was '%s')\n",
			br->saved_pw_profile);
	}
}

static void pipewire_restore_card(struct alsa_bridge *br)
{
	char cmd[320];

	if (!br->pw_suspended || !pulse_socket_alive())
		return;

	snprintf(cmd, sizeof(cmd),
		 "pactl set-card-profile alsa_card.platform-7702000.sound '%s' >/dev/null 2>&1",
		 br->saved_pw_profile);
	if (system(cmd) == 0)
		fprintf(stderr,
			"[pw] restored alsa_card.platform-7702000.sound -> '%s'\n",
			br->saved_pw_profile);
	(void)system("pactl set-default-sink alsa_output.platform-7702000.sound.HiFi__Speaker__sink >/dev/null 2>&1");
	(void)system("pactl set-default-source alsa_input.platform-7702000.sound.HiFi__Mic1__source >/dev/null 2>&1");
	br->pw_suspended = false;
}

static int apply_output_mixers(snd_ctl_t *ctl, const struct alsa_bridge_cfg *cfg)
{
	/* Map HFP VGS 0..15 -> WCD RX digital volume 69..84 (84 = 0 dB) */
	int vgs = cfg->vgs_level;
	long dig_vol;

	if (vgs < 0)
		vgs = 0;
	if (vgs > 15)
		vgs = 15;
	dig_vol = (vgs == 0) ? 0 : (69 + vgs);

	/* Mute all three outputs before enabling target path */
	set_mixer_ctrl(ctl, "EAR_S", 0, "ZERO");
	set_mixer_ctrl(ctl, "SPK DAC Switch", 0, NULL);
	set_mixer_ctrl(ctl, "HPHL", 0, "ZERO");
	set_mixer_ctrl(ctl, "HPHR", 0, "ZERO");
	set_mixer_ctrl(ctl, "RX1 MIX1 INP1", 0, "ZERO");
	set_mixer_ctrl(ctl, "RX2 MIX1 INP1", 0, "ZERO");
	set_mixer_ctrl(ctl, "RX3 MIX1 INP1", 0, "ZERO");

	if (cfg->output_mode == OUTPUT_SPEAKER) {
		set_mixer_ctrl(ctl, "RX3 MIX1 INP1", 0, "RX1");
		set_mixer_ctrl(ctl, "RX3 Mute Switch", 0, NULL);
		set_mixer_ctrl(ctl, "RX3 Digital Volume", dig_vol, NULL);
		set_mixer_ctrl(ctl, "SPK DAC Switch", 1, NULL);
	} else if (cfg->output_mode == OUTPUT_HEADPHONES) {
		set_mixer_ctrl(ctl, "RX1 MIX1 INP1", 0, "RX1");
		set_mixer_ctrl(ctl, "RX2 MIX1 INP1", 0, "RX2");
		set_mixer_ctrl(ctl, "RDAC2 MUX", 0, "RX2");
		set_mixer_ctrl(ctl, "RX1 Mute Switch", 0, NULL);
		set_mixer_ctrl(ctl, "RX2 Mute Switch", 0, NULL);
		set_mixer_ctrl(ctl, "RX1 Digital Volume", dig_vol, NULL);
		set_mixer_ctrl(ctl, "RX2 Digital Volume", dig_vol, NULL);
		set_mixer_ctrl(ctl, "HPHL", 0, "Switch");
		set_mixer_ctrl(ctl, "HPHR", 0, "Switch");
	} else {
		/* Earpiece handset default */
		set_mixer_ctrl(ctl, "RX1 MIX1 INP1", 0, "RX1");
		set_mixer_ctrl(ctl, "RDAC2 MUX", 0, "RX1");
		set_mixer_ctrl(ctl, "RX1 Mute Switch", 0, NULL);
		set_mixer_ctrl(ctl, "RX1 Digital Volume", dig_vol, NULL);
		set_mixer_ctrl(ctl, "EAR_S", 0, "Switch");
	}
	return 0;
}

static int apply_mic_mixers(snd_ctl_t *ctl, const struct alsa_bridge_cfg *cfg)
{
	int vgm = cfg->vgm_level;
	long tx_vol;

	if (vgm < 0)
		vgm = 0;
	if (vgm > 15)
		vgm = 15;
	tx_vol = (vgm == 0) ? 0 : (69 + vgm);

	/* Keep both AMIC1 (ch0/TX1) and AMIC3 (ch1/TX2) active for stereo cap_mic */
	set_mixer_ctrl(ctl, "CIC1 MUX", 0, "AMIC");
	set_mixer_ctrl(ctl, "DEC1 MUX", 0, "ADC1");
	set_mixer_ctrl(ctl, "ADC1 Volume", 6, NULL);
	set_mixer_ctrl(ctl, "TX1 HPF Switch", 1, NULL);
	set_mixer_ctrl(ctl, "TX1 Digital Volume", tx_vol, NULL);

	set_mixer_ctrl(ctl, "CIC2 MUX", 0, "AMIC");
	set_mixer_ctrl(ctl, "ADC2 MUX", 0, "INP3");
	set_mixer_ctrl(ctl, "DEC2 MUX", 0, "ADC2");
	set_mixer_ctrl(ctl, "ADC2 Volume", 6, NULL);
	set_mixer_ctrl(ctl, "TX2 HPF Switch", 1, NULL);
	set_mixer_ctrl(ctl, "TX2 Digital Volume", tx_vol, NULL);
	return 0;
}

static int configure_call_mixers(const struct alsa_bridge *br, bool active)
{
	char ctl_name[32];
	snd_ctl_t *ctl = NULL;
	int err;

	snprintf(ctl_name, sizeof(ctl_name), "hw:%d", br->devs.card);
	err = snd_ctl_open(&ctl, ctl_name, 0);
	if (err < 0) {
		fprintf(stderr, "[alsa] open ctl %s failed: %s\n", ctl_name,
			snd_strerror(err));
		return err;
	}

	if (active) {
		set_mixer_ctrl(ctl, "INT_BT_SCO_RX Audio Mixer MultiMedia1", 0,
			       NULL);
		set_mixer_ctrl(ctl, "PRI_MI2S_RX Audio Mixer MultiMedia3", 0,
			       NULL);
		set_mixer_ctrl(ctl, "MultiMedia2 Mixer INT_BT_SCO_TX", 0, NULL);
		set_mixer_ctrl(ctl, "MultiMedia5 Mixer TERT_MI2S_TX", 0, NULL);

		set_mixer_ctrl(ctl, "PRI_MI2S_RX Audio Mixer MultiMedia1", 1,
			       NULL);
		set_mixer_ctrl(ctl, "MultiMedia5 Mixer INT_BT_SCO_TX", 1, NULL);
		set_mixer_ctrl(ctl, "INT_BT_SCO_RX Audio Mixer MultiMedia3", 1,
			       NULL);
		set_mixer_ctrl(ctl, "MultiMedia2 Mixer TERT_MI2S_TX", 1, NULL);

		apply_output_mixers(ctl, &br->cfg);
		apply_mic_mixers(ctl, &br->cfg);
	} else {
		set_mixer_ctrl(ctl, "INT_BT_SCO_RX Audio Mixer MultiMedia3", 0,
			       NULL);
		set_mixer_ctrl(ctl, "MultiMedia5 Mixer INT_BT_SCO_TX", 0, NULL);
		set_mixer_ctrl(ctl, "PRI_MI2S_RX Audio Mixer MultiMedia1", 1,
			       NULL);
		set_mixer_ctrl(ctl, "MultiMedia2 Mixer TERT_MI2S_TX", 1, NULL);
	}

	snd_ctl_close(ctl);
	return 0;
}

static int open_pcm(int card, int dev, snd_pcm_stream_t dir,
		    unsigned int channels, snd_pcm_t **out)
{
	char pcm_name[32];
	snd_pcm_t *pcm = NULL;
	snd_pcm_hw_params_t *hw;
	snd_pcm_sw_params_t *sw;
	unsigned int rate = HFP_RATE;
	snd_pcm_uframes_t period = HFP_FRAME_SAMPLES;
	snd_pcm_uframes_t buffer = HFP_BUFFER_SAMPLES;
	int err = -EBUSY;
	int attempt;

	snprintf(pcm_name, sizeof(pcm_name), "hw:%d,%d", card, dev);
	for (attempt = 0; attempt < 15; attempt++) {
		err = snd_pcm_open(&pcm, pcm_name, dir, SND_PCM_NONBLOCK);
		if (err == 0 || err != -EBUSY)
			break;
		usleep(25000);
	}
	if (err < 0) {
		fprintf(stderr, "[alsa] snd_pcm_open(%s, %s) failed: %s\n",
			pcm_name,
			dir == SND_PCM_STREAM_PLAYBACK ? "PLAY" : "CAP",
			snd_strerror(err));
		return err;
	}

	snd_pcm_hw_params_alloca(&hw);
	snd_pcm_hw_params_any(pcm, hw);
	if ((err = snd_pcm_hw_params_set_access(
		     pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
	    (err = snd_pcm_hw_params_set_format(pcm, hw,
						SND_PCM_FORMAT_S16_LE)) < 0 ||
	    (err = snd_pcm_hw_params_set_channels(pcm, hw, channels)) < 0 ||
	    (err = snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, NULL)) < 0 ||
	    (err = snd_pcm_hw_params_set_period_size_near(pcm, hw, &period,
							  NULL)) < 0 ||
	    (err = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer)) <
		    0 ||
	    (err = snd_pcm_hw_params(pcm, hw)) < 0) {
		fprintf(stderr, "[alsa] hw_params(%s) failed: %s\n", pcm_name,
			snd_strerror(err));
		snd_pcm_close(pcm);
		return err;
	}

	snd_pcm_sw_params_alloca(&sw);
	snd_pcm_sw_params_current(pcm, sw);
	snd_pcm_sw_params_set_start_threshold(
		pcm, sw, dir == SND_PCM_STREAM_PLAYBACK ? period : 1);
	if ((err = snd_pcm_sw_params(pcm, sw)) < 0) {
		fprintf(stderr, "[alsa] sw_params(%s) failed: %s\n", pcm_name,
			snd_strerror(err));
		snd_pcm_close(pcm);
		return err;
	}

	fprintf(stderr,
		"[alsa] opened %s (%s): rate=%u period=%lu buffer=%lu\n",
		pcm_name, dir == SND_PCM_STREAM_PLAYBACK ? "PLAY" : "CAP", rate,
		(unsigned long)period, (unsigned long)buffer);
	*out = pcm;
	return 0;
}

static snd_pcm_sframes_t xrun_recover(struct alsa_bridge *br, snd_pcm_t *pcm,
				      int err, const char *tag)
{
	if (err == -EPIPE) {
		return snd_pcm_prepare(pcm);
	}
	if (err == -ESTRPIPE) {
		while ((err = snd_pcm_resume(pcm)) == -EAGAIN)
			usleep(10000);
		if (err < 0)
			return snd_pcm_prepare(pcm);
		return 0;
	}
	if (!br->stop_req)
		fprintf(stderr, "[alsa] %s unrecoverable error: %s\n", tag,
			snd_strerror(err));
	return err;
}

static int pcm_read_frame(struct alsa_bridge *br, snd_pcm_t *pcm, int16_t *dst,
			  snd_pcm_uframes_t frames, const char *tag)
{
	snd_pcm_uframes_t off = 0;

	while (off < frames && !br->stop_req) {
		snd_pcm_sframes_t n =
			snd_pcm_readi(pcm, dst + off, frames - off);
		if (n > 0) {
			off += (snd_pcm_uframes_t)n;
		} else if (n == -EAGAIN) {
			int w = snd_pcm_wait(pcm, 100);
			if (w < 0 && w != -EAGAIN &&
			    xrun_recover(br, pcm, w, tag) < 0)
				return -1;
		} else if (n < 0) {
			if (xrun_recover(br, pcm, (int)n, tag) < 0)
				return -1;
		}
	}
	return br->stop_req ? -1 : 0;
}

static int pcm_write_frame(struct alsa_bridge *br, snd_pcm_t *pcm,
			   const int16_t *src, snd_pcm_uframes_t frames,
			   const char *tag)
{
	snd_pcm_uframes_t off = 0;

	while (off < frames && !br->stop_req) {
		snd_pcm_sframes_t w =
			snd_pcm_writei(pcm, src + off, frames - off);
		if (w > 0) {
			off += (snd_pcm_uframes_t)w;
		} else if (w == -EAGAIN) {
			int wr = snd_pcm_wait(pcm, 100);
			if (wr < 0 && wr != -EAGAIN &&
			    xrun_recover(br, pcm, wr, tag) < 0)
				return -1;
		} else if (w < 0) {
			if (xrun_recover(br, pcm, (int)w, tag) < 0)
				return -1;
		}
	}
	return br->stop_req ? -1 : 0;
}

static void ref_ring_push(struct alsa_bridge *br, const int16_t *frame)
{
	pthread_mutex_lock(&br->aec_lock);
	if (br->ref_count == REF_RING_SLOTS) {
		br->ref_head = (br->ref_head + 1) % REF_RING_SLOTS;
		br->ref_count--;
	}
	memcpy(br->ref_ring[br->ref_tail], frame,
	       HFP_FRAME_SAMPLES * sizeof(int16_t));
	br->ref_tail = (br->ref_tail + 1) % REF_RING_SLOTS;
	br->ref_count++;
	pthread_mutex_unlock(&br->aec_lock);
}

static bool ref_ring_pop(struct alsa_bridge *br, int16_t *out)
{
	bool got = false;

	pthread_mutex_lock(&br->aec_lock);
	if (br->ref_count > 0) {
		memcpy(out, br->ref_ring[br->ref_head],
		       HFP_FRAME_SAMPLES * sizeof(int16_t));
		br->ref_head = (br->ref_head + 1) % REF_RING_SLOTS;
		br->ref_count--;
		got = true;
	}
	pthread_mutex_unlock(&br->aec_lock);
	return got;
}

static int16_t frame_peak(const int16_t *buf, size_t n)
{
	int16_t pk = 0;

	for (size_t i = 0; i < n; i++) {
		int v = buf[i] < 0 ? -buf[i] : buf[i];
		if (v > 32767)
			v = 32767;
		if (v > pk)
			pk = (int16_t)v;
	}
	return pk;
}

static void *downlink_worker(void *arg)
{
	struct alsa_bridge *br = arg;
	int16_t buf[HFP_FRAME_SAMPLES];
	int16_t silence[HFP_FRAME_SAMPLES] = {0};
	unsigned int frames = 0;
	int16_t sec_peak = 0;
	bool verbose;

	pthread_mutex_lock(&br->cfg_lock);
	verbose = br->cfg.verbose;
	pthread_mutex_unlock(&br->cfg_lock);

	/* Prefill 2 periods so playback starts smoothly without underrun */
	pcm_write_frame(br, br->play_spk, silence, HFP_FRAME_SAMPLES, "dl_pre");
	pcm_write_frame(br, br->play_spk, silence, HFP_FRAME_SAMPLES, "dl_pre");

	while (!br->stop_req) {
		int16_t pk;

		if (pcm_read_frame(br, br->cap_sco, buf, HFP_FRAME_SAMPLES,
				   "dl_cap") < 0)
			break;

		if (verbose) {
			pk = frame_peak(buf, HFP_FRAME_SAMPLES);
			if (pk > sec_peak)
				sec_peak = pk;
			if (++frames % 50 == 0) {
				fprintf(stderr, "[stat] dl_sco_peak=%d\n",
					sec_peak);
				sec_peak = 0;
			}
		}

		if (pcm_write_frame(br, br->play_spk, buf, HFP_FRAME_SAMPLES,
				    "dl_play") < 0)
			break;

		ref_ring_push(br, buf);
	}
	return NULL;
}

static void dual_mic_nc_process(struct alsa_bridge *br, const int16_t *pri,
				const int16_t *ref_mic, int16_t *out)
{
	double sum_pri = 0.0, sum_ref = 0.0;
	float rms_pri, rms_ref, ratio;
	bool adapt = false;

	for (int i = 0; i < HFP_FRAME_SAMPLES; i++) {
		double p = pri[i];
		double r = ref_mic[i];
		sum_pri += p * p;
		sum_ref += r * r;
	}
	rms_pri = (float)sqrt(sum_pri / HFP_FRAME_SAMPLES);
	rms_ref = (float)sqrt(sum_ref / HFP_FRAME_SAMPLES);

	/* Adapt NLMS filter only on ambient-noise frames; freeze during near-mouth speech */
	if (rms_ref >= 2.0f) {
		ratio = rms_pri / rms_ref;
		if (ratio < br->bg_ratio * 1.4f)
			br->bg_ratio = 0.98f * br->bg_ratio + 0.02f * ratio;
		adapt = (ratio <= br->bg_ratio * 1.35f);
	}

	for (int i = 0; i < HFP_FRAME_SAMPLES; i++) {
		float est = 0.0f;
		float err;

		br->nlms_hist[br->nlms_pos] = (float)ref_mic[i];
		for (int k = 0; k < NLMS_TAPS; k++) {
			int idx = (br->nlms_pos - k + NLMS_TAPS) % NLMS_TAPS;
			est += br->nlms_w[k] * br->nlms_hist[idx];
		}
		err = (float)pri[i] - est;

		if (adapt) {
			float norm = 500000.0f;
			for (int k = 0; k < NLMS_TAPS; k++) {
				int idx = (br->nlms_pos - k + NLMS_TAPS) %
					  NLMS_TAPS;
				norm += br->nlms_hist[idx] * br->nlms_hist[idx];
			}
			float step = 0.08f * err / norm;
			for (int k = 0; k < NLMS_TAPS; k++) {
				int idx = (br->nlms_pos - k + NLMS_TAPS) %
					  NLMS_TAPS;
				float w = br->nlms_w[k] +
					  step * br->nlms_hist[idx];
				if (w > 12.0f)
					w = 12.0f;
				else if (w < -12.0f)
					w = -12.0f;
				br->nlms_w[k] = w;
			}
		}

		br->nlms_pos = (br->nlms_pos + 1) % NLMS_TAPS;
		if (err > 32767.0f)
			err = 32767.0f;
		else if (err < -32768.0f)
			err = -32768.0f;
		out[i] = (int16_t)err;
	}
}

static void *uplink_worker(void *arg)
{
	struct alsa_bridge *br = arg;
	int16_t mic_stereo[HFP_FRAME_SAMPLES * 2];
	int16_t pri[HFP_FRAME_SAMPLES];
	int16_t ref_mic[HFP_FRAME_SAMPLES];
	int16_t mic[HFP_FRAME_SAMPLES];
	int16_t ref[HFP_FRAME_SAMPLES];
	int16_t proc[HFP_FRAME_SAMPLES];
	int16_t silence[HFP_FRAME_SAMPLES] = {0};
	unsigned int frames = 0;
	int16_t mic_peak = 0, out_peak = 0;

	pcm_write_frame(br, br->play_sco, silence, HFP_FRAME_SAMPLES, "ul_pre");
	pcm_write_frame(br, br->play_sco, silence, HFP_FRAME_SAMPLES, "ul_pre");

	while (!br->stop_req) {
		bool use_aec, use_ns, muted, verbose;
		enum hfp_mic_source mic_src;
		int16_t pk;

		if (pcm_read_frame(br, br->cap_mic, mic_stereo,
				   HFP_FRAME_SAMPLES, "ul_cap") < 0)
			break;

		for (int i = 0; i < HFP_FRAME_SAMPLES; i++) {
			pri[i] = mic_stereo[2 * i];
			ref_mic[i] = mic_stereo[2 * i + 1];
		}

		pthread_mutex_lock(&br->cfg_lock);
		use_aec = br->cfg.enable_aec;
		use_ns = br->cfg.enable_ns;
		muted = br->cfg.muted;
		verbose = br->cfg.verbose;
		mic_src = br->cfg.mic_source;
		pthread_mutex_unlock(&br->cfg_lock);

		if (mic_src == MIC_AMIC3)
			memcpy(mic, ref_mic, sizeof(mic));
		else if (mic_src == MIC_DUAL)
			dual_mic_nc_process(br, pri, ref_mic, mic);
		else
			memcpy(mic, pri, sizeof(mic));

		if (verbose) {
			pk = frame_peak(mic, HFP_FRAME_SAMPLES);
			if (pk > mic_peak)
				mic_peak = pk;
		}

		if (muted) {
			memset(proc, 0, sizeof(proc));
		} else {
			if (use_aec && br->echo && ref_ring_pop(br, ref))
				speex_echo_cancellation(br->echo, mic, ref,
							proc);
			else
				memcpy(proc, mic, sizeof(proc));

			if (use_ns && br->prep)
				speex_preprocess_run(br->prep, proc);
		}

		if (verbose) {
			pk = frame_peak(proc, HFP_FRAME_SAMPLES);
			if (pk > out_peak)
				out_peak = pk;
			if (++frames % 50 == 0) {
				fprintf(stderr,
					"[stat] ul_mic_peak=%d ul_out_peak=%d\n",
					mic_peak, out_peak);
				mic_peak = out_peak = 0;
			}
		}

		if (pcm_write_frame(br, br->play_sco, proc, HFP_FRAME_SAMPLES,
				    "ul_play") < 0)
			break;
	}
	return NULL;
}

struct alsa_bridge *alsa_bridge_create(const struct alsa_bridge_cfg *cfg)
{
	struct alsa_bridge *br = calloc(1, sizeof(*br));
	if (!br)
		return NULL;

	br->cfg = *cfg;
	pthread_mutex_init(&br->cfg_lock, NULL);
	pthread_mutex_init(&br->aec_lock, NULL);

	for (int attempt = 0; attempt < 20; attempt++) {
		if (find_msm8916_devs(&br->devs) == 0)
			break;
		sleep(1);
	}
	if (br->devs.card < 0 || br->devs.mm1_play < 0 ||
	    br->devs.mm2_cap < 0 || br->devs.mm3_play < 0 ||
	    br->devs.mm5_cap < 0) {
		pthread_mutex_destroy(&br->cfg_lock);
		pthread_mutex_destroy(&br->aec_lock);
		free(br);
		return NULL;
	}
	if (pulse_socket_alive()) {
		(void)system("pactl set-default-sink alsa_output.platform-7702000.sound.HiFi__Speaker__sink >/dev/null 2>&1");
		(void)system("pactl set-default-source alsa_input.platform-7702000.sound.HiFi__Mic1__source >/dev/null 2>&1");
	}
	fprintf(stderr,
		"[alsa] discovered msm8916 on card=%d (mm1=%d, mm2=%d, mm3=%d, mm5=%d)\n",
		br->devs.card, br->devs.mm1_play, br->devs.mm2_cap,
		br->devs.mm3_play, br->devs.mm5_cap);
	return br;
}

void alsa_bridge_destroy(struct alsa_bridge *br)
{
	if (!br)
		return;
	alsa_bridge_stop(br);
	pthread_mutex_destroy(&br->cfg_lock);
	pthread_mutex_destroy(&br->aec_lock);
	free(br);
}

int alsa_bridge_start(struct alsa_bridge *br)
{
	int rate = HFP_RATE;
	int val_on = 1;
	int val_off = 0;
	int noise_db = -18;
	int rc;

	if (!br)
		return -EINVAL;
	if (br->running)
		return 0;

	pipewire_suspend_card(br);

	pthread_mutex_lock(&br->cfg_lock);
	configure_call_mixers(br, true);
	br->mixers_active = true;
	pthread_mutex_unlock(&br->cfg_lock);

	if (open_pcm(br->devs.card, br->devs.mm5_cap, SND_PCM_STREAM_CAPTURE,
		     HFP_CHANNELS, &br->cap_sco) < 0 ||
	    open_pcm(br->devs.card, br->devs.mm1_play, SND_PCM_STREAM_PLAYBACK,
		     HFP_CHANNELS, &br->play_spk) < 0 ||
	    open_pcm(br->devs.card, br->devs.mm2_cap, SND_PCM_STREAM_CAPTURE,
		     2, &br->cap_mic) < 0 ||
	    open_pcm(br->devs.card, br->devs.mm3_play, SND_PCM_STREAM_PLAYBACK,
		     HFP_CHANNELS, &br->play_sco) < 0) {
		alsa_bridge_stop(br);
		return -EIO;
	}

	br->echo = speex_echo_state_init(HFP_FRAME_SAMPLES, AEC_TAIL_SAMPLES);
	if (br->echo)
		speex_echo_ctl(br->echo, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);

	br->prep = speex_preprocess_state_init(HFP_FRAME_SAMPLES, HFP_RATE);
	if (br->prep) {
		speex_preprocess_ctl(br->prep, SPEEX_PREPROCESS_SET_DENOISE,
				     &val_on);
		speex_preprocess_ctl(br->prep,
				     SPEEX_PREPROCESS_SET_NOISE_SUPPRESS,
				     &noise_db);
		speex_preprocess_ctl(br->prep, SPEEX_PREPROCESS_SET_AGC,
				     &val_off);
		if (br->echo)
			speex_preprocess_ctl(br->prep,
					     SPEEX_PREPROCESS_SET_ECHO_STATE,
					     br->echo);
	}

	br->ref_head = 0;
	br->ref_tail = 0;
	br->ref_count = 0;
	memset(br->nlms_w, 0, sizeof(br->nlms_w));
	memset(br->nlms_hist, 0, sizeof(br->nlms_hist));
	br->nlms_pos = 0;
	br->bg_ratio = 7.0f;
	br->stop_req = false;
	br->running = true;

	rc = pthread_create(&br->dl_thread, NULL, downlink_worker, br);
	if (rc != 0) {
		br->running = false;
		alsa_bridge_stop(br);
		return -rc;
	}
	rc = pthread_create(&br->ul_thread, NULL, uplink_worker, br);
	if (rc != 0) {
		br->stop_req = true;
		snd_pcm_drop(br->cap_sco);
		pthread_join(br->dl_thread, NULL);
		br->running = false;
		alsa_bridge_stop(br);
		return -rc;
	}

	fprintf(stderr, "[bridge] started full-duplex INT_BT_SCO audio bridge\n");
	return 0;
}

void alsa_bridge_stop(struct alsa_bridge *br)
{
	bool was_running;

	if (!br)
		return;

	was_running = br->running;
	br->stop_req = true;

	if (br->cap_sco)
		snd_pcm_drop(br->cap_sco);
	if (br->cap_mic)
		snd_pcm_drop(br->cap_mic);
	if (br->play_spk)
		snd_pcm_drop(br->play_spk);
	if (br->play_sco)
		snd_pcm_drop(br->play_sco);

	if (was_running) {
		pthread_join(br->dl_thread, NULL);
		pthread_join(br->ul_thread, NULL);
		br->running = false;
	}

	if (br->cap_sco) {
		snd_pcm_close(br->cap_sco);
		br->cap_sco = NULL;
	}
	if (br->play_spk) {
		snd_pcm_close(br->play_spk);
		br->play_spk = NULL;
	}
	if (br->cap_mic) {
		snd_pcm_close(br->cap_mic);
		br->cap_mic = NULL;
	}
	if (br->play_sco) {
		snd_pcm_close(br->play_sco);
		br->play_sco = NULL;
	}

	if (br->prep) {
		speex_preprocess_state_destroy(br->prep);
		br->prep = NULL;
	}
	if (br->echo) {
		speex_echo_state_destroy(br->echo);
		br->echo = NULL;
	}

	if (br->mixers_active) {
		configure_call_mixers(br, false);
		br->mixers_active = false;
	}
	pipewire_restore_card(br);

	if (was_running)
		fprintf(stderr, "[bridge] stopped full-duplex INT_BT_SCO audio bridge\n");
}

bool alsa_bridge_is_running(const struct alsa_bridge *br)
{
	return br && br->running;
}

static void refresh_active_mixers(struct alsa_bridge *br)
{
	char ctl_name[32];
	snd_ctl_t *ctl = NULL;

	if (!br->running)
		return;
	snprintf(ctl_name, sizeof(ctl_name), "hw:%d", br->devs.card);
	if (snd_ctl_open(&ctl, ctl_name, 0) == 0) {
		apply_output_mixers(ctl, &br->cfg);
		apply_mic_mixers(ctl, &br->cfg);
		snd_ctl_close(ctl);
	}
}

void alsa_bridge_set_output_mode(struct alsa_bridge *br, enum hfp_output_mode mode)
{
	if (!br)
		return;
	pthread_mutex_lock(&br->cfg_lock);
	br->cfg.output_mode = mode;
	refresh_active_mixers(br);
	pthread_mutex_unlock(&br->cfg_lock);
}

void alsa_bridge_set_mic_source(struct alsa_bridge *br, enum hfp_mic_source mic)
{
	if (!br)
		return;
	pthread_mutex_lock(&br->cfg_lock);
	br->cfg.mic_source = mic;
	refresh_active_mixers(br);
	pthread_mutex_unlock(&br->cfg_lock);
}

void alsa_bridge_set_vgs(struct alsa_bridge *br, int vgs)
{
	if (!br)
		return;
	pthread_mutex_lock(&br->cfg_lock);
	br->cfg.vgs_level = vgs;
	refresh_active_mixers(br);
	pthread_mutex_unlock(&br->cfg_lock);
}

void alsa_bridge_set_vgm(struct alsa_bridge *br, int vgm)
{
	if (!br)
		return;
	pthread_mutex_lock(&br->cfg_lock);
	br->cfg.vgm_level = vgm;
	refresh_active_mixers(br);
	pthread_mutex_unlock(&br->cfg_lock);
}

void alsa_bridge_set_mute(struct alsa_bridge *br, bool muted)
{
	if (!br)
		return;
	pthread_mutex_lock(&br->cfg_lock);
	br->cfg.muted = muted;
	pthread_mutex_unlock(&br->cfg_lock);
}

void alsa_bridge_set_dsp(struct alsa_bridge *br, bool enable_aec, bool enable_ns)
{
	if (!br)
		return;
	pthread_mutex_lock(&br->cfg_lock);
	br->cfg.enable_aec = enable_aec;
	br->cfg.enable_ns = enable_ns;
	pthread_mutex_unlock(&br->cfg_lock);
}

void alsa_bridge_get_cfg(struct alsa_bridge *br, struct alsa_bridge_cfg *out)
{
	if (!br || !out)
		return;
	pthread_mutex_lock(&br->cfg_lock);
	*out = br->cfg;
	pthread_mutex_unlock(&br->cfg_lock);
}
