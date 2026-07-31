/*
 *  miniwave - Tiny 820.11 wireless 
 *
 *  Note: you can use themes from http://www.eskil.org/wavelan-applet/  
 *
 *  originally based on wmwave
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

#include <sys/types.h>
#include <sys/ioctl.h>
/* update_wireless() uses rint() and log(). These do currently resolve via
 * iwlib.h, which includes math.h itself, but relying on a third-party
 * header's include list for our own declarations is a trap waiting to
 * spring. Declare what we use. */
#include <math.h>
/* inet_ntop, for the address shown in the popup. struct ifreq and IFNAMSIZ
 * already arrive via iwlib.h, which includes <net/if.h>. */
#include <arpa/inet.h>

#include <netdb.h>      /* gethostbyname, getnetbyname */
#if 0
#include <linux/if_arp.h>   /* For ARPHRD_ETHER */
#include <linux/socket.h>   /* For AF_INET & struct sockaddr */
#endif
#include <sys/socket.h>       /* For struct sockaddr_in */
#if 0
#include <linux/wireless.h>
#else
#include <iwlib.h>

#endif

#ifdef MB_HAVE_PNG
#define IMG_EXT "png"
#else
#define IMG_EXT "xpm"
#endif

enum {
  MW_BROKE = 0,
  MW_NO_LINK,
  MW_SIG_1_40,
  MW_SIG_41_60,
  MW_SIG_61_80,
  MW_SIG_80_100,
};

#define  MW_BROKE_IMG      "broken-0."      IMG_EXT
#define  MW_NO_LINK_IMG    "no-link-0."     IMG_EXT
#define  MW_SIG_1_40_IMG   "signal-1-40."   IMG_EXT
#define  MW_SIG_41_60_IMG  "signal-41-60."  IMG_EXT
#define  MW_SIG_61_80_IMG  "signal-61-80."  IMG_EXT
#define  MW_SIG_80_100_IMG "signal-81-100." IMG_EXT

static char *ImgLookup[64] = {
  MW_BROKE_IMG,      
  MW_NO_LINK_IMG,    
  MW_SIG_1_40_IMG,   
  MW_SIG_41_60_IMG,  
  MW_SIG_61_80_IMG,  
  MW_SIG_80_100_IMG, 

};

static char          *ThemeName = NULL;
static MBPixbuf      *pb;
static MBPixbufImage *Imgs[6] = { 0,0,0,0,0,0 }, 
                     *ImgsScaled[6] = { 0,0,0,0,0,0 };
static int            CurImg = MW_BROKE;
static int            LastImg = -1;



struct {
   char *iface;			/* Interface name */
   char *essid;			/* ESSID */
   char *mode;			/* Mode */
   int   quality;		/* Quality (%) */
   int   level, noise;		/* Signal level, noise (dBm) */
} Mwd;

/* iwlib stuff  */

int  Wfd; /* file descriptor for socket */
static struct wireless_info WInfo;

/* IPv4 address currently configured on an interface, or NULL if it has
 * none (not associated yet, or DHCP still running). Returns a pointer to
 * static storage. */
static const char *
iface_address(const char *ifname)
{
  static char   addr[INET_ADDRSTRLEN];
  struct ifreq  ifr;
  int           fd;

  addr[0] = '\0';

  if (ifname == NULL)
    return NULL;

  /* SIOCGIFADDR needs its own socket; Wfd is one, but it belongs to iwlib
   * and reusing it here would be relying on an implementation detail. */
  fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0)
    return NULL;

  memset(&ifr, 0, sizeof(ifr));
  ifr.ifr_addr.sa_family = AF_INET;
  strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);

  if (ioctl(fd, SIOCGIFADDR, &ifr) == 0)
    {
      struct sockaddr_in *sin = (struct sockaddr_in *) &ifr.ifr_addr;

      if (inet_ntop(AF_INET, &sin->sin_addr, addr, sizeof(addr)) == NULL)
	addr[0] = '\0';
    }

  close(fd);

  return addr[0] != '\0' ? addr : NULL;
}

/* Signal level and noise come back as raw __u8. When they are really dBm
 * the value is a signed 8-bit quantity stuffed into that byte, so anything
 * at or above 64 has to have 0x100 subtracted -- otherwise a perfectly
 * normal -39dBm reads as "217dBm". */
static int
iw_dbm(int raw)
{
  return raw >= 64 ? raw - 0x100 : raw;
}

Bool
update_wireless(void)
{
  /* urg, iwlib api :/ */

  if (Wfd == -1) 
    {
      fprintf(stderr, "mb-applet-wireless: Kernel lacks wireless support?\n" );
      return False;
    }

  if (Mwd.iface == NULL)
      return False;

  if (iw_get_basic_config(Wfd, Mwd.iface, &WInfo.b) < 0)
    {
      fprintf(stderr, "mb-applet-wireless: unable to read wireless config\n" );
      return False;
    }

  if(iw_get_range_info(Wfd, Mwd.iface, &(WInfo.range)) >= 0)
    WInfo.has_range = 1;  

  if (iw_get_stats(Wfd, Mwd.iface, 
		   &(WInfo.stats),
                   &(WInfo.range), WInfo.has_range) >= 0)
    WInfo.has_stats = 1;
    
  Mwd.essid = ( WInfo.b.has_essid ? WInfo.b.essid : NULL );
  Mwd.mode  = ( WInfo.b.has_mode ? (char *)iw_operation_mode[WInfo.b.mode] : NULL );

  if (WInfo.has_stats) 
    {
      /* via http://www.snorp.net/files/patches/wireless-applet.c */
      /* qual.qual is a __u8 and reads 0 with no association (and on hostap
       * while the link is down), where log(0) is -inf and casting that to
       * int is undefined. Take the floor directly instead. */
      if (WInfo.stats.qual.qual == 0)
	Mwd.quality = 0;
      else
	Mwd.quality = (int)rint ((log (WInfo.stats.qual.qual) / log (94)) * 100);

      if (Mwd.quality > 100)
	Mwd.quality = 100;
      else 
	if (Mwd.quality < 0) 
	  Mwd.quality = 0;

      /* Same test iwlib's own printer uses to decide the units. */
      if ((WInfo.stats.qual.updated & IW_QUAL_DBM)
	  || (WInfo.has_range
	      && WInfo.stats.qual.level > WInfo.range.max_qual.level))
	{
	  Mwd.level = iw_dbm(WInfo.stats.qual.level);
	  Mwd.noise = iw_dbm(WInfo.stats.qual.noise);
	}
      else
	{
	  Mwd.level = (int)WInfo.stats.qual.level;
	  Mwd.noise = (int)WInfo.stats.qual.noise;
	}
    }
  else 
    {
      Mwd.quality = -1;
      Mwd.level = -1;
      Mwd.noise = -1;
    }

  return True;
}

void
paint_callback (MBTrayApp *app, Drawable drw )
{

  MBPixbufImage *img_backing = NULL;

  if (update_wireless())
    {
      if (Mwd.quality != -1)
	{
	  if (Mwd.quality >= 0 && Mwd.quality <= 40)
	    CurImg = MW_SIG_1_40;
	  else if (Mwd.quality > 40 && Mwd.quality <= 60)
	    CurImg = MW_SIG_41_60;
	  else if (Mwd.quality > 60 && Mwd.quality <= 80)
	    CurImg = MW_SIG_61_80;
	  else if (Mwd.quality > 80)
	    CurImg = MW_SIG_80_100;
	  else
	    CurImg = MW_NO_LINK;
	}
      else CurImg = MW_NO_LINK;
    } 
  else CurImg = MW_BROKE;

  if (LastImg == CurImg) return;
  
  img_backing = mb_tray_app_get_background (app, pb);

  mb_pixbuf_img_copy_composite(pb, img_backing, 
			       ImgsScaled[CurImg], 0, 0,
			       mb_pixbuf_img_get_width(ImgsScaled[0]),
			       mb_pixbuf_img_get_height(ImgsScaled[0]),
			       mb_tray_app_tray_is_vertical(app) ? 
			       (mb_pixbuf_img_get_width(img_backing)-mb_pixbuf_img_get_width(ImgsScaled[0]))/2 : 0,
			       mb_tray_app_tray_is_vertical(app) ? 0 : 
			       (mb_pixbuf_img_get_height(img_backing)-mb_pixbuf_img_get_height(ImgsScaled[0]))/2 );

  mb_pixbuf_img_render_to_drawable(pb, img_backing, drw, 0, 0);

  mb_pixbuf_img_free( pb, img_backing );

  LastImg = CurImg;
}


void
load_icons(MBTrayApp *app)
{
 int   i;
 char *icon_path;

  for (i=0; i<6; i++)
    {
      if (Imgs[i] != NULL) mb_pixbuf_img_free(pb, Imgs[i]);
      icon_path = mb_dot_desktop_icon_get_full_path (ThemeName, 
						     32, 
						     ImgLookup[i]);
      
      if (icon_path == NULL 
	  || !(Imgs[i] = mb_pixbuf_img_new_from_file(pb, icon_path)))
	{
	  fprintf(stderr, "mb-applet-wireless: failed to load icon\n" );
	  exit(1);
	}

      free(icon_path);
    }
}

void
resize_callback (MBTrayApp *app, int w, int h )
{
  int  i;
  int  base_width  = mb_pixbuf_img_get_width(Imgs[0]);
  int  base_height = mb_pixbuf_img_get_height(Imgs[0]);
  int  scale_width = base_width, scale_height = base_height;
  Bool want_resize = True;

  if (mb_tray_app_tray_is_vertical(app) && w < base_width)
    {

      scale_width = w;
      scale_height = ( base_height * w ) / base_width;

      want_resize = False;
    }
  else if (!mb_tray_app_tray_is_vertical(app) && h < base_height)
    {
      scale_height = h;
      scale_width = ( base_width * h ) / base_height;
      want_resize = False;
    }

  if (w < base_width && h < base_height
      && ( scale_height > h || scale_width > w))
    {
       /* Something is really wrong to get here  */
      scale_height = h; scale_width = w;
      want_resize = False;
    }

  if (want_resize)  /* we only request a resize is absolutely needed */
    {
      LastImg = -1;
      mb_tray_app_request_size (app, scale_width, scale_height);
    }

  for (i=0; i<6; i++)
    {
      if (ImgsScaled[i] != NULL) 
	mb_pixbuf_img_free(pb, ImgsScaled[i]);

      ImgsScaled[i] = mb_pixbuf_img_scale(pb, 
					  Imgs[i], 
					  scale_width, 
					  scale_height);
    }
}

void
button_callback (MBTrayApp *app, int x, int y, Bool is_released )
{
  char tray_msg[320];		/* + the Address line */
  char quality[10];
  char level[12];		/* "-100dBm" + NUL */
  char noise[12];
  const char *addr;

  update_wireless();
  
  if (Mwd.quality != -1)
    snprintf (quality, 10, "%u%%", Mwd.quality);
  else
  	strncpy (quality, "Unknown", 10);
  /* %d, not %u: these are dBm and therefore negative. */
  if (Mwd.level != -1)
    snprintf (level, sizeof(level), "%ddBm", Mwd.level);
  else
  	strncpy (level, "Unknown", sizeof(level));
  if (Mwd.noise != -1)
    snprintf (noise, sizeof(noise), "%ddBm", Mwd.noise);
  else
  	strncpy (noise, "Unknown", sizeof(noise));

  if (!is_released) return;

  addr = iface_address(Mwd.iface);

  snprintf(tray_msg, sizeof(tray_msg),
	  "%s:\n"
	  "  Mode: %s\n"
	  "  ESSID: %s\n"
	  "  Address: %s\n"
	  "  Quality: %s\n"
	  "  Level: %s\n"
	  "  Noise: %s\n",
	  Mwd.iface,
	  Mwd.mode ? Mwd.mode : "Unknown",
	  Mwd.essid ? Mwd.essid  : "Unknown",
	  addr ? addr : "None",
	  quality,
	  level,
	  noise );

  mb_tray_app_tray_send_message(app, tray_msg, 5000);
}

void 
theme_callback (MBTrayApp *app, char *theme_name)
{
  if (!theme_name) return;
  if (ThemeName) free(ThemeName);

  LastImg = -1; 			/* Make sure paint gets updated */

  ThemeName = strdup(theme_name);

  load_icons(app);

  resize_callback (app, mb_tray_app_width(app), mb_tray_app_width(app) );
}

void
timeout_callback ( MBTrayApp *app )
{
  mb_tray_app_repaint (app);
}

int  				/* repeatadly call via enum_devices */
find_iwface(int Wfd, char *ifname, char *args[], int count)
{

  /* is it a wireless if */
 if (iw_get_basic_config(Wfd, ifname, &WInfo.b) < 0)
   return 0;

 /* dont stop check interfaces till we find one that supports stats
  * works round odd issues on Z with host AP.
 */
 if (Mwd.iface != NULL && WInfo.has_stats == 1)
   return 0;

 /* These probe the interface currently being enumerated, so they must use
  * ifname. They passed Mwd.iface, which is only assigned at the bottom of
  * this function -- so on the FIRST wireless interface it was still NULL,
  * and iwlib's iw_get_ext() does strncpy(ifr_name, ifname, IFNAMSIZ) on
  * it. The applet segfaulted during startup enumeration on any machine
  * that actually had a wireless interface, which is presumably why it was
  * never shipped. */
 if(iw_get_range_info(Wfd, ifname, &(WInfo.range)) >= 0)
   WInfo.has_range = 1;

 if (iw_get_stats(Wfd, ifname,
		  &(WInfo.stats),
		  &(WInfo.range), WInfo.has_range) >= 0)
   WInfo.has_stats = 1;

 /* mark first found as one to monitor */
 if (Mwd.iface)
   free(Mwd.iface);

 Mwd.iface = strdup(ifname);
 
 return 0;
}


int
main( int argc, char *argv[])
{
  MBTrayApp *app = NULL;
  struct timeval tv;

#if ENABLE_NLS
  setlocale (LC_ALL, "");
  bindtextdomain (PACKAGE, DATADIR "/locale");
  bind_textdomain_codeset (PACKAGE, "UTF-8"); 
  textdomain (PACKAGE);
#endif

  memset(&WInfo, 0, sizeof(struct wireless_info));
  Wfd = iw_sockets_open();

  if (Wfd != -1)
    iw_enum_devices(Wfd, find_iwface, NULL, 0);

  app = mb_tray_app_new ( _("Wireless Monitor"),
			  resize_callback,
			  paint_callback,
			  &argc,
			  &argv );  
  
   pb = mb_pixbuf_new(mb_tray_app_xdisplay(app), 
		      mb_tray_app_xscreen(app));
   
   memset(&tv,0,sizeof(struct timeval));

   tv.tv_sec = 2;

   load_icons(app);

   mb_tray_app_set_timeout_callback (app, timeout_callback, &tv); 
   
   mb_tray_app_set_button_callback (app, button_callback );
   
   mb_tray_app_set_theme_change_callback (app, theme_callback );

   mb_tray_app_set_icon(app, pb, Imgs[3]);
   
   mb_tray_app_main (app);

   if (Mwd.iface != NULL)
      free(Mwd.iface);

   return 1;
}



