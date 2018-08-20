/***************************************************************************\
*                                                                           *
*  BitlBee - An IRC to IM gateway                                           *
*  Simple module to facilitate Mastodon functionality.                      *
*                                                                           *
*  Copyright 2009-2010 Geert Mulders <g.c.w.m.mulders@gmail.com>            *
*  Copyright 2010-2013 Wilmer van der Gaast <wilmer@gaast.net>              *
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

/* For strptime(): */
#if (__sun)
#else
#define _XOPEN_SOURCE
#endif

#include "mastodon-http.h"
#include "mastodon.h"
#include "rot13.h"
#include "bitlbee.h"
#include "url.h"
#include "misc.h"
#include "base64.h"
#include "mastodon-lib.h"
#include "oauth2.h"
#include "json_util.h"
#include <assert.h>
#include <ctype.h>
#include <errno.h>

typedef enum {
	MT_HOME,
	MT_LOCAL,
	MT_FEDERATED,
	MT_HASHTAG,
	MT_LIST,
} mastodon_timeline_type_t;

typedef enum {
	ML_STATUS,
	ML_NOTIFICATION,
	ML_USER,
	ML_ID,
} mastodon_list_type_t;

struct mastodon_list {
	mastodon_list_type_t type;
	GSList *list;
};

struct mastodon_account {
	guint64 id;
	char *display_name;
	char *acct;
};

struct mastodon_status {
	time_t created_at;
	char *spoiler_text;
	char *text;
	char *content; /* same as text without CW and NSFW prefixes */
	char *url;
	struct mastodon_account *account;
	guint64 id;
	mastodon_visibility_t visibility;
	guint64 reply_to;
	GSList *tags;
	GSList *mentions;
	mastodon_timeline_type_t subscription; /* This status was created by a timeline subscription */
};

typedef enum {
	MN_MENTION = 1,
	MN_REBLOG,
	MN_FAVOURITE,
	MN_FOLLOW,
} mastodon_notification_type_t;

struct mastodon_notification {
	guint64 id;
	mastodon_notification_type_t type;
	time_t created_at;
	struct mastodon_account *account;
	struct mastodon_status *status;
};

struct mastodon_report {
	struct im_connection *ic;
	guint64 account_id;
	guint64 status_id;
	char *comment;
};

struct mastodon_command {
	struct im_connection *ic;
	guint64 id;
	guint64 id2;
	gboolean extra;
	char *str;
	char *undo;
	char *redo;
	struct groupchat *c;
	mastodon_command_type_t command;
};

/**
 * Frees a mastodon_account struct.
 */
static void ma_free(struct mastodon_account *ma)
{
	if (ma == NULL) {
		return;
	}

	g_free(ma->display_name);
	g_free(ma->acct);
	g_free(ma);
}

/**
 * Creates a duplicate of an account.
 */
static struct mastodon_account *ma_copy(struct mastodon_account *ma0)
{
	if (ma0 == NULL) {
		return NULL;
	}

	struct mastodon_account *ma = g_new0(struct mastodon_account, 1);
	ma->id = ma0->id;
	ma->display_name = g_strdup(ma0->display_name);
	ma->acct = g_strdup(ma0->acct);
	return ma;
}

/**
 * Frees a mastodon_status struct.
 */
static void ms_free(struct mastodon_status *ms)
{
	if (ms == NULL) {
		return;
	}

	g_free(ms->spoiler_text);
	g_free(ms->text);
	g_free(ms->content);
	g_free(ms->url);
	ma_free(ms->account);
	g_slist_free_full(ms->tags, g_free);
	g_slist_free_full(ms->mentions, g_free);
	g_free(ms);
}

/**
 * Frees a mastodon_notification struct.
 */
static void mn_free(struct mastodon_notification *mn)
{
	if (mn == NULL) {
		return;
	}

	ma_free(mn->account);
	ms_free(mn->status);
	g_free(mn);
}

/**
 * Free a mastodon_list struct.
 * type is the type of list the struct holds.
 */
static void ml_free(struct mastodon_list *ml)
{
	GSList *l;

	if (ml == NULL) {
		return;
	}

	for (l = ml->list; l; l = g_slist_next(l)) {
		if (ml->type == ML_STATUS) {
			ms_free((struct mastodon_status *) l->data);
		} else if (ml->type == ML_NOTIFICATION) {
			mn_free((struct mastodon_notification *) l->data);
		} else if (ml->type == ML_ID) {
			g_free(l->data);
		} else if (ml->type == ML_USER) {
			ma_free(l->data);
		}
	}

	g_slist_free(ml->list);
	g_free(ml);
}

/**
 * Frees a mastodon_report struct.
 */
static void mr_free(struct mastodon_report *mr)
{
	if (mr == NULL) {
		return;
	}

	g_free(mr->comment);
	g_free(mr);
}

/**
 * Frees a mastodon_command struct. Note that the groupchat c doesn't need to be freed. It is maintained elsewhere.
 */
static void mc_free(struct mastodon_command *mc)
{
	if (mc == NULL) {
		return;
	}

	g_free(mc->str);
	g_free(mc->undo);
	g_free(mc->redo);
	g_free(mc);
}


/**
 * Compare status elements
 */
static gint mastodon_compare_elements(gconstpointer a, gconstpointer b)
{
	struct mastodon_status *a_status = (struct mastodon_status *) a;
	struct mastodon_status *b_status = (struct mastodon_status *) b;

	if (a_status->created_at < b_status->created_at) {
		return -1;
	} else if (a_status->created_at > b_status->created_at) {
		return 1;
	} else {
		return 0;
	}
}

/**
 * Add a buddy if it is not already added, set the status to logged in.
 */
static void mastodon_add_buddy(struct im_connection *ic, gint64 id, char *name, const char *fullname)
{
	struct mastodon_data *md = ic->proto_data;

	// Check if the buddy is already in the buddy list.
	if (!bee_user_by_handle(ic->bee, ic, name)) {
		// The buddy is not in the list, add the buddy and set the status to logged in.
		imcb_add_buddy(ic, name, NULL);
		imcb_rename_buddy(ic, name, fullname);

		bee_user_t *bu = bee_user_by_handle(ic->bee, ic, name);
		struct mastodon_user_data *mud = (struct mastodon_user_data*) bu->data;
		mud->account_id = id;

		if (md->flags & MASTODON_MODE_CHAT) {
			/* Necessary so that nicks always get translated to the
			   exact Mastodon username. */
			imcb_buddy_nick_hint(ic, name, name);
			if (md->timeline_gc) {
				imcb_chat_add_buddy(md->timeline_gc, name);
			}
		} else if (md->flags & MASTODON_MODE_MANY) {
			imcb_buddy_status(ic, name, OPT_LOGGED_IN, NULL, NULL);
		}
	}
}

/* Warning: May return a malloc()ed value, which will be free()d on the next
   call. Only for short-term use. NOT THREADSAFE!  */
char *mastodon_parse_error(struct http_request *req)
{
	static char *ret = NULL;
	json_value *root, *err;

	g_free(ret);
	ret = NULL;

	if (req->body_size > 0) {
		root = json_parse(req->reply_body, req->body_size);
		err = json_o_get(root, "error");
		if (err && err->type == json_string && err->u.string.length) {
			ret = g_strdup_printf("%s (%s)", req->status_string, err->u.string.ptr);
		}
		json_value_free(root);
	}

	return ret ? ret : req->status_string;
}

/* WATCH OUT: This function might or might not destroy your connection.
   Sub-optimal indeed, but just be careful when this returns NULL! */
static json_value *mastodon_parse_response(struct im_connection *ic, struct http_request *req)
{
	char path[64] = "", *s;

	if ((s = strchr(req->request, ' '))) {
		path[sizeof(path) - 1] = '\0';
		strncpy(path, s + 1, sizeof(path) - 1);
		if ((s = strchr(path, '?')) || (s = strchr(path, ' '))) {
			*s = '\0';
		}
	}

	if (req->status_code != 200) {
		mastodon_log(ic, "Error: %s returned status code %s", path, mastodon_parse_error(req));

		if (!(ic->flags & OPT_LOGGED_IN)) {
			imc_logout(ic, TRUE);
		}
		return NULL;
	}

	json_value *ret;
	if ((ret = json_parse(req->reply_body, req->body_size)) == NULL) {
		imcb_error(ic, "Error: %s return data that could not be parsed as JSON", path);
	}
	return ret;
}

/**
 * For Mastodon 2, all id attributes in the REST API responses, including attributes that end in _id, are now returned
 * as strings instead of integers. This is because large integers cannot be encoded in JSON losslessly, and all IDs in
 * Mastodon are now bigint (Ruby on Rails: bigint uses 64 bits, signed, guint64 is 64 bits, unsigned). We are assuming
 * no negative ids.
 */
static guint64 mastodon_json_int64(const json_value *v)
{
	guint64 id;
	if (v->type == json_integer) {
		return v->u.integer; // Mastodon 1

	} else if (v->type == json_string &&
		   *v->u.string.ptr &&
		   parse_int64(v->u.string.ptr, 10, &id)) {
		return id; // Mastodon 2
	}
	return 0;
}

/* These two functions are useful to debug all sorts of callbacks. */
static void mastodon_log_object(struct im_connection *ic, json_value *node, int prefix);
static void mastodon_log_array(struct im_connection *ic, json_value *node, int prefix);

struct mastodon_account *mastodon_xt_get_user(const json_value *node)
{
	struct mastodon_account *ma;
	json_value *jv;

	ma = g_new0(struct mastodon_account, 1);
	ma->display_name = g_strdup(json_o_str(node, "display_name"));
	ma->acct = g_strdup(json_o_str(node, "acct"));

	if ((jv = json_o_get(node, "id")) &&
	    (ma->id = mastodon_json_int64(jv))) {
		return ma;
	}

	ma_free(ma);
	return NULL;
}

/* This is based on strip_html but in addition to what Bitlbee does, we treat p like br. */
void mastodon_strip_html(char *in)
{
	char *start = in;
	char out[strlen(in) + 1];
	char *s = out;

	memset(out, 0, sizeof(out));

	while (*in) {
		if (*in == '<') {
			if (g_strncasecmp(in + 1, "/p>", 3) == 0) {
				*(s++) = '\n';
				in += 4;
			} else {
				*(s++) = *(in++);
			}
		} else {
			*(s++) = *(in++);
		}
	}
	strcpy(start, out);
	strip_html(start);
}

mastodon_visibility_t mastodon_parse_visibility(char *value)
{
	if (g_strcasecmp(value, "public") == 0) {
		return MV_PUBLIC;
	} else if (g_strcasecmp(value, "unlisted") == 0) {
		return MV_UNLISTED;
	} else if (g_strcasecmp(value, "private") == 0) {
		return MV_PRIVATE;
	} else if (g_strcasecmp(value, "direct") == 0) {
		return MV_DIRECT;
	} else {
		return MV_UNKNOWN;
	}
}

/**
 * Here, we have a similar setup as for mastodon_chained_account_function. The flow is as follows: the
 * mastodon_command_handler() calls mastodon_unknown_list_delete(). This sets up the mastodon_command (mc). It then
 * calls mastodon_with_named_list() and passes along a callback, mastodon_http_list_delete(). It uses
 * mastodon_chained_list() to extract the list id and store it in mc, and calls the next handler,
 * mastodon_list_delete(). This is a mastodon_chained_command_function! It doesn't have to check whether ic is live.
 */
typedef void (*mastodon_chained_command_function)(struct im_connection *ic, struct mastodon_command *mc);

/**
 * This is the wrapper around callbacks that need to search for the list id in a list result. Note that list titles are
 * case-sensitive.
 */
static void mastodon_chained_list(struct http_request *req, mastodon_chained_command_function func) {
	struct mastodon_command *mc = req->data;
	struct im_connection *ic = mc->ic;

	if (!g_slist_find(mastodon_connections, ic)) {
		goto finally;
	}

	json_value *parsed;
	if (!(parsed = mastodon_parse_response(ic, req))) {
		/* ic would have been freed in imc_logout in this situation */
		ic = NULL;
		goto finally;
	}

	if (parsed->type != json_array || parsed->u.array.length == 0) {
		mastodon_log(ic, "You seem to have no lists defined. "
					 "Create one using 'list create <title>'.");
		goto finish;
	}

	int i;
	guint64 id = 0;
	char *title = mc->str;

	for (i = 0; i < parsed->u.array.length; i++) {
			json_value *a = parsed->u.array.values[i];
			json_value *it;
			if (a->type == json_object &&
				(it = json_o_get(a, "id")) &&
				g_strcmp0(title, json_o_str(a, "title")) == 0) {
				id = mastodon_json_int64(it);
				break;
			}
	}

	if (!id) {
		mastodon_log(ic, "There is no list called '%s'. "
					 "Use 'list' to show existing lists.", title);
		goto finish;
	} else {
		mc->id = id;
		func(ic, mc);
		/* If successful, we need to keep mc for one more request. */
		json_value_free(parsed);
		return;
	}

finish:
	/* We didn't find what we were looking for and need to free the parsed data. */
	json_value_free(parsed);
finally:
	/* We've encountered a problem and we need to free the mastodon_command. */
	mc_free(mc);
}

/**
 * Wrapper which sets up the first callback for functions acting on a list. For every command, the list has to be
 * searched, first. The callback you provide must used mastodon_chained_list() to extract the list id and then call the
 * reall callback.
 */
void mastodon_with_named_list(struct im_connection *ic, struct mastodon_command *mc, http_input_function func) {
	mastodon_http(ic, MASTODON_LIST_URL, func, mc, HTTP_GET, NULL, 0);
}

/**
 * Function to fill a mastodon_status struct.
 */
static struct mastodon_status *mastodon_xt_get_status(const json_value *node, struct im_connection *ic)
{
	struct mastodon_status *ms = {0};
	const json_value *rt = NULL;
	const json_value *text_value = NULL;
	const json_value *spoiler_value = NULL;
	const json_value *url_value = NULL;
	GSList *media = NULL;
	gboolean nsfw = FALSE;
	gboolean use_cw1 = g_strcasecmp(set_getstr(&ic->acc->set, "hide_sensitive"), "advanced_rot13") == 0;

	if (node->type != json_object) {
		return FALSE;
	}
	ms = g_new0(struct mastodon_status, 1);

	JSON_O_FOREACH(node, k, v) {
		if (strcmp("content", k) == 0 && v->type == json_string && *v->u.string.ptr) {
			text_value = v;
		} if (strcmp("spoiler_text", k) == 0 && v->type == json_string && *v->u.string.ptr) {
			spoiler_value = v;
		} else if (strcmp("url", k) == 0 && v->type == json_string) {
			url_value = v;
		} else if (strcmp("reblog", k) == 0 && v->type == json_object) {
			rt = v;
		} else if (strcmp("created_at", k) == 0 && v->type == json_string) {
			struct tm parsed;

			/* Very sensitive to changes to the formatting of
			   this field. :-( Also assumes the timezone used
			   is UTC since C time handling functions suck. */
			if (strptime(v->u.string.ptr, MASTODON_TIME_FORMAT, &parsed) != NULL) {
				ms->created_at = mktime_utc(&parsed);
			}
		} else if (strcmp("visibility", k) == 0 && v->type == json_string && *v->u.string.ptr) {
			ms->visibility = mastodon_parse_visibility(v->u.string.ptr);
		} else if (strcmp("account", k) == 0 && v->type == json_object) {
			ms->account = mastodon_xt_get_user(v);
		} else if (strcmp("id", k) == 0) {
			ms->id = mastodon_json_int64(v);
		} else if (strcmp("in_reply_to_id", k) == 0) {
			ms->reply_to = mastodon_json_int64(v);
		} else if (strcmp("tags", k) == 0 && v->type == json_array) {
			GSList *l = NULL;
			int i;
			for (i = 0; i < v->u.array.length; i++) {
				json_value *tag = v->u.array.values[i];
				if (tag->type == json_object) {
					const char *name = json_o_str(tag, "name");
					if (name) {
						l = g_slist_prepend(l, g_strdup(name));
					}
				}
			}
			ms->tags = l;
		} else if (strcmp("mentions", k) == 0 && v->type == json_array) {
			GSList *l = NULL;
			int i;
			/* skip the current user in mentions since we're only interested in this information for replies where we'll
			 * never want to mention ourselves */
			gint64 id = set_getint(&ic->acc->set, "account_id");
			for (i = 0; i < v->u.array.length; i++) {
				json_value *mention = v->u.array.values[i];
				json_value *it;
				const char *acct;
				if (mention->type == json_object &&
					(it = json_o_get(mention, "id")) &&
					mastodon_json_int64(it) != id &&
					(acct = json_o_str(mention, "acct"))) {
					l = g_slist_prepend(l, g_strdup(acct));
				}
			}
			ms->mentions = l;
		} else if (strcmp("sensitive", k) == 0 && v->type == json_boolean) {
			nsfw = v->u.boolean;
		} else if (strcmp("media_attachments", k) == 0 && v->type == json_array) {
			int i;
			for (i = 0; i < v->u.array.length; i++) {
				json_value *attachment = v->u.array.values[i];
				if (attachment->type == json_object) {
					// text_url is preferred because that's what the UI also copies
					// into the message; also ignore values such as /files/original/missing.png
					const char *url = json_o_str(attachment, "text_url");
					if (!url || !*url || strncmp(url, "http", 4)) {
						url = json_o_str(attachment, "url");
						if (!url || !*url || strncmp(url, "http", 4)) {
							url = json_o_str(attachment, "remote_url");
						}
					}
					if (url && *url && strncmp(url, "http", 4) == 0) {
						media = g_slist_prepend(media, (char *) url); // discarding const qualifier
					}
				}
			}
		}
	}

	if (rt) {
		struct mastodon_status *rms = mastodon_xt_get_status(rt, ic);
		if (rms) {
			/* Alternatively, we could free ms and just use rms, but we'd have to overwrite rms->account
			 * with ms->account, change rms->text, and maybe more. */
			ms->text = g_strdup_printf("boosted @%s: %s", rms->account->acct, rms->text);
			ms->id = rms->id;

			ms->url = rms->url; // adopt
			rms->url = NULL;

			g_slist_free_full(ms->tags, g_free);
			ms->tags = rms->tags; // adopt
			rms->tags = NULL;

			g_slist_free_full(ms->mentions, g_free);
			ms->mentions = rms->mentions; // adopt
			rms->mentions = NULL;

			/* add original author to mentions of boost if not ourselves */
			gint64 id = set_getint(&ic->acc->set, "account_id");
			if (rms->account->id != id) {
				ms->mentions = g_slist_prepend(ms->mentions, rms->account->acct); // adopt
				rms->account->acct = NULL;
			}

			ms_free(rms);
		}
	} else if (ms->id) {

		if (url_value) {
			ms->url = g_strdup(url_value->u.string.ptr);
		}

		// build status text
		GString *s = g_string_new(NULL);

		if (spoiler_value) {
			char *spoiler_text  = g_strdup(spoiler_value->u.string.ptr);
			mastodon_strip_html(spoiler_text);
			g_string_append_printf(s, "[CW: %s]", spoiler_text);
			ms->spoiler_text = spoiler_text;
			if (nsfw || !use_cw1) {
				g_string_append(s, " ");
			}
		}

		if (nsfw) {
			char *sensitive_flag = set_getstr(&ic->acc->set, "sensitive_flag");
			g_string_append(s, sensitive_flag);
		}

		if (text_value) {
			char *text = g_strdup(text_value->u.string.ptr);
			mastodon_strip_html(text);
			ms->content = g_strdup(text);
			char *fmt = "%s";
			if (spoiler_value && use_cw1) {
				char *wrapped = NULL;
				char **cwed = NULL;
				rot13(text);
				// "\001CW1 \001" = 6 bytes, there's also a nick length issue we take into account.
				// there's also irc_format_timestamp which can add like 28 bytes or something.
				wrapped = word_wrap(text, IRC_WORD_WRAP - 6 - MAX_NICK_LENGTH - 28);
				g_free(text);
				text = wrapped;
				cwed = g_strsplit(text, "\n", -1); // easier than a regex
				g_free(text);
				text = g_strjoinv("\001\n\001CW1 ", cwed); // easier than a replace
				g_strfreev(cwed);
				fmt = "\n\001CW1 %s\001"; // add a newline at the start because that makes word wrap a lot easier (and because it matches the web UI better)
			} else if (spoiler_value && g_strcasecmp(set_getstr(&ic->acc->set, "hide_sensitive"), "rot13") == 0) {
				rot13(text);
			} else if (spoiler_value && set_getbool(&ic->acc->set, "hide_sensitive")) {
				g_free(text);
				text = g_strdup(ms->url);
				if (text) {
					fmt = "[hidden: %s]";
				} else {
					fmt = "[hidden]";
				}
			}
			g_string_append_printf(s, fmt, text);
			g_free(text);
		}

		GSList *l = NULL;
		for (l = media; l; l = l->next) {
			// TODO maybe support hiding media when it's marked NSFW.
			// (note that only media is hidden when it's marked NSFW. the text still shows.)
			// (note that we already don't show media, since this is all text, but IRC clients might.)

			char *url = l->data;

			if ((text_value && strstr(text_value->u.string.ptr, url)) || strstr(s->str, url)) {
				// skip URLs already in the text
				continue;
			}

			if (s->len) {
				g_string_append(s, " ");
			}
			g_string_append(s, url);
		}

		ms->text = g_string_free(s, FALSE); // we keep the data

	}

	g_slist_free(media); // elements are pointers into node and don't need to be freed

	if (ms->text && ms->account && ms->id) {
		return ms;
	}

	ms_free(ms);
	return NULL;
}

/**
 * Function to fill a mastodon_notification struct.
 */
static struct mastodon_notification *mastodon_xt_get_notification(const json_value *node, struct im_connection *ic)
{
	if (node->type != json_object) {
		return FALSE;
	}

	struct mastodon_notification *mn = g_new0(struct mastodon_notification, 1);

	JSON_O_FOREACH(node, k, v) {
		if (strcmp("id", k) == 0) {
			mn->id = mastodon_json_int64(v);
		} else if (strcmp("created_at", k) == 0 && v->type == json_string) {
			struct tm parsed;

			/* Very sensitive to changes to the formatting of
			   this field. :-( Also assumes the timezone used
			   is UTC since C time handling functions suck. */
			if (strptime(v->u.string.ptr, MASTODON_TIME_FORMAT, &parsed) != NULL) {
				mn->created_at = mktime_utc(&parsed);
			}
		} else if (strcmp("account", k) == 0 && v->type == json_object) {
			mn->account = mastodon_xt_get_user(v);
		} else if (strcmp("status", k) == 0 && v->type == json_object) {
			mn->status = mastodon_xt_get_status(v, ic);
		} else if (strcmp("type", k) == 0 && v->type == json_string) {
			if (strcmp(v->u.string.ptr, "mention") == 0) {
				mn->type = MN_MENTION;
			} else if (strcmp(v->u.string.ptr, "reblog") == 0) {
				mn->type = MN_REBLOG;
			} else if (strcmp(v->u.string.ptr, "favourite") == 0) {
				mn->type = MN_FAVOURITE;
			} else if (strcmp(v->u.string.ptr, "follow") == 0) {
				mn->type = MN_FOLLOW;
			}
		}
	}

	if (mn->type) {
		return mn;
	}

	mn_free(mn);
	return NULL;
}

static gboolean mastodon_xt_get_status_list(struct im_connection *ic, const json_value *node,
					    struct mastodon_list *ml)
{
	ml->type = ML_STATUS;

	if (node->type != json_array) {
		return FALSE;
	}

	int i;
	for (i = 0; i < node->u.array.length; i++) {
		struct mastodon_status *ms = mastodon_xt_get_status(node->u.array.values[i], ic);
		if (ms) {
			ml->list = g_slist_prepend(ml->list, ms);
		}
	}
	ml->list = g_slist_reverse(ml->list);
	return TRUE;
}

static gboolean mastodon_xt_get_notification_list(struct im_connection *ic, const json_value *node,
						  struct mastodon_list *ml)
{
	ml->type = ML_NOTIFICATION;

	if (node->type != json_array) {
		return FALSE;
	}

	int i;
	for (i = 0; i < node->u.array.length; i++) {
		struct mastodon_notification *mn = mastodon_xt_get_notification(node->u.array.values[i], ic);
		if (mn) {
			ml->list = g_slist_prepend(ml->list, mn);
		}
	}
	ml->list = g_slist_reverse(ml->list);
	return TRUE;
}

/* Will log messages either way. Need to keep track of IDs for stream deduping.
   Plus, show_ids is on by default and I don't see why anyone would disable it. */
static char *mastodon_msg_add_id(struct im_connection *ic,
				 struct mastodon_status *ms, const char *prefix)
{
	struct mastodon_data *md = ic->proto_data;
	int reply_to = -1;
	int idx = -1;

	/* See if we know this status and if we know the status this one is replying to. */
	int i;
	for (i = 0; i < MASTODON_LOG_LENGTH; i++) {
		if (ms->reply_to && md->log[i].id == ms->reply_to) {
			reply_to = i;
		}
		if (md->log[i].id == ms->id) {
			idx = i;
		}
		if (idx != -1 && (!ms->reply_to || reply_to != -1)) {
			break;
		}
	}

	/* If we didn't find the status, it's new and needs an id, and we want to record who said it, and when they said
	 * it, and who they mentioned, and the spoiler they used. We need to do this in two places: the md->log, and per
	 * user in the mastodon_user_data (mud). */
	if (idx == -1) {
		idx = md->log_id = (md->log_id + 1) % MASTODON_LOG_LENGTH;
		md->log[idx].id = ms->id;

		md->log[idx].visibility = ms->visibility;
		g_slist_free_full(md->log[idx].mentions, g_free);
		md->log[idx].mentions = g_slist_copy_deep(ms->mentions, (GCopyFunc) g_strdup, NULL);

		g_free(md->log[idx].spoiler_text);
		md->log[idx].spoiler_text = g_strdup(ms->spoiler_text); // no problem if NULL

		gint64 id = set_getint(&ic->acc->set, "account_id");
		if (ms->account->id == id) {
			/* If this is our own status, use a fake bu without data since we can't be found by handle. This
			 * will allow us to reply to our own messages, for example. */
			md->log[idx].bu = &mastodon_log_local_user;
		} else {
			bee_user_t *bu = bee_user_by_handle(ic->bee, ic, ms->account->acct);
			struct mastodon_user_data *mud = bu->data;

			if (ms->id > mud->last_id) {
				mud->last_id = ms->id;
				mud->last_time = ms->created_at;

				mud->visibility = ms->visibility;
				g_slist_free_full(mud->mentions, g_free);
				mud->mentions = g_slist_copy_deep(ms->mentions, (GCopyFunc) g_strdup, NULL);

				g_free(mud->spoiler_text);
				mud->spoiler_text = g_strdup(ms->spoiler_text); // no problem if NULL
			}

			md->log[idx].bu = bu;
		}

	}

	if (set_getbool(&ic->acc->set, "show_ids")) {
		if (reply_to != -1) {
			return g_strdup_printf("\002[\002%02x->%02x\002]\002 %s%s",
			                       idx, reply_to, prefix, ms->text);
		} else {
			return g_strdup_printf("\002[\002%02x\002]\002 %s%s",
			                       idx, prefix, ms->text);
		}
	} else {
		if (*prefix) {
			return g_strconcat(prefix, ms->text, NULL);
		} else {
			return NULL;
		}
	}
}

/**
 * Helper function for mastodon_status_show_chat.
 */
static void mastodon_status_show_chat1(struct im_connection *ic, gboolean me, struct groupchat *c, char *msg, struct mastodon_status *ms)
{
	if (me) {
		mastodon_visibility_t default_visibility = mastodon_default_visibility(ic);
		if (ms->visibility == default_visibility) {
			imcb_chat_log(c, "You: %s", msg ? msg : ms->text);
		} else {
			imcb_chat_log(c, "You, %s: %s", mastodon_visibility(ms->visibility), msg ? msg : ms->text);
		}
	} else {
		imcb_chat_msg(c, ms->account->acct,
			      msg ? msg : ms->text, 0, ms->created_at);
	}
}

/**
 * Function that is called to see the statuses in a group chat. If the user created appropriate group chats (see setup
 * in mastodon_chat_join()), then we have extra streams providing the toots for these streams. The subscription
 * attribute gives us a basic hint of how the status wants to be sorted. Now, we also have a TIMELINE command, which
 * allows us to simulate the result. In this case, we can't be sure that appropriate group chats exist and thus we need
 * to put those statuses into the user timeline if they do not. */
static void mastodon_status_show_chat(struct im_connection *ic, struct mastodon_status *status)
{
	gint64 id = set_getint(&ic->acc->set, "account_id");
	gboolean me = (status->account->id == id);

	if (!me) {
		/* MUST be done before mastodon_msg_add_id() to avoid #872. */
		mastodon_add_buddy(ic, status->account->id, status->account->acct, status->account->display_name);
	}

	char *msg = mastodon_msg_add_id(ic, status, "");

	gboolean seen = FALSE;
	struct mastodon_user_data *mud;
	struct groupchat *c;
	bee_user_t *bu;
	GSList *l;

	switch (status->subscription) {

	case MT_LIST:
		/* Add the status to existing group chats with a topic matching any the lists this user is part of. */
		bu = bee_user_by_handle(ic->bee, ic, status->account->acct);
		mud = (struct mastodon_user_data*) bu->data;
		for (l = mud->lists; l; l = l->next) {
			char *title = l->data;
			struct groupchat *c = bee_chat_by_title(ic->bee, ic, title);
			if (c) {
				mastodon_status_show_chat1(ic, me, c, msg, status);
				seen = TRUE;
			}
		}
		break;

	case MT_HASHTAG:
		/* Add the status to any other existing group chats whose title matches one of the tags, including the hash! */
		for (l = status->tags; l; l = l->next) {
			char *tag = l->data;
			char *title = g_strdup_printf("#%s", tag);
			struct groupchat *c = bee_chat_by_title(ic->bee, ic, title);
			if (c) {
				mastodon_status_show_chat1(ic, me, c, msg, status);
				seen = TRUE;
			}
			g_free(title);
		}
		break;

	case MT_LOCAL:
		/* If there is an appropriate group chat, do not put it in the user timeline. */
		c = bee_chat_by_title(ic->bee, ic, "local");
		if (c) {
			mastodon_status_show_chat1(ic, me, c, msg, status);
			seen = TRUE;
		}
		break;

	case MT_FEDERATED:
		/* If there is an appropriate group chat, do not put it in the user timeline. */
		c = bee_chat_by_title(ic->bee, ic, "federated");
		if (c) {
			mastodon_status_show_chat1(ic, me, c, msg, status);
			seen = TRUE;
		}
		break;

	case MT_HOME:
		/* This is the default */
		break;
	}

	if (!seen) {
		c = mastodon_groupchat_init(ic);
		mastodon_status_show_chat1(ic, me, c, msg, status);
	}

	g_free(msg);
}

/**
 * Function that is called to see statuses as private messages.
 */
static void mastodon_status_show_msg(struct im_connection *ic, struct mastodon_status *ms)
{
	struct mastodon_data *md = ic->proto_data;
	char from[MAX_STRING] = "";
	char *prefix = NULL, *text = NULL;
	gint64 id = set_getint(&ic->acc->set, "account_id");
	gboolean me = (ms->account->id == id);
	char *name = set_getstr(&ic->acc->set, "name");

	if (md->flags & MASTODON_MODE_ONE) {
		g_strlcpy(from, name, sizeof(from));
	}

	if (md->flags & MASTODON_MODE_ONE) {
		prefix = g_strdup_printf("\002<\002%s\002>\002 ",
		                         ms->account->acct);
	} else if (!me) {
		mastodon_add_buddy(ic, ms->account->id, ms->account->acct, ms->account->display_name);
	} else {
		prefix = g_strdup("You: ");
	}

	text = mastodon_msg_add_id(ic, ms, prefix ? prefix : "");

	imcb_buddy_msg(ic,
	               *from ? from : ms->account->acct,
	               text ? text : ms->text, 0, ms->created_at);

	g_free(text);
	g_free(prefix);
}

struct mastodon_status *mastodon_notification_to_status(struct mastodon_notification *notification)
{
	struct mastodon_account *ma = notification->account;
	struct mastodon_status *ms = notification->status;

	if (ma == NULL) {
		// Should not happen.
		ma = g_new0(struct mastodon_account, 1);
		ma->acct = g_strdup("anon");
		ma->display_name = g_strdup("Unknown");
	}

	// The status in the notification was written by you, it's
	// account is your account, but now somebody else is doing
	// something with it. We want to avoid the extra You at the
	// beginning, "You: [01] @foo boosted your status: bla" should
	// be "<foo> [01] boosted your status: bla" or "<foo> followed
	// you".
	if (ms == NULL) {
		// Could be a FOLLOW notification without status.
		ms = g_new0(struct mastodon_status, 1);
		ms->account = ma_copy(notification->account);
		ms->created_at = notification->created_at;
		// This ensures that ms will be freed when the notification is freed.
		notification->status = ms;
	} else {
		// Adopt the account from the notification. The
		// account will be freed when the notification frees
		// the status.
		ma_free(ms->account);
		ms->account = ma;
		notification->account = NULL;
	}

	char *original = ms->text;

	switch (notification->type) {
	case MN_MENTION:
		// this is fine
		original = NULL;
		break;
	case MN_REBLOG:
		ms->text = g_strdup_printf("boosted your status: %s", original);
		break;
	case MN_FAVOURITE:
		ms->text = g_strdup_printf("favourited your status: %s", original);
		break;
	case MN_FOLLOW:
		ms->text = g_strdup_printf("[%s] followed you", ma->display_name);
		break;
	}

	g_free(original);

	return ms;
}

/**
 * Show the status to the user.
 */
static void mastodon_status_show(struct im_connection *ic, struct mastodon_status *ms)
{
	struct mastodon_data *md = ic->proto_data;

	if (ms->account == NULL || ms->text == NULL) {
		return;
	}

	/* Deduplicating only affects the previous status shown. Thus,
	 * if we got mentioned in a toot by a user that we're
	 * following, chances are that both events will arrive in
	 * sequence. In this case, the second one will be skipped.
	 * This will also work when flushing timelines after
	 * connecting: notification and status update should be close
	 * to each other. This will fail if the stream is really busy.
	 * Critically, it won't suppress statuses from later context
	 * and timeline requests. */
	if (ms->id == md->seen_id) {
		return;
	} else {
		md->seen_id = ms->id;
	}

	/* Grrrr. Would like to do this during parsing, but can't access
	   settings from there. */
	if (set_getbool(&ic->acc->set, "strip_newlines")) {
		strip_newlines(ms->text);
	}

	if (md->flags & MASTODON_MODE_CHAT) {
		mastodon_status_show_chat(ic, ms);
	} else {
		mastodon_status_show_msg(ic, ms);
	}
}

static void mastodon_notification_show(struct im_connection *ic, struct mastodon_notification *notification)
{
	gboolean show = TRUE;

	switch (notification->type) {
	case MN_MENTION:
		show = !set_getbool(&ic->acc->set, "hide_mentions");
		break;
	case MN_REBLOG:
		show = !set_getbool(&ic->acc->set, "hide_boosts");
		break;
	case MN_FAVOURITE:
		show = !set_getbool(&ic->acc->set, "hide_favourites");
		break;
	case MN_FOLLOW:
		show = !set_getbool(&ic->acc->set, "hide_follows");
		break;
	}

	if (show)
		mastodon_status_show(ic, mastodon_notification_to_status(notification));
}

/**
 * Add exactly one notification to the timeline.
 */
static void mastodon_stream_handle_notification(struct im_connection *ic, json_value *parsed, mastodon_timeline_type_t subscription)
{
	struct mastodon_notification *mn = mastodon_xt_get_notification(parsed, ic);
	if (mn) {
		if (mn->status)
			mn->status->subscription = subscription;
		mastodon_notification_show(ic, mn);
		mn_free(mn);
	}
}

/**
 * Add exactly one status to the timeline.
 */
static void mastodon_stream_handle_update(struct im_connection *ic, json_value *parsed, mastodon_timeline_type_t subscription)
{
	struct mastodon_status *ms = mastodon_xt_get_status(parsed, ic);
	if (ms) {
		ms->subscription = subscription;
		mastodon_status_show(ic, ms);
		ms_free(ms);
	}
}

/* Let the user know if a status they have recently seen was deleted. If we can't find the deleted status in our list of
 * recently seen statuses, ignore the event. */
static void mastodon_stream_handle_delete(struct im_connection *ic, json_value *parsed)
{
	struct mastodon_data *md = ic->proto_data;
	guint64 id = mastodon_json_int64(parsed);
	if (id) {
		int i;
		for (i = 0; i < MASTODON_LOG_LENGTH; i++) {
			if (md->log[i].id == id) {
				mastodon_log(ic, "Status %02x was deleted.", i);
				md->log[i].id = 0; // prevent future references
				return;
			}
		}
	} else {
		mastodon_log(ic, "Error parsing a deletion event.");
	}
}

static void mastodon_stream_handle_event(struct im_connection *ic, mastodon_evt_flags_t evt_type,
					 json_value *parsed, mastodon_timeline_type_t subscription)
{
	if (evt_type == MASTODON_EVT_UPDATE) {
		mastodon_stream_handle_update(ic, parsed, subscription);
	} else if (evt_type == MASTODON_EVT_NOTIFICATION) {
		mastodon_stream_handle_notification(ic, parsed, subscription);
	} else if (evt_type == MASTODON_EVT_DELETE) {
		mastodon_stream_handle_delete(ic, parsed);
	} else {
		mastodon_log(ic, "Ignoring event type %d", evt_type);
	}
}

/**
 * When streaming, we also want to tag the events appropriately. This only affects updates, for now.
 */
static void mastodon_http_stream(struct http_request *req, mastodon_timeline_type_t subscription)
{
	struct im_connection *ic = req->data;
	struct mastodon_data *md = ic->proto_data;
	int len = 0;
	char *nl;

	if (!g_slist_find(mastodon_connections, ic)) {
		return;
	}

	if ((req->flags & HTTPC_EOF) || !req->reply_body) {
		md->streams = g_slist_remove (md->streams, req);
		imcb_error(ic, "Stream closed (%s)", req->status_string);
		imc_logout(ic, TRUE);
		return;
	}

	/* It doesn't matter which stream sent us something. */
	ic->flags |= OPT_PONGED;

	/*
https://github.com/tootsuite/documentation/blob/master/Using-the-API/Streaming-API.md
https://developer.mozilla.org/en-US/docs/Web/API/Server-sent_events/Using_server-sent_events#Event_stream_format
	*/

	if (req->reply_body[0] == ':' &&
	    (nl = strchr(req->reply_body, '\n'))) {
		// found a comment such as the heartbeat ":thump\n"
		len = nl - req->reply_body + 1;
		goto end;
	} else if (!(nl = strstr(req->reply_body, "\n\n"))) {
		// wait until we have a complete event
		return;
	}

	// include the two newlines at the end
	len = nl - req->reply_body + 2;

	if (len > 0) {
		char *p;
		mastodon_evt_flags_t evt_type = MASTODON_EVT_UNKNOWN;

		// assuming space after colon
		if (strncmp(req->reply_body, "event: ", 7) == 0) {
			p = req->reply_body + 7;
			if (strncmp(p, "update\n", 7) == 0) {
				evt_type = MASTODON_EVT_UPDATE;
				p += 7;
			} else if (strncmp(p, "notification\n", 13) == 0) {
				evt_type = MASTODON_EVT_NOTIFICATION;
				p += 13;
			} else if (strncmp(p, "delete\n", 7) == 0) {
				evt_type = MASTODON_EVT_DELETE;
				p += 7;
			}
		}

		if (evt_type != MASTODON_EVT_UNKNOWN) {

			GString *data = g_string_new("");
			char* q;

			while (strncmp(p, "data: ", 6) == 0) {
				p += 6;
				q = strchr(p, '\n');
				p[q-p] = '\0';
				g_string_append(data, p);
				p = q + 1;
			}

			json_value *parsed;
			if ((parsed = json_parse(data->str, data->len))) {
				mastodon_stream_handle_event(ic, evt_type, parsed, subscription);
				json_value_free(parsed);
			}

			g_string_free(data, TRUE);
		}
	}

end:
	http_flush_bytes(req, len);

	/* We might have multiple events */
	if (req->body_size > 0) {
		mastodon_http_stream(req, subscription);
	}
}

static void mastodon_http_stream_user(struct http_request *req)
{
	mastodon_http_stream(req, MT_HOME);
}

static void mastodon_http_stream_hashtag(struct http_request *req)
{
	mastodon_http_stream(req, MT_HASHTAG);
}

static void mastodon_http_stream_local(struct http_request *req)
{
	mastodon_http_stream(req, MT_LOCAL);
}

static void mastodon_http_stream_federated(struct http_request *req)
{
	mastodon_http_stream(req, MT_FEDERATED);
}

static void mastodon_http_stream_list(struct http_request *req)
{
	mastodon_http_stream(req, MT_LIST);
}

/**
 * Make sure a request continues stream instead of closing.
 */
void mastodon_stream(struct im_connection *ic, struct http_request *req)
{
	struct mastodon_data *md = ic->proto_data;
	if (req) {
		req->flags |= HTTPC_STREAMING;
		md->streams = g_slist_prepend(md->streams, req);
	}
}

/**
 * Open the user (home) timeline.
 */
void mastodon_open_user_stream(struct im_connection *ic)
{
	struct http_request *req = mastodon_http(ic, MASTODON_STREAMING_USER_URL,
						 mastodon_http_stream_user, ic, HTTP_GET, NULL, 0);
	mastodon_stream(ic, req);
}

/**
 * Open a stream for a hashtag timeline and return the request.
 */
struct http_request *mastodon_open_hashtag_stream(struct im_connection *ic, char *hashtag)
{
	char *args[2] = {
		"tag", hashtag,
	};

	struct http_request *req = mastodon_http(ic, MASTODON_STREAMING_HASHTAG_URL,
						 mastodon_http_stream_hashtag, ic, HTTP_GET, args, 2);
	mastodon_stream(ic, req);
	return req;
}

/**
 * Part two of the first callback: now we have mc->id. Now we're good to go.
 */
void mastodon_list_stream(struct im_connection *ic, struct mastodon_command *mc) {
	char *args[2] = {
		"list", g_strdup_printf("%" G_GINT64_FORMAT, mc->id),
	};

	struct http_request *req = mastodon_http(ic, MASTODON_STREAMING_LIST_URL,
						 mastodon_http_stream_list, ic, HTTP_GET, args, 2);
	mastodon_stream(ic, req);
	/* We cannot return req here because this is a callback (as we had to figure out the list id before getting here).
	 * This is why we must rely on the groupchat being part of mastodon_command (mc). */
	mc->c->data = req;
}

/**
 * First callback to show the stream for a list. We need to parse the lists and find the one we're looking for, then
 * make our next request with the list id.
 */
static void mastodon_http_list_stream(struct http_request *req)
{
	mastodon_chained_list(req, mastodon_list_stream);
}

void mastodon_open_unknown_list_stream(struct im_connection *ic, struct groupchat *c, char *title)
{
	struct mastodon_command *mc = g_new0(struct mastodon_command, 1);
	mc->ic = ic;
	mc->c = c;
	mc->str = g_strdup(title);
	mastodon_with_named_list(ic, mc, mastodon_http_list_stream);
}

/**
 * Open a stream for the local timeline and return the request.
 */
struct http_request *mastodon_open_local_stream(struct im_connection *ic)
{
	struct http_request *req = mastodon_http(ic, MASTODON_STREAMING_LOCAL_URL,
						 mastodon_http_stream_local, ic, HTTP_GET, NULL, 0);
	mastodon_stream(ic, req);
	return req;
}

/**
 * Open a stream for the federated timeline and return the request.
 */
struct http_request *mastodon_open_federated_stream(struct im_connection *ic)
{
	struct http_request *req = mastodon_http(ic, MASTODON_STREAMING_FEDERATED_URL,
						 mastodon_http_stream_federated, ic, HTTP_GET, NULL, 0);
	mastodon_stream(ic, req);
	return req;
}

/**
 * Look for the Link header and remember the URL to the next page of results. This will be used by the "more" command.
 */
static void mastodon_handle_header(struct http_request *req, mastodon_more_t more_type)
{
	struct im_connection *ic = req->data;

	// remember the URL to fetch more if there is a header saying that there is more (URL in angled brackets)
	char *header = NULL;
	if ((header = get_rfc822_header(req->reply_headers, "Link", 0))) {
		char *url = NULL;
		gboolean next = FALSE;
		int i;
		for (i = 0; header[i]; i++) {
			if (header[i] == '<') {
				url = header + i + 1;
			} else if (url && header[i] == '>') {
				header[i] = 0;
				if (strncmp(header + i + 1, "; rel=\"next\"", 12) == 0) {
					next = TRUE;
					break;
				} else {
					url = NULL;
				}
			}
		}

		struct mastodon_data *md = ic->proto_data;
		g_free(md->next_url); md->next_url = NULL;
		if (next) md->next_url = g_strdup(url);
		md->more_type = more_type;

		g_free(header);
	}
}

/**
 * Handle a request whose response contains nothing but statuses. Note that we expect req->data to be an im_connection,
 * not a mastodon_command (or NULL).
 */
static void mastodon_http_timeline(struct http_request *req, mastodon_timeline_type_t subscription)
{
	struct im_connection *ic = req->data;
	if (!g_slist_find(mastodon_connections, ic)) {
		return;
	}

	json_value *parsed;
	if (!(parsed = mastodon_parse_response(ic, req))) {
		/* ic would have been freed in imc_logout in this situation */
		ic = NULL;
		return;
	}

	if (parsed->type != json_array || parsed->u.array.length == 0) {
		mastodon_log(ic, "No statuses found in this timeline.");
		goto finish;
	}

	mastodon_handle_header(req, MASTODON_MORE_STATUSES);

	// Show in reverse order!
	int i;
	for (i = parsed->u.array.length - 1; i >= 0 ; i--) {
		json_value *node = parsed->u.array.values[i];
		struct mastodon_status *ms = mastodon_xt_get_status(node, ic);
		if (ms) {
			ms->subscription = subscription;
			mastodon_status_show(ic, ms);
			ms_free(ms);
		}
	}
finish:
	json_value_free(parsed);
}

static void mastodon_http_hashtag_timeline(struct http_request *req)
{
	mastodon_http_timeline(req, MT_HASHTAG);
}

void mastodon_hashtag_timeline(struct im_connection *ic, char *hashtag)
{
	char *url = g_strdup_printf(MASTODON_HASHTAG_TIMELINE_URL, hashtag);
	mastodon_http(ic, url, mastodon_http_hashtag_timeline, ic, HTTP_GET, NULL, 0);
	g_free(url);
}

static void mastodon_http_home_timeline(struct http_request *req)
{
	mastodon_http_timeline(req, MT_HOME);
}

void mastodon_home_timeline(struct im_connection *ic)
{
	mastodon_http(ic, MASTODON_HOME_TIMELINE_URL, mastodon_http_home_timeline, ic, HTTP_GET, NULL, 0);
}

static void mastodon_http_local_timeline(struct http_request *req)
{
	mastodon_http_timeline(req, MT_LOCAL);
}

void mastodon_local_timeline(struct im_connection *ic)
{
	char *args[2] = {
		"local", "1",
	};

	mastodon_http(ic, MASTODON_PUBLIC_TIMELINE_URL, mastodon_http_local_timeline, ic, HTTP_GET, args, 2);
}

static void mastodon_http_federated_timeline(struct http_request *req)
{
	mastodon_http_timeline(req, MT_FEDERATED);
}

void mastodon_federated_timeline(struct im_connection *ic)
{
	mastodon_http(ic, MASTODON_PUBLIC_TIMELINE_URL, mastodon_http_federated_timeline, ic, HTTP_GET, NULL, 0);
}


/**
 * Second callback to show the timeline for a list. We finally got a list of statuses.
 */
static void mastodon_http_list_timeline2(struct http_request *req)
{
	/* We have used a mastodon_command (mc) all this time, but now it's time to forget about it and use an im_connection
	 * (ic) instead. */
	struct mastodon_command *mc = req->data;
	struct im_connection *ic = mc->ic;
	req->data = ic;
	mc_free(mc);
	mastodon_http_timeline(req, MT_LIST);
}

/**
 * Part two of the first callback to show the timeline for a list. In mc->id we have our list id.
 */
void mastodon_list_timeline(struct im_connection *ic, struct mastodon_command *mc) {
	char *url = g_strdup_printf(MASTODON_LIST_TIMELINE_URL, mc->id);
	mastodon_http(ic, url, mastodon_http_list_timeline2, mc, HTTP_GET, NULL, 0);
	g_free(url);
}

/**
 * First callback to show the timeline for a list. We need to parse the lists and find the one we're looking for, then
 * make our next request with the list id.
 */
static void mastodon_http_list_timeline(struct http_request *req)
{
	mastodon_chained_list(req, mastodon_list_timeline);
}

/**
 * Timeline for a named list. This requires two callbacks: the first to find the list id, the second one to do the
 * actual work.
 */
void mastodon_unknown_list_timeline(struct im_connection *ic, char *title)
{
	struct mastodon_command *mc = g_new0(struct mastodon_command, 1);
	mc->ic = ic;
	mc->str = g_strdup(title);
	mastodon_with_named_list(ic, mc, mastodon_http_list_timeline);
}

/**
 * Call this one after receiving timeline/notifications. Show to user
 * once we have both.
 */
void mastodon_flush_timeline(struct im_connection *ic)
{
	struct mastodon_data *md = ic->proto_data;
	struct mastodon_list *home_timeline;
	struct mastodon_list *notifications;
	GSList *output = NULL;
	GSList *l;

	if (md == NULL) {
		return;
	}
	home_timeline = md->home_timeline_obj;
	notifications = md->notifications_obj;

	imcb_connected(ic);

	if (!(md->flags & MASTODON_GOT_TIMELINE) ||
	    !(md->flags & MASTODON_GOT_NOTIFICATIONS)) {
		return;
	}

	if (home_timeline && home_timeline->list) {
		for (l = home_timeline->list; l; l = g_slist_next(l)) {
			output = g_slist_insert_sorted(output, l->data, mastodon_compare_elements);
		}
	}

	if (notifications && notifications->list) {
		for (l = notifications->list; l; l = g_slist_next(l)) {
			// Skip notifications older than the earliest entry in the timeline.
			struct mastodon_status *ms = mastodon_notification_to_status((struct mastodon_notification *) l->data);
			if (output && mastodon_compare_elements(ms, output->data) < 0) {
				continue;
			}

			output = g_slist_insert_sorted(output, ms, mastodon_compare_elements);
		}
	}

	while (output) {
		struct mastodon_status *ms = output->data;
		mastodon_status_show(ic, ms);
		output = g_slist_remove(output, ms);
	}

	ml_free(home_timeline);
	ml_free(notifications);
	g_slist_free(output);

	md->flags &= ~(MASTODON_GOT_TIMELINE | MASTODON_GOT_NOTIFICATIONS);
	md->home_timeline_obj = md->notifications_obj = NULL;
}

/**
 * Callback for getting the home timeline. This runs in parallel to
 * getting the notifications.
 */
static void mastodon_http_get_home_timeline(struct http_request *req)
{
	struct im_connection *ic = req->data;
	if (!g_slist_find(mastodon_connections, ic)) {
		return;
	}

	struct mastodon_data *md = ic->proto_data;

	json_value *parsed;
	if (!(parsed = mastodon_parse_response(ic, req))) {
		/* ic would have been freed in imc_logout in this situation */
		ic = NULL;
		return;
	}

	struct mastodon_list *ml = g_new0(struct mastodon_list, 1);

	mastodon_xt_get_status_list(ic, parsed, ml);
	json_value_free(parsed);

	md->home_timeline_obj = ml;
	md->flags |= MASTODON_GOT_TIMELINE;
	mastodon_flush_timeline(ic);
}

/**
 * Callback for getting the notifications. This runs in parallel to
 * getting the home timeline.
 */
static void mastodon_http_get_notifications(struct http_request *req)
{
	struct im_connection *ic = req->data;
	if (!g_slist_find(mastodon_connections, ic)) {
		return;
	}

	struct mastodon_data *md = ic->proto_data;

	json_value *parsed;
	if (!(parsed = mastodon_parse_response(ic, req))) {
		/* ic would have been freed in imc_logout in this situation */
		ic = NULL;
		return;
	}

	struct mastodon_list *ml = g_new0(struct mastodon_list, 1);

	mastodon_xt_get_notification_list(ic, parsed, ml);
	json_value_free(parsed);

	md->notifications_obj = ml;
	md->flags |= MASTODON_GOT_NOTIFICATIONS;
	mastodon_flush_timeline(ic);
}

/**
 * See mastodon_initial_timeline.
 */
static void mastodon_get_home_timeline(struct im_connection *ic)
{
	struct mastodon_data *md = ic->proto_data;

	ml_free(md->home_timeline_obj);
	md->home_timeline_obj = NULL;
	md->flags &= ~MASTODON_GOT_TIMELINE;

	mastodon_http(ic, MASTODON_HOME_TIMELINE_URL, mastodon_http_get_home_timeline, ic, HTTP_GET, NULL, 0);
}

/**
 * See mastodon_initial_timeline.
 */
static void mastodon_get_notifications(struct im_connection *ic)
{
	struct mastodon_data *md = ic->proto_data;

	ml_free(md->notifications_obj);
	md->notifications_obj = NULL;
	md->flags &= ~MASTODON_GOT_NOTIFICATIONS;

	mastodon_http(ic, MASTODON_NOTIFICATIONS_URL, mastodon_http_get_notifications, ic, HTTP_GET, NULL, 0);
}

/**
 * Get the initial timeline. This consists of two things: the home
 * timeline, and notifications. During normal use, these are provided
 * via the Streaming API. However, when we connect to an instance we
 * want to load the home timeline and notifications. In order to sort
 * them in a meaningful way, we use these flags:
 * MASTODON_GOT_TIMELINE to indicate that we now have home timeline,
 * MASTODON_GOT_NOTIFICATIONS to indicate that we now have notifications.
 * Both callbacks will attempt to flush the initial timeline, but this
 * will only succeed if both flags are set.
 */
void mastodon_initial_timeline(struct im_connection *ic)
{
	imcb_log(ic, "Getting home timeline");
	mastodon_get_home_timeline(ic);
	mastodon_get_notifications(ic);
	return;
}

/**
 * Callback for getting notifications manually.
 */
static void mastodon_http_notifications(struct http_request *req)
{
	struct im_connection *ic = req->data;
	if (!g_slist_find(mastodon_connections, ic)) {
		return;
	}

	json_value *parsed;
	if (!(parsed = mastodon_parse_response(ic, req))) {
		/* ic would have been freed in imc_logout in this situation */
		ic = NULL;
		return;
	}

	if (parsed->type != json_array || parsed->u.array.length == 0) {
		mastodon_log(ic, "No notifications found.");
		goto finish;
	}

	mastodon_handle_header(req, MASTODON_MORE_NOTIFICATIONS);

	// Show in reverse order!
	int i;
	for (i = parsed->u.array.length - 1; i >= 0 ; i--) {
		json_value *node = parsed->u.array.values[i];
		struct mastodon_notification *mn = mastodon_xt_get_notification(node, ic);
		if (mn) {
			mastodon_notification_show(ic, mn);
			mn_free(mn);
		}
	}
finish:
	json_value_free(parsed);
}

/**
 * Notifications are usually shown by the Streaming API, and when showing the initial timeline after connecting. In
 * order to allow manual review (and going through past notifications using the more command, we need yet another way to
 * get notifications.
 */
void mastodon_notifications(struct im_connection *ic)
{
	mastodon_http(ic, MASTODON_NOTIFICATIONS_URL, mastodon_http_notifications, ic, HTTP_GET, NULL, 0);
}

mastodon_visibility_t mastodon_default_visibility(struct im_connection *ic)
{
	return mastodon_parse_visibility(set_getstr(&ic->acc->set, "visibility"));
}

char *mastodon_visibility(mastodon_visibility_t visibility)
{
	switch (visibility) {
	case MV_UNKNOWN:
	case MV_PUBLIC:
		return "public";
	case MV_UNLISTED:
		return "unlisted";
	case MV_PRIVATE:
		return "private";
	case MV_DIRECT:
		return "direct";
	}
	g_assert(FALSE); // should not happen
	return NULL;
}

/**
 * Generic callback to use after sending a POST request to mastodon when the reply doesn't have any information we need.
 * All we care about are errors. If got here, there was no error. If you want to tell the user that everything went
 * fine, call mastodon_http_callback_and_ack instead. This command also stores some information for later use.
 */
static void mastodon_http_callback(struct http_request *req)
{
	struct mastodon_command *mc = req->data;
	struct im_connection *ic = mc->ic;
	if (!g_slist_find(mastodon_connections, ic)) {
		return;
	}

	json_value *parsed;
	if (!(parsed = mastodon_parse_response(ic, req))) {
		/* ic would have been freed in imc_logout in this situation */
		ic = NULL;
		return;
	}

	/* Store stuff in the undo/redo stack. */
	struct mastodon_data *md = ic->proto_data;
	md->last_id = 0;

	struct mastodon_status *ms;

	switch (mc->command) {
	case MC_UNKNOWN:
		break;
	case MC_POST:
		ms = mastodon_xt_get_status(parsed, ic);
		gint64 id = set_getint(&ic->acc->set, "account_id");
		if (ms && ms->id && ms->account->id == id) {
			/* we posted this status */
			md->last_id = ms->id;
			md->last_visibility = ms->visibility;
			g_free(md->last_spoiler_text);
			md->last_spoiler_text = ms->spoiler_text; // adopt
			ms->spoiler_text = NULL;
			g_slist_free_full(md->mentions, g_free);
			md->mentions = ms->mentions; // adopt
			ms->mentions = NULL;

			if(md->undo_type == MASTODON_NEW) {

				GString *todo = g_string_new (NULL);
				char *undo = g_strdup_printf("delete %" G_GUINT64_FORMAT, ms->id);

				/* At this point redoing the reply no longer has the reference to the toot we are replying to (which
				 * only works by looking it up in the mastodon_user_data (mud) or the md->log). That is why we need
				 * to add spoiler_text and visibility to the todo item on our redo list. */

				if (ms->spoiler_text) {
					g_string_append_printf(todo, "cw %s" FS, ms->spoiler_text);
				} else {
					g_string_append(todo, "cw" FS);
				}

				if (mastodon_default_visibility(ic) != ms->visibility) {
					g_string_append_printf(todo, "visibility %s" FS, mastodon_visibility(ms->visibility));
				} else {
					g_string_append(todo, "visibility" FS);
				}

				if (ms->reply_to) {
					g_string_append_printf(todo, "reply %" G_GUINT64_FORMAT " ", ms->reply_to);
				} else {
					g_string_append(todo, "post ");
				}
				g_string_append(todo, ms->content);

				mastodon_do(ic, todo->str, undo);

				g_string_free(todo, FALSE); /* data is kept by mastodon_do! */

			} else {
				char *s = g_strdup_printf("delete %" G_GUINT64_FORMAT, ms->id);
				mastodon_do_update(ic, s);
				g_free(s);
			}
		}
		break;
	case MC_FOLLOW:
	case MC_UNFOLLOW:
	case MC_BLOCK:
	case MC_UNBLOCK:
	case MC_FAVOURITE:
	case MC_UNFAVOURITE:
	case MC_PIN:
	case MC_UNPIN:
	case MC_ACCOUNT_MUTE:
	case MC_ACCOUNT_UNMUTE:
	case MC_STATUS_MUTE:
	case MC_STATUS_UNMUTE:
	case MC_BOOST:
	case MC_UNBOOST:
	case MC_LIST_CREATE:
	case MC_LIST_DELETE:
	case MC_LIST_ADD_ACCOUNT:
	case MC_LIST_REMOVE_ACCOUNT:
	case MC_DELETE:
		md->last_id = 0;
		mastodon_do(ic, mc->redo, mc->undo);
		// adopting these strings: do not free them at the end
		mc->redo = mc->undo = 0;
		break;
	}
	mc_free(mc);
	json_value_free(parsed);
}

/**
 * Call the generic callback function and print an acknowledgement for the user.
 * Commands should use mastodon_post instead.
 */
static void mastodon_http_callback_and_ack(struct http_request *req)
{
	struct mastodon_command *mc = req->data;
	struct im_connection *ic = mc->ic;
	mastodon_http_callback(req); // this frees mc

	if (req->status_code == 200) {
		mastodon_log(ic, "Command processed successfully");
	}
}

/**
 * Return a static string n spaces long. No deallocation needed.
 */
static char *indent(int n)
{
	char *spaces = "          ";
	int len = 10;
	return n > len ? spaces : spaces + len - n;
}

/**
 * Return a static yes or no string. No deallocation needed.
 */
static char *yes_or_no(int bool)
{
	return bool ? "yes" : "no";
}

/**
 * Log a JSON array out to the channel. When you call it, use a
 * prefix of 0. Recursive calls will then indent nested objects.
 */
static void mastodon_log_array(struct im_connection *ic, json_value *node, int prefix)
{
	int i;
	for (i = 0; i < node->u.array.length; i++) {
		json_value *v = node->u.array.values[i];
		char *s;
		switch (v->type) {
		case json_object:
			if (v->u.object.values == 0) {
				mastodon_log(ic, "%s{}", indent(prefix));
				break;
			}
			mastodon_log(ic, "%s{", indent(prefix));
			mastodon_log_object (ic, v, prefix + 1);
			mastodon_log(ic, "%s}", indent(prefix));
			break;
		case json_array:
			if (v->u.array.length == 0) {
				mastodon_log(ic, "%s[]", indent(prefix));
				break;
			}
			mastodon_log(ic, "%s[", indent(prefix));
			int i;
			for (i = 0; i < v->u.array.length; i++) {
				mastodon_log_object (ic, node->u.array.values[i], prefix + 1);
			}
			mastodon_log(ic, "%s]", indent(prefix));
			break;
		case json_string:
			s = g_strdup(v->u.string.ptr);
			mastodon_strip_html(s);
			mastodon_log(ic, "%s%s", indent(prefix), s);
			g_free(s);
			break;
		case json_double:
			mastodon_log(ic, "%s%f", indent(prefix), v->u.dbl);
			break;
		case json_integer:
			mastodon_log(ic, "%s%d", indent(prefix), v->u.boolean);
			break;
		case json_boolean:
			mastodon_log(ic, "%s%s: %s", indent(prefix), yes_or_no(v->u.boolean));
			break;
		case json_null:
			mastodon_log(ic, "%snull", indent(prefix));
			break;
		case json_none:
			mastodon_log(ic, "%snone", indent(prefix));
			break;
		}
	}
}

/**
 * Log a JSON object out to the channel. When you call it, use a
 * prefix of 0. Recursive calls will then indent nested objects.
 */
static void mastodon_log_object(struct im_connection *ic, json_value *node, int prefix)
{
	char *s;
	JSON_O_FOREACH(node, k, v) {
		switch (v->type) {
		case json_object:
			if (v->u.object.values == 0) {
				mastodon_log(ic, "%s%s: {}", indent(prefix), k);
				break;
			}
			mastodon_log(ic, "%s%s: {", indent(prefix), k);
			mastodon_log_object (ic, v, prefix + 1);
			mastodon_log(ic, "%s}", indent(prefix));
			break;
		case json_array:
			if (v->u.array.length == 0) {
				mastodon_log(ic, "%s%s: []", indent(prefix), k);
				break;
			}
			mastodon_log(ic, "%s%s: [", indent(prefix), k);
			mastodon_log_array(ic, v, prefix + 1);
			mastodon_log(ic, "%s]", indent(prefix));
			break;
		case json_string:
			s = g_strdup(v->u.string.ptr);
			mastodon_strip_html(s);
			mastodon_log(ic, "%s%s: %s", indent(prefix), k, s);
			g_free(s);
			break;
		case json_double:
			mastodon_log(ic, "%s%s: %f", indent(prefix), k, v->u.dbl);
			break;
		case json_integer:
			mastodon_log(ic, "%s%s: %d", indent(prefix), k, v->u.boolean);
			break;
		case json_boolean:
			mastodon_log(ic, "%s%s: %s", indent(prefix), k, yes_or_no(v->u.boolean));
			break;
		case json_null:
			mastodon_log(ic, "%s%s: null", indent(prefix), k);
			break;
		case json_none:
			mastodon_log(ic, "%s%s: unknown type", indent(prefix), k);
			break;
		}
	}
}

/**
 * Generic callback which simply logs the JSON response to the
 * channel.
 */
static void mastodon_http_log_all(struct http_request *req)
{
	struct im_connection *ic = req->data;
	if (!g_slist_find(mastodon_connections, ic)) {
		return;
	}

	json_value *parsed;
	if (!(parsed = mastodon_parse_response(ic, req))) {
		/* ic would have been freed in imc_logout in this situation */
		ic = NULL;
		return;
	}

	if (parsed->type == json_object) {
		mastodon_log_object(ic, parsed, 0);
	} else if (parsed->type == json_array) {
		mastodon_log_array(ic, parsed, 0);
	} else {
		mastodon_log(ic, "Sadly, the response to this request is not a JSON object or array.");
	}

	json_value_free(parsed);
}

/**
 * Function to POST a new status to mastodon.
 */
void mastodon_post_status(struct im_connection *ic, char *msg, guint64 in_reply_to, mastodon_visibility_t visibility, char *spoiler_text)
{
	char *args[8] = {
		"status", msg,
		"visibility", mastodon_visibility(visibility),
		"spoiler_text", spoiler_text,
		"in_reply_to_id", g_strdup_printf("%" G_GUINT64_FORMAT, in_reply_to)
	};
	int count = 8;

	struct mastodon_command *mc = g_new0(struct mastodon_command, 1);
	mc->ic = ic;

	mc->command = MC_POST;

	if (!in_reply_to) {
		count -= 2;
	}
	if (!spoiler_text) {
		count -= 2;
		if (in_reply_to) {
			args[4] = args[6];
			args[5] = args[7]; // we have 2 pointers to the in_reply_to_id string now,
		}
	}
	// No need to acknowledge the processing of a post: we will get notified.
	mastodon_http(ic, MASTODON_STATUS_POST_URL, mastodon_http_callback, mc, HTTP_POST,
	             args, count);

	g_free(args[7]); // but we only free one of them!
}

/**
 * Generic POST request taking a numeric ID. The format string must contain one placeholder for the ID, like
 * "/accounts/%" G_GINT64_FORMAT "/mute".
 */
void mastodon_post(struct im_connection *ic, char *format, mastodon_command_type_t command, guint64 id)
{
	struct mastodon_data *md = ic->proto_data;
	struct mastodon_command *mc = g_new0(struct mastodon_command, 1);
	mc->ic = ic;

	if (md->undo_type == MASTODON_NEW) {
		mc->command = command;

		switch (command) {
		case MC_UNKNOWN:
		case MC_POST:
		case MC_DELETE:
		case MC_LIST_CREATE:
		case MC_LIST_DELETE:
		case MC_LIST_ADD_ACCOUNT:
		case MC_LIST_REMOVE_ACCOUNT:
			/* These commands should not be calling mastodon_post. Instead, call mastodon_post_status or whatever else
			 * is required. */
			break;
		case MC_FOLLOW:
			mc->redo = g_strdup_printf("follow %" G_GUINT64_FORMAT, id);
			mc->undo = g_strdup_printf("unfollow %" G_GUINT64_FORMAT, id);
			break;
		case MC_UNFOLLOW:
			mc->redo = g_strdup_printf("unfollow %" G_GUINT64_FORMAT, id);
			mc->undo = g_strdup_printf("follow %" G_GUINT64_FORMAT, id);
			break;
		case MC_BLOCK:
			mc->redo = g_strdup_printf("block %" G_GUINT64_FORMAT, id);
			mc->undo = g_strdup_printf("unblock %" G_GUINT64_FORMAT, id);
			break;
		case MC_UNBLOCK:
			mc->redo = g_strdup_printf("unblock %" G_GUINT64_FORMAT, id);
			mc->undo = g_strdup_printf("block %" G_GUINT64_FORMAT, id);
			break;
		case MC_FAVOURITE:
			mc->redo = g_strdup_printf("favourite %" G_GUINT64_FORMAT, id);
			mc->undo = g_strdup_printf("unfavourite %" G_GUINT64_FORMAT, id);
			break;
		case MC_UNFAVOURITE:
			mc->redo = g_strdup_printf("unfavourite %" G_GUINT64_FORMAT, id);
			mc->undo = g_strdup_printf("favourite %" G_GUINT64_FORMAT, id);
			break;
		case MC_PIN:
			mc->redo = g_strdup_printf("pin %" G_GUINT64_FORMAT, id);
			mc->undo = g_strdup_printf("unpin %" G_GUINT64_FORMAT, id);
			break;
		case MC_UNPIN:
			mc->redo = g_strdup_printf("unpin %" G_GUINT64_FORMAT, id);
			mc->undo = g_strdup_printf("pin %" G_GUINT64_FORMAT, id);
			break;
		case MC_ACCOUNT_MUTE:
			mc->redo = g_strdup_printf("mute user %" G_GUINT64_FORMAT, id);
			mc->undo = g_strdup_printf("unmute user %" G_GUINT64_FORMAT, id);
			break;
		case MC_ACCOUNT_UNMUTE:
			mc->redo = g_strdup_printf("unmute user %" G_GUINT64_FORMAT, id);
			mc->undo = g_strdup_printf("mute user %" G_GUINT64_FORMAT, id);
			break;
		case MC_STATUS_MUTE:
			mc->redo = g_strdup_printf("mute %" G_GUINT64_FORMAT, id);
			mc->undo = g_strdup_printf("unmute %" G_GUINT64_FORMAT, id);
			break;
		case MC_STATUS_UNMUTE:
			mc->redo = g_strdup_printf("unmute %" G_GUINT64_FORMAT, id);
			mc->undo = g_strdup_printf("mute %" G_GUINT64_FORMAT, id);
			break;
		case MC_BOOST:
			mc->redo = g_strdup_printf("boost %" G_GUINT64_FORMAT, id);
			mc->undo = g_strdup_printf("unboost %" G_GUINT64_FORMAT, id);
			break;
		case MC_UNBOOST:
			mc->redo = g_strdup_printf("unboost %" G_GUINT64_FORMAT, id);
			mc->undo = g_strdup_printf("boost %" G_GUINT64_FORMAT, id);
			break;
		}
	}

	char *url = g_strdup_printf(format, id);
	mastodon_http(ic, url, mastodon_http_callback_and_ack, mc, HTTP_POST, NULL, 0);
	g_free(url);
}

void mastodon_http_status_delete(struct http_request *req)
{
	struct mastodon_command *mc = req->data;
	struct im_connection *ic = mc->ic;
	if (!g_slist_find(mastodon_connections, ic)) {
		return;
	}

	json_value *parsed;
	if (!(parsed = mastodon_parse_response(ic, req))) {
		/* ic would have been freed in imc_logout in this situation */
		ic = NULL;
		return;
	}

	/* Maintain undo/redo list. */
	struct mastodon_status *ms = mastodon_xt_get_status(parsed, ic);
	struct mastodon_data *md = ic->proto_data;
	gint64 id = set_getint(&ic->acc->set, "account_id");
	if (ms && ms->id && ms->account->id == id) {
		/* we deleted our own status */
		md->last_id = ms->id;

		mc->redo = g_strdup_printf("delete %" G_GUINT64_FORMAT, ms->id);
		GString *todo = g_string_new (NULL);

		if (ms->spoiler_text) {
			g_string_append_printf(todo, "cw %s" FS, ms->spoiler_text);
		} else {
			g_string_append(todo, "cw" FS);
		}

		if (mastodon_default_visibility(ic) != ms->visibility) {
			g_string_append_printf(todo, "visibility %s" FS, mastodon_visibility(ms->visibility));
		} else {
			g_string_append(todo, "visibility" FS);
		}

		if (ms->reply_to) {
			g_string_append_printf(todo, "reply %" G_GUINT64_FORMAT " ", ms->reply_to);
		} else {
			g_string_append(todo, "post ");
		}
		g_string_append(todo, ms->content);

		mc->undo = todo->str;
		g_string_free(todo, FALSE); /* data is kept by mc! */
	}

	char *url = g_strdup_printf(MASTODON_STATUS_URL, mc->id);
	// No need to acknowledge the processing of the delete: we will get notified.
	mastodon_http(ic, url, mastodon_http_callback, mc, HTTP_DELETE, NULL, 0);
	g_free(url);
}

/**
 * Helper for all functions that need to act on a status before they
 * can do anything else. Provide a function to use as a callback. This
 * callback will get the status back and will need to call
 * mastodon_xt_get_status and do something with it.
 */
void mastodon_with_status(struct mastodon_command *mc, guint64 id, http_input_function func)
{
	char *url = g_strdup_printf(MASTODON_STATUS_URL, id);
	mastodon_http(mc->ic, url, func, mc, HTTP_GET, NULL, 0);
	g_free(url);
}

/**
 * Delete a status. In order to ensure that we can undo and redo this,
 * fetch the status to be deleted before actually deleting it.
 */
void mastodon_status_delete(struct im_connection *ic, guint64 id)
{
	struct mastodon_data *md = ic->proto_data;
	struct mastodon_command *mc = g_new0(struct mastodon_command, 1);
	mc->ic = ic;

	if (md->undo_type == MASTODON_NEW) {
		mc->command = MC_DELETE;
		mc->id = id;
		mastodon_with_status(mc, id, mastodon_http_status_delete);
	} else {
		// Shortcut
		char *url = g_strdup_printf(MASTODON_STATUS_URL, id);
		// No need to acknowledge the processing of the delete: we will get notified.
		mastodon_http(ic, url, mastodon_http_callback, mc, HTTP_DELETE, NULL, 0);
		g_free(url);
	}
}

/**
 * Callback for reporting a user for sending spam.
 */
void mastodon_http_report(struct http_request *req)
{
	struct mastodon_report *mr = req->data;
	struct im_connection *ic = mr->ic;
	if (!g_slist_find(mastodon_connections, ic)) {
		goto finally;
	}

	json_value *parsed;
	if (!(parsed = mastodon_parse_response(ic, req))) {
		/* ic would have been freed in imc_logout in this situation */
		ic = NULL;
		goto finally;
	}

	struct mastodon_status *ms = mastodon_xt_get_status(parsed, ic);
	if (ms) {
		mr->account_id = ms->account->id;
		ms_free(ms);
	} else {
		mastodon_log(ic, "Error: could not fetch toot to report.");
		goto finish;
	}

	char *args[6] = {
		"account_id", g_strdup_printf("%" G_GUINT64_FORMAT, mr->account_id),
		"status_ids", g_strdup_printf("%" G_GUINT64_FORMAT, mr->status_id), // API allows an array, here
		"comment", mr->comment,
	};

	struct mastodon_command *mc = g_new0(struct mastodon_command, 1);
	mc->ic = ic;
	mastodon_http(ic, MASTODON_REPORT_URL, mastodon_http_callback_and_ack, mc, HTTP_POST, args, 6);

	g_free(args[1]);
	g_free(args[3]);
finish:
	ms_free(ms);
	json_value_free(parsed);
finally:
	// The report structure was created by mastodon_report and has
	// to be freed under all circumstances.
	mr_free(mr);
}

/**
 * Report a user. Since all we have is the id of the offending status,
 * we need to retrieve the status, first.
 */
void mastodon_report(struct im_connection *ic, guint64 id, char *comment)
{
	char *url = g_strdup_printf(MASTODON_STATUS_URL, id);
	struct mastodon_report *mr = g_new0(struct mastodon_report, 1);

	mr->ic = ic;
	mr->status_id = id;
	mr->comment = g_strdup(comment);

	mastodon_http(ic, url, mastodon_http_report, mr, HTTP_POST, NULL, 0);
	g_free(url);
}

/**
 * Callback for search.
 */
void mastodon_http_search(struct http_request *req)
{
	struct im_connection *ic = req->data;
	if (!g_slist_find(mastodon_connections, ic)) {
		return;
	}

	json_value *parsed;
	if (!(parsed = mastodon_parse_response(ic, req))) {
		/* ic would have been freed in imc_logout in this situation */
		ic = NULL;
		return;
	}

	json_value *v;
	gboolean found = FALSE;

	/* hashtags  */
	if ((v = json_o_get(parsed, "hashtags")) &&
	    (v->type == json_array) &&
	    (v->u.array.length > 0)) {
		found = TRUE;
		int i;
		for (i = 0; i < v->u.array.length; i++) {
			json_value *s;
			s = v->u.array.values[i];
			if (s->type == json_string) {
				mastodon_log(ic, "#%s", s->u.string.ptr);
			}
		}
	}

	/* accounts */
	if ((v = json_o_get(parsed, "accounts")) &&
	    (v->type == json_array) &&
	    (v->u.array.length > 0)) {
		found = TRUE;
		int i;
		for (i = 0; i < v->u.array.length; i++) {
			json_value *a;
			a = v->u.array.values[i];
			if ((a->type == json_object)) {
				mastodon_log(ic, "@%s %s",
					     json_o_str(a, "acct"),
					     json_o_str(a, "display_name"));
			}
		}
	}

	/* statuses */
	if ((v = json_o_get(parsed, "statuses")) &&
	    (v->type == json_array) &&
	    (v->u.array.length > 0)) {
		found = TRUE;
		struct mastodon_list *ml = g_new0(struct mastodon_list, 1);
		mastodon_xt_get_status_list(ic, v, ml);
		GSList *l;
		for (l = ml->list; l; l = g_slist_next(l)) {
			struct mastodon_status *s = (struct mastodon_status *) l->data;
			mastodon_status_show_chat(ic, s);
		}
		ml_free(ml);
	}

	json_value_free(parsed);

	if (!found) {
		mastodon_log(ic, "Search returned no results on this instance");
	}
}

/**
 * Search for a status URL, account, or hashtag.
 */
void mastodon_search(struct im_connection *ic, char *what)
{
	char *args[2] = {
		"q", what,
	};

	mastodon_http(ic, MASTODON_SEARCH_URL, mastodon_http_search, ic, HTTP_GET, args, 2);
}

/**
 * Show information about the instance.
 */
void mastodon_instance(struct im_connection *ic)
{
	mastodon_http(ic, MASTODON_INSTANCE_URL, mastodon_http_log_all, ic, HTTP_GET, NULL, 0);
}

/**
 * Show information about an account.
 */
void mastodon_account(struct im_connection *ic, guint64 id)
{
	char *url = g_strdup_printf(MASTODON_ACCOUNT_URL, id);
	mastodon_http(ic, url, mastodon_http_log_all, ic, HTTP_GET, NULL, 0);
	g_free(url);
}

/**
 * Helper for all functions that need to search for an account before
 * they can do anything else. Provide a function to use as a callback.
 * This callback will get the account search result back and will need
 * to call mastodon_xt_get_user and do something with it.
 */
void mastodon_with_search_account(struct im_connection *ic, char *who, http_input_function func)
{
	char *args[2] = {
		"q", who,
	};

	mastodon_http(ic, MASTODON_ACCOUNT_SEARCH_URL, func, ic, HTTP_GET, args, 2);
}

/**
 * Show debug information for an account.
 */
void mastodon_search_account(struct im_connection *ic, char *who)
{
	mastodon_with_search_account(ic, who, mastodon_http_log_all);
}

/**
 * Show debug information for the relationship with an account.
 */
void mastodon_relationship(struct im_connection *ic, guint64 id)
{
	char *args[2] = {
		"id", g_strdup_printf("%" G_GUINT64_FORMAT, id),
	};

	mastodon_http(ic, MASTODON_ACCOUNT_RELATIONSHIP_URL, mastodon_http_log_all, ic, HTTP_GET, args, 2);
	g_free(args[1]);
}

/**
 * Callback to print debug information about a relationship.
 */
static void mastodon_http_search_relationship(struct http_request *req)
{
	struct im_connection *ic = req->data;
	if (!g_slist_find(mastodon_connections, ic)) {
		return;
	}

	json_value *parsed;
	if (!(parsed = mastodon_parse_response(ic, req))) {
		/* ic would have been freed in imc_logout in this situation */
		ic = NULL;
		return;
	}

	struct mastodon_account *ma = mastodon_xt_get_user(parsed);

	if (!ma) {
		mastodon_log(ic, "Couldn't find a matching account.");
		goto finish;
	}

	char *args[2] = {
		"id", g_strdup_printf("%" G_GUINT64_FORMAT, ma->id),
	};

	mastodon_http(ic, MASTODON_ACCOUNT_RELATIONSHIP_URL, mastodon_http_log_all, ic, HTTP_GET, args, 2);

	g_free(args[1]);
finish:
	ma_free(ma);
	json_value_free(parsed);
}

/**
 * Search for an account and and show debug information for the
 * relationship with the first account found.
 */
void mastodon_search_relationship(struct im_connection *ic, char *who)
{
	mastodon_with_search_account(ic, who, mastodon_http_search_relationship);
}

/**
 * Show debug information for a status.
 */
void mastodon_status(struct im_connection *ic, guint64 id)
{
	char *url = g_strdup_printf(MASTODON_STATUS_URL, id);
	mastodon_http(ic, url, mastodon_http_log_all, ic, HTTP_GET, NULL, 0);
	g_free(url);
}

/**
 * Allow the user to make a raw request.
 */
void mastodon_raw(struct im_connection *ic, char *method, char *url, char **arguments, int arguments_len) {
	http_method_t m = HTTP_GET;
	if (g_ascii_strcasecmp(method, "get") == 0) {
		m = HTTP_GET;
	} else if (g_ascii_strcasecmp(method, "put") == 0) {
		m = HTTP_PUT;
	} else if (g_ascii_strcasecmp(method, "post") == 0) {
		m = HTTP_POST;
	} else if (g_ascii_strcasecmp(method, "delete") == 0) {
		m = HTTP_DELETE;
	}
	mastodon_http(ic, url, mastodon_http_log_all, ic, m, arguments, arguments_len);
}

/**
 * Callback for showing the URL of a status.
 */
static void mastodon_http_status_show_url(struct http_request *req)
{
	struct im_connection *ic = req->data;
	if (!g_slist_find(mastodon_connections, ic)) {
		return;
	}

	json_value *parsed;
	if (!(parsed = mastodon_parse_response(ic, req))) {
		/* ic would have been freed in imc_logout in this situation */
		ic = NULL;
		return;
	}

	struct mastodon_status *ms = mastodon_xt_get_status(parsed, ic);
	if (ms) {
		mastodon_log(ic, ms->url);
		ms_free(ms);
	} else {
		mastodon_log(ic, "Error: could not fetch toot url.");
	}

	json_value_free(parsed);
}

/**
 * Show the URL for a status.
 */
void mastodon_status_show_url(struct im_connection *ic, guint64 id)
{
	char *url = g_strdup_printf(MASTODON_STATUS_URL, id);
	mastodon_http(ic, url, mastodon_http_status_show_url, ic, HTTP_GET, NULL, 0);
	g_free(url);
}

/**
 * Append a string data to a gstring user_data, separated with a space, if necessary. This is to be used with
 * g_list_foreach(). The prefix "@" is added in front of every element.
 */
static void mastodon_string_append(gchar *data, GString *user_data)
{
	if (user_data->len > 0) {
		g_string_append(user_data, " ");
	}
	g_string_append(user_data, "@");
	g_string_append(user_data, data);
}

/**
 * Join all the strings in a list, space-separated. Be sure to free the returned GString with g_string_free(). If there
 * is no initial element for the list, use NULL for the second argument. The prefix "@" is added in front of every
 * element. It is added to the initial element, too! This is used to generated a list of accounts to mention in a toot.
 */
GString *mastodon_string_join(GSList *l, gchar *init)
{
	if (!l && !init) return NULL;
	GString *s = g_string_new(NULL);
	if (init) {
		g_string_append(s, "@");
		g_string_append(s, init);
	}
	g_slist_foreach(l, (GFunc) mastodon_string_append, s);
	return s;
}

/**
 * Show the list of mentions for a status in our log data.
 */
void mastodon_show_mentions(struct im_connection *ic, GSList *l)
{
	if (l) {
		GString *s = mastodon_string_join(l, NULL);
		mastodon_log(ic, "Mentioned: %s", s->str);
		g_string_free(s, TRUE);
	} else {
		mastodon_log(ic, "Nobody was mentioned in this toot");
	}
}

/**
 * Callback for showing the mentions of a status.
 */
static void mastodon_http_status_show_mentions(struct http_request *req)
{
	struct im_connection *ic = req->data;
	if (!g_slist_find(mastodon_connections, ic)) {
		return;
	}

	json_value *parsed;
	if (!(parsed = mastodon_parse_response(ic, req))) {
		/* ic would have been freed in imc_logout in this situation */
		ic = NULL;
		return;
	}

	struct mastodon_status *ms = mastodon_xt_get_status(parsed, ic);
	if (ms) {
		mastodon_show_mentions(ic, ms->mentions);
		ms_free(ms);
	} else {
		mastodon_log(ic, "Error: could not fetch toot url.");
	}

	json_value_free(parsed);
}

/**
 * Show the mentions for a status.
 */
void mastodon_status_show_mentions(struct im_connection *ic, guint64 id)
{
	char *url = g_strdup_printf(MASTODON_STATUS_URL, id);
	mastodon_http(ic, url, mastodon_http_status_show_mentions, ic, HTTP_GET, NULL, 0);
	g_free(url);
}

/**
 * Attempt to flush the context data. This is called by the two
 * callbacks for the context request because we need to wait for two
 * responses: the original status details, and the context itself.
 */
void mastodon_flush_context(struct im_connection *ic)
{
	struct mastodon_data *md = ic->proto_data;

	if (!(md->flags & MASTODON_GOT_STATUS) ||
	    !(md->flags & MASTODON_GOT_CONTEXT)) {
		return;
	}

	struct mastodon_status *ms = md->status_obj;
	struct mastodon_list *bl = md->context_before_obj;
	struct mastodon_list *al = md->context_after_obj;
	GSList *l;

	for (l = bl->list; l; l = g_slist_next(l)) {
		struct mastodon_status *s = (struct mastodon_status *) l->data;
		mastodon_status_show_chat(ic, s);
	}

	mastodon_status_show_chat(ic, ms);

	for (l = al->list; l; l = g_slist_next(l)) {
		struct mastodon_status *s = (struct mastodon_status *) l->data;
		mastodon_status_show_chat(ic, s);
	}

	ml_free(al);
	ml_free(bl);
	ms_free(ms);

	md->flags &= ~(MASTODON_GOT_TIMELINE | MASTODON_GOT_NOTIFICATIONS);
	md->status_obj = md->context_before_obj = md->context_after_obj = NULL;
}

/**
 * Callback for the context of a status. Store it in our mastodon data
 * structure and attempt to flush it.
 */
void mastodon_http_context(struct http_request *req)
{
	struct im_connection *ic = req->data;
	if (!g_slist_find(mastodon_connections, ic)) {
		return;
	}

	struct mastodon_data *md = ic->proto_data;

	json_value *parsed;
	if (!(parsed = mastodon_parse_response(ic, req))) {
		/* ic would have been freed in imc_logout in this situation */
		ic = NULL;
		goto end;
	}

	if (parsed->type != json_object) {
		goto finished;
	}

	struct mastodon_list *bl = g_new0(struct mastodon_list, 1);
	struct mastodon_list *al = g_new0(struct mastodon_list, 1);

	json_value *before = json_o_get(parsed, "ancestors");
	json_value *after  = json_o_get(parsed, "descendants");

	if (before->type == json_array &&
	    mastodon_xt_get_status_list(ic, before, bl)) {
		md->context_before_obj = bl;
	}

	if (after->type == json_array &&
	    mastodon_xt_get_status_list(ic, after, al)) {
		md->context_after_obj = al;
	}
finished:
	json_value_free(parsed);
end:
	if (ic) {
		md->flags |= MASTODON_GOT_CONTEXT;
		mastodon_flush_context(ic);
	}
}

/**
 * Callback for the original status as part of a context request.
 * Store it in our mastodon data structure and attempt to flush it.
 */
void mastodon_http_context_status(struct http_request *req)
{
	struct im_connection *ic = req->data;
	if (!g_slist_find(mastodon_connections, ic)) {
		return;
	}

	struct mastodon_data *md = ic->proto_data;

	json_value *parsed;
	if (!(parsed = mastodon_parse_response(ic, req))) {
		/* ic would have been freed in imc_logout in this situation */
		ic = NULL;
		goto end;
	}

	md->status_obj = mastodon_xt_get_status(parsed, ic);

	json_value_free(parsed);
end:
	if (ic) {
		md->flags |= MASTODON_GOT_STATUS;
		mastodon_flush_context(ic);
	}
}

/**
 * Search for a status and its context. The problem is that the
 * context doesn't include the status we're interested in. That's why
 * we must make two requests and wait until we get the response to
 * both.
 */
void mastodon_context(struct im_connection *ic, guint64 id)
{
	struct mastodon_data *md = ic->proto_data;

	ms_free(md->status_obj);
	ml_free(md->context_before_obj);
	ml_free(md->context_after_obj);

	md->status_obj = md->context_before_obj = md->context_after_obj = NULL;

	md->flags &= ~(MASTODON_GOT_STATUS | MASTODON_GOT_CONTEXT);

	char *url = g_strdup_printf(MASTODON_STATUS_CONTEXT_URL, id);
	mastodon_http(ic, url, mastodon_http_context, ic, HTTP_GET, NULL, 0);
	g_free(url);

	url = g_strdup_printf(MASTODON_STATUS_URL, id);
	mastodon_http(ic, url, mastodon_http_context_status, ic, HTTP_GET, NULL, 0);
	g_free(url);
}


/**
 * The callback functions for mastodon_http_statuses_chain() should look like this, e.g. mastodon_account_statuses or
 * mastodon_account_pinned_statuses. The way this works: If you know the id of an account, call the function directly:
 * mastodon_account_statuses(). The callback for this is mastodon_http_statuses() which uses mastodon_http_timeline() to
 * display the statuses. If you don't know the id of an account by you know the "who" of the account, call a function
 * like mastodon_unknown_account_statuses(). It calls mastodon_with_search_account() and the callback for this is
 * mastodon_http_unknown_account_statuses(). The http_request it gets has the account(s) you need. Call
 * mastodon_chained_account() which parses the request and determines the actual account id. Finally, it calls the last
 * callback, which mastodon_account_statuses(), with the account id. And we already know how that works! Phew!!
 *
 */
typedef void (*mastodon_chained_account_function)(struct im_connection *ic, guint64 id);

void mastodon_chained_account(struct http_request *req, mastodon_chained_account_function func)
{
	struct im_connection *ic = req->data;
	if (!g_slist_find(mastodon_connections, ic)) {
		return;
	}

	json_value *parsed;
	if (!(parsed = mastodon_parse_response(ic, req))) {
		/* ic would have been freed in imc_logout in this situation */
		ic = NULL;
		return;
	}

	if (parsed->type != json_array || parsed->u.array.length == 0) {
		mastodon_log(ic, "Couldn't find a matching account.");
		goto finish;
	}

	// Just use the first one, let's hope these are sorted appropriately!
	struct mastodon_account *ma = mastodon_xt_get_user(parsed->u.array.values[0]);

	if (ma) {
		func(ic, ma->id);
	} else {
		mastodon_log(ic, "Couldn't find a matching account.");
	}

	ma_free(ma);
finish:
	json_value_free(parsed);
}

/**
 * Callback for a reponse containing one or more statuses which are to be shown, usually the result of looking at the
 * statuses of an account.
 */
void mastodon_http_statuses(struct http_request *req)
{
	mastodon_http_timeline(req, MT_HOME);
}

/**
 * Given a command that showed a bunch of statuses, which will have used mastodon_http_timeline as a callback, and which
 * will therefore have set md->next_url, we can use now use this URL to request more statuses.
 */
void mastodon_more(struct im_connection *ic)
{
	struct mastodon_data *md = ic->proto_data;

	if (!md->next_url) {
		mastodon_log(ic, "Next URL is not set. This shouldn't happen, as they say!?");
		return;
	}

	char *url = g_strdup(md->next_url);
	char *s = NULL;
	int len = 0;
	int i;

	for (i = 0; url[i]; i++) {
		if (url[i] == '?') {
			url[i] = 0;
			s = url + i + 1;
			len = 1;
		} else if (s && url[i] == '&') {
			url[i] = '='; // for later splitting
			len++;
		}
	}

	gchar **args = NULL;

	if (s) {
		args = g_strsplit (s, "=", -1);
	}

	switch(md->more_type) {
	case MASTODON_MORE_STATUSES:
		mastodon_http(ic, url, mastodon_http_statuses, ic, HTTP_GET, args, len);
		break;
	case MASTODON_MORE_NOTIFICATIONS:
		mastodon_http(ic, url, mastodon_http_notifications, ic, HTTP_GET, args, len);
		break;
	}

	g_strfreev(args);
	g_free(url);
}

/**
 * Show the timeline of a user.
 */
void mastodon_account_statuses(struct im_connection *ic, guint64 id)
{
	char *url = g_strdup_printf(MASTODON_ACCOUNT_STATUSES_URL, id);
	mastodon_http(ic, url, mastodon_http_statuses, ic, HTTP_GET, NULL, 0);
	g_free(url);
}

/**
 * Show the pinned statuses of a user.
 */
void mastodon_account_pinned_statuses(struct im_connection *ic, guint64 id)
{
	char *args[2] = {
		"pinned", "1",
	};

	char *url = g_strdup_printf(MASTODON_ACCOUNT_STATUSES_URL, id);
	mastodon_http(ic, url, mastodon_http_statuses, ic, HTTP_GET, args, 2);
	g_free(url);
}

/**
 * Callback to display the timeline for a unknown user. We got the account data back and now we just take the first user
 * and display their timeline.
 */
void mastodon_http_unknown_account_statuses(struct http_request *req)
{
	mastodon_chained_account(req, mastodon_account_statuses);
}

/**
 * Show the timeline of an unknown user. Thus, we first have to search for them.
 */
void mastodon_unknown_account_statuses(struct im_connection *ic, char *who)
{
	mastodon_with_search_account(ic, who, mastodon_http_unknown_account_statuses);
}

/**
 * Callback to display the timeline for a unknown user. We got the account data back and now we just take the first user
 * and display their timeline.
 */
void mastodon_http_unknown_account_pinned_statuses(struct http_request *req)
{
	mastodon_chained_account(req, mastodon_account_pinned_statuses);
}

/**
 * Show the timeline of an unknown user. Thus, we first have to search for them.
 */
void mastodon_unknown_account_pinned_statuses(struct im_connection *ic, char *who)
{
	mastodon_with_search_account(ic, who, mastodon_http_unknown_account_pinned_statuses);
}

/**
 * Callback for the user bio.
 */
void mastodon_http_account_bio(struct http_request *req)
{
	struct im_connection *ic = req->data;
	if (!g_slist_find(mastodon_connections, ic)) {
		return;
	}

	json_value *parsed;
	if (!(parsed = mastodon_parse_response(ic, req))) {
		/* ic would have been freed in imc_logout in this situation */
		ic = NULL;
		return;
	}

	const char *display_name = json_o_str(parsed, "display_name");
	char *note = g_strdup(json_o_str(parsed, "note"));
	mastodon_strip_html(note); // modified in place

	mastodon_log(ic, "Bio for %s: %s", display_name, note);

	g_free(note);
	json_value_free(parsed);
}

/**
 * Show a user bio.
 */
void mastodon_account_bio(struct im_connection *ic, guint64 id)
{
	char *url = g_strdup_printf(MASTODON_ACCOUNT_URL, id);
	mastodon_http(ic, url, mastodon_http_account_bio, ic, HTTP_GET, NULL, 0);
	g_free(url);
}
/**
 * Callback to display the timeline for a unknown user. We got the account data back and now we just take the first user
 * and show their bio.
 */
void mastodon_http_unknown_account_bio(struct http_request *req)
{
	mastodon_chained_account(req, mastodon_account_bio);
}

/**
 * Show the bio of an unknown user. Thus, we first have to search for them.
 */
void mastodon_unknown_account_bio(struct im_connection *ic, char *who)
{
	mastodon_with_search_account(ic, who, mastodon_http_unknown_account_bio);
}

/**
 * Call back for step 3 of mastodon_follow: adding the buddy.
 */
static void mastodon_http_follow3(struct http_request *req)
{
	struct im_connection *ic = req->data;
	if (!g_slist_find(mastodon_connections, ic)) {
		return;
	}

	json_value *parsed;
	if (!(parsed = mastodon_parse_response(ic, req))) {
		/* ic would have been freed in imc_logout in this situation */
		ic = NULL;
		return;
	}

	struct mastodon_account *ma = mastodon_xt_get_user(parsed);

	if (ma) {
		mastodon_add_buddy(ic, ma->id, ma->acct, ma->display_name);
		mastodon_log(ic, "You are now following %s.", ma->acct);
	} else {
		mastodon_log(ic, "Couldn't find a matching account.");
	}

	ma_free(ma);
	json_value_free(parsed);
}

/**
 * Call back for step 2 of mastodon_follow: actually following.
 */
static void mastodon_http_follow2(struct http_request *req)
{
	struct im_connection *ic = req->data;
	if (!g_slist_find(mastodon_connections, ic)) {
		return;
	}

	json_value *parsed;
	if (!(parsed = mastodon_parse_response(ic, req))) {
		/* ic would have been freed in imc_logout in this situation */
		ic = NULL;
		return;
	}

	json_value *it;
	if ((it = json_o_get(parsed, "domain_blocking")) && it->type == json_boolean && it->u.boolean) {
		mastodon_log(ic, "This user's domain is being blocked by your instance.");
	}

	if ((it = json_o_get(parsed, "blocking")) && it->type == json_boolean && it->u.boolean) {
		mastodon_log(ic, "You need to unblock this user.");
	}

	if ((it = json_o_get(parsed, "muting")) && it->type == json_boolean && it->u.boolean) {
		mastodon_log(ic, "You might want to unmute this user.");
	}

	if ((it = json_o_get(parsed, "muting")) && it->type == json_boolean && it->u.boolean) {
		mastodon_log(ic, "You might want to unmute this user.");
	}

	if ((it = json_o_get(parsed, "requested")) && it->type == json_boolean && it->u.boolean) {
		mastodon_log(ic, "You have requested to follow this user.");
	}

	if ((it = json_o_get(parsed, "followed_by")) && it->type == json_boolean && it->u.boolean) {
		mastodon_log(ic, "Nice, this user is already following you.");
	}

	if ((it = json_o_get(parsed, "following")) && it->type == json_boolean && it->u.boolean) {
		guint64 id;
		if ((it = json_o_get(parsed, "id")) &&
		    (id = mastodon_json_int64(it))) {
			char *url = g_strdup_printf(MASTODON_ACCOUNT_URL, id);
			mastodon_http(ic, url, mastodon_http_follow3, ic, HTTP_GET, NULL, 0);
			g_free(url);
		} else {
			mastodon_log(ic, "I can't believe it: this relation has no id. I can't add them!");
		}
	}

	json_value_free(parsed);
}

/**
 * Call back for step 1 of mastodon_follow: searching for the account to follow.
 */
static void mastodon_http_follow1(struct http_request *req)
{
	struct im_connection *ic = req->data;
	if (!g_slist_find(mastodon_connections, ic)) {
		return;
	}

	json_value *parsed;
	if (!(parsed = mastodon_parse_response(ic, req))) {
		/* ic would have been freed in imc_logout in this situation */
		ic = NULL;
		return;
	}

	if (parsed->type != json_array || parsed->u.array.length == 0) {
		mastodon_log(ic, "Couldn't find a matching account.");
		goto finish;
	}

	// Just use the first one, let's hope these are sorted appropriately!
	struct mastodon_account *ma = mastodon_xt_get_user(parsed->u.array.values[0]);

	if (ma) {
		char *url = g_strdup_printf(MASTODON_ACCOUNT_FOLLOW_URL, ma->id);
		mastodon_http(ic, url, mastodon_http_follow2, ic, HTTP_POST, NULL, 0);
		g_free(url);
		ma_free(ma);
	} else {
		mastodon_log(ic, "Couldn't find a matching account.");
	}
finish:
	json_value_free(parsed);
}

/**
 * Function to follow an unknown user. First we need to search for it,
 * though.
 */
void mastodon_follow(struct im_connection *ic, char *who)
{
	mastodon_with_search_account(ic, who, mastodon_http_follow1);
}

/**
 * Callback for adding the buddies you are following.
 */
static void mastodon_http_following(struct http_request *req)
{
	struct im_connection *ic = req->data;
	if (!g_slist_find(mastodon_connections, ic)) {
		return;
	}

	json_value *parsed;
	if (!(parsed = mastodon_parse_response(ic, req))) {
		/* ic would have been freed in imc_logout in this situation */
		ic = NULL;
		return;
	}

	if (parsed->type != json_array || parsed->u.array.length == 0) {
		goto finish;
	}

	int i;
	for (i = 0; i < parsed->u.array.length; i++) {

		struct mastodon_account *ma = mastodon_xt_get_user(parsed->u.array.values[i]);

		if (ma) {
			mastodon_add_buddy(ic, ma->id, ma->acct, ma->display_name);
		}

		ma_free(ma);
	}

finish:
	json_value_free(parsed);
	gboolean done = TRUE;

	// try to fetch more if there is a header saying that there is
	// more (URL in angled brackets)
	char *header = NULL;
	if ((header = get_rfc822_header(req->reply_headers, "Link", 0))) {

		char *url = NULL;
		char *s = NULL;
		int len = 0;
		int i;

		for (i = 0; header[i]; i++) {
			if (header[i] == '<') {
				url = header + i + 1;
			} else if (header[i] == '?') {
				header[i] = 0; // end url
				s = header + i + 1;
				len = 1;
			} else if (s && header[i] == '&') {
				header[i] = '='; // for later splitting
				len++;
			} else if (url && header[i] == '>') {
				header[i] = 0;
				if (strncmp(header + i + 1, "; rel=\"next\"", 12) == 0) {
					break;
				} else {
					url = NULL;
					s = NULL;
					len = 0;
				}
			}
		}

		if (url) {
			gchar **args = NULL;

			if (s) {
				args = g_strsplit (s, "=", -1);
			}

			mastodon_http(ic, url, mastodon_http_following, ic, HTTP_GET, args, len);
			done = FALSE;

			g_strfreev(args);
		}

		g_free(header);
	}

	if (done) {
		/* Now that we have reached the end of the list, everybody has mastodon_user_data set, at last: imcb_add_buddy →
		   bee_user_new → ic->acc->prpl->buddy_data_add → mastodon_buddy_data_add. Now we're ready to (re)load lists. */
		mastodon_list_reload(ic, TRUE);

		struct mastodon_data *md = ic->proto_data;
		md->flags |= MASTODON_HAVE_FRIENDS;
	}
}

/**
 * Add the buddies the current account is following.
 */
void mastodon_following(struct im_connection *ic)
{
	gint64 id = set_getint(&ic->acc->set, "account_id");

	if (!id) {
		return;
	}

	char *url = g_strdup_printf(MASTODON_ACCOUNT_FOLLOWING_URL, id);
	mastodon_http(ic, url, mastodon_http_following, ic, HTTP_GET, NULL, 0);
	g_free(url);
}

/**
 * Callback for the list of lists.
 */
void mastodon_http_lists(struct http_request *req)
{
	struct im_connection *ic = req->data;
	if (!g_slist_find(mastodon_connections, ic)) {
		return;
	}

	json_value *parsed;
	if (!(parsed = mastodon_parse_response(ic, req))) {
		/* ic would have been freed in imc_logout in this situation */
		ic = NULL;
		return;
	}

	if (parsed->type != json_array || parsed->u.array.length == 0) {
		mastodon_log(ic, "Use 'list create <name>' to create a list.");
		goto finish;
	}

	int i;
	GString *s = g_string_new(g_strdup_printf("Lists: "));
	gboolean first = TRUE;
	for (i = 0; i < parsed->u.array.length; i++) {
			json_value *a = parsed->u.array.values[i];
			if (a->type == json_object) {
				if (first) {
					first = FALSE;
				} else {
					g_string_append(s, "; ");
				}
				g_string_append(s, json_o_str(a, "title"));
			}
	}
	mastodon_log(ic, s->str);
	g_string_free(s, TRUE);
finish:
	json_value_free(parsed);
}

/**
 * Retrieving lists. Returns at most 50 Lists without pagination.
 */
void mastodon_lists(struct im_connection *ic) {
	mastodon_http(ic, MASTODON_LIST_URL, mastodon_http_lists, ic, HTTP_GET, NULL, 0);
}

/**
 * Create a list.
 */
void mastodon_list_create(struct im_connection *ic, char *title) {
	struct mastodon_data *md = ic->proto_data;

	struct mastodon_command *mc = g_new0(struct mastodon_command, 1);
	mc->ic = ic;

	if (md->undo_type == MASTODON_NEW) {
		mc->command = MC_LIST_CREATE;
		mc->redo = g_strdup_printf("list create %s", title);
		mc->undo = g_strdup_printf("list delete %s", title);
	}

	char *args[2] = {
		"title", title,
	};

	mastodon_http(ic, MASTODON_LIST_URL, mastodon_http_callback_and_ack, mc, HTTP_POST, args, 2);
}

/**
 * Rename a list.
 */
void mastodon_list_update(struct im_connection *ic, guint64 id, char *title);

/**
 * Second callback for the list of accounts.
 */
void mastodon_http_list_accounts2(struct http_request *req) {
	struct mastodon_command *mc = req->data;
	struct im_connection *ic = mc->ic;

	if (!g_slist_find(mastodon_connections, ic)) {
		goto finally;
	}

	json_value *parsed;
	if (!(parsed = mastodon_parse_response(ic, req))) {
		/* ic would have been freed in imc_logout in this situation */
		ic = NULL;
		goto finally;
	}

	if (parsed->type != json_array || parsed->u.array.length == 0) {
		mastodon_log(ic, "There are no members in this list. Your options:\n"
					 "Delete it using 'list delete %s'\n"
					 "Add members using 'list add <nick> to %s'",
					 mc->str, mc->str);
		goto finish;
	}

	int i;
	GString *m = g_string_new("Members:");

	for (i = 0; i < parsed->u.array.length; i++) {

		struct mastodon_account *ma = mastodon_xt_get_user(parsed->u.array.values[i]);

		if (ma) {
			g_string_append(m, " ");
			bee_user_t *bu = bee_user_by_handle(ic->bee, ic, ma->acct);
			if (bu) {
				irc_user_t *iu = bu->ui_data;
				g_string_append(m, iu->nick);
			} else {
				g_string_append(m, "@");
				g_string_append(m, ma->acct);
			}
			ma_free(ma);
		}
	}
	mastodon_log(ic, m->str);
	g_string_free(m, TRUE);

finish:
	/* We need to free the parsed data. */
	json_value_free(parsed);
finally:
	/* We've encountered a problem and we need to free the mastodon_command. */
	mc_free(mc);
}


/**
 * Part two of the first callback: now we have mc->id. Call the URL which will give us the accounts themselves. The API
 * documentation says: If you specify limit=0 in the query, all accounts will be returned without pagination.
 */
void mastodon_list_accounts(struct im_connection *ic, struct mastodon_command *mc) {
	char *args[2] = { "limit", "0",	};
	char *url = g_strdup_printf(MASTODON_LIST_ACCOUNTS_URL, mc->id);
	mastodon_http(ic, url, mastodon_http_list_accounts2, mc, HTTP_GET, args, 2);
	g_free(url);
}

/**
 * First callback for listing the accounts in a list. First, get the list id from the data we received, then call the
 * next function.
 */
void mastodon_http_list_accounts(struct http_request *req) {
	mastodon_chained_list(req, mastodon_list_accounts);
}

/**
 * Show accounts in a list.
 */
void mastodon_unknown_list_accounts(struct im_connection *ic, char *title) {
	struct mastodon_command *mc = g_new0(struct mastodon_command, 1);
	mc->ic = ic;
	mc->str = g_strdup(title);
	mastodon_with_named_list(ic, mc, mastodon_http_list_accounts);
}

/**
 * Second callback for list delete. We get back the accounts in the list we are about to delete. We need these to
 * prepare the undo command. Undo is serious business. Then we delete the list.
 */
void mastodon_http_list_delete2(struct http_request *req) {
	struct mastodon_command *mc = req->data;
	struct im_connection *ic = mc->ic;
	struct mastodon_data *md = ic->proto_data;

	if (!g_slist_find(mastodon_connections, ic)) {
		goto finish;
	}

	json_value *parsed;
	if (!(parsed = mastodon_parse_response(ic, req))) {
		/* ic would have been freed in imc_logout in this situation */
		ic = NULL;
		goto finish;
	}

	if (parsed->type != json_array || parsed->u.array.length == 0) {
		mastodon_log(ic, "There are no members in this list. Cool!");
	} else if (md->undo_type == MASTODON_NEW) {
		int i;
		char *title = mc->str;
		GString *undo = g_string_new(mc->undo);

		for (i = 0; i < parsed->u.array.length; i++) {

			struct mastodon_account *ma = mastodon_xt_get_user(parsed->u.array.values[i]);

			if (ma) {
				g_string_append(undo, FS);
				g_string_append_printf(undo, "list add %" G_GINT64_FORMAT " to %s", ma->id, title);
			}
			ma_free(ma);
		}

		g_free(mc->undo); mc->undo = undo->str; // adopt
		g_string_free(undo, FALSE);
	}

	char *url = g_strdup_printf(MASTODON_LIST_DATA_URL, mc->id);
	mastodon_http(ic, url, mastodon_http_callback_and_ack, mc, HTTP_DELETE, NULL, 0);
	g_free(url);
	goto success;
finish:
	mc_free(mc);
success:
	json_value_free(parsed);
}

/**
 * This is part of the first callback. We have the list id in mc->id! If this is a new command, we want to find all the
 * accounts in the list in order to prepare the undo command. Undo is serious business. If this command is not new, in
 * other words, this is a redo command, then we can skip right ahead and go for the list delete.
 */
void mastodon_list_delete(struct im_connection *ic, struct mastodon_command *mc) {
	struct mastodon_data *md = ic->proto_data;

	if (md->undo_type == MASTODON_NEW) {
		/* Make sure we get all the accounts for undo. The API documentation says: If you specify limit=0 in the query,
		 * all accounts will be returned without pagination. */
		char *args[2] = { "limit", "0",	};
		char *url = g_strdup_printf(MASTODON_LIST_ACCOUNTS_URL, mc->id);
		mastodon_http(ic, url, mastodon_http_list_delete2, mc, HTTP_GET, args, 2);
		g_free(url);
	} else {
		/* This is a short cut! */
		char *url = g_strdup_printf(MASTODON_LIST_DATA_URL, mc->id);
		mastodon_http(ic, url, mastodon_http_callback_and_ack, mc, HTTP_DELETE, NULL, 0);
		g_free(url);
	}
}

/**
 * Callback for list delete. We get back the lists we know about and need to find the list to delete. Once we have it,
 * we use another callback to get the list of all its members in order to prepare the undo command. Undo is serious
 * business.
 */
void mastodon_http_list_delete(struct http_request *req) {
	mastodon_chained_list(req, mastodon_list_delete);
}

/**
 * Delete a list by title.
 */
void mastodon_unknown_list_delete(struct im_connection *ic, char *title) {
	struct mastodon_command *mc = g_new0(struct mastodon_command, 1);
	mc->ic = ic;
	mc->str = g_strdup(title);
	struct mastodon_data *md = ic->proto_data;
	if (md->undo_type == MASTODON_NEW) {
		mc->command = MC_LIST_DELETE;
		mc->redo = g_strdup_printf("list delete %s", title);
		mc->undo = g_strdup_printf("list create %s", title);
	}
	mastodon_with_named_list(ic, mc, mastodon_http_list_delete);
};

/**
 * Part two of the first callback: now we have mc->id. Call the URL which will give us the accounts themselves. The API
 * documentation says: If you specify limit=0 in the query, all accounts will be returned without pagination.
 */
void mastodon_list_add_account(struct im_connection *ic, struct mastodon_command *mc) {
	char *args[2] = {
		"account_ids[]", g_strdup_printf("%" G_GUINT64_FORMAT, mc->id2),
	};
	char *url = g_strdup_printf(MASTODON_LIST_ACCOUNTS_URL, mc->id);
	mastodon_http(ic, url, mastodon_http_callback_and_ack, mc, HTTP_POST, args, 2);
	g_free(args[1]);
	g_free(url);
}

/**
 * First callback for adding an account to a list. First, get the list id from the data we received, then call the next
 * function.
 */
void mastodon_http_list_add_account(struct http_request *req) {
	mastodon_chained_list(req, mastodon_list_add_account);
}

/**
 * Add one or more accounts to a list.
 */
void mastodon_unknown_list_add_account(struct im_connection *ic, guint64 id, char *title) {
	struct mastodon_command *mc = g_new0(struct mastodon_command, 1);
	mc->ic = ic;
	mc->id2 = id;
	mc->str = g_strdup(title);
	struct mastodon_data *md = ic->proto_data;
	if (md->undo_type == MASTODON_NEW) {
		mc->command = MC_LIST_ADD_ACCOUNT;
		mc->redo = g_strdup_printf("list add %" G_GINT64_FORMAT " to %s", id, title);
		mc->undo = g_strdup_printf("list remove %" G_GINT64_FORMAT " from %s", id, title);
	}
	mastodon_with_named_list(ic, mc, mastodon_http_list_add_account);
};

/**
 * Part two of the first callback: now we have mc->id. Call the URL which will give us the accounts themselves. The API
 * documentation says: If you specify limit=0 in the query, all accounts will be returned without pagination.
 */
void mastodon_list_remove_account(struct im_connection *ic, struct mastodon_command *mc) {
	char *args[2] = {
		"account_ids[]", g_strdup_printf("%" G_GUINT64_FORMAT, mc->id2),
	};
	char *url = g_strdup_printf(MASTODON_LIST_ACCOUNTS_URL, mc->id);
	mastodon_http(ic, url, mastodon_http_callback_and_ack, mc, HTTP_DELETE, args, 2);
	g_free(args[1]);
	g_free(url);
}

/**
 * First callback for removing an accounts from a list. First, get the list id from the data we received, then call the
 * next function.
 */
void mastodon_http_list_remove_account(struct http_request *req) {
	mastodon_chained_list(req, mastodon_list_remove_account);
}

/**
 * Remove one or more accounts from a list.
 */
void mastodon_unknown_list_remove_account(struct im_connection *ic, guint64 id, char *title) {
	struct mastodon_command *mc = g_new0(struct mastodon_command, 1);
	mc->ic = ic;
	mc->id2 = id;
	mc->str = g_strdup(title);
	struct mastodon_data *md = ic->proto_data;
	if (md->undo_type == MASTODON_NEW) {
		mc->command = MC_LIST_REMOVE_ACCOUNT;
		mc->redo = g_strdup_printf("list remove %" G_GINT64_FORMAT " from %s", id, title);
		mc->undo = g_strdup_printf("list add %" G_GINT64_FORMAT " to %s", id, title);
	}
	mastodon_with_named_list(ic, mc, mastodon_http_list_remove_account);
}

/**
 * Second callback to reload all the lists. We are getting all the accounts for one of the lists, here.
 * The mastodon_command (mc) has id (id of the list), str (title of the list), and optionally extra.
 */
static void mastodon_http_list_reload2(struct http_request *req) {
	struct mastodon_command *mc = req->data;
	struct im_connection *ic = mc->ic;

	if (!g_slist_find(mastodon_connections, ic)) {
		goto finally;
	}

	json_value *parsed;
	if (!(parsed = mastodon_parse_response(ic, req))) {
		/* ic would have been freed in imc_logout in this situation */
		ic = NULL;
		goto finally;
	}

	if (parsed->type != json_array || parsed->u.array.length == 0) {
		goto finish;
	}

	int i;
	for (i = 0; i < parsed->u.array.length; i++) {

		struct mastodon_account *ma;
		bee_user_t *bu;
		struct mastodon_user_data *mud;
		if ((ma = mastodon_xt_get_user(parsed->u.array.values[i])) &&
			(bu = bee_user_by_handle(ic->bee, ic, ma->acct)) &&
			(mud = (struct mastodon_user_data*) bu->data)) {
			mud->lists = g_slist_prepend(mud->lists, g_strdup(mc->str));
			ma_free(ma);
		}
	}

	mastodon_log(ic, "Membership of %s list reloaded", mc->str);

finish:
	json_value_free(parsed);

	if (mc->extra) {
		/* Keep using the mc and don't free it! */
		mastodon_list_timeline(ic, mc);
		return;
	}

finally:
	/* We've encountered a problem and we need to free the mastodon_command. */
	mc_free(mc);
}

/**
 * First callback to reload all the lists. We are getting all the lists, here. For each one, get the members in the
 * list. Our mastodon_command (mc) might have the extra attribute set.
 */
static void mastodon_http_list_reload(struct http_request *req) {
	struct mastodon_command *mc = req->data;
	struct im_connection *ic = mc->ic;

	if (!g_slist_find(mastodon_connections, ic)) {
		goto finally;
	}

	json_value *parsed;
	if (!(parsed = mastodon_parse_response(ic, req))) {
		/* ic would have been freed in imc_logout in this situation */
		ic = NULL;
		goto finally;
	}

	if (parsed->type != json_array || parsed->u.array.length == 0) {
		goto finish;
	}

	/* Clear existing list membership. */
	GSList *l;
	for (l = ic->bee->users; l; l = l->next) {
		bee_user_t *bu = l->data;
		struct mastodon_user_data *mud = (struct mastodon_user_data*) bu->data;
		if (mud) {
			g_slist_free_full(mud->lists, g_free); mud->lists = NULL;
		}
	}

	int i;
	guint64 id = 0;

	/* Get members for every list defined. */
	for (i = 0; i < parsed->u.array.length; i++) {
			json_value *a = parsed->u.array.values[i];
			json_value *it;
			const char *title;
			if (a->type == json_object &&
				(it = json_o_get(a, "id")) &&
				(id = mastodon_json_int64(it)) &&
				(title = json_o_str(a, "title"))) {

				struct mastodon_command *mc2 = g_new0(struct mastodon_command, 1);
				mc2->ic = ic;
				mc2->id = id;
				mc2->str = g_strdup(title);
				mc2->extra = mc->extra;

				char *url = g_strdup_printf(MASTODON_LIST_ACCOUNTS_URL, id);
				mastodon_http(ic, url, mastodon_http_list_reload2, mc2, HTTP_GET, NULL, 0);
				g_free(url);
			}
	}

finish:
	json_value_free(parsed);
finally:
	/* We've encountered a problem and we need to free the mastodon_command. */
	mc_free(mc);
}

/**
 * Reload the memberships of all the lists. We need this for mastodon_status_show_chat(). The populate parameter says
 * whether we should issue a timeline request for every list we have a group chat for, at the end.
 */
void mastodon_list_reload(struct im_connection *ic, gboolean populate) {
	struct mastodon_command *mc = g_new0(struct mastodon_command, 1);
	mc->ic = ic;
	mc->extra = populate;
	mastodon_http(ic, MASTODON_LIST_URL, mastodon_http_list_reload, mc, HTTP_GET, NULL, 0);
}

/**
 * Callback for getting your own account. This saves the account_id.
 * Once we have that, we are ready to figure out who our followers are.
 */
static void mastodon_http_verify_credentials(struct http_request *req)
{
	struct im_connection *ic = req->data;
	if (!g_slist_find(mastodon_connections, ic)) {
		return;
	}

	json_value *parsed;
	if ((parsed = mastodon_parse_response(ic, req))) {
		json_value *it;
		guint64 id;
		if ((it = json_o_get(parsed, "id")) &&
		    (id = mastodon_json_int64(it))) {
			set_setint(&ic->acc->set, "account_id", id);
		}
		json_value_free(parsed);

		mastodon_following(ic);
	}
}

/**
 * Get the account of the current user.
 */
void mastodon_verify_credentials(struct im_connection *ic)
{
	imcb_log(ic, "Verifying credentials");
	mastodon_http(ic, MASTODON_VERIFY_CREDENTIALS_URL, mastodon_http_verify_credentials, ic, HTTP_GET, NULL, 0);
}

/**
 * Callback for registering a new application.
 */
static void mastodon_http_register_app(struct http_request *req)
{
	struct im_connection *ic = req->data;
	if (!g_slist_find(mastodon_connections, ic)) {
		return;
	}

	mastodon_log(ic, "Parsing application registration response");

	json_value *parsed;
	if ((parsed = mastodon_parse_response(ic, req))) {

		set_setint(&ic->acc->set, "app_id", json_o_get(parsed, "id")->u.integer);

		char *key = json_o_strdup(parsed, "client_id");
		char *secret = json_o_strdup(parsed, "client_secret");

		json_value_free(parsed);

		// save for future sessions
		set_setstr(&ic->acc->set, "consumer_key", key);
		set_setstr(&ic->acc->set, "consumer_secret", secret);

		// and set for the current session, and connect
		struct mastodon_data *md = ic->proto_data;
		struct oauth2_service *os = md->oauth2_service;
		os->consumer_key = key;
		os->consumer_secret = secret;

		oauth2_init(ic);
	}
}

/**
 * Function to register a new application (Bitlbee) for the server.
 */
void mastodon_register_app(struct im_connection *ic)
{
	char *args[8] = {
		"client_name", "bitlbee",
		"redirect_uris", "urn:ietf:wg:oauth:2.0:oob",
		"scopes", "read write follow",
		"website", "https://www.bitlbee.org/"
	};

	mastodon_http(ic, MASTODON_REGISTER_APP_URL, mastodon_http_register_app, ic, HTTP_POST, args, 8);
}
