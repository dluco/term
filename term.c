#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <X11/Xlib.h>

#include <unistd.h>

#define DEFAULT_COLS 80
#define DEFAULT_ROWS 24

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
	Display *display;			/* X display */
	Window win;					/* X window */
	Visual *visual;				/* default visual */
	Colormap colormap;			/* default colormap */
	XSetWindowAttributes attrs;	/* window attributes */
	int screen;					/* display screen */
	int geometry;				/* geometry mask */
	int x, y;					/* offset from top-left of screen */
	int width, height;			/* window width and height */
	char *display_name;			/* name of display */
} XWindow;

/* Function prototypes */
static void die(char *fmt, ...);
static void x_init(void);
static void term_init(int cols, int rows);

/* Globals */
static XWindow xw;
static Term term;

#define DEBUG(msg, ...) \
	fprintf(stderr, "DEBUG %s:%d: " msg "\n", \
			__FILE__, __LINE__, ##__VA_ARGS__)

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
 * Initialize all X related items.
 */
static void x_init(void)
{
	/* Open connection to X server */
	if (!(xw.display = XOpenDisplay(xw.display_name)))
		die("Cannot open X display\n");

	/* Get default screen and visual */
	xw.screen = XDefaultScreen(xw.display);
	xw.visual = XDefaultVisual(xw.display, xw.screen);

	/* Colors */
	xw.colormap = XDefaultColormap(xw.display, xw.screen);

	/* Window geometry */
	xw.width = term.cols;
	xw.height = term.rows;

	/* Window attributes */
	xw.attrs.background_pixel = BlackPixel(xw.display, xw.screen);
	xw.attrs.border_pixel = BlackPixel(xw.display, xw.screen);
	xw.attrs.bit_gravity = NorthWestGravity;
	xw.attrs.event_mask = ExposureMask | StructureNotifyMask; // TODO
	xw.attrs.colormap = xw.colormap;

	xw.win = XCreateWindow(xw.display, XRootWindow(xw.display, xw.screen),
			xw.x, xw.y, xw.width, xw.height, 0,
			XDefaultDepth(xw.display, xw.screen), InputOutput,
			xw.visual, CWBackPixel | CWBorderPixel | CWBitGravity
			| CWEventMask | CWColormap, &xw.attrs);

	/* Map window */
	XMapWindow(xw.display, xw.win);
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
void term_run(void)
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

			/* XXX DEBUG XXX */
			DEBUG("width = %d", width);
			DEBUG("height = %d", height);
		} else if (event.type == MapNotify) {
			break;
		}

	}

	/* FIXME: REMOVE THIS LINE */
	sleep(1);

	return;
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

	/* XXX DEBUG XXX */
	DEBUG("display name = %s",
			XDisplayName(xw.display_name));
	DEBUG("cols = %d", cols);
	DEBUG("rows = %d", rows);

	term_init(cols, rows);

	x_init();

	term_run();

	return 0;
}
