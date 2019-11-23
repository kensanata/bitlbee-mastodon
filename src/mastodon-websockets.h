/*
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

#pragma once

#include "nogaim.h"
#include "mastodon.h"

void mw_free(struct mastodon_websocket *mw);
void mastodon_ws_connect(struct im_connection *ic, char *url, mastodon_timeline_type_t subscription);
void mastodon_open_user_websocket(struct im_connection *ic);
