/*
 * roster.c
 *
 * Copyright (C) 2012 - 2015 James Booth <boothj5@gmail.com>
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
 * along with Profanity.  If not, see <http://www.gnu.org/licenses/>.
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
#include <stdlib.h>
#include <string.h>

#include <glib.h>

#ifdef HAVE_LIBMESODE
#include <mesode.h>
#endif
#ifdef HAVE_LIBSTROPHE
#include <strophe.h>
#endif

#include "log.h"
#include "profanity.h"
#include "ui/ui.h"
#include "event/server_events.h"
#include "event/client_events.h"
#include "tools/autocomplete.h"
#include "config/preferences.h"
#include "xmpp/connection.h"
#include "xmpp/roster.h"
#include "roster_list.h"
#include "xmpp/stanza.h"
#include "xmpp/xmpp.h"

#define HANDLE(type, func) xmpp_handler_add(conn, func, XMPP_NS_ROSTER, \
STANZA_NAME_IQ, type, ctx)

// callback data for group commands
typedef struct _group_data {
    char *name;
    char *group;
} GroupData;

// event handlers
static int _roster_set_handler(xmpp_conn_t * const conn,
    xmpp_stanza_t * const stanza, void * const userdata);
static int _roster_result_handler(xmpp_conn_t * const conn,
    xmpp_stanza_t * const stanza, void * const userdata);

// id handlers
static int
_group_add_handler(xmpp_conn_t * const conn, xmpp_stanza_t * const stanza,
    void * const userdata);
static int
_group_remove_handler(xmpp_conn_t * const conn, xmpp_stanza_t * const stanza,
    void * const userdata);

// helper functions
GSList * _get_groups_from_item(xmpp_stanza_t *item);

void
roster_add_handlers(void)
{
    xmpp_conn_t * const conn = connection_get_conn();
    xmpp_ctx_t * const ctx = connection_get_ctx();

    HANDLE(STANZA_TYPE_SET,    _roster_set_handler);
    HANDLE(STANZA_TYPE_RESULT, _roster_result_handler);
}

void
roster_request(void)
{
    xmpp_conn_t * const conn = connection_get_conn();
    xmpp_ctx_t * const ctx = connection_get_ctx();
    xmpp_stanza_t *iq = stanza_create_roster_iq(ctx);
    xmpp_send(conn, iq);
    xmpp_stanza_release(iq);
}

void
roster_send_add_new(const char * const barejid, const char * const name)
{
    xmpp_conn_t * const conn = connection_get_conn();
    xmpp_ctx_t * const ctx = connection_get_ctx();
    char *id = create_unique_id("roster");
    xmpp_stanza_t *iq = stanza_create_roster_set(ctx, id, barejid, name, NULL);
    free(id);
    xmpp_send(conn, iq);
    xmpp_stanza_release(iq);
}

void
roster_send_remove(const char * const barejid)
{
    xmpp_conn_t * const conn = connection_get_conn();
    xmpp_ctx_t * const ctx = connection_get_ctx();
    xmpp_stanza_t *iq = stanza_create_roster_remove_set(ctx, barejid);
    xmpp_send(conn, iq);
    xmpp_stanza_release(iq);
}

void
roster_send_name_change(const char * const barejid, const char * const new_name, GSList *groups)
{
    xmpp_conn_t * const conn = connection_get_conn();
    xmpp_ctx_t * const ctx = connection_get_ctx();
    char *id = create_unique_id("roster");
    xmpp_stanza_t *iq = stanza_create_roster_set(ctx, id, barejid, new_name, groups);
    free(id);
    xmpp_send(conn, iq);
    xmpp_stanza_release(iq);
}

void
roster_send_add_to_group(const char * const group, PContact contact)
{
    GSList *groups = p_contact_groups(contact);
    GSList *new_groups = NULL;
    while (groups) {
        new_groups = g_slist_append(new_groups, strdup(groups->data));
        groups = g_slist_next(groups);
    }

    new_groups = g_slist_append(new_groups, strdup(group));
    // add an id handler to handle the response
    char *unique_id = create_unique_id(NULL);
    GroupData *data = malloc(sizeof(GroupData));
    data->group = strdup(group);
    if (p_contact_name(contact)) {
        data->name = strdup(p_contact_name(contact));
    } else {
        data->name = strdup(p_contact_barejid(contact));
    }

    xmpp_conn_t * const conn = connection_get_conn();
    xmpp_ctx_t * const ctx = connection_get_ctx();
    xmpp_id_handler_add(conn, _group_add_handler, unique_id, data);
    xmpp_stanza_t *iq = stanza_create_roster_set(ctx, unique_id, p_contact_barejid(contact),
        p_contact_name(contact), new_groups);
    xmpp_send(conn, iq);
    xmpp_stanza_release(iq);
    free(unique_id);
}

static int
_group_add_handler(xmpp_conn_t * const conn, xmpp_stanza_t * const stanza,
    void * const userdata)
{
    if (userdata) {
        GroupData *data = userdata;
        ui_group_added(data->name, data->group);
        free(data->name);
        free(data->group);
        free(userdata);
    }
    return 0;
}

void
roster_send_remove_from_group(const char * const group, PContact contact)
{
    GSList *groups = p_contact_groups(contact);
    GSList *new_groups = NULL;
    while (groups) {
        if (strcmp(groups->data, group) != 0) {
            new_groups = g_slist_append(new_groups, strdup(groups->data));
        }
        groups = g_slist_next(groups);
    }

    xmpp_conn_t * const conn = connection_get_conn();
    xmpp_ctx_t * const ctx = connection_get_ctx();

    // add an id handler to handle the response
    char *unique_id = create_unique_id(NULL);
    GroupData *data = malloc(sizeof(GroupData));
    data->group = strdup(group);
    if (p_contact_name(contact)) {
        data->name = strdup(p_contact_name(contact));
    } else {
        data->name = strdup(p_contact_barejid(contact));
    }

    xmpp_id_handler_add(conn, _group_remove_handler, unique_id, data);
    xmpp_stanza_t *iq = stanza_create_roster_set(ctx, unique_id, p_contact_barejid(contact),
        p_contact_name(contact), new_groups);
    xmpp_send(conn, iq);
    xmpp_stanza_release(iq);
    free(unique_id);
}

static int
_group_remove_handler(xmpp_conn_t * const conn, xmpp_stanza_t * const stanza,
    void * const userdata)
{
    if (userdata) {
        GroupData *data = userdata;
        ui_group_removed(data->name, data->group);
        free(data->name);
        free(data->group);
        free(userdata);
    }
    return 0;
}

static int
_roster_set_handler(xmpp_conn_t * const conn, xmpp_stanza_t * const stanza,
    void * const userdata)
{
    xmpp_stanza_t *query =
        xmpp_stanza_get_child_by_name(stanza, STANZA_NAME_QUERY);
    xmpp_stanza_t *item =
        xmpp_stanza_get_child_by_name(query, STANZA_NAME_ITEM);

    if (item == NULL) {
        return 1;
    }

    // if from attribute exists and it is not current users barejid, ignore push
    Jid *my_jid = jid_create(jabber_get_fulljid());
    const char *from = xmpp_stanza_get_attribute(stanza, STANZA_ATTR_FROM);
    if (from && (strcmp(from, my_jid->barejid) != 0)) {
        jid_destroy(my_jid);
        return 1;
    }
    jid_destroy(my_jid);

    const char *barejid = xmpp_stanza_get_attribute(item, STANZA_ATTR_JID);
    gchar *barejid_lower = g_utf8_strdown(barejid, -1);
    const char *name = xmpp_stanza_get_attribute(item, STANZA_ATTR_NAME);
    const char *sub = xmpp_stanza_get_attribute(item, STANZA_ATTR_SUBSCRIPTION);
    const char *ask = xmpp_stanza_get_attribute(item, STANZA_ATTR_ASK);

    // do not set nickname to empty string, set to NULL instead
    if (name && (strlen(name) == 0)) {
        name = NULL;
    }

    // remove from roster
    if (g_strcmp0(sub, "remove") == 0) {
        // remove barejid and name
        if (name == NULL) {
            name = barejid_lower;
        }
        roster_remove(name, barejid_lower);
        ui_roster_remove(barejid_lower);

    // otherwise update local roster
    } else {

        // check for pending out subscriptions
        gboolean pending_out = FALSE;
        if (ask && (strcmp(ask, "subscribe") == 0)) {
            pending_out = TRUE;
        }

        GSList *groups = _get_groups_from_item(item);

        // update the local roster
        PContact contact = roster_get_contact(barejid_lower);
        if (contact == NULL) {
            gboolean added = roster_add(barejid_lower, name, groups, sub, pending_out);
            if (added) {
                ui_roster_add(barejid_lower, name);
            }
        } else {
            sv_ev_roster_update(barejid_lower, name, groups, sub, pending_out);
        }
    }

    g_free(barejid_lower);

    return 1;
}

static int
_roster_result_handler(xmpp_conn_t * const conn, xmpp_stanza_t * const stanza, void * const userdata)
{
    const char *id = xmpp_stanza_get_attribute(stanza, STANZA_ATTR_ID);

    if (g_strcmp0(id, "roster") != 0) {
        return 1;
    }

    // handle initial roster response
    xmpp_stanza_t *query = xmpp_stanza_get_child_by_name(stanza, STANZA_NAME_QUERY);
    xmpp_stanza_t *item = xmpp_stanza_get_children(query);

    while (item) {
        const char *barejid = xmpp_stanza_get_attribute(item, STANZA_ATTR_JID);
        gchar *barejid_lower = g_utf8_strdown(barejid, -1);
        const char *name = xmpp_stanza_get_attribute(item, STANZA_ATTR_NAME);
        const char *sub = xmpp_stanza_get_attribute(item, STANZA_ATTR_SUBSCRIPTION);

        // do not set nickname to empty string, set to NULL instead
        if (name && (strlen(name) == 0)) name = NULL;

        gboolean pending_out = FALSE;
        const char *ask = xmpp_stanza_get_attribute(item, STANZA_ATTR_ASK);
        if (g_strcmp0(ask, "subscribe") == 0) {
            pending_out = TRUE;
        }

        GSList *groups = _get_groups_from_item(item);

        gboolean added = roster_add(barejid_lower, name, groups, sub, pending_out);
        if (!added) {
            log_warning("Attempt to add contact twice: %s", barejid_lower);
        }

        g_free(barejid_lower);
        item = xmpp_stanza_get_next(item);
    }

    sv_ev_roster_received();

    char *account = jabber_get_account_name();
    resource_presence_t conn_presence = accounts_get_login_presence(account);

    char *last_activity_str = accounts_get_last_activity(account);
    if (last_activity_str) {
        GDateTime *nowdt = g_date_time_new_now_utc();

        GTimeVal lasttv;
        gboolean res = g_time_val_from_iso8601(last_activity_str, &lasttv);
        if (res) {
            GDateTime *lastdt = g_date_time_new_from_timeval_utc(&lasttv);
            GTimeSpan diff_micros = g_date_time_difference(nowdt, lastdt);
            int diff_secs = (diff_micros / 1000) / 1000;
            if (prefs_get_boolean(PREF_LASTACTIVITY)) {
                cl_ev_presence_send(conn_presence, NULL, diff_secs);
            } else {
                cl_ev_presence_send(conn_presence, NULL, 0);
            }
            g_date_time_unref(lastdt);
        } else {
            cl_ev_presence_send(conn_presence, NULL, 0);
        }

        free(last_activity_str);
        g_date_time_unref(nowdt);
    } else {
        cl_ev_presence_send(conn_presence, NULL, 0);
    }

    return 1;
}

GSList *
_get_groups_from_item(xmpp_stanza_t *item)
{
    GSList *groups = NULL;
    xmpp_stanza_t *group_element = xmpp_stanza_get_children(item);

    while (group_element) {
        if (strcmp(xmpp_stanza_get_name(group_element), STANZA_NAME_GROUP) == 0) {
            char *groupname = xmpp_stanza_get_text(group_element);
            if (groupname) {
                groups = g_slist_append(groups, groupname);
            }
        }
        group_element = xmpp_stanza_get_next(group_element);
    }

    return groups;
}
