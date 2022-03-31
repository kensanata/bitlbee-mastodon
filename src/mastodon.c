/***************************************************************************\
*                                                                           *
*  BitlBee - An IRC to IM gateway                                           *
*  Simple module to facilitate Mastodon functionality.                      *
*                                                                           *
*  Copyright 2009-2010 Geert Mulders <g.c.w.m.mulders@gmail.com>            *
*  Copyright 2010-2013 Wilmer van der Gaast <wilmer@gaast.net>              *
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

#include "bitlbee.h"
#include "account.h"
#include "nogaim.h"
#include "oauth.h"
#include "oauth2.h"
#include "mastodon.h"
#include "mastodon-http.h"
#include "mastodon-websockets.h"
#include "mastodon-lib.h"
#include "rot13.h"
#include "url.h"
#include "help.h"
#include <stdbool.h>

#define HELPFILE_NAME "mastodon-help.txt"

static void mastodon_help_init()
{
  /* Figure out where our help file is by looking at the global helpfile. */
  gchar *dir = g_path_get_dirname (global.helpfile);
  if (strcmp(dir, ".") == 0) {
    log_message(LOGLVL_WARNING, "Error finding the directory of helpfile %s.", global.helpfile);
    g_free(dir);
    return;
  }
  gchar *df = g_strjoin("/", dir, HELPFILE_NAME, NULL);
  g_free(dir);

  /* Load help from our own help file. */
  help_t *dh;
  help_init(&dh, df);
  if(dh == NULL) {
    log_message(LOGLVL_WARNING, "Error opening helpfile: %s.", df);
    g_free(df);
    return;
  }
  g_free(df);

  /* Link the last entry of global.help with first entry of our help. */
  help_t *h, *l = NULL;
  for (h = global.help; h; h = h->next) {
    l = h;
  }
  if (l) {
    l->next = dh;
  } else {
    /* No global help but ours? */
    global.help = dh;
  }
}

#ifdef BITLBEE_ABI_VERSION_CODE
struct plugin_info *init_plugin_info(void)
{
	/* Run ./configure to change these. */
	static struct plugin_info info = {
		BITLBEE_ABI_VERSION_CODE,
		PACKAGE_NAME,
		PACKAGE_VERSION,
		"Bitlbee plugin for Mastodon <https://joinmastodon.org/>",
		"Alex Schroeder <alex@gnu.org>",
		"https://alexschroeder.ch/cgit/bitlbee-mastodon/about/"
	};

	return &info;
}
#endif

GSList *mastodon_connections = NULL;

struct groupchat *mastodon_groupchat_init(struct im_connection *ic)
{
	struct groupchat *gc;
	struct mastodon_data *md = ic->proto_data;
	GSList *l;

	if (md->timeline_gc) {
		return md->timeline_gc;
	}

	md->timeline_gc = gc = imcb_chat_new(ic, "mastodon/timeline");
	imcb_chat_name_hint(gc, md->name);

	for (l = ic->bee->users; l; l = l->next) {
		bee_user_t *bu = l->data;
		if (bu->ic == ic) {
			imcb_chat_add_buddy(gc, bu->handle);
		}
	}
	imcb_chat_add_buddy(gc, ic->acc->user);

	return gc;
}

/**
 * Free the oauth2_service struct.
 */
static void os_free(struct oauth2_service *os) {

	if (os == NULL) {
		return;
	}

	g_free(os->auth_url);
	g_free(os->token_url);
	g_free(os);
}

/**
 * Create a new oauth2_service struct. If we haven never connected to
 * the server, we'll be missing our key and secret.
 */
static struct oauth2_service *get_oauth2_service(struct im_connection *ic)
{
	struct mastodon_data *md = ic->proto_data;

	struct oauth2_service *os = g_new0(struct oauth2_service, 1);
	os->auth_url = g_strconcat("https://", md->url_host, "/oauth/authorize", NULL);
	os->token_url = g_strconcat("https://", md->url_host, "/oauth/token", NULL);
	os->redirect_url = "urn:ietf:wg:oauth:2.0:oob";
	os->scope = MASTODON_SCOPE;

	// possibly empty strings if the client is not registered
	os->consumer_key = set_getstr(&ic->acc->set, "consumer_key");
	os->consumer_secret = set_getstr(&ic->acc->set, "consumer_secret");

	return os;
}

/**
 * Check message length by comparing it to the appropriate setting.
 * Note this issue: "Count all URLs in text as 23 characters flat, do
 * not count domain part of usernames."
 * https://github.com/tootsuite/mastodon/pull/4427
 **/
static gboolean mastodon_length_check(struct im_connection *ic, gchar *msg, char *cw)
{
	int len = g_utf8_strlen(msg, -1);
	if (len == 0) {
		mastodon_log(ic, "This message is empty.");
		return FALSE;
	}

	if (cw != NULL) {
		len += g_utf8_strlen(cw, -1);
	}

	int max = set_getint(&ic->acc->set, "message_length");
	if (max == 0) {
		return TRUE;
	}

	GRegex *regex = g_regex_new (MASTODON_URL_REGEX, 0, 0, NULL);
	GMatchInfo *match_info;

	g_regex_match (regex, msg, 0, &match_info);
	while (g_match_info_matches (match_info))
	{
	    gchar *url = g_match_info_fetch (match_info, 0);
	    len = len - g_utf8_strlen(url, -1) + 23;
	    g_free (url);
	    g_match_info_next (match_info, NULL);
	}
	g_regex_unref (regex);

	regex = g_regex_new (MASTODON_MENTION_REGEX, 0, 0, NULL);
	g_regex_match (regex, msg, 0, &match_info);
	while (g_match_info_matches (match_info))
	{
	    gchar *mention = g_match_info_fetch (match_info, 0);
	    gchar *nick = g_match_info_fetch (match_info, 2);
	    len = len - g_utf8_strlen(mention, -1) + g_utf8_strlen(nick, -1);
	    g_free (mention);
	    g_free (nick);
	    g_match_info_next (match_info, NULL);
	}
	g_regex_unref (regex);

	g_match_info_free (match_info);

	if (len <= max) {
		return TRUE;
	}

	mastodon_log(ic, "Maximum message length exceeded: %d > %d", len, max);

	return FALSE;
}

static char *set_eval_commands(set_t * set, char *value)
{
	if (g_ascii_strcasecmp(value, "strict") == 0) {
		return value;
	} else {
		return set_eval_bool(set, value);
	}
}

static char *set_eval_mode(set_t * set, char *value)
{
	if (g_ascii_strcasecmp(value, "one") == 0 ||
	    g_ascii_strcasecmp(value, "many") == 0 || g_ascii_strcasecmp(value, "chat") == 0) {
		return value;
	} else {
		return NULL;
	}
}

static char *set_eval_hide_sensitive(set_t * set, char *value)
{
	if (g_ascii_strcasecmp(value, "rot13") == 0 ||
	    g_ascii_strcasecmp(value, "advanced_rot13") == 0) {
		return value;
	} else {
		return set_eval_bool(set, value);
	}
}

static char *set_eval_visibility(set_t * set, char *value)
{
	if (g_ascii_strcasecmp(value, "public") == 0
	    || g_ascii_strcasecmp(value, "unlisted") == 0
	    || g_ascii_strcasecmp(value, "private") == 0) {
		return value;
	} else {
		return "public";
	}
}

static void mastodon_init(account_t * acc)
{
	set_t *s;

	char* handle = acc -> user,
	    * new_user_name;
	bool change_user_name = false;

	if (*handle == '@') {
		change_user_name = true;
		new_user_name = ++ handle;
	} else new_user_name = acc -> user;

	size_t handle_sz = strlen(handle);
	char const* base_url;

	while (*handle != '@') {
		if (*handle == 0) {
			/* the user has entered an invalid handle - the smart thing
			 * to do here would be to fail, but bitlbee doesn't provide
			 * a way for us to indicate that an account add command has
			 * failed, so we glue a common instance name to the account
			 * and hope for the best */
			base_url = MASTODON_DEFAULT_INSTANCE;
			goto no_instance_in_username;
		}
		handle++;
	}

	*handle = 0; /* delete the server component from the handle */
	change_user_name = true;
	size_t endpoint_sz = (handle - (acc -> user));
	handle_sz -= endpoint_sz + 1;

	/* construct a server url */ {
		char const* instance = handle + 1;
		char* endpoint = alloca( /* using alloca instead of VLAs to
									avoid thorny scope problems */
			endpoint_sz +
			sizeof "https://" +
			1 /* trailing nul */
		);

		char* eptr = endpoint;
		eptr = g_stpcpy(eptr, "https://");
		eptr = g_stpcpy(eptr, instance);

		base_url = endpoint;
	}

no_instance_in_username:
	if (change_user_name) {
		char saved_str [handle_sz + 1]; g_stpcpy(saved_str, new_user_name);
 		/* i promise i can explain.
 		 * i haven't dug too deeply into what causes this bug, because
 		 * it's 5am and i've gotten no sleep tonight, but for some
 		 * ungodly reason - due to a bug in either glib or the bitlbee
 		 * set structure - passing a substring of the set's existing
 		 * value appears to cause memory corruption of some kind (in
 		 * this instance, deleting the first character of the username.)
 		 * temporarily duplicating the string and setting it from the
 		 * duplicate seems to fix the problem. it's an atrocious hack,
 		 * and if you're reading this, i beg you to do what i did not
 		 * have the strength to, and figure out why on god's green
 		 * earth it happened. */

 		set_setstr(&acc -> set, "username", saved_str);
	}

	s = set_add(&acc->set, "auto_reply_timeout", "10800", set_eval_int, acc);

	s = set_add(&acc->set, "base_url", base_url, NULL, acc);
	s->flags |= ACC_SET_OFFLINE_ONLY;

	s = set_add(&acc->set, "commands", "true", set_eval_commands, acc);

	s = set_add(&acc->set, "message_length", "500", set_eval_int, acc);

	s = set_add(&acc->set, "mode", "chat", set_eval_mode, acc);
	s->flags |= ACC_SET_OFFLINE_ONLY;

	s = set_add(&acc->set, "name", "", NULL, acc);
	s->flags |= ACC_SET_OFFLINE_ONLY;

	s = set_add(&acc->set, "show_ids", "true", set_eval_bool, acc);

	s = set_add(&acc->set, "strip_newlines", "false", set_eval_bool, acc);

	s = set_add(&acc->set, "hide_sensitive", "false", set_eval_hide_sensitive, acc);
	s = set_add(&acc->set, "sensitive_flag", "*NSFW* ", NULL, acc);

	s = set_add(&acc->set, "visibility", "public", set_eval_visibility, acc);

	s = set_add(&acc->set, "hide_boosts", "false", set_eval_bool, acc);
	s = set_add(&acc->set, "hide_favourites", "false", set_eval_bool, acc);
	s = set_add(&acc->set, "hide_mentions", "false", set_eval_bool, acc);
	s = set_add(&acc->set, "hide_follows", "false", set_eval_bool, acc);

	s = set_add(&acc->set, "app_id", "0", set_eval_int, acc);
	s->flags |= SET_HIDDEN;

	s = set_add(&acc->set, "account_id", "0", set_eval_int, acc);
	s->flags |= SET_HIDDEN;

	s = set_add(&acc->set, "consumer_key", "", NULL, acc);
	s->flags |= SET_HIDDEN;

	s = set_add(&acc->set, "consumer_secret", "", NULL, acc);
	s->flags |= SET_HIDDEN;

	mastodon_help_init();
}

/**
 * Set the name of the Mastodon channel, either based on a preference, or based on hostname and account name.
 */
static void mastodon_set_name(struct im_connection *ic)
{
	struct mastodon_data *md = ic->proto_data;
	char *name = set_getstr(&ic->acc->set, "name");
	if (name[0]) {
		md->name = g_strdup(name);
	} else {
		md->name = g_strdup_printf("%s_%s", md->url_host, ic->acc->user);
	}
}


/**
 * Connect to Mastodon server, using the data we saved in the account.
 */
static void mastodon_connect(struct im_connection *ic)
{
	struct mastodon_data *md = ic->proto_data;
	url_t url;
	char *s;

	imcb_log(ic, "Connecting");

	if (!url_set(&url, set_getstr(&ic->acc->set, "base_url")) ||
	    url.proto != PROTO_HTTPS) {
		imcb_error(ic, "Incorrect API base URL: %s", set_getstr(&ic->acc->set, "base_url"));
		imc_logout(ic, FALSE);
		return;
	}

	md->url_ssl = url.proto == PROTO_HTTPS; // always
	md->url_port = url.port;
	md->url_host = g_strdup(url.host);

	mastodon_set_name(ic);
	imcb_add_buddy(ic, md->name, NULL);
	imcb_buddy_status(ic, md->name, OPT_LOGGED_IN, NULL, NULL);

	md->log = g_new0(struct mastodon_log_data, MASTODON_LOG_LENGTH);
	md->log_id = -1;

	s = set_getstr(&ic->acc->set, "mode");
	if (g_ascii_strcasecmp(s, "one") == 0) {
		md->flags |= MASTODON_MODE_ONE;
	} else if (g_ascii_strcasecmp(s, "many") == 0) {
		md->flags |= MASTODON_MODE_MANY;
	} else {
		md->flags |= MASTODON_MODE_CHAT;
	}

	if (!(md->flags & MASTODON_MODE_ONE) &&
	    !(md->flags & MASTODON_HAVE_FRIENDS)) {
		// find our account_id and store it, eventually
		mastodon_verify_credentials(ic);
	}

	/* Create the room. */
	if (md->flags & MASTODON_MODE_CHAT) {
		mastodon_groupchat_init(ic);
	}

	mastodon_initial_timeline(ic);
	mastodon_open_user_websocket(ic);

	ic->flags |= OPT_PONGS;
}

/**
 * Initiate OAuth dialog with user. A reply to the MASTODON_OAUTH_HANDLE is handled by mastodon_buddy_msg.
 */
void oauth2_init(struct im_connection *ic)
{
	struct mastodon_data *md = ic->proto_data;

	imcb_log(ic, "Starting OAuth authentication");

	/* Temporary contact, just used to receive the OAuth response. */
	imcb_add_buddy(ic, MASTODON_OAUTH_HANDLE, NULL);

	char *url = oauth2_url(md->oauth2_service);
	char *msg = g_strdup_printf("Open this URL in your browser to authenticate: %s", url);
	imcb_buddy_msg(ic, MASTODON_OAUTH_HANDLE, msg, 0, 0);

	g_free(msg);
	g_free(url);

	imcb_buddy_msg(ic, MASTODON_OAUTH_HANDLE, "Respond to this message with the returned "
	               "authorization token.", 0, 0);

	ic->flags |= OPT_SLOW_LOGIN;
}

int oauth2_refresh(struct im_connection *ic, const char *refresh_token);

static void mastodon_login(account_t * acc)
{
	struct im_connection *ic = imcb_new(acc);
	struct mastodon_data *md = g_new0(struct mastodon_data, 1);
	url_t url;

	imcb_log(ic, "Login");

	mastodon_connections = g_slist_append(mastodon_connections, ic);
	ic->proto_data = md;
	md->user = g_strdup(acc->user);

	if (!url_set(&url, set_getstr(&ic->acc->set, "base_url"))) {
		imcb_error(ic, "Cannot parse API base URL: %s", set_getstr(&ic->acc->set, "base_url"));
		imc_logout(ic, FALSE);
		return;
	}
	if (url.proto != PROTO_HTTPS) {
		imcb_error(ic, "API base URL must use HTTPS: %s", set_getstr(&ic->acc->set, "base_url"));
		imc_logout(ic, FALSE);
		return;
	}

	md->url_ssl = 1;
	md->url_port = url.port;
	md->url_host = g_strdup(url.host);
 	mastodon_set_name(ic);

	GSList *p_in = NULL;
	const char *tok;

	md->oauth2_service = get_oauth2_service(ic);

	oauth_params_parse(&p_in, ic->acc->pass);

	/* If we did not have these stored, register the app and try
	 * again. We'll call oauth2_init from the callback in order to
	 * connect, eventually. */
	if (!md->oauth2_service->consumer_key || !md->oauth2_service->consumer_secret ||
	    strlen(md->oauth2_service->consumer_key) == 0 || strlen(md->oauth2_service->consumer_secret) == 0) {
		mastodon_register_app(ic);
	}
        /* If we have a refresh token, in which case any access token
	   we *might* have has probably expired already anyway.
	   Refresh and connect. */
	else if ((tok = oauth_params_get(&p_in, "refresh_token"))) {
		oauth2_refresh(ic, tok);
	}
	/* If we don't have a refresh token, let's hope the access
	   token is still usable. */
	else if ((tok = oauth_params_get(&p_in, "access_token"))) {
		md->oauth2_access_token = g_strdup(tok);
		mastodon_connect(ic);
	}
	/* If we don't have any, start the OAuth process now. */
	else {
		oauth2_init(ic);
	}
	/* All of the above will end up calling mastodon_connect(). */

	oauth_params_free(&p_in);
}

/**
 * Logout method. Just free the mastodon_data.
 */
static void mastodon_logout(struct im_connection *ic)
{
	struct mastodon_data *md = ic->proto_data;

	// Set the status to logged out.
	ic->flags &= ~OPT_LOGGED_IN;

	if (md) {
		if (md->timeline_gc) {
			imcb_chat_free(md->timeline_gc);
		}

		GSList *l;

		for (l = md->websockets; l; l = l->next) {
			struct mastodon_websocket *mw = l->data;
			mw_free(mw);
		}

		g_slist_free(md->websockets); md->websockets = NULL;

		if (md->log) {
			/* When mastodon_connect hasn't been called, yet, such as when imc_logout is being called from
			 * mastodon_login, the log hasn not yet been initialised. */
			int i;
			for (i = 0; i < MASTODON_LOG_LENGTH; i++) {
				g_slist_free_full(md->log[i].mentions, g_free); md->log[i].mentions = NULL;
				g_free(md->log[i].spoiler_text);
			}
			g_free(md->log); md->log = NULL;
		}

		mastodon_filters_destroy(md);

		g_slist_free_full(md->mentions, g_free); md->mentions = NULL;
		g_free(md->last_spoiler_text); md->last_spoiler_text = NULL;
		g_free(md->spoiler_text); md->spoiler_text = NULL;

		os_free(md->oauth2_service); md->oauth2_service = NULL;
		g_free(md->user); md->user = NULL;
		g_free(md->name); md->name = NULL;
		g_free(md->next_url); md->next_url = NULL;
		g_free(md->url_host); md->url_host = NULL;
		g_free(md);
		ic->proto_data = NULL;
	}

	mastodon_connections = g_slist_remove(mastodon_connections, ic);
}

/**
 * When the user replies to the MASTODON_OAUTH_HANDLE with a refresh token we request the access token and this is where
 * we get it. Save both in our settings and proceed to mastodon_connect.
 */
void oauth2_got_token(gpointer data, const char *access_token, const char *refresh_token, const char *error)
{
	struct im_connection *ic = data;
	struct mastodon_data *md;
	GSList *auth = NULL;

	if (g_slist_find(mastodon_connections, ic) == NULL) {
		return;
	}

	md = ic->proto_data;

	if (access_token == NULL) {
		imcb_error(ic, "OAuth failure (%s)", error);
		imc_logout(ic, TRUE);
		return;
	}

	oauth_params_parse(&auth, ic->acc->pass);
	if (refresh_token) {
		oauth_params_set(&auth, "refresh_token", refresh_token);
	}
	if (access_token) {
		oauth_params_set(&auth, "access_token", access_token);
	}

	g_free(ic->acc->pass);
	ic->acc->pass = oauth_params_string(auth);
	oauth_params_free(&auth);

	g_free(md->oauth2_access_token);
	md->oauth2_access_token = g_strdup(access_token);

	mastodon_connect(ic);
}

static gboolean oauth2_remove_contact(gpointer data, gint fd, b_input_condition cond)
{
	struct im_connection *ic = data;

	if (g_slist_find(mastodon_connections, ic)) {
		imcb_remove_buddy(ic, MASTODON_OAUTH_HANDLE, NULL);
	}
	return FALSE;
}

/**
 * Get the refresh token from the user via a reply to MASTODON_OAUTH_HANDLE in mastodon_buddy_msg.
 * Then get the access token Using the refresh token. The access token is then handled by oauth2_got_token.
 */
int oauth2_get_refresh_token(struct im_connection *ic, const char *msg)
{
	struct mastodon_data *md = ic->proto_data;
	char *code;
	int ret;

	imcb_log(ic, "Requesting OAuth access token");

	/* Don't do it here because the caller may get confused if the contact
	   we're currently sending a message to is deleted. */
	b_timeout_add(1, oauth2_remove_contact, ic);

	code = g_strdup(msg);
	g_strstrip(code);
	ret = oauth2_access_token(md->oauth2_service, OAUTH2_AUTH_CODE,
	                          code, oauth2_got_token, ic);

	g_free(code);
	return ret;
}

int oauth2_refresh(struct im_connection *ic, const char *refresh_token)
{
	struct mastodon_data *md = ic->proto_data;

	return oauth2_access_token(md->oauth2_service, OAUTH2_AUTH_REFRESH,
	                           refresh_token, oauth2_got_token, ic);
}

/**
 * Post a message. Make sure we get all the meta data for the status right.
 */
static void mastodon_post_message(struct im_connection *ic, char *message, guint64 in_reply_to,
				  char *who, mastodon_message_t type, GSList *mentions, mastodon_visibility_t visibility,
				  char *spoiler_text)
{
	struct mastodon_data *md = ic->proto_data;
	char *text = NULL;
	GString *m = NULL;
	int wlen;
	char *s;

	switch (type) {
	case MASTODON_DIRECT:
		visibility = MV_DIRECT;
		// fall through
	case MASTODON_REPLY:
		/* Mentioning OP and other mentions is the traditional thing to do. Note that who can be NULL if we're
		   redoing a command like "redo 1234567 foo" where we didn't get any user info from the status id. */
		if (!who) break;
		if (g_ascii_strcasecmp(who, md->user) == 0) {
			/* if replying to ourselves, we still want to mention others, if any */
			m = mastodon_account_join(mentions, NULL);
		} else {
			/* if replying to others, mention them, too */
			m = mastodon_account_join(mentions, who);
		}
		if (m) {
			text = g_strdup_printf("%s %s", m->str, message);
			g_string_free(m, TRUE);
		}
		/* Note that visibility and spoiler_text have already been set, no need to do anything else. */
		break;
	case MASTODON_NEW_MESSAGE:
		visibility = md->visibility;
		/* Note that at the end we will use the default visibility if this is NULL. */
		break;
	case MASTODON_MAYBE_REPLY:
		{
			g_assert(visibility == MV_UNKNOWN);
			wlen = strlen(who); // length of the first word

			// If the message starts with "nick:" or "nick,"
			if (who && wlen && strncmp(who, message, wlen) == 0 &&
			    (s = message + wlen - 1) && (*s == ':' || *s == ',')) {

				// Trim punctuation from who.
				who[wlen - 1] = '\0';

				// Determine what we are replying to.
				bee_user_t *bu;
				if ((bu = bee_user_by_handle(ic->bee, ic, who))) {
					struct mastodon_user_data *mud = bu->data;

					if (time(NULL) < mud->last_time + set_getint(&ic->acc->set, "auto_reply_timeout")) {
						// this is a reply
						in_reply_to = mud->last_id;
						// We're always replying to at least one person. bu->handle is fully qualified unlike who
						m = mastodon_account_join(mud->mentions, bu->handle);
						visibility = mud->visibility;
						spoiler_text = mud->spoiler_text;
					} else {
						// this is a new message but we still need to prefix the @ and use bu->handle instead of who
						m = g_string_new("@");
						g_string_append(m, bu->handle);
					}

					// use +wlen+1 to remove "nick: " (note the space) from message
					text = g_strdup_printf("%s %s", m->str, message + wlen + 1);
					g_string_free(m, TRUE);

				} else if (g_ascii_strcasecmp(who, md->user) == 0) {
					/* Compare case-insensitively because this is user input. */

					/* Same as a above but replying to myself and therefore using mastodon_data
					   (md). We don't set this data to NULL because we might want to send multiple
					   replies to ourselves. We want this to work on a slow instance, so the user
					   can send multiple replies without having to wait for replies to come back and
					   set these values again via mastodon_http_callback. */
					in_reply_to = md->last_id;
					visibility = md->last_visibility;
					spoiler_text = g_strdup(md->last_spoiler_text);
					if (md->mentions) {
						m = mastodon_account_join(md->mentions, NULL);
						mastodon_log(ic, "Mentions %s", m->str);
						text = g_strdup_printf("%s %s", m->str, message + wlen + 1);
						g_string_free(m, TRUE);
					} else {
						// use +wlen+1 to remove "nick: " (note the space) from message
						message += wlen + 1;
					}
				}
			}
		}
		break;
	}

	if (!mastodon_length_check(ic, text ? text : message,
			     md->spoiler_text ? md->spoiler_text : spoiler_text)) {
		goto finish;
	}

	/* If we explicitly set a visibility for the next toot, use that. Otherwise, use the visibility as determined above,
	 * but make sure that a higher default visibility takes precedence: higher means more private. See
	 * mastodon_visibility_t. */
	if (md->visibility != MV_UNKNOWN) {
		visibility = md->visibility;
	} else {
		mastodon_visibility_t default_visibility = mastodon_default_visibility(ic);
		if (default_visibility > visibility) visibility = default_visibility;
	}

	/* md->spoiler_text set by the CW command and md->visibility set by the VISIBILITY command take precedence and get
	 * removed after posting. */
	mastodon_post_status(ic, text ? text : message, in_reply_to, visibility,
			     md->spoiler_text ? md->spoiler_text : spoiler_text);
	g_free(md->spoiler_text); md->spoiler_text = NULL;
	md->visibility = MV_UNKNOWN;

finish:
	g_free(text);
	g_free(spoiler_text);
}

static void mastodon_handle_command(struct im_connection *ic, char *message, mastodon_undo_t undo_type);

/**
 * Send a direct message. If this buddy is the magic mastodon oauth handle, then treat the message as the refresh token.
 * If this buddy is me, then treat the message as a command. Everything else is a message to a buddy in a query.
 */
static int mastodon_buddy_msg(struct im_connection *ic, char *who, char *message, int away)
{
	struct mastodon_data *md = ic->proto_data;

	/* OAuth message to "mastodon_oauth" */
	if (g_ascii_strcasecmp(who, MASTODON_OAUTH_HANDLE) == 0 &&
	    !(md->flags & OPT_LOGGED_IN)) {

		if (oauth2_get_refresh_token(ic, message)) {
			return 1;
		} else {
			imcb_error(ic, "OAuth failure");
			imc_logout(ic, TRUE);
			return 0;
		}
	}

	if (g_ascii_strcasecmp(who, md->name) == 0) {
		/* Message to ourselves */
		mastodon_handle_command(ic, message, MASTODON_NEW);
	} else {
		/* Determine who and to what post id we are replying to */
		guint64 in_reply_to = 0;
		bee_user_t *bu;
		if ((bu = bee_user_by_handle(ic->bee, ic, who))) {
			struct mastodon_user_data *mud = bu->data;
			if (time(NULL) < mud->last_direct_time + set_getint(&ic->acc->set, "auto_reply_timeout")) {
				/* this is a reply */
				in_reply_to = mud->last_direct_id;
			}
		}
		mastodon_post_message(ic, message, in_reply_to, who, MASTODON_REPLY, NULL, MV_DIRECT, NULL);
	}
	return 0;
}

static void mastodon_user(struct im_connection *ic, char *who);

static void mastodon_get_info(struct im_connection *ic, char *who)
{
	struct mastodon_data *md = ic->proto_data;
	struct irc_channel *ch = md->timeline_gc->ui_data;

	imcb_log(ic, "Sending output to %s", ch->name);
	if (g_ascii_strcasecmp(who, md->name) == 0) {
		mastodon_instance(ic);
	} else {
		mastodon_user(ic, who);
	}
}

static void mastodon_chat_msg(struct groupchat *c, char *message, int flags)
{
	if (c && message) {
		mastodon_handle_command(c->ic, message, MASTODON_NEW);
	}
}

/**
 * Joining a group chat means showing the appropriate timeline and connecting a websocket.
 */
static struct groupchat *mastodon_chat_join(struct im_connection *ic,
                                           const char *room, const char *nick,
                                           const char *password, set_t **sets)
{
	char *topic = g_strdup(room);
	struct groupchat *c = imcb_chat_new(ic, topic);
	imcb_chat_topic(c, NULL, topic, 0);
	imcb_chat_add_buddy(c, ic->acc->user);
	struct mastodon_websocket *mw = NULL;
	if (strcmp(topic, "local") == 0) {
		mastodon_local_timeline(ic);
		mw = mastodon_open_local_websocket(ic);
	} else if (strcmp(topic, "federated") == 0) {
		mastodon_federated_timeline(ic);
		mw = mastodon_open_federated_websocket(ic);
	} else if (topic[0] == '#') {
		mastodon_hashtag_timeline(ic, topic + 1);
		mw = mastodon_open_hashtag_websocket(ic, topic + 1);
	} else {
		/* After the initial login we cannot be sure that an initial list timeline will work because the lists are not
		   loaded, yet. That's why mastodon_following() will end up reloading the lists with the extra parameter which
		   will load these timelines. If we're creating this channel at a later point, however, this should be possible.
		   One way to determine if we're "at a later point" is by looking at MASTODON_HAVE_FRIENDS. It's actually not
		   quite correct: at this point we have the lists but not the list members, but it should be good enough as
		   we're only interested in later chat joining, not auto_join. */
		struct mastodon_data *md = ic->proto_data;
		if (md->flags & MASTODON_HAVE_FRIENDS) {
			mastodon_unknown_list_timeline(ic, topic);
		}
		/* We need to identify the list to connect the websocket but we don't get a websocket on the return from
		   mastodon_open_unknown_list. Instead, we pass the channel along and when we have the list, the
		   websocket will be set accordingly. */
		mastodon_open_unknown_list(ic, c, topic);
	}
	g_free(topic);
	c->data = mw;
	return c;
}

/**
 * If the user leaves the main channel: Fine. Rejoin him/her once new toots come in. If the user leaves a channel that
 * is connected to a websocket, we need to find the websocket and close it, too.
 */
static void mastodon_chat_leave(struct groupchat *c)
{
	GSList *l;
	struct mastodon_data *md = c->ic->proto_data;

	if (c == md->timeline_gc) {
		md->timeline_gc = NULL;
	}

	for (l = md->websockets; l; l = l->next) {
		if (c->data == l->data) {
			struct mastodon_websocket *mw = l->data;
			md->websockets = g_slist_remove(md->websockets, mw);
			mw_free(mw);
			break;
		}
	}

	imcb_chat_free(c);
}

static void mastodon_add_permit(struct im_connection *ic, char *who)
{
}

static void mastodon_rem_permit(struct im_connection *ic, char *who)
{
}

static void mastodon_buddy_data_add(bee_user_t *bu)
{
	bu->data = g_new0(struct mastodon_user_data, 1);
}

static void mastodon_buddy_data_free(bee_user_t *bu)
{
	struct mastodon_user_data *mud = (struct mastodon_user_data*) bu->data;
	g_slist_free_full(mud->lists, g_free); mud->lists = NULL;
	g_slist_free_full(mud->mentions, g_free); mud->mentions = NULL;
	g_free(mud->spoiler_text); mud->spoiler_text = NULL;
	g_free(bu->data);
}

bee_user_t mastodon_log_local_user;

/**
 * Find a user account based on their nick name.
 */
static bee_user_t *mastodon_user_by_nick(struct im_connection *ic, char *nick)
{
	GSList *l;
	for (l = ic->bee->users; l; l = l->next) {
		bee_user_t *bu = l->data;
		irc_user_t *iu = bu->ui_data;
		if (g_ascii_strcasecmp(iu->nick, nick) == 0) {
			/* Compare case-insentively because this is user input. */
			return bu;
		}
	}
	return NULL;
}

/**
 * Convert the given bitlbee toot ID or bitlbee username into a
 * mastodon status ID and returns it. If provided with a pointer to a
 * bee_user_t, fills that as well. Provide NULL if you don't need it.
 * The same is true for mentions, visibility and spoiler text.
 *
 * Returns 0 if the user provides garbage.
 */
static guint64 mastodon_message_id_from_command_arg(struct im_connection *ic, char *arg, bee_user_t **bu_,
						    GSList **mentions_, mastodon_visibility_t *visibility_,
						    char **spoiler_text_)
{
	struct mastodon_data *md = ic->proto_data;
	struct mastodon_user_data *mud;
	bee_user_t *bu = NULL;
	guint64 id = 0;

	if (bu_) {
		*bu_ = NULL;
	}
	if (!arg || !arg[0]) {
		return 0;
	}

	if (arg[0] != '#' && (bu = mastodon_user_by_nick(ic, arg))) {
		if ((mud = bu->data)) {
			id = mud->last_id;
			if (mentions_) *mentions_ = mud->mentions;
			if (visibility_) *visibility_ = mud->visibility;
			if (spoiler_text_) *spoiler_text_ = mud->spoiler_text;
		}
	} else {
		if (arg[0] == '#') {
			arg++;
		}
		if (parse_int64(arg, 16, &id) && id < MASTODON_LOG_LENGTH) {
			if (mentions_) *mentions_ = md->log[id].mentions;
			if (visibility_) *visibility_ = md->log[id].visibility;
			if (spoiler_text_) *spoiler_text_ = md->log[id].spoiler_text;
			bu = md->log[id].bu;
			id = md->log[id].id;
		} else if (parse_int64(arg, 10, &id)) {
			/* Allow normal toot IDs as well. Required do undo posts, for example. */
		} else {
			/* Reset id if id was a valid hex number but >= MASTODON_LOG_LENGTH. */
			id = 0;
		}
	}
	if (bu_) {
		if (bu == &mastodon_log_local_user) {
			/* HACK alert. There's no bee_user object for the local
			 * user so just fake one for the few cmds that need it. */
			mastodon_log_local_user.handle = md->user;
		} else {
			/* Beware of dangling pointers! */
			if (!g_slist_find(ic->bee->users, bu)) {
				bu = NULL;
			}
		}
		*bu_ = bu;
	}
	return id;
}

static void mastodon_no_id_warning(struct im_connection *ic, char *what)
{
	mastodon_log(ic, "User or status '%s' is unknown.", what);
}

static void mastodon_unknown_user_warning(struct im_connection *ic, char *who)
{
	mastodon_log(ic, "User '%s' is unknown.", who);
}

/**
 * Get the message id given a nick or a status id. If possible, also set a number of other variables by reference.
 */
static guint64 mastodon_message_id_or_warn_and_more(struct im_connection *ic, char *what, bee_user_t **bu,
					   GSList **mentions, mastodon_visibility_t *visibility, char **spoiler_text)
{
	guint64 id = mastodon_message_id_from_command_arg(ic, what, bu, mentions, visibility, spoiler_text);
	if (!id) {
		mastodon_no_id_warning(ic, what);
	}
	return id;
}

/**
 * Simple interface to mastodon_message_id_or_warn_and_more. Get the message id given a nick or a status id.
 */
static guint64 mastodon_message_id_or_warn(struct im_connection *ic, char *what)
{
	return mastodon_message_id_or_warn_and_more(ic, what, NULL, NULL, NULL, NULL);
}

static guint64 mastodon_account_id(bee_user_t *bu) {
	struct mastodon_user_data *mud;
	if (bu != NULL && (mud = bu->data)) {
		return mud->account_id;
	}
	return 0;
}

static guint64 mastodon_user_id_or_warn(struct im_connection *ic, char *who)
{
	bee_user_t *bu;
	guint64 id;
	if ((bu = mastodon_user_by_nick(ic, who)) &&
	    (id = mastodon_account_id(bu))) {
		return id;
	} else if (parse_int64(who, 10, &id)) {
		return id;
	}
	mastodon_unknown_user_warning(ic, who);
	return 0;
}

static void mastodon_user(struct im_connection *ic, char *who)
{
	bee_user_t *bu;
	guint64 id;
	if ((bu = mastodon_user_by_nick(ic, who)) &&
	    (id = mastodon_account_id(bu))) {
		mastodon_account(ic, id);
	} else {
		mastodon_search_account(ic, who);
	}
}

static void mastodon_relation_to_user(struct im_connection *ic, char *who)
{
	bee_user_t *bu;
	guint64 id;
	if ((bu = mastodon_user_by_nick(ic, who)) &&
	    (id = mastodon_account_id(bu))) {
		mastodon_relationship(ic, id);
	} else {
		mastodon_search_relationship(ic, who);
	}
}

static void mastodon_add_buddy(struct im_connection *ic, char *who, char *group)
{
	bee_user_t *bu;
	guint64 id;
	if ((bu = mastodon_user_by_nick(ic, who)) &&
	    (id = mastodon_account_id(bu))) {
		// If the nick is already in the channel (when we just
		// unfollowed them, for example), we're taking a
		// shortcut. No fancy looking at the relationship and
		// all that. The nick is already here, after all.
		mastodon_post(ic, MASTODON_ACCOUNT_FOLLOW_URL, MC_FOLLOW, id);
	} else if (parse_int64(who, 10, &id)) {
		// If we provided a numerical id, then that will also
		// work. This is used by redo/undo.
		mastodon_post(ic, MASTODON_ACCOUNT_FOLLOW_URL, MC_FOLLOW, id);
	} else {
		// Alternatively, we're looking for an unknown user.
		// They must be searched, followed, and added to the
		// channel. It's going to take more requests.
		mastodon_follow(ic, who);
	}
}

static void mastodon_remove_buddy(struct im_connection *ic, char *who, char *group)
{
	guint64 id;
	if ((id = mastodon_user_id_or_warn(ic, who))) {
		mastodon_post(ic, MASTODON_ACCOUNT_UNFOLLOW_URL, MC_UNFOLLOW, id);
	}
}

static void mastodon_add_deny(struct im_connection *ic, char *who)
{
	guint64 id;
	if ((id = mastodon_user_id_or_warn(ic, who))) {
		mastodon_post(ic, MASTODON_ACCOUNT_BLOCK_URL, MC_BLOCK, id);
	}
}

static void mastodon_rem_deny(struct im_connection *ic, char *who)
{
	guint64 id;
	if ((id = mastodon_user_id_or_warn(ic, who))) {
		mastodon_post(ic, MASTODON_ACCOUNT_UNBLOCK_URL, MC_UNBLOCK, id);
	}
}

/**
 * Add a command and a way to undo it to the undo stack. Remember that
 * only the callback knows whether a command succeeded or not, and
 * what the id of a newly posted status is, and all that. Thus,
 * there's a delay that we need to take into account.
 *
 * The stack is organized as follows if we just did D:
 *           0 1 2 3 4 5 6 7 8 9
 *   undo = [a b c d e f g h i j]
 *   redo = [A B C D E F G H I J]
 *   first_undo = 3
 *   current_undo = 3
 * If we do X:
 *   undo = [a b c d x f g h i j]
 *   redo = [A B C D X F G H I J]
 *   first_undo = 4
 *   current_undo = 4
 * If we undo it, send x and:
 *   undo = [a b c d x f g h i j]
 *   redo = [A B C D X F G H I J]
 *   first_undo = 4
 *   current_undo = 3
 * If we redo, send X and increase current_undo.
 * If we undo instead, send d and decrease current_undo again:
 *   undo = [a b c d x f g h i j]
 *   redo = [A B C D X F G H I J]
 *   first_undo = 4
 *   current_undo = 2
 * If we do Y with current_undo different from first_undo, null the tail:
 *  undo = [a b c y 0 f g h i j]
 *  redo = [A B C Y 0 F G H I J]
 *  first_undo = 3
 *  current_undo = 3
 */
void mastodon_do(struct im_connection *ic, char *redo, char *undo) {
	struct mastodon_data *md = ic->proto_data;
	int i = (md->current_undo + 1) % MASTODON_MAX_UNDO;

	g_free(md->redo[i]);
	g_free(md->undo[i]);
	md->redo[i] = redo;
	md->undo[i] = undo;

	if (md->current_undo == md->first_undo) {
		md->current_undo = md->first_undo = i;
	} else {
		md->current_undo = i;
		int end = (md->first_undo + 1) % MASTODON_MAX_UNDO;
		for (i = (md->current_undo + 1) % MASTODON_MAX_UNDO; i != end; i = (i + 1) % MASTODON_MAX_UNDO) {
			g_free(md->redo[i]);
			g_free(md->undo[i]);
			md->redo[i] = NULL;
			md->undo[i] = NULL;
		}

		md->first_undo = md->current_undo;
	}
}

/**
 * Undo the last command.
 */
void mastodon_undo(struct im_connection *ic) {
	struct mastodon_data *md = ic->proto_data;
	char *cmd = md->undo[md->current_undo];

	if (!cmd) {
		mastodon_log(ic, "There is nothing to undo.");
		return;
	}

	gchar **cmds = g_strsplit (cmd, FS, -1);

	int i;
	for (i = 0; cmds[i]; i++) {
		mastodon_handle_command(ic, cmds[i], MASTODON_UNDO);
	}

	g_strfreev(cmds);

	// beware of negatives and modulo
	md->current_undo = (md->current_undo + MASTODON_MAX_UNDO - 1) % MASTODON_MAX_UNDO;
}

/**
 * Redo the last command. Multiple commands can be executed as one using the ASCII Field Separator (FS).
 */
void mastodon_redo(struct im_connection *ic) {
	struct mastodon_data *md = ic->proto_data;

	if (md->current_undo == md->first_undo) {
		mastodon_log(ic, "There is nothing to redo.");
		return;
	}

	md->current_undo = (md->current_undo + 1) % MASTODON_MAX_UNDO;
	char *cmd = md->redo[md->current_undo];

	gchar **cmds = g_strsplit (cmd, FS, -1);

	int i;
	for (i = 0; cmds[i]; i++) {
		mastodon_handle_command(ic, cmds[i], MASTODON_REDO);
	}

	g_strfreev(cmds);
}

/**
 * Update the current command in the stack. This is necessary when
 * executing commands which change references that we saved. For
 * example: every delete statement refers to an id. Whenever a post
 * happens because of redo, the delete command in the undo stack has
 * to be replaced. Whenever a post happens because of undo, the delete
 * command in the redo stack has to be replaced.
 *
 * We make our own copies of 'to'.
 */
void mastodon_do_update(struct im_connection *ic, char *to)
{
	struct mastodon_data *md = ic->proto_data;
	char *from = NULL;
	int i;

	switch (md->undo_type) {
	case MASTODON_NEW:
		// should not happen
		return;
	case MASTODON_UNDO:
		// after post due to undo of a delete statement, the
		// old delete statement is in the next redo element
		i = (md->current_undo + 1) % MASTODON_MAX_UNDO;
		from = g_strdup(md->redo[i]);
		break;
	case MASTODON_REDO:
		// after post due to redo of a post statement, the
		// old delete statement is in the undo element
		i = md->current_undo;
		from = g_strdup(md->undo[i]);
		break;
	}

	/* After a post and a delete of that post, there are at least
	 * two cells where the old reference can be hiding (undo of
	 * the post and redo of the delete). Brute force! */
	for (i = 0; i < MASTODON_MAX_UNDO; i++) {
		if (md->undo[i] && strcmp(from, md->undo[i]) == 0) {
			g_free(md->undo[i]);
			md->undo[i] = g_strdup(to);
			break;
		}
	}
	for (i = 0; i < MASTODON_MAX_UNDO; i++) {
		if (md->redo[i] && strcmp(from, md->redo[i]) == 0) {
			g_free(md->redo[i]);
			md->redo[i] = g_strdup(to);
			break;
		}
	}

	g_free(from);
}

/**
 * Show the current history. The history shows the redo
 * commands.
 */
void mastodon_history(struct im_connection *ic, gboolean undo_history) {
	struct mastodon_data *md = ic->proto_data;
	int i;
	for (i = 0; i < MASTODON_MAX_UNDO; i++) {
		// start with the last
		int n = (md->first_undo + i + 1) % MASTODON_MAX_UNDO;
		char *cmd = undo_history ? md->undo[n] : md->redo[n];

		if (cmd) {
			gchar **cmds = g_strsplit (cmd, FS, -1);

			int j;
			for (j = 0; cmds[j]; j++) {
				if (n == md->current_undo) {
					mastodon_log(ic, "%02d > %s", MASTODON_MAX_UNDO - i, cmds[j]);
				} else {
					mastodon_log(ic, "%02d %s", MASTODON_MAX_UNDO - i, cmds[j]);
				}
			}

			g_strfreev(cmds);
		}
	}
}

/**
 * Commands we understand. Changes should be documented in
 * doc/mastodon-help.txt and on https://wiki.bitlbee.org/HowtoMastodon
 */
static void mastodon_handle_command(struct im_connection *ic, char *message, mastodon_undo_t undo_type)
{
	struct mastodon_data *md = ic->proto_data;
	gboolean allow_post = g_ascii_strcasecmp(set_getstr(&ic->acc->set, "commands"), "strict") != 0;
	bee_user_t *bu = NULL;
	guint64 id;

	md->undo_type = undo_type;

	char *cmds = g_strdup(message);
	char **cmd = split_command_parts(cmds, 2);

	if (cmd[0] == NULL) {
		/* Nothing to do */
	} else if (!set_getbool(&ic->acc->set, "commands") && allow_post) {
		/* Not supporting commands if "commands" is set to true/strict. */
	} else if (g_ascii_strcasecmp(cmd[0], "help") == 0) {
		/* For unsupported undo and redo commands. */
		mastodon_log(ic, "Please use help mastodon in the control channel, &bitlbee.");
	} else if (g_ascii_strcasecmp(cmd[0], "info") == 0) {
		if (!cmd[1]) {
			mastodon_log(ic, "Usage:\n"
				     "- info instance\n"
				     "- info [id|screenname]\n"
				     "- info user [nick|account]\n"
				     "- info relation [nick|account]\n"
				     "- info [get|put|post|delete] url [args]");
		} else if (g_ascii_strcasecmp(cmd[1], "instance") == 0) {
			mastodon_instance(ic);
		} else if (g_ascii_strcasecmp(cmd[1], "user") == 0) {
			if (cmd[2]) {
				mastodon_user(ic, cmd[2]);
			} else {
				mastodon_log(ic, "User info about whom?");
			}
		} else if (g_ascii_strcasecmp(cmd[1], "relation") == 0) {
			if (cmd[2]) {
				mastodon_relation_to_user(ic, cmd[2]);
			} else {
				mastodon_log(ic, "Relation with whom?");
			}
		} else if ((id = mastodon_message_id_or_warn(ic, cmd[1]))) {
			mastodon_status(ic, id);
		}
	} else if (g_ascii_strcasecmp(cmd[0], "api") == 0) {
		if (!cmd[1] || !cmd[2]) {
			mastodon_log(ic, "Usage: api [get|put|post|delete] endpoint params...\n"
						 "Example: api post /lists/12/accounts account_ids[] 321");
		} else if ((g_ascii_strcasecmp(cmd[1], "get") == 0 ||
					g_ascii_strcasecmp(cmd[1], "put") == 0 ||
					g_ascii_strcasecmp(cmd[1], "post") == 0 ||
					g_ascii_strcasecmp(cmd[1], "delete") == 0) && cmd[2]) {
			char *s = strstr(cmd[2], " ");
			if (s) {
				*s = '\0';
				char **args = g_strsplit(s+1, " ", 0);
				/* find length of null-terminated vector */
				int i = 0;
				for (; args[i]; i++);
				if (i % 2) {
					mastodon_log(ic, "Wrong number of arguments. Did you forget the URL?");
				} else {
					mastodon_raw(ic, cmd[1], cmd[2], args, i);
				}
				g_strfreev(args);
			} else {
				mastodon_raw(ic, cmd[1], cmd[2], NULL, 0);
			}
		} else {
			mastodon_log(ic, "Usage: 'api [get|put|post|delete] url [name value]*");
		}
	} else if (g_ascii_strcasecmp(cmd[0], "undo") == 0) {
		if (cmd[1] == NULL) {
			mastodon_undo(ic);
		} else {
			// because it used to take an argument
			mastodon_log(ic, "Undo takes no arguments.");
		}
	} else if (g_ascii_strcasecmp(cmd[0], "redo") == 0) {
		if (cmd[1] == NULL) {
			mastodon_redo(ic);
		} else {
			mastodon_log(ic, "Redo takes no arguments.");
		}
	} else if (g_ascii_strcasecmp(cmd[0], "his") == 0 ||
		   g_ascii_strcasecmp(cmd[0], "history") == 0) {
		if (cmd[1] && g_ascii_strcasecmp(cmd[1], "undo") == 0) {
			mastodon_history(ic, TRUE);
		} else if (cmd[1] == NULL) {
			mastodon_history(ic, FALSE);
		} else {
			mastodon_log(ic, "History only takes the optional undo argument.");
		}
	} else if (g_ascii_strcasecmp(cmd[0], "del") == 0 ||
		   g_ascii_strcasecmp(cmd[0], "delete") == 0) {
		if (cmd[1] == NULL && md->last_id) {
			mastodon_status_delete(ic, md->last_id);
		} else if (cmd[1] && (id = mastodon_message_id_from_command_arg(ic, cmd[1], NULL, NULL, NULL, NULL))) {
			mastodon_status_delete(ic, id);
		} else {
			mastodon_log(ic, "Could not delete the last post.");
		}
	} else if ((g_ascii_strcasecmp(cmd[0], "favourite") == 0 ||
	            g_ascii_strcasecmp(cmd[0], "favorite") == 0 ||
	            g_ascii_strcasecmp(cmd[0], "fav") == 0 ||
	            g_ascii_strcasecmp(cmd[0], "like") == 0)) {
		if (cmd[1] && (id = mastodon_message_id_or_warn(ic, cmd[1]))) {
			mastodon_post(ic, MASTODON_STATUS_FAVOURITE_URL, MC_FAVOURITE, id);
		} else {
			mastodon_log(ic, "Huh? Please provide a log number or nick.");
		}
	} else if ((g_ascii_strcasecmp(cmd[0], "unfavourite") == 0 ||
	            g_ascii_strcasecmp(cmd[0], "unfavorite") == 0 ||
	            g_ascii_strcasecmp(cmd[0], "unfav") == 0 ||
	            g_ascii_strcasecmp(cmd[0], "unlike") == 0 ||
	            g_ascii_strcasecmp(cmd[0], "dislike") == 0)) {
		if (cmd[1] && (id = mastodon_message_id_or_warn(ic, cmd[1]))) {
			mastodon_post(ic, MASTODON_STATUS_UNFAVOURITE_URL, MC_UNFAVOURITE, id);
		} else {
			mastodon_log(ic, "What? Please provide a log number or nick.");
		}
	} else if (g_ascii_strcasecmp(cmd[0], "pin") == 0) {
		if (cmd[1] && (id = mastodon_message_id_or_warn(ic, cmd[1]))) {
			mastodon_post(ic, MASTODON_STATUS_PIN_URL, MC_PIN, id);
		} else {
			mastodon_log(ic, "Sorry, what? Please provide a log number or nick.");
		}
	} else if (g_ascii_strcasecmp(cmd[0], "unpin") == 0) {
		if (cmd[1] && (id = mastodon_message_id_or_warn(ic, cmd[1]))) {
			mastodon_post(ic, MASTODON_STATUS_UNPIN_URL, MC_UNPIN, id);
		} else {
			mastodon_log(ic, "No can do! I need a a log number or nick.");
		}
	} else if (g_ascii_strcasecmp(cmd[0], "follow") == 0) {
		if (cmd[1]) {
			mastodon_add_buddy(ic, cmd[1], NULL);
		} else {
			mastodon_log(ic, "I'm confused! Follow whom?");
		}
	} else if (g_ascii_strcasecmp(cmd[0], "unfollow") == 0) {
		if (cmd[1]) {
			mastodon_remove_buddy(ic, cmd[1], NULL);
		} else {
			mastodon_log(ic, "Unfollow whom?");
		}
	} else if (g_ascii_strcasecmp(cmd[0], "block") == 0) {
		if (cmd[1]) {
			mastodon_add_deny(ic, cmd[1]);
		} else {
			mastodon_log(ic, "Whom should I block?");
		}
	} else if (g_ascii_strcasecmp(cmd[0], "unblock") == 0 ||
			   g_ascii_strcasecmp(cmd[0], "allow") == 0) {
		if (cmd[1]) {
			mastodon_rem_deny(ic, cmd[1]);
		} else {
			mastodon_log(ic, "Unblock who?");
		}
	} else if (g_ascii_strcasecmp(cmd[0], "mute") == 0 &&
			   g_ascii_strcasecmp(cmd[1], "user") == 0) {
		if (cmd[2] && (id = mastodon_user_id_or_warn(ic, cmd[2]))) {
			mastodon_post(ic, MASTODON_ACCOUNT_MUTE_URL, MC_ACCOUNT_MUTE, id);
		} else {
			mastodon_log(ic, "Mute user? I also need a nick!");
		}
	} else if (g_ascii_strcasecmp(cmd[0], "unmute") == 0 &&
			   g_ascii_strcasecmp(cmd[1], "user") == 0) {
		if (cmd[2] && (id = mastodon_user_id_or_warn(ic, cmd[2]))) {
			mastodon_post(ic, MASTODON_ACCOUNT_UNMUTE_URL, MC_ACCOUNT_UNMUTE, id);
		} else {
			mastodon_log(ic, "Sure, unmute a user. But who is it? Give me a nick!");
		}
	} else if (g_ascii_strcasecmp(cmd[0], "mute") == 0) {
		if (cmd[1] && (id = mastodon_message_id_or_warn(ic, cmd[1]))) {
			mastodon_post(ic, MASTODON_STATUS_MUTE_URL, MC_STATUS_MUTE, id);
		} else {
			mastodon_log(ic, "Muting? Please provide a log number or nick!");
		}
	} else if (g_ascii_strcasecmp(cmd[0], "unmute") == 0) {
		if (cmd[1] && (id = mastodon_message_id_or_warn(ic, cmd[1]))) {
			mastodon_post(ic, MASTODON_STATUS_UNMUTE_URL, MC_STATUS_UNMUTE, id);
		} else {
			mastodon_log(ic, "OK, I'll unmute something. But what? I need a log number or nick.");
		}
	} else if (g_ascii_strcasecmp(cmd[0], "boost") == 0) {
		if (cmd[1] && (id = mastodon_message_id_or_warn(ic, cmd[1]))) {
			mastodon_post(ic, MASTODON_STATUS_BOOST_URL, MC_BOOST, id);
		} else {
			mastodon_log(ic, "Failed to boost! Please provide a log number or nick.");
		}
	} else if (g_ascii_strcasecmp(cmd[0], "unboost") == 0) {
		if (cmd[1] && (id = mastodon_message_id_or_warn(ic, cmd[1]))) {
			mastodon_post(ic, MASTODON_STATUS_UNBOOST_URL, MC_UNBOOST, id);
		} else {
			mastodon_log(ic, "Argh, #fail! Please provide a log number or nick.");
		}
	} else if (g_ascii_strcasecmp(cmd[0], "url") == 0) {
		if (cmd[1] && (id = mastodon_message_id_or_warn(ic, cmd[1]))) {
			mastodon_status_show_url(ic, id);
		} else {
			mastodon_log(ic, "This is confusing. Do you have a log number or nick?");
		}
	} else if ((g_ascii_strcasecmp(cmd[0], "whois") == 0 ||
		    g_ascii_strcasecmp(cmd[0], "who") == 0)) {
		if (!cmd[1]) {
			mastodon_log(ic, "The IRC command /names should give you a list.");
		} else if ((bu = mastodon_user_by_nick(ic, cmd[1]))) {
			mastodon_log(ic, "%s [%s]", bu->handle, bu->fullname);
		} else if ((parse_int64(cmd[1], 16, &id) && id < MASTODON_LOG_LENGTH)) {
			mastodon_show_mentions(ic, md->log[id].mentions);
		} else if ((parse_int64(cmd[1], 10, &id))) {
			mastodon_status_show_mentions(ic, id);
		} else if (g_ascii_strcasecmp(cmd[1], md->user) == 0) {
			mastodon_log(ic, "This is you!");
		} else {
			mastodon_unknown_user_warning(ic, cmd[1]);
		}
	} else if (g_ascii_strcasecmp(cmd[0], "report") == 0 ||
			   g_ascii_strcasecmp(cmd[0], "spam") == 0) {
		if (cmd[1] && (id = mastodon_message_id_or_warn(ic, cmd[1]))) {
			if (!cmd[2] || strlen(cmd[2]) == 0) {
				mastodon_log(ic, "You must provide a comment with your report.");
			} else {
				mastodon_report(ic, id, cmd[2]);
			}
		} else {
			mastodon_log(ic, "I need a log number or nick, and a comment!");
		}
	} else if (g_ascii_strcasecmp(cmd[0], "search") == 0) {
		if (cmd[1]) {
			mastodon_search(ic, cmd[1]);
		} else {
			mastodon_log(ic, "Sure, but what?");
		}
	} else if (g_ascii_strcasecmp(cmd[0], "context") == 0) {
		if (cmd[1] && (id = mastodon_message_id_or_warn(ic, cmd[1]))) {
			mastodon_context(ic, id);
		} else {
			mastodon_log(ic, "Context of what, though? Please provide a log number or nick.");
		}
	} else if (g_ascii_strcasecmp(cmd[0], "timeline") == 0) {
		if (!cmd[1] || strcmp(cmd[1], "home") == 0) {
			mastodon_home_timeline(ic);
		} else if ((bu = mastodon_user_by_nick(ic, cmd[1])) &&
				   (id = mastodon_account_id(bu))) {
			mastodon_account_statuses(ic, id);
		} else if (*cmd[1] == '#') {
			mastodon_hashtag_timeline(ic, cmd[1] + 1);
		} else if (*cmd[1] == '@') {
			mastodon_unknown_account_statuses(ic, cmd[1] + 1);
		} else if (strcmp(cmd[1], "local") == 0) {
			mastodon_local_timeline(ic);
		} else if (strcmp(cmd[1], "federated") == 0) {
			mastodon_federated_timeline(ic);
		} else {
			mastodon_unknown_list_timeline(ic, message + 9); // "timeline %s"
		}
	} else if (g_ascii_strcasecmp(cmd[0], "notifications") == 0) {
		if (cmd[1] == NULL) {
			mastodon_notifications(ic);
		} else {
			mastodon_log(ic, "Notifications takes no arguments.");
		}
	} else if (g_ascii_strcasecmp(cmd[0], "pinned") == 0) {
		if (!cmd[1]) {
			mastodon_log(ic, "Pin the void? I need a nick or an account.");
		} else if ((bu = mastodon_user_by_nick(ic, cmd[1])) &&
				   (id = mastodon_account_id(bu))) {
			mastodon_account_pinned_statuses(ic, id);
		} else {
			mastodon_unknown_account_pinned_statuses(ic, cmd[1]);
		}
	} else if (g_ascii_strcasecmp(cmd[0], "bio") == 0) {
		if (!cmd[1]) {
			mastodon_log(ic, "Bio what? Please provide a nick or an account.");
		} else if ((bu = mastodon_user_by_nick(ic, cmd[1])) &&
				   (id = mastodon_account_id(bu))) {
			mastodon_account_bio(ic, id);
		} else {
			mastodon_unknown_account_bio(ic, cmd[1]);
		}
	} else if (g_ascii_strcasecmp(cmd[0], "more") == 0) {
		if (cmd[1]) {
			mastodon_log(ic, "More takes no arguments.");
		} else if (md->next_url) {
			mastodon_more(ic);
		} else {
			mastodon_log(ic, "More of what? Use the timeline command, first.");
		}
	} else if (g_ascii_strcasecmp(cmd[0], "list") == 0) {
		if (!cmd[1]) {
			mastodon_lists(ic);
		} else if (g_ascii_strcasecmp(cmd[1], "create") == 0) {
			if (!cmd[2]) {
				mastodon_log(ic, "You forgot the title of the new list!");
			} else {
				mastodon_list_create(ic, message + 12); // "list create %s"
			}
		} else if (g_ascii_strcasecmp(cmd[1], "reload") == 0) {
			if (cmd[2]) {
				mastodon_log(ic, "List reloading takes no argument");
			} else {
				mastodon_list_reload(ic, FALSE);
			}
		} else if (g_ascii_strcasecmp(cmd[1], "delete") == 0) {
			if (!cmd[2]) {
				mastodon_log(ic, "Which list should be deleted? Use list to find out.");
			} else {
				mastodon_unknown_list_delete(ic, message + 12); // "list delete %s"
			}
		} else if (g_ascii_strcasecmp(cmd[1], "add") == 0) {
			char **args = g_strsplit(cmd[2], " to ", 2);
			if (args[0] && args[1] && (id = mastodon_user_id_or_warn(ic, args[0]))) {
				mastodon_unknown_list_add_account(ic, id, args[1]);
			} else {
				mastodon_log(ic, "I am confused. Please use list add <nick> to <list>.");
			}
			g_strfreev(args);
		} else if (g_ascii_strcasecmp(cmd[1], "remove") == 0) {
			char **args = g_strsplit(cmd[2], " from ", 2);
			if (args[0] && args[1] && (id = mastodon_user_id_or_warn(ic, args[0]))) {
				mastodon_unknown_list_remove_account(ic, id, args[1]);
			} else {
				mastodon_log(ic, "I need to what to do! Use list remove <nick> from <list>.");
			}
			g_strfreev(args);
		} else {
			mastodon_unknown_list_accounts(ic, message + 5); // "list %s"
		}
	} else if (g_ascii_strcasecmp(cmd[0], "filter") == 0) {
		if (!cmd[1]) {
			mastodon_filters(ic);
		} else if (g_ascii_strcasecmp(cmd[1], "create") == 0) {
			if (!cmd[2]) {
				mastodon_log(ic, "What do you want to filter?");
			} else {
				mastodon_filter_create(ic, message + 14); // "filter create %s"
			}
		} else if (g_ascii_strcasecmp(cmd[1], "delete") == 0) {
			if (!cmd[2]) {
				mastodon_log(ic, "Which filter should be deleted? Use filter to find out.");
			} else {
				mastodon_filter_delete(ic, cmd[2]);
			}
		} else {
			mastodon_log(ic, "I only understand the filter subcommands create and delete.");
		}
	} else if (g_ascii_strcasecmp(cmd[0], "reply") == 0) {
		if (!cmd[1] || !cmd[2]) {
			mastodon_log(ic, "Sorry, what? Please provide a log number or nick, and your reply.");
		} else {
			/* These three variables will be set, if we find the toot we are replying to in our log or in the
			 * mastodon_user_data (mud). If we are replying to a fixed id, then we'll get an id and the three variables
			 * remain untouched, so handle them with care. */
			GSList *mentions = NULL;
			char *spoiler_text = NULL;
			mastodon_visibility_t visibility = MV_UNKNOWN;
			if ((id = mastodon_message_id_or_warn_and_more(ic, cmd[1], &bu, &mentions, &visibility, &spoiler_text))) {
				mastodon_visibility_t default_visibility = mastodon_default_visibility(ic);
				if (default_visibility > visibility) visibility = default_visibility;
				char *who = bu ? bu->handle : NULL;
				mastodon_post_message(ic, cmd[2], id, who, MASTODON_REPLY, mentions, visibility, spoiler_text);
			} else {
				mastodon_log(ic, "Sorry, I can't figure out what you're reply to!");
			}
		}
	} else if (g_ascii_strcasecmp(cmd[0], "cw") == 0) {
		g_free(md->spoiler_text);
		if (cmd[1] == NULL) {
			md->spoiler_text = NULL;
			mastodon_log(ic, "Next post will get no content warning");
		} else {
			md->spoiler_text = g_strdup(message + 3);
			mastodon_log(ic, "Next post will get content warning '%s'", md->spoiler_text);
		}
	} else if ((g_ascii_strcasecmp(cmd[0], "visibility") == 0 ||
				g_ascii_strcasecmp(cmd[0], "vis") == 0)) {
		if (cmd[1] == NULL) {
			md->visibility = mastodon_default_visibility(ic);
		} else {
			md->visibility = mastodon_parse_visibility(cmd[1]);
		}
		mastodon_log(ic, "Next post is %s",
					 mastodon_visibility(md->visibility));
	} else if (g_ascii_strcasecmp(cmd[0], "post") == 0) {
		if (cmd[1] == NULL) {
			mastodon_log(ic, "What should we post?");
		} else {
			mastodon_post_message(ic, message + 5, 0, cmd[1], MASTODON_NEW_MESSAGE, NULL, MV_UNKNOWN, NULL);
		}
	} else if (g_ascii_strcasecmp(cmd[0], "public") == 0 ||
			   g_ascii_strcasecmp(cmd[0], "unlisted") == 0 ||
			   g_ascii_strcasecmp(cmd[0], "private") == 0 ||
			   g_ascii_strcasecmp(cmd[0], "direct") == 0) {
		mastodon_log(ic, "Please use the visibility command instead");
	} else if (allow_post) {
		mastodon_post_message(ic, message, 0, cmd[0], MASTODON_MAYBE_REPLY, NULL, MV_UNKNOWN, NULL);
	} else {
		mastodon_log(ic, "Unknown command: %s", cmd[0]);
	}

	g_free(cmds);
}

void mastodon_log(struct im_connection *ic, char *format, ...)
{
	struct mastodon_data *md = ic->proto_data;
	va_list params;
	char *text;

	va_start(params, format);
	text = g_strdup_vprintf(format, params);
	va_end(params);

	if (md->timeline_gc) {
		imcb_chat_log(md->timeline_gc, "%s", text);
	} else {
		imcb_log(ic, "%s", text);
	}

	g_free(text);
}

G_MODULE_EXPORT void init_plugin(void)
{
	struct prpl *ret = g_new0(struct prpl, 1);

	ret->options = PRPL_OPT_NOOTR | PRPL_OPT_NO_PASSWORD;
	ret->name = "mastodon";
	ret->login = mastodon_login;
	ret->init = mastodon_init;
	ret->logout = mastodon_logout;
	ret->buddy_msg = mastodon_buddy_msg;
	ret->get_info = mastodon_get_info;
	ret->add_buddy = mastodon_add_buddy;
	ret->remove_buddy = mastodon_remove_buddy;
	ret->chat_msg = mastodon_chat_msg;
	ret->chat_join = mastodon_chat_join;
	ret->chat_leave = mastodon_chat_leave;
	ret->add_permit = mastodon_add_permit;
	ret->rem_permit = mastodon_rem_permit;
	ret->add_deny = mastodon_add_deny;
	ret->rem_deny = mastodon_rem_deny;
	ret->buddy_data_add = mastodon_buddy_data_add;
	ret->buddy_data_free = mastodon_buddy_data_free;
	ret->handle_cmp = g_ascii_strcasecmp;

	register_protocol(ret);
}
