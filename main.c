#define _POSIX_C_SOURCE 200809L
#include "server.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wlr/util/log.h>

static FILE *kono_logf;

static void kono_log_handler(enum wlr_log_importance verbosity, const char *fmt, va_list args) {
	(void)verbosity;
	va_list copy;
	va_copy(copy, args);
	vfprintf(stderr, fmt, args);
	fputc('\n', stderr);
	if (kono_logf) {
		vfprintf(kono_logf, fmt, copy);
		fputc('\n', kono_logf);
		fflush(kono_logf);
	}
	va_end(copy);
}

static void kono_open_log(void) {
	const char *home = getenv("HOME");
	const char *state = getenv("XDG_STATE_HOME");
	char dir[512];
	if (state && state[0]) {
		snprintf(dir, sizeof(dir), "%s/kono", state);
	} else if (home) {
		snprintf(dir, sizeof(dir), "%s/.local/state/kono", home);
	} else {
		return;
	}
	mkdir(dir, 0755);
	char path[640];
	snprintf(path, sizeof(path), "%s/kono.log", dir);
	kono_logf = fopen(path, "a");
	if (kono_logf) {
		fprintf(kono_logf, "---- Kono starting ----\n");
		fflush(kono_logf);
	}
}

int main(int argc, char *argv[]) {
	if (argc > 0 && argv[0]) {
		char *slash = strrchr(argv[0], '/');
		char *base = slash ? slash + 1 : argv[0];
		if (strcmp(base, "kono") == 0) {
			base[0] = 'K';
		}
	}

	if (!getenv("WLR_RENDERER")) {
		setenv("WLR_RENDERER", "gles2", 1);
	}
	if (!getenv("XCURSOR_THEME")) {
		setenv("XCURSOR_THEME", "Adwaita", 1);
	}
	if (!getenv("XCURSOR_SIZE")) {
		setenv("XCURSOR_SIZE", "24", 1);
	}

	kono_open_log();
	wlr_log_init(WLR_INFO, kono_log_handler);

	struct kono_server server = {0};
	if (!server_init(&server)) {
		return EXIT_FAILURE;
	}

	server_run(&server);
	server_fini(&server);
	if (kono_logf) {
		fprintf(kono_logf, "---- Kono stopped ----\n");
		fclose(kono_logf);
	}
	return EXIT_SUCCESS;
}
