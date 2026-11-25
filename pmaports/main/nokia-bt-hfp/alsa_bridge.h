#ifndef ALSA_BRIDGE_H
#define ALSA_BRIDGE_H

#include <stdbool.h>

enum hfp_output_mode {
	OUTPUT_EARPIECE = 0,
	OUTPUT_SPEAKER,
	OUTPUT_HEADPHONES,
};

enum hfp_mic_source {
	MIC_AMIC1 = 0,
	MIC_AMIC3,
	MIC_DUAL,
};

struct alsa_bridge_cfg {
	enum hfp_output_mode output_mode;
	enum hfp_mic_source mic_source;
	int vgs_level; /* 0..15 */
	int vgm_level; /* 0..15 */
	bool enable_aec;
	bool enable_ns;
	bool muted;
	bool verbose;
};

struct alsa_bridge;

struct alsa_bridge *alsa_bridge_create(const struct alsa_bridge_cfg *cfg);
void alsa_bridge_destroy(struct alsa_bridge *br);

int alsa_bridge_start(struct alsa_bridge *br);
void alsa_bridge_stop(struct alsa_bridge *br);
bool alsa_bridge_is_running(const struct alsa_bridge *br);

void alsa_bridge_set_output_mode(struct alsa_bridge *br, enum hfp_output_mode mode);
void alsa_bridge_set_mic_source(struct alsa_bridge *br, enum hfp_mic_source mic);
void alsa_bridge_set_vgs(struct alsa_bridge *br, int vgs);
void alsa_bridge_set_vgm(struct alsa_bridge *br, int vgm);
void alsa_bridge_set_mute(struct alsa_bridge *br, bool muted);
void alsa_bridge_set_dsp(struct alsa_bridge *br, bool enable_aec, bool enable_ns);
void alsa_bridge_get_cfg(struct alsa_bridge *br, struct alsa_bridge_cfg *out);

#endif /* ALSA_BRIDGE_H */
