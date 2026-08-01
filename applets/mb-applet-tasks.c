/* mb-applet-tasks - a grouping taskbar for the Matchbox panel
 *
 * Classic Windows / GNOME 2 behaviour: one button per *application* rather
 * than per window, showing that application's icon and name, with a count
 * when it has more than one window open. Clicking a single-window app
 * raises it (or minimises it, if it is already the active one); clicking a
 * multi-window app pops a menu of its windows to choose from, in the same
 * style as mb-applet-card's eject menu.
 *
 * This replaces the "Active Tasks" folder that matchbox-desktop's tasks.so
 * module drew. Nothing here needs matchbox-desktop to exist.
 *
 * Everything is plain EWMH against the root window, so the applet does not
 * have to be told anything by the window manager beyond what the spec
 * already requires:
 *
 *   _NET_CLIENT_LIST      root    which windows exist, oldest first
 *   _NET_ACTIVE_WINDOW    root    which one is on top
 *   _NET_WM_STATE         client  _NET_WM_STATE_HIDDEN = minimised,
 *                                 _NET_WM_STATE_SKIP_TASKBAR = not ours
 *   _NET_WM_NAME/WM_NAME  client  the label in the window menu
 *   _NET_WM_PID/WM_CLASS  client  who this window belongs to (see below)
 *
 * and it drives the window manager back with _NET_ACTIVE_WINDOW and
 * _NET_WM_STATE client messages. matchbox-window-manager grew
 * _NET_WM_STATE_HIDDEN support for this; against a WM without it the
 * minimise half degrades to doing nothing and the rest still works.
 *
 * Copyright 2026 the piko project. GPL v2 or later, matching matchbox-panel.
 */

#include <libmb/mb.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <X11/Xatom.h>
#include <X11/Xutil.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#define _(x) (x)

#ifndef DATADIR
#define DATADIR "/usr/share"
#endif

#define APPS_DIR      DATADIR "/applications"

#define MAX_TASKS     64
#define MAX_GROUPS    24
#define MAX_DESKTOPS  64      /* .desktop entries we will index */
#define MAX_ICONS     24      /* icon cache slots */

#define KEY_LEN       64
#define NAME_LEN      96
#define TITLE_LEN     160

#define FALLBACK_ICON   "mbnoapp.png"

/* What the "there are more tasks than fit" button says. */
#define OVERFLOW_LABEL  ">>"

/* Button metrics, in pixels. Deliberately generous minimums: this runs on
 * a 640x480 touchscreen with no mouse, so a button that is hard to hit is
 * worse than one that is truncated. */
#define BTN_PAD        4      /* inside the button, left and right */
#define BTN_GAP        2      /* between buttons */
#define BTN_MAX_W    124
#define ICON_TEXT_GAP  4

/* Poll interval. Window titles and .desktop files are picked up here;
 * everything structural arrives as a root PropertyNotify instead. */
#define POLL_SECS      2

enum {
  ATOM_CLIENT_LIST,
  ATOM_ACTIVE_WINDOW,
  ATOM_CLOSE_WINDOW,
  ATOM_WM_NAME,
  ATOM_UTF8_STRING,
  ATOM_WM_WINDOW_TYPE,
  ATOM_WM_WINDOW_TYPE_NORMAL,
  ATOM_WM_ICON,
  ATOM_WM_PID,
  ATOM_WM_STATE_PROP,             /* _NET_WM_STATE  */
  ATOM_WM_STATE_HIDDEN,
  ATOM_WM_STATE_SKIP_TASKBAR,
  ATOM_ICCCM_WM_STATE,            /* WM_STATE, for IconicState */
  ATOM_PANEL_CLIENT_LIST,         /* _NET_CLIENT_LIST, read off the panel */
  ATOM_MB_CURRENT_APP,            /* matchbox: the app behind any dialog */
  ATOM_COUNT
};

static const char *AtomNames[ATOM_COUNT] = {
  "_NET_CLIENT_LIST",
  "_NET_ACTIVE_WINDOW",
  "_NET_CLOSE_WINDOW",
  "_NET_WM_NAME",
  "UTF8_STRING",
  "_NET_WM_WINDOW_TYPE",
  "_NET_WM_WINDOW_TYPE_NORMAL",
  "_NET_WM_ICON",
  "_NET_WM_PID",
  "_NET_WM_STATE",
  "_NET_WM_STATE_HIDDEN",
  "_NET_WM_STATE_SKIP_TASKBAR",
  "WM_STATE",
  "_NET_CLIENT_LIST",
  "_MB_CURRENT_APP_WINDOW"
};

typedef struct {
  Window win;
  char   title[TITLE_LEN];
  char   key[KEY_LEN];          /* which application this belongs to */
  char   klass[NAME_LEN];       /* WM_CLASS res_class */
  Bool   minimized;
} Task;

typedef struct {
  char           key[KEY_LEN];
  char           label[NAME_LEN];      /* what the button says, sans count */
  int            task[MAX_TASKS];      /* indices into Tasks, newest first */
  int            n_tasks;
  Bool           has_active;
  Bool           all_minimized;
  MBPixbufImage *icon;                 /* borrowed from the icon cache */
  int            x, w;                 /* filled in by layout() */
} Group;

typedef struct {
  char exec[KEY_LEN];           /* Exec= basename, our join key */
  char name[NAME_LEN];
  char icon[NAME_LEN];
} DeskEntry;

typedef struct {
  char           key[KEY_LEN];
  MBPixbufImage *img;           /* NULL means "looked, found nothing" */
  Bool           used;
} IconSlot;

static MBTrayApp     *App;
static MBPixbuf      *Pb;
static MBFont        *Fnt;
static MBColor       *ColText;
static MBMenu        *Menu;
static char          *ThemeName;
static Display       *Dpy;
static Window         Root;
static Atom           Atoms[ATOM_COUNT];

static Task           Tasks[MAX_TASKS];
static int            NTasks;
static Group          Groups[MAX_GROUPS];
static int            NGroups;
static int            NVisible;        /* groups actually drawn */
static Bool           Overflow;        /* ... and a ">>" button after them */
static int            OverflowX, OverflowW;

static DeskEntry      Desktops[MAX_DESKTOPS];
static int            NDesktops;
static time_t         DesktopsMtime;

static IconSlot       Icons[MAX_ICONS];

static int            IconSize = 24;
static int            FontPixels;      /* 0 = derive from the panel height */
static int            Visible = -1;    /* tri-state, -1 = not decided yet */
static Bool           Dirty;           /* a rescan is owed */
static int            LastWidth = -1;  /* last size we asked the panel for */

static int  trapped_error_code;
static int (*old_error_handler) (Display *d, XErrorEvent *e);

static void rebuild (void);
static int  layout (void);
static void icons_flush (void);

/* ------------------------------------------------------------------ */
/* X error trapping. Every window we touch belongs to somebody else and
 * can be destroyed between the client list arriving and us reading it. */

static int
error_handler (Display *display, XErrorEvent *error)
{
  (void) display;
  trapped_error_code = error->error_code;
  return 0;
}

static void
trap_errors (void)
{
  trapped_error_code = 0;
  old_error_handler  = XSetErrorHandler (error_handler);
}

static int
untrap_errors (void)
{
  XSetErrorHandler (old_error_handler);
  return trapped_error_code;
}

/* Fetch a whole property. Caller XFree()s.
 *
 * `format` is checked, not assumed: everything read here as long[] is a
 * 32-bit property, and a window that answered with an 8- or 16-bit one
 * would otherwise be walked as longs -- reading four times the bytes that
 * are there. Nothing stops an application setting _NET_WM_ICON to a
 * string. */
static void *
get_prop (Window win, Atom prop, Atom type, int format,
	  unsigned long *n_items)
{
  Atom           type_ret;
  int            format_ret;
  unsigned long  items_ret = 0, after_ret;
  unsigned char *data = NULL;

  if (n_items)
    *n_items = 0;

  if (XGetWindowProperty (Dpy, win, prop, 0, 0x7fffffff, False, type,
			  &type_ret, &format_ret, &items_ret, &after_ret,
			  &data) != Success)
    return NULL;

  if (data == NULL)
    return NULL;

  if (type_ret == None || format_ret != format)
    {
      XFree (data);
      return NULL;
    }

  if (n_items)
    *n_items = items_ret;

  return data;
}

/* 32-bit properties come back as long[], whatever the wire format says --
 * that is Xlib's documented behaviour, and why every count here is in
 * items rather than bytes. */
static long *
get_prop32 (Window win, Atom prop, Atom type, unsigned long *n_items)
{
  return (long *) get_prop (win, prop, type, 32, n_items);
}

static Window
get_window_prop (Window win, Atom prop)
{
  unsigned long  n = 0;
  long          *data;
  Window         result = None;

  data = get_prop32 (win, prop, XA_WINDOW, &n);
  if (data)
    {
      if (n > 0)
	result = (Window) data[0];
      XFree (data);
    }
  return result;
}

/* ------------------------------------------------------------------ */
/* small string helpers                                                */

/* snprintf("%s") would do, but it is undefined when source and
 * destination overlap -- and group_label() copies one field of a Group
 * over another. memmove is defined for that, and truncating here is the
 * intent rather than an accident worth a -Wformat-truncation warning. */
static void
str_copy (char *dst, size_t len, const char *src)
{
  size_t n;

  if (len == 0)
    return;

  if (src == NULL)
    {
      dst[0] = '\0';
      return;
    }

  n = strlen (src);
  if (n >= len)
    n = len - 1;

  memmove (dst, src, n);
  dst[n] = '\0';
}

static void
str_lower (char *s)
{
  for (; *s; s++)
    *s = tolower ((unsigned char) *s);
}

static const char *
base_name (const char *path)
{
  const char *slash = strrchr (path, '/');
  return slash ? slash + 1 : path;
}

/* "st" -> "St". Only used when we have no .desktop file to take a
 * properly capitalised Name from. */
static void
str_titlecase (char *s)
{
  if (s[0])
    s[0] = toupper ((unsigned char) s[0]);
}

/* ------------------------------------------------------------------ */
/* the .desktop index                                                  */

/* Exec= may be a full path and may carry field codes ("%f", "%U"). The
 * join key is just the basename of the program, which is also what
 * /proc/<pid>/cmdline and WM_CLASS's res_name normally give us. */
static void
exec_key (const char *exec, char *out, size_t len)
{
  char  buf[256];
  char *space;

  str_copy (buf, sizeof (buf), exec);
  if ((space = strchr (buf, ' ')) != NULL)
    *space = '\0';

  str_copy (out, len, base_name (buf));
  str_lower (out);
}

static void
scan_desktops (void)
{
  DIR           *d;
  struct dirent *e;

  NDesktops = 0;

  if ((d = opendir (APPS_DIR)) == NULL)
    return;

  while ((e = readdir (d)) != NULL && NDesktops < MAX_DESKTOPS)
    {
      char           path[512];
      MBDotDesktop  *dd;
      unsigned char *type, *name, *icon;
      char          *exec;
      DeskEntry     *ent;

      if (e->d_name[0] == '.')
	continue;
      if (strstr (e->d_name, ".desktop") == NULL)
	continue;

      snprintf (path, sizeof (path), "%s/%s", APPS_DIR, e->d_name);

      if ((dd = mb_dotdesktop_new_from_file (path)) == NULL)
	continue;

      /* Type=PanelApp entries are the applets themselves -- they dock into
       * this panel rather than opening a window, so they can never appear
       * in the task list and would only add false icon matches. */
      type = mb_dotdesktop_get (dd, "Type");
      if (type == NULL || strcmp ((char *) type, "Application") != 0)
	{
	  mb_dotdesktop_free (dd);
	  continue;
	}

      if ((exec = mb_dotdesktop_get_exec (dd)) == NULL)
	{
	  mb_dotdesktop_free (dd);
	  continue;
	}

      ent = &Desktops[NDesktops];
      memset (ent, 0, sizeof (*ent));

      exec_key (exec, ent->exec, sizeof (ent->exec));
      free (exec);

      name = mb_dotdesktop_get (dd, "Name");
      icon = mb_dotdesktop_get (dd, "Icon");

      str_copy (ent->name, sizeof (ent->name), (char *) name);
      str_copy (ent->icon, sizeof (ent->icon), (char *) icon);

      if (ent->exec[0])
	NDesktops++;

      mb_dotdesktop_free (dd);
    }

  closedir (d);
}

static DeskEntry *
desktop_for_key (const char *key)
{
  int i;

  if (!key[0])
    return NULL;

  for (i = 0; i < NDesktops; i++)
    if (strcmp (Desktops[i].exec, key) == 0)
      return &Desktops[i];

  return NULL;
}

/* Cheap: one stat(), so it can run on every poll. New applications appear
 * on this device by being installed onto an SD card, not only at boot. */
static Bool
desktops_changed (void)
{
  struct stat st;

  if (stat (APPS_DIR, &st) != 0)
    return False;
  if (st.st_mtime == DesktopsMtime)
    return False;

  DesktopsMtime = st.st_mtime;
  return True;
}

/* ------------------------------------------------------------------ */
/* identifying which application a window belongs to                   */

/* /proc/<pid>/cmdline's argv[0], basename'd and lowercased.
 *
 * Tried before WM_CLASS on purpose. WM_CLASS is whatever the toolkit felt
 * like: every FLTK program on this device calls itself "FLTK" unless it
 * has been told otherwise, which would fold unrelated applications into
 * one taskbar button. argv[0] is the binary that was actually launched,
 * which is also exactly what .desktop Exec= names, so it doubles as the
 * icon lookup key. Everything here is on the same machine, so /proc is a
 * legitimate source -- there is no network transparency to preserve. */
static Bool
key_from_pid (Window win, char *out, size_t len)
{
  unsigned long  n = 0;
  long          *pid_data;
  char           path[64], buf[256];
  FILE          *f;
  size_t         got;

  out[0] = '\0';

  pid_data = get_prop32 (win, Atoms[ATOM_WM_PID], XA_CARDINAL, &n);
  if (pid_data == NULL)
    return False;

  if (n < 1 || pid_data[0] <= 0)
    {
      XFree (pid_data);
      return False;
    }

  snprintf (path, sizeof (path), "/proc/%ld/cmdline", (long) pid_data[0]);
  XFree (pid_data);

  if ((f = fopen (path, "r")) == NULL)
    return False;

  got = fread (buf, 1, sizeof (buf) - 1, f);
  fclose (f);

  if (got == 0)
    return False;

  buf[got] = '\0';		/* argv[0] is NUL-terminated within buf */

  str_copy (out, len, base_name (buf));
  str_lower (out);

  return out[0] != '\0';
}

/* ------------------------------------------------------------------ */
/* icons                                                               */

static MBPixbufImage *
scale_to_icon (MBPixbufImage *img)
{
  MBPixbufImage *scaled;

  if (img == NULL)
    return NULL;

  if (mb_pixbuf_img_get_width (img) == IconSize
      && mb_pixbuf_img_get_height (img) == IconSize)
    return img;

  scaled = mb_pixbuf_img_scale (Pb, img, IconSize, IconSize);
  mb_pixbuf_img_free (Pb, img);

  return scaled;
}

static MBPixbufImage *
icon_from_file (const char *icon_name)
{
  char          *path;
  MBPixbufImage *img;

  if (icon_name == NULL || !icon_name[0])
    return NULL;

  path = mb_dot_desktop_icon_get_full_path (ThemeName, IconSize,
					    (char *) icon_name);
  if (path == NULL)
    return NULL;

  img = mb_pixbuf_img_new_from_file (Pb, path);
  free (path);

  return scale_to_icon (img);
}

/* _NET_WM_ICON is a run of (width, height, w*h ARGB pixels) blocks. Pick
 * the largest one that is not bigger than we need, else the smallest
 * available -- scaling down looks better than scaling up. */
static MBPixbufImage *
icon_from_net_wm_icon (Window win)
{
  unsigned long  n = 0, i = 0;
  long          *data;
  long          *best = NULL;
  long           best_w = 0;
  MBPixbufImage *img = NULL;

  data = get_prop32 (win, Atoms[ATOM_WM_ICON], XA_CARDINAL, &n);
  if (data == NULL)
    return NULL;

  while (i + 2 <= n)
    {
      long          w = data[i], h = data[i + 1];
      unsigned long px;

      if (w <= 0 || h <= 0)
	break;

      px = (unsigned long) w * (unsigned long) h;
      if (i + 2 + px > n)	/* truncated/corrupt -- stop believing it */
	break;

      if (best == NULL
	  || (best_w > IconSize && w < best_w)     /* everything too big */
	  || (w > best_w && w <= IconSize))        /* a better fit */
	{
	  best   = &data[i];
	  best_w = w;
	}

      i += 2 + px;
    }

  /* Narrow long[] to int[] rather than casting the pointer. Xlib hands
   * back 32-bit properties as long[], which is 64 bits wide on anything
   * but the target -- a straight cast reads every other word as a pixel.
   * matchbox-desktop's tasks.so had exactly that bug and got away with it
   * only because the device is 32-bit. */
  if (best != NULL)
    {
      unsigned long  px = (unsigned long) best[0] * (unsigned long) best[1];
      int           *argb = malloc (px * sizeof (int));

      if (argb != NULL)
	{
	  unsigned long k;

	  for (k = 0; k < px; k++)
	    argb[k] = (int) (best[2 + k] & 0xffffffffL);

	  img = mb_pixbuf_img_new_from_int_data (Pb, argb, best[0], best[1]);
	  free (argb);
	}
    }

  XFree (data);

  return scale_to_icon (img);
}

static MBPixbufImage *
icon_from_wmhints (Window win)
{
  XWMHints      *hints;
  MBPixbufImage *img = NULL;
  Window         dummy;
  int            x, y;
  unsigned int   w, h, bw, depth;

  if ((hints = XGetWMHints (Dpy, win)) == NULL)
    return NULL;

  if ((hints->flags & IconPixmapHint)
      && hints->icon_pixmap != None
      && XGetGeometry (Dpy, hints->icon_pixmap, &dummy, &x, &y, &w, &h,
		       &bw, &depth))
    {
      img = mb_pixbuf_img_new_from_drawable (Pb,
					     (Drawable) hints->icon_pixmap,
					     (hints->flags & IconMaskHint)
					       ? (Drawable) hints->icon_mask
					       : None,
					     0, 0, w, h);
    }

  XFree (hints);

  return scale_to_icon (img);
}

static void
icons_flush (void)
{
  int i;

  for (i = 0; i < MAX_ICONS; i++)
    {
      if (Icons[i].img)
	mb_pixbuf_img_free (Pb, Icons[i].img);
      Icons[i].img    = NULL;
      Icons[i].used   = False;
      Icons[i].key[0] = '\0';
    }
}

/* Cached by application key, so a second window of the same program costs
 * nothing and a program that is closed and reopened does not re-read its
 * PNG off flash. */
static MBPixbufImage *
icon_for (const char *key, Window win)
{
  MBPixbufImage *img = NULL;
  DeskEntry     *ent;
  int            i, slot = -1;

  for (i = 0; i < MAX_ICONS; i++)
    {
      if (Icons[i].used && strcmp (Icons[i].key, key) == 0)
	return Icons[i].img;
      if (slot < 0 && !Icons[i].used)
	slot = i;
    }

  /* Preference order: what the application menu shows for this program,
   * then what the window itself supplies, then the stock placeholder.
   * The .desktop icon comes first deliberately -- a taskbar button that
   * does not match the icon the user launched it from reads as a
   * different program. */
  if ((ent = desktop_for_key (key)) != NULL)
    img = icon_from_file (ent->icon);

  if (img == NULL && win != None)
    {
      trap_errors ();
      img = icon_from_net_wm_icon (win);
      if (img == NULL)
	img = icon_from_wmhints (win);
      if (untrap_errors () && img != NULL)
	{
	  mb_pixbuf_img_free (Pb, img);
	  img = NULL;
	}
    }

  if (img == NULL)
    img = icon_from_file (FALLBACK_ICON);

  /* Cache full. Every entry is for an application that has since gone
   * away (there can never be more live groups than slots), so throwing
   * the lot away costs one reload each and bounds memory -- whereas
   * returning an uncached image would leak one icon per rebuild, and
   * rebuilds happen every couple of seconds. */
  if (slot < 0)
    {
      icons_flush ();
      slot = 0;
    }

  Icons[slot].used = True;
  Icons[slot].img  = img;
  str_copy (Icons[slot].key, sizeof (Icons[slot].key), key);

  return img;
}

/* ------------------------------------------------------------------ */
/* scanning                                                            */

static Bool
window_is_iconic (Window win)
{
  unsigned long  n = 0;
  long          *data;
  Bool           iconic = False;

  data = get_prop32 (win, Atoms[ATOM_ICCCM_WM_STATE],
		     Atoms[ATOM_ICCCM_WM_STATE], &n);
  if (data)
    {
      if (n > 0 && data[0] == IconicState)
	iconic = True;
      XFree (data);
    }

  return iconic;
}

/* Returns False for windows a taskbar has no business listing.
 *
 * One trap/untrap pair around the whole thing, with a single exit. Every
 * window here belongs to another process and may be destroyed between the
 * client list arriving and us reading it, so any of these calls can fail
 * -- and an early return that skipped untrap_errors() would leave our
 * handler installed over the rest of the applet, swallowing later errors
 * silently. */
static Bool
task_from_window (Window win, Task *t)
{
  XWindowAttributes  attr;
  XClassHint         klass;
  long              *type;
  long              *state;
  unsigned long      n = 0, i;
  unsigned char     *name;
  Window             trans = None;
  Bool               iconic, keep = True;
  char               pid_key[KEY_LEN];

  memset (t, 0, sizeof (*t));
  t->win = win;

  trap_errors ();

  if (!XGetWindowAttributes (Dpy, win, &attr) || attr.override_redirect)
    keep = False;

  iconic = keep ? window_is_iconic (win) : False;

  /* Unmapped is normally "not a task", but a minimised window is unmapped
   * too and very much still a task -- that is the whole point of having a
   * taskbar. Tell them apart by WM_STATE rather than by map state. */
  if (keep && attr.map_state != IsViewable && !iconic)
    keep = False;

  if (keep)
    {
      type = get_prop32 (win, Atoms[ATOM_WM_WINDOW_TYPE], XA_ATOM, &n);
      if (type)
	{
	  /* docks, desktops, splashes, menus */
	  if (n < 1 || (Atom) type[0] != Atoms[ATOM_WM_WINDOW_TYPE_NORMAL])
	    keep = False;
	  XFree (type);
	}
    }

  /* A transient is a dialog of something else; it belongs to its parent's
   * button, not to one of its own. */
  if (keep
      && XGetTransientForHint (Dpy, win, &trans)
      && trans != None && trans != win)
    keep = False;

  if (keep)
    {
      state = get_prop32 (win, Atoms[ATOM_WM_STATE_PROP], XA_ATOM, &n);
      if (state)
	{
	  for (i = 0; i < n; i++)
	    {
	      if ((Atom) state[i] == Atoms[ATOM_WM_STATE_SKIP_TASKBAR])
		keep = False;
	      if ((Atom) state[i] == Atoms[ATOM_WM_STATE_HIDDEN])
		t->minimized = True;
	    }
	  XFree (state);
	}
    }

  /* WM_STATE is the authority even where _NET_WM_STATE_HIDDEN is absent,
   * which is what a window manager without our patch looks like. */
  if (iconic)
    t->minimized = True;

  if (keep)
    {
      /* Title */
      name = get_prop (win, Atoms[ATOM_WM_NAME], Atoms[ATOM_UTF8_STRING],
		       8, NULL);
      if (name)
	{
	  str_copy (t->title, sizeof (t->title), (char *) name);
	  XFree (name);
	}
      else
	{
	  char *legacy = NULL;
	  if (XFetchName (Dpy, win, &legacy) && legacy)
	    {
	      str_copy (t->title, sizeof (t->title), legacy);
	      XFree (legacy);
	    }
	}

      /* Identity */
      memset (&klass, 0, sizeof (klass));
      if (XGetClassHint (Dpy, win, &klass))
	{
	  if (klass.res_class)
	    str_copy (t->klass, sizeof (t->klass), klass.res_class);
	  if (klass.res_name)
	    {
	      str_copy (t->key, sizeof (t->key), klass.res_name);
	      str_lower (t->key);
	    }
	  if (klass.res_name)  XFree (klass.res_name);
	  if (klass.res_class) XFree (klass.res_class);
	}

      /* /proc wins over WM_CLASS -- see key_from_pid(). */
      if (key_from_pid (win, pid_key, sizeof (pid_key)))
	str_copy (t->key, sizeof (t->key), pid_key);
    }

  if (untrap_errors ())
    keep = False;

  if (!keep)
    return False;

  if (!t->key[0])
    str_copy (t->key, sizeof (t->key), t->klass[0] ? t->klass : t->title);
  str_lower (t->key);

  if (!t->key[0])
    str_copy (t->key, sizeof (t->key), "?");

  if (!t->title[0])
    str_copy (t->title, sizeof (t->title), _("Untitled"));

  return True;
}

/* Is WM_CLASS's res_class just a nicer spelling of the key, or a
 * different word entirely?
 *
 * "steam" for a key of "steamwebhelper" is the same program and the
 * better label. "FLTK" for a key of "piko-designer" is the toolkit's
 * name, which every FLTK program on this device shares -- taking it as a
 * label would put the same word on unrelated buttons. Prefix either way
 * is the test, because res_class is variously the binary, a shortening of
 * it, or a capitalisation of it. */
static Bool
klass_matches_key (const char *klass, const char *key)
{
  char lower[NAME_LEN];

  if (!klass[0] || !key[0])
    return False;

  str_copy (lower, sizeof (lower), klass);
  str_lower (lower);

  return strncmp (lower, key, strlen (lower)) == 0
         || strncmp (key, lower, strlen (key)) == 0;
}

/* The label for a whole group: the application's name, not any one
 * window's title. .desktop Name first so the taskbar and the launcher
 * menu agree; WM_CLASS next when it is really about this program; the
 * key itself otherwise. */
static void
group_label (Group *g, Task *first)
{
  DeskEntry *ent = desktop_for_key (g->key);

  if (ent != NULL && ent->name[0])
    {
      str_copy (g->label, sizeof (g->label), ent->name);
      return;
    }

  if (klass_matches_key (first->klass, g->key))
    {
      str_copy (g->label, sizeof (g->label), first->klass);
      str_titlecase (g->label);
      return;
    }

  str_copy (g->label, sizeof (g->label), g->key);
  str_titlecase (g->label);
}

/* Which application window counts as "on screen now".
 *
 * _NET_ACTIVE_WINDOW is whatever holds focus, which under matchbox is the
 * *dialog* whenever an application has one open -- and a dialog is never
 * in the task list, so no button would light up while a file chooser was
 * on screen. _MB_CURRENT_APP_WINDOW is matchbox's own answer to exactly
 * that question. Prefer it, fall back to the standard property under any
 * other window manager. */
static Window
active_app_window (void)
{
  Window win = get_window_prop (Root, Atoms[ATOM_MB_CURRENT_APP]);

  if (win == None)
    win = get_window_prop (Root, Atoms[ATOM_ACTIVE_WINDOW]);

  return win;
}

static void
rebuild (void)
{
  long          *wins;
  Window         active;
  unsigned long  n = 0;
  int            i, j;

  NTasks  = 0;
  NGroups = 0;

  active = active_app_window ();

  wins = get_prop32 (Root, Atoms[ATOM_CLIENT_LIST], XA_WINDOW, &n);
  if (wins == NULL)
    return;

  /* _NET_CLIENT_LIST is oldest-first. Walk it backwards so that within a
   * group the most recently opened window sorts first, which is the one a
   * single click should raise. */
  for (i = (int) n - 1; i >= 0 && NTasks < MAX_TASKS; i--)
    {
      Task  *t = &Tasks[NTasks];
      Group *g = NULL;

      if (!task_from_window ((Window) wins[i], t))
	continue;

      for (j = 0; j < NGroups; j++)
	if (strcmp (Groups[j].key, t->key) == 0)
	  {
	    g = &Groups[j];
	    break;
	  }

      if (g == NULL)
	{
	  if (NGroups >= MAX_GROUPS)
	    continue;		/* pathological; drop rather than misdraw */
	  g = &Groups[NGroups++];
	  memset (g, 0, sizeof (*g));
	  str_copy (g->key, sizeof (g->key), t->key);
	  group_label (g, t);
	  g->all_minimized = True;
	}

      if (g->n_tasks < MAX_TASKS)
	g->task[g->n_tasks++] = NTasks;

      if (t->win == active && !t->minimized)
	g->has_active = True;
      if (!t->minimized)
	g->all_minimized = False;

      NTasks++;
    }

  XFree (wins);

  for (i = 0; i < NGroups; i++)
    Groups[i].icon = icon_for (Groups[i].key,
			       Tasks[Groups[i].task[0]].win);
}

/* ------------------------------------------------------------------ */
/* layout                                                              */

static int
text_width (const char *s)
{
  return mb_font_render_simple_get_width (Fnt, BTN_MAX_W,
					  (unsigned char *) s,
					  MB_ENCODING_UTF8, 0);
}

/* What this group's button says, count included. */
static void
button_text (Group *g, char *out, size_t len)
{
  if (g->n_tasks > 1)
    snprintf (out, len, "%s (%d)", g->label, g->n_tasks);
  else
    str_copy (out, len, g->label);
}

/* Where the label starts. An application with no icon anywhere -- no
 * .desktop entry, no _NET_WM_ICON, and no stock placeholder installed --
 * would otherwise get a button indented around empty space. */
static int
label_offset (Group *g)
{
  return BTN_PAD + (g->icon ? IconSize + ICON_TEXT_GAP : 0);
}

/* How much room the panel actually has for us.
 *
 * The panel reparents every applet into itself and advertises the docked
 * set as _NET_CLIENT_LIST on its own window (panel_update_client_list_prop
 * in matchbox-panel's panel.c), so the nearest applet to our right marks
 * the end of the space we may take. Asking for more than that does not
 * fail -- the panel just lets the applets overlap -- so nobody would tell
 * us we had got it wrong. */
static int
available_width (void)
{
  Window            win = mb_tray_app_xwin (App);
  Window            parent = None, root_ret = None, *children = NULL;
  unsigned int      n_children = 0;
  long             *siblings;
  unsigned long     n = 0;
  int               screen_w, our_x = 0, limit, i;
  Bool              ok;
  XWindowAttributes attr;

  screen_w = DisplayWidth (Dpy, mb_tray_app_xscreen (App));
  limit    = screen_w;

  if (win == None)		/* not docked yet */
    return screen_w / 2;

  /* One trap around the lot, one untrap, same reasoning as
   * task_from_window(): a half-installed error handler outlives the call
   * that installed it. */
  trap_errors ();

  ok = XQueryTree (Dpy, win, &root_ret, &parent, &children, &n_children)
       ? True : False;
  if (children)
    XFree (children);

  if (ok && parent != None && XGetWindowAttributes (Dpy, win, &attr))
    our_x = attr.x;
  else
    ok = False;

  if (ok)
    {
      if (XGetWindowAttributes (Dpy, parent, &attr))
	limit = attr.width;

      siblings = get_prop32 (parent, Atoms[ATOM_PANEL_CLIENT_LIST],
			     XA_WINDOW, &n);
      if (siblings)
	{
	  for (i = 0; i < (int) n; i++)
	    {
	      if ((Window) siblings[i] == win)
		continue;
	      if (!XGetWindowAttributes (Dpy, (Window) siblings[i], &attr))
		continue;
	      if (attr.x > our_x && attr.x < limit)
		limit = attr.x;
	    }
	  XFree (siblings);
	}
    }

  if (untrap_errors ())
    ok = False;

  if (!ok)
    return screen_w / 2;

  limit -= our_x + BTN_GAP;

  return limit > 0 ? limit : 0;
}

/* Fills in each visible group's x/w, and decides how many fit.
 *
 * Buttons get their natural width while there is room and shrink evenly
 * once there is not, down to an icon-only square. Below that they are not
 * hittable any more, so the tail goes behind a ">>" button rather than
 * being silently dropped -- a task you cannot see and cannot reach is
 * exactly the failure a taskbar exists to prevent. */
static int
layout (void)
{
  int budget = available_width ();
  int min_w  = IconSize + 2 * BTN_PAD;
  int natural[MAX_GROUPS];
  int total = 0, i, x = 0, per;

  NVisible  = 0;
  Overflow  = False;
  OverflowW = 0;

  if (NGroups == 0)
    return 0;

  for (i = 0; i < NGroups; i++)
    {
      char label[NAME_LEN + 16];

      button_text (&Groups[i], label, sizeof (label));

      natural[i] = BTN_PAD + label_offset (&Groups[i]) + text_width (label);
      if (natural[i] > BTN_MAX_W)
	natural[i] = BTN_MAX_W;

      total += natural[i] + (i ? BTN_GAP : 0);
    }

  if (total <= budget)
    {
      for (i = 0; i < NGroups; i++)
	{
	  Groups[i].x = x;
	  Groups[i].w = natural[i];
	  x += natural[i] + BTN_GAP;
	}
      NVisible = NGroups;
      return total;
    }

  /* Even share, but never below a button you can actually hit. */
  per = (budget - (NGroups - 1) * BTN_GAP) / NGroups;

  if (per >= min_w)
    {
      for (i = 0; i < NGroups; i++)
	{
	  Groups[i].x = x;
	  Groups[i].w = (natural[i] < per) ? natural[i] : per;
	  x += Groups[i].w + BTN_GAP;
	}
      NVisible = NGroups;
      return x - BTN_GAP;
    }

  /* Not even icons fit: show what we can and hand the rest to a menu. */
  OverflowW = min_w;
  Overflow  = True;

  for (i = 0; i < NGroups; i++)
    {
      if (x + min_w + BTN_GAP + OverflowW > budget)
	break;
      Groups[i].x = x;
      Groups[i].w = min_w;
      x += min_w + BTN_GAP;
      NVisible++;
    }

  OverflowX = x;

  return x + OverflowW;
}

/* ------------------------------------------------------------------ */
/* painting                                                            */

static void
fill_rect (MBPixbufImage *img, int x, int y, int w, int h,
	   int r, int g, int b, int a)
{
  int i, j;
  int max_x = mb_pixbuf_img_get_width (img);
  int max_y = mb_pixbuf_img_get_height (img);

  for (j = y; j < y + h; j++)
    {
      if (j < 0 || j >= max_y)
	continue;
      for (i = x; i < x + w; i++)
	{
	  if (i < 0 || i >= max_x)
	    continue;
	  mb_pixbuf_img_plot_pixel_with_alpha (Pb, img, i, j, r, g, b, a);
	}
    }
}

static void
draw_border (MBPixbufImage *img, int x, int y, int w, int h,
	     int r, int g, int b, int a)
{
  fill_rect (img, x,         y,         w, 1, r, g, b, a);
  fill_rect (img, x,         y + h - 1, w, 1, r, g, b, a);
  fill_rect (img, x,         y,         1, h, r, g, b, a);
  fill_rect (img, x + w - 1, y,         1, h, r, g, b, a);
}

/* The frame and the icon, which are pixbuf work. Text is not: libmb only
 * renders fonts onto an X drawable, so labels have to wait until the
 * finished image has been blitted -- see paint_callback(). */
static void
draw_button_bg (MBPixbufImage *img, int x, int w, int h,
		MBPixbufImage *icon, Bool active, Bool minimized)
{
  int icon_x = x + BTN_PAD;
  int icon_y = (h - IconSize) / 2;

  if (active)
    {
      /* Pressed in: darker than the panel, with a firmer edge. */
      fill_rect   (img, x, 0, w, h, 0, 0, 0, 46);
      draw_border (img, x, 0, w, h, 0, 0, 0, 110);
    }
  else
    {
      fill_rect   (img, x, 0, w, h, 255, 255, 255, 40);
      draw_border (img, x, 0, w, h, 0, 0, 0, 55);
    }

  if (icon != NULL)
    {
      /* A minimised application is still running, so it keeps its button;
       * fading the icon is what says "not on screen right now". */
      if (minimized)
	mb_pixbuf_img_copy_composite_with_alpha (Pb, img, icon,
						 0, 0, IconSize, IconSize,
						 icon_x, icon_y, 128);
      else
	mb_pixbuf_img_copy_composite (Pb, img, icon,
				      0, 0, IconSize, IconSize,
				      icon_x, icon_y);
    }
}

static void
draw_button_label (MBDrawable *drw, Group *g, int h, const char *label)
{
  int text_x = g->x + label_offset (g);
  int text_w = (g->x + g->w - BTN_PAD) - text_x;
  int text_y = (h - mb_font_get_height (Fnt)) / 2;

  if (label == NULL || !label[0] || text_w <= 8)
    return;

  mb_font_render_simple (Fnt, drw, text_x, text_y, text_w,
			 (unsigned char *) label, MB_ENCODING_UTF8, 0);
}

static void
paint_callback (MBTrayApp *app, Drawable pxm)
{
  MBPixbufImage *img_bg;
  MBDrawable    *drw;
  int            h = mb_tray_app_height (app);
  int            i;

  img_bg = mb_tray_app_get_background (app, Pb);
  if (img_bg == NULL)
    return;

  for (i = 0; i < NVisible; i++)
    {
      Group *g = &Groups[i];

      draw_button_bg (img_bg, g->x, g->w, h, g->icon,
		      g->has_active, g->all_minimized);
    }

  if (Overflow)
    draw_button_bg (img_bg, OverflowX, OverflowW, h, NULL, False, False);

  drw = mb_drawable_new_from_pixmap (Pb, pxm);
  if (drw == NULL)
    {
      mb_pixbuf_img_free (Pb, img_bg);
      return;
    }

  mb_pixbuf_img_render_to_drawable (Pb, img_bg, mb_drawable_pixmap (drw),
				    0, 0);

  for (i = 0; i < NVisible; i++)
    {
      Group *g = &Groups[i];
      char   label[NAME_LEN + 16];

      /* Squeezed down to a square, there is no room for anything but the
       * icon; the name is still reachable through the window menu. */
      if (g->w <= label_offset (g) + BTN_PAD)
	continue;

      button_text (g, label, sizeof (label));
      draw_button_label (drw, g, h, label);
    }

  if (Overflow)
    {
      /* Centred, not icon-indented: this button has no icon. */
      int tw = text_width (OVERFLOW_LABEL);
      int tx = OverflowX + (OverflowW - tw) / 2;

      mb_font_render_simple (Fnt, drw, tx,
			     (h - mb_font_get_height (Fnt)) / 2,
			     OverflowW, (unsigned char *) OVERFLOW_LABEL,
			     MB_ENCODING_UTF8, 0);
    }

  mb_pixbuf_img_free (Pb, img_bg);
  mb_drawable_unref (drw);
}

/* ------------------------------------------------------------------ */
/* driving the window manager                                          */

static void
send_root_message (Window win, Atom message, long d0, long d1, long d2)
{
  XEvent ev;

  memset (&ev, 0, sizeof (ev));
  ev.xclient.type         = ClientMessage;
  ev.xclient.window       = win;
  ev.xclient.message_type = message;
  ev.xclient.format       = 32;
  ev.xclient.data.l[0]    = d0;
  ev.xclient.data.l[1]    = d1;
  ev.xclient.data.l[2]    = d2;

  XSendEvent (Dpy, Root, False,
	      SubstructureRedirectMask | SubstructureNotifyMask, &ev);
  XFlush (Dpy);
}

static void
activate_window (Window win)
{
  send_root_message (win, Atoms[ATOM_ACTIVE_WINDOW], 2, CurrentTime, 0);
}

static void
minimize_window (Window win)
{
  /* 1 = _NET_WM_STATE_ADD */
  send_root_message (win, Atoms[ATOM_WM_STATE_PROP], 1,
		     (long) Atoms[ATOM_WM_STATE_HIDDEN], 0);
}

/* Clicking the button of whatever is already on screen minimises it, the
 * way it does on every other desktop; clicking anything else raises it. */
static void
toggle_window (Window win, Bool is_active)
{
  if (is_active)
    minimize_window (win);
  else
    activate_window (win);
}

/* ------------------------------------------------------------------ */
/* menus                                                               */

/* Menu items carry the target window by value, never a pointer into
 * Tasks[]. A rescan can renumber that array while the menu is on screen,
 * and a menu item holding a stale index would raise the wrong window --
 * or, worse, one that has since been closed. */
static void
menu_window_activate (MBMenuItem *item)
{
  Window win = (Window) (long) mb_menu_item_get_user_data (item);
  Window active;

  if (win == None)
    return;

  active = active_app_window ();
  toggle_window (win, win == active);
}

static void
menu_free (void)
{
  if (Menu != NULL)
    {
      mb_menu_free (Menu);
      Menu = NULL;
    }
}

static MBMenuMenu *
menu_start (void)
{
  menu_free ();

  Menu = mb_menu_new (mb_tray_app_xdisplay (App), mb_tray_app_xscreen (App));
  if (Menu == NULL)
    return NULL;

  return mb_menu_get_root_menu (Menu);
}

static void
menu_popup (int at_x)
{
  int abs_x, abs_y, menu_w, menu_h;

  if (Menu == NULL)
    return;

  mb_tray_app_get_absolute_coords (App, &abs_x, &abs_y);
  mb_menu_get_root_menu_size (Menu, &menu_w, &menu_h);

  abs_x += at_x;

  /* Keep it on screen: a button near the right edge would otherwise open
   * a menu that runs off it. */
  if (abs_x + menu_w > DisplayWidth (Dpy, mb_tray_app_xscreen (App)))
    abs_x = DisplayWidth (Dpy, mb_tray_app_xscreen (App)) - menu_w;
  if (abs_x < 0)
    abs_x = 0;

  if (mb_tray_app_tray_is_vertical (App))
    {
      mb_menu_activate (Menu, abs_x, abs_y);
      return;
    }

  /* South-facing panel: the menu opens upwards, so it is positioned by
   * its bottom edge -- which mb_menu takes as y + height. */
  if (abs_y > (DisplayHeight (Dpy, mb_tray_app_xscreen (App)) / 2))
    mb_menu_activate (Menu, abs_x, abs_y);
  else
    mb_menu_activate (Menu, abs_x, abs_y + mb_tray_app_height (App) + menu_h);
}

static void
popup_group_menu (Group *g)
{
  MBMenuMenu *root = menu_start ();
  int         i;

  if (root == NULL)
    return;

  mb_menu_set_icon_size (Menu, 16);

  for (i = 0; i < g->n_tasks; i++)
    {
      Task       *t = &Tasks[g->task[i]];
      MBMenuItem *item;
      char        title[TITLE_LEN + 8];

      if (t->minimized)
	snprintf (title, sizeof (title), "[%s]", t->title);
      else
	str_copy (title, sizeof (title), t->title);

      item = mb_menu_add_item_to_menu (Menu, root, title, NULL, NULL,
				       menu_window_activate,
				       (void *) (long) t->win,
				       MBMENU_ITEM_APP);

      if (item != NULL && g->icon != NULL)
	mb_menu_item_icon_set (Menu, item, g->icon);
    }

  menu_popup (g->x);
}

static void
popup_overflow_menu (void)
{
  MBMenuMenu *root = menu_start ();
  int         i, j;

  if (root == NULL)
    return;

  mb_menu_set_icon_size (Menu, 16);

  for (i = NVisible; i < NGroups; i++)
    {
      Group *g = &Groups[i];

      for (j = 0; j < g->n_tasks; j++)
	{
	  Task       *t = &Tasks[g->task[j]];
	  MBMenuItem *item;
	  char        title[NAME_LEN + TITLE_LEN + 8];

	  snprintf (title, sizeof (title), "%s: %s", g->label, t->title);

	  item = mb_menu_add_item_to_menu (Menu, root, title, NULL, NULL,
					   menu_window_activate,
					   (void *) (long) t->win,
					   MBMENU_ITEM_APP);

	  if (item != NULL && g->icon != NULL)
	    mb_menu_item_icon_set (Menu, item, g->icon);
	}
    }

  menu_popup (OverflowX);
}

/* ------------------------------------------------------------------ */
/* callbacks                                                           */

static void
relayout_and_paint (void)
{
  int want = layout ();
  int h    = mb_tray_app_height (App);

  if (want < 1)
    want = 1;

  /* The panel reads a square request as "this applet wants to be square"
   * and overrides the width with the height (panel_app.c), so a taskbar
   * that happened to want exactly h pixels would silently get h back
   * forever. One pixel wider is enough to stay out of that branch. */
  if (want == h)
    want = h + 1;

  if (want != LastWidth)
    {
      LastWidth = want;
      mb_tray_app_request_size (App, want, h);
    }

  mb_tray_app_repaint (App);
}

static void
update_visibility (void)
{
  int want = (NGroups > 0);

  if (want == Visible)
    return;

  if (want)
    mb_tray_app_unhide (App);
  else
    mb_tray_app_hide (App);

  /* Unhiding re-docks from scratch, so the panel knows nothing about the
   * width we asked for last time we were up. Forget it too, or the next
   * relayout sees "same as before" and never sends the request. */
  LastWidth = -1;

  Visible = want;
}

/* One place decides when it is safe to rebuild. mb_menu's items point at
 * memory this rebuild reuses, and mb_menu dispatches clicks from inside
 * our own event loop, so rebuilding under an open menu is a
 * use-after-free waiting for the user to tap. Defer instead; the poll
 * picks it up once the menu closes. */
static void
request_rebuild (void)
{
  if (Menu != NULL && mb_menu_is_active (Menu))
    {
      Dirty = True;
      return;
    }

  Dirty = False;
  rebuild ();
  update_visibility ();
  relayout_and_paint ();
}

static void
button_callback (MBTrayApp *app, int x, int y, Bool is_released)
{
  int i;

  (void) app;
  (void) y;

  if (!is_released)
    return;

  if (Menu != NULL && mb_menu_is_active (Menu))
    return;

  if (Overflow && x >= OverflowX && x < OverflowX + OverflowW)
    {
      popup_overflow_menu ();
      return;
    }

  for (i = 0; i < NVisible; i++)
    {
      Group *g = &Groups[i];

      if (x < g->x || x >= g->x + g->w)
	continue;

      if (g->n_tasks > 1)
	popup_group_menu (g);
      else if (g->n_tasks == 1)
	toggle_window (Tasks[g->task[0]].win, g->has_active);

      return;
    }
}

static void
xevent_callback (MBTrayApp *app, XEvent *ev)
{
  (void) app;

  if (Menu != NULL)
    mb_menu_handle_xevent (Menu, ev);

  if (ev->type == PropertyNotify && ev->xproperty.window == Root)
    {
      if (ev->xproperty.atom == Atoms[ATOM_CLIENT_LIST]
	  || ev->xproperty.atom == Atoms[ATOM_ACTIVE_WINDOW]
	  || ev->xproperty.atom == Atoms[ATOM_MB_CURRENT_APP])
	request_rebuild ();
    }
}

static void
timeout_callback (MBTrayApp *app)
{
  (void) app;

  if (Menu != NULL && mb_menu_is_active (Menu))
    return;

  if (desktops_changed ())
    {
      scan_desktops ();
      icons_flush ();		/* names and icons may both have changed */
      Dirty = True;
    }

  if (Dirty)
    {
      request_rebuild ();
      return;
    }

  /* Titles change without any root property changing -- a terminal
   * retitles itself on every cd. Cheap to re-check: a couple of property
   * reads per window. */
  {
    Task  old[MAX_TASKS];
    int   old_n = NTasks;
    Bool  changed = False;
    int   i;

    memcpy (old, Tasks,
	    sizeof (Task) * (NTasks < MAX_TASKS ? NTasks : MAX_TASKS));
    rebuild ();

    if (old_n != NTasks)
      changed = True;
    else
      for (i = 0; i < NTasks; i++)
	if (old[i].win != Tasks[i].win
	    || old[i].minimized != Tasks[i].minimized
	    || strcmp (old[i].title, Tasks[i].title) != 0)
	  {
	    changed = True;
	    break;
	  }

    if (changed)
      {
	update_visibility ();
	relayout_and_paint ();
      }
    else
      {
	/* rebuild() re-creates Groups[] from scratch, which zeroes the x/w
	 * that layout() put there -- so even a rebuild that changed
	 * nothing visible has to be followed by one, or the next tap is
	 * matched against a row of zero-width buttons and does nothing. */
	layout ();
      }
  }
}

static void
resize_callback (MBTrayApp *app, int w, int h)
{
  (void) app;
  (void) w;

  /* The panel decides our height; the icon follows it rather than the
   * other way round. Anything below 16 is not recognisable. */
  IconSize = h - 8;
  if (IconSize < 16)
    IconSize = 16;
  if (IconSize > 32)
    IconSize = 32;

  if (Fnt != NULL && FontPixels == 0)
    {
      int px = h / 2;

      if (px < 10) px = 10;
      if (px > 16) px = 16;

      mb_font_set_size_to_pixels (Fnt, px, NULL);
    }

  icons_flush ();		/* cached at the old size */
  Dirty = True;
}

static void
theme_change_callback (MBTrayApp *app, char *theme_name)
{
  if (theme_name == NULL)
    return;

  if (ThemeName != NULL)
    free (ThemeName);
  ThemeName = strdup (theme_name);

  icons_flush ();
  request_rebuild ();

  mb_tray_app_repaint (app);
}

/* ------------------------------------------------------------------ */

static void
usage (const char *name)
{
  fprintf (stderr,
	   "Usage: %s [options]\n"
	   "  -s, --font-size <pixels>  label size (default: half the panel)\n"
	   "  -i, --icon-size <pixels>  icon size (default: panel height - 8)\n"
	   "  -d, --dump                print what would be shown, then exit\n",
	   name);
  exit (1);
}

/* Scan once, print the result, exit. There is no debugger on the target
 * and the panel swallows an applet's stderr, so "what does it think is
 * running, and why did it pick that icon" is otherwise unanswerable
 * without a camera pointed at the screen. Runs without a system tray --
 * it needs a display connection, not a dock. */
static void
dump (void)
{
  int i, j;

  scan_desktops ();
  rebuild ();
  layout ();

  printf ("%d .desktop entries indexed from %s\n", NDesktops, APPS_DIR);
  for (i = 0; i < NDesktops; i++)
    printf ("  exec=%-20s name=%-24s icon=%s\n",
	    Desktops[i].exec, Desktops[i].name, Desktops[i].icon);

  printf ("\n%d task(s) in %d group(s), %d shown%s\n",
	  NTasks, NGroups, NVisible, Overflow ? " + overflow" : "");

  for (i = 0; i < NGroups; i++)
    {
      Group *g = &Groups[i];
      char   label[NAME_LEN + 16];

      button_text (g, label, sizeof (label));

      printf ("  [%d] key=%-16s label=%-24s x=%-4d w=%-4d%s%s icon=%s\n",
	      i, g->key, label, g->x, g->w,
	      g->has_active ? " ACTIVE" : "",
	      g->all_minimized ? " MIN" : "",
	      g->icon ? "yes" : "none");

      for (j = 0; j < g->n_tasks; j++)
	{
	  Task *t = &Tasks[g->task[j]];
	  printf ("        win=0x%lx %s%s\n", (unsigned long) t->win,
		  t->title, t->minimized ? " (minimised)" : "");
	}
    }
}

int
main (int argc, char *argv[])
{
  struct timeval tv;
  int            i;
  int            icon_size_opt = 0;
  Bool           want_dump = False;

  App = mb_tray_app_new ((unsigned char *) _("Tasks"),
			 resize_callback,
			 paint_callback,
			 &argc,
			 &argv);
  if (App == NULL)
    {
      fprintf (stderr, "mb-applet-tasks: failed to create tray app\n");
      exit (1);
    }

  /* After mb_tray_app_new(), which strips the arguments it handles. */
  for (i = 1; i < argc; i++)
    {
      if (!strcmp (argv[i], "-s") || !strcmp (argv[i], "--font-size"))
	{
	  if (++i >= argc) usage (argv[0]);
	  FontPixels = atoi (argv[i]);
	  if (FontPixels < 6) usage (argv[0]);
	}
      else if (!strcmp (argv[i], "-i") || !strcmp (argv[i], "--icon-size"))
	{
	  if (++i >= argc) usage (argv[0]);
	  icon_size_opt = atoi (argv[i]);
	  if (icon_size_opt < 8) usage (argv[0]);
	}
      else if (!strcmp (argv[i], "-d") || !strcmp (argv[i], "--dump"))
	want_dump = True;
      else
	usage (argv[0]);
    }

  Dpy  = mb_tray_app_xdisplay (App);
  Root = mb_tray_app_xrootwin (App);

  XInternAtoms (Dpy, (char **) AtomNames, ATOM_COUNT, False, Atoms);

  Pb = mb_pixbuf_new (Dpy, mb_tray_app_xscreen (App));
  if (Pb == NULL)
    {
      fprintf (stderr, "mb-applet-tasks: failed to create pixbuf\n");
      exit (1);
    }

  Fnt = mb_font_new_from_string (Dpy, "Sans");
  if (Fnt == NULL)
    {
      fprintf (stderr, "mb-applet-tasks: failed to open a font\n");
      exit (1);
    }

  ColText = mb_col_new_from_spec (Pb, "#000000");
  mb_font_set_color (Fnt, ColText);

  if (FontPixels)
    mb_font_set_size_to_pixels (Fnt, FontPixels, NULL);
  if (icon_size_opt)
    IconSize = icon_size_opt;

  if (want_dump)
    {
      dump ();
      return 0;
    }

  mb_tray_app_set_button_callback (App, button_callback);
  mb_tray_app_set_xevent_callback (App, xevent_callback);
  mb_tray_app_set_theme_change_callback (App, theme_change_callback);

  /* The panel puts START-gravity applets on the left, which is where a
   * taskbar belongs; mb-applet-menu-launcher asks for the same thing and
   * ends up to our left because it is started after us. */
  mb_tray_app_request_offset (App, -1);

  desktops_changed ();		/* prime the mtime */
  scan_desktops ();
  rebuild ();

  tv.tv_sec  = POLL_SECS;
  tv.tv_usec = 0;
  mb_tray_app_set_timeout_callback (App, timeout_callback, &tv);

  /* Same reasoning as mb-applet-card: pre-hide rather than hide after
   * docking, so a session that starts with no windows open never creates
   * a tray window at all instead of docking and flickering away. Do NOT
   * call mb_tray_app_main_init() -- mb_tray_app_main() does it. */
  if (NGroups == 0)
    mb_tray_app_hide (App);
  Visible = (NGroups > 0);

  mb_tray_app_main (App);

  return 0;
}
