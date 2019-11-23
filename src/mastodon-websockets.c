/*
 * Copyright 2015-2016 Artem Savkov <artem.savkov@gmail.com>
 * Copyright 2019 Alex Schroeder <alex@gnu.org>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include <config.h>
#include <ssl_client.h>
#include <events.h>

#include "mastodon-websockets.h"
#include "mastodon-lib.h"
#include "mastodon.h"
#include "json.h"
#include "json_util.h"

// https://tools.ietf.org/html/rfc6455#section-5.2
typedef enum {
  OPCODE_CONTINUATION = 0x0,
  OPCODE_TEXT         = 0x1,
  OPCODE_CLOSE        = 0x8,
  OPCODE_PING         = 0x9,
  OPCODE_PONG         = 0xA,
} mastodon_opcode;

static void mastodon_ws_remove_event(gint *event)
{
  if (*event > 0) {
    b_event_remove(*event);
    *event = 0;
  }
}

void mastodon_ws_cleanup(struct mastodon_websocket *mw)
{
  mastodon_ws_remove_event(&mw->inpa);

  if (mw->ssl != NULL) {
    ssl_disconnect(mw->ssl);
    mw->ssl = NULL;
  }
}

/**
 * Frees a mastodon_websocket struct and disconnects the websocket.
 */
void mw_free(struct mastodon_websocket *mw)
{
	if (mw == NULL) {
		return;
	}
	mastodon_ws_cleanup(mw);
	g_free(mw);
}

/**
 * Handle websocket unmasking
 */
static gchar *mastodon_ws_mask(guchar key[4], const char *pload,
                              guint64 psize)
{
  gchar *ret = g_malloc0(psize + 1); /* null terminated string */

  for (guint64 i = 0; i < psize; i++) {
    ret[i] = pload[i] ^ key[i % 4];
  }

  return ret;
}

static int mastodon_ws_send_payload(struct mastodon_websocket *mw, int opcode, const char *pload, guint64 psize)
{
  gchar *buf;
  guint64 hlen = 6;
  size_t ret = 0;
  guchar mkey[4];
  gchar *mpload;

  random_bytes(mkey, sizeof(mkey));
  mpload = mastodon_ws_mask(mkey, pload, psize);

  if (psize > 125) {
    if (psize > G_MAXUINT16) {
      hlen += 8;
    } else {
      hlen += 2;
    }
  }

  buf = g_malloc0(hlen + psize);

  buf[0] = 0x80 | opcode; // always sending FIN (just one frame)
  if (psize < 126) {
	  buf[1] = (gchar)(psize | 0x80); // always sending MASK
  } else if (psize > G_MAXUINT16) {
	  guint64 esize = GUINT64_TO_BE(psize);
	  buf[1] = (gchar)(127 | 0x80); // always sending MASK
	  memcpy(buf + 2, &esize, sizeof(esize));
  } else {
	  guint16 esize = GUINT16_TO_BE(psize);
	  buf[1] = (gchar)(126 | 0x80); // always sending MASK
	  memcpy(buf + 2, &esize, sizeof(esize));
  }

  memcpy(buf + hlen - sizeof(mkey), &mkey, sizeof(mkey));
  memcpy(buf + hlen, mpload, psize);
  g_free(mpload);

  ret = ssl_write(mw->ssl, buf, hlen + psize);

  g_free(buf);
  return ret;
}

static gboolean mastodon_ws_send_pong(struct mastodon_websocket *mw, gchar *buf, guint64 len)
{
	struct im_connection *ic = mw->ic;

	if (mw->state == WS_CONNECTED) {
		mastodon_ws_send_payload(mw, OPCODE_PONG, buf, len); // if we managed zero bytes, we're disconnected
		// imcb_log(ic, "sent back PONG for %s", mw->url);
	} else {
		imcb_log(ic, "attempt to send PONG in the wrong state: %d", mw->state);
	}
	return FALSE; /* FALSE means we haven't been disconnected */
}

static void mastodon_handle_event(struct mastodon_websocket *mw, json_value *parsed)
{
	struct im_connection *ic = mw->ic;
	mastodon_evt_flags_t evt_type = MASTODON_EVT_UNKNOWN;

	const char *s;
	if (parsed->type == json_object &&
		(s = json_o_str(parsed, "event"))) {
		if (g_strcmp0(s, "update") == 0) {
			evt_type = MASTODON_EVT_UPDATE;
		} else if (g_strcmp0(s, "notification") == 0) {
			evt_type = MASTODON_EVT_NOTIFICATION;
		} else if (g_strcmp0(s, "delete") == 0) {
			evt_type = MASTODON_EVT_DELETE;
		}
	} else {
		imcb_log(ic, "unknown JSON type %d", parsed->type);
		return;
	}

	json_value *event;
	const char *payload = json_o_str(parsed, "payload");
	// imcb_log(ic, "handling event: %s", payload);
	if (payload &&
		(event = json_parse(payload, strlen(payload)))) {
		mastodon_stream_handle_event(ic, evt_type, event, mw->subscription);
	} else {
		imcb_log(ic, "unable to parse payload: %s", payload);
	}
}

static gboolean mastodon_handle(struct mastodon_websocket *mw, int opcode, gchar *data, guint64 len)
{
	struct im_connection *ic = mw->ic;

	if (opcode == OPCODE_PING) {
		return mastodon_ws_send_pong(mw, data, len);
	} else if (opcode != OPCODE_TEXT) {
		imcb_log(ic, "unhandled opcode %d on %s: %s", opcode, mw->url, (char*)data);
		return FALSE; /* FALSE means we haven't been disconnected */
	}

	json_value *parsed;
	if ((parsed = json_parse(data, len))) {
		// imcb_log(ic, "parsed data on %s: %s", mw->url, (char*)data);
		mastodon_handle_event(mw, parsed);
		json_value_free(parsed);
	} else {
		imcb_log(ic, "unparsed data on %s: %s", mw->url, (char*)data);
	}
	return FALSE; /* FALSE means we haven't been disconnected */
}

static void mastodon_ws_reconnect(struct mastodon_websocket *mw);

/**
 * Callback that handles websocket upgrades and reads websocket payloads.
 */
static gboolean mastodon_ws_in_callback(gpointer data, int source, b_input_condition cond)
{
	struct mastodon_websocket *mw = data;
	struct im_connection *ic = mw->ic;

	if (g_slist_find(mastodon_connections, ic) == NULL) {
		return FALSE;
	}

	if (mw->state == WS_CONNECTING) {
		gchar buf[4096] = "";
		if (ssl_read(mw->ssl, buf, sizeof(buf)) < 1) {
			if (ssl_errno == SSL_AGAIN)
				return TRUE;
			imcb_error(ic, "failed to read from %s while switching to websocket mode: %d", mw->url, ssl_errno);
			imc_logout(ic, TRUE);
			return FALSE;
		}
		if (g_strrstr_len(buf, 25, "101 Switching") != NULL
			&& g_str_has_suffix(buf, "\r\n\r\n")) {
			mw->state = WS_CONNECTED;
			imcb_log(ic, "websocket connected: %s", mw->url);
		} else {
			imcb_error(ic, "failed to switch to websocket mode for %s", mw->url);
			imc_logout(ic, TRUE);
			return FALSE;
		}
	} else {
		gchar buf = 0 ;
		guint64 len = 0;
		gboolean mask = FALSE;
		guchar mkey[4] = {0};
		gpointer rdata = NULL;
		guint64 read = 0;
		gboolean disconnected;
		gboolean fin = FALSE;

		if (ssl_read(mw->ssl, &buf, 1) < 1) {
			if (ssl_errno == SSL_AGAIN)
				return TRUE;
			imcb_error(ic, "failed to read ws header from %s: %d", mw->url, ssl_errno);
			mastodon_ws_reconnect(mw);
			return FALSE;
		}

		// imcb_log(ic, "read websocket header byte on %s: 0x%hhx", mw->url, buf);

		fin = (buf & 0x80); /* first bit */
		if (!fin) imcb_log(ic, "unsupported continuation seen on websocket %s", mw->url);

		int opcode = (buf & 0x0f); /* bits five to eight */
		switch (opcode) {
		case OPCODE_CONTINUATION:
			imcb_log(ic, "websocket %s wants to continue", mw->url);
			break; /* I hope we can handle this! */
		case OPCODE_TEXT:
			// imcb_log(ic, "websocket %s is sending text", mw->url);
			break; /* we can handle this */
		case OPCODE_CLOSE:
			imcb_log(ic, "websocket %s is closing", mw->url);
			if (mw->state == WS_CONNECTED) {
				imcb_log(ic, "token expired, cleaning up");
				set_setstr(&ic->acc->set, "token_cache", NULL);
			}
			imc_logout(ic, TRUE);
			return FALSE;
		case OPCODE_PING:
			/* It doesn't matter which stream sent us something. */
			ic->flags |= OPT_PONGED;
			break; /* we can handle this */
		case OPCODE_PONG:
			// imcb_log(ic, "websocket %s sent PONG", mw->url);
			return TRUE; /* ignoring this */
		}

		if (ssl_read(mw->ssl, &buf, 1) < 1) {
			imcb_error(ic, "disconnected from %s", mw->url);
			imc_logout(ic, TRUE);
			return FALSE;
		}
		len = buf & 0x7f;
		mask = (buf & 0x80) != 0;

		if (len == 126) {
			guint16 lbuf;
			if (ssl_read(mw->ssl, (gchar*)&lbuf, 2) < 2) {
				imcb_error(ic, "failed to read extended payload length on websocket %s", mw->url);
				imc_logout(ic, TRUE);
				return FALSE;
			}
			len = GUINT16_FROM_BE(lbuf);
		} else if (len == 127) {
			guint64 lbuf;
			if (ssl_read(mw->ssl, (gchar*)&lbuf, 8) < 8) {
				imcb_error(ic, "failed to read extended payload length continued on websocket %s", mw->url);
				imc_logout(ic, TRUE);
				return FALSE;
			}
			len = GUINT64_FROM_BE(lbuf);
		}

		if (mask) {
			if (ssl_read(mw->ssl, (gchar*)mkey, 4) < 4) {
				imcb_error(ic, "failed to read masking key on websocket %s", mw->url);
				imc_logout(ic, TRUE);
				return FALSE;
			}
		}

		rdata = g_malloc0(len + 1); /* null terminated string */
		while (read < len) {
			int ret = ssl_read(mw->ssl, rdata + read, len - read);
			read += ret;
			if (ret == 0) {
				break;
			}
		}

		if (read != len) {
			imcb_error(ic, "failed to read enough payload data on websocket %s", mw->url);
			imc_logout(ic, TRUE);
			g_free(rdata);
			return FALSE;
		}

		if (mask) {
			gchar *mdata = mastodon_ws_mask(mkey, rdata, len);
			disconnected = mastodon_handle(mw, opcode, mdata, len);
			g_free(mdata);
		} else {
			disconnected = mastodon_handle(mw, opcode, rdata, len);
		}
		g_free(rdata);
		if (disconnected)
			return FALSE;
	}
	if (ssl_pending(mw->ssl)) {
		/* The SSL library empties the TCP buffers completely but may keep some
		   data in its internal buffers. select() won't see that, but
		   ssl_pending() does. */
		return mastodon_ws_in_callback(mw, source, cond);
	} else {
		return TRUE;
	}
}

/**
 * Callback that requests an upgrade to a websocket connection.
 */
static gboolean mastodon_ws_connected_callback(gpointer data, int retcode, void *source, b_input_condition cond)
{
  struct mastodon_websocket *mw = data;
  struct im_connection *ic = mw->ic;

  if (g_slist_find(mastodon_connections, ic) == NULL) {
	  return FALSE;
  }

  struct mastodon_data *md = ic->proto_data;
  gchar *bkey;
  GString *req;
  guchar key[16];

  if (source == NULL) {
    mw->ssl = NULL;
    imcb_error(ic, "Failed to establish connection.");
    imc_logout(ic, TRUE);
    return FALSE;
  }

  random_bytes(key, sizeof(key));

  bkey = g_base64_encode(key, 16);

  req = g_string_new("");
  g_string_printf(req, "GET %s HTTP/1.1\r\n"
				  "Host: %s\r\n"
				  "Connection: keep-alive, Upgrade\r\n"
				  "Upgrade: websocket\r\n"
				  "Origin: %s\r\n"
				  "Pragma: no-cache\r\n"
				  "Cache-Control: no-cache\r\n"
				  "Authorization: Bearer %s\r\n"
				  "Sec-WebSocket-Version: 13\r\n"
				  "Sec-WebSocket-Key: %s\r\n"
				  "User-Agent: BitlBee " BITLBEE_VERSION "\r\n"
				  "\r\n",
				  mw->url,
				  md->url_host, /* Host */
				  set_getstr(&ic->acc->set, "base_url"), /* Origin */
				  md->oauth2_access_token, /* Authorization */
				  bkey); /* Sec-WebSocket-Key */

  g_free(bkey);

  mw->sslfd = ssl_getfd(source);
  mw->inpa = b_input_add(mw->sslfd, B_EV_IO_READ, mastodon_ws_in_callback, mw);
  ssl_write(mw->ssl, req->str, req->len);
  g_string_free(req, TRUE);
  return FALSE;
}

/**
 * Generic websocket connection for any URL.
 */
void mastodon_ws_connect(struct im_connection *ic, char *url, mastodon_timeline_type_t subscription) {
	struct mastodon_data *md = ic->proto_data;
	struct mastodon_websocket *mw = g_new0(struct mastodon_websocket, 1);
	mw->subscription = subscription;
	mw->ic = ic;
	mw->url = url;
	mw->state = WS_CONNECTING;
	mw->ssl = ssl_connect(md->url_host, md->url_port, TRUE, mastodon_ws_connected_callback, mw);
	if (mw->ssl == NULL) {
		mw_free(mw);
	} else {
		md->websockets = g_slist_prepend(md->websockets, mw);
	}
}

void mastodon_soft_reconnect(struct mastodon_websocket *mw)
{
  struct im_connection *ic = mw->ic;
  struct mastodon_data *md = ic->proto_data;
  imcb_log(ic, "Performing soft-reconnect");
  mastodon_ws_cleanup(mw);
  mw->state = WS_CONNECTING;
  mw->ssl = ssl_connect(md->url_host, md->url_port, TRUE, mastodon_ws_connected_callback, mw);
  if (mw->ssl == NULL) {
	  md->websockets = g_slist_remove(md->websockets, mw);
	  mw_free(mw);
  }
}

static void mastodon_ws_reconnect(struct mastodon_websocket *mw)
{
  struct im_connection *ic = mw->ic;

  if (mw->state == WS_READY) {
    mastodon_soft_reconnect(mw);
  } else {
    imc_logout(ic, TRUE);
  }
}

/**
 * Open the user (home) timeline via a websocket
 */
void mastodon_open_user_websocket(struct im_connection *ic)
{
	mastodon_ws_connect(ic, MASTODON_WEBSOCKET_USER_URL, MT_HOME);
}
