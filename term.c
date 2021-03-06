#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <locale.h>
#include <limits.h>
#include <errno.h>
#include <unistd.h>
#include <pwd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/select.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/Xresource.h>
#include <pty.h>

#include "term.h"

static char *argv0;

/* Macros */
#define DEBUG(msg, ...) \
	fprintf(stderr, "DEBUG %s:%s:%d: " msg "\n", \
			__FILE__, __func__, __LINE__, ##__VA_ARGS__)

#define debug(level, fmt, ...) \
	{if ((level) <= DEBUG_LEVEL) warn(fmt, ##__VA_ARGS__);}

#define MIN(a, b)		((a) < (b)) ? (a) : (b)
#define MAX(a, b)		((a) > (b)) ? (a) : (b)
#define LIMIT(x, a, b)	((x) = ((x) < (a)) ? (a) : ((x) > (b)) ? (b) : (x))
#define DEFAULT(a, b)	((a) ? (a) : (b))
#define LEN(a)			(sizeof(a) / sizeof(a)[0])
#define MODBIT(x, set, bit) ((set) ? ((x) |= (bit)) : ((x) &= ~(bit)))

#define RES_NAME		"term"
#define RES_CLASS		"Term"
#define DEFAULT_COLS	80
#define DEFAULT_ROWS	24
#define DEFAULT_FONT	"fixed"

#define MAX_TARGETS		6

#define XEMBED_FOCUS_IN		4
#define XEMBED_FOCUS_OUT	5

#define XK_ANY_MOD	UINT_MAX

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

typedef struct {
	int x;
	int y;
} Coord;

/* Internal representation of screen */
typedef struct {
	int rows;		/* number of rows */
	int cols;		/* number of columns */
	Coord cursor;	/* position of cursor */
	char **line;	/* lines */
	Bool *dirty;	/* dirtyness of lines */
} Term;

/* Visual representation of screen */
typedef struct {
	Display *display;			/* X display */
	Window win;					/* X window */
	Drawable drawbuf;			/* drawing buffer */
	Visual *visual;				/* default visual */
	Colormap colormap;			/* default colormap */
	XSetWindowAttributes attrs;	/* window attributes */
	XIM xim;					/* X input method */
	XIC xic;					/* X input context */
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
	XFontSet font_set;
	int ascent, descent;
	int width, height;
	char *name;
} XFont;

typedef struct {
	char *primary, *clipboard;
	Time sel_time, clip_time;
	Atom target;
} Selection;

typedef struct {
	uint mod;
	KeySym keysym;
	void (*func)(XKeyEvent *xkey);
} Shortcut;

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
static size_t sstrlen(const char *s);
static void die(const char *fmt, ...);

static void tty_read(void);
static void tty_write(const char *s, size_t len);
static void tty_init(void);
static void tty_resize(int cols, int rows);

static void draw(void);
static void draw_region(int col1, int row1, int col2, int row2);
static void redraw(void);

static void term_putc(char c);
static void term_moveto(int x, int y);
static void term_resize(int cols, int rows);
static void term_clear(int x1, int y1, int x2, int y2);
static void term_setdirty(int top, int bottom);
static void term_fulldirty(void);
static void term_reset(void);
static void term_init(int cols, int rows);

static void sel_init(void);
static void sel_convert(Atom selection, Time time);
static Bool sel_own(Atom selection, Time time);
static void sel_copy(Time time);

static void set_title(char *title);
static void set_urgency(int urgent);
static void load_font(XFont *font, char *font_name);
static void xwindow_clear(int col1, int row1, int col2, int row2);
static void xwindow_abs_clear(int x1, int y1, int x2, int y2);
static void xwindow_resize(int cols, int rows);
static void x_init(void);
static void main_loop(void);
static void exec_cmd(void);
static void resize_all(int width, int height);

static char *get_resource(char *name, char *class);
static void extract_resources(void);

static void event_keypress(XEvent *event);
static void event_brelease(XEvent *e);
static void event_cmessage(XEvent *event);
static void event_resize(XEvent *event);
static void event_expose(XEvent *event);
static void event_focus(XEvent *event);
static void event_unmap(XEvent *event);
static void event_visibility(XEvent *event);
static void event_selnotify(XEvent *event);
static void event_selrequest(XEvent *event);
static void event_selclear(XEvent *event);

static void sc_paste_sel(XKeyEvent *xkey);
static void sc_paste_clip(XKeyEvent *xkey);
static void sc_copy_clip(XKeyEvent *xkey);

static int geomask_to_gravity(int mask);

/* Event handlers */
static void (*event_handler[LASTEvent])(XEvent *) = {
	[KeyPress] = event_keypress,
	[ButtonRelease] = event_brelease,
	[ClientMessage] = event_cmessage,
	[ConfigureNotify] = event_resize,
	[Expose] = event_expose,
	[FocusIn] = event_focus,
	[FocusOut] = event_focus,
	[UnmapNotify] = event_unmap,
	[VisibilityNotify] = event_visibility,
	[SelectionNotify] = event_selnotify,
	[SelectionRequest] = event_selrequest,
	[SelectionClear] = event_selclear,
};

#include "config.h"

/* Globals */
static TTY tty;
static XWindow xw;
static Term term;
static Selection sel;
static DC dc;
static XResources xres;
static XrmDatabase rDB;
static char *res_name = NULL;
static char *res_class = RES_CLASS;
static char **cmd = NULL;

static Atom wmdeletewin_atom;
static Atom xembed_atom;
static Atom clipboard_atom;
static Atom timestamp_atom;
static Atom targets_atom;
static Atom text_atom;
static Atom utf8_atom;

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

static size_t sstrlen(const char *s)
{
	if (s == NULL)
		return 0;

	return strlen(s);
}

static void die(const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "%s: ", argv0);

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	
	fprintf(stderr, "\n");
	exit(EXIT_FAILURE);
}

static void warn(const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "%s: ", argv0);

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	
	fprintf(stderr, "\n");
}

static int check_mod(uint mod, uint state)
{
	return (mod == XK_ANY_MOD) || (mod == state);
}

/*
 * Read from the tty.
 */
static void tty_read(void)
{
	char buf[BUFSIZ], *p;
	int len;

	if ((len = read(tty.fd, buf, sizeof(buf))) < 0)
		die("Failed to read from shell: %s", strerror(errno));

	// FIXME
	DEBUG("%s", buf);
//	XSetForeground(xw.display, dc.gc, dc.colors[color_fg].pixel);
//	XmbDrawString(xw.display, xw.win, dc.font.font_set, dc.gc,
//			0, 0, buf, strlen(buf));
//	XFlush(xw.display);
	
	for (p = buf; *p; p++) {
		term_putc(*p);
	}
}

/*
 * Write a string to the tty.
 */
static void tty_write(const char *s, size_t len)
{
	if (swrite(tty.fd, s, len) == -1)
		die("write error on tty: %s", strerror(errno));
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
		debug(D_WARN, "unable to set window size: %s", strerror(errno));
}

static int x_error_handler(Display *display, XErrorEvent *ev)
{
	char buf[BUFSIZ/4];

	XGetErrorText(display, ev->error_code, buf, BUFSIZ/4);
	die(buf);

	return 0;
}

/*
 * KeyPress event handler.
 */
static void event_keypress(XEvent *event)
{
	XKeyEvent *key_event = &event->xkey;
	KeySym keysym;
	char buf[32];
	Shortcut *sc;
	int len;

	len = XmbLookupString(xw.xic, key_event, buf, sizeof(buf), &keysym, NULL);

	/* Shortcuts */
	for (sc = shortcuts; sc < shortcuts+LEN(shortcuts); sc++) {
		if (keysym == sc->keysym && check_mod(sc->mod, key_event->state)) {
			sc->func(key_event);
			return;
		}
	}

	if (len == 0)
		return;

	DEBUG("key pressed: %s", buf);

	// FIXME

	tty_write(buf, len);
}

/*
 * ButtonRelease event handler.
 */
static void event_brelease(XEvent *event)
{
	if (event->xbutton.button == Button1) {
		sel_convert(XA_PRIMARY, event->xbutton.time);
	} else if (event->xbutton.button == Button2) {
		sel_copy(event->xbutton.time);
	}
}

/*
 * ClientMessage event handler.
 */
static void event_cmessage(XEvent *event)
{
	if (event->xclient.data.l[0] == wmdeletewin_atom) {
		/* Destroy window */
		XCloseDisplay(xw.display);
		exit(EXIT_SUCCESS);
	} else if (event->xclient.message_type == xembed_atom && event->xclient.format == 32) {
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
		XSetICFocus(xw.xic);
		DEBUG("FOCUS IN");
	} else {
		xw.state &= ~WIN_FOCUSED;
		XUnsetICFocus(xw.xic);
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
		/* Window has become visible - flag for redraw */
		xw.state |= WIN_VISIBLE | WIN_REDRAW;
	}
}

/*
 * SelectionNotify event handler.
 */
static void event_selnotify(XEvent *event)
{
	XSelectionEvent *xsev;
	ulong offset, nitems, after;
	Atom target;
	int format;
	uchar *data;

	xsev = &event->xselection;

	if (xsev->property == None) {
		/* Selection conversion refused */
		return;
	}

	/* Get selection contents in chunks */
	offset = 0L;
	do {
		XGetWindowProperty(xw.display, xw.win, xsev->property,
				offset, BUFSIZ/4, False, (Atom)AnyPropertyType,
				&target, &format, &nitems, &after, &data);

		if (target != sel.target || target != XA_STRING ||
				target != text_atom) {
			/* non-TEXT target */
			debug(D_WARN, "selection notify: unhandled target 0x%lx",
					target);
			break;
		}

		puts((char *)data);

		XFree(data);

		offset += (nitems * format) / 32;
	} while (after > 0);

	XDeleteProperty(xsev->display, xsev->requestor, xsev->property);
}

/*
 * SelectionRequest event handler.
 */
static void event_selrequest(XEvent *event)
{
	XSelectionRequestEvent *xsrev;
	XSelectionEvent xsev;
	Time timestamp;
	char *sel_text;

	xsrev = &event->xselectionrequest;

	xsev.type = SelectionNotify;
	xsev.display = xsrev->display;
	xsev.requestor = xsrev->requestor;
	xsev.selection = xsrev->selection;
	xsev.target = xsrev->target;
	xsev.time = xsrev->time;

	if (xsrev->property == None) {
		/* Obsolete requestor */
		xsrev->property = xsrev->target;
	}

	if (xsrev->target == timestamp_atom) {
		/* TIMESTAMP request: return timestamp used to acquire selection */
		if (xsrev->selection == XA_PRIMARY) {
			timestamp = sel.sel_time;
		} else if (xsrev->selection == clipboard_atom) {
			timestamp = sel.clip_time;
		} else {
			debug(D_WARN, "timestamp request: unhandled selection: 0x%lx",
					xsrev->selection);
			return;
			/* FIXME: refuse conversion? */
		}
		xsev.property = xsrev->property;
		XChangeProperty(xsev.display, xsev.requestor, xsev.property,
				XA_INTEGER, 32, PropModeReplace,
				(uchar *)timestamp, 1);
	} else if (xsrev->target == targets_atom) {
		/* TARGETS request: return list of supported targets */
		Atom supported[3] = { XA_STRING, text_atom, utf8_atom };

		xsev.property = xsrev->property;
		XChangeProperty(xsev.display, xsev.requestor, xsev.property,
				XA_ATOM, 32, PropModeReplace,
				(uchar *)supported, LEN(supported));
	} else if (xsrev->target == sel.target || xsrev->target == XA_STRING
			|| xsrev->target == text_atom) {
		/* STRING, TEXT request */
		if (xsrev->selection == XA_PRIMARY) {
			sel_text = sel.primary;
		} else if (xsrev->selection == clipboard_atom) {
			sel_text = sel.clipboard;
		} else {
			debug(D_WARN, "text request: unhandled selection: 0x%lx",
					xsrev->selection);
			return;
		}
		if (sel_text) {
			xsev.property = xsrev->property;
			XChangeProperty(xsev.display, xsev.requestor,
					xsev.property, xsev.target, 8,
					PropModeReplace, (uchar *)sel_text,
					sstrlen(sel_text));
		}
	} else {
		/* Refuse conversion to requested target */
		xsev.property = None;
	}

	if (!XSendEvent(xsev.display, xsev.requestor,
				False, (ulong)NULL, (XEvent *)&xsev))
		debug(D_WARN, "error sending SelectionNotify event");
}

/*
 * SelectionClear event handler.
 */
static void event_selclear(XEvent *event)
{
	/*
	 * TODO: clear logical & visual selection.
	 */
}

static void sc_paste_sel(XKeyEvent *xkey)
{
	sel_convert(XA_PRIMARY, xkey->time);
}

static void sc_paste_clip(XKeyEvent *xkey)
{
	sel_convert(clipboard_atom, xkey->time);
}

static void sc_copy_clip(XKeyEvent *xkey)
{
	if (sel.clipboard != NULL) {
		free(sel.clipboard);
	}
	if (sel.primary != NULL) {
		sel.clipboard = strdup(sel.primary);
		if (sel_own(clipboard_atom, xkey->time))
			sel.clip_time = xkey->time;
	}
}

/*
 * Draw the buffer into the window.
 */
static void draw(void)
{
	draw_region(0, 0, term.cols, term.rows);
	XCopyArea(xw.display, xw.drawbuf, xw.win, dc.gc,
			0, 0, xw.width, xw.height, 0, 0);
	XSetForeground(xw.display, dc.gc, dc.colors[color_bg].pixel);
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
		xwindow_clear(0, row, term.cols, row);
//		term.dirty[row] = False;

		/* TODO ... */
		XSetForeground(xw.display, dc.gc, dc.colors[color_fg].pixel);
		XmbDrawString(xw.display, xw.drawbuf, dc.font.font_set, dc.gc,
				0, 0, term.line[row], sstrlen(term.line[row]));
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
 * TODO
 */
static void term_putc(char c)
{
	term.line[term.cursor.y][term.cursor.x] = c;
	term.dirty[term.cursor.y] = True;

	term_moveto(++term.cursor.x, term.cursor.y);
}

static void term_moveto(int x, int y)
{
	term.cursor.x = LIMIT(x, 0, term.cols-1);
	term.cursor.y = LIMIT(y, 0, term.rows-1);
}

/*
 * Resize terminal (internal).
 */
static void term_resize(int cols, int rows)
{
	int i;
	int mincols = MIN(term.cols, cols);
	int minrows = MIN(term.rows, rows);

	for (i = 0; i <= term.cursor.y - rows; i++) {
		free(term.line[i]);
	}
	if (i > 0) {
		memmove(term.line, term.line + i, rows * sizeof(*term.line));
	}
	for (i += rows; i < term.rows; i++) {
		free(term.line[i]);
	}

	/* Reallocate height dependent elements */
	term.line = realloc(term.line, rows * sizeof(*term.line));
	term.dirty = realloc(term.dirty, rows * sizeof(*term.dirty));

	/* Resize rows */
	for (i = 0; i < minrows; i++) {
		term.line[i] = realloc(term.line[i], cols * sizeof(*term.line[i]));
	}
	/* Allocate new rows (if any) */
	for (; i < rows; i++) {
		term.line[i] = malloc(cols * sizeof(*term.line[i]));
	}

	/* Update terminal size */
	term.cols = cols;
	term.rows = rows;

	/* Clear new cols */
	if (cols > mincols && rows > 0)
		term_clear(mincols, 0, cols - 1, minrows - 1);
	/* Clear new rows (and new cols/rows overlap) */
	if (rows > minrows && cols > 0)
		term_clear(0, minrows, cols - 1, rows - 1);
}

/*
 * Clear a region of the internal terminal.
 */
static void term_clear(int x1, int y1, int x2, int y2)
{
	int x, y;

	/* Contrain coords */
	LIMIT(x1, 0, term.cols-1);
	LIMIT(y1, 0, term.rows-1);
	LIMIT(x2, 0, term.cols-1);
	LIMIT(y2, 0, term.rows-1);

	for (y = y1; y <= y2; y++) {
		term.dirty[y] = True;
		for (x = x1; x <= x2; x++) {
			term.line[y][x] = ' ';
		}
	}
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
 * Initialize Selection. Must be called after
 * x_init() in order to set target atom.
 */
static void sel_init(void)
{
	sel.primary = NULL;
	sel.clipboard = NULL;
	sel.target = utf8_atom;
}

static void sel_convert(Atom selection, Time time)
{
	DEBUG("Converting selection");

	XConvertSelection(xw.display, selection, sel.target,
			sel.target, xw.win, time);
}

static Bool sel_own(Atom selection, Time time)
{
	XSetSelectionOwner(xw.display, selection, xw.win, time);

	if (XGetSelectionOwner(xw.display, selection) != xw.win) 
		return False;

	XSetErrorHandler(x_error_handler); // FIXME: is this necessary?
	return True;
}

static void sel_copy(Time time)
{
	if (sel.primary)
		free(sel.primary);

	/* TODO: get text selection for primary */
	sel.primary = strdup("text");

	if (sel_own(XA_PRIMARY, time))
		sel.sel_time = time;
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

	MODBIT(wm_hints->flags, urgent, XUrgencyHint);
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
		die("Failed to allocate window size hints");
	}

	/* WM hints */
	if (!(wm_hints = XAllocWMHints())) {
		die("Failed to allocate window wm hints");
	}

	/* Class hints */
	if (!(class_hints = XAllocClassHint())) {
		die("Failed to allocate window class hints");
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

	class_hints->res_name = res_name;
	class_hints->res_class = res_class;

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
	XFontStruct **fonts;
	XFontSetExtents *font_extents;
	char **missing_charset_list;
	int missing_charset_count;
	char *def_string;
	char **font_name_list;
	int i;
	int num_fonts;
	int ascent, descent;

	/* Try to create font set structure */
	font->font_set = XCreateFontSet(xw.display, font_name,
			&missing_charset_list, &missing_charset_count,
			&def_string);
	if (!font->font_set) {
		die("failed to create font set \"%s\"", font_name);
	}
	for (i = 0; i < missing_charset_count; i++) {
		debug(D_WARN, "font for charset \"%s\" missing",
				missing_charset_list[i]);
	}

	/* Get font metrics */
	font_extents = XExtentsOfFontSet(font->font_set);
	num_fonts = XFontsOfFontSet(font->font_set, &fonts, &font_name_list);
	for (i = 0, ascent = 0, descent = 0; i < num_fonts; i++, fonts++) {
		if (ascent < (*fonts)->ascent)
			ascent = (*fonts)->ascent;
		if (descent < (*fonts)->descent)
			descent = (*fonts)->descent;
	}
	font->ascent = ascent;
	font->descent = descent;
	font->width = font_extents->max_logical_extent.width;
	font->height = font_extents->max_logical_extent.height;

	xw.cw = font->width;
	xw.ch = font->height;
}

static void load_colors(void)
{
	char *name;
	int i;
	
	/* Load colors [0-15] */
	for (i = 0; i < 16; i++) {
		/* Use resource color, if set */
		name = DEFAULT(xres.colors[i], color_names[i]);

		if (!XAllocNamedColor(xw.display, xw.colormap, name,
					&dc.colors[i], &dc.colors[i]))
			die("Failed to allocate color \"%s\"", name);
	}
	/* Load xterm colors [16-231] */
	for (i = 16; i < 232; i++) {
		// TODO
		dc.colors[i].red = 0;
		dc.colors[i].green = 0;
		dc.colors[i].blue = 0;
		if (!XAllocColor(xw.display, xw.colormap, &dc.colors[i]))
			die("Failed to allocate color %d", i);
	}

	/* Load xterm (grayscale) colors [232-255] */
	for (i = 232; i < 256; i++) {
		dc.colors[i].red = 0x0808 + 0x0a0a*(i - 232);
		dc.colors[i].blue = dc.colors[i].green = dc.colors[i].red;
		if (!XAllocColor(xw.display, xw.colormap, &dc.colors[i]))
			die("Failed to allocate color %d", i);
	}
}

/*
 * Clear region of the window (column,row coordinates).
 */
static void xwindow_clear(int col1, int row1, int col2, int row2)
{
	XSetForeground(xw.display, dc.gc, dc.colors[color_bg].pixel);
	XFillRectangle(xw.display, xw.drawbuf, dc.gc,
			xw.border + col1 * xw.cw,
			xw.border + row1 * xw.ch,
			(col2-col1+1) * xw.cw,
			(row2-row1+1) * xw.ch);
}

/*
 * Clear region of the window (absolute x,y coordinates).
 */
static void xwindow_abs_clear(int x1, int y1, int x2, int y2)
{
	XSetForeground(xw.display, dc.gc, dc.colors[color_bg].pixel);
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

	xwindow_abs_clear(0, 0, xw.width, xw.height);
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
	Atom netwmpid_atom;

	/* Open connection to X server */
	if (!(xw.display = XOpenDisplay(xw.display_name)))
		die("Cannot open X display \"%s\"", XDisplayName(xw.display_name));

	/* Get default screen and visual */
	xw.screen = XDefaultScreen(xw.display);
	xw.visual = XDefaultVisual(xw.display, xw.screen);

	/* Initialize rDB resources database */
	XrmInitialize();
	/* Get the resources from the server, if any */
	if ((s = XResourceManagerString(xw.display)) != NULL) {
		serverDB = XrmGetStringDatabase(s);
		XrmMergeDatabases(serverDB, &rDB);

		memset(&xres, 0, sizeof(xres));
		/* Get all resources from database */
		extract_resources();
	}

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
	xw.attrs.event_mask = ExposureMask | KeyPressMask | ButtonReleaseMask |
		StructureNotifyMask | VisibilityChangeMask | FocusChangeMask;

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
	memset(&gcvalues, 0, sizeof(gcvalues));
	gcvalues.graphics_exposures = False;
	dc.gc = XCreateGC(xw.display, XRootWindow(xw.display, xw.screen),
			GCGraphicsExposures, &gcvalues);
	/* Fill buffer with background color */
	XSetForeground(xw.display, dc.gc, dc.colors[color_bg].pixel);
	XFillRectangle(xw.display, xw.drawbuf, dc.gc, 0, 0, xw.width, xw.height);

	/* Input method(s) and input context */
	XSetLocaleModifiers("");
	if (!( xw.xim = XOpenIM(xw.display, rDB, res_name, res_class))) {
		XSetLocaleModifiers("@im=local");
		if (!( xw.xim = XOpenIM(xw.display, rDB, res_name, res_class))) {
			XSetLocaleModifiers("@im=");
			if (!( xw.xim = XOpenIM(xw.display, rDB, res_name, res_class)))
				die("could not open input device");
		}
	}
	xw.xic = XCreateIC(xw.xim, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
			XNClientWindow, xw.win, XNFocusWindow, xw.win, NULL);
	if (!xw.xic)
		die("could not create input context");

	/* Get atom(s) */
	wmdeletewin_atom = XInternAtom(xw.display, "WM_DELETE_WINDOW", False);
	netwmpid_atom = XInternAtom(xw.display, "_NET_WM_PID", False);
	xembed_atom = XInternAtom(xw.display, "_XEMBED", False);
	timestamp_atom = XInternAtom(xw.display, "TIMESTAMP", False);
	targets_atom = XInternAtom(xw.display, "TARGETS", False);
	text_atom = XInternAtom(xw.display, "TEXT", False);
	clipboard_atom = XInternAtom(xw.display, "CLIPBOARD", False);
	utf8_atom = XInternAtom(xw.display, "UTF8_STRING", True);
	if (utf8_atom == None)
		utf8_atom = XA_STRING;

	XSetWMProtocols(xw.display, xw.win, &wmdeletewin_atom, 1);
	XChangeProperty(xw.display, xw.win, netwmpid_atom, XA_CARDINAL, 32,
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

		if ((s = get_resource(color_name, color_class)) != NULL) {
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

static void term_reset(void)
{
	term.cursor = (Coord){ .x = 0, .y = 0 };
	term_clear(0, 0, term.cols-1, term.rows-1);
}

/*
 * Initialize Term.
 */
static void term_init(int cols, int rows)
{
	/* Set initial size, and force allocation
	 * of internal structures. */
	term_resize(cols, rows);
	term_reset();
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
	//struct timespec *tv = NULL; // Block indefinitely
	struct timespec tv; // FIXME

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
	resize_all(width, height);

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

		tv.tv_sec = 1;
		tv.tv_nsec = 0;

		/* Check if fd(s) are ready to be read */
		if (pselect(tty.fd+1, &read_fds, NULL, NULL, &tv, NULL) < 0) {
			if (errno == EINTR)
				continue; // Interrupted
			die("pselect failed: %s", strerror(errno));
		}

		if (FD_ISSET(tty.fd, &read_fds)) {
			/* Read from tty device */
			tty_read();
		}

		//XDrawLine(xw.display, xw.win, dc.gc, 0, 0, 20, 20);

		/* Process all pending events */
		while (XPending(xw.display)) {
			XNextEvent(xw.display, &event);

			if (XFilterEvent(&event, None))
				continue;
			/* Search event handlers for event type */
			if (event_handler[event.type])
				(event_handler[event.type])(&event);
		}

		draw();
		XFlush(xw.display);
	}
}

/*
 * Exec shell or command.
 * TODO
 */
static void exec_cmd(void)
{
	const struct passwd *pw;
	char *shell, *prog, **args;
	char buf[sizeof(long)*8 + 1];

	if (!(pw = getpwuid(getuid()))) {
		die("getpwuid: %s", strerror(errno));
	}

	/* Get the user's shell */
	if (!(shell = getenv("SHELL")) && !(shell = pw->pw_shell)) {
		shell = config_shell;
	}

	if (cmd) {
		prog = *cmd;
		args = cmd;
	} else {
		prog = shell;
		args = (char *[]) {shell, NULL};
	}

	snprintf(buf, sizeof(buf), "%lu", xw.win);

	/* Set up environment */
	unsetenv("COLUMNS");
	unsetenv("LINES");
	setenv("USER", pw->pw_name, 1);
	setenv("LOGNAME", pw->pw_name, 1);
	setenv("SHELL", shell, 1);
	setenv("HOME", pw->pw_dir, 1);
	setenv("WINDOWID", buf, 1);

	/* Default signal handlers */
	signal(SIGALRM, SIG_DFL);
	signal(SIGCHLD, SIG_DFL);
	signal(SIGHUP, SIG_DFL);
	signal(SIGINT, SIG_DFL);
	signal(SIGQUIT, SIG_DFL);
	signal(SIGTERM, SIG_DFL);

	if (execvp(prog, args) < 0) {
		exit(EXIT_FAILURE);
	}
}

/*
 * SIGCHLD signal handler.
 */
void sigchld(int signal)
{
	int status;

	if (signal != SIGCHLD)
		return;

	if (waitpid(tty.pid, &status, 0) < 0)
		debug(D_WARN, "waiting for pid %hd failed: %s", tty.pid, strerror(errno));

	if (WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS)
		exit(EXIT_SUCCESS);

	debug(D_WARN, "child exited with status %d", status);
	exit(EXIT_FAILURE);
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
		die("failed to open pty: %s", strerror(errno));

	switch (tty.pid = fork()) {
	case -1:
		die("fork: %s", strerror(errno));
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
			die("ioctl TIOCSTTY failed: %s", strerror(errno));
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

/* TODO */
static void usage()
{
	printf("Usage:\n  %s: usage goes here\n\n", argv0);
	exit(EXIT_SUCCESS);
}

int main(int argc, char *argv[])
{
	uint cols = DEFAULT_COLS, rows = DEFAULT_ROWS;
	char **arg, *p;
	xw.display_name = NULL;
	xw.parent = None;
	dc.font.name = NULL;

	argv0 = *argv;

#define OPT(s)  (strcmp(*arg, (s)) == 0)
#define OPTARG(s) (OPT((s)) && (*(arg+1) ? (arg++, 1) :\
			(die("option \"%s\" requires an argument", (s)), 0)))

	/* Parse options and arguments */
	for (arg = argv+1; *arg; arg++) {
		if (*arg[0] != '-') // Not an option
			continue;
		else if (OPT("--")) // End of options
			break;

		if (OPT("-h"))
			usage();
		else if (OPT("-v")) {
			printf("%s %s, %s\n", argv0, VERSION, AUTHOR);
			exit(EXIT_SUCCESS);
		} else if (OPTARG("-f"))
			dc.font.name = *arg;
		else if (OPTARG("-d"))
			xw.display_name = *arg;
		else if (OPTARG("-g"))
			xw.geomask = XParseGeometry(*arg, &xw.x, &xw.y, &cols, &rows);
		else if (OPTARG("-w"))
			xw.parent = strtol(*arg, NULL, 0);
		else if (OPTARG("-n"))
			res_name = *arg;
		else if (OPTARG("-c"))
			res_class = *argv;
		else if (OPTARG("-e")) {
			cmd = arg; // All remaining args are part of command
			break;
		} else {
			die("unknown option \"%s\"", *arg);
		}
	}
	if (!res_name) {
		res_name = (p = strrchr(argv[0], '/')) ? (p+1) : RES_NAME;
	}

	/* Set up locale */
	setlocale(LC_CTYPE, "");

	term_init(cols, rows);
	x_init();
	sel_init();

	main_loop();

	return 0;
}
