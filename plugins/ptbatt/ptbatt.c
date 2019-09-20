/*
Copyright (c) 2018 Raspberry Pi (Trading) Ltd.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the copyright holder nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <errno.h>
#include <locale.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <glib/gi18n.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <asm/ioctl.h>
#ifdef __arm__
#include <zmq.h>
#endif
#include "batt_sys.h"

#include "plugin.h"

//#define TEST_MODE
#ifdef TEST_MODE
#define INTERVAL 500
#else
#define INTERVAL 5000
#endif

#define POLL_TIMEOUT 2000

/* Plug-in global data */

typedef struct {
    GtkWidget *plugin;              /* Back pointer to the widget */
    LXPanel *panel;                 /* Back pointer to panel */
    GtkWidget *tray_icon;           /* Displayed image */
    config_setting_t *settings;     /* Plugin settings */
    battery *batt;
    GdkPixbuf *plug;
    GdkPixbuf *flash;
    guint timer;
#ifdef __arm__
    gboolean pt_batt_avail;
    void *context;
    void *requester;
#endif
} PtBattPlugin;

/* Battery states */

typedef enum
{
    STAT_UNKNOWN = -1,
    STAT_DISCHARGING = 0,
    STAT_CHARGING = 1,
    STAT_EXT_POWER = 2
} status_t;


/* gdk_pixbuf_get_from_surface function from GDK+3 */

static void
convert_alpha (guchar *dest_data,
               int     dest_stride,
               guchar *src_data,
               int     src_stride,
               int     src_x,
               int     src_y,
               int     width,
               int     height)
{
  int x, y;

  src_data += src_stride * src_y + src_x * 4;

  for (y = 0; y < height; y++) {
    guint32 *src = (guint32 *) src_data;

    for (x = 0; x < width; x++) {
      guint alpha = src[x] >> 24;

      if (alpha == 0)
        {
          dest_data[x * 4 + 0] = 0;
          dest_data[x * 4 + 1] = 0;
          dest_data[x * 4 + 2] = 0;
        }
      else
        {
          dest_data[x * 4 + 0] = (((src[x] & 0xff0000) >> 16) * 255 + alpha / 2) / alpha;
          dest_data[x * 4 + 1] = (((src[x] & 0x00ff00) >>  8) * 255 + alpha / 2) / alpha;
          dest_data[x * 4 + 2] = (((src[x] & 0x0000ff) >>  0) * 255 + alpha / 2) / alpha;
        }
      dest_data[x * 4 + 3] = alpha;
    }

    src_data += src_stride;
    dest_data += dest_stride;
  }
}

GdkPixbuf *
gdk_pixbuf_get_from_surface  (cairo_surface_t *surface,
                              gint             src_x,
                              gint             src_y,
                              gint             width,
                              gint             height)
{
  GdkPixbuf *dest = gdk_pixbuf_new (GDK_COLORSPACE_RGB, TRUE, 8, width, height);

  convert_alpha (gdk_pixbuf_get_pixels (dest),
                   gdk_pixbuf_get_rowstride (dest),
                   cairo_image_surface_get_data (surface),
                   cairo_image_surface_get_stride (surface),
                   0, 0,
                   width, height);

  cairo_surface_destroy (surface);
  return dest;
}


/* Initialise measurements and check for a battery */

static int init_measurement (PtBattPlugin *pt)
{
#ifdef TEST_MODE
	return 1;
#endif
#ifdef __arm__
	pt->pt_batt_avail = FALSE;
	if (system ("systemctl status pt-device-manager | grep -wq active") == 0)
	{
		g_message ("pi-top device manager found");
		pt->pt_batt_avail = TRUE;
		pt->context = zmq_ctx_new ();
		if (pt->context)
		{

			pt->requester = zmq_socket (pt->context, ZMQ_SUB);
			if (pt->requester)
			{
				int lingerTime = 0;
				int timeout = 2000;

				if((zmq_setsockopt(pt->requester, ZMQ_SNDTIMEO, &timeout, sizeof(int)) == 0) &&
				(zmq_setsockopt(pt->requester, ZMQ_RCVTIMEO, &timeout, sizeof(int)) == 0) &&
				(zmq_setsockopt(pt->requester, ZMQ_LINGER, &lingerTime, sizeof(int)) == 0))
				{
					if (zmq_connect (pt->requester, "tcp://127.0.0.1:3781") == 0)
					{
					
						if(zmq_setsockopt(pt->requester, ZMQ_SUBSCRIBE, "305", 3) == 0) 
						{

							g_message ("connected to pi-top device manager");
							return 1;
						}
					}
				}
				zmq_close(pt->requester);
			}
		}
		pt->requester = NULL;
		return 0;
	}
#endif

	int val;
	if (config_setting_lookup_int (pt->settings, "BattNum", &val))
		pt->batt = battery_get (val);
	else
		pt->batt = battery_get (0);
	if (pt->batt) return 1;

	return 0;
}


Bool receiveMessage(char *message, PtBattPlugin *pt)
{
 
	Bool returnValue = False;
	zmq_msg_t zmqMessage;

	int result = zmq_msg_init(&zmqMessage);

	if (result == 0)
	{
		result = zmq_msg_recv(&zmqMessage, pt->requester, 0);

		if (result == -1)
		{
			g_warning("Receiving error!");
		}
		else
		{
			size_t messageLength = zmq_msg_size(&zmqMessage);
			void* pMessageData = zmq_msg_data(&zmqMessage);
			
			memcpy(message, pMessageData, messageLength);
			message[messageLength] = '\0';
			returnValue = True;
		}

		zmq_msg_close(&zmqMessage);
	}
	else
	{
    		g_warning("Creating message buffer error!");
	}

	return returnValue;
}


/* Read current capacity, status and time remaining from battery */

static int charge_level (PtBattPlugin *pt, status_t *status, int *tim)
{
#ifdef TEST_MODE
    static int level = 0;
   *tim = 30;
    if (level < 100) level +=5;
    else level = -100;
    if (level < 0)
    {
        *status = STAT_DISCHARGING;
        return (level * -1);
    }
    else if (level == 100) *status = STAT_EXT_POWER;
    else *status = STAT_CHARGING;
    return level;
#endif
    *status = STAT_UNKNOWN;
    *tim = 0;
#ifdef __arm__
    int capacity, state, time, res;
    char buffer[100];

    if (pt->pt_batt_avail)
    {
        if (!pt->requester) return -1;

        res = zmq_recv (pt->requester, buffer, 100, ZMQ_NOBLOCK);
        if (res > 0 && res < 100)
        {
            buffer[res] = 0;
            if (sscanf (buffer, "%d|%d|%d|%d|", &res, &state, &capacity, &time) == 4)
            {
                if (res == 218 && (state == STAT_CHARGING || state == STAT_DISCHARGING || state == STAT_EXT_POWER))
                {
                    // these two lines shouldn't be necessary once EXT_POWER state is implemented in the device manager
                    if (capacity == 100 && time == 0) *status = STAT_EXT_POWER;
                    else
                    *status = (status_t) state;
                    *tim = time;
                }
            }
        }
        zmq_send (pt->requester, "118", 3, ZMQ_NOBLOCK);
        if (*status != STAT_UNKNOWN) return capacity;
        else return -1;
    }
#endif
    battery *b = pt->batt;
    int mins;
    if (b)
    {
        battery_update (b);
        if (battery_is_charging (b))
        {
            if (strcasecmp (b->state, "full") == 0) *status = STAT_EXT_POWER;
            else *status = STAT_CHARGING;
        }
        else *status = STAT_DISCHARGING;
        mins = b->seconds;
        mins /= 60;
        *tim = mins;
        return b->percentage;
    }
    else return -1;
}


/* Draw the icon in relevant colour and fill level */

static void draw_icon (PtBattPlugin *pt, int lev, float r, float g, float b, int powered)
{
    int h, w, f, ic; 

    // calculate dimensions based on icon size
    ic = panel_get_icon_size (pt->panel);
    w = ic < 36 ? 36 : ic;
    h = ((w * 10) / 36) * 2; // force it to be even
    if (h < 18) h = 18;
    if (h >= ic) h = ic - 2;

    // create and clear the drawing surface
    cairo_surface_t *surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create (surface);
    cairo_set_source_rgba (cr, 0, 0, 0, 0);
    cairo_rectangle (cr, 0, 0, w, h);
    cairo_fill (cr);

    // draw base icon on surface
    cairo_set_source_rgb (cr, r, g, b);
    cairo_rectangle (cr, 4, 1, w - 10, 1);
    cairo_rectangle (cr, 3, 2, w - 8, 1);
    cairo_rectangle (cr, 3, h - 3, w - 8, 1);
    cairo_rectangle (cr, 4, h - 2, w - 10, 1);
    cairo_rectangle (cr, 2, 3, 2, h - 6);
    cairo_rectangle (cr, w - 6, 3, 2, h - 6);
    cairo_rectangle (cr, w - 4, (h >> 1) - 3, 2, 6);
    cairo_fill (cr);

    cairo_set_source_rgba (cr, r, g, b, 0.5);
    cairo_rectangle (cr, 3, 1, 1, 1);
    cairo_rectangle (cr, 2, 2, 1, 1);
    cairo_rectangle (cr, 2, h - 3, 1, 1);
    cairo_rectangle (cr, 3, h - 2, 1, 1);
    cairo_rectangle (cr, w - 6, 1, 1, 1);
    cairo_rectangle (cr, w - 5, 2, 1, 1);
    cairo_rectangle (cr, w - 5, h - 3, 1, 1);
    cairo_rectangle (cr, w - 6, h - 2, 1, 1);
    cairo_fill (cr);

    // fill the battery
    if (lev < 0) f = 0;
    else if (lev > 97) f = w - 12;
    else
    {
        f = (w - 12) * lev;
        f /= 97;
        if (f > w - 12) f = w - 12;
    }
    cairo_set_source_rgb (cr, r, g, b);
    cairo_rectangle (cr, 5, 4, f, h - 8);
    cairo_fill (cr);

    // show icons
    if (powered == 1 && pt->flash)
    {
        gdk_cairo_set_source_pixbuf (cr, pt->flash, (w >> 1) - 15, (h >> 1) - 16);
        cairo_paint (cr);
    }
    if (powered == 2 && pt->plug)
    {
        gdk_cairo_set_source_pixbuf (cr, pt->plug, (w >> 1) - 16, (h >> 1) - 16);
        cairo_paint (cr);
    }

    // create a pixbuf from the cairo surface
    GdkPixbuf *pixbuf = gdk_pixbuf_get_from_surface (surface, 0, 0, w, h);

    // copy the pixbuf to the icon resource
    g_object_ref_sink (pt->tray_icon);
    gtk_image_set_from_pixbuf (GTK_IMAGE (pt->tray_icon), pixbuf);

    g_object_unref (pixbuf);
    cairo_destroy (cr);
}

/* Read the current charge state and update the icon accordingly */

static void update_icon (PtBattPlugin *pt)
{
    int capacity, time;
    status_t status;
    float ftime;
    char str[255];

    // read the charge status
    capacity = charge_level (pt, &status, &time);
    if (status == STAT_UNKNOWN) return;
    ftime = time / 60.0;

    // fill the battery symbol and create the tooltip
    if (status == STAT_CHARGING)
    {
        if (time <= 0)
            sprintf (str, _("Charging : %d%%"), capacity);
        else if (time < 90)
            sprintf (str, _("Charging : %d%%\nTime remaining : %d minutes"), capacity, time);
        else
            sprintf (str, _("Charging : %d%%\nTime remaining : %0.1f hours"), capacity, ftime);
        draw_icon (pt, capacity, 0.95, 0.64, 0, 1);
    }
    else if (status == STAT_EXT_POWER)
    {
        sprintf (str, _("Charged : %d%%\nOn external power"), capacity);
        draw_icon (pt, capacity, 0, 0.85, 0, 2);
    }
    else
    {
        if (time <= 0)
            sprintf (str, _("Discharging : %d%%"), capacity);
        else if (time < 90)
            sprintf (str, _("Discharging : %d%%\nTime remaining : %d minutes"), capacity, time);
        else
            sprintf (str, _("Discharging : %d%%\nTime remaining : %0.1f hours"), capacity, ftime);
        if (capacity <= 20) draw_icon (pt, capacity, 1, 0, 0, 0);
        else draw_icon (pt, capacity, 0, 0.85, 0, 0);
    }

    // set the tooltip
    gtk_widget_set_tooltip_text (pt->tray_icon, str);
}

void * run(void *ptx)
{
  usleep(1000000);
  Bool m_continue = True;
  PtBattPlugin *pt = (PtBattPlugin *)ptx;
  
  while (m_continue)
  {
    zmq_pollitem_t pollItems[1];

    pollItems[0].socket = pt->requester;
    pollItems[0].events = ZMQ_POLLIN;
    int result = zmq_poll(pollItems, 1, POLL_TIMEOUT);

    if (result < 0)
    {
      //m_continue = False;
    }
    else if (result == 0)
    {
      // No events detected (timeout)
    }
    else
    {
      char message[200];
       
      if (receiveMessage(message,pt))
      {
         int capacity, state,time, res;
         char buffer[100];
         float ftime;
         char str[255];
         if (sscanf (message, "%d|%d|%d|%d|", &res, &state, &capacity, &time) )
            {
                char debug[200];
                sprintf(debug,"echo =%d=%d=%d=%d= >> /tmp/printBt",res, state, capacity, time);
                system(debug);
                
                if ((state == STAT_CHARGING || state == STAT_DISCHARGING || state == STAT_EXT_POWER))
                {
                    // these two lines shouldn't be necessary once EXT_POWER state is implemented in the device manager
                    if (capacity == 100 && time == 0) state = STAT_EXT_POWER;
                    
                    //*status = (status_t) state;
                    //*tim = time;
                    
                    if (state == STAT_UNKNOWN) continue;
                    ftime = time / 60.0;

                        // fill the battery symbol and create the tooltip
                        if (state == STAT_CHARGING)
                        {
                            if (time <= 0)
                                sprintf (str, _("Charging : %d%%"), capacity);
                            else if (time < 90)
                                sprintf (str, _("Charging : %d%%\nTime remaining : %d minutes"), capacity, time);
                            else
                                sprintf (str, _("Charging : %d%%\nTime remaining : %0.1f hours"), capacity, ftime);
                            draw_icon (pt, capacity, 0.95, 0.64, 0, 1);
                        }
                        else if (state == STAT_EXT_POWER)
                        {
                            sprintf (str, _("Charged : %d%%\nOn external power"), capacity);
                            draw_icon (pt, capacity, 0, 0.85, 0, 2);
                        }
                        else
                        {
                            if (time <= 0)
                                sprintf (str, _("Discharging : %d%%"), capacity);
                            else if (time < 90)
                                sprintf (str, _("Discharging : %d%%\nTime remaining : %d minutes"), capacity, time);
                            else
                                sprintf (str, _("Discharging : %d%%\nTime remaining : %0.1f hours"), capacity, ftime);
                            if (capacity <= 20) draw_icon (pt, capacity, 1, 0, 0, 0);
                            else draw_icon (pt, capacity, 0, 0.85, 0, 0);
                        }

                        // set the tooltip
                        gtk_widget_set_tooltip_text (pt->tray_icon, str);
                                        
                }
            }
         
      }
    }
  }
}

static gboolean timer_event (PtBattPlugin *pt)
{
    update_icon (pt);
    return TRUE;
}

/* Plugin functions */

/* Handler for menu button click */
static gboolean ptbatt_button_press_event (GtkWidget *widget, GdkEventButton *event, LXPanel *panel)
{
    PtBattPlugin *pt = lxpanel_plugin_get_data (widget);

#ifdef ENABLE_NLS
    textdomain ( GETTEXT_PACKAGE );
#endif

    return FALSE;
}

/* Handler for system config changed message from panel */
static void ptbatt_configuration_changed (LXPanel *panel, GtkWidget *p)
{
    PtBattPlugin *pt = lxpanel_plugin_get_data (p);
    update_icon (pt);
}

/* Plugin destructor. */
static void ptbatt_destructor (gpointer user_data)
{
    PtBattPlugin *pt = (PtBattPlugin *) user_data;

    /* Disconnect the timer. */
    g_source_remove (pt->timer);

#ifdef __arm__
    zmq_close (pt->requester);
    zmq_ctx_destroy (pt->context);
#endif

    /* Deallocate memory */
    g_free (pt);
}

/* Plugin constructor. */
static GtkWidget *ptbatt_constructor (LXPanel *panel, config_setting_t *settings)
{
    /* Allocate and initialize plugin context */
    PtBattPlugin *pt = g_new0 (PtBattPlugin, 1);

#ifdef ENABLE_NLS
    setlocale (LC_ALL, "");
    bindtextdomain ( GETTEXT_PACKAGE, PACKAGE_LOCALE_DIR );
    bind_textdomain_codeset ( GETTEXT_PACKAGE, "UTF-8" );
    textdomain ( GETTEXT_PACKAGE );
#endif

    pt->tray_icon = gtk_image_new ();
    gtk_widget_set_visible (pt->tray_icon, TRUE);

    /* Allocate top level widget and set into Plugin widget pointer. */
    pt->panel = panel;
    pt->plugin = gtk_button_new ();
    gtk_button_set_relief (GTK_BUTTON (pt->plugin), GTK_RELIEF_NONE);
    g_signal_connect (pt->plugin, "button-press-event", G_CALLBACK(ptbatt_button_press_event), NULL);
    pt->settings = settings;
    lxpanel_plugin_set_data (pt->plugin, pt, ptbatt_destructor);
    gtk_widget_add_events (pt->plugin, GDK_BUTTON_PRESS_MASK);

    /* Allocate icon as a child of top level */
    gtk_container_add (GTK_CONTAINER(pt->plugin), pt->tray_icon);

    /* Load the symbols */
    pt->plug = gdk_pixbuf_new_from_file ("/usr/share/lxpanel/images/plug.png", NULL);
    pt->flash = gdk_pixbuf_new_from_file ("/usr/share/lxpanel/images/flash.png", NULL);

    /* Show the widget */
    gtk_widget_show_all (pt->plugin);

    /* Initialise measurements and check for a battery */
    if (init_measurement (pt))
    {
        /* Start timed events to monitor status */
        //pt->timer = g_timeout_add (INTERVAL, (GSourceFunc) timer_event, (gpointer) pt);
        pthread_t ntid;
        int err; 
        err = pthread_create(&ntid, NULL, run,(void*)(pt));
        pthread_detach(ntid);
        
        char debug[200];
         sprintf(debug,"echo =%s= >> /tmp/printBt","init_measurement success!");
         system(debug);
    }
    else
    {
        gtk_widget_hide_all (pt->plugin);
        gtk_widget_set_sensitive (pt->plugin, FALSE);
    }

    return pt->plugin;
}

FM_DEFINE_MODULE(lxpanel_gtk, ptbatt)

/* Plugin descriptor. */
LXPanelPluginInit fm_module_init_lxpanel_gtk = {
    .name = N_("Battery (pi-top / laptop)"),
    .description = N_("Monitors battery for pi-top and laptops"),
    .new_instance = ptbatt_constructor,
    .reconfigure = ptbatt_configuration_changed,
    .button_press_event = ptbatt_button_press_event,
    .gettext_package = GETTEXT_PACKAGE
};
