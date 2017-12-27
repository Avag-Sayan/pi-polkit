/*
 *      lx-polkit-listener.c
 *
 *      Copyright 2010 PCMan <pcman.tw@gmail.com>
 *
 *      This program is free software; you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License as published by
 *      the Free Software Foundation; either version 2 of the License, or
 *      (at your option) any later version.
 *
 *      This program is distributed in the hope that it will be useful,
 *      but WITHOUT ANY WARRANTY; without even the implied warranty of
 *      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *      GNU General Public License for more details.
 *
 *      You should have received a copy of the GNU General Public License
 *      along with this program; if not, write to the Free Software
 *      Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *      MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "lxpolkit-listener.h"
#include <gtk/gtk.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <string.h>

#include <sys/types.h>
#include <pwd.h>
#include <grp.h>

#ifdef G_ENABLE_DEBUG
#define DEBUG(...)  g_debug(__VA_ARGS__)
#else
#define DEBUG(...)
#endif

static void lxpolkit_listener_finalize  			(GObject *object);

G_DEFINE_TYPE(LXPolkitListener, lxpolkit_listener, POLKIT_AGENT_TYPE_LISTENER);

typedef struct _DlgData DlgData;
struct _DlgData
{
    LXPolkitListener* listener;
    GSimpleAsyncResult* result;
    GtkWidget* dlg;
    GtkWidget* id;
    GtkWidget* request;
    GtkWidget* request_label;
    GtkWidget* auth_button;
    GtkWidget* auth_spin;
    GtkWidget* info_box;
    GCancellable* cancellable;
    GAsyncReadyCallback callback;
    gpointer user_data;
    char* cookie;
    char* action_id;
    PolkitAgentSession* session;
};

/* defined in lxpolkit.c */
void show_msg(GtkWindow* parent, GtkMessageType type, const char* msg);
void show_info(const gchar *msg, GtkMessageType type, DlgData* data);

static void on_cancelled(GCancellable* cancellable, DlgData* data);
static inline void dlg_data_free(DlgData* data);
static void on_user_changed(GtkComboBox* id_combo, DlgData* data);

static void auth_clicked(GtkButton * button, GtkWidget *info, DlgData *data);
static void cancel_clicked(GtkButton * button, GtkWidget *info, DlgData *data);
static GdkPixbuf * get_background_pixbuf(void);
gboolean draw(GtkWidget * widget, cairo_t * cr, GdkPixbuf * pixbuf);

static GApplication *polapp;

inline void dlg_data_free(DlgData* data)
{
    DEBUG("dlg_data_free");
    gtk_widget_hide(data->dlg);
    gtk_widget_destroy(data->dlg);

    g_signal_handlers_disconnect_by_func(data->cancellable, on_cancelled, data);
    g_object_unref(data->cancellable);
    g_object_unref(data->session);
    g_object_unref(data->result);
    g_free(data->action_id);
    g_free(data->cookie);
    g_slice_free(DlgData, data);
}

static void on_completed(PolkitAgentSession* session, gboolean authorized, DlgData* data) {
    DEBUG("on_complete");
    gtk_widget_set_sensitive(data->dlg, TRUE);

    if(!authorized && !g_cancellable_is_cancelled(data->cancellable)) {
        gtk_spinner_stop(GTK_SPINNER (data->auth_spin));
        gtk_widget_hide(data->auth_spin);
        gtk_widget_set_sensitive(data->auth_button, TRUE);
        GNotification *donenoti = g_notification_new ("Wrong Password");
        GIcon *doneicon = g_themed_icon_new ("dialog-password-symbolic");
        g_notification_set_icon (donenoti, doneicon);
        g_application_send_notification (polapp, NULL, donenoti);
        //show_msg(GTK_WINDOW (data->dlg), GTK_MESSAGE_ERROR, _("Authentication failed! Wrong password?"));
        show_info(_("Authentication failed! Wrong password?"), GTK_MESSAGE_ERROR, data);
        /* initiate a new session */
        g_object_unref(data->session);
        data->session = NULL;
        gtk_entry_set_text(GTK_ENTRY (data->request), "");
        gtk_widget_grab_focus(data->request);
        on_user_changed(GTK_COMBO_BOX (data->id), data);
        return;
    } else {
        GNotification *donenoti = g_notification_new ("Authenticated");
        GIcon *doneicon = g_themed_icon_new ("dialog-password-symbolic");
        g_notification_set_icon (donenoti, doneicon);
        g_application_send_notification (polapp, NULL, donenoti);
    }
    g_simple_async_result_complete(data->result);
    dlg_data_free(data);
}

static void on_request(PolkitAgentSession* session, gchar* request, gboolean echo_on, DlgData* data) {
    const char* msg;
    DEBUG("on_request: %s", request);
    if(strcmp("Password: ", request) == 0)
        msg = _("Password: ");
    else
        msg = request;
    gtk_label_set_text(GTK_LABEL (data->request_label), msg);
    gtk_entry_set_visibility(GTK_ENTRY (data->request), echo_on);
}

static void on_show_error(PolkitAgentSession* session, gchar* text, DlgData* data) {
    DEBUG("on error: %s", text);
    GtkWidget *dialog;
    dialog = gtk_message_dialog_new (GTK_WINDOW (data->dlg),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_ERROR,
        GTK_BUTTONS_OK_CANCEL,
        text);
    gtk_dialog_run (GTK_DIALOG (dialog));
    gtk_widget_destroy (dialog);
}

static void on_show_info(PolkitAgentSession* session, gchar* text, DlgData* data) {
    DEBUG("on info: %s", text);
    GtkWidget *dialog;
    dialog = gtk_message_dialog_new (GTK_WINDOW (data->dlg),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_INFO,
        GTK_BUTTONS_OK_CANCEL,
        text);
    gtk_dialog_run (GTK_DIALOG (dialog));
    gtk_widget_destroy (dialog);
}

void show_info(const gchar *msg, GtkMessageType type, DlgData* data) {
    GtkWidget *info = gtk_info_bar_new();
    gtk_info_bar_set_message_type (GTK_INFO_BAR (info), type);
    gtk_container_add(GTK_CONTAINER (gtk_info_bar_get_content_area(GTK_INFO_BAR (info))), gtk_label_new(msg));
    gtk_container_add(GTK_CONTAINER (data->info_box), info);
    gtk_widget_show_all(data->info_box);
}

void on_cancelled(GCancellable* cancellable, DlgData* data)
{
    DEBUG("on_cancelled");
    if(data->session)
        polkit_agent_session_cancel(data->session);
    else
        dlg_data_free(data);
}

/* A different user is selected. */
static void on_user_changed(GtkComboBox* id_combo, DlgData* data) {
    GtkTreeIter it;
    GtkTreeModel* model = gtk_combo_box_get_model(id_combo);
    DEBUG("on_user_changed");
    if(gtk_combo_box_get_active_iter(id_combo, &it)) {
        PolkitIdentity* id;
        gtk_tree_model_get(model, &it, 1, &id, -1);
        if(data->session) /* delete old session object */
        {
            /* prevent receiving completed signal. */
            g_signal_handlers_disconnect_matched(data->session, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, data);
            polkit_agent_session_cancel(data->session);
            g_object_unref(data->session);
        }
        /* create authentication session for currently selected user */
        data->session = polkit_agent_session_new(id, data->cookie);
        g_object_unref(id);
        g_signal_connect(data->session, "completed", G_CALLBACK(on_completed), data);
        g_signal_connect(data->session, "request", G_CALLBACK(on_request), data);
        g_signal_connect(data->session, "show-error", G_CALLBACK(on_show_error), data);
        g_signal_connect(data->session, "show-info", G_CALLBACK(on_show_info), data);
        polkit_agent_session_initiate(data->session);
    }
}

static void initiate_authentication(PolkitAgentListener  *listener,
                                    const gchar          *action_id,
                                    const gchar          *message,
                                    const gchar          *icon_name,
                                    PolkitDetails        *details,
                                    const gchar          *cookie,
                                    GList                *identities,
                                    GCancellable         *cancellable,
                                    GAsyncReadyCallback   callback,
                                    gpointer              user_data)
{
    g_print("Application is requesting authentication: \r\n");
    g_print(message);
    g_print("\r\n With the icon: \r\n");
    g_print(icon_name);
    g_print("\r\n And ActionID \r\n");
    g_print(action_id);
    GtkWidget *icon, *msg;
    GList* l;
    DlgData* data = g_slice_new0(DlgData);
    DEBUG("init_authentication");
    DEBUG("action_id = %s", action_id);
#ifdef G_ENABLE_DEBUG
    char** p;
    for(p = polkit_details_get_keys(details);*p;++p)
        g_print("%s: %s", *p, polkit_details_lookup(details, *p));
#endif
    data->listener = (LXPolkitListener*)listener;
    
    data->result = g_simple_async_result_new(G_OBJECT (listener), callback, user_data, initiate_authentication);

    data->action_id = g_strdup(action_id);
    data->cancellable = (GCancellable*)g_object_ref(cancellable);
    data->callback = callback;
    data->user_data = user_data;
    data->cookie = g_strdup(cookie);
    data->auth_button = (GtkWidget*)gtk_button_new();
    data->auth_spin = (GtkWidget*)gtk_spinner_new ();
    data->dlg = (GtkWidget*)gtk_window_new(GTK_WINDOW_TOPLEVEL);
    icon = (GtkWidget*)gtk_image_new_from_icon_name("dialog-password-symbolic", 0);
    data->id = (GtkWidget*)gtk_combo_box_new ();
    data->request = (GtkWidget*)gtk_entry_new ();
    data->request_label = (GtkWidget*)gtk_label_new("Password:");
    data->info_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);

    /* set dialog icon */
    if(icon_name && *icon_name)
        gtk_image_set_from_icon_name(GTK_IMAGE(icon), icon_name, GTK_ICON_SIZE_DIALOG);
    else
        gtk_image_set_from_icon_name(GTK_IMAGE(icon), "dialog-password-symbolic", GTK_ICON_SIZE_DIALOG);

    /* create combo box for user selection */
    if( identities ) {
        GtkListStore* store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_OBJECT);
        g_signal_connect(data->id, "changed", G_CALLBACK(on_user_changed), data);
        for(l = identities; l; l=l->next) {
            PolkitIdentity* id = (PolkitIdentity*)l->data;
            char* name;
            if(POLKIT_IS_UNIX_USER(id)) {
                struct passwd* pwd = getpwuid(polkit_unix_user_get_uid(POLKIT_UNIX_USER(id)));
                gtk_list_store_insert_with_values(store, NULL, -1, 0, pwd->pw_name, 1, id, -1);
            } else if(POLKIT_IS_UNIX_GROUP(id)) {
                struct group* grp = getgrgid(polkit_unix_group_get_gid(POLKIT_UNIX_GROUP(id)));
                char* str = g_strdup_printf(_("Group: %s"), grp->gr_name);
                gtk_list_store_insert_with_values(store, NULL, -1, 0, str, 1, id, -1);
                g_free(str);
            } else {
                /* FIXME: what's this? */
                
                char* str = polkit_identity_to_string(id);
                gtk_list_store_insert_with_values(store, NULL, -1, 0, str, 1, id, -1);
                g_free(str);
            }
        }
        gtk_combo_box_set_model(GTK_COMBO_BOX (data->id), GTK_TREE_MODEL(store));
        GtkCellRenderer *pwdrend = gtk_cell_renderer_text_new ();
        gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (data->id), pwdrend, TRUE);
        gtk_cell_layout_add_attribute (GTK_CELL_LAYOUT (data->id), pwdrend, "text", 0);

        g_object_unref(store);
        /* select the fist user in the list */
        gtk_combo_box_set_active(GTK_COMBO_BOX (data->id), 0);
    } else {
        GtkWidget *dialog;
        dialog = gtk_message_dialog_new (GTK_WINDOW (data->dlg),
            GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
            GTK_MESSAGE_INFO,
            GTK_BUTTONS_OK,
            "No Users Found");
        gtk_dialog_run (GTK_DIALOG (dialog));
        gtk_widget_destroy (dialog);

        DEBUG("no identities list, is this an error?");
        g_simple_async_result_complete_in_idle(data->result);
        dlg_data_free(data);
        return;
    }
    
    g_object_set (gtk_settings_get_default (), "gtk-dialogs-use-header", TRUE, "gtk-application-prefer-dark-theme", TRUE, NULL);
    
    /* Get the background pixbuf. */
    GdkPixbuf * pixbuf = get_background_pixbuf();

    /* Create the toplevel window. */
    gtk_window_set_decorated(GTK_WINDOW(data->dlg), FALSE);
    gtk_window_fullscreen(GTK_WINDOW(data->dlg));
    gtk_window_set_title(GTK_WINDOW(data->dlg), "Authenticate");
    gtk_window_set_icon_name(GTK_WINDOW(data->dlg), "dialog-password-symbolic");
    GdkScreen* screen = gtk_widget_get_screen(data->dlg);
    gtk_window_set_default_size(GTK_WINDOW(data->dlg), gdk_screen_get_width(screen), gdk_screen_get_height(screen));
    gtk_widget_set_app_paintable(data->dlg, TRUE);
    g_signal_connect(G_OBJECT(data->dlg), "draw", G_CALLBACK(draw), pixbuf);

    /* Toplevel container */
    GtkWidget* alignment = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_halign (alignment, GTK_ALIGN_CENTER);
    gtk_widget_set_valign (alignment, GTK_ALIGN_CENTER);
    gtk_container_add(GTK_CONTAINER(data->dlg), alignment);

    GtkWidget* center_area = gtk_event_box_new();
    gtk_container_add(GTK_CONTAINER(alignment), center_area);

    GtkWidget* center_vbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(center_vbox), 12);
    gtk_container_add(GTK_CONTAINER(center_area), center_vbox);

    GtkWidget* controls = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_halign (controls, GTK_ALIGN_CENTER);
    gtk_widget_set_valign (controls, GTK_ALIGN_CENTER);

    gtk_image_set_pixel_size (GTK_IMAGE (icon), 128);
    
    gtk_box_pack_start(GTK_BOX(center_vbox), icon, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(center_vbox), controls, FALSE, FALSE, 2);

    /* Create the label. */
    GtkWidget * label = gtk_label_new("");
    gtk_label_set_markup(GTK_LABEL(label), g_strdup_printf(_("<b><big>%s</big></b>"), message));
    gtk_box_pack_start(GTK_BOX(controls), label, FALSE, FALSE, 4);
    
    GtkWidget* lcontrols = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_halign (lcontrols, GTK_ALIGN_START);
    gtk_widget_set_valign (lcontrols, GTK_ALIGN_CENTER);
    
    /* User Picker */
    gtk_box_pack_start(GTK_BOX(lcontrols), data->id, FALSE, FALSE, 2);
    
    gtk_widget_set_halign (data->request_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(lcontrols), data->request_label, FALSE, FALSE, 2);
    
    /* Password Box */
    gtk_entry_set_placeholder_text (GTK_ENTRY (data->request), "Password");
    gtk_entry_set_icon_from_icon_name (GTK_ENTRY (data->request), GTK_ENTRY_ICON_SECONDARY, "dialog-password-symbolic");
    gtk_box_pack_start(GTK_BOX(lcontrols), data->request, FALSE, FALSE, 2);
    
    gtk_widget_set_size_request (lcontrols, 400, -1);
    gtk_box_pack_start(GTK_BOX(controls), lcontrols, FALSE, FALSE, 2);
    
    gtk_box_pack_start(GTK_BOX(lcontrols), data->info_box, FALSE, FALSE, 2);
    
    /* Add Buttons */
    GtkWidget *btnbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(controls), btnbox, FALSE, FALSE, 0);
    GtkAccelGroup* accel_group = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(data->dlg), accel_group);

    /* Authenticate */
    GtkWidget * lolabel = gtk_label_new_with_mnemonic (_("_Authenticate"));
	gtk_style_context_add_class(gtk_widget_get_style_context(data->auth_button), "suggested-action");
    GtkWidget *authbtnbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_container_add(GTK_CONTAINER (authbtnbox), lolabel);
    gtk_container_add(GTK_CONTAINER (authbtnbox), data->auth_spin);
    gtk_container_add(GTK_CONTAINER (data->auth_button), authbtnbox);
    g_signal_connect(G_OBJECT(data->auth_button), "clicked", G_CALLBACK(auth_clicked), data);
    gtk_widget_add_accelerator(data->auth_button, "activate", accel_group,
        GDK_KEY_KP_Enter, (GdkModifierType)0, GTK_ACCEL_VISIBLE);
    gtk_box_pack_start(GTK_BOX(btnbox), data->auth_button, FALSE, FALSE, 3);

    /* Create the Cancel button. */
    GtkWidget * cancel_button = gtk_button_new();
    GtkWidget * cnlabel = gtk_label_new_with_mnemonic (_("_Cancel"));
    gtk_container_add(GTK_CONTAINER (cancel_button), cnlabel);
    g_signal_connect(G_OBJECT(cancel_button), "clicked", G_CALLBACK(cancel_clicked), NULL);
    gtk_widget_add_accelerator(cancel_button, "activate", accel_group,
        GDK_KEY_Escape, (GdkModifierType)0, GTK_ACCEL_VISIBLE);
    gtk_box_pack_start(GTK_BOX(btnbox), cancel_button, FALSE, FALSE, 3);


    /* Show everything. */
    gtk_widget_show_all(data->dlg);
    gtk_widget_hide(data->auth_spin);
    gtk_entry_set_input_purpose (GTK_ENTRY (data->request), GTK_INPUT_PURPOSE_PASSWORD);
    gtk_widget_grab_focus (data->request);
}

/* Get the background pixbuf. */
static GdkPixbuf * get_background_pixbuf(void) {
    /* Get the root window pixmap. */
    GdkScreen * screen = gdk_screen_get_default();
    GdkPixbuf * pixbuf = gdk_pixbuf_get_from_window(gdk_get_default_root_window(), 0, 0, gdk_screen_get_width(screen), gdk_screen_get_height(screen));	

    /* Make the background darker. */
    if (pixbuf != NULL) {
        unsigned char * pixels = gdk_pixbuf_get_pixels(pixbuf);
        int width = gdk_pixbuf_get_width(pixbuf);
        int height = gdk_pixbuf_get_height(pixbuf);
        int pixel_stride = ((gdk_pixbuf_get_has_alpha(pixbuf)) ? 4 : 3);
        int row_stride = gdk_pixbuf_get_rowstride(pixbuf);
        int y;
        for (y = 0; y < height; y += 1) {
            unsigned char * p = pixels;
            int x;
            for (x = 0; x < width; x += 1)
            {
                p[0] = p[0] / 2.5;
                p[1] = p[1] / 2.5;
                p[2] = p[2] / 2.5;
                p += pixel_stride;
            }
            pixels += row_stride;
        }
    }
    return pixbuf;
}

/* Handler for "expose_event" on background. */
gboolean draw(GtkWidget * widget, cairo_t * cr, GdkPixbuf * pixbuf) {
    if (pixbuf != NULL) {
        /* Copy the appropriate rectangle of the root window pixmap to the drawing area.
         * All drawing areas are immediate children of the toplevel window, so the allocation yields the source coordinates directly. */
        gdk_cairo_set_source_pixbuf (cr,  pixbuf, 0, 0);
        cairo_paint (cr);
    }
    return FALSE;
}

/* Handler for "clicked" signal on Cancel button. */
static void cancel_clicked(GtkButton * button, GtkWidget *info, DlgData *data) {
    dlg_data_free(data);
    g_cancellable_cancel(data->cancellable);
}

static void auth_clicked(GtkButton * button, GtkWidget *info, DlgData *data) {
    gtk_widget_set_sensitive(data->auth_button, FALSE);
    gtk_widget_show(data->auth_spin);
    gtk_spinner_start (GTK_SPINNER (data->auth_spin));
    const char* request = gtk_entry_get_text(GTK_ENTRY (data->request));
    polkit_agent_session_response(data->session, request);
}

static gboolean initiate_authentication_finish(PolkitAgentListener  *listener,
                                              GAsyncResult         *res,
                                              GError              **error)
{
    LXPolkitListener* self = (LXPolkitListener*)listener;
    DEBUG("init_authentication_finish");
    return !g_simple_async_result_propagate_error(G_SIMPLE_ASYNC_RESULT(res), error);
}

static void lxpolkit_listener_class_init(LXPolkitListenerClass *klass) {
	GObjectClass *g_object_class;
    PolkitAgentListenerClass* pkal_class;
	g_object_class = G_OBJECT_CLASS(klass);
	g_object_class->finalize = lxpolkit_listener_finalize;

    pkal_class = POLKIT_AGENT_LISTENER_CLASS(klass);
    pkal_class->initiate_authentication = initiate_authentication;
    pkal_class->initiate_authentication_finish = initiate_authentication_finish;
}


static void lxpolkit_listener_finalize(GObject *object) {
	LXPolkitListener *self;

	g_return_if_fail(object != NULL);
	g_return_if_fail(IS_LXPOLKIT_LISTENER(object));

	self = LXPOLKIT_LISTENER(object);

	G_OBJECT_CLASS(lxpolkit_listener_parent_class)->finalize(object);
}


static void lxpolkit_listener_init(LXPolkitListener *self) {
    polapp = g_application_new("org.raspberrypi.system.polkit", G_APPLICATION_IS_SERVICE);
    g_application_register (polapp, NULL, NULL);
}


PolkitAgentListener *lxpolkit_listener_new(void) {
	return g_object_new(LXPOLKIT_LISTENER_TYPE, NULL);
}
