/***************************************************************************\
*                                                                           *
*  BitlBee - An IRC to IM gateway                                           *
*  Simple module to facilitate Mastodon functionality.                      *
*                                                                           *
*  Copyright 2009-2010 Geert Mulders <g.c.w.m.mulders@gmail.com>            *
*  Copyright 2010-2012 Wilmer van der Gaast <wilmer@gaast.net>              *
*  Copyright 2017-2019 Alex Schroeder <alex@gnu.org>                        *
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

#include "nogaim.h"

#pragma once

#ifdef DEBUG_MASTODON
#define debug(...) imcb_log(ic, __VA_ARGS__);
#else
#define debug(...)
#endif

#define MASTODON_OAUTH_HANDLE "mastodon_oauth"
#define MASTODON_SCOPE "read+write+follow" // URL escaped
#define MASTODON_URL_REGEX "https?://\\S+"
#define MASTODON_MENTION_REGEX "@(([a-zA-Z0-9_]+)@[a-zA-Z0-9.-]+[a-zA-Z0-9])"

typedef enum {
	MASTODON_HAVE_FRIENDS      = 0x00001,
	MASTODON_MODE_ONE          = 0x00002,
	MASTODON_MODE_MANY         = 0x00004,
	MASTODON_MODE_CHAT         = 0x00008,
	MASTODON_GOT_TIMELINE      = 0x00010,
	MASTODON_GOT_NOTIFICATIONS = 0x00020,
	MASTODON_GOT_FILTERS       = 0x00040,
	MASTODON_GOT_STATUS        = 0x00100,
	MASTODON_GOT_CONTEXT       = 0x00200,
} mastodon_flags_t;

typedef enum {
	MASTODON_DIRECT,
	MASTODON_REPLY,
	MASTODON_MAYBE_REPLY,
	MASTODON_NEW_MESSAGE,
} mastodon_message_t;

/**
 * Visibility of a status. MV_UNKNOWN mean that we will use the default visibility when posting.
 * Higher has precedence!
 */
typedef enum {
	MV_UNKNOWN,
	MV_PUBLIC,
	MV_UNLISTED,
	MV_PRIVATE,
	MV_DIRECT,
} mastodon_visibility_t;

/**
 * Various things that come in pages such that the "more" command needs to know about it.
 */
typedef enum {
	MASTODON_MORE_STATUSES,
	MASTODON_MORE_NOTIFICATIONS,
} mastodon_more_t;

/**
 * These are the various ways a command can influence the undo/redo
 * queue.
 */
typedef enum {
	MASTODON_NEW,
	MASTODON_UNDO,
	MASTODON_REDO,
} mastodon_undo_t;

#define FS "\x1e"

/**
 * These are the commands that can be undone and redone.
 */
typedef enum {
	MC_UNKNOWN,
	MC_POST,
	MC_DELETE,
	MC_FOLLOW,
	MC_UNFOLLOW,
	MC_BLOCK,
	MC_UNBLOCK,
	MC_FAVOURITE,
	MC_UNFAVOURITE,
	MC_PIN,
	MC_UNPIN,
	MC_ACCOUNT_MUTE,
	MC_ACCOUNT_UNMUTE,
	MC_STATUS_MUTE,
	MC_STATUS_UNMUTE,
	MC_BOOST,
	MC_UNBOOST,
	MC_LIST_CREATE,
	MC_LIST_DELETE,
	MC_LIST_ADD_ACCOUNT,
	MC_LIST_REMOVE_ACCOUNT,
	MC_FILTER_CREATE,
	MC_FILTER_DELETE,
} mastodon_command_type_t;

typedef enum {
	WS_IDLE,
	WS_CONNECTING,
	WS_CONNECTED,
	WS_ALMOST_READY,
	WS_READY,
	WS_CLOSING,
} ws_state_t;

typedef enum {
	MT_HOME,
	MT_LOCAL,
	MT_FEDERATED,
	MT_HASHTAG,
	MT_LIST,
} mastodon_timeline_type_t;

struct mastodon_websocket {
	ws_state_t state;
	void *ssl;
	int  sslfd;
	gint inpa;
	char *url;
	mastodon_timeline_type_t subscription;
	struct im_connection *ic;
};

struct mastodon_log_data;

#define MASTODON_MAX_UNDO 10

struct mastodon_data {
	char* user; /* to be used when parsing commands */
	struct oauth2_service *oauth2_service;
	char *oauth2_access_token;

	gpointer home_timeline_obj; /* of mastodon_list */
	gpointer notifications_obj; /* of mastodon_list */
	gpointer status_obj; /* of mastodon_status */
	gpointer context_before_obj; /* of mastodon_list */
	gpointer context_after_obj; /* of mastodon_list */

	GSList *streams; /* of struct http_request */
	struct groupchat *timeline_gc;
	guint64 seen_id; /* For deduplication */
	mastodon_flags_t flags;

	GSList *filters; /* of struct mastodon_filter */

	guint64 last_id; /* Information about our last status posted */
	mastodon_visibility_t last_visibility;
	char *last_spoiler_text;
	GSList *mentions;

	mastodon_visibility_t visibility; /* visibility for the next status */
	char *spoiler_text; /* CW for the next post */

	mastodon_undo_t undo_type; /* for the current command */
	char *undo[MASTODON_MAX_UNDO]; /* a small stack of undo statements */
	char *redo[MASTODON_MAX_UNDO]; /* a small stack of redo statements */
	int first_undo; /* index of the latest item in the undo and redo stacks */
	int current_undo; /* index of the current item in the undo and redo stacks */

	/* for the more command */
	char *next_url;
	mastodon_more_t more_type;

	/* set base_url */
	gboolean url_ssl;
	int url_port;
	char *url_host;

	/* web socket */
	GSList *websockets; /* of struct mastodon_websocket */

	char *name; /* Used to generate contact + channel name. */

	/* set show_ids */
	struct mastodon_log_data *log;
	int log_id;
};

struct mastodon_user_data {
	guint64 account_id;
	guint64 last_id; /* last status id (in case we reply to it) */
	time_t last_time; /* when was this last status sent (if we maybe reply) */
	guint64 last_direct_id; /* last direct status id (in case we reply to it) */
	time_t last_direct_time; /* when was this last direct status sent (if we reply) */
	mastodon_visibility_t visibility; /* what visibility did it have so can use it in our reply */
	GSList *mentions; /* what accounts did it mention so we can mention them in our reply, too */
	char *spoiler_text; /* what CW did it use so we can keep it in our reply */
	GSList *lists; /* list membership of this account */
};

#define MASTODON_LOG_LENGTH 256

struct mastodon_log_data {
	guint64 id;
	/* DANGER: bu can be a dead pointer. Check it first.
	 * mastodon_message_id_from_command_arg() will do this. */
	struct bee_user *bu;
	mastodon_visibility_t visibility;
	GSList *mentions;
	char *spoiler_text;
};

/**
 * This has the same function as the msn_connections GSList. We use this to
 * make sure the connection is still alive in callbacks before we do anything
 * else.
 */
extern GSList *mastodon_connections;

/**
 * Evil hack: Fake bee_user which will always point at the local user.
 * Sometimes used as a return value by mastodon_message_id_from_command_arg.
 * NOT thread safe but don't you dare to even think of ever making BitlBee
 * threaded. :-)
 */
extern bee_user_t mastodon_log_local_user;

struct http_request;
char *mastodon_parse_error(struct http_request *req);

void mastodon_log(struct im_connection *ic, char *format, ...);
void oauth2_init(struct im_connection *ic);
struct groupchat *mastodon_groupchat_init(struct im_connection *ic);

void mastodon_do(struct im_connection *ic, char *redo, char *undo);
void mastodon_do_update(struct im_connection *ic, char *to);
