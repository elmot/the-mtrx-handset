#define _GNU_SOURCE
#include "alsa_bridge.h"

#include <alsa/asoundlib.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <speex/speex_echo.h>
#include <speex/speex_preprocess.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define HFP_RATE 8000
#define HFP_CHANNELS 1
#define HFP_FRAME_SAMPLES 160 /* 20 ms @ 8 kHz = 320 B (>= Q6ASM CAPTURE_MIN_PERIOD_SIZE) */
#define HFP_BUFFER_SAMPLES (HFP_FRAME_SAMPLES * 4)
#define AEC_TAIL_SAMPLES 800 /* 100 ms acoustic echo tail */
#define REF_RING_SLOTS 4
#define NLMS_TAPS 64 /* 8 ms dual-mic ambient path span @ 8 kHz (power of 2) */
#define VOL_PLACEHOLDER (-1)

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define LOG(fmt, ...) fprintf(stderr, fmt "\n", ##__VA_ARGS__)

#define PW_CARD "alsa_card.platform-7702000.sound"
#define PW_DEFAULT_PROFILE "HiFi (Mic1, Speaker)"
#define PW_HPH_PROFILE "HiFi (Headphones, Mic1, Mic2)"
#define PW_DEFAULT_SINK "alsa_output.platform-7702000.sound.HiFi__Speaker__sink"
#define PW_HPH_SINK "alsa_output.platform-7702000.sound.HiFi__Headphones__sink"
#define PW_DEFAULT_SOURCE "alsa_input.platform-7702000.sound.HiFi__Mic1__source"

struct alsa_devs {
	int card;
	int mm1_play;
	int mm2_cap;
	int mm3_play;
	int mm5_cap;
};

struct mixer_entry {
	const char *name;
	long val;
	const char *enum_val;
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
	float nlms_hist[NLMS_TAPS * 2]; /* mirrored ring for contiguous dot product */
	int nlms_pos;
	float nlms_norm;
	float bg_ratio_sq;

	bool pw_suspended;
};

static long hfp_vol_to_wcd(int level)
{
	if (level <= 0)
		return 0;
	if (level > 15)
		level = 15;
	return 69 + level; /* 70..84 (84 = 0 dB) */
}

static void set_mixer_ctrl(snd_ctl_t *ctl, const char *name, long val_int,
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

	if ((err = snd_ctl_elem_info(ctl, info)) < 0) {
		LOG("[alsa] missing control '%s': %s", name, snd_strerror(err));
		return;
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

		if (!val_enum)
			return;
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
			LOG("[alsa] enum '%s' invalid for '%s'", val_enum, name);
			return;
		}
		for (i = 0; i < count; i++)
			snd_ctl_elem_value_set_enumerated(val, i,
							  (unsigned int)target);
	} else {
		return;
	}

	if ((err = snd_ctl_elem_write(ctl, val)) < 0)
		LOG("[alsa] write '%s' failed: %s", name, snd_strerror(err));
}

static void apply_mixer_table(snd_ctl_t *ctl, const struct mixer_entry *tbl,
			      size_t n, long vol)
{
	for (size_t i = 0; i < n; i++)
		set_mixer_ctrl(ctl, tbl[i].name,
			       tbl[i].val == VOL_PLACEHOLDER ? vol : tbl[i].val,
			       tbl[i].enum_val);
}

static int find_msm8916_devs(struct alsa_devs *out)
{
	static const struct {
		snd_pcm_stream_t dir;
		const char *match;
		size_t off;
	} want[] = {
		{ SND_PCM_STREAM_PLAYBACK, "MultiMedia1", offsetof(struct alsa_devs, mm1_play) },
		{ SND_PCM_STREAM_PLAYBACK, "MultiMedia3", offsetof(struct alsa_devs, mm3_play) },
		{ SND_PCM_STREAM_CAPTURE,  "MultiMedia2", offsetof(struct alsa_devs, mm2_cap) },
		{ SND_PCM_STREAM_CAPTURE,  "MultiMedia5", offsetof(struct alsa_devs, mm5_cap) },
	};
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
			while (snd_ctl_pcm_next_device(ctl, &dev) >= 0 && dev >= 0) {
				snd_pcm_info_set_device(pinfo, (unsigned int)dev);
				snd_pcm_info_set_subdevice(pinfo, 0);
				for (size_t i = 0; i < ARRAY_SIZE(want); i++) {
					const char *pid, *pname;

					snd_pcm_info_set_stream(pinfo, want[i].dir);
					if (snd_ctl_pcm_info(ctl, pinfo) != 0)
						continue;
					pid = snd_pcm_info_get_id(pinfo);
					pname = snd_pcm_info_get_name(pinfo);
					if ((pid && strstr(pid, want[i].match)) ||
					    (pname && strstr(pname, want[i].match)))
						*(int *)((char *)out + want[i].off) = dev;
				}
			}
			snd_ctl_close(ctl);
			break;
		}
		snd_ctl_close(ctl);
	}

	if (out->card < 0 || out->mm1_play < 0 || out->mm2_cap < 0 ||
	    out->mm3_play < 0 || out->mm5_cap < 0) {
		LOG("[alsa] missing msm8916 PCMs (card=%d mm1=%d mm2=%d mm3=%d mm5=%d)",
		    out->card, out->mm1_play, out->mm2_cap, out->mm3_play, out->mm5_cap);
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

static int run_pactl(const char *const argv[])
{
	pid_t pid = fork();

	if (pid < 0)
		return -1;
	if (pid == 0) {
		int nullfd = open("/dev/null", O_RDWR);
		if (nullfd >= 0) {
			dup2(nullfd, STDOUT_FILENO);
			dup2(nullfd, STDERR_FILENO);
			if (nullfd > STDERR_FILENO)
				close(nullfd);
		}
		execvp(argv[0], (char *const *)argv);
		_exit(127);
	}
	int status = 0;
	while (waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR)
			return -1;
	}
	return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

static void pipewire_set_defaults(enum hfp_output_mode mode)
{
	const char *prof = (mode == OUTPUT_HEADPHONES) ? PW_HPH_PROFILE : PW_DEFAULT_PROFILE;
	const char *sink = (mode == OUTPUT_HEADPHONES) ? PW_HPH_SINK : PW_DEFAULT_SINK;
	const char *const prof_cmd[] = { "pactl", "set-card-profile", PW_CARD, prof, NULL };
	const char *const sink_cmd[] = { "pactl", "set-default-sink", sink, NULL };
	static const char *const src_cmd[] = { "pactl", "set-default-source", PW_DEFAULT_SOURCE, NULL };

	if (!pulse_socket_alive())
		return;
	run_pactl(prof_cmd);
	run_pactl(sink_cmd);
	run_pactl(src_cmd);
}

static void pipewire_suspend_card(struct alsa_bridge *br)
{
	static const char *const off_cmd[] = { "pactl", "set-card-profile", PW_CARD, "off", NULL };

	br->pw_suspended = false;
	if (!pulse_socket_alive())
		return;
	if (run_pactl(off_cmd) == 0) {
		br->pw_suspended = true;
		LOG("[pw] suspended %s", PW_CARD);
	}
}

static void pipewire_restore_card(struct alsa_bridge *br)
{
	enum hfp_output_mode mode;

	if (!br->pw_suspended || !pulse_socket_alive())
		return;
	pthread_mutex_lock(&br->cfg_lock);
	mode = br->cfg.output_mode;
	pthread_mutex_unlock(&br->cfg_lock);
	pipewire_set_defaults(mode);
	LOG("[pw] restored %s (output=%d)", PW_CARD, mode);
	br->pw_suspended = false;
}

static void apply_output_mixers(snd_ctl_t *ctl, const struct alsa_bridge_cfg *cfg)
{
	static const struct mixer_entry mute_all[] = {
		{ "EAR_S", 0, "ZERO" },
		{ "SPK DAC Switch", 0, NULL },
		{ "HPHL", 0, "ZERO" },
		{ "HPHR", 0, "ZERO" },
		{ "RX1 MIX1 INP1", 0, "ZERO" },
		{ "RX2 MIX1 INP1", 0, "ZERO" },
		{ "RX3 MIX1 INP1", 0, "ZERO" },
	};
	static const struct mixer_entry out_ear[] = {
		{ "RX1 MIX1 INP1", 0, "RX1" },
		{ "RDAC2 MUX", 0, "RX1" },
		{ "RX1 Mute Switch", 0, NULL },
		{ "RX1 Digital Volume", VOL_PLACEHOLDER, NULL },
		{ "EAR_S", 0, "Switch" },
	};
	static const struct mixer_entry out_spk[] = {
		{ "RX3 MIX1 INP1", 0, "RX1" },
		{ "RX3 Mute Switch", 0, NULL },
		{ "RX3 Digital Volume", VOL_PLACEHOLDER, NULL },
		{ "SPK DAC Switch", 1, NULL },
	};
	static const struct mixer_entry out_hph[] = {
		{ "RX1 MIX1 INP1", 0, "RX1" },
		{ "RX2 MIX1 INP1", 0, "RX1" }, /* mono HFP on RX1 feeds both ears */
		{ "RDAC2 MUX", 0, "RX2" },
		{ "RX1 Mute Switch", 0, NULL },
		{ "RX2 Mute Switch", 0, NULL },
		{ "RX1 Digital Volume", VOL_PLACEHOLDER, NULL },
		{ "RX2 Digital Volume", VOL_PLACEHOLDER, NULL },
		{ "HPHL", 0, "Switch" },
		{ "HPHR", 0, "Switch" },
	};
	long dig_vol = hfp_vol_to_wcd(cfg->vgs_level);

	apply_mixer_table(ctl, mute_all, ARRAY_SIZE(mute_all), 0);
	if (cfg->output_mode == OUTPUT_SPEAKER)
		apply_mixer_table(ctl, out_spk, ARRAY_SIZE(out_spk), dig_vol);
	else if (cfg->output_mode == OUTPUT_HEADPHONES)
		apply_mixer_table(ctl, out_hph, ARRAY_SIZE(out_hph), dig_vol);
	else
		apply_mixer_table(ctl, out_ear, ARRAY_SIZE(out_ear), dig_vol);
}

static void apply_mic_mixers(snd_ctl_t *ctl, const struct alsa_bridge_cfg *cfg)
{
	/* Keep both AMIC1 (ch0/TX1) and AMIC3 (ch1/TX2) active for stereo cap_mic */
	static const struct mixer_entry mic_tbl[] = {
		{ "CIC1 MUX", 0, "AMIC" },
		{ "DEC1 MUX", 0, "ADC1" },
		{ "ADC1 Volume", 6, NULL },
		{ "TX1 HPF Switch", 1, NULL },
		{ "TX1 Digital Volume", VOL_PLACEHOLDER, NULL },
		{ "CIC2 MUX", 0, "AMIC" },
		{ "ADC2 MUX", 0, "INP3" },
		{ "DEC2 MUX", 0, "ADC2" },
		{ "ADC2 Volume", 6, NULL },
		{ "TX2 HPF Switch", 1, NULL },
		{ "TX2 Digital Volume", VOL_PLACEHOLDER, NULL },
	};

	apply_mixer_table(ctl, mic_tbl, ARRAY_SIZE(mic_tbl),
			  hfp_vol_to_wcd(cfg->vgm_level));
}

static void configure_call_mixers(const struct alsa_bridge *br, bool active)
{
	static const struct mixer_entry route_on[] = {
		{ "INT_BT_SCO_RX Audio Mixer MultiMedia1", 0, NULL },
		{ "PRI_MI2S_RX Audio Mixer MultiMedia3", 0, NULL },
		{ "MultiMedia2 Mixer INT_BT_SCO_TX", 0, NULL },
		{ "MultiMedia5 Mixer TERT_MI2S_TX", 0, NULL },
		{ "PRI_MI2S_RX Audio Mixer MultiMedia1", 1, NULL },
		{ "MultiMedia5 Mixer INT_BT_SCO_TX", 1, NULL },
		{ "INT_BT_SCO_RX Audio Mixer MultiMedia3", 1, NULL },
		{ "MultiMedia2 Mixer TERT_MI2S_TX", 1, NULL },
	};
	static const struct mixer_entry route_off[] = {
		{ "INT_BT_SCO_RX Audio Mixer MultiMedia3", 0, NULL },
		{ "MultiMedia5 Mixer INT_BT_SCO_TX", 0, NULL },
		{ "PRI_MI2S_RX Audio Mixer MultiMedia1", 1, NULL },
		{ "MultiMedia2 Mixer TERT_MI2S_TX", 1, NULL },
	};
	char ctl_name[32];
	snd_ctl_t *ctl = NULL;
	int err;

	snprintf(ctl_name, sizeof(ctl_name), "hw:%d", br->devs.card);
	if ((err = snd_ctl_open(&ctl, ctl_name, 0)) < 0) {
		LOG("[alsa] open ctl %s failed: %s", ctl_name, snd_strerror(err));
		return;
	}
	if (active) {
		apply_mixer_table(ctl, route_on, ARRAY_SIZE(route_on), 0);
		apply_output_mixers(ctl, &br->cfg);
		apply_mic_mixers(ctl, &br->cfg);
	} else {
		apply_mixer_table(ctl, route_off, ARRAY_SIZE(route_off), 0);
	}
	snd_ctl_close(ctl);
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
	const char *dir_s = dir == SND_PCM_STREAM_PLAYBACK ? "PLAY" : "CAP";

	snprintf(pcm_name, sizeof(pcm_name), "hw:%d,%d", card, dev);
	for (int attempt = 0; attempt < 15; attempt++) {
		err = snd_pcm_open(&pcm, pcm_name, dir, SND_PCM_NONBLOCK);
		if (err == 0 || err != -EBUSY)
			break;
		usleep(25000);
	}
	if (err < 0) {
		LOG("[alsa] snd_pcm_open(%s, %s) failed: %s", pcm_name, dir_s,
		    snd_strerror(err));
		return err;
	}

	snd_pcm_hw_params_alloca(&hw);
	snd_pcm_hw_params_any(pcm, hw);
	if ((err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
	    (err = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE)) < 0 ||
	    (err = snd_pcm_hw_params_set_channels(pcm, hw, channels)) < 0 ||
	    (err = snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, NULL)) < 0 ||
	    (err = snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, NULL)) < 0 ||
	    (err = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer)) < 0 ||
	    (err = snd_pcm_hw_params(pcm, hw)) < 0) {
		LOG("[alsa] hw_params(%s) failed: %s", pcm_name, snd_strerror(err));
		snd_pcm_close(pcm);
		return err;
	}

	snd_pcm_sw_params_alloca(&sw);
	snd_pcm_sw_params_current(pcm, sw);
	snd_pcm_sw_params_set_start_threshold(
		pcm, sw, dir == SND_PCM_STREAM_PLAYBACK ? period : 1);
	if ((err = snd_pcm_sw_params(pcm, sw)) < 0) {
		LOG("[alsa] sw_params(%s) failed: %s", pcm_name, snd_strerror(err));
		snd_pcm_close(pcm);
		return err;
	}

	LOG("[alsa] opened %s (%s): rate=%u period=%lu buffer=%lu",
	    pcm_name, dir_s, rate, (unsigned long)period, (unsigned long)buffer);
	*out = pcm;
	return 0;
}

static snd_pcm_sframes_t xrun_recover(struct alsa_bridge *br, snd_pcm_t *pcm,
				      int err, const char *tag)
{
	if (err == -EPIPE)
		return snd_pcm_prepare(pcm);
	if (err == -ESTRPIPE) {
		while ((err = snd_pcm_resume(pcm)) == -EAGAIN)
			usleep(10000);
		return err < 0 ? snd_pcm_prepare(pcm) : 0;
	}
	if (!br->stop_req)
		LOG("[alsa] %s unrecoverable error: %s", tag, snd_strerror(err));
	return err;
}

static int pcm_xfer(struct alsa_bridge *br, snd_pcm_t *pcm, int16_t *buf,
		    snd_pcm_uframes_t frames, unsigned int ch, bool is_write,
		    const char *tag)
{
	snd_pcm_uframes_t off = 0;

	while (off < frames && !br->stop_req) {
		int16_t *ptr = buf + off * ch;
		snd_pcm_sframes_t n = is_write
			? snd_pcm_writei(pcm, ptr, frames - off)
			: snd_pcm_readi(pcm, ptr, frames - off);
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
	int16_t silence[HFP_FRAME_SAMPLES] = { 0 };
	unsigned int frames = 0;
	int16_t sec_peak = 0;
	bool verbose;

	pthread_mutex_lock(&br->cfg_lock);
	verbose = br->cfg.verbose;
	pthread_mutex_unlock(&br->cfg_lock);

	/* Prefill 2 periods so playback starts smoothly without underrun */
	pcm_xfer(br, br->play_spk, silence, HFP_FRAME_SAMPLES, 1, true, "dl_pre");
	pcm_xfer(br, br->play_spk, silence, HFP_FRAME_SAMPLES, 1, true, "dl_pre");

	while (!br->stop_req) {
		if (pcm_xfer(br, br->cap_sco, buf, HFP_FRAME_SAMPLES, 1, false,
			     "dl_cap") < 0)
			break;

		if (verbose) {
			int16_t pk = frame_peak(buf, HFP_FRAME_SAMPLES);
			if (pk > sec_peak)
				sec_peak = pk;
			if (++frames % 50 == 0) {
				LOG("[stat] dl_sco_peak=%d", sec_peak);
				sec_peak = 0;
			}
		}

		if (pcm_xfer(br, br->play_spk, buf, HFP_FRAME_SAMPLES, 1, true,
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
	bool adapt = false;

	for (int i = 0; i < HFP_FRAME_SAMPLES; i++) {
		double p = pri[i], r = ref_mic[i];
		sum_pri += p * p;
		sum_ref += r * r;
	}

	/* Adapt NLMS filter only on ambient-noise frames; freeze during near-mouth speech */
	if (sum_ref >= 4.0 * HFP_FRAME_SAMPLES) {
		float ratio_sq = (float)(sum_pri / sum_ref);
		if (ratio_sq < br->bg_ratio_sq * 1.96f)
			br->bg_ratio_sq = 0.98f * br->bg_ratio_sq + 0.02f * ratio_sq;
		adapt = (ratio_sq <= br->bg_ratio_sq * 1.8225f);
	}

	for (int i = 0; i < HFP_FRAME_SAMPLES; i++) {
		int p = (br->nlms_pos - 1) & (NLMS_TAPS - 1);
		float x = (float)ref_mic[i];
		float oldest = br->nlms_hist[p];
		const float *h;
		float est = 0.0f, err;

		br->nlms_pos = p;
		br->nlms_hist[p] = x;
		br->nlms_hist[p + NLMS_TAPS] = x;
		br->nlms_norm += x * x - oldest * oldest;
		if (br->nlms_norm < 0.0f)
			br->nlms_norm = 0.0f;
		h = &br->nlms_hist[p];

		for (int k = 0; k < NLMS_TAPS; k++)
			est += br->nlms_w[k] * h[k];
		err = (float)pri[i] - est;

		if (adapt) {
			float step = 0.08f * err / (500000.0f + br->nlms_norm);
			for (int k = 0; k < NLMS_TAPS; k++) {
				float w = br->nlms_w[k] + step * h[k];
				if (w > 12.0f)
					w = 12.0f;
				else if (w < -12.0f)
					w = -12.0f;
				br->nlms_w[k] = w;
			}
		}

		if (err > 32767.0f)
			err = 32767.0f;
		else if (err < -32768.0f)
			err = -32768.0f;
		out[i] = (int16_t)err;
	}

	/* Refresh running energy once per 20 ms frame to prevent float drift */
	float norm = 0.0f;
	const float *h = &br->nlms_hist[br->nlms_pos];
	for (int k = 0; k < NLMS_TAPS; k++)
		norm += h[k] * h[k];
	br->nlms_norm = norm;
}

static void *uplink_worker(void *arg)
{
	struct alsa_bridge *br = arg;
	int16_t mic_stereo[HFP_FRAME_SAMPLES * 2];
	int16_t pri[HFP_FRAME_SAMPLES], ref_mic[HFP_FRAME_SAMPLES];
	int16_t mic[HFP_FRAME_SAMPLES], ref[HFP_FRAME_SAMPLES];
	int16_t proc[HFP_FRAME_SAMPLES];
	int16_t silence[HFP_FRAME_SAMPLES] = { 0 };
	unsigned int frames = 0;
	int16_t mic_peak = 0, out_peak = 0;

	pcm_xfer(br, br->play_sco, silence, HFP_FRAME_SAMPLES, 1, true, "ul_pre");
	pcm_xfer(br, br->play_sco, silence, HFP_FRAME_SAMPLES, 1, true, "ul_pre");

	while (!br->stop_req) {
		struct alsa_bridge_cfg cfg;

		if (pcm_xfer(br, br->cap_mic, mic_stereo, HFP_FRAME_SAMPLES, 2,
			     false, "ul_cap") < 0)
			break;

		for (int i = 0; i < HFP_FRAME_SAMPLES; i++) {
			pri[i] = mic_stereo[2 * i];
			ref_mic[i] = mic_stereo[2 * i + 1];
		}

		alsa_bridge_get_cfg(br, &cfg);

		if (cfg.mic_source == MIC_AMIC3)
			memcpy(mic, ref_mic, sizeof(mic));
		else if (cfg.mic_source == MIC_DUAL)
			dual_mic_nc_process(br, pri, ref_mic, mic);
		else
			memcpy(mic, pri, sizeof(mic));

		if (cfg.verbose) {
			int16_t pk = frame_peak(mic, HFP_FRAME_SAMPLES);
			if (pk > mic_peak)
				mic_peak = pk;
		}

		if (cfg.muted) {
			memset(proc, 0, sizeof(proc));
		} else {
			if (cfg.enable_aec && br->echo) {
				if (!ref_ring_pop(br, ref))
					memset(ref, 0, sizeof(ref));
				speex_echo_cancellation(br->echo, mic, ref, proc);
			} else {
				memcpy(proc, mic, sizeof(proc));
			}
			if (cfg.enable_ns && br->prep)
				speex_preprocess_run(br->prep, proc);
		}

		if (cfg.verbose) {
			int16_t pk = frame_peak(proc, HFP_FRAME_SAMPLES);
			if (pk > out_peak)
				out_peak = pk;
			if (++frames % 50 == 0) {
				LOG("[stat] ul_mic_peak=%d ul_out_peak=%d",
				    mic_peak, out_peak);
				mic_peak = out_peak = 0;
			}
		}

		if (pcm_xfer(br, br->play_sco, proc, HFP_FRAME_SAMPLES, 1, true,
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
	pipewire_set_defaults(br->cfg.output_mode);
	LOG("[alsa] discovered msm8916 on card=%d (mm1=%d, mm2=%d, mm3=%d, mm5=%d)",
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
	int rate = HFP_RATE, val_on = 1, val_off = 0, noise_db = -18, rc;

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
		speex_preprocess_ctl(br->prep, SPEEX_PREPROCESS_SET_DENOISE, &val_on);
		speex_preprocess_ctl(br->prep, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &noise_db);
		speex_preprocess_ctl(br->prep, SPEEX_PREPROCESS_SET_AGC, &val_off);
		if (br->echo)
			speex_preprocess_ctl(br->prep, SPEEX_PREPROCESS_SET_ECHO_STATE, br->echo);
	}

	br->ref_head = br->ref_tail = br->ref_count = 0;
	memset(br->nlms_w, 0, sizeof(br->nlms_w));
	memset(br->nlms_hist, 0, sizeof(br->nlms_hist));
	br->nlms_pos = 0;
	br->nlms_norm = 0.0f;
	br->bg_ratio_sq = 49.0f; /* 7.0^2 */
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

	LOG("[bridge] started full-duplex INT_BT_SCO audio bridge");
	return 0;
}

void alsa_bridge_stop(struct alsa_bridge *br)
{
	snd_pcm_t **pcms[4];
	bool was_running;

	if (!br)
		return;

	was_running = br->running;
	br->stop_req = true;

	pcms[0] = &br->cap_sco;
	pcms[1] = &br->play_spk;
	pcms[2] = &br->cap_mic;
	pcms[3] = &br->play_sco;
	for (size_t i = 0; i < ARRAY_SIZE(pcms); i++) {
		if (*pcms[i])
			snd_pcm_drop(*pcms[i]);
	}

	if (was_running) {
		pthread_join(br->dl_thread, NULL);
		pthread_join(br->ul_thread, NULL);
		br->running = false;
	}

	for (size_t i = 0; i < ARRAY_SIZE(pcms); i++) {
		if (*pcms[i]) {
			snd_pcm_close(*pcms[i]);
			*pcms[i] = NULL;
		}
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
		LOG("[bridge] stopped full-duplex INT_BT_SCO audio bridge");
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
	bool running;

	if (!br)
		return;
	pthread_mutex_lock(&br->cfg_lock);
	br->cfg.output_mode = mode;
	running = br->running;
	refresh_active_mixers(br);
	pthread_mutex_unlock(&br->cfg_lock);

	if (!running)
		pipewire_set_defaults(mode);
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
