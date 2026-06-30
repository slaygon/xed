/*
 * xed-session.c
 * This file is part of xed
 *
 * Crash-recovery autosave and session restore.
 *
 * The goal is to never lose work to a crash or power loss, even for
 * documents that were never saved (a fresh Ctrl-N buffer). Every few
 * seconds we write:
 *
 *   - a recovery copy of every *modified* buffer to
 *     ~/.local/share/xed/recovery/<id>
 *   - an index of all open documents to ~/.config/xed/session
 *
 * On the next plain launch (xed with no file arguments) the index is read
 * back and the documents are reopened: saved files are loaded from their
 * location, while unsaved/untitled buffers are restored from their recovery
 * copy and marked as modified so the user is prompted to save.
 *
 * The snapshot is self-healing: when a document is saved or closed it stops
 * being modified, so on the next tick its recovery copy is no longer written
 * and the now-orphaned file is removed. Nothing is written on clean shutdown,
 * so the last good snapshot survives both a crash and a normal quit.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <config.h>

#include <glib/gstdio.h>

#include "xed-session.h"
#include "xed-dirs.h"
#include "xed-document.h"
#include "xed-settings.h"
#include "xed-debug.h"

/* How often (in seconds) the open documents are snapshotted. */
#define SESSION_SAVE_INTERVAL 10

/* Object-data key holding the per-document recovery file name. */
#define RECOVERY_ID_KEY "xed-session-recovery-id"

#define SESSION_INFO_GROUP "session"
#define DOCUMENT_GROUP_PREFIX "document-"

static guint      save_timeout_id   = 0;
static GSettings *editor_settings   = NULL;

static GSettings *
get_editor_settings (void)
{
    if (editor_settings == NULL)
    {
        editor_settings = g_settings_new ("org.x.editor.preferences.editor");
    }

    return editor_settings;
}

static gboolean
session_enabled (void)
{
    return g_settings_get_boolean (get_editor_settings (), XED_SETTINGS_RESTORE_SESSION);
}

static gchar *
get_recovery_dir (void)
{
    return g_build_filename (xed_dirs_get_user_data_dir (), "recovery", NULL);
}

static gchar *
get_session_file (void)
{
    return g_build_filename (xed_dirs_get_user_config_dir (), "session", NULL);
}

/* Returns the recovery file name for doc, allocating a fresh one the first
 * time it is needed. The id is stored on the document so the same recovery
 * file is reused (overwritten) across snapshots.
 */
static const gchar *
ensure_recovery_id (XedDocument *doc)
{
    const gchar *id;

    id = g_object_get_data (G_OBJECT (doc), RECOVERY_ID_KEY);
    if (id == NULL)
    {
        gchar *new_id = g_uuid_string_random ();
        g_object_set_data_full (G_OBJECT (doc), RECOVERY_ID_KEY, new_id, g_free);
        id = new_id;
    }

    return id;
}

static gboolean
write_recovery_file (XedDocument *doc,
                     const gchar *recovery_dir,
                     const gchar *id)
{
    GtkTextIter start, end;
    gchar *text;
    gchar *path;
    gboolean ok;
    GError *error = NULL;

    gtk_text_buffer_get_bounds (GTK_TEXT_BUFFER (doc), &start, &end);
    text = gtk_text_buffer_get_text (GTK_TEXT_BUFFER (doc), &start, &end, TRUE);

    path = g_build_filename (recovery_dir, id, NULL);

    /* g_file_set_contents writes to a temporary file and renames it into
     * place, so a crash mid-write cannot corrupt an existing recovery copy.
     */
    ok = g_file_set_contents (path, text, -1, &error);
    if (!ok)
    {
        g_warning ("xed-session: could not write recovery file '%s': %s", path, error->message);
        g_clear_error (&error);
    }

    g_free (path);
    g_free (text);

    return ok;
}

/* Remove recovery files that are no longer referenced by any open, modified
 * document (saved or closed since the previous snapshot).
 */
static void
cleanup_recovery_dir (const gchar *recovery_dir,
                      GHashTable  *active_ids)
{
    GDir *dir;
    const gchar *name;

    dir = g_dir_open (recovery_dir, 0, NULL);
    if (dir == NULL)
    {
        return;
    }

    while ((name = g_dir_read_name (dir)) != NULL)
    {
        if (!g_hash_table_contains (active_ids, name))
        {
            gchar *path = g_build_filename (recovery_dir, name, NULL);
            g_unlink (path);
            g_free (path);
        }
    }

    g_dir_close (dir);
}

void
_xed_session_save (XedApp *app)
{
    GList *windows, *w;
    GKeyFile *key_file;
    GHashTable *active_ids;
    gchar *recovery_dir;
    gchar *session_file;
    gchar *config_dir;
    gint index = 0;
    GError *error = NULL;

    g_return_if_fail (XED_IS_APP (app));

    if (!session_enabled ())
    {
        return;
    }

    xed_debug (DEBUG_APP);

    recovery_dir = get_recovery_dir ();
    g_mkdir_with_parents (recovery_dir, 0700);

    key_file = g_key_file_new ();
    active_ids = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

    windows = xed_app_get_main_windows (app);
    for (w = windows; w != NULL; w = w->next)
    {
        XedWindow *window = XED_WINDOW (w->data);
        GList *docs, *d;

        docs = xed_window_get_documents (window);
        for (d = docs; d != NULL; d = d->next)
        {
            XedDocument *doc = XED_DOCUMENT (d->data);
            GFile *location;
            gboolean modified;
            gchar *group;
            gchar *name;

            /* A pristine, never-touched empty buffer is not worth restoring. */
            if (xed_document_is_untouched (doc))
            {
                continue;
            }

            modified = gtk_text_buffer_get_modified (GTK_TEXT_BUFFER (doc));
            location = xed_document_get_location (doc);

            /* Nothing to remember: no file on disk and no unsaved content. */
            if (location == NULL && !modified)
            {
                continue;
            }

            group = g_strdup_printf (DOCUMENT_GROUP_PREFIX "%d", index++);

            if (location != NULL)
            {
                gchar *uri = g_file_get_uri (location);
                g_key_file_set_string (key_file, group, "uri", uri);
                g_free (uri);
            }

            if (modified)
            {
                const gchar *id = ensure_recovery_id (doc);

                if (write_recovery_file (doc, recovery_dir, id))
                {
                    g_key_file_set_string (key_file, group, "recovery", id);
                    g_hash_table_add (active_ids, g_strdup (id));
                }
            }

            name = xed_document_get_short_name_for_display (doc);
            if (name != NULL)
            {
                g_key_file_set_string (key_file, group, "name", name);
                g_free (name);
            }

            g_free (group);

            if (location != NULL)
            {
                g_object_unref (location);
            }
        }

        g_list_free (docs);
    }
    g_list_free (windows);

    g_key_file_set_integer (key_file, SESSION_INFO_GROUP, "count", index);

    session_file = get_session_file ();
    config_dir = g_path_get_dirname (session_file);
    g_mkdir_with_parents (config_dir, 0700);
    g_free (config_dir);

    if (!g_key_file_save_to_file (key_file, session_file, &error))
    {
        g_warning ("xed-session: could not save session '%s': %s", session_file, error->message);
        g_clear_error (&error);
    }

    /* Drop recovery files belonging to documents that were saved or closed. */
    cleanup_recovery_dir (recovery_dir, active_ids);

    g_hash_table_destroy (active_ids);
    g_key_file_free (key_file);
    g_free (session_file);
    g_free (recovery_dir);
}

static gboolean
on_save_timeout (gpointer data)
{
    _xed_session_save (XED_APP (data));

    return G_SOURCE_CONTINUE;
}

void
_xed_session_init (XedApp *app)
{
    g_return_if_fail (XED_IS_APP (app));

    if (save_timeout_id == 0)
    {
        save_timeout_id = g_timeout_add_seconds (SESSION_SAVE_INTERVAL,
                                                 (GSourceFunc) on_save_timeout,
                                                 app);
    }
}

void
_xed_session_shutdown (void)
{
    if (save_timeout_id != 0)
    {
        g_source_remove (save_timeout_id);
        save_timeout_id = 0;
    }

    g_clear_object (&editor_settings);
}

/* Restore a single document described by group into window. */
static gboolean
restore_document (XedWindow   *window,
                  GKeyFile    *key_file,
                  const gchar *group,
                  const gchar *recovery_dir,
                  gboolean     jump_to)
{
    gchar *uri;
    gchar *recovery;
    gboolean restored = FALSE;

    uri = g_key_file_get_string (key_file, group, "uri", NULL);
    recovery = g_key_file_get_string (key_file, group, "recovery", NULL);

    if (recovery != NULL && recovery[0] != '\0')
    {
        gchar *path = g_build_filename (recovery_dir, recovery, NULL);
        gchar *content = NULL;
        gsize length = 0;

        if (g_file_get_contents (path, &content, &length, NULL))
        {
            XedTab *tab;
            XedDocument *doc;
            GtkTextBuffer *buffer;

            tab = xed_window_create_tab (window, jump_to);
            doc = xed_tab_get_document (tab);
            buffer = GTK_TEXT_BUFFER (doc);

            /* Keep the same recovery id so we keep overwriting one file. */
            g_object_set_data_full (G_OBJECT (doc), RECOVERY_ID_KEY,
                                    g_strdup (recovery), g_free);

            if (uri != NULL && uri[0] != '\0')
            {
                GFile *location = g_file_new_for_uri (uri);
                xed_document_set_location (doc, location);
                g_object_unref (location);
            }

            gtk_text_buffer_set_text (buffer, content, length);
            gtk_text_buffer_set_modified (buffer, TRUE);

            restored = TRUE;
        }

        g_free (content);
        g_free (path);
    }

    /* No (readable) recovery copy: fall back to loading the saved file, if
     * any. This also covers documents that were saved and unmodified.
     */
    if (!restored && uri != NULL && uri[0] != '\0')
    {
        GFile *location = g_file_new_for_uri (uri);
        xed_window_create_tab_from_location (window, location, NULL, 0, FALSE, jump_to);
        g_object_unref (location);

        restored = TRUE;
    }

    g_free (uri);
    g_free (recovery);

    return restored;
}

gboolean
_xed_session_restore (XedApp    *app,
                      XedWindow *window)
{
    GKeyFile *key_file;
    gchar *session_file;
    gchar *recovery_dir;
    gchar **groups;
    gsize n_groups, i;
    gboolean restored = FALSE;
    gboolean jump_to = TRUE;

    g_return_val_if_fail (XED_IS_APP (app), FALSE);
    g_return_val_if_fail (XED_IS_WINDOW (window), FALSE);

    if (!session_enabled ())
    {
        return FALSE;
    }

    session_file = get_session_file ();
    key_file = g_key_file_new ();

    if (!g_key_file_load_from_file (key_file, session_file, G_KEY_FILE_NONE, NULL))
    {
        g_key_file_free (key_file);
        g_free (session_file);
        return FALSE;
    }

    xed_debug (DEBUG_APP);

    recovery_dir = get_recovery_dir ();

    groups = g_key_file_get_groups (key_file, &n_groups);
    for (i = 0; i < n_groups; i++)
    {
        if (!g_str_has_prefix (groups[i], DOCUMENT_GROUP_PREFIX))
        {
            continue;
        }

        if (restore_document (window, key_file, groups[i], recovery_dir, jump_to))
        {
            /* Only the first restored tab should grab focus. */
            jump_to = FALSE;
            restored = TRUE;
        }
    }

    g_strfreev (groups);
    g_key_file_free (key_file);
    g_free (recovery_dir);
    g_free (session_file);

    return restored;
}
