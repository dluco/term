#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <locale.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <pty.h>

/* Macros */
#define DEBUG(msg, ...) \
	fprintf(stderr, "DEBUG %s:%s:%d: " msg "\n", \
			__FILE__, __func__, __LINE__, ##__VA_ARGS__)
#define MIN(a, b)		((a) < (b)) ? (a) : (b)
#define MAX(a, b)		((a) > (b)) ? (a) : (b)
#define LIMIT(x, a, b)	(x) = ((x) < (a)) ? (a) : ((x) > (b)) ? (b) : (x)
#define LEN(a)			(sizeof(a) / sizeof(a)[0])

#define DEFAULT_COLS 80
#define DEFAULT_ROWS 24
#define DEFAULT_FONT "fixed"

/* Enums */
enum window_state {
	WIN_VISIBLE	= 1 << 0,
	WIN_FOCUSED	= 1 << 1,
	WIN_REDRAW	= 1 << 2,
};

enum color_class {
	COLOR0	= 0,
	COLOR1	= 1,
	COLOR2	= 2,
	COLOR3	= 3,
	COLOR4	= 4,
	COLOR5	= 5,
	COLOR6	= 6,
	COLOR7	= 7,
	COLOR8	= 8,
	COLOR9	= 9,
	COLOR10	= 10,
	COLOR11	= 11,
	COLOR12	= 12,
	COLOR13	= 13,
	COLOR14	= 14,
	COLOR15	= 15,
	COLORUL	= 16,
	COLORBD	= 17,
};

/* Typedefs for types */
typedef unsigned char uchar;
typedef unsigned int uint;
typedef unsigned long ulong;
typedef unsigned short ushort;

/* Structs */
typedef struct {
	pid_t pid;			/* PID of slave pty */
	int fd;				/* fd of process running in pty */
	struct winsize ws;	/* window size struct (for openpty and ioctl) */
} TTY;

/* Internal representation of screen */
typedef struct {
	int rows;		/* number of rows */
	int cols;		/* number of columns */
	Bool *dirty;	/* dirtyness of lines */
} Term;

typedef struct {
	Display *display;			/* X display */
	Window win;					/* X window */
	Drawable drawbuf;			/* drawing buffer */
	Visual *visual;				/* default visual */
	Colormap colormap;			/* default colormap */
	XSetWindowAttributes attrs;	/* window attributes */
	Atom wmdeletewin, netwmpid;	/* atoms */
	int screen;					/* display screen */
	int geomask;				/* geometry mask */
	int x, y;					/* offset from top-left of screen */
	int width, height;			/* window width and height */
	int cw, ch;					/* char width and height */
	char *display_name;			/* name of display */
	int state;					/* window state: visible, focused */
} XWindow;

/* Font structure */
typedef struct {
	XFontStruct *font_info;
	int width;
	int height;
	char *name;
} XFont;

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

/* Drawing context */
typedef struct {
	XFont font;
	XColor colors[MAX(LEN(color_names), 256)];
	GC gc;
} DC;

/* Function prototypes */
static ssize_t swrite(int fd, const void *buf, size_t count);
static void die(char *fmt, ...);

static void tty_read(void);
static void tty_write(const char *s, size_t len);
static void tty_init(void);
static void tty_resize(int cols, int rows);

static void draw(void);
static void redraw(void);

static void term_resize(int cols, int rows);
static void term_setdirty(int top, int bottom);
static void term_fulldirty(void);
static void term_init(int cols, int rows);
static void set_title(char *title);
static void load_font(XFont *font, char *font_name);
static int font_max_width(XFontStruct *font_info);
static void xwindow_clear(int x1, int y1, int x2, int y2);
static void xwindow_resize(int cols, int rows);
static void x_init(void);
static void main_loop(void);
static void exec_cmd(void);
static void resize_all(int width, int height);

static void event_keypress(XEvent *event);
static void event_cmessage(XEvent *event);
static void event_resize(XEvent *event);
static void event_expose(XEvent *event);
static void event_focus(XEvent *event);
static void event_unmap(XEvent *event);
static void event_visibility(XEvent *event);

static int geomask_to_gravity(int mask);

/* Event handlers */
static void (*event_handler[LASTEvent])(XEvent *) = {
	[KeyPress] = event_keypress,
	[ClientMessage] = event_cmessage,
	[ConfigureNotify] = event_resize,
	[Expose] = event_expose,
	[FocusIn] = event_focus,
	[FocusOut] = event_focus,
	[UnmapNotify] = event_unmap,
	[VisibilityNotify] = event_visibility,
};

/* Globals */
static TTY tty;
static XWindow xw;
static Term term;
static DC dc;


/*
 * Write count bytes to fd.
 *
 * Safer than regular write(2), since it makes sure
 * that all bytes are written.
 */
static ssize_t swrite(int fd, const void *buf, size_t count)
{
	size_t aux = count;

	while (count > 0) {
		ssize_t r = write(fd, buf, count);
		if (r < 0)
			return r;
		count -= r;
		buf += r;
	}
	return aux;
}

/*
 * Print an error message and exit.
 */
static void die(char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	exit(EXIT_FAILURE);
}

/*
 * Read from the tty.
 */
static void tty_read(void)
{
	char buf[256];
	int len;

	if ((len = read(tty.fd, buf, sizeof buf)) < 0)
		die("Failed to read from shell: %s\n", strerror(errno));

	// FIXME
	DEBUG("%s", buf);
	XDrawString(xw.display, xw.drawbuf, dc.gc, 0, 0, buf, len);
	XFlush(xw.display);
}

/*
 * Write a string to the tty.
 */
static void tty_write(const char *s, size_t len)
{
	if (swrite(tty.fd, s, len) == -1)
		die("write error on tty: %s\n", strerror(errno));
}

/*
 * Resize the tty.
 */
static void tty_resize(int cols, int rows)
{
	/* Update fields in winsize struct */
	tty.ws.ws_row = rows;
	tty.ws.ws_col = cols;
	tty.ws.ws_xpixel = cols * xw.cw;
	tty.ws.ws_ypixel = rows * xw.ch;

	if (ioctl(tty.fd, TIOCSWINSZ, &tty.ws) < 0)
		fprintf(stderr, "Unable to set window size: %s\n",
				strerror(errno));
}

/*
 * KeyPress event handler.
 */
static void event_keypress(XEvent *event)
{
	XKeyEvent *key_event = &event->xkey;
	KeySym keysym;
	char buf[32];
	int len;

	len = XLookupString(key_event, buf, sizeof buf, &keysym, NULL);
	if (len == 0)
		return;

	DEBUG("key pressed: %s", buf);

	// FIXME
	tty_write(buf, len);
}

/*
 * ClientMessage event handler.
 */
static void event_cmessage(XEvent *event)
{
	/* Destroy window */
	if (event->xclient.data.l[0] == xw.wmdeletewin) {
		XCloseDisplay(xw.display);
		exit(EXIT_SUCCESS);
	}
}

/*
 * ConfigureNotify event handler.
 */
static void event_resize(XEvent *event)
{
	/* Only resize if size was actually changed */
	if (event->xconfigure.width == xw.width && event->xconfigure.height == xw.height)
		return;

	resize_all(event->xconfigure.width, event->xconfigure.height);
}

/*
 * Expose event handler.
 */
static void event_expose(XEvent *event)
{
	XExposeEvent *xexpose = &event->xexpose;

	if (xw.state & WIN_REDRAW) {
		DEBUG("HERE");
		if (xexpose->count == 0)
			xw.state &= ~WIN_REDRAW;
	}

	redraw();
}

static void event_focus(XEvent *event)
{
	XFocusChangeEvent *xfocus = &event->xfocus;

	if (xfocus->mode == NotifyGrab)
		return;

	if (event->type == FocusIn) {
		xw.state |= WIN_FOCUSED;
		DEBUG("FOCUS IN");
	} else {
		xw.state &= ~WIN_FOCUSED;
		DEBUG("FOCUS OUT");
	}
}

/*
 * UnmapNotify event handler.
 */
static void event_unmap(XEvent *event)
{
	/* Unset visibility bit of state mask */
	xw.state &= ~WIN_VISIBLE;
}

/*
 * VisibilityNotify event handler.
 */
static void event_visibility(XEvent *event)
{
	XVisibilityEvent *xvisibility = &event->xvisibility;

	if (xvisibility->state == VisibilityFullyObscured) {
		/* Unset visibility bit of state mask */
		xw.state &= ~WIN_VISIBLE;
	} else if (!(xw.state & WIN_VISIBLE)) {
		/* XXX */
		xw.state |= WIN_VISIBLE | WIN_REDRAW;
	}
}

/*
 * Draw the buffer into the window.
 * TODO
 */
static void draw(void)
{
	XCopyArea(xw.display, xw.drawbuf, xw.win, dc.gc,
			0, 0, xw.width, xw.height, 0, 0);
}

static void redraw(void)
{
	term_fulldirty();
	draw();
}

/*
 * Resize terminal (internal).
 */
static void term_resize(int cols, int rows)
{
	/* Reallocate height dependent elements */
	term.dirty = realloc(term.dirty, rows * sizeof *term.dirty);

	/* Update terminal size */
	term.cols = cols;
	term.rows = rows;
}

/*
 * Set term rows in range [top,bottom] as dirty.
 */
static void term_setdirty(int top, int bottom)
{
	int i;

	LIMIT(top, 0, term.rows-1);
	LIMIT(bottom, 0, term.rows-1);

	for (i = top; i <= bottom; i++)
		term.dirty[i] = True;
}

/*
 * Set all term rows as dirty.
 */
static void term_fulldirty(void)
{
	term_setdirty(0, term.rows-1);
}

/*
 * Set the title of the window.
 */
static void set_title(char *title)
{
	XTextProperty prop;

	XStringListToTextProperty(&title, 1, &prop);
	XSetWMName(xw.display, xw.win, &prop);
	XFree(prop.value);
}

/*
 * Set size hints for window.
 */
static void set_hints(void)
{
	XSizeHints *size_hints;
	XWMHints *wm_hints;
	XClassHint *class_hints;

	/* Size hints */
	if (!(size_hints = XAllocSizeHints())) {
		die("Failed to allocate window size hints\n");
	}

	/* WM hints */
	if (!(wm_hints = XAllocWMHints())) {
		die("Failed to allocate window wm hints\n");
	}

	/* Class hints */
	if (!(class_hints = XAllocClassHint())) {
		die("Failed to allocate window wm hints\n");
	}

	size_hints->flags = PSize | PBaseSize | PResizeInc;
	size_hints->width = xw.width;
	size_hints->height = xw.height;
	size_hints->base_width = 0; // TODO: replace with 2 * border
	size_hints->base_height = 0; // TODO: replace with 2 * border
	size_hints->width_inc = xw.cw;
	size_hints->height_inc = xw.ch;
	
	/* Set position and gravity elements */
	if (xw.geomask & (XValue|YValue)) {
		size_hints->flags |= USPosition | PWinGravity;
		size_hints->x = xw.x;
		size_hints->y = xw.y;
		size_hints->win_gravity = geomask_to_gravity(xw.geomask);
	}

	wm_hints->flags = InputHint;
	wm_hints->input = True;

	class_hints->res_name = "term";
	class_hints->res_class = "Term";

	XSetWMNormalHints(xw.display, xw.win, size_hints);
	XSetWMHints(xw.display, xw.win, wm_hints);
	XSetClassHint(xw.display, xw.win, class_hints);

	XFree(size_hints);
	XFree(wm_hints);
	XFree(class_hints);
}

/*
 * Load the specified font, and get width & height.
 */
static void load_font(XFont *font, char *font_name)
{
	/* Try to load and retrieve font structuce */
	if (!(font->font_info = XLoadQueryFont(xw.display, font_name))) {
		die("Failed to load font '%s'\n", font_name);
	}
	/* Get (max) font dimensions */
	font->width = font_max_width(font->font_info);
	font->height = font->font_info->ascent + font->font_info->descent;

	xw.cw = font->width;
	xw.ch = font->height;
}

/*
 * Get widest character in specified font.
 *
 * Courtesy: rxvt-2.7.10/src/main.c
 */
static int font_max_width(XFontStruct *font)
{
	int i, width = 0;

	if (font->min_bounds.width == font->max_bounds.width)
		return font->min_bounds.width;
	if (font->per_char == NULL)
		return font->max_bounds.width;

	for (i = (font->max_char_or_byte2 - font->min_char_or_byte2); i >= 0; i--) {
		width = MAX(width, font->per_char[i].width);
	}

	return width;
}

static void load_color(XColor *color, char *color_name)
{
	if (!XParseColor(xw.display, xw.colormap, color_name, color))
		die("Failed to parse color '%s'\n", color_name);

	if (!XAllocColor(xw.display, xw.colormap, color))
		die("Failed to allocate color '%s'\n", color_name);
}

static void load_colors(void)
{
	int i;
	
	for (i = 0; i < LEN(color_names); i++) {
		load_color(&dc.colors[i], color_names[i]);
	}

	for (i = 16; i < 232; i++) {
		// TODO
		dc.colors[i].red = 0;
		dc.colors[i].green = 0;
		dc.colors[i].blue = 0;
		if (!XAllocColor(xw.display, xw.colormap, &dc.colors[i]))
			die("error!");
	}

	for (i = 232; i < 256; i++) {
		// TODO
		continue;
	}
}

/*
 * Clear region of the window, resetting to the background color.
 */
static void xwindow_clear(int x1, int y1, int x2, int y2)
{
	XSetForeground(xw.display, dc.gc, dc.colors[color_fg].pixel); // FIXME
	XFillRectangle(xw.display, xw.drawbuf, dc.gc, x1, y1, x2-x1, y2-y1);
}

/*
 * Resize X window.
 */
static void xwindow_resize(int cols, int rows)
{
	/* Update drawbuf to new dimensions */
	XFreePixmap(xw.display, xw.drawbuf);
	xw.drawbuf = XCreatePixmap(xw.display, xw.win, xw.width, xw.height,
			DefaultDepth(xw.display, xw.screen));

	xwindow_clear(0, 0, xw.width, xw.height);
}

/*
 * Resize terminal and X window.
 */
static void resize_all(int width, int height)
{
	int cols, rows;

	if (width != 0)
		xw.width = width;
	if (height != 0)
		xw.height = height;

	cols = (xw.width) / xw.cw;
	rows = (xw.height) / xw.ch;

	/* Resize term (internal), X window, and tty */
	term_resize(cols, rows);
	xwindow_resize(cols, rows);
	tty_resize(cols, rows);

	DEBUG("Window resized: width = %d, height = %d, cols=%d, rows=%d",
			xw.width, xw.height, cols, rows);
}

/*
 * Convert a geometry mask to a gravity.
 */
static int geomask_to_gravity(int mask)
{
	switch (mask & (XNegative|YNegative)) {
	case 0:
		return NorthWestGravity;
	case XNegative:
		return NorthEastGravity;
	case YNegative:
		return SouthWestGravity;
	}
	return SouthEastGravity;
}

/*
 * Initialize all X related items.
 */
static void x_init(void)
{
	XGCValues gcvalues;
	pid_t pid = getpid();

	/* Open connection to X server */
	if (!(xw.display = XOpenDisplay(xw.display_name)))
		die("Cannot open X display\n");

	/* Get default screen and visual */
	xw.screen = XDefaultScreen(xw.display);
	xw.visual = XDefaultVisual(xw.display, xw.screen);

	/* Load font */
	/* TODO: dynamically load font */
	load_font(&dc.font, DEFAULT_FONT);
	DEBUG("font width = %d", xw.cw);
	DEBUG("font height = %d", xw.ch);

	/* Colors */
	xw.colormap = XDefaultColormap(xw.display, xw.screen);
	//load_color(&dc.color, "#ff00ff");
	//load_color(&dc.color, "blue");

	/*
	for (i = 0; i < LEN(color_names); i++) {
		load_color(&dc.colors[i], color_names[i]);
	}
	*/
	load_colors();

	/* Window geometry */
	xw.width = term.cols * xw.cw;
	xw.height = term.rows * xw.ch;
	/* Convert negative coordinates to absolute */
	if (xw.geomask & XNegative)
		xw.x += DisplayWidth(xw.display, xw.screen) - xw.width;
	if (xw.geomask & YNegative)
		xw.y += DisplayHeight(xw.display, xw.screen) - xw.height;

	/* Window attributes */
	xw.attrs.background_pixel = BlackPixel(xw.display, xw.screen);
	xw.attrs.border_pixel = BlackPixel(xw.display, xw.screen);
	xw.attrs.colormap = xw.colormap;
	xw.attrs.bit_gravity = NorthWestGravity;
	xw.attrs.event_mask = ExposureMask | KeyPressMask |
		StructureNotifyMask | VisibilityChangeMask | FocusChangeMask; // TODO

	xw.win = XCreateWindow(xw.display, XRootWindow(xw.display, xw.screen),
			xw.x, xw.y, xw.width, xw.height, 0,
			XDefaultDepth(xw.display, xw.screen), InputOutput,
			xw.visual, CWBackPixel | CWBorderPixel | CWBitGravity
			| CWEventMask | CWColormap, &xw.attrs);

	/* Window drawing buffer */
	xw.drawbuf = XCreatePixmap(xw.display, xw.win, xw.width, xw.height,
			DefaultDepth(xw.display, xw.screen));

	/* Graphics context */
	gcvalues.graphics_exposures = False;
	dc.gc = XCreateGC(xw.display, XRootWindow(xw.display, xw.screen),
			GCGraphicsExposures, &gcvalues);
	/* Fill buffer with background color */
	XSetForeground(xw.display, dc.gc, dc.colors[color_fg].pixel);
	XFillRectangle(xw.display, xw.drawbuf, dc.gc, 0, 0, xw.width, xw.height);

	/* Get atom(s) */
	xw.wmdeletewin = XInternAtom(xw.display, "WM_DELETE_WINDOW", False);
	xw.netwmpid = XInternAtom(xw.display, "_NET_WM_PID", False);
	XSetWMProtocols(xw.display, xw.win, &xw.wmdeletewin, 1);
	XChangeProperty(xw.display, xw.win, xw.netwmpid, XA_CARDINAL, 32,
			PropModeReplace, (uchar *)&(pid), 1);

	/* (Re)set window title */
	set_title("term");

	/* Map window and set hints */
	XMapWindow(xw.display, xw.win);
	set_hints();

	XSync(xw.display, False);

	return;
}

/*
 * Initialize Term.
 */
static void term_init(int cols, int rows)
{
	/* Set initial size, and force allocation
	 * of internal structures. */
	term_resize(cols, rows);
}

/*
 * Main loop of terminal.
 */
void main_loop(void)
{
	XEvent event;
	int width = xw.width;
	int height = xw.height;

	/* Wait for window to be mapped */
	while (1) {
		XNextEvent(xw.display, &event);

		if (XFilterEvent(&event, None))
			continue;
		if (event.type == ConfigureNotify) {
			/* Get size of mapped window */
			width = event.xconfigure.width;
			height = event.xconfigure.height;
		} else if (event.type == MapNotify) {
			break;
		}

	}

	/* Set up tty and exec command */
	tty_init();

	/* XXX DEBUG XXX */
	DEBUG("width = %d", width);
	DEBUG("height = %d", height);
	DEBUG("cols = %d", term.cols);
	DEBUG("rows = %d", term.rows);

	XSetForeground(xw.display, dc.gc, dc.colors[color_fg].pixel);

	sleep(1);

	while (1) {

		XDrawLine(xw.display, xw.win, dc.gc, 0, 0, 20, 20);

		//tty_read();

		/* Process all events */
		while (XPending(xw.display)) {
			XNextEvent(xw.display, &event);
			if (XFilterEvent(&event, None))
				continue;
			/* Search event handlers for event type */
			if (event_handler[event.type])
				(event_handler[event.type])(&event);
		}

		sleep(1);
	}
}

/*
 * Exec shell or command.
 */
static void exec_cmd(void)
{
	char *prog, **args;

	if (!(prog = getenv("SHELL")))
		die("Failed to get shell name\n");

	args = (char *[]) {prog, NULL};

	execvp(prog, args);
}

/*
 * SIGCHLD signal handler.
 */
void sigchld(int signal)
{
	int status, ret;

	if (waitpid(tty.pid, &status, 0) < 0)
		die("Waiting for pid %hd failed: %s\n", tty.pid, strerror(errno));

	ret = WIFEXITED(status) ? WEXITSTATUS(status) : EXIT_FAILURE;
	if (ret != EXIT_SUCCESS)
		die("child exited with error '%d'\n", status);
	exit(EXIT_SUCCESS);
}

/*
 * Initialize pty master and slave.
 */
static void tty_init(void)
{
	int master, slave;
	struct winsize winp = {term.rows, term.cols, 0, 0};

	/* Open an available pseudoterminal */
	if (openpty(&master, &slave, NULL, NULL, &winp) < 0)
		die("failed to open pty: %s\n", strerror(errno));

	switch (tty.pid = fork()) {
	case -1:
		die("fork failed\n");
		break;
	case 0:		/* CHILD */
		/* Create a new process group */
		setsid(); 
		/* Duplicate file descriptors */
		dup2(slave, STDIN_FILENO);
		dup2(slave, STDOUT_FILENO);
		dup2(slave, STDERR_FILENO);
		/* Become the controlling terminal */
		if (ioctl(slave, TIOCSCTTY, NULL) < 0)
			die("ioctl TIOCSTTY failed: %s\n", strerror(errno));
		/* Close master and slave */
		close(slave);
		close(master);
		/* Exec command */
		exec_cmd();
		break;
	default:	/* PARENT */
		/* Close slave */
		close(slave);
		tty.fd = master;
		tty.ws = winp;
		signal(SIGCHLD, sigchld);
		break;
	}
}

int main(int argc, char *argv[])
{
	uint cols = DEFAULT_COLS, rows = DEFAULT_ROWS;
	xw.display_name = NULL;
	int i;

	/* FIXME: Parse options and arguments */
	for (i = 1; i < argc; i++) {
		if (strncmp(argv[i], "-d", 3) == 0 && (i+1) < argc)
			xw.display_name = argv[++i];
		else if (strncmp(argv[i], "-g", 3) == 0 && (i+1) < argc)
			xw.geomask = XParseGeometry(argv[++i], &xw.x, &xw.y, &cols, &rows);
	}

	DEBUG("display name = %s",
			XDisplayName(xw.display_name));
	DEBUG("cols = %d", cols);
	DEBUG("rows = %d", rows);

	/* Set up locale */
	setlocale(LC_CTYPE, "");
	XSetLocaleModifiers("");

	term_init(cols, rows);

	x_init();

	main_loop();

	return 0;
}
