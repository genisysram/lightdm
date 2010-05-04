/*
 * Copyright (C) 2010 Robert Ancell.
 * Author: Robert Ancell <robert.ancell@canonical.com>
 * 
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See http://www.gnu.org/copyleft/gpl.html the full text of the
 * license.
 */

#include <dbus/dbus-glib.h>
#include <security/pam_appl.h>

#include "greeter.h"

enum {
    SHOW_PROMPT,
    SHOW_MESSAGE,
    SHOW_ERROR,
    AUTHENTICATION_COMPLETE,
    TIMED_LOGIN,
    LAST_SIGNAL
};
static guint signals[LAST_SIGNAL] = { 0 };

struct GreeterPrivate
{
    DBusGConnection *bus;

    DBusGProxy *display_proxy, *session_proxy, *user_proxy;

    gboolean have_users;
    GList *users;

    gboolean have_sessions;
    GList *sessions;
    gchar *session;

    gboolean is_authenticated;

    gchar *timed_user;
    gint login_delay;
    guint login_timeout;
};

G_DEFINE_TYPE (Greeter, greeter, G_TYPE_OBJECT);

Greeter *
greeter_new (/*int argc, char **argv*/)
{
    /*if (argc != 2)
    {
        g_warning ("Incorrect arguments provided to Greeter");
        return NULL;
    }*/

    return g_object_new (GREETER_TYPE, /*"?", argv[1],*/ NULL);
}

static gboolean
timed_login_cb (gpointer data)
{
    Greeter *greeter = data;

    g_signal_emit (G_OBJECT (greeter), signals[TIMED_LOGIN], 0, greeter->priv->timed_user);

    return TRUE;
}

gboolean
greeter_connect (Greeter *greeter)
{
    gboolean result;
    GError *error = NULL;

    result = dbus_g_proxy_call (greeter->priv->display_proxy, "Connect", &error,
                                G_TYPE_INVALID,
                                G_TYPE_STRING, &greeter->priv->session,                                
                                G_TYPE_STRING, &greeter->priv->timed_user,
                                G_TYPE_INT, &greeter->priv->login_delay,
                                G_TYPE_INVALID);

    if (!result)
        g_warning ("Failed to connect to display manager: %s", error->message);
    g_clear_error (&error);
    if (!result)
        return FALSE;

    /* Set timeout for default login */
    if (greeter->priv->timed_user[0] != '\0' && greeter->priv->login_delay > 0)
    {
        g_debug ("Logging in as %s in %d seconds", greeter->priv->timed_user, greeter->priv->login_delay);
        greeter->priv->login_timeout = g_timeout_add (greeter->priv->login_delay * 1000, timed_login_cb, greeter);
    }

    return result;
}

#define TYPE_USER dbus_g_type_get_struct ("GValueArray", G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN, G_TYPE_INVALID)
#define TYPE_USER_LIST dbus_g_type_get_collection ("GPtrArray", TYPE_USER)

static void
update_users (Greeter *greeter)
{
    GPtrArray *users;
    gboolean result;
    gint i;
    GError *error = NULL;

    if (greeter->priv->have_users)
        return;

    result = dbus_g_proxy_call (greeter->priv->user_proxy, "GetUsers", &error,
                                G_TYPE_INVALID,
                                TYPE_USER_LIST, &users,
                                G_TYPE_INVALID);
    if (!result)
        g_warning ("Failed to get users: %s", error->message);
    g_clear_error (&error);
  
    if (!result)
        return;
  
    for (i = 0; i < users->len; i++)
    {
        GValue value = { 0 };
        UserInfo *info;

        info = g_malloc0 (sizeof (UserInfo));

        g_value_init (&value, TYPE_USER);
        g_value_set_static_boxed (&value, users->pdata[i]);
        dbus_g_type_struct_get (&value, 0, &info->name, 1, &info->real_name, 2, &info->image, 3, &info->logged_in, G_MAXUINT);

        g_value_unset (&value);

        greeter->priv->users = g_list_append (greeter->priv->users, info);
    }

    g_ptr_array_free (users, TRUE);

    greeter->priv->have_users = TRUE;
}

gint
greeter_get_num_users (Greeter *greeter)
{
    update_users (greeter);
    return g_list_length (greeter->priv->users);
}

const GList *
greeter_get_users (Greeter *greeter)
{
    update_users (greeter);
    return greeter->priv->users;
}

#define TYPE_SESSION dbus_g_type_get_struct ("GValueArray", G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INVALID)
#define TYPE_SESSION_LIST dbus_g_type_get_collection ("GPtrArray", TYPE_SESSION)

static void
update_sessions (Greeter *greeter)
{
    GPtrArray *sessions;
    gboolean result;
    gint i;
    GError *error = NULL;

    if (greeter->priv->have_sessions)
        return;

    result = dbus_g_proxy_call (greeter->priv->session_proxy, "GetSessions", &error,
                                G_TYPE_INVALID,
                                TYPE_SESSION_LIST, &sessions,
                                G_TYPE_INVALID);
    if (!result)
        g_warning ("Failed to get sessions: %s", error->message);
    g_clear_error (&error);
  
    if (!result)
        return;
  
    for (i = 0; i < sessions->len; i++)
    {
        GValue value = { 0 };
        Session *session;
      
        session = g_malloc0 (sizeof (Session));
      
        g_value_init (&value, TYPE_SESSION);
        g_value_set_static_boxed (&value, sessions->pdata[i]);
        dbus_g_type_struct_get (&value, 0, &session->key, 1, &session->name, 2, &session->comment, G_MAXUINT);

        g_value_unset (&value);

        greeter->priv->sessions = g_list_append (greeter->priv->sessions, session);
    }

    g_ptr_array_free (sessions, TRUE);

    greeter->priv->have_sessions = TRUE;
}

const GList *
greeter_get_sessions (Greeter *greeter)
{
    update_sessions (greeter);
    return greeter->priv->sessions;
}

void
greeter_set_session (Greeter *greeter, const gchar *session)
{
    GError *error = NULL;

    if (!dbus_g_proxy_call (greeter->priv->display_proxy, "SetSession", &error,
                            G_TYPE_STRING, session,
                            G_TYPE_INVALID,
                            G_TYPE_INVALID))
        g_warning ("Failed to set session: %s", error->message);
    else
    {
        g_free (greeter->priv->session);
        greeter->priv->session = g_strdup (session);
    }
    g_clear_error (&error);
}

const gchar *
greeter_get_session (Greeter *greeter)
{
    return greeter->priv->session;
}

gchar *
greeter_get_timed_login_user (Greeter *greeter)
{
    return greeter->priv->timed_user;
}

gint
greeter_get_timed_login_delay (Greeter *greeter)
{
    return greeter->priv->login_delay;
}

void
greeter_cancel_timed_login (Greeter *greeter)
{
    if (greeter->priv->login_timeout)
       g_source_remove (greeter->priv->login_timeout);
    greeter->priv->login_timeout = 0;
}

#define TYPE_MESSAGE dbus_g_type_get_struct ("GValueArray", G_TYPE_INT, G_TYPE_STRING, G_TYPE_INVALID)
#define TYPE_MESSAGE_LIST dbus_g_type_get_collection ("GPtrArray", TYPE_MESSAGE)

static void
auth_response_cb (DBusGProxy *proxy, DBusGProxyCall *call, gpointer userdata)
{
    Greeter *greeter = userdata;
    gboolean result;
    GError *error = NULL;
    gint return_code;
    GPtrArray *array;
    int i;

    result = dbus_g_proxy_end_call (proxy, call, &error, G_TYPE_INT, &return_code, TYPE_MESSAGE_LIST, &array, G_TYPE_INVALID);
    if (!result)
        g_warning ("Failed to complete D-Bus call: %s", error->message);
    g_clear_error (&error);
    if (!result)
        return;

    for (i = 0; i < array->len; i++)
    {
        GValue value = { 0 };
        gint msg_style;
        gchar *msg;
      
        g_value_init (&value, TYPE_MESSAGE);
        g_value_set_static_boxed (&value, array->pdata[i]);
        dbus_g_type_struct_get (&value, 0, &msg_style, 1, &msg, G_MAXUINT);

        // FIXME: Should stop on prompts?
        switch (msg_style)
        {
        case PAM_PROMPT_ECHO_OFF:
        case PAM_PROMPT_ECHO_ON:
            g_signal_emit (G_OBJECT (greeter), signals[SHOW_PROMPT], 0, msg);
            break;
        case PAM_ERROR_MSG:
            g_signal_emit (G_OBJECT (greeter), signals[SHOW_ERROR], 0, msg);
            break;
        case PAM_TEXT_INFO:
            g_signal_emit (G_OBJECT (greeter), signals[SHOW_MESSAGE], 0, msg);
            break;
        }

        g_free (msg);

        g_value_unset (&value);
    }

    if (array->len == 0)
    {
        greeter->priv->is_authenticated = (return_code == 0);
        g_signal_emit (G_OBJECT (greeter), signals[AUTHENTICATION_COMPLETE], 0);
    }

    g_ptr_array_unref (array);
}

void
greeter_start_authentication (Greeter *greeter, const char *username)
{
    dbus_g_proxy_begin_call (greeter->priv->display_proxy, "StartAuthentication", auth_response_cb, greeter, NULL, G_TYPE_STRING, username, G_TYPE_INVALID);
}

void
greeter_provide_secret (Greeter *greeter, const gchar *secret)
{
    gchar **secrets;

    // FIXME: Could be multiple secrets required
    secrets = g_malloc (sizeof (char *) * 2);
    secrets[0] = g_strdup (secret);
    secrets[1] = NULL;
    dbus_g_proxy_begin_call (greeter->priv->display_proxy, "ContinueAuthentication", auth_response_cb, greeter, NULL, G_TYPE_STRV, secrets, G_TYPE_INVALID);
}

void
greeter_cancel_authentication (Greeter *greeter)
{
}

gboolean
greeter_get_is_authenticated (Greeter *greeter)
{
    return greeter->priv->is_authenticated;
}

void
greeter_suspend (Greeter *greeter)
{
    // FIXME
}

void
greeter_hibernate (Greeter *greeter)
{
    // FIXME
}

void
greeter_restart (Greeter *greeter)
{
    // FIXME
}

void
greeter_shutdown (Greeter *greeter)
{
    // FIXME
}

static void
greeter_init (Greeter *greeter)
{
    GError *error = NULL;

    greeter->priv = G_TYPE_INSTANCE_GET_PRIVATE (greeter, GREETER_TYPE, GreeterPrivate);
  
    greeter->priv->bus = dbus_g_bus_get (DBUS_BUS_SYSTEM, &error);
    if (!greeter->priv->bus)
        g_error ("Failed to connect to bus: %s", error->message);
    g_clear_error (&error);

    greeter->priv->display_proxy = dbus_g_proxy_new_for_name (greeter->priv->bus,
                                                              "org.gnome.LightDisplayManager",
                                                              "/org/gnome/LightDisplayManager/Display0",
                                                              "org.gnome.LightDisplayManager.Greeter");
    greeter->priv->session_proxy = dbus_g_proxy_new_for_name (greeter->priv->bus,
                                                              "org.gnome.LightDisplayManager",
                                                              "/org/gnome/LightDisplayManager/Session",
                                                              "org.gnome.LightDisplayManager.Session");
    greeter->priv->user_proxy = dbus_g_proxy_new_for_name (greeter->priv->bus,
                                                           "org.gnome.LightDisplayManager",
                                                           "/org/gnome/LightDisplayManager/Users",
                                                           "org.gnome.LightDisplayManager.Users");
}

static void
greeter_class_init (GreeterClass *klass)
{
    g_type_class_add_private (klass, sizeof (GreeterPrivate));

    signals[SHOW_PROMPT] =
        g_signal_new ("show-prompt",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET (GreeterClass, show_prompt),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__STRING,
                      G_TYPE_NONE, 1, G_TYPE_STRING);
    signals[SHOW_MESSAGE] =
        g_signal_new ("show-message",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET (GreeterClass, show_message),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__STRING,
                      G_TYPE_NONE, 1, G_TYPE_STRING);
    signals[SHOW_ERROR] =
        g_signal_new ("show-error",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET (GreeterClass, show_error),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__STRING,
                      G_TYPE_NONE, 1, G_TYPE_STRING);
    signals[AUTHENTICATION_COMPLETE] =
        g_signal_new ("authentication-complete",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET (GreeterClass, authentication_complete),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__VOID,
                      G_TYPE_NONE, 0);
    signals[TIMED_LOGIN] =
        g_signal_new ("timed-login",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET (GreeterClass, timed_login),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__STRING,
                      G_TYPE_NONE, 1, G_TYPE_STRING);
}
