#define _GNU_SOURCE
#include "alsa_bridge.h"

#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
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
#define AT_QUEUE_SIZE 24

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
	"</node>";

struct cind_map {
	int service;
	int call;
	int callsetup;
	int callheld;
	int signal;
	int roam;
	int battchg;
};

struct hfp_app {
	GMainLoop *loop;
	GDBusConnection *sys_bus;
	guint reg_id;
	guint bluez_watch_id;
	guint adapter_timer_id;
	int adapter_retries;
	bool profile_registered;

	struct alsa_bridge *bridge;
	char ctl_sock_path[256];
	char state_path[256];

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
	int val_service;
	int val_signal;
	char caller_id[64];
};

static struct hfp_app *g_app = NULL;

static void get_runtime_paths(char *sock_out, size_t sock_sz, char *state_out,
			      size_t state_sz)
{
	const char *xdg = getenv("XDG_RUNTIME_DIR");
	char base[128];

	if (xdg && xdg[0]) {
		snprintf(base, sizeof(base), "%s", xdg);
	} else {
		snprintf(base, sizeof(base), "/tmp");
	}
	snprintf(sock_out, sock_sz, "%s/nokia-bt-hfp.sock", base);
	snprintf(state_out, state_sz, "%s/nokia-bt-hfp.state", base);
}

static const char *output_mode_str(enum hfp_output_mode m)
{
	switch (m) {
	case OUTPUT_SPEAKER:
		return "speaker";
	case OUTPUT_HEADPHONES:
		return "headphones";
	default:
		return "earpiece";
	}
}

static const char *mic_source_str(enum hfp_mic_source m)
{
	switch (m) {
	case MIC_AMIC3:
		return "amic3";
	case MIC_DUAL:
		return "dual";
	default:
		return "amic1";
	}
}

static void write_state_file(struct hfp_app *app)
{
	struct alsa_bridge_cfg cfg;
	char tmp_path[280];
	FILE *f;

	alsa_bridge_get_cfg(app->bridge, &cfg);
	snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", app->state_path);
	f = fopen(tmp_path, "w");
	if (!f)
		return;
	fprintf(f,
		"connected=%d\n"
		"slc_ready=%d\n"
		"sco_active=%d\n"
		"bridge_running=%d\n"
		"peer=%s\n"
		"call=%d\n"
		"callsetup=%d\n"
		"callheld=%d\n"
		"caller_id=%s\n"
		"output=%s\n"
		"mic=%s\n"
		"vgs=%d\n"
		"vgm=%d\n"
		"muted=%d\n"
		"aec=%d\n"
		"ns=%d\n",
		app->rfcomm_fd >= 0 ? 1 : 0, app->slc_ready ? 1 : 0,
		app->sco_fd >= 0 ? 1 : 0,
		alsa_bridge_is_running(app->bridge) ? 1 : 0, app->peer_addr,
		app->val_call, app->val_callsetup, app->val_callheld,
		app->caller_id, output_mode_str(cfg.output_mode),
		mic_source_str(cfg.mic_source), cfg.vgs_level, cfg.vgm_level,
		cfg.muted ? 1 : 0, cfg.enable_aec ? 1 : 0,
		cfg.enable_ns ? 1 : 0);
	fclose(f);
	rename(tmp_path, app->state_path);
}

static void run_call_hook(struct hfp_app *app, const char *event)
{
	const char *home = getenv("HOME");
	char hook[256];
	struct stat st;

	if (home && home[0]) {
		snprintf(hook, sizeof(hook), "%s/.config/nokia-bt-hfp/hook",
			 home);
		if (stat(hook, &st) == 0 && (st.st_mode & S_IXUSR)) {
			char *argv[] = { hook, (char *)event, app->peer_addr,
					 app->caller_id, NULL };
			g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
				      NULL, NULL, NULL, NULL);
		}
	}
	if (strcmp(event, "incoming") == 0) {
		const char *who = app->caller_id[0] ? app->caller_id
						    : app->peer_addr;
		char *argv[] = { "notify-send", "-u", "critical",
				 "Incoming Bluetooth Call", (char *)who, NULL };
		g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
			      NULL, NULL);
	}
}

static gboolean on_at_timeout(gpointer user_data)
{
	struct hfp_app *app = user_data;

	app->at_timeout_id = 0;
	if (app->at_inflight) {
		fprintf(stderr, "[hfp] AT timeout waiting for response to: %s\n",
			app->at_current);
		app->at_inflight = false;
	}
	return G_SOURCE_REMOVE;
}

static void pump_at_queue(struct hfp_app *app)
{
	char wire[140];
	size_t len;
	ssize_t w;

	if (app->rfcomm_fd < 0 || app->at_inflight)
		return;
	if (app->at_head == app->at_tail)
		return;

	snprintf(app->at_current, sizeof(app->at_current), "%s",
		 app->at_queue[app->at_head]);
	app->at_head = (app->at_head + 1) % AT_QUEUE_SIZE;

	len = (size_t)snprintf(wire, sizeof(wire), "%s\r", app->at_current);
	w = write(app->rfcomm_fd, wire, len);
	if (w < 0) {
		fprintf(stderr, "[hfp] RFCOMM write '%s' failed: %s\n",
			app->at_current, strerror(errno));
		return;
	}

	app->at_inflight = true;
	if (app->at_timeout_id)
		g_source_remove(app->at_timeout_id);
	app->at_timeout_id = g_timeout_add_seconds(3, on_at_timeout, app);
	fprintf(stderr, "[hfp TX] %s\n", app->at_current);
}

static void queue_at(struct hfp_app *app, const char *cmd)
{
	int next = (app->at_tail + 1) % AT_QUEUE_SIZE;

	if (next == app->at_head) {
		fprintf(stderr, "[hfp] AT queue full, dropping '%s'\n", cmd);
		return;
	}
	snprintf(app->at_queue[app->at_tail],
		 sizeof(app->at_queue[app->at_tail]), "%s", cmd);
	app->at_tail = next;
	pump_at_queue(app);
}

static void parse_cind_map(struct hfp_app *app, const char *line)
{
	const char *p = line;
	int idx = 1;

	memset(&app->cind, 0, sizeof(app->cind));
	while ((p = strchr(p, '(')) != NULL) {
		const char *q1 = strchr(p, '"');
		const char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
		if (!q1 || !q2)
			break;
		if (strncmp(q1 + 1, "service", (size_t)(q2 - q1 - 1)) == 0)
			app->cind.service = idx;
		else if (strncmp(q1 + 1, "call", (size_t)(q2 - q1 - 1)) == 0)
			app->cind.call = idx;
		else if (strncmp(q1 + 1, "callsetup", (size_t)(q2 - q1 - 1)) ==
			 0)
			app->cind.callsetup = idx;
		else if (strncmp(q1 + 1, "call_setup", (size_t)(q2 - q1 - 1)) ==
			 0)
			app->cind.callsetup = idx;
		else if (strncmp(q1 + 1, "callheld", (size_t)(q2 - q1 - 1)) ==
			 0)
			app->cind.callheld = idx;
		else if (strncmp(q1 + 1, "signal", (size_t)(q2 - q1 - 1)) == 0)
			app->cind.signal = idx;
		else if (strncmp(q1 + 1, "roam", (size_t)(q2 - q1 - 1)) == 0)
			app->cind.roam = idx;
		else if (strncmp(q1 + 1, "battchg", (size_t)(q2 - q1 - 1)) == 0)
			app->cind.battchg = idx;
		idx++;
		p = q2 + 1;
	}
	fprintf(stderr,
		"[hfp] CIND map: service=%d call=%d callsetup=%d callheld=%d\n",
		app->cind.service, app->cind.call, app->cind.callsetup,
		app->cind.callheld);
}

static void apply_indicator(struct hfp_app *app, int idx, int val)
{
	if (idx <= 0)
		return;
	if (idx == app->cind.service) {
		app->val_service = val;
	} else if (idx == app->cind.call) {
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
		else if (val == 0 && prev == 1 && app->val_call == 0)
			app->caller_id[0] = '\0';
	} else if (idx == app->cind.callheld) {
		app->val_callheld = val;
	} else if (idx == app->cind.signal) {
		app->val_signal = val;
	}
}

static void handle_at_line(struct hfp_app *app, const char *line)
{
	if (!line[0])
		return;
	fprintf(stderr, "[hfp RX] %s\n", line);

	if (strncmp(line, "+BRSF:", 6) == 0) {
		app->ag_features = (unsigned int)strtoul(line + 6, NULL, 10);
		fprintf(stderr, "[hfp] AG features=0x%x (codec_neg=%d)\n",
			app->ag_features, (app->ag_features & (1U << 9)) ? 1 : 0);
	} else if (strncmp(line, "+CIND:", 6) == 0) {
		const char *rest = line + 6;
		while (*rest == ' ')
			rest++;
		if (strchr(rest, '(')) {
			parse_cind_map(app, rest);
		} else {
			int idx = 1;
			char *dup = strdup(rest);
			char *tok = strtok(dup, ",");
			while (tok) {
				apply_indicator(app, idx++, atoi(tok));
				tok = strtok(NULL, ",");
			}
			free(dup);
		}
	} else if (strncmp(line, "+CIEV:", 6) == 0) {
		int idx = 0, val = 0;
		if (sscanf(line + 6, " %d , %d", &idx, &val) == 2)
			apply_indicator(app, idx, val);
	} else if (strncmp(line, "+BCS:", 5) == 0) {
		int codec = atoi(line + 5);
		char reply[32];
		snprintf(reply, sizeof(reply), "AT+BCS=%d", codec);
		queue_at(app, reply);
	} else if (strncmp(line, "+CLIP:", 6) == 0) {
		const char *q1 = strchr(line + 6, '"');
		const char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
		if (q1 && q2 && q2 > q1 + 1) {
			size_t n = (size_t)(q2 - q1 - 1);
			if (n >= sizeof(app->caller_id))
				n = sizeof(app->caller_id) - 1;
			memcpy(app->caller_id, q1 + 1, n);
			app->caller_id[n] = '\0';
		}
	} else if (strncmp(line, "+VGS:", 5) == 0) {
		int v = atoi(line + 5);
		alsa_bridge_set_vgs(app->bridge, v);
	} else if (strncmp(line, "+VGM:", 5) == 0) {
		int v = atoi(line + 5);
		alsa_bridge_set_vgm(app->bridge, v);
	} else if (strcmp(line, "RING") == 0) {
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
				if (app->ag_features & (1U << 9))
					queue_at(app, "AT+BAC=1");
				queue_at(app, "AT+CIND=?");
				queue_at(app, "AT+CIND?");
				queue_at(app, "AT+CMER=3,0,0,1");
				if (app->ag_features & (1U << 0))
					queue_at(app, "AT+CHLD=?");
				queue_at(app, "AT+CLIP=1");
				queue_at(app, "AT+CMEE=1");
				queue_at(app, "AT+VGS=12");
				queue_at(app, "AT+VGM=12");
			} else if (strncmp(app->at_current, "AT+CMER=", 8) == 0 &&
				   ok) {
				app->slc_ready = true;
				fprintf(stderr, "[hfp] SLC established\n");
			}
			app->at_inflight = false;
			pump_at_queue(app);
		}
	}
	write_state_file(app);
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
	write_state_file(app);
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
	app->val_call = 0;
	app->val_callsetup = 0;
	app->val_callheld = 0;
	app->device_path[0] = '\0';
	app->peer_addr[0] = '\0';
	app->caller_id[0] = '\0';
	write_state_file(app);
}

static gboolean on_rfcomm_io(GIOChannel *src, GIOCondition cond,
			     gpointer user_data)
{
	struct hfp_app *app = user_data;
	int fd = g_io_channel_unix_get_fd(src);

	if (cond & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
		fprintf(stderr, "[hfp] RFCOMM disconnected\n");
		close_rfcomm(app);
		return G_SOURCE_REMOVE;
	}

	if (cond & G_IO_IN) {
		ssize_t n = read(fd, app->rx_buf + app->rx_len,
				 sizeof(app->rx_buf) - 1 - app->rx_len);
		if (n <= 0) {
			if (n < 0 && (errno == EAGAIN || errno == EINTR))
				return G_SOURCE_CONTINUE;
			fprintf(stderr, "[hfp] RFCOMM EOF/read err: %s\n",
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
					handle_at_line(app,
						       app->rx_buf + start);
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

static gboolean on_sco_conn_io(GIOChannel *src, GIOCondition cond,
			       gpointer user_data)
{
	struct hfp_app *app = user_data;
	int fd = g_io_channel_unix_get_fd(src);

	if (cond & G_IO_IN) {
		uint8_t discard[256];
		ssize_t n = recv(fd, discard, sizeof(discard), MSG_DONTWAIT);
		if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
			fprintf(stderr, "[sco] connection closed\n");
			close_sco(app);
			return G_SOURCE_REMOVE;
		}
	}
	if (cond & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
		fprintf(stderr, "[sco] connection HUP/ERR\n");
		close_sco(app);
		return G_SOURCE_REMOVE;
	}
	return G_SOURCE_CONTINUE;
}

static void attach_sco_fd(struct hfp_app *app, int fd)
{
	GIOChannel *ch;

	close_sco(app);
	app->sco_fd = fd;
	ch = g_io_channel_unix_new(fd);
	g_io_channel_set_close_on_unref(ch, FALSE);
	app->sco_watch_id = g_io_add_watch(
		ch, G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL, on_sco_conn_io,
		app);
	g_io_channel_unref(ch);

	if (alsa_bridge_start(app->bridge) < 0)
		fprintf(stderr, "[sco] failed to start ALSA INT_BT_SCO bridge\n");
	write_state_file(app);
}

static gboolean on_sco_accept(GIOChannel *src, GIOCondition cond,
			      gpointer user_data)
{
	struct hfp_app *app = user_data;
	int lfd = g_io_channel_unix_get_fd(src);
	struct bt_sockaddr_sco peer;
	socklen_t plen = sizeof(peer);
	int cfd;

	if (cond & (G_IO_ERR | G_IO_NVAL))
		return G_SOURCE_REMOVE;

	cfd = accept(lfd, (struct sockaddr *)&peer, &plen);
	if (cfd < 0) {
		if (errno != EAGAIN && errno != EINTR)
			fprintf(stderr, "[sco] accept failed: %s\n",
				strerror(errno));
		return G_SOURCE_CONTINUE;
	}

	fprintf(stderr,
		"[sco] accepted incoming eSCO from %02X:%02X:%02X:%02X:%02X:%02X (fd=%d)\n",
		peer.sco_bdaddr[5], peer.sco_bdaddr[4], peer.sco_bdaddr[3],
		peer.sco_bdaddr[2], peer.sco_bdaddr[1], peer.sco_bdaddr[0],
		cfd);
	/* Keep SCO fd open without writing to avoid btqcomsmd -EILSEQ (-84) */
	attach_sco_fd(app, cfd);
	return G_SOURCE_CONTINUE;
}

static int setup_sco_listener(struct hfp_app *app)
{
	struct bt_sockaddr_sco addr;
	GIOChannel *ch;
	int fd;

	fd = socket(AF_BLUETOOTH, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC,
		    BTPROTO_SCO);
	if (fd < 0) {
		fprintf(stderr, "[sco] socket(BTPROTO_SCO) failed: %s\n",
			strerror(errno));
		return -errno;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sco_family = AF_BLUETOOTH;
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		fprintf(stderr, "[sco] bind failed: %s\n", strerror(errno));
		close(fd);
		return -errno;
	}
	if (listen(fd, 4) < 0) {
		fprintf(stderr, "[sco] listen failed: %s\n", strerror(errno));
		close(fd);
		return -errno;
	}

	app->sco_listen_fd = fd;
	ch = g_io_channel_unix_new(fd);
	g_io_channel_set_close_on_unref(ch, FALSE);
	app->sco_listen_watch_id = g_io_add_watch(
		ch, G_IO_IN | G_IO_ERR | G_IO_NVAL, on_sco_accept, app);
	g_io_channel_unref(ch);
	fprintf(stderr, "[sco] listening on BTPROTO_SCO\n");
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
				  const gchar *obj_path,
				  const gchar *iface_name,
				  const gchar *method_name,
				  GVariant *parameters,
				  GDBusMethodInvocation *invocation,
				  gpointer user_data)
{
	struct hfp_app *app = user_data;
	(void)conn;
	(void)sender;
	(void)obj_path;
	(void)iface_name;

	if (strcmp(method_name, "Release") == 0) {
		fprintf(stderr, "[bluez] Profile1.Release called\n");
		close_rfcomm(app);
		g_dbus_method_invocation_return_value(invocation, NULL);
	} else if (strcmp(method_name, "RequestDisconnection") == 0) {
		const gchar *dev = NULL;
		g_variant_get(parameters, "(&o)", &dev);
		fprintf(stderr, "[bluez] RequestDisconnection(%s)\n",
			dev ? dev : "");
		close_rfcomm(app);
		g_dbus_method_invocation_return_value(invocation, NULL);
	} else if (strcmp(method_name, "NewConnection") == 0) {
		const gchar *dev = NULL;
		gint32 fd_idx = -1;
		GVariant *props = NULL;
		GDBusMessage *msg = g_dbus_method_invocation_get_message(invocation);
		GUnixFDList *fd_list = g_dbus_message_get_unix_fd_list(msg);
		GError *err = NULL;
		GIOChannel *ch;
		int fd;

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

		int flags = fcntl(fd, F_GETFL, 0);
		if (flags >= 0)
			fcntl(fd, F_SETFL, flags | O_NONBLOCK);

		ch = g_io_channel_unix_new(fd);
		g_io_channel_set_close_on_unref(ch, FALSE);
		app->rfcomm_watch_id = g_io_add_watch(
			ch, G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
			on_rfcomm_io, app);
		g_io_channel_unref(ch);

		fprintf(stderr, "[bluez] NewConnection dev=%s peer=%s fd=%d\n",
			app->device_path, app->peer_addr, fd);
		g_dbus_method_invocation_return_value(invocation, NULL);

		/* Initiate HFP 1.7 SLC: bits 0|1|2|4|5|7 = 183 */
		queue_at(app, "AT+BRSF=183");
		write_state_file(app);
	}
}

static void register_bluez_profile(struct hfp_app *app)
{
	GVariantBuilder b;
	GVariant *res;
	GError *err = NULL;

	g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&b, "{sv}", "Name",
			      g_variant_new_string("Handsfree"));
	g_variant_builder_add(&b, "{sv}", "AutoConnect",
			      g_variant_new_boolean(TRUE));
	g_variant_builder_add(&b, "{sv}", "Version",
			      g_variant_new_uint16(0x0107));
	g_variant_builder_add(&b, "{sv}", "Features",
			      g_variant_new_uint16(0x0017));

	res = g_dbus_connection_call_sync(
		app->sys_bus, "org.bluez", "/org/bluez",
		"org.bluez.ProfileManager1", "RegisterProfile",
		g_variant_new("(os@a{sv})", PROFILE_OBJ_PATH, HFP_HF_UUID,
			      g_variant_builder_end(&b)),
		NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &err);
	if (!res) {
		fprintf(stderr,
			"[bluez] RegisterProfile failed (is WirePlumber hfp_hf disabled?): %s\n",
			err ? err->message : "unknown");
		g_clear_error(&err);
		app->profile_registered = false;
	} else {
		g_variant_unref(res);
		app->profile_registered = true;
		fprintf(stderr, "[bluez] Registered HFP-HF profile (%s)\n",
			HFP_HF_UUID);
	}
}

static bool set_adapter_prop(struct hfp_app *app, const char *prop,
			     GVariant *val)
{
	GVariant *res;
	GError *err = NULL;

	res = g_dbus_connection_call_sync(
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

static bool configure_bluez_adapter(struct hfp_app *app)
{
	if (!set_adapter_prop(app, "Powered", g_variant_new_boolean(TRUE)))
		return false;
	set_adapter_prop(app, "Alias", g_variant_new_string("nokia-argon"));
	set_adapter_prop(app, "PairableTimeout", g_variant_new_uint32(0));
	set_adapter_prop(app, "Pairable", g_variant_new_boolean(TRUE));
	set_adapter_prop(app, "DiscoverableTimeout", g_variant_new_uint32(0));
	set_adapter_prop(app, "Discoverable", g_variant_new_boolean(TRUE));
	fprintf(stderr,
		"[bluez] Configured hci0 (Alias=nokia-argon, Powered=1, Discoverable=1, Pairable=1)\n");
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
	app->profile_registered = false;
	close_rfcomm(app);
}

static void handle_ctl_cmd(struct hfp_app *app, const char *cmdline, char *reply,
			   size_t reply_sz)
{
	char cmd[256];
	char *arg;

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
			 "connected=%d slc=%d sco=%d bridge=%d peer=%s call=%d callsetup=%d caller=%s output=%s mic=%s vgs=%d vgm=%d mute=%d aec=%d ns=%d\n",
			 app->rfcomm_fd >= 0, app->slc_ready,
			 app->sco_fd >= 0, alsa_bridge_is_running(app->bridge),
			 app->peer_addr[0] ? app->peer_addr : "-",
			 app->val_call, app->val_callsetup,
			 app->caller_id[0] ? app->caller_id : "-",
			 output_mode_str(cfg.output_mode),
			 mic_source_str(cfg.mic_source), cfg.vgs_level,
			 cfg.vgm_level, cfg.muted, cfg.enable_aec,
			 cfg.enable_ns);
	} else if (strcmp(cmd, "answer") == 0) {
		if (app->rfcomm_fd < 0)
			snprintf(reply, reply_sz, "ERR not connected\n");
		else {
			queue_at(app, "ATA");
			snprintf(reply, reply_sz, "OK ATA\n");
		}
	} else if (strcmp(cmd, "hangup") == 0) {
		if (app->rfcomm_fd < 0)
			snprintf(reply, reply_sz, "ERR not connected\n");
		else {
			queue_at(app, "AT+CHUP");
			snprintf(reply, reply_sz, "OK AT+CHUP\n");
		}
	} else if (strcmp(cmd, "redial") == 0) {
		if (app->rfcomm_fd < 0)
			snprintf(reply, reply_sz, "ERR not connected\n");
		else {
			queue_at(app, "AT+BLDN");
			snprintf(reply, reply_sz, "OK AT+BLDN\n");
		}
	} else if (strcmp(cmd, "dial") == 0 && arg && arg[0]) {
		if (app->rfcomm_fd < 0)
			snprintf(reply, reply_sz, "ERR not connected\n");
		else {
			char at[128];
			snprintf(at, sizeof(at), "ATD%s;", arg);
			queue_at(app, at);
			snprintf(reply, reply_sz, "OK %s\n", at);
		}
	} else if (strcmp(cmd, "sco-open") == 0) {
		if (app->rfcomm_fd < 0)
			snprintf(reply, reply_sz, "ERR not connected\n");
		else {
			queue_at(app, "AT+BCC");
			snprintf(reply, reply_sz, "OK AT+BCC\n");
		}
	} else if (strcmp(cmd, "earpiece") == 0) {
		alsa_bridge_set_output_mode(app->bridge, OUTPUT_EARPIECE);
		write_state_file(app);
		snprintf(reply, reply_sz, "OK earpiece\n");
	} else if (strcmp(cmd, "speaker") == 0) {
		alsa_bridge_set_output_mode(app->bridge, OUTPUT_SPEAKER);
		write_state_file(app);
		snprintf(reply, reply_sz, "OK speaker\n");
	} else if (strcmp(cmd, "headphones") == 0) {
		alsa_bridge_set_output_mode(app->bridge, OUTPUT_HEADPHONES);
		write_state_file(app);
		snprintf(reply, reply_sz, "OK headphones\n");
	} else if (strcmp(cmd, "mic") == 0 && arg) {
		if (strcmp(arg, "dual") == 0)
			alsa_bridge_set_mic_source(app->bridge, MIC_DUAL);
		else if (strcmp(arg, "amic3") == 0)
			alsa_bridge_set_mic_source(app->bridge, MIC_AMIC3);
		else
			alsa_bridge_set_mic_source(app->bridge, MIC_AMIC1);
		write_state_file(app);
		snprintf(reply, reply_sz, "OK mic %s\n", arg);
	} else if (strcmp(cmd, "mute") == 0) {
		alsa_bridge_set_mute(app->bridge, true);
		write_state_file(app);
		snprintf(reply, reply_sz, "OK muted\n");
	} else if (strcmp(cmd, "unmute") == 0) {
		alsa_bridge_set_mute(app->bridge, false);
		write_state_file(app);
		snprintf(reply, reply_sz, "OK unmuted\n");
	} else if (strcmp(cmd, "vgs") == 0 && arg) {
		int v = atoi(arg);
		alsa_bridge_set_vgs(app->bridge, v);
		if (app->rfcomm_fd >= 0) {
			char at[32];
			snprintf(at, sizeof(at), "AT+VGS=%d", v);
			queue_at(app, at);
		}
		write_state_file(app);
		snprintf(reply, reply_sz, "OK vgs=%d\n", v);
	} else if (strcmp(cmd, "vgm") == 0 && arg) {
		int v = atoi(arg);
		alsa_bridge_set_vgm(app->bridge, v);
		if (app->rfcomm_fd >= 0) {
			char at[32];
			snprintf(at, sizeof(at), "AT+VGM=%d", v);
			queue_at(app, at);
		}
		write_state_file(app);
		snprintf(reply, reply_sz, "OK vgm=%d\n", v);
	} else if (strcmp(cmd, "dsp") == 0 && arg) {
		bool aec = strstr(arg, "noaec") == NULL;
		bool ns = strstr(arg, "nons") == NULL;
		alsa_bridge_set_dsp(app->bridge, aec, ns);
		write_state_file(app);
		snprintf(reply, reply_sz, "OK aec=%d ns=%d\n", aec, ns);
	} else {
		snprintf(reply, reply_sz, "ERR unknown command\n");
	}
}

static gboolean on_ctl_accept(GIOChannel *src, GIOCondition cond,
			      gpointer user_data)
{
	struct hfp_app *app = user_data;
	int lfd = g_io_channel_unix_get_fd(src);
	int cfd;
	char buf[256] = {0};
	char reply[512];
	ssize_t n;

	if (cond & (G_IO_ERR | G_IO_NVAL))
		return G_SOURCE_REMOVE;

	cfd = accept(lfd, NULL, NULL);
	if (cfd < 0)
		return G_SOURCE_CONTINUE;

	n = read(cfd, buf, sizeof(buf) - 1);
	if (n > 0) {
		buf[n] = '\0';
		handle_ctl_cmd(app, buf, reply, sizeof(reply));
		(void)write(cfd, reply, strlen(reply));
	}
	close(cfd);
	return G_SOURCE_CONTINUE;
}

static int setup_ctl_socket(struct hfp_app *app)
{
	struct sockaddr_un addr;
	GIOChannel *ch;
	int fd;

	unlink(app->ctl_sock_path);
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -errno;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s",
		 app->ctl_sock_path);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
	    listen(fd, 8) < 0) {
		close(fd);
		return -errno;
	}

	app->ctl_listen_fd = fd;
	ch = g_io_channel_unix_new(fd);
	g_io_channel_set_close_on_unref(ch, FALSE);
	app->ctl_watch_id = g_io_add_watch(
		ch, G_IO_IN | G_IO_ERR | G_IO_NVAL, on_ctl_accept, app);
	g_io_channel_unref(ch);
	return 0;
}

static int send_ctl_cli(const char *sock_path, int argc, char **argv, int start)
{
	struct sockaddr_un addr;
	char cmd[256] = {0};
	char buf[512];
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
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
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

static void on_sig(int sig)
{
	(void)sig;
	if (g_app && g_app->loop)
		g_main_loop_quit(g_app->loop);
}

int main(int argc, char **argv)
{
	struct hfp_app app;
	struct alsa_bridge_cfg bcfg = {
		.output_mode = OUTPUT_EARPIECE,
		.mic_source = MIC_AMIC1,
		.vgs_level = 12,
		.vgm_level = 12,
		.enable_aec = true,
		.enable_ns = true,
		.muted = false,
	};
	GDBusNodeInfo *introspection = NULL;
	const GDBusInterfaceVTable vtable = {
		.method_call = handle_profile_method,
	};
	GError *err = NULL;

	memset(&app, 0, sizeof(app));
	app.rfcomm_fd = -1;
	app.sco_fd = -1;
	app.sco_listen_fd = -1;
	app.ctl_listen_fd = -1;
	get_runtime_paths(app.ctl_sock_path, sizeof(app.ctl_sock_path),
			  app.state_path, sizeof(app.state_path));

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--ctl") == 0 && i + 1 < argc) {
			return send_ctl_cli(app.ctl_sock_path, argc, argv,
					    i + 1);
		} else if (strcmp(argv[i], "--speaker") == 0) {
			bcfg.output_mode = OUTPUT_SPEAKER;
		} else if (strcmp(argv[i], "--earpiece") == 0) {
			bcfg.output_mode = OUTPUT_EARPIECE;
		} else if (strcmp(argv[i], "--headphones") == 0) {
			bcfg.output_mode = OUTPUT_HEADPHONES;
		} else if (strcmp(argv[i], "--mic") == 0 && i + 1 < argc) {
			i++;
			if (strcmp(argv[i], "dual") == 0)
				bcfg.mic_source = MIC_DUAL;
			else if (strcmp(argv[i], "amic3") == 0)
				bcfg.mic_source = MIC_AMIC3;
			else
				bcfg.mic_source = MIC_AMIC1;
		} else if (strcmp(argv[i], "--no-aec") == 0) {
			bcfg.enable_aec = false;
		} else if (strcmp(argv[i], "--no-ns") == 0) {
			bcfg.enable_ns = false;
		} else if (strcmp(argv[i], "--verbose") == 0) {
			bcfg.verbose = true;
		} else {
			fprintf(stderr,
				"Usage: %s [--earpiece|--speaker|--headphones] [--mic amic1|amic3] [--no-aec] [--no-ns] [--verbose]\n"
				"       %s --ctl status|answer|hangup|redial|dial <num>|sco-open|earpiece|speaker|headphones|mic amic1|amic3|mute|unmute|vgs <0-15>|vgm <0-15>\n",
				argv[0], argv[0]);
			return 1;
		}
	}

	app.bridge = alsa_bridge_create(&bcfg);
	if (!app.bridge) {
		fprintf(stderr, "Failed to initialize msm8916 ALSA bridge\n");
		return 1;
	}

	app.sys_bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &err);
	if (!app.sys_bus) {
		fprintf(stderr, "Failed to connect to system D-Bus: %s\n",
			err ? err->message : "unknown");
		g_clear_error(&err);
		alsa_bridge_destroy(app.bridge);
		return 1;
	}

	introspection = g_dbus_node_info_new_for_xml(profile_xml, &err);
	if (!introspection) {
		fprintf(stderr, "XML error: %s\n", err->message);
		return 1;
	}

	app.reg_id = g_dbus_connection_register_object(
		app.sys_bus, PROFILE_OBJ_PATH, introspection->interfaces[0],
		&vtable, &app, NULL, &err);
	g_dbus_node_info_unref(introspection);
	if (app.reg_id == 0) {
		fprintf(stderr, "Register object failed: %s\n", err->message);
		return 1;
	}

	if (setup_sco_listener(&app) < 0)
		return 1;
	if (setup_ctl_socket(&app) < 0)
		fprintf(stderr, "[ctl] failed to bind %s\n", app.ctl_sock_path);

	app.loop = g_main_loop_new(NULL, FALSE);
	g_app = &app;
	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	signal(SIGPIPE, SIG_IGN);

	app.bluez_watch_id = g_bus_watch_name_on_connection(
		app.sys_bus, "org.bluez", G_BUS_NAME_WATCHER_FLAGS_NONE,
		on_bluez_appeared, on_bluez_vanished, &app, NULL);

	write_state_file(&app);
	fprintf(stderr, "[main] nokia-bt-hfp started (ctl=%s)\n",
		app.ctl_sock_path);
	g_main_loop_run(app.loop);

	fprintf(stderr, "[main] shutting down\n");
	close_rfcomm(&app);
	if (app.sco_listen_watch_id)
		g_source_remove(app.sco_listen_watch_id);
	if (app.sco_listen_fd >= 0)
		close(app.sco_listen_fd);
	if (app.ctl_watch_id)
		g_source_remove(app.ctl_watch_id);
	if (app.ctl_listen_fd >= 0)
		close(app.ctl_listen_fd);
	unlink(app.ctl_sock_path);
	unlink(app.state_path);
	if (app.adapter_timer_id)
		g_source_remove(app.adapter_timer_id);
	if (app.bluez_watch_id)
		g_bus_unwatch_name(app.bluez_watch_id);
	if (app.reg_id)
		g_dbus_connection_unregister_object(app.sys_bus, app.reg_id);
	g_object_unref(app.sys_bus);
	g_main_loop_unref(app.loop);
	alsa_bridge_destroy(app.bridge);
	return 0;
}
