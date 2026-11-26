#define _GNU_SOURCE
#include "alsa_bridge.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glib-unix.h>
#include <linux/input.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif
#ifndef BTPROTO_SCO
#define BTPROTO_SCO 2
#endif

struct bt_sockaddr_sco {
	sa_family_t sco_family;
	uint8_t sco_bdaddr[6];
};

#define HFP_HF_UUID "0000111e-0000-1000-8000-00805f9b34fb"
#define PROFILE_OBJ_PATH "/org/nokia/BtHfpProfile"
#define GATT_APP_PATH "/org/nokia/BtHfpGatt"
#define GATT_SVC_PATH "/org/nokia/BtHfpGatt/service0"
#define GATT_CHAR_LEVEL_PATH "/org/nokia/BtHfpGatt/service0/char0"
#define GATT_CHAR_POWER_PATH "/org/nokia/BtHfpGatt/service0/char1"
#define GATT_CHAR_STATUS_PATH "/org/nokia/BtHfpGatt/service0/char2"
#define GATT_BAS_UUID "0000180f-0000-1000-8000-00805f9b34fb"
#define GATT_CHAR_LEVEL_UUID "00002a19-0000-1000-8000-00805f9b34fb"
#define GATT_CHAR_POWER_UUID "00002a1a-0000-1000-8000-00805f9b34fb"
#define GATT_CHAR_STATUS_UUID "00002bed-0000-1000-8000-00805f9b34fb"
#define LOCAL_BT_ALIAS "THE MTRX HANDSET"
#define AT_QUEUE_SIZE 24

#define LOG(fmt, ...) fprintf(stderr, fmt "\n", ##__VA_ARGS__)

static const char *const output_names[] = {
	[OUTPUT_EARPIECE] = "earpiece",
	[OUTPUT_SPEAKER] = "speaker",
	[OUTPUT_HEADPHONES] = "headphones",
};

static const char *const mic_names[] = {
	[MIC_AMIC1] = "amic1",
	[MIC_AMIC3] = "amic3",
	[MIC_DUAL] = "dual",
};

static const char profile_xml[] =
	"<node>"
	"  <interface name='org.bluez.Profile1'>"
	"    <method name='Release'/>"
	"    <method name='NewConnection'>"
	"      <arg type='o' name='device' direction='in'/>"
	"      <arg type='h' name='fd' direction='in'/>"
	"      <arg type='a{sv}' name='fd_properties' direction='in'/>"
	"    </method>"
	"    <method name='RequestDisconnection'>"
	"      <arg type='o' name='device' direction='in'/>"
	"    </method>"
	"  </interface>"
	"  <interface name='org.bluez.Agent1'>"
	"    <method name='Release'/>"
	"    <method name='AuthorizeService'>"
	"      <arg type='o' name='device' direction='in'/>"
	"      <arg type='s' name='uuid' direction='in'/>"
	"    </method>"
	"    <method name='RequestConfirmation'>"
	"      <arg type='o' name='device' direction='in'/>"
	"      <arg type='u' name='passkey' direction='in'/>"
	"    </method>"
	"    <method name='RequestAuthorization'>"
	"      <arg type='o' name='device' direction='in'/>"
	"    </method>"
	"    <method name='Cancel'/>"
	"  </interface>"
	"</node>";

static const char gatt_om_xml[] =
	"<node>"
	"  <interface name='org.freedesktop.DBus.ObjectManager'>"
	"    <method name='GetManagedObjects'>"
	"      <arg type='a{oa{sa{sv}}}' name='objects' direction='out'/>"
	"    </method>"
	"  </interface>"
	"</node>";

static const char gatt_svc_xml[] =
	"<node>"
	"  <interface name='org.bluez.GattService1'>"
	"    <property name='UUID' type='s' access='read'/>"
	"    <property name='Primary' type='b' access='read'/>"
	"  </interface>"
	"</node>";

static const char gatt_char_xml[] =
	"<node>"
	"  <interface name='org.bluez.GattCharacteristic1'>"
	"    <method name='ReadValue'>"
	"      <arg type='a{sv}' name='options' direction='in'/>"
	"      <arg type='ay' name='value' direction='out'/>"
	"    </method>"
	"    <method name='StartNotify'/>"
	"    <method name='StopNotify'/>"
	"    <property name='UUID' type='s' access='read'/>"
	"    <property name='Service' type='o' access='read'/>"
	"    <property name='Flags' type='as' access='read'/>"
	"    <property name='Value' type='ay' access='read'/>"
	"  </interface>"
	"</node>";

struct cind_map {
	int call;
	int callsetup;
	int callheld;
};

struct hfp_app {
	GMainLoop *loop;
	GDBusConnection *sys_bus;
	guint reg_id;
	guint bluez_watch_id;
	guint bluez_prop_sub_id;
	guint bluez_rem_sub_id;
	guint adapter_timer_id;
	int adapter_retries;

	struct alsa_bridge *bridge;
	char ctl_sock_path[256];

	int ctl_listen_fd;
	guint ctl_watch_id;

	int sco_listen_fd;
	guint sco_listen_watch_id;
	int sco_fd;
	guint sco_watch_id;

	int rfcomm_fd;
	guint rfcomm_watch_id;
	char device_path[256];
	char peer_addr[32];
	char rx_buf[2048];
	size_t rx_len;

	char at_queue[AT_QUEUE_SIZE][128];
	int at_head;
	int at_tail;
	bool at_inflight;
	char at_current[128];
	guint at_timeout_id;

	unsigned int ag_features;
	bool slc_ready;
	struct cind_map cind;

	int val_call;
	int val_callsetup;
	int val_callheld;
	char caller_id[64];

	char local_bt_name[64];
	bool bonded;
	char bonded_name[128];
	char bonded_addr[32];
	char bonded_path[256];
	bool pairing_active;

	int jack_fd;
	guint jack_watch_id;
	enum hfp_output_mode saved_output_mode;

	int slide_fd;
	guint slide_watch_id;
	bool slider_open;

	bool ag_hf_ind_battery_enabled;
	bool ag_xapl_supported;
	int bat_percent;
	bool bat_present;
	bool bat_charging;
	bool bat_discharging;
	bool bat_full;
	bool ext_power;
	guint bat_timer_id;

	guint gatt_om_reg_id;
	guint gatt_svc_reg_id;
	guint gatt_char_reg_id[3];
	bool gatt_notify[3];
	bool gatt_registered;
};

static void pump_at_queue(struct hfp_app *app);
static void queue_at(struct hfp_app *app, const char *cmd);

static bool read_sysfs_str(const char *path, char *out, size_t sz)
{
	FILE *f = fopen(path, "r");
	size_t n;

	if (!f)
		return false;
	if (!fgets(out, (int)sz, f)) {
		fclose(f);
		return false;
	}
	fclose(f);
	n = strlen(out);
	while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' '))
		out[--n] = '\0';
	return n > 0;
}

static bool read_power_supply(struct hfp_app *app, bool *changed)
{
	DIR *dir = opendir("/sys/class/power_supply");
	struct dirent *de;
	int new_pct = -1;
	bool new_present = false;
	bool new_charging = false;
	bool new_discharging = false;
	bool new_full = false;
	bool new_ext = false;

	if (changed)
		*changed = false;
	if (!dir)
		return false;

	while ((de = readdir(dir)) != NULL) {
		char path[320];
		char type_buf[64] = { 0 };
		char buf[64] = { 0 };

		if (de->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "/sys/class/power_supply/%s/type",
			 de->d_name);
		read_sysfs_str(path, type_buf, sizeof(type_buf));

		if (type_buf[0] && strcmp(type_buf, "Battery") != 0) {
			snprintf(path, sizeof(path),
				 "/sys/class/power_supply/%s/online", de->d_name);
			if (read_sysfs_str(path, buf, sizeof(buf)) && atoi(buf) == 1)
				new_ext = true;
			continue;
		}

		snprintf(path, sizeof(path), "/sys/class/power_supply/%s/capacity",
			 de->d_name);
		if (new_pct < 0 && read_sysfs_str(path, buf, sizeof(buf))) {
			char *end = NULL;
			long v = strtol(buf, &end, 10);
			if (end != buf) {
				if (v < 0)
					v = 0;
				if (v > 100)
					v = 100;
				new_pct = (int)v;
				new_present = true;

				snprintf(path, sizeof(path),
					 "/sys/class/power_supply/%s/status",
					 de->d_name);
				if (read_sysfs_str(path, buf, sizeof(buf))) {
					if (strcmp(buf, "Charging") == 0) {
						new_charging = true;
						new_ext = true;
					} else if (strcmp(buf, "Full") == 0) {
						new_full = true;
						new_ext = true;
					} else if (strcmp(buf, "Discharging") == 0) {
						new_discharging = true;
					}
				}
			}
		}
	}
	closedir(dir);

	if (new_pct < 0)
		return false;

	if (new_pct != app->bat_percent || new_present != app->bat_present ||
	    new_charging != app->bat_charging ||
	    new_discharging != app->bat_discharging ||
	    new_full != app->bat_full || new_ext != app->ext_power) {
		app->bat_percent = new_pct;
		app->bat_present = new_present;
		app->bat_charging = new_charging;
		app->bat_discharging = new_discharging;
		app->bat_full = new_full;
		app->ext_power = new_ext;
		if (changed)
			*changed = true;
	}
	return true;
}

static size_t build_gatt_char_value(const struct hfp_app *app, int idx,
				    uint8_t *buf, size_t max_sz)
{
	uint8_t pct = (app->bat_percent >= 0) ? (uint8_t)app->bat_percent : 0;

	if (idx == 0 && max_sz >= 1) {
		/* 0x2A19 Battery Level: uint8 0..100 */
		buf[0] = pct;
		return 1;
	}
	if (idx == 1 && max_sz >= 1) {
		/* 0x2A1A Battery Power State: 2 bits per field */
		uint8_t present = app->bat_present ? 3 : 2;
		uint8_t disch = app->bat_discharging ? 3 : 2;
		uint8_t chg = app->bat_charging ? 3 : 2;
		uint8_t lvl = (app->bat_present && pct <= 15) ? 3 : 2;
		buf[0] = (uint8_t)(present | (disch << 2) | (chg << 4) | (lvl << 6));
		return 1;
	}
	if (idx == 2 && max_sz >= 4) {
		/* 0x2BED Battery Level Status: Flags(1) + PowerState(2) + Level(1) */
		uint16_t ps = 0;
		uint16_t wired = app->ext_power ? 1 : 0;
		uint16_t chg_state = app->bat_charging ? 1 :
				     (app->bat_discharging ? 2 : 3);
		uint16_t chg_lvl = (pct <= 10) ? 3 : ((pct <= 20) ? 2 : 1);

		if (app->bat_present)
			ps |= (1U << 0);
		ps |= (wired << 1);
		ps |= (chg_state << 5);
		ps |= (chg_lvl << 7);

		buf[0] = 0x02; /* Battery Level Present */
		buf[1] = (uint8_t)(ps & 0xff);
		buf[2] = (uint8_t)((ps >> 8) & 0xff);
		buf[3] = pct;
		return 4;
	}
	return 0;
}

static void emit_gatt_battery_notifications(struct hfp_app *app)
{
	static const char *const paths[3] = {
		GATT_CHAR_LEVEL_PATH,
		GATT_CHAR_POWER_PATH,
		GATT_CHAR_STATUS_PATH,
	};

	if (!app->sys_bus)
		return;
	for (int i = 0; i < 3; i++) {
		uint8_t val[4];
		size_t len;
		GVariantBuilder changed_props;
		GVariantBuilder inv_props;

		if (!app->gatt_notify[i])
			continue;
		len = build_gatt_char_value(app, i, val, sizeof(val));
		if (len == 0)
			continue;

		g_variant_builder_init(&changed_props, G_VARIANT_TYPE("a{sv}"));
		g_variant_builder_add(
			&changed_props, "{sv}", "Value",
			g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, val, len, 1));
		g_variant_builder_init(&inv_props, G_VARIANT_TYPE("as"));

		g_dbus_connection_emit_signal(
			app->sys_bus, NULL, paths[i],
			"org.freedesktop.DBus.Properties", "PropertiesChanged",
			g_variant_new("(s@a{sv}@as)",
				      "org.bluez.GattCharacteristic1",
				      g_variant_builder_end(&changed_props),
				      g_variant_builder_end(&inv_props)),
			NULL);
	}
}

static void send_hfp_battery(struct hfp_app *app)
{
	char at[48];

	if (app->rfcomm_fd < 0 || !app->slc_ready || app->bat_percent < 0)
		return;
	if (app->ag_hf_ind_battery_enabled) {
		snprintf(at, sizeof(at), "AT+BIEV=2,%d", app->bat_percent);
		queue_at(app, at);
	}
	if (app->ag_xapl_supported) {
		int lvl9 = (app->bat_percent * 9 + 50) / 100;
		int dock = (app->bat_charging || app->ext_power) ? 1 : 0;
		if (lvl9 > 9)
			lvl9 = 9;
		snprintf(at, sizeof(at), "AT+IPHONEACCEV=2,1,%d,2,%d", lvl9, dock);
		queue_at(app, at);
	}
}

static gboolean on_battery_poll(gpointer user_data)
{
	struct hfp_app *app = user_data;
	bool changed = false;

	if (read_power_supply(app, &changed) && changed) {
		LOG("[bat] level=%d%% charging=%d discharging=%d full=%d ext=%d",
		    app->bat_percent, app->bat_charging, app->bat_discharging,
		    app->bat_full, app->ext_power);
		send_hfp_battery(app);
		emit_gatt_battery_notifications(app);
	}
	return G_SOURCE_CONTINUE;
}

static int lookup_name(const char *const *tbl, size_t n, const char *s)
{
	for (size_t i = 0; i < n; i++) {
		if (strcmp(tbl[i], s) == 0)
			return (int)i;
	}
	return -1;
}

static void get_ctl_sock_path(char *out, size_t sz)
{
	const char *xdg = getenv("XDG_RUNTIME_DIR");

	snprintf(out, sz, "%s/nokia-bt-hfp.sock",
		 (xdg && xdg[0]) ? xdg : "/tmp");
}

static void emit_hfp_state(struct hfp_app *app)
{
	const char *state = "idle";

	if (!app->sys_bus)
		return;
	if (app->val_call == 1)
		state = "active";
	else if (app->val_callsetup == 1)
		state = "incoming";
	g_dbus_connection_emit_signal(
		app->sys_bus, NULL, PROFILE_OBJ_PATH,
		"org.nokia.BtHfp", "CallState",
		g_variant_new("(bss)", app->rfcomm_fd >= 0, state,
			      app->caller_id),
		NULL);
}

static void emit_audio_state(struct hfp_app *app)
{
	struct alsa_bridge_cfg cfg;

	if (!app->sys_bus || !app->bridge)
		return;
	alsa_bridge_get_cfg(app->bridge, &cfg);
	g_dbus_connection_emit_signal(
		app->sys_bus, NULL, PROFILE_OBJ_PATH,
		"org.nokia.BtHfp", "AudioState",
		g_variant_new("(siib)", output_names[cfg.output_mode],
			      cfg.vgs_level, cfg.vgm_level, cfg.muted),
		NULL);
}

static void emit_slider_state(struct hfp_app *app)
{
	if (!app->sys_bus)
		return;
	g_dbus_connection_emit_signal(
		app->sys_bus, NULL, PROFILE_OBJ_PATH,
		"org.nokia.BtHfp", "SliderState",
		g_variant_new("(b)", app->slider_open),
		NULL);
}

static void emit_bond_state(struct hfp_app *app)
{
	if (!app->sys_bus)
		return;
	g_dbus_connection_emit_signal(
		app->sys_bus, NULL, PROFILE_OBJ_PATH,
		"org.nokia.BtHfp", "BondState",
		g_variant_new("(bssb)", app->bonded, app->bonded_name,
			      app->bonded_addr, app->pairing_active),
		NULL);
}

static void emit_pairing_success(struct hfp_app *app, const char *name,
				 const char *addr)
{
	if (!app->sys_bus)
		return;
	g_dbus_connection_emit_signal(
		app->sys_bus, NULL, PROFILE_OBJ_PATH,
		"org.nokia.BtHfp", "PairingSuccess",
		g_variant_new("(ss)", name ? name : "", addr ? addr : ""),
		NULL);
}

static void run_call_hook(struct hfp_app *app, const char *event)
{
	const char *home = getenv("HOME");
	char hook[256];
	struct stat st;

	emit_hfp_state(app);

	if (home && home[0]) {
		snprintf(hook, sizeof(hook), "%s/.config/nokia-bt-hfp/hook", home);
		if (stat(hook, &st) == 0 && (st.st_mode & S_IXUSR)) {
			char *argv[] = { hook, (char *)event, app->peer_addr,
					 app->caller_id, NULL };
			g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
				      NULL, NULL, NULL, NULL);
		}
	}
}

static gboolean on_at_timeout(gpointer user_data)
{
	struct hfp_app *app = user_data;

	app->at_timeout_id = 0;
	if (app->at_inflight) {
		LOG("[hfp] AT timeout waiting for response to: %s", app->at_current);
		app->at_inflight = false;
		pump_at_queue(app);
	}
	return G_SOURCE_REMOVE;
}

static void pump_at_queue(struct hfp_app *app)
{
	char wire[140];
	size_t len;
	ssize_t w;

	if (app->rfcomm_fd < 0 || app->at_inflight || app->at_head == app->at_tail)
		return;

	snprintf(app->at_current, sizeof(app->at_current), "%s",
		 app->at_queue[app->at_head]);
	app->at_head = (app->at_head + 1) % AT_QUEUE_SIZE;

	len = (size_t)snprintf(wire, sizeof(wire), "%s\r", app->at_current);
	w = write(app->rfcomm_fd, wire, len);
	if (w != (ssize_t)len)
		LOG("[hfp] RFCOMM write '%s' failed: %s", app->at_current,
		    w < 0 ? strerror(errno) : "short write");

	/* Arm the timeout even on failure so the queue keeps draining */
	app->at_inflight = true;
	if (app->at_timeout_id)
		g_source_remove(app->at_timeout_id);
	app->at_timeout_id = g_timeout_add_seconds(3, on_at_timeout, app);
	LOG("[hfp TX] %s", app->at_current);
}

static void queue_at(struct hfp_app *app, const char *cmd)
{
	int next = (app->at_tail + 1) % AT_QUEUE_SIZE;

	if (next == app->at_head) {
		LOG("[hfp] AT queue full, dropping '%s'", cmd);
		return;
	}
	snprintf(app->at_queue[app->at_tail],
		 sizeof(app->at_queue[app->at_tail]), "%s", cmd);
	app->at_tail = next;
	pump_at_queue(app);
}

static void parse_cind_map(struct hfp_app *app, const char *line)
{
	static const struct {
		const char *name;
		size_t off;
	} names[] = {
		{ "call", offsetof(struct cind_map, call) },
		{ "callsetup", offsetof(struct cind_map, callsetup) },
		{ "call_setup", offsetof(struct cind_map, callsetup) },
		{ "callheld", offsetof(struct cind_map, callheld) },
	};
	const char *p = line;
	int idx = 1;

	memset(&app->cind, 0, sizeof(app->cind));
	while ((p = strchr(p, '(')) != NULL) {
		const char *q1 = strchr(p, '"');
		const char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
		size_t len;

		if (!q1 || !q2)
			break;
		len = (size_t)(q2 - q1 - 1);
		for (size_t i = 0; i < G_N_ELEMENTS(names); i++) {
			if (strlen(names[i].name) == len &&
			    strncmp(q1 + 1, names[i].name, len) == 0)
				*(int *)((char *)&app->cind + names[i].off) = idx;
		}
		idx++;
		p = q2 + 1;
	}
	LOG("[hfp] CIND map: call=%d callsetup=%d callheld=%d",
	    app->cind.call, app->cind.callsetup, app->cind.callheld);
}

static void apply_indicator(struct hfp_app *app, int idx, int val)
{
	if (idx <= 0)
		return;
	if (idx == app->cind.call) {
		int prev = app->val_call;
		app->val_call = val;
		if (val == 1 && prev == 0)
			run_call_hook(app, "active");
		else if (val == 0 && prev == 1) {
			app->caller_id[0] = '\0';
			run_call_hook(app, "ended");
		}
	} else if (idx == app->cind.callsetup) {
		int prev = app->val_callsetup;
		app->val_callsetup = val;
		if (val == 1 && prev != 1)
			run_call_hook(app, "incoming");
		else if (val == 0 && prev == 1 && app->val_call == 0) {
			app->caller_id[0] = '\0';
			run_call_hook(app, "ended");
		}
	} else if (idx == app->cind.callheld) {
		app->val_callheld = val;
	}
}

static void handle_at_line(struct hfp_app *app, const char *line)
{
	if (!line[0])
		return;
	LOG("[hfp RX] %s", line);

	if (strncmp(line, "+BRSF:", 6) == 0) {
		app->ag_features = (unsigned int)strtoul(line + 6, NULL, 10);
		LOG("[hfp] AG features=0x%x (codec_neg=%d hf_ind=%d)",
		    app->ag_features,
		    (app->ag_features & (1U << 9)) ? 1 : 0,
		    (app->ag_features & (1U << 10)) ? 1 : 0);
	} else if (strncmp(line, "+BIND:", 6) == 0) {
		int ind = 0, state = 0;
		if (sscanf(line + 6, " %d , %d", &ind, &state) == 2 && ind == 2) {
			app->ag_hf_ind_battery_enabled = (state == 1);
			LOG("[hfp] HF indicator battery %s",
			    app->ag_hf_ind_battery_enabled ? "enabled" : "disabled");
			if (app->ag_hf_ind_battery_enabled && app->slc_ready &&
			    app->bat_percent >= 0) {
				char at[32];
				snprintf(at, sizeof(at), "AT+BIEV=2,%d", app->bat_percent);
				queue_at(app, at);
			}
		}
	} else if (strncmp(line, "+XAPL=", 6) == 0) {
		app->ag_xapl_supported = true;
	} else if (strncmp(line, "+CIND:", 6) == 0) {
		const char *rest = line + 6;
		while (*rest == ' ')
			rest++;
		if (strchr(rest, '(')) {
			parse_cind_map(app, rest);
		} else {
			int idx = 1;
			const char *p = rest;
			char *end;

			while (*p) {
				long v = strtol(p, &end, 10);
				if (end == p)
					break;
				apply_indicator(app, idx++, (int)v);
				p = end;
				while (*p == ',' || *p == ' ')
					p++;
			}
		}
	} else if (strncmp(line, "+CIEV:", 6) == 0) {
		int idx = 0, val = 0;
		if (sscanf(line + 6, " %d , %d", &idx, &val) == 2)
			apply_indicator(app, idx, val);
	} else if (strncmp(line, "+BCS:", 5) == 0) {
		/* Only CVSD (1) is usable over INT_BT_SCO 8 kHz; re-advertise otherwise */
		queue_at(app, atoi(line + 5) == 1 ? "AT+BCS=1" : "AT+BAC=1");
	} else if (strncmp(line, "+CLIP:", 6) == 0) {
		const char *q1 = strchr(line + 6, '"');
		const char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
		if (q1 && q2 && q2 > q1 + 1) {
			size_t n = (size_t)(q2 - q1 - 1);
			if (n >= sizeof(app->caller_id))
				n = sizeof(app->caller_id) - 1;
			memcpy(app->caller_id, q1 + 1, n);
			app->caller_id[n] = '\0';
			emit_hfp_state(app);
		}
	} else if (strncmp(line, "+VGS:", 5) == 0) {
		alsa_bridge_set_vgs(app->bridge, atoi(line + 5));
		emit_audio_state(app);
	} else if (strncmp(line, "+VGM:", 5) == 0) {
		alsa_bridge_set_vgm(app->bridge, atoi(line + 5));
		emit_audio_state(app);
	} else if (strcmp(line, "RING") == 0) {
		if (app->val_call == 0 && app->val_callsetup == 0)
			app->val_callsetup = 1;
		run_call_hook(app, "ring");
	} else if (strcmp(line, "OK") == 0 || strcmp(line, "ERROR") == 0 ||
		   strncmp(line, "+CME ERROR:", 11) == 0) {
		bool ok = (strcmp(line, "OK") == 0);

		if (app->at_timeout_id) {
			g_source_remove(app->at_timeout_id);
			app->at_timeout_id = 0;
		}
		if (app->at_inflight) {
			if (strncmp(app->at_current, "AT+BRSF=", 8) == 0 && ok) {
				struct alsa_bridge_cfg cfg;
				char at[32];

				alsa_bridge_get_cfg(app->bridge, &cfg);
				if (app->ag_features & (1U << 9))
					queue_at(app, "AT+BAC=1");
				queue_at(app, "AT+CIND=?");
				queue_at(app, "AT+CIND?");
				queue_at(app, "AT+CMER=3,0,0,1");
				if (app->ag_features & (1U << 0))
					queue_at(app, "AT+CHLD=?");
				if (app->ag_features & (1U << 10)) {
					queue_at(app, "AT+BIND=2");
					queue_at(app, "AT+BIND=?");
					queue_at(app, "AT+BIND?");
				}
				queue_at(app, "AT+CLIP=1");
				queue_at(app, "AT+CMEE=1");
				snprintf(at, sizeof(at), "AT+VGS=%d", cfg.vgs_level);
				queue_at(app, at);
				snprintf(at, sizeof(at), "AT+VGM=%d", cfg.vgm_level);
				queue_at(app, at);
			} else if (strncmp(app->at_current, "AT+CMER=", 8) == 0 && ok) {
				app->slc_ready = true;
				LOG("[hfp] SLC established");
				emit_hfp_state(app);
				send_hfp_battery(app);
				queue_at(app, "AT+XAPL=0000-0000-0100,2");
			} else if (strncmp(app->at_current, "AT+XAPL=", 8) == 0 && ok) {
				app->ag_xapl_supported = true;
				send_hfp_battery(app);
			}
			app->at_inflight = false;
			pump_at_queue(app);
		}
	}
}

static guint watch_fd(int fd, GIOCondition cond, GIOFunc cb, gpointer data)
{
	GIOChannel *ch = g_io_channel_unix_new(fd);
	guint id;

	g_io_channel_set_close_on_unref(ch, FALSE);
	id = g_io_add_watch(ch, cond, cb, data);
	g_io_channel_unref(ch);
	return id;
}

static void close_sco(struct hfp_app *app)
{
	if (app->sco_watch_id) {
		g_source_remove(app->sco_watch_id);
		app->sco_watch_id = 0;
	}
	if (app->sco_fd >= 0) {
		close(app->sco_fd);
		app->sco_fd = -1;
	}
	alsa_bridge_stop(app->bridge);
}

static void close_rfcomm(struct hfp_app *app)
{
	close_sco(app);
	if (app->at_timeout_id) {
		g_source_remove(app->at_timeout_id);
		app->at_timeout_id = 0;
	}
	if (app->rfcomm_watch_id) {
		g_source_remove(app->rfcomm_watch_id);
		app->rfcomm_watch_id = 0;
	}
	if (app->rfcomm_fd >= 0) {
		close(app->rfcomm_fd);
		app->rfcomm_fd = -1;
	}
	app->rx_len = 0;
	app->at_head = app->at_tail = 0;
	app->at_inflight = false;
	app->slc_ready = false;
	app->ag_hf_ind_battery_enabled = false;
	app->ag_xapl_supported = false;
	app->val_call = app->val_callsetup = app->val_callheld = 0;
	app->device_path[0] = app->peer_addr[0] = app->caller_id[0] = '\0';
	emit_hfp_state(app);
}

static gboolean on_rfcomm_io(GIOChannel *src, GIOCondition cond, gpointer user_data)
{
	struct hfp_app *app = user_data;
	int fd = g_io_channel_unix_get_fd(src);

	if (cond & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
		LOG("[hfp] RFCOMM disconnected");
		close_rfcomm(app);
		return G_SOURCE_REMOVE;
	}

	if (cond & G_IO_IN) {
		ssize_t n = read(fd, app->rx_buf + app->rx_len,
				 sizeof(app->rx_buf) - 1 - app->rx_len);
		if (n <= 0) {
			if (n < 0 && (errno == EAGAIN || errno == EINTR))
				return G_SOURCE_CONTINUE;
			LOG("[hfp] RFCOMM EOF/read err: %s",
			    n == 0 ? "EOF" : strerror(errno));
			close_rfcomm(app);
			return G_SOURCE_REMOVE;
		}
		app->rx_len += (size_t)n;
		app->rx_buf[app->rx_len] = '\0';

		size_t start = 0;
		for (size_t i = 0; i < app->rx_len; i++) {
			if (app->rx_buf[i] == '\r' || app->rx_buf[i] == '\n') {
				app->rx_buf[i] = '\0';
				if (i > start)
					handle_at_line(app, app->rx_buf + start);
				start = i + 1;
			}
		}
		if (start > 0) {
			size_t rem = app->rx_len - start;
			memmove(app->rx_buf, app->rx_buf + start, rem);
			app->rx_len = rem;
		} else if (app->rx_len >= sizeof(app->rx_buf) - 1) {
			app->rx_len = 0;
		}
	}
	return G_SOURCE_CONTINUE;
}

static gboolean on_sco_conn_io(GIOChannel *src, GIOCondition cond, gpointer user_data)
{
	struct hfp_app *app = user_data;
	int fd = g_io_channel_unix_get_fd(src);

	if (cond & G_IO_IN) {
		uint8_t discard[256];
		ssize_t n = recv(fd, discard, sizeof(discard), MSG_DONTWAIT);
		if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
			LOG("[sco] connection closed");
			close_sco(app);
			return G_SOURCE_REMOVE;
		}
	}
	if (cond & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
		LOG("[sco] connection HUP/ERR");
		close_sco(app);
		return G_SOURCE_REMOVE;
	}
	return G_SOURCE_CONTINUE;
}

static void attach_sco_fd(struct hfp_app *app, int fd)
{
	close_sco(app);
	app->sco_fd = fd;
	app->sco_watch_id = watch_fd(
		fd, G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL, on_sco_conn_io, app);
	if (alsa_bridge_start(app->bridge) < 0)
		LOG("[sco] failed to start ALSA INT_BT_SCO bridge");
}

static gboolean on_sco_accept(GIOChannel *src, GIOCondition cond, gpointer user_data)
{
	struct hfp_app *app = user_data;
	struct bt_sockaddr_sco peer;
	socklen_t plen = sizeof(peer);
	int cfd;

	if (cond & (G_IO_ERR | G_IO_NVAL)) {
		app->sco_listen_watch_id = 0;
		return G_SOURCE_REMOVE;
	}

	cfd = accept(g_io_channel_unix_get_fd(src), (struct sockaddr *)&peer, &plen);
	if (cfd < 0) {
		if (errno != EAGAIN && errno != EINTR)
			LOG("[sco] accept failed: %s", strerror(errno));
		return G_SOURCE_CONTINUE;
	}

	LOG("[sco] accepted incoming eSCO from %02X:%02X:%02X:%02X:%02X:%02X (fd=%d)",
	    peer.sco_bdaddr[5], peer.sco_bdaddr[4], peer.sco_bdaddr[3],
	    peer.sco_bdaddr[2], peer.sco_bdaddr[1], peer.sco_bdaddr[0], cfd);
	/* Keep SCO fd open without writing to avoid btqcomsmd -EILSEQ (-84) */
	attach_sco_fd(app, cfd);
	return G_SOURCE_CONTINUE;
}

static int setup_sco_listener(struct hfp_app *app)
{
	struct bt_sockaddr_sco addr = { .sco_family = AF_BLUETOOTH };
	int fd = socket(AF_BLUETOOTH, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC,
			BTPROTO_SCO);

	if (fd < 0) {
		LOG("[sco] socket(BTPROTO_SCO) failed: %s", strerror(errno));
		return -errno;
	}
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
	    listen(fd, 4) < 0) {
		LOG("[sco] bind/listen failed: %s", strerror(errno));
		close(fd);
		return -errno;
	}

	app->sco_listen_fd = fd;
	app->sco_listen_watch_id =
		watch_fd(fd, G_IO_IN | G_IO_ERR | G_IO_NVAL, on_sco_accept, app);
	LOG("[sco] listening on BTPROTO_SCO");
	return 0;
}

static void extract_mac_from_dev_path(const char *path, char *out, size_t sz)
{
	const char *p = strstr(path, "dev_");
	size_t i = 0;

	if (!p) {
		snprintf(out, sz, "unknown");
		return;
	}
	p += 4;
	while (*p && i + 1 < sz) {
		out[i++] = (*p == '_') ? ':' : *p;
		p++;
	}
	out[i] = '\0';
}

static void handle_profile_method(GDBusConnection *conn, const gchar *sender,
				  const gchar *obj_path, const gchar *iface_name,
				  const gchar *method_name, GVariant *parameters,
				  GDBusMethodInvocation *invocation,
				  gpointer user_data)
{
	struct hfp_app *app = user_data;
	(void)conn;
	(void)sender;
	(void)obj_path;
	(void)iface_name;

	if (strcmp(iface_name, "org.bluez.Agent1") == 0) {
		const gchar *dev = NULL;
		if (strcmp(method_name, "AuthorizeService") == 0) {
			g_variant_get(parameters, "(&os)", &dev, NULL);
		} else if (strcmp(method_name, "RequestConfirmation") == 0) {
			g_variant_get(parameters, "(&ou)", &dev, NULL);
		} else if (strcmp(method_name, "RequestAuthorization") == 0) {
			g_variant_get(parameters, "(&o)", &dev);
		} else if (strcmp(method_name, "Release") == 0 ||
			   strcmp(method_name, "Cancel") == 0) {
			g_dbus_method_invocation_return_value(invocation, NULL);
			return;
		}
		if (dev && dev[0]) {
			GVariant *r = g_dbus_connection_call_sync(
				app->sys_bus, "org.bluez", dev,
				"org.freedesktop.DBus.Properties", "Set",
				g_variant_new("(ssv)", "org.bluez.Device1",
					      "Trusted", g_variant_new_boolean(TRUE)),
				NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
			if (r)
				g_variant_unref(r);
		}
		LOG("[bluez] Agent1.%s auto-accepted (%s)", method_name, dev ? dev : "-");
		g_dbus_method_invocation_return_value(invocation, NULL);
		return;
	}

	if (strcmp(method_name, "Release") == 0) {
		LOG("[bluez] Profile1.Release called");
		close_rfcomm(app);
		g_dbus_method_invocation_return_value(invocation, NULL);
	} else if (strcmp(method_name, "RequestDisconnection") == 0) {
		const gchar *dev = NULL;
		g_variant_get(parameters, "(&o)", &dev);
		LOG("[bluez] RequestDisconnection(%s)", dev ? dev : "");
		close_rfcomm(app);
		g_dbus_method_invocation_return_value(invocation, NULL);
	} else if (strcmp(method_name, "NewConnection") == 0) {
		const gchar *dev = NULL;
		gint32 fd_idx = -1;
		GVariant *props = NULL;
		GDBusMessage *msg = g_dbus_method_invocation_get_message(invocation);
		GUnixFDList *fd_list = g_dbus_message_get_unix_fd_list(msg);
		GError *err = NULL;
		int fd, flags;

		g_variant_get(parameters, "(&oh@a{sv})", &dev, &fd_idx, &props);
		if (props)
			g_variant_unref(props);

		if (!fd_list) {
			g_dbus_method_invocation_return_error(
				invocation, G_IO_ERROR, G_IO_ERROR_FAILED,
				"Missing FD list");
			return;
		}
		fd = g_unix_fd_list_get(fd_list, fd_idx, &err);
		if (fd < 0) {
			g_dbus_method_invocation_return_gerror(invocation, err);
			g_error_free(err);
			return;
		}

		close_rfcomm(app);
		app->rfcomm_fd = fd;
		snprintf(app->device_path, sizeof(app->device_path), "%s",
			 dev ? dev : "");
		extract_mac_from_dev_path(app->device_path, app->peer_addr,
					  sizeof(app->peer_addr));

		if ((flags = fcntl(fd, F_GETFL, 0)) >= 0)
			fcntl(fd, F_SETFL, flags | O_NONBLOCK);

		app->rfcomm_watch_id = watch_fd(
			fd, G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
			on_rfcomm_io, app);

		/* Auto-trust peer device so future RFCOMM/AVRCP connections never fail authorization */
		if (app->device_path[0]) {
			GVariant *r = g_dbus_connection_call_sync(
				app->sys_bus, "org.bluez", app->device_path,
				"org.freedesktop.DBus.Properties", "Set",
				g_variant_new("(ssv)", "org.bluez.Device1",
					      "Trusted", g_variant_new_boolean(TRUE)),
				NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
			if (r)
				g_variant_unref(r);
			if (!app->bonded || strcmp(app->bonded_path, app->device_path) != 0) {
				char dev_name[128];
				char dev_addr[32];

				app->bonded = true;
				snprintf(app->bonded_path, sizeof(app->bonded_path),
					 "%s", app->device_path);
				snprintf(dev_name, sizeof(dev_name), "%s", app->peer_addr);
				snprintf(dev_addr, sizeof(dev_addr), "%s", app->peer_addr);
				{
					GVariant *res = g_dbus_connection_call_sync(
						app->sys_bus, "org.bluez", app->device_path,
						"org.freedesktop.DBus.Properties", "GetAll",
						g_variant_new("(s)", "org.bluez.Device1"),
						G_VARIANT_TYPE("(a{sv})"),
						G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
					if (res) {
						GVariant *dprops = NULL;
						g_variant_get(res, "(@a{sv})", &dprops);
						if (dprops) {
							GVariant *v = g_variant_lookup_value(
								dprops, "Alias", G_VARIANT_TYPE_STRING);
							if (!v)
								v = g_variant_lookup_value(
									dprops, "Name", G_VARIANT_TYPE_STRING);
							if (v) {
								snprintf(dev_name, sizeof(dev_name), "%s",
									 g_variant_get_string(v, NULL));
								g_variant_unref(v);
							}
							g_variant_unref(dprops);
						}
						g_variant_unref(res);
					}
				}
				snprintf(app->bonded_name, sizeof(app->bonded_name), "%s", dev_name);
				snprintf(app->bonded_addr, sizeof(app->bonded_addr), "%s", dev_addr);
				emit_bond_state(app);
			}
		}

		LOG("[bluez] NewConnection dev=%s peer=%s fd=%d",
		    app->device_path, app->peer_addr, fd);
		g_dbus_method_invocation_return_value(invocation, NULL);

		/* Initiate HFP 1.7 SLC: bits 0|1|2|4|5|7|8 = 439 (includes HF Indicators) */
		emit_hfp_state(app);
		emit_audio_state(app);
		queue_at(app, "AT+BRSF=439");
	}
}

static void register_bluez_profile(struct hfp_app *app)
{
	GVariantBuilder b;
	GVariant *res;
	GError *err = NULL;

	g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&b, "{sv}", "Name", g_variant_new_string("Handsfree"));
	g_variant_builder_add(&b, "{sv}", "AutoConnect", g_variant_new_boolean(TRUE));
	g_variant_builder_add(&b, "{sv}", "RequireAuthentication", g_variant_new_boolean(FALSE));
	g_variant_builder_add(&b, "{sv}", "RequireAuthorization", g_variant_new_boolean(FALSE));
	g_variant_builder_add(&b, "{sv}", "Version", g_variant_new_uint16(0x0107));
	g_variant_builder_add(&b, "{sv}", "Features", g_variant_new_uint16(0x0017));

	res = g_dbus_connection_call_sync(
		app->sys_bus, "org.bluez", "/org/bluez",
		"org.bluez.ProfileManager1", "RegisterProfile",
		g_variant_new("(os@a{sv})", PROFILE_OBJ_PATH, HFP_HF_UUID,
			      g_variant_builder_end(&b)),
		NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &err);
	if (!res) {
		LOG("[bluez] RegisterProfile failed (is WirePlumber hfp_hf disabled?): %s",
		    err ? err->message : "unknown");
		g_clear_error(&err);
	} else {
		g_variant_unref(res);
		LOG("[bluez] Registered HFP-HF profile (%s)", HFP_HF_UUID);
	}
}

static bool set_adapter_prop(struct hfp_app *app, const char *prop, GVariant *val)
{
	GError *err = NULL;
	GVariant *res = g_dbus_connection_call_sync(
		app->sys_bus, "org.bluez", "/org/bluez/hci0",
		"org.freedesktop.DBus.Properties", "Set",
		g_variant_new("(ssv)", "org.bluez.Adapter1", prop, val), NULL,
		G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &err);

	if (!res) {
		g_clear_error(&err);
		return false;
	}
	g_variant_unref(res);
	return true;
}

static void configure_bluez_agent(struct hfp_app *app)
{
	GError *err = NULL;
	GVariant *res = g_dbus_connection_call_sync(
		app->sys_bus, "org.bluez", "/org/bluez",
		"org.bluez.AgentManager1", "RegisterAgent",
		g_variant_new("(os)", PROFILE_OBJ_PATH, "NoInputNoOutput"),
		NULL, G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &err);
	if (res) {
		g_variant_unref(res);
		res = g_dbus_connection_call_sync(
			app->sys_bus, "org.bluez", "/org/bluez",
			"org.bluez.AgentManager1", "RequestDefaultAgent",
			g_variant_new("(o)", PROFILE_OBJ_PATH),
			NULL, G_DBUS_CALL_FLAGS_NONE, 3000, NULL, NULL);
		if (res)
			g_variant_unref(res);
		LOG("[bluez] Registered default NoInputNoOutput pairing/authorization agent");
	} else {
		g_clear_error(&err);
	}
}

static bool dict_lookup_str(GVariant *dict, const char *key, char *out, size_t sz)
{
	GVariant *v = g_variant_lookup_value(dict, key, G_VARIANT_TYPE_STRING);

	if (!v)
		return false;
	snprintf(out, sz, "%s", g_variant_get_string(v, NULL));
	g_variant_unref(v);
	return out[0] != '\0';
}

static bool dict_lookup_bool(GVariant *dict, const char *key, bool *out)
{
	GVariant *v = g_variant_lookup_value(dict, key, G_VARIANT_TYPE_BOOLEAN);

	if (!v)
		return false;
	*out = g_variant_get_boolean(v);
	g_variant_unref(v);
	return true;
}

static void trust_device(struct hfp_app *app, const char *dev_path)
{
	GVariant *r;

	if (!app->sys_bus || !dev_path || !dev_path[0])
		return;
	r = g_dbus_connection_call_sync(
		app->sys_bus, "org.bluez", dev_path,
		"org.freedesktop.DBus.Properties", "Set",
		g_variant_new("(ssv)", "org.bluez.Device1",
			      "Trusted", g_variant_new_boolean(TRUE)),
		NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
	if (r)
		g_variant_unref(r);
}

static void fetch_device_info(struct hfp_app *app, const char *dev_path,
			      char *name_out, size_t name_sz,
			      char *addr_out, size_t addr_sz)
{
	GVariant *res, *props = NULL;

	name_out[0] = '\0';
	extract_mac_from_dev_path(dev_path, addr_out, addr_sz);

	if (!app->sys_bus || !dev_path || !dev_path[0])
		return;
	res = g_dbus_connection_call_sync(
		app->sys_bus, "org.bluez", dev_path,
		"org.freedesktop.DBus.Properties", "GetAll",
		g_variant_new("(s)", "org.bluez.Device1"),
		G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, 2000,
		NULL, NULL);
	if (!res)
		return;
	g_variant_get(res, "(@a{sv})", &props);
	if (props) {
		dict_lookup_str(props, "Address", addr_out, addr_sz);
		if (!dict_lookup_str(props, "Alias", name_out, name_sz))
			dict_lookup_str(props, "Name", name_out, name_sz);
		g_variant_unref(props);
	}
	g_variant_unref(res);
	if (!name_out[0])
		snprintf(name_out, name_sz, "%s", addr_out);
}

static void set_pairing_mode(struct hfp_app *app, bool enable)
{
	if (!app->sys_bus)
		return;
	if (enable) {
		set_adapter_prop(app, "PairableTimeout", g_variant_new_uint32(120));
		set_adapter_prop(app, "DiscoverableTimeout", g_variant_new_uint32(120));
		set_adapter_prop(app, "Pairable", g_variant_new_boolean(TRUE));
		set_adapter_prop(app, "Discoverable", g_variant_new_boolean(TRUE));
		app->pairing_active = true;
		LOG("[bluez] Pairing discovery enabled (120s timeout)");
	} else {
		set_adapter_prop(app, "Discoverable", g_variant_new_boolean(FALSE));
		set_adapter_prop(app, "Pairable", g_variant_new_boolean(FALSE));
		app->pairing_active = false;
		LOG("[bluez] Pairing discovery disabled");
	}
	emit_bond_state(app);
}

static void scan_bluez_devices(struct hfp_app *app)
{
	GError *err = NULL;
	GVariant *res;
	GVariantIter *obj_iter = NULL;
	const gchar *obj_path = NULL;
	GVariant *ifaces = NULL;
	bool found_bonded = false;
	char found_name[128] = { 0 };
	char found_addr[32] = { 0 };
	char found_path[256] = { 0 };

	if (!app->sys_bus)
		return;
	res = g_dbus_connection_call_sync(
		app->sys_bus, "org.bluez", "/",
		"org.freedesktop.DBus.ObjectManager", "GetManagedObjects",
		NULL, G_VARIANT_TYPE("(a{oa{sa{sv}}})"),
		G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &err);
	if (!res) {
		g_clear_error(&err);
		return;
	}

	g_variant_get(res, "(a{oa{sa{sv}}})", &obj_iter);
	while (g_variant_iter_loop(obj_iter, "{&o@a{sa{sv}}}", &obj_path, &ifaces)) {
		if (strcmp(obj_path, "/org/bluez/hci0") == 0) {
			GVariant *aprops = g_variant_lookup_value(
				ifaces, "org.bluez.Adapter1", G_VARIANT_TYPE("a{sv}"));
			if (aprops) {
				bool disc = false;
				dict_lookup_str(aprops, "Alias", app->local_bt_name,
						sizeof(app->local_bt_name));
				if (dict_lookup_bool(aprops, "Discoverable", &disc))
					app->pairing_active = disc;
				g_variant_unref(aprops);
			}
		} else if (strstr(obj_path, "/org/bluez/hci0/dev_") != NULL) {
			GVariant *dprops = g_variant_lookup_value(
				ifaces, "org.bluez.Device1", G_VARIANT_TYPE("a{sv}"));
			if (dprops) {
				bool paired = false, trusted = false;
				dict_lookup_bool(dprops, "Paired", &paired);
				dict_lookup_bool(dprops, "Trusted", &trusted);
				if (paired && !trusted)
					trust_device(app, obj_path);
				if (paired && (!found_bonded ||
					       (app->device_path[0] &&
						strcmp(obj_path, app->device_path) == 0))) {
					found_bonded = true;
					snprintf(found_path, sizeof(found_path), "%s", obj_path);
					extract_mac_from_dev_path(obj_path, found_addr,
								  sizeof(found_addr));
					dict_lookup_str(dprops, "Address", found_addr,
							sizeof(found_addr));
					found_name[0] = '\0';
					if (!dict_lookup_str(dprops, "Alias", found_name,
							     sizeof(found_name)))
						dict_lookup_str(dprops, "Name", found_name,
								sizeof(found_name));
					if (!found_name[0])
						snprintf(found_name, sizeof(found_name),
							 "%s", found_addr);
				}
				g_variant_unref(dprops);
			}
		}
	}
	g_variant_iter_free(obj_iter);
	g_variant_unref(res);

	app->bonded = found_bonded;
	snprintf(app->bonded_path, sizeof(app->bonded_path), "%s", found_path);
	snprintf(app->bonded_name, sizeof(app->bonded_name), "%s", found_name);
	snprintf(app->bonded_addr, sizeof(app->bonded_addr), "%s", found_addr);
	emit_bond_state(app);
}

static void unpair_all_devices(struct hfp_app *app)
{
	GError *err = NULL;
	GVariant *res;
	GVariantIter *obj_iter = NULL;
	const gchar *obj_path = NULL;
	GVariant *ifaces = NULL;
	char paths[16][256];
	size_t npaths = 0;

	close_rfcomm(app);

	if (app->sys_bus) {
		res = g_dbus_connection_call_sync(
			app->sys_bus, "org.bluez", "/",
			"org.freedesktop.DBus.ObjectManager", "GetManagedObjects",
			NULL, G_VARIANT_TYPE("(a{oa{sa{sv}}})"),
			G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &err);
		if (res) {
			g_variant_get(res, "(a{oa{sa{sv}}})", &obj_iter);
			while (g_variant_iter_loop(obj_iter, "{&o@a{sa{sv}}}",
						   &obj_path, &ifaces)) {
				if (strstr(obj_path, "/org/bluez/hci0/dev_") != NULL &&
				    npaths < G_N_ELEMENTS(paths)) {
					snprintf(paths[npaths], sizeof(paths[npaths]),
						 "%s", obj_path);
					npaths++;
				}
			}
			g_variant_iter_free(obj_iter);
			g_variant_unref(res);
		} else {
			g_clear_error(&err);
		}

		for (size_t i = 0; i < npaths; i++) {
			GVariant *r = g_dbus_connection_call_sync(
				app->sys_bus, "org.bluez", "/org/bluez/hci0",
				"org.bluez.Adapter1", "RemoveDevice",
				g_variant_new("(o)", paths[i]),
				NULL, G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &err);
			if (r) {
				LOG("[bluez] Removed device %s", paths[i]);
				g_variant_unref(r);
			} else {
				LOG("[bluez] RemoveDevice(%s) failed: %s", paths[i],
				    err ? err->message : "unknown");
				g_clear_error(&err);
			}
		}
	}

	app->bonded = false;
	app->bonded_path[0] = '\0';
	app->bonded_name[0] = '\0';
	app->bonded_addr[0] = '\0';
	emit_bond_state(app);
}

static void on_bluez_properties_changed(GDBusConnection *conn, const gchar *sender,
					const gchar *obj_path, const gchar *iface_name,
					const gchar *sig_name, GVariant *parameters,
					gpointer user_data)
{
	struct hfp_app *app = user_data;
	const gchar *changed_iface = NULL;
	GVariant *changed = NULL;

	(void)conn;
	(void)sender;
	(void)iface_name;
	(void)sig_name;

	g_variant_get(parameters, "(&s@a{sv}@as)", &changed_iface, &changed, NULL);
	if (!changed)
		return;

	if (strcmp(changed_iface, "org.bluez.Adapter1") == 0 &&
	    strcmp(obj_path, "/org/bluez/hci0") == 0) {
		bool disc = false;
		bool notify = false;

		if (dict_lookup_bool(changed, "Discoverable", &disc)) {
			app->pairing_active = disc;
			notify = true;
		}
		if (dict_lookup_str(changed, "Alias", app->local_bt_name,
				    sizeof(app->local_bt_name)))
			notify = true;
		if (notify)
			emit_bond_state(app);
	} else if (strcmp(changed_iface, "org.bluez.Device1") == 0 &&
		   strstr(obj_path, "/org/bluez/hci0/dev_") != NULL) {
		bool paired = false;

		if (dict_lookup_bool(changed, "Paired", &paired)) {
			if (paired) {
				char dev_name[128];
				char dev_addr[32];

				trust_device(app, obj_path);
				fetch_device_info(app, obj_path, dev_name,
						  sizeof(dev_name), dev_addr,
						  sizeof(dev_addr));
				app->bonded = true;
				snprintf(app->bonded_path, sizeof(app->bonded_path),
					 "%s", obj_path);
				snprintf(app->bonded_name, sizeof(app->bonded_name),
					 "%s", dev_name);
				snprintf(app->bonded_addr, sizeof(app->bonded_addr),
					 "%s", dev_addr);
				LOG("[bluez] Pairing succeeded: %s (%s)",
				    app->bonded_name, app->bonded_addr);
				set_pairing_mode(app, false);
				emit_pairing_success(app, app->bonded_name,
						     app->bonded_addr);
			} else {
				scan_bluez_devices(app);
			}
		} else if (app->bonded && strcmp(obj_path, app->bonded_path) == 0) {
			if (dict_lookup_str(changed, "Alias", app->bonded_name,
					    sizeof(app->bonded_name)) ||
			    dict_lookup_str(changed, "Name", app->bonded_name,
					    sizeof(app->bonded_name)))
				emit_bond_state(app);
		}
	}

	g_variant_unref(changed);
}

static void on_bluez_interfaces_removed(GDBusConnection *conn, const gchar *sender,
					const gchar *obj_path, const gchar *iface_name,
					const gchar *sig_name, GVariant *parameters,
					gpointer user_data)
{
	struct hfp_app *app = user_data;
	const gchar *removed_path = NULL;

	(void)conn;
	(void)sender;
	(void)obj_path;
	(void)iface_name;
	(void)sig_name;

	g_variant_get(parameters, "(&o@as)", &removed_path, NULL);
	if (removed_path && strstr(removed_path, "/org/bluez/hci0/dev_") != NULL)
		scan_bluez_devices(app);
}

static void register_gatt_application(struct hfp_app *app)
{
	GVariantBuilder b;
	GVariant *res;
	GError *err = NULL;

	if (!app->sys_bus || app->gatt_registered)
		return;
	g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
	res = g_dbus_connection_call_sync(
		app->sys_bus, "org.bluez", "/org/bluez/hci0",
		"org.bluez.GattManager1", "RegisterApplication",
		g_variant_new("(o@a{sv})", GATT_APP_PATH,
			      g_variant_builder_end(&b)),
		NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &err);
	if (!res) {
		LOG("[gatt] RegisterApplication failed: %s",
		    err ? err->message : "unknown");
		g_clear_error(&err);
	} else {
		g_variant_unref(res);
		app->gatt_registered = true;
		LOG("[gatt] Registered Battery Service (0x180F: 0x2A19, 0x2A1A, 0x2BED)");
	}
}

static bool configure_bluez_adapter(struct hfp_app *app)
{
	const struct {
		const char *prop;
		GVariant *val;
	} props[] = {
		{ "Alias", g_variant_new_string(LOCAL_BT_ALIAS) },
		{ "PairableTimeout", g_variant_new_uint32(120) },
		{ "DiscoverableTimeout", g_variant_new_uint32(120) },
		{ "Pairable", g_variant_new_boolean(FALSE) },
		{ "Discoverable", g_variant_new_boolean(FALSE) },
	};

	if (!set_adapter_prop(app, "Powered", g_variant_new_boolean(TRUE))) {
		for (size_t i = 0; i < G_N_ELEMENTS(props); i++)
			g_variant_unref(g_variant_ref_sink(props[i].val));
		return false;
	}
	for (size_t i = 0; i < G_N_ELEMENTS(props); i++)
		set_adapter_prop(app, props[i].prop, props[i].val);
	snprintf(app->local_bt_name, sizeof(app->local_bt_name), "%s", LOCAL_BT_ALIAS);
	app->pairing_active = false;
	LOG("[bluez] Configured hci0 (Alias=%s, Powered=1, Discoverable=0, Pairable=0)",
	    LOCAL_BT_ALIAS);
	register_gatt_application(app);
	scan_bluez_devices(app);
	return true;
}

static gboolean on_adapter_retry(gpointer user_data)
{
	struct hfp_app *app = user_data;

	if (configure_bluez_adapter(app) || ++app->adapter_retries >= 30) {
		app->adapter_timer_id = 0;
		return G_SOURCE_REMOVE;
	}
	return G_SOURCE_CONTINUE;
}

static void on_bluez_appeared(GDBusConnection *conn, const gchar *name,
			      const gchar *owner, gpointer user_data)
{
	struct hfp_app *app = user_data;
	(void)conn;
	(void)name;
	(void)owner;
	register_bluez_profile(app);
	configure_bluez_agent(app);
	if (!configure_bluez_adapter(app) && app->adapter_timer_id == 0) {
		app->adapter_retries = 0;
		app->adapter_timer_id =
			g_timeout_add_seconds(2, on_adapter_retry, app);
	}
}

static void on_bluez_vanished(GDBusConnection *conn, const gchar *name,
			      gpointer user_data)
{
	struct hfp_app *app = user_data;
	(void)conn;
	(void)name;
	if (app->adapter_timer_id) {
		g_source_remove(app->adapter_timer_id);
		app->adapter_timer_id = 0;
	}
	app->gatt_registered = false;
	app->pairing_active = false;
	close_rfcomm(app);
	emit_bond_state(app);
}

static void handle_ctl_cmd(struct hfp_app *app, const char *cmdline, char *reply,
			   size_t reply_sz)
{
	static const struct {
		const char *cmd;
		const char *at;
	} at_cmds[] = {
		{ "answer", "ATA" },
		{ "hangup", "AT+CHUP" },
		{ "redial", "AT+BLDN" },
		{ "sco-open", "AT+BCC" },
	};
	char cmd[256];
	char *arg;
	int idx;

	snprintf(cmd, sizeof(cmd), "%s", cmdline);
	cmd[strcspn(cmd, "\r\n")] = '\0';
	arg = strchr(cmd, ' ');
	if (arg) {
		*arg++ = '\0';
		while (*arg == ' ')
			arg++;
	}

	if (strcmp(cmd, "status") == 0) {
		struct alsa_bridge_cfg cfg;
		alsa_bridge_get_cfg(app->bridge, &cfg);
		snprintf(reply, reply_sz,
			 "connected=%d slc=%d sco=%d bridge=%d peer=%s call=%d callsetup=%d caller=%s output=%s mic=%s vgs=%d vgm=%d mute=%d aec=%d ns=%d slider=%d bonded=%d\n",
			 app->rfcomm_fd >= 0, app->slc_ready,
			 app->sco_fd >= 0, alsa_bridge_is_running(app->bridge),
			 app->peer_addr[0] ? app->peer_addr : "-",
			 app->val_call, app->val_callsetup,
			 app->caller_id[0] ? app->caller_id : "-",
			 output_names[cfg.output_mode],
			 mic_names[cfg.mic_source], cfg.vgs_level,
			 cfg.vgm_level, cfg.muted, cfg.enable_aec,
			 cfg.enable_ns, app->slider_open ? 1 : 0,
			 app->bonded ? 1 : 0);
		return;
	}

	if (strcmp(cmd, "bt-info") == 0) {
		snprintf(reply, reply_sz,
			 "bonded=%d\ndevice_name=%s\ndevice_addr=%s\nlocal_name=%s\npairing=%d\nslider=%d\n",
			 app->bonded ? 1 : 0,
			 app->bonded_name,
			 app->bonded_addr,
			 app->local_bt_name[0] ? app->local_bt_name : LOCAL_BT_ALIAS,
			 app->pairing_active ? 1 : 0,
			 app->slider_open ? 1 : 0);
		return;
	}

	if (strcmp(cmd, "pair-start") == 0) {
		set_pairing_mode(app, true);
		snprintf(reply, reply_sz, "OK pairing\n");
		return;
	}

	if (strcmp(cmd, "pair-stop") == 0) {
		set_pairing_mode(app, false);
		snprintf(reply, reply_sz, "OK stopped\n");
		return;
	}

	if (strcmp(cmd, "unpair") == 0) {
		unpair_all_devices(app);
		snprintf(reply, reply_sz, "OK unpaired\n");
		return;
	}

	for (size_t i = 0; i < G_N_ELEMENTS(at_cmds); i++) {
		if (strcmp(cmd, at_cmds[i].cmd) == 0) {
			if (app->rfcomm_fd < 0)
				snprintf(reply, reply_sz, "ERR not connected\n");
			else {
				queue_at(app, at_cmds[i].at);
				snprintf(reply, reply_sz, "OK %s\n", at_cmds[i].at);
			}
			return;
		}
	}

	if ((idx = lookup_name(output_names, G_N_ELEMENTS(output_names), cmd)) >= 0) {
		enum hfp_output_mode mode = (enum hfp_output_mode)idx;
		if (mode != OUTPUT_HEADPHONES)
			app->saved_output_mode = mode;
		alsa_bridge_set_output_mode(app->bridge, mode);
		emit_audio_state(app);
		snprintf(reply, reply_sz, "OK %s\n", output_names[idx]);
	} else if (strcmp(cmd, "dial") == 0 && arg && arg[0]) {
		if (app->rfcomm_fd < 0)
			snprintf(reply, reply_sz, "ERR not connected\n");
		else if (arg[strspn(arg, "0123456789*#+")] != '\0' || strlen(arg) > 32)
			snprintf(reply, reply_sz, "ERR invalid number\n");
		else {
			char at[64];
			snprintf(at, sizeof(at), "ATD%s;", arg);
			queue_at(app, at);
			snprintf(reply, reply_sz, "OK %s\n", at);
		}
	} else if (strcmp(cmd, "mic") == 0 && arg) {
		idx = lookup_name(mic_names, G_N_ELEMENTS(mic_names), arg);
		if (idx < 0)
			idx = MIC_DUAL;
		alsa_bridge_set_mic_source(app->bridge, (enum hfp_mic_source)idx);
		snprintf(reply, reply_sz, "OK mic %s\n", mic_names[idx]);
	} else if (strcmp(cmd, "mute") == 0 || strcmp(cmd, "unmute") == 0) {
		bool m = (cmd[0] == 'm');
		alsa_bridge_set_mute(app->bridge, m);
		emit_audio_state(app);
		snprintf(reply, reply_sz, "OK %s\n", m ? "muted" : "unmuted");
	} else if ((strcmp(cmd, "vgs") == 0 || strcmp(cmd, "vgm") == 0) && arg) {
		bool is_vgs = (cmd[2] == 's');
		int v = atoi(arg);
		if (is_vgs)
			alsa_bridge_set_vgs(app->bridge, v);
		else
			alsa_bridge_set_vgm(app->bridge, v);
		emit_audio_state(app);
		if (app->rfcomm_fd >= 0) {
			char at[32];
			snprintf(at, sizeof(at), "AT+VG%c=%d", is_vgs ? 'S' : 'M', v);
			queue_at(app, at);
		}
		snprintf(reply, reply_sz, "OK %s=%d\n", cmd, v);
	} else if (strcmp(cmd, "dsp") == 0 && arg) {
		bool aec = strstr(arg, "noaec") == NULL;
		bool ns = strstr(arg, "nons") == NULL;
		alsa_bridge_set_dsp(app->bridge, aec, ns);
		snprintf(reply, reply_sz, "OK aec=%d ns=%d\n", aec, ns);
	} else {
		snprintf(reply, reply_sz, "ERR unknown command\n");
	}
}

static gboolean on_ctl_accept(GIOChannel *src, GIOCondition cond, gpointer user_data)
{
	struct hfp_app *app = user_data;
	struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
	char buf[256] = { 0 }, reply[512];
	int cfd;
	ssize_t n;

	if (cond & (G_IO_ERR | G_IO_NVAL)) {
		app->ctl_watch_id = 0;
		return G_SOURCE_REMOVE;
	}

	cfd = accept(g_io_channel_unix_get_fd(src), NULL, NULL);
	if (cfd < 0)
		return G_SOURCE_CONTINUE;

	/* Never let a silent client block the main loop */
	setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	if ((n = read(cfd, buf, sizeof(buf) - 1)) > 0) {
		buf[n] = '\0';
		handle_ctl_cmd(app, buf, reply, sizeof(reply));
		(void)write(cfd, reply, strlen(reply));
	}
	close(cfd);
	return G_SOURCE_CONTINUE;
}

static int setup_ctl_socket(struct hfp_app *app)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	mode_t old_umask;
	int fd;

	unlink(app->ctl_sock_path);
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -errno;

	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", app->ctl_sock_path);
	old_umask = umask(0007);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
	    listen(fd, 8) < 0) {
		umask(old_umask);
		close(fd);
		return -errno;
	}
	umask(old_umask);
	chmod(app->ctl_sock_path, 0666);

	app->ctl_listen_fd = fd;
	app->ctl_watch_id =
		watch_fd(fd, G_IO_IN | G_IO_ERR | G_IO_NVAL, on_ctl_accept, app);
	return 0;
}

#define JACK_EVDEV_PATH "/dev/input/by-path/platform-7702000.sound-event"

static void apply_jack_state(struct hfp_app *app, bool inserted)
{
	struct alsa_bridge_cfg cfg;

	alsa_bridge_get_cfg(app->bridge, &cfg);
	if (inserted) {
		if (cfg.output_mode != OUTPUT_HEADPHONES)
			app->saved_output_mode = cfg.output_mode;
		alsa_bridge_set_output_mode(app->bridge, OUTPUT_HEADPHONES);
		emit_audio_state(app);
		LOG("[jack] wired headset plugged in -> output=headphones");
	} else if (cfg.output_mode == OUTPUT_HEADPHONES) {
		alsa_bridge_set_output_mode(app->bridge, app->saved_output_mode);
		emit_audio_state(app);
		LOG("[jack] wired headset unplugged -> output=%s",
		    output_names[app->saved_output_mode]);
	}
}

static gboolean on_jack_io(GIOChannel *src, GIOCondition cond, gpointer user_data)
{
	struct hfp_app *app = user_data;
	int fd = g_io_channel_unix_get_fd(src);
	struct input_event ev;

	if (cond & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
		app->jack_watch_id = 0;
		return G_SOURCE_REMOVE;
	}

	while (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
		if (ev.type == EV_SW && ev.code == SW_HEADPHONE_INSERT) {
			apply_jack_state(app, ev.value != 0);
		} else if (ev.type == EV_KEY && ev.code == KEY_MEDIA && ev.value == 1) {
			if (app->rfcomm_fd >= 0) {
				if (app->val_callsetup == 1)
					queue_at(app, "ATA");
				else if (app->val_call == 1 || app->val_callsetup > 1)
					queue_at(app, "AT+CHUP");
			}
		}
	}
	return G_SOURCE_CONTINUE;
}

static void setup_jack_listener(struct hfp_app *app)
{
	unsigned long sw_bits[(SW_MAX + 8 * sizeof(unsigned long)) / (8 * sizeof(unsigned long))] = { 0 };
	int fd = open(JACK_EVDEV_PATH, O_RDONLY | O_NONBLOCK | O_CLOEXEC);

	if (fd < 0) {
		LOG("[jack] open %s failed: %s", JACK_EVDEV_PATH, strerror(errno));
		return;
	}
	app->jack_fd = fd;
	app->jack_watch_id =
		watch_fd(fd, G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL, on_jack_io, app);

	if (ioctl(fd, EVIOCGSW(sizeof(sw_bits)), sw_bits) >= 0) {
		bool inserted = (sw_bits[SW_HEADPHONE_INSERT / (8 * sizeof(unsigned long))] >>
				 (SW_HEADPHONE_INSERT % (8 * sizeof(unsigned long)))) & 1UL;
		if (inserted)
			apply_jack_state(app, true);
	}
	LOG("[jack] monitoring wired headset jack on %s", JACK_EVDEV_PATH);
}

#define SLIDE_EVDEV_PATH "/dev/input/by-path/platform-gpio-keys-event"
#define KBD_LED_PATH "/sys/class/leds/white:kbd_backlight/brightness"

static void set_kbd_backlight(bool on)
{
	int fd = open(KBD_LED_PATH, O_WRONLY | O_CLOEXEC);

	if (fd >= 0) {
		(void)write(fd, on ? "1\n" : "0\n", 2);
		close(fd);
	}
}

static void apply_slide_state(struct hfp_app *app, bool open_state)
{
	app->slider_open = open_state;
	set_kbd_backlight(open_state);
	emit_slider_state(app);
	if (app->rfcomm_fd >= 0) {
		if (open_state && app->val_callsetup == 1) {
			LOG("[slide] keypad slid open during incoming call -> ATA");
			queue_at(app, "ATA");
		} else if (!open_state && app->val_call == 1) {
			LOG("[slide] keypad slid closed during active call -> AT+CHUP");
			queue_at(app, "AT+CHUP");
		}
	}
}

static gboolean on_slide_io(GIOChannel *src, GIOCondition cond, gpointer user_data)
{
	struct hfp_app *app = user_data;
	int fd = g_io_channel_unix_get_fd(src);
	struct input_event ev;

	if (cond & (G_IO_HUP | G_IO_ERR | G_IO_NVAL))
		return G_SOURCE_REMOVE;

	while (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
		if (ev.type == EV_SW && ev.code == SW_KEYPAD_SLIDE)
			apply_slide_state(app, ev.value != 0);
	}
	return G_SOURCE_CONTINUE;
}

static void setup_slide_listener(struct hfp_app *app)
{
	unsigned long sw_bits[(SW_MAX + 8 * sizeof(unsigned long)) / (8 * sizeof(unsigned long))] = { 0 };
	int fd = open(SLIDE_EVDEV_PATH, O_RDONLY | O_NONBLOCK | O_CLOEXEC);

	if (fd < 0)
		return;
	app->slide_fd = fd;
	app->slide_watch_id =
		watch_fd(fd, G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL, on_slide_io, app);

	if (ioctl(fd, EVIOCGSW(sizeof(sw_bits)), sw_bits) >= 0) {
		bool is_open = (sw_bits[SW_KEYPAD_SLIDE / (8 * sizeof(unsigned long))] >>
				(SW_KEYPAD_SLIDE % (8 * sizeof(unsigned long)))) & 1UL;
		app->slider_open = is_open;
		set_kbd_backlight(is_open);
	}
	LOG("[slide] monitoring keypad slider on %s", SLIDE_EVDEV_PATH);
}

static int send_ctl_cli(const char *sock_path, int argc, char **argv, int start)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	char cmd[256] = { 0 }, buf[512];
	int fd;
	ssize_t n;

	for (int i = start; i < argc; i++) {
		if (i > start)
			strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
		strncat(cmd, argv[i], sizeof(cmd) - strlen(cmd) - 1);
	}
	strncat(cmd, "\n", sizeof(cmd) - strlen(cmd) - 1);

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("socket");
		return 1;
	}
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("connect nokia-bt-hfp");
		close(fd);
		return 1;
	}
	(void)write(fd, cmd, strlen(cmd));
	while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
		buf[n] = '\0';
		fputs(buf, stdout);
	}
	close(fd);
	return 0;
}

static int gatt_char_index(const char *obj_path)
{
	if (strcmp(obj_path, GATT_CHAR_LEVEL_PATH) == 0)
		return 0;
	if (strcmp(obj_path, GATT_CHAR_POWER_PATH) == 0)
		return 1;
	if (strcmp(obj_path, GATT_CHAR_STATUS_PATH) == 0)
		return 2;
	return -1;
}

static const char *gatt_char_uuid(int idx)
{
	switch (idx) {
	case 0:
		return GATT_CHAR_LEVEL_UUID;
	case 1:
		return GATT_CHAR_POWER_UUID;
	case 2:
		return GATT_CHAR_STATUS_UUID;
	default:
		return "";
	}
}

static GVariant *build_gatt_flags_variant(void)
{
	GVariantBuilder b;

	g_variant_builder_init(&b, G_VARIANT_TYPE("as"));
	g_variant_builder_add(&b, "s", "read");
	g_variant_builder_add(&b, "s", "notify");
	return g_variant_builder_end(&b);
}

static void handle_gatt_om_method(GDBusConnection *conn, const gchar *sender,
				  const gchar *obj_path, const gchar *iface_name,
				  const gchar *method_name, GVariant *parameters,
				  GDBusMethodInvocation *invocation,
				  gpointer user_data)
{
	struct hfp_app *app = user_data;
	GVariantBuilder objs;
	GVariantBuilder svc_ifaces;
	GVariantBuilder svc_props;
	static const char *const char_paths[3] = {
		GATT_CHAR_LEVEL_PATH,
		GATT_CHAR_POWER_PATH,
		GATT_CHAR_STATUS_PATH,
	};

	(void)conn;
	(void)sender;
	(void)obj_path;
	(void)iface_name;
	(void)parameters;

	if (strcmp(method_name, "GetManagedObjects") != 0) {
		g_dbus_method_invocation_return_error(
			invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
			"Unknown method");
		return;
	}

	read_power_supply(app, NULL);

	g_variant_builder_init(&objs, G_VARIANT_TYPE("a{oa{sa{sv}}}"));

	g_variant_builder_init(&svc_props, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&svc_props, "{sv}", "UUID",
			      g_variant_new_string(GATT_BAS_UUID));
	g_variant_builder_add(&svc_props, "{sv}", "Primary",
			      g_variant_new_boolean(TRUE));
	g_variant_builder_init(&svc_ifaces, G_VARIANT_TYPE("a{sa{sv}}"));
	g_variant_builder_add(&svc_ifaces, "{sa{sv}}", "org.bluez.GattService1",
			      &svc_props);
	g_variant_builder_add(&objs, "{oa{sa{sv}}}", GATT_SVC_PATH, &svc_ifaces);

	for (int i = 0; i < 3; i++) {
		GVariantBuilder cifaces;
		GVariantBuilder cprops;
		uint8_t val[4];
		size_t len = build_gatt_char_value(app, i, val, sizeof(val));

		g_variant_builder_init(&cprops, G_VARIANT_TYPE("a{sv}"));
		g_variant_builder_add(&cprops, "{sv}", "UUID",
				      g_variant_new_string(gatt_char_uuid(i)));
		g_variant_builder_add(&cprops, "{sv}", "Service",
				      g_variant_new_object_path(GATT_SVC_PATH));
		g_variant_builder_add(&cprops, "{sv}", "Flags",
				      build_gatt_flags_variant());
		g_variant_builder_add(
			&cprops, "{sv}", "Value",
			g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, val, len, 1));
		g_variant_builder_init(&cifaces, G_VARIANT_TYPE("a{sa{sv}}"));
		g_variant_builder_add(&cifaces, "{sa{sv}}",
				      "org.bluez.GattCharacteristic1", &cprops);
		g_variant_builder_add(&objs, "{oa{sa{sv}}}", char_paths[i],
				      &cifaces);
	}

	g_dbus_method_invocation_return_value(invocation,
					      g_variant_new("(@a{oa{sa{sv}}})",
							    g_variant_builder_end(&objs)));
}

static GVariant *handle_gatt_get_prop(GDBusConnection *conn, const gchar *sender,
				      const gchar *obj_path, const gchar *iface_name,
				      const gchar *prop_name, GError **error,
				      gpointer user_data)
{
	struct hfp_app *app = user_data;
	int idx;

	(void)conn;
	(void)sender;
	(void)error;

	if (strcmp(iface_name, "org.bluez.GattService1") == 0) {
		if (strcmp(prop_name, "UUID") == 0)
			return g_variant_new_string(GATT_BAS_UUID);
		if (strcmp(prop_name, "Primary") == 0)
			return g_variant_new_boolean(TRUE);
		return NULL;
	}

	idx = gatt_char_index(obj_path);
	if (idx < 0)
		return NULL;

	if (strcmp(prop_name, "UUID") == 0)
		return g_variant_new_string(gatt_char_uuid(idx));
	if (strcmp(prop_name, "Service") == 0)
		return g_variant_new_object_path(GATT_SVC_PATH);
	if (strcmp(prop_name, "Flags") == 0)
		return build_gatt_flags_variant();
	if (strcmp(prop_name, "Value") == 0) {
		uint8_t val[4];
		size_t len;
		read_power_supply(app, NULL);
		len = build_gatt_char_value(app, idx, val, sizeof(val));
		return g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, val, len, 1);
	}
	return NULL;
}

static void handle_gatt_char_method(GDBusConnection *conn, const gchar *sender,
				    const gchar *obj_path, const gchar *iface_name,
				    const gchar *method_name, GVariant *parameters,
				    GDBusMethodInvocation *invocation,
				    gpointer user_data)
{
	struct hfp_app *app = user_data;
	int idx = gatt_char_index(obj_path);

	(void)conn;
	(void)sender;
	(void)iface_name;
	(void)parameters;

	if (idx < 0) {
		g_dbus_method_invocation_return_error(
			invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_OBJECT,
			"Unknown characteristic");
		return;
	}

	if (strcmp(method_name, "ReadValue") == 0) {
		uint8_t val[4];
		size_t len;
		read_power_supply(app, NULL);
		len = build_gatt_char_value(app, idx, val, sizeof(val));
		g_dbus_method_invocation_return_value(
			invocation,
			g_variant_new("(@ay)",
				      g_variant_new_fixed_array(
					      G_VARIANT_TYPE_BYTE, val, len, 1)));
	} else if (strcmp(method_name, "StartNotify") == 0) {
		app->gatt_notify[idx] = true;
		g_dbus_method_invocation_return_value(invocation, NULL);
	} else if (strcmp(method_name, "StopNotify") == 0) {
		app->gatt_notify[idx] = false;
		g_dbus_method_invocation_return_value(invocation, NULL);
	} else {
		g_dbus_method_invocation_return_error(
			invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
			"Unknown method");
	}
}

static void register_gatt_dbus_objects(struct hfp_app *app)
{
	static const GDBusInterfaceVTable om_vtable = {
		.method_call = handle_gatt_om_method,
	};
	static const GDBusInterfaceVTable svc_vtable = {
		.get_property = handle_gatt_get_prop,
	};
	static const GDBusInterfaceVTable char_vtable = {
		.method_call = handle_gatt_char_method,
		.get_property = handle_gatt_get_prop,
	};
	static const char *const char_paths[3] = {
		GATT_CHAR_LEVEL_PATH,
		GATT_CHAR_POWER_PATH,
		GATT_CHAR_STATUS_PATH,
	};
	GDBusNodeInfo *om_info = g_dbus_node_info_new_for_xml(gatt_om_xml, NULL);
	GDBusNodeInfo *svc_info = g_dbus_node_info_new_for_xml(gatt_svc_xml, NULL);
	GDBusNodeInfo *char_info = g_dbus_node_info_new_for_xml(gatt_char_xml, NULL);

	if (om_info) {
		app->gatt_om_reg_id = g_dbus_connection_register_object(
			app->sys_bus, GATT_APP_PATH, om_info->interfaces[0],
			&om_vtable, app, NULL, NULL);
		g_dbus_node_info_unref(om_info);
	}
	if (svc_info) {
		app->gatt_svc_reg_id = g_dbus_connection_register_object(
			app->sys_bus, GATT_SVC_PATH, svc_info->interfaces[0],
			&svc_vtable, app, NULL, NULL);
		g_dbus_node_info_unref(svc_info);
	}
	if (char_info) {
		for (int i = 0; i < 3; i++) {
			app->gatt_char_reg_id[i] = g_dbus_connection_register_object(
				app->sys_bus, char_paths[i],
				char_info->interfaces[0], &char_vtable, app,
				NULL, NULL);
		}
		g_dbus_node_info_unref(char_info);
	}
}

static gboolean on_quit_signal(gpointer user_data)
{
	g_main_loop_quit(user_data);
	return G_SOURCE_CONTINUE;
}

int main(int argc, char **argv)
{
	struct hfp_app app = {
		.rfcomm_fd = -1,
		.sco_fd = -1,
		.sco_listen_fd = -1,
		.ctl_listen_fd = -1,
		.jack_fd = -1,
		.slide_fd = -1,
		.slider_open = true,
		.bat_percent = -1,
	};
	struct alsa_bridge_cfg bcfg = {
		.output_mode = OUTPUT_EARPIECE,
		.mic_source = MIC_DUAL,
		.vgs_level = 12,
		.vgm_level = 12,
		.enable_aec = true,
		.enable_ns = true,
	};
	const GDBusInterfaceVTable vtable = { .method_call = handle_profile_method };
	GDBusNodeInfo *introspection = NULL;
	GError *err = NULL;
	int ret = 1;

	snprintf(app.local_bt_name, sizeof(app.local_bt_name), "%s", LOCAL_BT_ALIAS);
	get_ctl_sock_path(app.ctl_sock_path, sizeof(app.ctl_sock_path));

	for (int i = 1; i < argc; i++) {
		int idx;
		if (strcmp(argv[i], "--ctl") == 0 && i + 1 < argc) {
			return send_ctl_cli(app.ctl_sock_path, argc, argv, i + 1);
		} else if (strncmp(argv[i], "--", 2) == 0 &&
			   (idx = lookup_name(output_names, G_N_ELEMENTS(output_names),
					      argv[i] + 2)) >= 0) {
			bcfg.output_mode = (enum hfp_output_mode)idx;
		} else if (strcmp(argv[i], "--mic") == 0 && i + 1 < argc) {
			idx = lookup_name(mic_names, G_N_ELEMENTS(mic_names), argv[++i]);
			bcfg.mic_source = idx >= 0 ? (enum hfp_mic_source)idx : MIC_DUAL;
		} else if (strcmp(argv[i], "--no-aec") == 0) {
			bcfg.enable_aec = false;
		} else if (strcmp(argv[i], "--no-ns") == 0) {
			bcfg.enable_ns = false;
		} else if (strcmp(argv[i], "--verbose") == 0) {
			bcfg.verbose = true;
		} else {
			LOG("Usage: %s [--earpiece|--speaker|--headphones] [--mic dual|amic1|amic3] [--no-aec] [--no-ns] [--verbose]",
			    argv[0]);
			LOG("       %s --ctl status|bt-info|pair-start|pair-stop|unpair|answer|hangup|redial|dial <num>|sco-open|earpiece|speaker|headphones|mic dual|amic1|amic3|mute|unmute|vgs <0-15>|vgm <0-15>",
			    argv[0]);
			return 1;
		}
	}

	app.saved_output_mode =
		(bcfg.output_mode == OUTPUT_HEADPHONES) ? OUTPUT_EARPIECE : bcfg.output_mode;
	app.bridge = alsa_bridge_create(&bcfg);
	if (!app.bridge) {
		LOG("Failed to initialize msm8916 ALSA bridge");
		return 1;
	}
	setup_jack_listener(&app);
	setup_slide_listener(&app);

	app.sys_bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &err);
	if (!app.sys_bus) {
		LOG("Failed to connect to system D-Bus: %s",
		    err ? err->message : "unknown");
		goto out;
	}

	introspection = g_dbus_node_info_new_for_xml(profile_xml, &err);
	if (!introspection) {
		LOG("XML error: %s", err ? err->message : "unknown");
		goto out;
	}

	app.reg_id = g_dbus_connection_register_object(
		app.sys_bus, PROFILE_OBJ_PATH, introspection->interfaces[0],
		&vtable, &app, NULL, &err);
	if (app.reg_id != 0 && introspection->interfaces[1]) {
		g_dbus_connection_register_object(
			app.sys_bus, PROFILE_OBJ_PATH, introspection->interfaces[1],
			&vtable, &app, NULL, NULL);
	}
	g_dbus_node_info_unref(introspection);
	if (app.reg_id == 0) {
		LOG("Register object failed: %s", err ? err->message : "unknown");
		goto out;
	}
	register_gatt_dbus_objects(&app);
	read_power_supply(&app, NULL);
	app.bat_timer_id = g_timeout_add_seconds(10, on_battery_poll, &app);

	app.bluez_prop_sub_id = g_dbus_connection_signal_subscribe(
		app.sys_bus, "org.bluez", "org.freedesktop.DBus.Properties",
		"PropertiesChanged", NULL, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
		on_bluez_properties_changed, &app, NULL);
	app.bluez_rem_sub_id = g_dbus_connection_signal_subscribe(
		app.sys_bus, "org.bluez", "org.freedesktop.DBus.ObjectManager",
		"InterfacesRemoved", NULL, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
		on_bluez_interfaces_removed, &app, NULL);

	if (setup_sco_listener(&app) < 0)
		goto out;
	if (setup_ctl_socket(&app) < 0)
		LOG("[ctl] failed to bind %s", app.ctl_sock_path);

	app.loop = g_main_loop_new(NULL, FALSE);
	g_unix_signal_add(SIGINT, on_quit_signal, app.loop);
	g_unix_signal_add(SIGTERM, on_quit_signal, app.loop);
	signal(SIGPIPE, SIG_IGN);

	app.bluez_watch_id = g_bus_watch_name_on_connection(
		app.sys_bus, "org.bluez", G_BUS_NAME_WATCHER_FLAGS_NONE,
		on_bluez_appeared, on_bluez_vanished, &app, NULL);

	LOG("[main] nokia-bt-hfp started (ctl=%s)", app.ctl_sock_path);
	g_main_loop_run(app.loop);
	LOG("[main] shutting down");
	ret = 0;

out:
	g_clear_error(&err);
	close_rfcomm(&app);
	if (app.sco_listen_watch_id)
		g_source_remove(app.sco_listen_watch_id);
	if (app.sco_listen_fd >= 0)
		close(app.sco_listen_fd);
	if (app.ctl_watch_id)
		g_source_remove(app.ctl_watch_id);
	if (app.ctl_listen_fd >= 0) {
		close(app.ctl_listen_fd);
		unlink(app.ctl_sock_path);
	}
	if (app.jack_watch_id)
		g_source_remove(app.jack_watch_id);
	if (app.jack_fd >= 0)
		close(app.jack_fd);
	if (app.slide_watch_id)
		g_source_remove(app.slide_watch_id);
	if (app.slide_fd >= 0)
		close(app.slide_fd);
	if (app.adapter_timer_id)
		g_source_remove(app.adapter_timer_id);
	if (app.bat_timer_id)
		g_source_remove(app.bat_timer_id);
	if (app.bluez_watch_id)
		g_bus_unwatch_name(app.bluez_watch_id);
	if (app.bluez_prop_sub_id && app.sys_bus)
		g_dbus_connection_signal_unsubscribe(app.sys_bus, app.bluez_prop_sub_id);
	if (app.bluez_rem_sub_id && app.sys_bus)
		g_dbus_connection_signal_unsubscribe(app.sys_bus, app.bluez_rem_sub_id);
	if (app.gatt_om_reg_id && app.sys_bus)
		g_dbus_connection_unregister_object(app.sys_bus, app.gatt_om_reg_id);
	if (app.gatt_svc_reg_id && app.sys_bus)
		g_dbus_connection_unregister_object(app.sys_bus, app.gatt_svc_reg_id);
	for (int i = 0; i < 3; i++) {
		if (app.gatt_char_reg_id[i] && app.sys_bus)
			g_dbus_connection_unregister_object(app.sys_bus,
							    app.gatt_char_reg_id[i]);
	}
	if (app.reg_id)
		g_dbus_connection_unregister_object(app.sys_bus, app.reg_id);
	if (app.sys_bus)
		g_object_unref(app.sys_bus);
	if (app.loop)
		g_main_loop_unref(app.loop);
	alsa_bridge_destroy(app.bridge);
	return ret;
}
