#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <pty.h>

/* Macros */
#define DEBUG(msg, ...) \
	fprintf(stderr, "DEBUG %s:%s:%d: " msg "\n", \
			__FILE__, __func__, __LINE__, ##__VA_ARGS__)

#define DEFAULT_COLS 80
#define DEFAULT_ROWS 24
#define DEFAULT_FONT "fixed"

/* Typedefs for types */
typedef unsigned char uchar;
typedef unsigned int uint;
typedef unsigned long ulong;
typedef unsigned short ushort;

/* Structs */
typedef struct {
	int rows;		/* number of rows */
	int cols;		/* number of columns */
} Term;

typedef struct {
	pid_t pid;		/* PID of slave pty */
	int fd;			/* fd of process running in pty */
} TTY;

typedef struct {
	Display *display;			/* X display */
	Window win;					/* X window */
	Drawable buf;				/* drawing buffer */
	Visual *visual;				/* default visual */
	Colormap colormap;			/* default colormap */
	XSetWindowAttributes attrs;	/* window attributes */
	Atom wmdeletewin;			/* atoms */
	int screen;					/* display screen */
	int geometry;				/* geometry mask */
	int x, y;					/* offset from top-left of screen */
	int width, height;			/* window width and height */
	int cw, ch;					/* char width and height */
	char *display_name;			/* name of display */
} XWindow;

/* Font structure */
typedef struct {
	XFontStruct *font_info;
	int width;
	int height;
	char *name;
} XFont;

/* Drawing context */
typedef struct {
	XFont font;
	XColor color;
	GC gc;
} DC;

/* Function prototypes */
static ssize_t swrite(int fd, const void *buf, size_t count);
static void ttyread(void);
static void ttywrite(const char *s, size_t len);
static void die(char *fmt, ...);
static void draw(void);
static void set_title(char *title);
static void load_font(XFont *font, char *font_name);
static int font_max_width(XFontStruct *font_info);
static void xwindow_resize(int cols, int rows);
static void resize_all(int width, int height);
static void x_init(void);
static void term_init(int cols, int rows);
static void main_loop(void);
static void exec_cmd(void);
static void tty_init(void);

static void event_keypress(XEvent *event);
static void event_cmessage(XEvent *event);
static void event_resize(XEvent *event);

/* Event handlers */
static void (*event_handler[LASTEvent])(XEvent *) = {
	[KeyPress] = event_keypress,
	[ClientMessage] = event_cmessage,
	[ConfigureNotify] = event_resize,
};

/* Globals */
static TTY tty;
static XWindow xw;
static Term term;
static DC dc;

/*
 * Write count bytes to fd.
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
 * Read from the tty.
 */
static void ttyread(void)
{
	char buf[256];
	int len;

	if ((len = read(tty.fd, buf, sizeof buf)) < 0)
		die("Failed to read from shell: %s\n", strerror(errno));

	// FIXME
	DEBUG("%s", buf);
	XDrawString(xw.display, xw.buf, dc.gc, 0, 0, buf, len);
	XFlush(xw.display);
}

/*
 * Write a string to the tty.
 */
static void ttywrite(const char *s, size_t len)
{
	if (swrite(tty.fd, s, len) == -1)
		die("write error on tty: %s\n", strerror(errno));
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
	ttywrite(buf, len);
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
 * Draw the buffer into the window.
 * TODO
 */
static void draw(void)
{
	XCopyArea(xw.display, xw.buf, xw.win, dc.gc,
			0, 0, xw.width, xw.height, 0, 0);
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
static int font_max_width(XFontStruct *font_info)
{
	int i, width = 0;

	if (font_info->min_bounds.width == font_info->max_bounds.width)
		return font_info->min_bounds.width;
	if (font_info->per_char == NULL)
		return font_info->max_bounds.width;

	for (i = (font_info->max_char_or_byte2 - font_info->min_char_or_byte2); i >= 0; i--) {
		if (font_info->per_char[i].width > width)
			width = font_info->per_char[i].width;
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

/*
 * Resize and redraw X window.
 * TODO
 */
static void xwindow_resize(int cols, int rows)
{
	return;
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

	/* resize X window */
	xwindow_resize(cols, rows);

	DEBUG("Window resized: width = %d, height = %d, cols=%d, rows=%d",
			xw.width, xw.height, cols, rows);
}

/*
 * Initialize all X related items.
 */
static void x_init(void)
{
	XGCValues gcvalues;

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
	load_color(&dc.color, "#ff00ff");

	/* Window geometry */
	xw.width = term.cols * xw.cw;
	xw.height = term.rows * xw.ch;

	/* Window attributes */
	xw.attrs.background_pixel = BlackPixel(xw.display, xw.screen);
	xw.attrs.border_pixel = BlackPixel(xw.display, xw.screen);
	xw.attrs.bit_gravity = NorthWestGravity;
	xw.attrs.event_mask = ExposureMask | KeyPressMask |
		StructureNotifyMask; // TODO
	xw.attrs.colormap = xw.colormap;

	xw.win = XCreateWindow(xw.display, XRootWindow(xw.display, xw.screen),
			xw.x, xw.y, xw.width, xw.height, 0,
			XDefaultDepth(xw.display, xw.screen), InputOutput,
			xw.visual, CWBackPixel | CWBorderPixel | CWBitGravity
			| CWEventMask | CWColormap, &xw.attrs);

	/* Window drawing buffer */
	xw.buf = XCreatePixmap(xw.display, xw.win, xw.width, xw.height,
			DefaultDepth(xw.display, xw.screen));

	gcvalues.graphics_exposures = False;
	dc.gc = XCreateGC(xw.display, XRootWindow(xw.display, xw.screen),
			GCGraphicsExposures, &gcvalues);

	/* Get atoms */
	xw.wmdeletewin = XInternAtom(xw.display, "WM_DELETE_WINDOW", False);
	XSetWMProtocols(xw.display, xw.win, &xw.wmdeletewin, 1);

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
	term.cols = cols;
	term.rows = rows;
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

	XSetForeground(xw.display, dc.gc, dc.color.pixel);

	sleep(1);

	while (1) {

		DEBUG(__TIME__);

		//XDrawLine(xw.display, xw.win, dc.gc, 0, 0, 20, 20);

		ttyread();

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

	return;
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

	if (openpty(&master, &slave, NULL, NULL, &winp) < 0)
		die("failed to open pty: %s\n", strerror(errno));

	switch (tty.pid = fork()) {
	case -1:
		die("fork failed\n");
		break;
	case 0:		/* child process */
		/* create a new process group */
		setsid(); 
		/* duplicate file descriptors */
		dup2(slave, STDIN_FILENO);
		dup2(slave, STDOUT_FILENO);
		dup2(slave, STDERR_FILENO);
		/* close master and slave */
		close(slave);
		close(master);
		/* exec command */
		exec_cmd();
		break;
	default:	/* parent process */
		/* close slave */
		close(slave);
		tty.fd = master;
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
			xw.geometry = XParseGeometry(argv[++i], &xw.x, &xw.y, &cols, &rows);
	}

	DEBUG("display name = %s",
			XDisplayName(xw.display_name));
	DEBUG("cols = %d", cols);
	DEBUG("rows = %d", rows);

	term_init(cols, rows);

	x_init();

	main_loop();

	return 0;
}
