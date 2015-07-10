static char *config_shell = "/bin/sh";

static Shortcut shortcuts[] = {
	{ ShiftMask,				XK_Insert,	sc_paste_sel },
	{ ControlMask|ShiftMask,	XK_Insert,	sc_paste_clip },
	{ ControlMask|ShiftMask,	XK_C,		sc_copy_clip },
	{ ControlMask|ShiftMask,	XK_V,		sc_paste_clip },
};
