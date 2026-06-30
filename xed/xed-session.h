/*
 * xed-session.h
 * This file is part of xed
 *
 * Crash-recovery autosave and session restore.
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

#ifndef __XED_SESSION_H__
#define __XED_SESSION_H__

#include "xed-app.h"
#include "xed-window.h"

G_BEGIN_DECLS

/* Start the periodic snapshot timer. Safe to call once at startup. */
void     _xed_session_init     (XedApp *app);

/* Stop the periodic snapshot timer and release resources. */
void     _xed_session_shutdown (void);

/* Take a snapshot now: persist the list of open documents and a recovery
 * copy of every modified buffer. Does nothing when the feature is disabled.
 */
void     _xed_session_save     (XedApp *app);

/* Recreate the documents stored in the last snapshot into window.
 * Returns TRUE if at least one tab was created. Does nothing (returns
 * FALSE) when the feature is disabled or there is no saved session.
 */
gboolean _xed_session_restore  (XedApp    *app,
                                XedWindow *window);

G_END_DECLS

#endif /* __XED_SESSION_H__ */
