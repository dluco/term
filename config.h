static char *config_shell = "/bin/sh";

static Shortcut shortcuts[] = {
	{ ShiftMask,				XK_Insert,	sc_paste_sel },
	{ ControlMask|ShiftMask,	XK_Insert,	sc_paste_clip },
	{ ControlMask|ShiftMask,	XK_C,		sc_copy_clip },
	{ ControlMask|ShiftMask,	XK_V,		sc_paste_clip },
};

static char *color_names[] = {
	"black",
	"red3",
	"green3",
	"yellow3",
	"blue3",
	"magenta3",
	"cyan3",
	"gray90",

	"gray30",
	"red",
	"green",
	"yellow",
	"blue",
	"magenta",
	"cyan",
	"white",
};

int color_fg = 7;
int color_bg = 0;
