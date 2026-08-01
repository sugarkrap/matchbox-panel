/*
 *  mb-applet-system-monitor - tiny sys monitor
 *
 *  cpu reading code based on wmbubblemon
 *
 *  Two bars normally -- CPU in green, memory in red -- and a third, blue
 *  one for swap whenever there is swap to show. This machine's only swap
 *  area is a file on the SD card (see userspace/src/cardswap.c), so the
 *  third bar exists exactly while a card is mounted and its swapfile came
 *  up, and disappears again when the card goes. The applet grows and
 *  shrinks with it: the icon is a picture of two tubes or of three, and
 *  the panel is told the new width so it can re-lay-out around us.
 *
 *  Nothing tells us when that happens -- the card is mounted behind our
 *  back by mdev -- so, like mb-applet-card, we watch for it, here by
 *  polling /proc/swaps a couple of times a second.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Street #330, Boston, MA 02111-1307, USA.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

#include <libmb/mb.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef ENABLE_NLS
# include <libintl.h>
# define _(text) gettext(text)
#else
# define _(text) (text)
#endif

#ifdef MB_HAVE_PNG
#define IMG_EXT "png"
#else
#define IMG_EXT "xpm"
#endif

#define MINISYS_IMG  "minisys." IMG_EXT

/* The three-meter variant of the same icon: the identical tube, three times
 * over, on a canvas half again as wide (48x32 rather than 32x32, and 24x16
 * rather than 16x16 in the small-icon set) with the original's margins and
 * gap. It is shown only while there is a swap area to put in the third
 * tube; see swap_is_active() below.
 *
 * Regenerating it: crop columns 2..13 out of minisys.png (columns 1..6 for
 * the 16px one) and paste that tube at x = 2, 18, 34 (x = 1, 9, 17) on the
 * wider transparent canvas. Copying the pixels rather than redrawing them
 * is what keeps the bevel and its antialiasing identical between the two
 * icons, so the applet does not visibly change style when it grows. */
#define MINISYS3_IMG "minisys3." IMG_EXT

/* Meter geometry, in 32nds of the ICON'S HEIGHT.
 *
 * Both icons are 32 units tall and their wells -- the recessed slots the
 * bars are painted into -- sit at the same places relative to that height,
 * so one set of numbers describes every size and both variants:
 *
 *      well i starts at (WELL_X0 + i * WELL_PITCH), is WELL_W wide,
 *      and runs from WELL_Y for WELL_H
 *
 * The two-tube icon is ICON_W_2BAR units wide and has wells 0 and 1; the
 * three-tube one is ICON_W_3BAR wide and has wells 0, 1 and 2. (The old
 * code spelled the same positions as width/4 - width/16 and width/8, which
 * only worked while the icon was square.) */
#define ICON_UNITS   32
#define ICON_W_2BAR  32
#define ICON_W_3BAR  48
#define WELL_X0       6
#define WELL_PITCH   16
#define WELL_W        4
#define WELL_Y        6
#define WELL_H       20

#define PROC_SWAPS   "/proc/swaps"

/* Only swap on the SD card counts. This applet's third bar is specifically
 * the card's swapfile (/mnt/card/.zaurus/swap -- created and enabled by
 * /usr/sbin/cardswap when the card is mounted, and turned off again when it
 * goes away), so the bar appearing means "the card is mounted AND its swap
 * came up", which is exactly the state worth showing. Matching on any swap
 * area at all would be broader but would say less. */
#define CARD_SWAP_PREFIX "/mnt/card/"

/* How many 400ms timeout ticks between /proc/swaps checks. */
#define SWAP_CHECK_TICKS 5

struct {

   /* cpu data  */
   int loadIndex;
   int samples;
   u_int64_t *load, *total;

   /* memory data  */
   u_int64_t mem_used;
   u_int64_t mem_max;
   u_int64_t swap_used;
   u_int64_t swap_max;
   unsigned int swap_percent;  /* swap used, in percent */
   unsigned int mem_percent;   /* memory used, in percent */

} msd;

MBPixbuf *pb = NULL;
/* ImgIcon is an alias for whichever of the two loaded icons is in use; it
 * is never freed through that name. ImgIcon3 stays NULL if the wider icon
 * is not installed, and that alone pins the applet to two bars forever --
 * an old icon theme degrades to the old appearance instead of drawing a
 * bar into thin air. */
MBPixbufImage *ImgIcon = NULL, *ImgIcon2 = NULL, *ImgIcon3 = NULL;
MBPixbufImage *ImgIconScaled = NULL, *ImgGraph = NULL;
int GraphHeight = 0, GraphWidth = 0;
char *ThemeName;
int IsKernel26 = 0;
int Bars = 2;			/* 2 without card swap, 3 with it */

int 
check_if_kernel_2_6(void)
{
  float v_nr=0;
  FILE  *version;
  
  if ((version = fopen("/proc/version", "r")) == NULL)
    {
      fprintf(stderr, "mb-applet-system-monitor: failed to open /proc/version. Exiting\n");
      exit(1);
    }
  fscanf(version, "%*s %*s %f", &v_nr);
  fclose(version);
  
  return (v_nr > 2.5);
}

/* returns current CPU load in percent, 0 to 100 */
int system_cpu(void)
{
    unsigned int cpuload;
    u_int64_t load, total, oload, ototal;
    u_int64_t ab, ac, ad, ae;
    int i;
    FILE *stat;

    if ((stat = fopen("/proc/stat", "r")) == NULL)
      {
	fprintf(stderr, "mb-applet-system-monitor: failed to open /proc/stat. Exiting\n");
	exit(1);
      }

    fscanf(stat, "%*s %Ld %Ld %Ld %Ld", &ab, &ac, &ad, &ae);
    fclose(stat);

    /* Find out the CPU load */
    /* user + sys = load
     * total = total */
    load = ab + ac + ad;	/* cpu.user + cpu.sys; */
    total = ab + ac + ad + ae;	/* cpu.total; */

    /* "i" is an index into a load history */
    i = msd.loadIndex;
    oload = msd.load[i];
    ototal = msd.total[i];

    msd.load[i] = load;
    msd.total[i] = total;
    msd.loadIndex = (i + 1) % msd.samples;

    if (ototal == 0)	
	cpuload = 0;
    else
	cpuload = (100 * (load - oload)) / (total - ototal);

    return cpuload;
}

int system_memory(void)
{
  u_int64_t  total, mfree, buffers, cached, used, shared = 0,
    cache_total, cache_free, cache_used = 0;
  
  u_int64_t my_mem_used, my_mem_max;
  u_int64_t my_swap_max;
  
  static int mem_delay = 0;
  FILE      *mem;
  
  /* put this in permanent storage instead of stack */
  static char not_needed[2048];
  
  if (mem_delay-- <= 0) 
    {
      if ((mem = fopen("/proc/meminfo", "r")) == NULL) 
	{
	  fprintf(stderr, "mb-applet-system-monitor: failed to open /proc/meminfo. Exiting.\n");
	  exit(1);
	}
      
      if (IsKernel26)
	{
	  /* Parse /proc/meminfo by KEY, not by position.
	   *
	   * This used to be a run of positional fscanf()s matching the
	   * field order of 2.6.0: MemTotal, MemFree, Buffers, Cached, ...,
	   * SwapTotal, SwapFree.  The kernel has inserted fields since --
	   * MemAvailable landed third, in 3.14 -- so on anything modern
	   * every value after MemFree was read out of the wrong line:
	   * "buffers" got MemAvailable, "cached" got Buffers, and the two
	   * swap slots landed nowhere near SwapTotal/SwapFree.  The
	   * resulting `cache_used + used - cached - buffers` then underflowed
	   * (these are unsigned) into an enormous mem_used and a nonsense
	   * percentage.  Keying off the labels is immune to field order.  */
	  u_int64_t available = 0;
	  Bool      have_total = False, have_available = False;

	  total = mfree = buffers = cached = 0;
	  cache_total = cache_free = 0;

	  while (fgets(not_needed, 2048, mem) != NULL)
	    {
	      /* Scan into an explicit unsigned long long: u_int64_t is
	       * "unsigned long" on some hosts and "unsigned long long" on
	       * others, so no single length modifier is right for both. */
	      unsigned long long val;

	      /* Every line is "Key:   <n> kB" (a few have no unit). */
	      if (sscanf(not_needed, "MemTotal: %llu", &val) == 1)
		{ total = val; have_total = True; }
	      else if (sscanf(not_needed, "MemFree: %llu", &val) == 1)
		mfree = val;
	      else if (sscanf(not_needed, "MemAvailable: %llu", &val) == 1)
		{ available = val; have_available = True; }
	      else if (sscanf(not_needed, "Buffers: %llu", &val) == 1)
		buffers = val;
	      else if (sscanf(not_needed, "Cached: %llu", &val) == 1)
		cached = val;	/* NB: "SwapCached:" does not match this */
	      else if (sscanf(not_needed, "SwapTotal: %llu", &val) == 1)
		cache_total = val;
	      else if (sscanf(not_needed, "SwapFree: %llu", &val) == 1)
		cache_free = val;
	    }

	  if (!have_total)	/* not a layout we understand at all */
	    {
	      fclose(mem);
	      mem_delay = 25;
	      return 0;
	    }

	  /* MemAvailable is the kernel's own estimate of what a new
	   * allocation could get without swapping, so total - available is
	   * the honest "in use" figure. Without it, fall back to excluding
	   * the reclaimable pages by hand. */
	  if (have_available)
	    used = (available < total) ? total - available : 0;
	  else
	    {
	      u_int64_t reclaimable = mfree + buffers + cached;
	      used = (reclaimable < total) ? total - reclaimable : 0;
	    }

	  total       = total * 1024;
	  mfree       = mfree * 1024;
	  buffers     = buffers * 1024;
	  cached      = cached * 1024;
	  used        = used * 1024;
	  cache_total = cache_total * 1024;
	  cache_used  = cache_total - (cache_free * 1024);
	}
      else
	{ /* Assume 2.4  */
	  /*
	    total:    used:    free:  shared: buffers:  cached:
	  */
	  fgets(not_needed, 2048, mem);
	  fscanf(mem, "%*s %Ld %Ld %Ld %Ld %Ld %Ld", &total, &used, &mfree,
		 &shared, &buffers, &cached);
	  fscanf(mem, "%*s %Ld %Ld", &cache_total, &cache_used);
	  used = (cached + buffers < used) ? used - cached - buffers : 0;
	}

      fclose(mem);

      mem_delay = 25;

      /* calculate it */
      my_mem_max      = total;
      my_swap_max     = cache_total;
      my_mem_used     = used;
      msd.mem_used    = my_mem_used;
      msd.mem_max     = my_mem_max;
      /* Swap is reported separately -- folding it into a percentage OF RAM
       * is what let this bar read past 100%. */
      msd.swap_used   = cache_used;
      msd.swap_max    = my_swap_max;
      msd.swap_percent = my_swap_max ? (100 * cache_used) / my_swap_max : 0;
      msd.mem_percent = my_mem_max ? (100 * my_mem_used) / my_mem_max : 0;
      if (msd.mem_percent > 100) msd.mem_percent = 100;
      if (msd.swap_percent > 100) msd.swap_percent = 100;

    /* memory info changed - update things */
    return 1;
  }
  /* nothing new */
  return 0;
}

/* Is the SD card's swapfile currently enabled?
 *
 * /proc/swaps rather than /proc/meminfo's SwapTotal, because the question
 * is not "is there swap" but "is there swap ON THE CARD" -- see
 * CARD_SWAP_PREFIX. The file is one line per swap area plus a header, and
 * the kernel writes the area's path in the first column (octal-escaping
 * any space in it; the path we look for has none). It is a couple of
 * hundred bytes and this is called every SWAP_CHECK_TICKS repaints, not
 * every one. */
int
swap_is_active (void)
{
  FILE *f;
  char  line[512];
  int   active = 0;

  if ((f = fopen(PROC_SWAPS, "r")) == NULL)
    return 0;			/* kernel without swap support at all */

  /* Drop the header ("Filename  Type  Size  Used  Priority"). */
  if (fgets(line, sizeof(line), f) != NULL)
    {
      while (fgets(line, sizeof(line), f) != NULL)
	{
	  if (strncmp(line, CARD_SWAP_PREFIX,
		      strlen(CARD_SWAP_PREFIX)) == 0)
	    {
	      active = 1;
	      break;
	    }
	}
    }

  fclose(f);
  return active;
}

void
paint_callback (MBTrayApp *app, Drawable drw )
{
  static int prev_pixels[3] = { -1, -1, -1 };
  static int prev_bars = -1;

  /* One row per meter, left to right, in the order the wells appear in the
   * icon. Colours: the CPU and memory bars keep the pure green and red
   * they have always had. Swap gets blue, but a lightened one rather than
   * 0x0000ff -- against the 0x666666 well, pure blue is the one primary
   * that is genuinely hard to pick out on this machine's transflective
   * panel, especially in daylight. This still reads unambiguously as
   * "the blue one". */
  static const struct { unsigned char r, g, b; } colour[3] = {
    { 0x00, 0xff, 0x00 },	/* cpu    */
    { 0xff, 0x00, 0x00 },	/* memory */
    { 0x33, 0x66, 0xff }	/* swap   */
  };

  int        percent[3];
  int        pixels[3];
  int        i, x, y;
  int        icon_w, icon_units_w;
  int        well_y, well_h, well_w;
  int        changed = 0;

  MBPixbufImage *img_backing = NULL;

  if (ImgIconScaled == NULL)
    return;

  system_memory(); 		/* Update reading */

  percent[0] = system_cpu();
  percent[1] = msd.mem_percent;
  percent[2] = msd.swap_percent;

  /* Everything is derived from the SCALED icon's own size, so the wells
   * line up with the painted tubes whatever size the tray gave us -- and
   * whichever of the two icons is in it. */
  icon_w       = mb_pixbuf_img_get_width(ImgIconScaled);
  icon_units_w = (Bars == 3) ? ICON_W_3BAR : ICON_W_2BAR;

  well_y = (WELL_Y * mb_pixbuf_img_get_height(ImgIconScaled)) / ICON_UNITS;
  well_h = (WELL_H * mb_pixbuf_img_get_height(ImgIconScaled)) / ICON_UNITS;
  well_w = (WELL_W * icon_w) / icon_units_w;

  if (well_w < 1) well_w = 1;
  if (well_h < 1) well_h = 1;

  for (i = 0; i < Bars; i++)
    {
      /* Clamped, so the fill loop below cannot climb out of its tube:
       * system_cpu() can read over 100 across a sampling boundary. */
      if (percent[i] < 0)   percent[i] = 0;
      if (percent[i] > 100) percent[i] = 100;

      pixels[i] = (percent[i] * well_h) / 100;
      if (pixels[i] != prev_pixels[i])
	changed = 1;
    }

  /* A mode change redraws even when every bar happens to be unmoved: the
   * icon underneath it is a different one. */
  if (!changed && Bars == prev_bars)
    return;

  img_backing = mb_tray_app_get_background (app, pb);

  mb_pixbuf_img_composite(pb, img_backing, ImgIconScaled, 0, 0);

  for (i = 0; i < Bars; i++)
    {
      int well_x = (((WELL_X0 + i * WELL_PITCH) * icon_w) / icon_units_w);

      /* Clear the well to its empty colour, then fill it from the bottom
       * up. Both loops are bounded by the well, so a bar can never run
       * past the tube that contains it. */
      for (y = well_y; y < well_y + well_h; y++)
	for (x = 0; x < well_w; x++)
	  mb_pixbuf_img_plot_pixel(pb, img_backing, well_x + x, y,
				   0x66, 0x66, 0x66);

      for (y = well_y + well_h - 1; y >= well_y + well_h - pixels[i]; y--)
	for (x = well_x; x < well_x + well_w; x++)
	  mb_pixbuf_img_plot_pixel(pb, img_backing, x, y,
				   colour[i].r, colour[i].g, colour[i].b);

      prev_pixels[i] = pixels[i];
    }

  /* XXX Alert here for low memory  */

  mb_pixbuf_img_render_to_drawable(pb, img_backing, drw, 0, 0);

  mb_pixbuf_img_free( pb, img_backing );

  prev_bars = Bars;
}

void
button_callback (MBTrayApp *app, int x, int y, Bool is_released )
{
  char tray_msg[256];
  int cpu = system_cpu();

  if (!is_released)
    return;

  if (Bars == 3)
    snprintf(tray_msg, sizeof(tray_msg),
	     _("CPU: %i %%, MEMORY: %i %%, SWAP: %i %%\n"),
	     cpu, msd.mem_percent, msd.swap_percent);
  else
    snprintf(tray_msg, sizeof(tray_msg),
	     _("CPU: %i %%, MEMORY: %i %%\n"), cpu, msd.mem_percent);

  mb_tray_app_tray_send_message(app, tray_msg, 5000);
}

void
resize_callback (MBTrayApp *app, int w, int h )
{

 if (ImgIconScaled) mb_pixbuf_img_free(pb, ImgIconScaled);
 if (ImgGraph) mb_pixbuf_img_free(pb, ImgGraph);

 ImgIconScaled = NULL;
 ImgGraph = NULL;

 if (ImgIcon)
   ImgIconScaled = mb_pixbuf_img_scale(pb, ImgIcon, w, h);

}

/* Switch between the two-bar and three-bar shapes.
 *
 * The applet is square with two bars and half again as wide with three, so
 * this is a real size change and the panel has to be told about it -- it
 * lays applets out side by side and cannot discover our new width on its
 * own. mb_tray_app_request_size() resizes the window; the ConfigureNotify
 * that comes back calls resize_callback() with the size we were actually
 * given, which is not necessarily the one we asked for.
 *
 * We nevertheless rescale here too, before returning. Otherwise the next
 * repaint -- which can happen first -- would paint three wells' worth of
 * bars onto a still-two-tube icon. */
void
set_bars (MBTrayApp *app, int bars)
{
  int h, w;

  if (bars == 3 && ImgIcon3 == NULL)
    bars = 2;			/* no wide icon installed; stay as we are */

  if (bars == Bars)
    return;

  Bars    = bars;
  ImgIcon = (Bars == 3) ? ImgIcon3 : ImgIcon2;

  h = mb_tray_app_height(app);
  if (h < 1) h = ICON_UNITS;

  /* Square for two bars, which is also what the panel forces a square
   * request back to; ICON_W_3BAR/ICON_UNITS as wide for three. */
  w = (Bars == 3) ? (h * ICON_W_3BAR) / ICON_UNITS : h;

  mb_tray_app_set_icon(app, pb, ImgIcon);
  mb_tray_app_request_size(app, w, h);
  resize_callback(app, w, h);
}

void
load_icon(void)
{
 char *icon_path = NULL;

 if (ImgIcon2) mb_pixbuf_img_free(pb, ImgIcon2);
 if (ImgIcon3) mb_pixbuf_img_free(pb, ImgIcon3);
 ImgIcon2 = ImgIcon3 = NULL;

 icon_path = mb_dot_desktop_icon_get_full_path (ThemeName,
						32,
						MINISYS_IMG );

 if (icon_path == NULL
     || !(ImgIcon2 = mb_pixbuf_img_new_from_file(pb, icon_path)))
    {
      fprintf(stderr, "miniapm: failed to load icon %s\n", MINISYS_IMG);
      exit(1);
    }

 free(icon_path);

 /* The three-tube icon is optional, and its absence is not an error: an
  * icon theme that predates it, or one that only overrides some names,
  * simply leaves this applet with the two bars it has always had. Silent
  * rather than a warning, because it is a legitimate configuration and
  * this would print it on every theme change. */
 icon_path = mb_dot_desktop_icon_get_full_path (ThemeName,
						32,
						MINISYS3_IMG );
 if (icon_path != NULL)
   {
     ImgIcon3 = mb_pixbuf_img_new_from_file(pb, icon_path);
     free(icon_path);
   }

 if (Bars == 3 && ImgIcon3 == NULL)
   Bars = 2;			/* the new theme has no wide icon */

 ImgIcon = (Bars == 3) ? ImgIcon3 : ImgIcon2;

 return;

}

void
theme_callback (MBTrayApp *app, char *theme_name)
{
  if (!theme_name) return;
  if (ThemeName) free(ThemeName);
  ThemeName = strdup(theme_name);
  load_icon();
  resize_callback (app, mb_tray_app_width(app), mb_tray_app_height(app) );
}

void
timeout_callback ( MBTrayApp *app )
{
  /* Poll for the card's swap area appearing and disappearing, but not on
   * every tick. The timeout fires every 400ms; the state only changes when
   * a card is inserted or removed, so one check every SWAP_CHECK_TICKS
   * ticks -- two seconds -- is far more often than it can matter, and
   * still fast enough that the bar looks like it belongs to the insertion
   * rather than turning up later on its own. */
  static int countdown = 0;

  if (--countdown <= 0)
    {
      countdown = SWAP_CHECK_TICKS;
      set_bars (app, swap_is_active() ? 3 : 2);
    }

  mb_tray_app_repaint (app);
}

int
main( int argc, char *argv[])
{
  MBTrayApp *app = NULL;
  struct timeval tv;

  int i;
  u_int64_t load = 0, total = 0;

#if ENABLE_NLS
  setlocale (LC_ALL, "");
  bindtextdomain (PACKAGE, DATADIR "/locale");
  bind_textdomain_codeset (PACKAGE, "UTF-8");
  textdomain (PACKAGE);
#endif

  IsKernel26 = check_if_kernel_2_6();

  app = mb_tray_app_new ( _("CPU/Mem Monitor"),
			  resize_callback,
			  paint_callback,
			  &argc,
			  &argv );  

   msd.samples = 16;
   
   if (msd.load) {
      load = msd.load[msd.loadIndex];
      free(msd.load);
   }
   
   if (msd.total) {
      total = msd.total[msd.loadIndex];
      free(msd.total);
   }
   
   msd.loadIndex = 0;
   msd.load = malloc(msd.samples * sizeof(u_int64_t));
   msd.total = malloc(msd.samples * sizeof(u_int64_t));
   for (i = 0; i < msd.samples; i++) {
      msd.load[i] = load;
      msd.total[i] = total;
   }
  
  pb = mb_pixbuf_new(mb_tray_app_xdisplay(app), 
		     mb_tray_app_xscreen(app));
   
  memset(&tv,0,sizeof(struct timeval));
  tv.tv_usec = 400000;

  mb_tray_app_set_theme_change_callback (app, theme_callback );

  mb_tray_app_set_timeout_callback (app, timeout_callback, &tv); 

  mb_tray_app_set_button_callback (app, button_callback );
  
  load_icon();

  mb_tray_app_set_icon(app, pb, ImgIcon);
  
  mb_tray_app_main (app);
   
   return 1;
}
