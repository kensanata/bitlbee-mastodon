# Copyright 2016 Artem Savkov <artem.savkov@gmail.com>
# Copyright 2017 Alex Schroeder <alex@gnu.org>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

libdir = $(plugindir)
lib_LTLIBRARIES = mastodon.la

mastodon_la_CFLAGS  = \
	$(BITLBEE_CFLAGS) \
	$(GLIB_CFLAGS) \
	-Wall

mastodon_la_LDFLAGS = \
	-module \
	-avoid-version \
	$(BITLBEE_LIBS) \
	$(GLIB_LIBS)

mastodon_la_SOURCES = \
	mastodon.c \
	mastodon.h \
	mastodon-http.c \
	mastodon-http.h \
	mastodon-lib.c \
	mastodon-lib.h \
	mastodon-websockets.c \
	mastodon-websockets.h \
	rot13.c \
	rot13.h
