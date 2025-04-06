/*
 * bookmark.c
 * vim: expandtab:ts=4:sts=4:sw=4
 *
 * Copyright (C) 2012 - 2019 James Booth <boothj5@gmail.com>
 *
 * This file is part of Profanity.
 *
 * Profanity is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Profanity is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Profanity.  If not, see <https://www.gnu.org/licenses/>.
 *
 * In addition, as a special exception, the copyright holders give permission to
 * link the code of portions of this program with the OpenSSL library under
 * certain conditions as described in each individual source file, and
 * distribute linked combinations including the two.
 *
 * You must obey the GNU General Public License in all respects for all of the
 * code used other than OpenSSL. If you modify file(s) with this exception, you
 * may extend this exception to your version of the file(s), but you are not
 * obligated to do so. If you do not wish to do so, delete this exception
 * statement from your version. If you delete this exception statement from all
 * source files in the program, then also delete it here.
 *
 */

#include "config.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>

#include <strophe.h>

#include "common.h"
#include "log.h"
#include "event/server_events.h"
#include "plugins/plugins.h"
#include "ui/ui.h"
#include "xmpp/connection.h"
#include "xmpp/iq.h"
#include "xmpp/stanza.h"
#include "xmpp/xmpp.h"
#include "xmpp/bookmark.h"
#include "xmpp/muc.h"

#define BOOKMARK_TIMEOUT 5000

static Autocomplete bookmark_ac;
static GHashTable* bookmarks;

// id handlers
static int _bookmark_result_id_handler(xmpp_stanza_t* const stanza, void* const userdata);

static void _bookmark_destroy(Bookmark* bookmark);
static void _send_bookmarks(void);

/**
 * @brief Shutdown handler for bookmarks.
 * Frees memory used by bookmarks and autocomplete data.
 */
static void
_bookmark_shutdown(void)
{
    if (bookmarks) {
        g_hash_table_destroy(bookmarks);
    }
    autocomplete_free(bookmark_ac);
}

/**
 * @brief Send an IQ request to fetch stored bookmarks.
 */
void
bookmark_request(void)
{
    prof_add_shutdown_routine(_bookmark_shutdown);
    if (bookmarks) {
        g_hash_table_destroy(bookmarks);
    }
    bookmarks = g_hash_table_new_full(g_str_hash, g_str_equal, free, (GDestroyNotify)_bookmark_destroy);

    autocomplete_free(bookmark_ac);
    bookmark_ac = autocomplete_new();

    char* id = "bookmark_init_request";
    iq_id_handler_add(id, _bookmark_result_id_handler, free, NULL);

    xmpp_ctx_t* ctx = connection_get_ctx();
    xmpp_stanza_t* iq = stanza_create_bookmarks_storage_request(ctx);
    xmpp_stanza_set_id(iq, id);

    iq_send_stanza(iq);
    xmpp_stanza_release(iq);
}


/**
 * @brief Add a new bookmark for a MUC room.
 *
 * @param jid        Room JID.
 * @param nick       Optional nickname to use in the room.
 * @param password   Optional room password.
 * @param autojoin_str "on"/"off" for autojoin flag.
 * @param name       Optional display name for the bookmark.
 * @return TRUE if added, FALSE if already exists.
 */
gboolean
bookmark_add(const char* jid, const char* nick, const char* password, const char* autojoin_str, const char* name)
{
    assert(jid != NULL);

    auto_jid Jid* jidp = jid_create(jid);
    if (jidp->domainpart) {
        muc_confserver_add(jidp->domainpart);
    }

    if (g_hash_table_contains(bookmarks, jid)) {
        return FALSE;
    }

    Bookmark* bookmark = malloc(sizeof(Bookmark));
    bookmark->barejid = strdup(jid);
    if (nick) {
        bookmark->nick = strdup(nick);
    } else {
        bookmark->nick = NULL;
    }
    if (password) {
        bookmark->password = strdup(password);
    } else {
        bookmark->password = NULL;
    }
    if (name) {
        bookmark->name = strdup(name);
    } else {
        bookmark->name = NULL;
    }
    bookmark->ext_gajim_minimize = 0;

    if (g_strcmp0(autojoin_str, "on") == 0) {
        bookmark->autojoin = TRUE;
    } else {
        bookmark->autojoin = FALSE;
    }

    g_hash_table_insert(bookmarks, strdup(jid), bookmark);
    autocomplete_add(bookmark_ac, jid);

    _send_bookmarks();

    return TRUE;
}

/**
 * @brief Update an existing bookmark.
 *
 * @param jid        Room JID.
 * @param nick       Updated nickname.
 * @param password   Updated password.
 * @param autojoin_str "on"/"off" for autojoin flag.
 * @param name       Updated name.
 * @return TRUE if updated, FALSE if bookmark not found.
 */
gboolean
bookmark_update(const char* jid, const char* nick, const char* password, const char* autojoin_str, const char* name)
{
    assert(jid != NULL);

    Bookmark* bookmark = g_hash_table_lookup(bookmarks, jid);
    if (!bookmark) {
        return FALSE;
    }

    if (nick) {
        free(bookmark->nick);
        bookmark->nick = strdup(nick);
    }
    if (password) {
        free(bookmark->password);
        bookmark->password = strdup(password);
    }
    if (name) {
        free(bookmark->name);
        bookmark->name = strdup(name);
    }
    if (autojoin_str) {
        if (g_strcmp0(autojoin_str, "on") == 0) {
            bookmark->autojoin = TRUE;
        } else if (g_strcmp0(autojoin_str, "off") == 0) {
            bookmark->autojoin = FALSE;
        }
    }

    _send_bookmarks();
    return TRUE;
}


/**
 * @brief Join a bookmarked MUC room using the saved details.
 *
 * @param jid Room JID.
 * @return TRUE if join initiated, FALSE otherwise.
 */
gboolean
bookmark_join(const char* jid)
{
    assert(jid != NULL);

    Bookmark* bookmark = g_hash_table_lookup(bookmarks, jid);
    if (!bookmark) {
        return FALSE;
    }

    ProfAccount* account = accounts_get_account(session_get_account_name());
    if (!muc_active(bookmark->barejid)) {
        char* nick = bookmark->nick;
        if (!nick) {
            nick = account->muc_nick;
        }
        presence_join_room(bookmark->barejid, nick, bookmark->password);
        muc_join(bookmark->barejid, nick, bookmark->password, FALSE);
        iq_room_affiliation_list(bookmark->barejid, "member", false);
        iq_room_affiliation_list(bookmark->barejid, "admin", false);
        iq_room_affiliation_list(bookmark->barejid, "owner", false);
        account_free(account);
    } else if (muc_roster_complete(bookmark->barejid)) {
        ui_room_join(bookmark->barejid, TRUE);
        account_free(account);
    }

    return TRUE;
}

/**
 * @brief Remove a bookmark by room JID.
 *
 * @param jid Room JID.
 * @return TRUE if removed, FALSE if not found.
 */
gboolean
bookmark_remove(const char* jid)
{
    assert(jid != NULL);

    Bookmark* bookmark = g_hash_table_lookup(bookmarks, jid);
    if (!bookmark) {
        return FALSE;
    }

    g_hash_table_remove(bookmarks, jid);
    autocomplete_remove(bookmark_ac, jid);

    _send_bookmarks();

    return TRUE;
}


/**
 * @brief Get a bookmark struct by room JID.
 * @param jid Room JID.
 * @return Pointer to Bookmark or NULL.
 */
Bookmark*
bookmark_get_by_jid(const char* jid)
{
    Bookmark* bookmark = g_hash_table_lookup(bookmarks, jid);
    return bookmark;
}

/**
 * @brief Get a list of all current bookmarks.
 * @return GList* of Bookmark*.
 */
GList*
bookmark_get_list(void)
{
    return g_hash_table_get_values(bookmarks);
}

/**
 * @brief Find a bookmark match via autocomplete.
 *
 * @param search_str String to search.
 * @param previous Direction flag.
 * @param context   Optional user context (unused).
 * @return Matching bookmark JID string.
 */
char*
bookmark_find(const char* const search_str, gboolean previous, void* context)
{
    return autocomplete_complete(bookmark_ac, search_str, TRUE, previous);
}

/**
 * @brief Reset bookmark autocomplete state.
 */
void
bookmark_autocomplete_reset(void)
{
    if (bookmark_ac) {
        autocomplete_reset(bookmark_ac);
    }
}

/**
 * @brief Check if a bookmark exists for a given JID.
 * @param room Room JID.
 * @return TRUE if found.
 */
gboolean
bookmark_exists(const char* const room)
{
    return g_hash_table_contains(bookmarks, room);
}


/**
 * @brief Internal handler for IQ result containing stored bookmarks.
 *
 * Parses the result and updates the local bookmarks table.
 *
 * @param stanza Received IQ stanza.
 * @param userdata Unused.
 * @return Always returns 0.
 */
static int
_bookmark_result_id_handler(xmpp_stanza_t* const stanza, void* const userdata)
{
    const char* name = xmpp_stanza_get_name(stanza);
    if (!name || strcmp(name, STANZA_NAME_IQ) != 0) {
        return 0;
    }

    xmpp_stanza_t* query = xmpp_stanza_get_child_by_name(stanza, STANZA_NAME_QUERY);
    if (!query) {
        return 0;
    }
    xmpp_stanza_t* storage = xmpp_stanza_get_child_by_name(query, STANZA_NAME_STORAGE);
    if (!storage) {
        return 0;
    }

    if (bookmark_ac == NULL) {
        bookmark_ac = autocomplete_new();
    }

    xmpp_stanza_t* child = xmpp_stanza_get_children(storage);
    while (child) {
        name = xmpp_stanza_get_name(child);
        if (!name || strcmp(name, STANZA_NAME_CONFERENCE) != 0) {
            child = xmpp_stanza_get_next(child);
            continue;
        }
        const char* barejid = xmpp_stanza_get_attribute(child, STANZA_ATTR_JID);
        if (!barejid) {
            child = xmpp_stanza_get_next(child);
            continue;
        }

        log_debug("Handle bookmark for %s", barejid);

        const char* room_name = xmpp_stanza_get_attribute(child, "name");

        char* nick = NULL;
        xmpp_stanza_t* nick_st = xmpp_stanza_get_child_by_name(child, "nick");
        if (nick_st) {
            nick = stanza_text_strdup(nick_st);
        }

        char* password = NULL;
        xmpp_stanza_t* password_st = xmpp_stanza_get_child_by_name(child, "password");
        if (password_st) {
            password = stanza_text_strdup(password_st);
        }

        const char* autojoin = xmpp_stanza_get_attribute(child, "autojoin");
        gboolean autojoin_val = FALSE;
        if (autojoin && (strcmp(autojoin, "1") == 0 || strcmp(autojoin, "true") == 0)) {
            autojoin_val = TRUE;
        }

        // we save minimize, which is not standard, so that we don't remove it if it was set by gajim
        int minimize = 0;
        xmpp_stanza_t* minimize_st = xmpp_stanza_get_child_by_name_and_ns(child, STANZA_NAME_MINIMIZE, STANZA_NS_EXT_GAJIM_BOOKMARKS);
        if (minimize_st) {
            auto_char char* min_str = xmpp_stanza_get_text(minimize_st);
            if (strcmp(min_str, "true") == 0) {
                minimize = 1;
            } else if (strcmp(min_str, "false") == 0) {
                minimize = 2;
            }
        }

        autocomplete_add(bookmark_ac, barejid);
        Bookmark* bookmark = malloc(sizeof(Bookmark));
        bookmark->barejid = strdup(barejid);
        bookmark->nick = nick;
        bookmark->password = password;
        bookmark->name = room_name ? strdup(room_name) : NULL;
        bookmark->autojoin = autojoin_val;
        bookmark->ext_gajim_minimize = minimize;
        g_hash_table_insert(bookmarks, strdup(barejid), bookmark);

        if (autojoin_val) {
            sv_ev_bookmark_autojoin(bookmark);
        }

        auto_jid Jid* jidp = jid_create(barejid);
        if (jidp->domainpart) {
            muc_confserver_add(jidp->domainpart);
        }

        child = xmpp_stanza_get_next(child);
    }

    return 0;
}

/**
 * @brief Free all memory associated with a Bookmark object.
 * @param bookmark Pointer to Bookmark.
 */
static void
_bookmark_destroy(Bookmark* bookmark)
{
    if (bookmark) {
        free(bookmark->barejid);
        free(bookmark->nick);
        free(bookmark->password);
        free(bookmark->name);
        free(bookmark);
    }
}

/**
 * @brief Send updated bookmarks to the XMPP server for storage.
 */
static void
_send_bookmarks(void)
{
    xmpp_ctx_t* ctx = connection_get_ctx();

    auto_char char* id = connection_create_stanza_id();
    xmpp_stanza_t* iq = xmpp_iq_new(ctx, STANZA_TYPE_SET, id);

    xmpp_stanza_t* query = xmpp_stanza_new(ctx);
    xmpp_stanza_set_name(query, STANZA_NAME_QUERY);
    xmpp_stanza_set_ns(query, "jabber:iq:private");

    xmpp_stanza_t* storage = xmpp_stanza_new(ctx);
    xmpp_stanza_set_name(storage, STANZA_NAME_STORAGE);
    xmpp_stanza_set_ns(storage, "storage:bookmarks");

    GList* bookmark_list = g_hash_table_get_values(bookmarks);
    GList* curr = bookmark_list;
    while (curr) {
        Bookmark* bookmark = curr->data;
        xmpp_stanza_t* conference = xmpp_stanza_new(ctx);
        xmpp_stanza_set_name(conference, STANZA_NAME_CONFERENCE);
        xmpp_stanza_set_attribute(conference, STANZA_ATTR_JID, bookmark->barejid);

        if (bookmark->name) {
            // use specified bookmark name
            xmpp_stanza_set_attribute(conference, STANZA_ATTR_NAME, bookmark->name);
        } else {
            // use localpart of JID by if no name is specified
            auto_jid Jid* jidp = jid_create(bookmark->barejid);
            if (jidp->localpart) {
                xmpp_stanza_set_attribute(conference, STANZA_ATTR_NAME, jidp->localpart);
            }
        }

        if (bookmark->autojoin) {
            xmpp_stanza_set_attribute(conference, STANZA_ATTR_AUTOJOIN, "true");
        } else {
            xmpp_stanza_set_attribute(conference, STANZA_ATTR_AUTOJOIN, "false");
        }

        if (bookmark->nick) {
            xmpp_stanza_t* nick_st = xmpp_stanza_new(ctx);
            xmpp_stanza_set_name(nick_st, STANZA_NAME_NICK);
            xmpp_stanza_t* nick_text = xmpp_stanza_new(ctx);
            xmpp_stanza_set_text(nick_text, bookmark->nick);
            xmpp_stanza_add_child(nick_st, nick_text);
            xmpp_stanza_add_child(conference, nick_st);

            xmpp_stanza_release(nick_text);
            xmpp_stanza_release(nick_st);
        }

        if (bookmark->password) {
            xmpp_stanza_t* password_st = xmpp_stanza_new(ctx);
            xmpp_stanza_set_name(password_st, STANZA_NAME_PASSWORD);
            xmpp_stanza_t* password_text = xmpp_stanza_new(ctx);
            xmpp_stanza_set_text(password_text, bookmark->password);
            xmpp_stanza_add_child(password_st, password_text);
            xmpp_stanza_add_child(conference, password_st);

            xmpp_stanza_release(password_text);
            xmpp_stanza_release(password_st);
        }

        if (bookmark->ext_gajim_minimize == 1 || bookmark->ext_gajim_minimize == 2) {
            xmpp_stanza_t* minimize_st = xmpp_stanza_new(ctx);
            xmpp_stanza_set_name(minimize_st, STANZA_NAME_MINIMIZE);
            xmpp_stanza_set_ns(minimize_st, STANZA_NS_EXT_GAJIM_BOOKMARKS);

            xmpp_stanza_t* minimize_text = xmpp_stanza_new(ctx);
            if (bookmark->ext_gajim_minimize == 1) {
                xmpp_stanza_set_text(minimize_text, "true");
            } else {
                xmpp_stanza_set_text(minimize_text, "false");
            }

            xmpp_stanza_add_child(minimize_st, minimize_text);
            xmpp_stanza_add_child(conference, minimize_st);

            xmpp_stanza_release(minimize_text);
            xmpp_stanza_release(minimize_st);
        }

        xmpp_stanza_add_child(storage, conference);
        xmpp_stanza_release(conference);

        curr = curr->next;
    }

    g_list_free(bookmark_list);

    xmpp_stanza_add_child(query, storage);
    xmpp_stanza_add_child(iq, query);
    xmpp_stanza_release(storage);
    xmpp_stanza_release(query);

    iq_send_stanza(iq);
    xmpp_stanza_release(iq);
}
