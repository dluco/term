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
#include <sys/select.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/Xresource.h>
#include <pty.h>

/* Macros */
#define DEBUG(msg, ...) \
	fprintf(stderr, "DEBUG %s:%s:%d: " msg "\n", \
			__FILE__, __func__, __LINE__, ##__VA_ARGS__)
#define MIN(a, b)		((a) < (b)) ? (a) : (b)
#define MAX(a, b)		((a) > (b)) ? (a) : (b)
#define LIMIT(x, a, b)	(x) = ((x) < (a)) ? (a) : ((x) > (b)) ? (b) : (x)
#define DEFAULT(a, b)	((a) ? (a) : (b))
#define LEN(a)			(sizeof(a) / sizeof(a)[0])

#define RES_NAME		"term"
#define RES_CLASS		"Term"
#define DEFAULT_COLS	80
#define DEFAULT_ROWS	24
#define DEFAULT_FONT	"fixed"

#define XEMBED_FOCUS_IN		4
#define XEMBED_FOCUS_OUT	5

/* Enums */
enum window_state {
	WIN_VISIBLE	= 1 << 0,
	WIN_FOCUSED	= 1 << 1,
	WIN_REDRAW	= 1 << 2,
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
	Atom wmdeletewin,			/* atoms */
		 netwmpid, xembed;
	int screen;					/* display screen */
	Window parent;				/* Parent window */
	int geomask;				/* geometry mask */
	int x, y;					/* offset from top-left of screen */
	int width, height;			/* window width and height */
	int cw, ch;					/* char width and height */
	int border;					/* window border width (pixels) */
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

//int color_fg = 7;
int color_fg = 1;
int color_bg = 0;

/* Drawing context */
typedef struct {
	GC gc;
	XFont font;
	XColor colors[256];
} DC;

typedef struct {
	XFont font;
	char *colors[16];
} XResources;

/* Function prototypes */
static ssize_t swrite(int fd, const void *buf, size_t count);
static void die(char *fmt, ...);

static void tty_read(void);
static void tty_write(const char *s, size_t len);
static void tty_init(void);
static void tty_resize(int cols, int rows);

static void draw(void);
static void draw_region(int col1, int row1, int col2, int row2);
static void redraw(void);

static void term_resize(int cols, int rows);
static void term_setdirty(int top, int bottom);
static void term_fulldirty(void);
static void term_init(int cols, int rows);

static void set_title(char *title);
static void set_urgency(int urgent);
static void load_font(XFont *font, char *font_name);
static int font_max_width(XFontStruct *font_info);
static void term_clear(int col1, int row1, int col2, int row2);
static void xwindow_clear(int x1, int y1, int x2, int y2);
static void xwindow_resize(int cols, int rows);
static void x_init(void);
static void main_loop(void);
static void exec_cmd(void);
static void resize_all(int width, int height);

static char *get_resource(char *name, char *class);
static void extract_resources(void);

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
static XResources xres;
static XrmDatabase rDB;
static char *res_name = NULL;
static char *res_class = RES_CLASS;


/*
 * Write count bytes to fd.
 *
 * Safer than regular write(2), since it ensures
 * that all bytes are written to fd.
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
	//XDrawString(xw.display, xw.drawbuf, dc.gc, 0, 0, buf, len);
	//XFlush(xw.display);
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
	} else if (event->xclient.message_type == xw.xembed && event->xclient.format == 32) {
		/* XEmbed message */
		if (event->xclient.data.l[1] == XEMBED_FOCUS_IN) {
			xw.state |= WIN_FOCUSED;
			set_urgency(0);
		} else if (event->xclient.data.l[1] == XEMBED_FOCUS_OUT) {
			xw.state &= ~WIN_FOCUSED;
		}
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
		set_urgency(0);
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
	draw_region(0, 0, term.cols, term.rows);
	XCopyArea(xw.display, xw.drawbuf, xw.win, dc.gc,
			0, 0, xw.width, xw.height, 0, 0);
}

/*
 * Copy the internal terminal buffer to the window buffer,
 * within the specified region.
 * TODO
 */
static void draw_region(int col1, int row1, int col2, int row2)
{
	int row;
	//int col;

	/* Check if window is visible */
	if (!(xw.state & WIN_VISIBLE))
		return;

	for (row = row1; row < row2; row++) {
		/* Skip clean lines */
		if (!term.dirty[row])
			continue;

		/* Clear current row in buffer and reset dirtyness */
		term_clear(0, row, term.cols, row);
		term.dirty[row] = False;

		/* ... */
	}
}

/*
 * Force a complete redraw of the window.
 */
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

static void set_urgency(int urgent)
{
	XWMHints *wm_hints = XGetWMHints(xw.display, xw.win);

	// TODO
	XSetWMHints(xw.display, xw.win, wm_hints);
	XFree(wm_hints);
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
	size_hints->base_width = 2 * xw.border;
	size_hints->base_height = 2 * xw.border;
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

static void load_colors(void)
{
	int i;
	
	/* Load colors [0-15] */
	for (i = 0; i < 16; i++) {
		if (!XAllocNamedColor(xw.display, xw.colormap, color_names[i],
					&dc.colors[i], &dc.colors[i]))
			die("Failed to allocate color '%s'\n", color_names[i]);
	}

	/* Load xterm colors [16-231] */
	for (i = 16; i < 232; i++) {
		// TODO
		dc.colors[i].red = 0;
		dc.colors[i].green = 0;
		dc.colors[i].blue = 0;
		if (!XAllocColor(xw.display, xw.colormap, &dc.colors[i]))
			die("Failed to allocate color %d\n", i);
	}

	/* Load xterm (grayscale) colors [232-255] */
	for (i = 232; i < 256; i++) {
		// TODO
		dc.colors[i].red = 0;
		dc.colors[i].green = 0;
		dc.colors[i].blue = 0;
		if (!XAllocColor(xw.display, xw.colormap, &dc.colors[i]))
			die("Failed to allocate color %d\n", i);
	}
}

/*
 * Clear region of the window (column,row coordinates).
 */
static void term_clear(int col1, int row1, int col2, int row2)
{
	XSetForeground(xw.display, dc.gc, dc.colors[color_fg].pixel); // FIXME
	XFillRectangle(xw.display, xw.drawbuf, dc.gc,
			xw.border + col1 * xw.cw,
			xw.border + row1 * xw.ch,
			(col2-col1+1) * xw.cw,
			(row2-row1+1) * xw.ch);
}

/*
 * Clear region of the window (absolute x,y coordinates).
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

	cols = (xw.width - 2* xw.border) / xw.cw;
	rows = (xw.height - 2 * xw.border) / xw.ch;

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
	XrmDatabase serverDB;
	XGCValues gcvalues;
	pid_t pid = getpid();
	char *s;

	/* Open connection to X server */
	if (!(xw.display = XOpenDisplay(xw.display_name)))
		die("Cannot open X display\n");

	/* Get default screen and visual */
	xw.screen = XDefaultScreen(xw.display);
	xw.visual = XDefaultVisual(xw.display, xw.screen);

	/* Initialize rDB resources database */
	XrmInitialize();
	/* Get the resources from the server, if any */
	if ((s = XResourceManagerString(xw.display)) != NULL) {
		serverDB = XrmGetStringDatabase(s);
		XrmMergeDatabases(serverDB, &rDB);
	}
	/* Get all resources from database */
	extract_resources();

	/* Load font, in order:
	 * 1. Resource database
	 * 2. Commandline
	 * 3. Default
	 */
	dc.font.name = DEFAULT(xres.font.name, DEFAULT(dc.font.name, DEFAULT_FONT));
	load_font(&dc.font, dc.font.name);

	DEBUG("font width = %d", xw.cw);
	DEBUG("font height = %d", xw.ch);

	/* Colors */
	xw.colormap = XDefaultColormap(xw.display, xw.screen);
	load_colors();

	/* Window geometry */
	xw.width = term.cols * xw.cw + 2 * xw.border;
	xw.height = term.rows * xw.ch + 2 * xw.border;
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

	xw.parent = DEFAULT(xw.parent, XRootWindow(xw.display, xw.screen));

	xw.win = XCreateWindow(xw.display, xw.parent,
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
	xw.xembed = XInternAtom(xw.display, "_XEMBED", False);
	XSetWMProtocols(xw.display, xw.win, &xw.wmdeletewin, 1);
	XChangeProperty(xw.display, xw.win, xw.netwmpid, XA_CARDINAL, 32,
			PropModeReplace, (uchar *)&(pid), 1);

	/* (Re)set window title */
	set_title("term");

	/* Map window and set hints */
	XMapWindow(xw.display, xw.win);
	set_hints();

	XSync(xw.display, False);
}

/*
 * Extract the named resource from the database and
 * return a pointer to the static string containing it.
 */
static char *get_resource(char *name, char *class)
{
	static char resource[256];
	char str_name[256], str_class[256];
	XrmValue value;
	char *str_type;

	sprintf(str_name, "%s.%s", res_name, name);
	sprintf(str_class, "%s.%s", res_class, class);

	if (XrmGetResource(rDB, str_name, str_class, &str_type, &value) == True) {
		strncpy(resource, value.addr, MIN((int)value.size, sizeof(resource)));
		return resource;
	}

	return NULL;
}

/*
 * Search and load all applicable resources from the
 * resource database.
 */
static void extract_resources(void)
{
	char *s, color_name[16], color_class[16];
	uint cols, rows;
	int i;

	/* Resources that cannot be applied immediately */

	/* Font resource */
	if ((s = get_resource("font", "Font")) != NULL) {
		xres.font.name = strdup(s);
	}

	/* Color resources [0-15] */
	for (i = 0; i < 16; i++) {
		sprintf(color_name, "color%d", i);
		sprintf(color_class, "Color%d", i);

		DEBUG("%s, %s", color_name, color_class);
		if ((s = get_resource(color_name, color_class)) != NULL) {
			DEBUG("%s: %s", color_name, s);
			xres.colors[i] = strdup(s);
		}
	}

	/* Resources that can be applied immediately */

	/* Border width resource */
	if ((s = get_resource("borderWidth", "BorderWidth")) != NULL) {
		xw.border = atoi(s);
	}

	/* Geometry resource */
	if ((s = get_resource("geometry", "Geometry")) != NULL) {
		xw.geomask = XParseGeometry(s, &xw.x, &xw.y, &cols, &rows);
		term_resize(cols, rows);
	}
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
 * TODO
 */
void main_loop(void)
{
	XEvent event;
	int width = xw.width;
	int height = xw.height;
	fd_set read_fds;
	struct timespec *tv = NULL; // Block indefinitely

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

	//sleep(1);

	while (1) {
		/* Reset file descriptor set */
		FD_ZERO(&read_fds);
		FD_SET(tty.fd, &read_fds);

		/* Check if fd(s) are ready to be read */
		if (pselect(tty.fd+1, &read_fds, NULL, NULL, tv, NULL) < 0) {
			if (errno == EINTR)
				continue; // Interrupted
			die("pselect failed: %s\n", strerror(errno));
		}

		if (FD_ISSET(tty.fd, &read_fds)) {
			/* Read from tty device */
			//tty_read();
		}

		XDrawLine(xw.display, xw.win, dc.gc, 0, 0, 20, 20);

		/* Process all pending events */
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
 * TODO
 */
static void exec_cmd(void)
{
	char *prog, **args;

	if (!(prog = getenv("SHELL")))
		die("Failed to get shell name\n");

	args = (char *[]) {prog, NULL};

	/* Default signal handlers */
	signal(SIGALRM, SIG_DFL);
	signal(SIGCHLD, SIG_DFL);
	signal(SIGHUP, SIG_DFL);
	signal(SIGINT, SIG_DFL);
	signal(SIGQUIT, SIG_DFL);
	signal(SIGTERM, SIG_DFL);

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
	char *p;
	int i;
	xw.display_name = NULL;
	xw.parent = None;
	dc.font.name = NULL;

	/* FIXME: Parse options and arguments */
	for (i = 1; i < argc; i++) {
		if (strncmp(argv[i], "-f", 3) == 0 && (i+1) < argc)
			dc.font.name = argv[++i];
		else if (strncmp(argv[i], "-d", 3) == 0 && (i+1) < argc)
			xw.display_name = argv[++i];
		else if (strncmp(argv[i], "-g", 3) == 0 && (i+1) < argc)
			xw.geomask = XParseGeometry(argv[++i], &xw.x, &xw.y, &cols, &rows);
		else if (strncmp(argv[i], "-w", 3) == 0 && (i+1) < argc)
			xw.parent = strtol(argv[++i], NULL, 0);
		else if (strncmp(argv[i], "-n", 3) == 0 && (i+1) < argc)
			res_name = argv[++i];
		else if (strncmp(argv[i], "-c", 3) == 0 && (i+1) < argc)
			res_class = argv[++i];
	}

	if (!res_name) {
		res_name = (p = strrchr(argv[0], '/')) ? (p+1) : RES_NAME;
	}
	DEBUG("resource name = %s", res_name);
	DEBUG("resource class = %s", res_class);

	DEBUG("display name = %s", XDisplayName(xw.display_name));
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
