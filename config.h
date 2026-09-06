#ifndef KONO_CONFIG_H
#define KONO_CONFIG_H

#include "server.h"

#define KONO_MAX_BINDS 256
#define KONO_MAX_EXEC 64
#define KONO_MAX_CMD 512

enum kono_dispatcher {
	KONO_DISP_EXEC,
	KONO_DISP_KILLACTIVE,
	KONO_DISP_EXIT,
	KONO_DISP_FULLSCREEN,
	KONO_DISP_TOGGLEFLOATING,
	KONO_DISP_WORKSPACE,
	KONO_DISP_MOVETOWORKSPACE,
	KONO_DISP_MOVEFOCUS,
	KONO_DISP_CYCLENEXT,
	KONO_DISP_CYCLEPREV,
};

struct kono_bind {
	uint32_t mods;
	xkb_keysym_t keysym;
	enum kono_dispatcher dispatcher;
	char arg[KONO_MAX_CMD];
};

#endif
