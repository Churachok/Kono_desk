#define _POSIX_C_SOURCE 200809L

#include "config.h"

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wlr/util/log.h>

static struct kono_bind binds[KONO_MAX_BINDS];
static int num_binds;
static char exec_once[KONO_MAX_EXEC][KONO_MAX_CMD];
static int num_exec_once;

static char *trim(char *s) {
	while (*s && isspace((unsigned char)*s)) {
		s++;
	}
	if (*s == '\0') {
		return s;
	}
	char *end = s + strlen(s) - 1;
	while (end > s && isspace((unsigned char)*end)) {
		*end-- = '\0';
	}
	return s;
}

static uint32_t parse_mod_token(const char *tok) {
	if (strcasecmp(tok, "SUPER") == 0 || strcasecmp(tok, "MOD4") == 0 ||
			strcasecmp(tok, "LOGO") == 0 || strcasecmp(tok, "WIN") == 0) {
		return WLR_MODIFIER_LOGO;
	}
	if (strcasecmp(tok, "SHIFT") == 0) {
		return WLR_MODIFIER_SHIFT;
	}
	if (strcasecmp(tok, "CTRL") == 0 || strcasecmp(tok, "CONTROL") == 0) {
		return WLR_MODIFIER_CTRL;
	}
	if (strcasecmp(tok, "ALT") == 0 || strcasecmp(tok, "MOD1") == 0) {
		return WLR_MODIFIER_ALT;
	}
	return 0;
}

static uint32_t parse_mods(char *mods) {
	uint32_t result = 0;
	for (char *p = mods; *p; p++) {
		if (*p == '+' || *p == ',') {
			*p = ' ';
		}
	}
	char *save = NULL;
	for (char *tok = strtok_r(mods, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save)) {
		result |= parse_mod_token(tok);
	}
	return result;
}

static xkb_keysym_t parse_key(const char *key) {
	if (strncasecmp(key, "mouse:", 6) == 0) {
		return XKB_KEY_NoSymbol;
	}
	if (strcasecmp(key, "Return") == 0 || strcasecmp(key, "Enter") == 0) {
		return XKB_KEY_Return;
	}
	if (strcasecmp(key, "Space") == 0) {
		return XKB_KEY_space;
	}
	if (strcasecmp(key, "Esc") == 0) {
		return XKB_KEY_Escape;
	}
	xkb_keysym_t sym = xkb_keysym_from_name(key, XKB_KEYSYM_CASE_INSENSITIVE);
	if (sym == XKB_KEY_NoSymbol) {
		wlr_log(WLR_ERROR, "Unknown key: %s", key);
		return XKB_KEY_NoSymbol;
	}
	return xkb_keysym_to_lower(sym);
}

static enum kono_dispatcher parse_dispatcher(const char *name) {
	if (strcasecmp(name, "exec") == 0) {
		return KONO_DISP_EXEC;
	}
	if (strcasecmp(name, "killactive") == 0) {
		return KONO_DISP_KILLACTIVE;
	}
	if (strcasecmp(name, "exit") == 0) {
		return KONO_DISP_EXIT;
	}
	if (strcasecmp(name, "fullscreen") == 0) {
		return KONO_DISP_FULLSCREEN;
	}
	if (strcasecmp(name, "togglefloating") == 0) {
		return KONO_DISP_TOGGLEFLOATING;
	}
	if (strcasecmp(name, "workspace") == 0) {
		return KONO_DISP_WORKSPACE;
	}
	if (strcasecmp(name, "movetoworkspace") == 0) {
		return KONO_DISP_MOVETOWORKSPACE;
	}
	if (strcasecmp(name, "movefocus") == 0) {
		return KONO_DISP_MOVEFOCUS;
	}
	if (strcasecmp(name, "cyclenext") == 0) {
		return KONO_DISP_CYCLENEXT;
	}
	if (strcasecmp(name, "cycleprev") == 0) {
		return KONO_DISP_CYCLEPREV;
	}
	return KONO_DISP_EXEC;
}

static bool parse_color(const char *str, float out[4]) {
	char buf[32];
	snprintf(buf, sizeof(buf), "%s", str);
	char *s = trim(buf);
	if (strncmp(s, "rgba(", 5) == 0) {
		s += 5;
		char *end = strchr(s, ')');
		if (end) {
			*end = '\0';
		}
	} else if (strncmp(s, "rgb(", 4) == 0) {
		s += 4;
		char *end = strchr(s, ')');
		if (end) {
			*end = '\0';
		}
	} else if (s[0] == '#') {
		s++;
	} else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		s += 2;
	}

	size_t len = strlen(s);
	unsigned r = 0, g = 0, b = 0, a = 255;
	if (len == 8 && sscanf(s, "%02x%02x%02x%02x", &r, &g, &b, &a) == 4) {
		/* ok */
	} else if (len == 6 && sscanf(s, "%02x%02x%02x", &r, &g, &b) == 3) {
		a = 255;
	} else {
		return false;
	}

	float af = a / 255.0f;
	out[0] = (r / 255.0f) * af;
	out[1] = (g / 255.0f) * af;
	out[2] = (b / 255.0f) * af;
	out[3] = af;
	return true;
}

static bool parse_bool(const char *s) {
	return strcasecmp(s, "true") == 0 || strcasecmp(s, "yes") == 0 || strcmp(s, "1") == 0;
}

void kono_config_defaults(struct kono_config *cfg) {
	memset(cfg, 0, sizeof(*cfg));
	cfg->gaps_in = 5;
	cfg->gaps_out = 14;
	cfg->border_size = 2;
	cfg->builtin_bar = true;
	snprintf(cfg->kb_layout, sizeof(cfg->kb_layout), "us");

	parse_color("rgba(33ccffee)", cfg->active_border);
	parse_color("rgba(595959aa)", cfg->inactive_border);
	parse_color("rgba(11111bff)", cfg->background);
	parse_color("rgba(181825ff)", cfg->bar_bg);
	parse_color("rgba(33ccffff)", cfg->bar_active);
	parse_color("rgba(45475aff)", cfg->bar_inactive);
	parse_color("rgba(89b4faff)", cfg->bar_occupied);
}

void kono_config_dir(char *out, size_t n) {
	const char *xdg = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");
	if (xdg && xdg[0]) {
		snprintf(out, n, "%s/kono", xdg);
	} else if (home) {
		snprintf(out, n, "%s/.config/kono", home);
	} else {
		snprintf(out, n, "/tmp/kono");
	}
}

static void append_unique_line(const char *path, const char *line) {
	FILE *f = fopen(path, "a+");
	if (!f) {
		return;
	}
	rewind(f);
	char buf[1024];
	bool found = false;
	while (fgets(buf, sizeof(buf), f)) {
		buf[strcspn(buf, "\r\n")] = '\0';
		if (strcmp(buf, line) == 0) {
			found = true;
			break;
		}
	}
	if (!found) {
		fprintf(f, "%s\n", line);
	}
	fclose(f);
}

static void mkdir_p(const char *path) {
	char tmp[640];
	snprintf(tmp, sizeof(tmp), "%s", path);
	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			mkdir(tmp, 0755);
			*p = '/';
		}
	}
	mkdir(tmp, 0755);
}

static void install_dolphin_action(const char *script) {
	const char *home = getenv("HOME");
	if (!home) {
		return;
	}

	const char *dirs[] = {
		"%s/.local/share/kio/servicemenus",
		"%s/.local/share/kservices5/ServiceMenus",
	};
	for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
		char dir[512], path[640];
		snprintf(dir, sizeof(dir), dirs[i], home);
		mkdir_p(dir);
		snprintf(path, sizeof(path), "%s/kono-wallpaper.desktop", dir);
		FILE *f = fopen(path, "w");
		if (!f) {
			continue;
		}
		fprintf(f,
				"[Desktop Entry]\n"
				"Type=Service\n"
				"MimeType=image/*;\n"
				"Actions=setAsWallpaper;\n"
				"X-KDE-Priority=TopLevel\n"
				"X-KDE-StartupNotify=false\n"
				"\n"
				"[Desktop Action setAsWallpaper]\n"
				"Name=Set as Kono wallpaper\n"
				"Name[ru]=Установить как обои Kono\n"
				"Icon=preferences-desktop-wallpaper\n"
				"Exec=%s %%f\n",
				script);
		fclose(f);
		chmod(path, 0755);
	}
}

void kono_ensure_dirs(void) {
	char dir[512];
	kono_config_dir(dir, sizeof(dir));
	mkdir(dir, 0755);

	char walls[640];
	snprintf(walls, sizeof(walls), "%s/wallpapers", dir);
	mkdir(walls, 0755);

	const char *home = getenv("HOME");
	if (home) {
		char local_bin[512];
		snprintf(local_bin, sizeof(local_bin), "%s/.local/bin", home);
		mkdir(local_bin, 0755);
		char script[640];
		snprintf(script, sizeof(script), "%s/kono-set-wallpaper", local_bin);

		FILE *f = fopen(script, "w");
		if (f) {
			fprintf(f,
					"#!/bin/sh\n"
					"set -e\n"
					"raw=\"${1:-}\"\n"
					"if [ -z \"$raw\" ]; then\n"
					"  echo \"usage: kono-set-wallpaper <image>\" >&2\n"
					"  exit 1\n"
					"fi\n"
					"case \"$raw\" in\n"
					"  file://*) raw=\"${raw#file://}\" ;;\n"
					"esac\n"
					"raw=$(printf '%%s' \"$raw\" | sed 's/%%20/ /g')\n"
					"file=$(realpath \"$raw\")\n"
					"cfg=\"${XDG_CONFIG_HOME:-$HOME/.config}/kono\"\n"
					"mkdir -p \"$cfg/wallpapers\"\n"
					"base=$(basename \"$file\")\n"
					"dest=\"$cfg/wallpapers/$base\"\n"
					"if [ \"$file\" != \"$dest\" ]; then\n"
					"  cp -f \"$file\" \"$dest\"\n"
					"fi\n"
					"printf '%%s\\n' \"$dest\" > \"$cfg/current-wallpaper\"\n"
					"pkill -USR1 -x kono >/dev/null 2>&1 || pkill -USR1 kono >/dev/null 2>&1 || true\n");
			fclose(f);
			chmod(script, 0755);
		}

		install_dolphin_action(script);

		char gtk3[512], gtk4[512];
		snprintf(gtk3, sizeof(gtk3), "%s/.config/gtk-3.0", home);
		snprintf(gtk4, sizeof(gtk4), "%s/.config/gtk-4.0", home);
		mkdir(gtk3, 0755);
		mkdir(gtk4, 0755);
		char bookmark[768];
		snprintf(bookmark, sizeof(bookmark), "file://%s Обои", walls);
		char b3[640], b4[640];
		snprintf(b3, sizeof(b3), "%s/bookmarks", gtk3);
		snprintf(b4, sizeof(b4), "%s/bookmarks", gtk4);
		append_unique_line(b3, bookmark);
		append_unique_line(b4, bookmark);
	}
}

static void add_bind(uint32_t mods, xkb_keysym_t key, enum kono_dispatcher disp, const char *arg) {
	if (num_binds >= KONO_MAX_BINDS || key == XKB_KEY_NoSymbol) {
		return;
	}
	binds[num_binds].mods = mods;
	binds[num_binds].keysym = key;
	binds[num_binds].dispatcher = disp;
	snprintf(binds[num_binds].arg, KONO_MAX_CMD, "%s", arg ? arg : "");
	num_binds++;
}

static void parse_hypr_bind(char *right) {
	char *parts[8];
	int n = 0;
	char *save = NULL;
	for (char *tok = strtok_r(right, ",", &save); tok && n < 8; tok = strtok_r(NULL, ",", &save)) {
		parts[n++] = trim(tok);
	}
	if (n < 2) {
		return;
	}

	uint32_t mods = parse_mods(parts[0]);
	xkb_keysym_t key = parse_key(parts[1]);
	if (n == 2) {
		add_bind(mods, key, KONO_DISP_EXEC, "");
		return;
	}

	enum kono_dispatcher disp = parse_dispatcher(parts[2]);
	char arg[KONO_MAX_CMD] = {0};
	if (n >= 4) {
		size_t off = 0;
		for (int i = 3; i < n; i++) {
			off += snprintf(arg + off, sizeof(arg) - off, "%s%s", i > 3 ? "," : "", parts[i]);
			if (off >= sizeof(arg)) {
				break;
			}
		}
	}
	if (disp == KONO_DISP_EXEC && arg[0] == '\0' && n >= 3 &&
			strcasecmp(parts[2], "exec") != 0 &&
			strcasecmp(parts[2], "killactive") != 0 &&
			strcasecmp(parts[2], "exit") != 0 &&
			strcasecmp(parts[2], "fullscreen") != 0 &&
			strcasecmp(parts[2], "togglefloating") != 0 &&
			strcasecmp(parts[2], "workspace") != 0 &&
			strcasecmp(parts[2], "movetoworkspace") != 0 &&
			strcasecmp(parts[2], "movefocus") != 0 &&
			strcasecmp(parts[2], "cyclenext") != 0 &&
			strcasecmp(parts[2], "cycleprev") != 0) {
		snprintf(arg, sizeof(arg), "%s", parts[2]);
		disp = KONO_DISP_EXEC;
	}
	add_bind(mods, key, disp, trim(arg));
	wlr_log(WLR_DEBUG, "bind: mods=0x%x key=%u disp=%d arg='%s'", mods, key, disp, arg);
}

static void parse_legacy_bind(char *right) {
	char *comma = strchr(right, ',');
	if (!comma) {
		return;
	}
	*comma = '\0';
	char *combo = trim(right);
	char *cmd = trim(comma + 1);

	char *last_plus = strrchr(combo, '+');
	char *keystr = combo;
	uint32_t mods = 0;
	if (last_plus) {
		*last_plus = '\0';
		mods = parse_mods(combo);
		keystr = last_plus + 1;
	}
	add_bind(mods, parse_key(trim(keystr)), KONO_DISP_EXEC, cmd);
}

static void apply_kv(struct kono_config *cfg, const char *section, const char *key, const char *value) {
	if (strcmp(key, "gaps_in") == 0) {
		cfg->gaps_in = atoi(value);
	} else if (strcmp(key, "gaps_out") == 0) {
		cfg->gaps_out = atoi(value);
	} else if (strcmp(key, "border_size") == 0) {
		cfg->border_size = atoi(value);
	} else if (strcmp(key, "col.active_border") == 0) {
		parse_color(value, cfg->active_border);
	} else if (strcmp(key, "col.inactive_border") == 0) {
		parse_color(value, cfg->inactive_border);
	} else if (strcmp(key, "background_color") == 0 || strcmp(key, "col.background") == 0) {
		parse_color(value, cfg->background);
	} else if (strcmp(key, "builtin_bar") == 0) {
		cfg->builtin_bar = parse_bool(value);
	} else if (strcmp(key, "kb_layout") == 0) {
		snprintf(cfg->kb_layout, sizeof(cfg->kb_layout), "%s", value);
	} else if (strcmp(key, "kb_variant") == 0) {
		snprintf(cfg->kb_variant, sizeof(cfg->kb_variant), "%s", value);
	} else if (strcmp(key, "kb_options") == 0) {
		snprintf(cfg->kb_options, sizeof(cfg->kb_options), "%s", value);
	} else if (strcmp(section, "misc") == 0 && strcmp(key, "disable_hyprland_logo") == 0) {
		(void)value;
	}
}

static void ensure_user_config(const char *src) {
	const char *home = getenv("HOME");
	if (!home) {
		return;
	}
	char dir[512], dest[640];
	snprintf(dir, sizeof(dir), "%s/.config/kono", home);
	snprintf(dest, sizeof(dest), "%s/kono.conf", dir);
	if (access(dest, R_OK) == 0) {
		return;
	}
	mkdir(dir, 0755);
	FILE *in = fopen(src, "r");
	if (!in) {
		return;
	}
	FILE *out = fopen(dest, "w");
	if (!out) {
		fclose(in);
		return;
	}
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
		fwrite(buf, 1, n, out);
	}
	fclose(in);
	fclose(out);
	wlr_log(WLR_INFO, "Installed default config at %s", dest);
}

static bool parse_config_file(struct kono_server *server, const char *path) {
	FILE *f = fopen(path, "r");
	if (!f) {
		return false;
	}

	wlr_log(WLR_INFO, "Loading config %s", path);
	char line[1024];
	char section[64] = {0};
	while (fgets(line, sizeof(line), f)) {
		char *s = trim(line);
		if (s[0] == '\0' || s[0] == '#') {
			continue;
		}
		if (s[0] == '}') {
			section[0] = '\0';
			continue;
		}

		char *brace = strchr(s, '{');
		if (brace) {
			*brace = '\0';
			snprintf(section, sizeof(section), "%s", trim(s));
			continue;
		}

		if (strncmp(s, "bind", 4) == 0 && (s[4] == ' ' || s[4] == '=' || s[4] == '\t')) {
			char *eq = strchr(s, '=');
			if (!eq) {
				continue;
			}
			char *right = trim(eq + 1);
			if (strchr(right, ',') && strchr(strchr(right, ',') + 1, ',')) {
				parse_hypr_bind(right);
			} else {
				parse_legacy_bind(right);
			}
			continue;
		}

		if (strncmp(s, "exec-once", 9) == 0) {
			char *eq = strchr(s, '=');
			if (eq && num_exec_once < KONO_MAX_EXEC) {
				snprintf(exec_once[num_exec_once++], KONO_MAX_CMD, "%s", trim(eq + 1));
			}
			continue;
		}

		char *eq = strchr(s, '=');
		if (!eq) {
			continue;
		}
		*eq = '\0';
		apply_kv(&server->config, section, trim(s), trim(eq + 1));
	}
	fclose(f);
	wlr_log(WLR_INFO, "Loaded %d keybinds", num_binds);
	return true;
}

bool load_kono_config(struct kono_server *server, const char *path) {
	num_binds = 0;
	num_exec_once = 0;
	kono_config_defaults(&server->config);

	char found[512] = {0};
	if (path && access(path, R_OK) == 0) {
		snprintf(found, sizeof(found), "%s", path);
	} else {
		const char *xdg = getenv("XDG_CONFIG_HOME");
		const char *home = getenv("HOME");
		if (xdg) {
			snprintf(found, sizeof(found), "%s/kono/kono.conf", xdg);
		} else if (home) {
			snprintf(found, sizeof(found), "%s/.config/kono/kono.conf", home);
		}
		if (found[0] == '\0' || access(found, R_OK) != 0) {
			if (access("kono.conf", R_OK) == 0) {
				ensure_user_config("kono.conf");
				if (home) {
					snprintf(found, sizeof(found), "%s/.config/kono/kono.conf", home);
				} else {
					snprintf(found, sizeof(found), "kono.conf");
				}
			}
		}
	}

	if (found[0] && access(found, R_OK) == 0) {
		return parse_config_file(server, found);
	}

	wlr_log(WLR_INFO, "No config file found, using defaults");
	add_bind(WLR_MODIFIER_LOGO, XKB_KEY_Return, KONO_DISP_EXEC, "kitty");
	add_bind(WLR_MODIFIER_LOGO, XKB_KEY_e, KONO_DISP_EXEC, "dolphin");
	add_bind(WLR_MODIFIER_LOGO, XKB_KEY_q, KONO_DISP_KILLACTIVE, "");
	add_bind(WLR_MODIFIER_LOGO, XKB_KEY_m, KONO_DISP_EXIT, "");
	add_bind(WLR_MODIFIER_LOGO, XKB_KEY_f, KONO_DISP_FULLSCREEN, "");
	add_bind(WLR_MODIFIER_LOGO, XKB_KEY_v, KONO_DISP_TOGGLEFLOATING, "");
	for (int i = 1; i <= 9; i++) {
		char n[4];
		snprintf(n, sizeof(n), "%d", i);
		add_bind(WLR_MODIFIER_LOGO, XKB_KEY_1 + (i - 1), KONO_DISP_WORKSPACE, n);
		add_bind(WLR_MODIFIER_LOGO | WLR_MODIFIER_SHIFT, XKB_KEY_1 + (i - 1),
				KONO_DISP_MOVETOWORKSPACE, n);
	}
	return false;
}

void kono_run_exec_once(void) {
	for (int i = 0; i < num_exec_once; i++) {
		kono_spawn(exec_once[i]);
	}
}

bool handle_keybinding(struct kono_server *server, xkb_keysym_t sym, uint32_t mods) {
	uint32_t cleaned = mods & (WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL |
			WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO);
	xkb_keysym_t lower = xkb_keysym_to_lower(sym);
	if (lower >= XKB_KEY_KP_1 && lower <= XKB_KEY_KP_9) {
		lower = XKB_KEY_1 + (lower - XKB_KEY_KP_1);
		sym = lower;
	}

	for (int i = 0; i < num_binds; i++) {
		if (binds[i].keysym != lower && binds[i].keysym != sym) {
			continue;
		}
		if (binds[i].mods != cleaned) {
			continue;
		}

		switch (binds[i].dispatcher) {
		case KONO_DISP_EXEC:
			if (binds[i].arg[0]) {
				kono_spawn(binds[i].arg);
			}
			break;
		case KONO_DISP_KILLACTIVE:
			kono_kill_active();
			break;
		case KONO_DISP_EXIT:
			kono_quit();
			break;
		case KONO_DISP_FULLSCREEN:
			kono_toggle_fullscreen();
			break;
		case KONO_DISP_TOGGLEFLOATING:
			kono_toggle_floating();
			break;
		case KONO_DISP_WORKSPACE:
			kono_switch_workspace_arg(binds[i].arg);
			break;
		case KONO_DISP_MOVETOWORKSPACE:
			kono_move_to_workspace(atoi(binds[i].arg));
			break;
		case KONO_DISP_MOVEFOCUS:
			kono_move_focus(binds[i].arg);
			break;
		case KONO_DISP_CYCLENEXT:
			kono_cycle_view(1);
			break;
		case KONO_DISP_CYCLEPREV:
			kono_cycle_view(-1);
			break;
		}
		(void)server;
		return true;
	}
	return false;
}
