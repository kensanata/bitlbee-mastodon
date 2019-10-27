/***************************************************************************\
*                                                                           *
*  BitlBee - An IRC to IM gateway                                           *
*  Simple module to facilitate Mastodon functionality.                      *
*                                                                           *
*  Copyright 2009 Geert Mulders <g.c.w.m.mulders@gmail.com>                 *
*  Copyright 2017-2018 Alex Schroeder <alex@gnu.org>                        *
*                                                                           *
*  This library is free software; you can redistribute it and/or            *
*  modify it under the terms of the GNU Lesser General Public               *
*  License as published by the Free Software Foundation, version            *
*  2.1.                                                                     *
*                                                                           *
*  This library is distributed in the hope that it will be useful,          *
*  but WITHOUT ANY WARRANTY; without even the implied warranty of           *
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU        *
*  Lesser General Public License for more details.                          *
*                                                                           *
*  You should have received a copy of the GNU Lesser General Public License *
*  along with this library; if not, write to the Free Software Foundation,  *
*  Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA           *
*                                                                           *
****************************************************************************/

#pragma once

#include "nogaim.h"
#include "mastodon-http.h"

#define MASTODON_DEFAULT_INSTANCE "https://mastodon.social"

// "2017-08-02T10:45:03.000Z" -- but we're ignoring microseconds and UTC timezone
#define MASTODON_TIME_FORMAT "%Y-%m-%dT%H:%M:%S"

#define MASTODON_REGISTER_APP_URL "/api/v1/apps"
#define MASTODON_VERIFY_CREDENTIALS_URL "/api/v1/accounts/verify_credentials"
#define MASTODON_STREAMING_USER_URL "/api/v1/streaming/user"
#define MASTODON_STREAMING_HASHTAG_URL "/api/v1/streaming/hashtag"
#define MASTODON_STREAMING_LOCAL_URL "/api/v1/streaming/public/local"
#define MASTODON_STREAMING_FEDERATED_URL "/api/v1/streaming/public"
#define MASTODON_STREAMING_LIST_URL "/api/v1/streaming/list"
#define MASTODON_HOME_TIMELINE_URL "/api/v1/timelines/home"
#define MASTODON_PUBLIC_TIMELINE_URL "/api/v1/timelines/public"
#define MASTODON_HASHTAG_TIMELINE_URL "/api/v1/timelines/tag/%s"
#define MASTODON_LIST_TIMELINE_URL "/api/v1/timelines/list/%" G_GINT64_FORMAT
#define MASTODON_NOTIFICATIONS_URL "/api/v1/notifications"

#define MASTODON_REPORT_URL "/api/v1/reports"
#define MASTODON_SEARCH_URL "/api/v2/search"

#define MASTODON_INSTANCE_URL "/api/v1/instance"

#define MASTODON_STATUS_POST_URL "/api/v1/statuses"
#define MASTODON_STATUS_URL "/api/v1/statuses/%" G_GINT64_FORMAT
#define MASTODON_STATUS_BOOST_URL "/api/v1/statuses/%" G_GINT64_FORMAT "/reblog"
#define MASTODON_STATUS_UNBOOST_URL "/api/v1/statuses/%" G_GINT64_FORMAT "/unreblog"
#define MASTODON_STATUS_MUTE_URL "/api/v1/statuses/%" G_GINT64_FORMAT "/mute"
#define MASTODON_STATUS_UNMUTE_URL "/api/v1/statuses/%" G_GINT64_FORMAT "/unmute"
#define MASTODON_STATUS_FAVOURITE_URL "/api/v1/statuses/%" G_GINT64_FORMAT "/favourite"
#define MASTODON_STATUS_UNFAVOURITE_URL "/api/v1/statuses/%" G_GINT64_FORMAT "/unfavourite"
#define MASTODON_STATUS_PIN_URL "/api/v1/statuses/%" G_GINT64_FORMAT "/pin"
#define MASTODON_STATUS_UNPIN_URL "/api/v1/statuses/%" G_GINT64_FORMAT "/unpin"
#define MASTODON_STATUS_CONTEXT_URL "/api/v1/statuses/%" G_GINT64_FORMAT "/context"

#define MASTODON_ACCOUNT_URL "/api/v1/accounts/%" G_GINT64_FORMAT
#define MASTODON_ACCOUNT_SEARCH_URL "/api/v1/accounts/search"
#define MASTODON_ACCOUNT_STATUSES_URL "/api/v1/accounts/%" G_GINT64_FORMAT "/statuses"
#define MASTODON_ACCOUNT_FOLLOWING_URL "/api/v1/accounts/%" G_GINT64_FORMAT "/following"
#define MASTODON_ACCOUNT_BLOCK_URL "/api/v1/accounts/%" G_GINT64_FORMAT "/block"
#define MASTODON_ACCOUNT_UNBLOCK_URL "/api/v1/accounts/%" G_GINT64_FORMAT "/unblock"
#define MASTODON_ACCOUNT_FOLLOW_URL "/api/v1/accounts/%" G_GINT64_FORMAT "/follow"
#define MASTODON_ACCOUNT_UNFOLLOW_URL "/api/v1/accounts/%" G_GINT64_FORMAT "/unfollow"
#define MASTODON_ACCOUNT_MUTE_URL "/api/v1/accounts/%" G_GINT64_FORMAT "/mute"
#define MASTODON_ACCOUNT_UNMUTE_URL "/api/v1/accounts/%" G_GINT64_FORMAT "/unmute"

#define MASTODON_LIST_URL "/api/v1/lists"
#define MASTODON_LIST_DATA_URL "/api/v1/lists/%" G_GINT64_FORMAT
#define MASTODON_LIST_ACCOUNTS_URL "/api/v1/lists/%" G_GINT64_FORMAT "/accounts"

#define MASTODON_FILTER_URL "/api/v1/filters"
#define MASTODON_FILTER_DATA_URL "/api/v1/filters/%" G_GINT64_FORMAT

#define MASTODON_ACCOUNT_RELATIONSHIP_URL "/api/v1/accounts/relationships"

typedef enum {
	MASTODON_EVT_UNKNOWN,
	MASTODON_EVT_UPDATE,
	MASTODON_EVT_NOTIFICATION,
	MASTODON_EVT_DELETE,
} mastodon_evt_flags_t;

void mastodon_register_app(struct im_connection *ic);
void mastodon_verify_credentials(struct im_connection *ic);
void mastodon_notifications(struct im_connection *ic);
void mastodon_initial_timeline(struct im_connection *ic);
void mastodon_hashtag_timeline(struct im_connection *ic, char *hashtag);
void mastodon_home_timeline(struct im_connection *ic);
void mastodon_local_timeline(struct im_connection *ic);
void mastodon_federated_timeline(struct im_connection *ic);
void mastodon_open_user_stream(struct im_connection *ic);
void mastodon_unknown_list_timeline(struct im_connection *ic, char *title);
struct http_request *mastodon_open_hashtag_stream(struct im_connection *ic, char *hashtag);
struct http_request *mastodon_open_local_stream(struct im_connection *ic);
struct http_request *mastodon_open_federated_stream(struct im_connection *ic);
void mastodon_open_unknown_list_stream(struct im_connection *ic, struct groupchat *c, char *title);
mastodon_visibility_t mastodon_default_visibility(struct im_connection *ic);
mastodon_visibility_t mastodon_parse_visibility(char *value);
char *mastodon_visibility(mastodon_visibility_t visibility);
void mastodon_post_status(struct im_connection *ic, char *msg, guint64 in_reply_to, mastodon_visibility_t visibility, char *spoiler_text);
void mastodon_post(struct im_connection *ic, char *format, mastodon_command_type_t command, guint64 id);
GString *mastodon_account_join(GSList *l, gchar *init);
void mastodon_show_mentions(struct im_connection *ic, GSList *l);
void mastodon_status_show_mentions(struct im_connection *ic, guint64 id);
void mastodon_status_show_url(struct im_connection *ic, guint64 id);
void mastodon_report(struct im_connection *ic, guint64 id, char *comment);
void mastodon_follow(struct im_connection *ic, char *who);
void mastodon_status_delete(struct im_connection *ic, guint64 id);
void mastodon_instance(struct im_connection *ic);
void mastodon_account(struct im_connection *ic, guint64 id);
void mastodon_search_account(struct im_connection *ic, char *who);
void mastodon_status(struct im_connection *ic, guint64 id);
void mastodon_raw(struct im_connection *ic, char *method, char *url, char **arguments, int arguments_len);
void mastodon_relationship(struct im_connection *ic, guint64 id);
void mastodon_search_relationship(struct im_connection *ic, char *who);
void mastodon_search(struct im_connection *ic, char *what);
void mastodon_context(struct im_connection *ic, guint64 id);
void mastodon_more(struct im_connection *ic);
void mastodon_account_statuses(struct im_connection *ic, guint64 id);
void mastodon_unknown_account_statuses(struct im_connection *ic, char *who);
void mastodon_account_pinned_statuses(struct im_connection *ic, guint64 id);
void mastodon_unknown_account_pinned_statuses(struct im_connection *ic, char *who);
void mastodon_account_bio(struct im_connection *ic, guint64 id);
void mastodon_unknown_account_bio(struct im_connection *ic, char *who);
void mastodon_lists(struct im_connection *ic);
void mastodon_list_create(struct im_connection *ic, char *title);
void mastodon_unknown_list_accounts(struct im_connection *ic, char *title);
void mastodon_unknown_list_delete(struct im_connection *ic, char *title);
void mastodon_unknown_list_add_account(struct im_connection *ic, guint64 id, char *title);
void mastodon_unknown_list_remove_account(struct im_connection *ic, guint64 id, char *title);
void mastodon_list_reload(struct im_connection *ic, gboolean populate);
void mastodon_filters_destroy(struct mastodon_data *md);
void mastodon_filters(struct im_connection *ic);
void mastodon_filter_create(struct im_connection *ic, char *str);
void mastodon_filter_delete(struct im_connection *ic, char *arg);
