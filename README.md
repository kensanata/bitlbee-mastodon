Mastodon plugin for Bitlbee
---------------------------

This plugin allows [Bitlbee](https://www.bitlbee.org/) to communicate
with [Mastodon](https://joinmastodon.org/) instances. Mastodon is a
free, open-source, decentralized microblogging network. Bitlbee is an
IRC server connecting to various other text messaging services. You
run Bitlbee and connect to it using an IRC client, then configure
Bitlbee to connect to other services, such as a Mastodon instance
where you already have an account. The benefit is that you can now use
[any IRC client](https://en.wikipedia.org/wiki/Comparison_of_Internet_Relay_Chat_clients)
you want to connect to Mastodon.

Most of the source code is distributed under the
[GNU Lesser Public License 2.1](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html#SEC1).

The build system is distributed under the 
[GNU Public License 2.0 or any later version](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html#SEC1).

Anything without an obvious license in the file header also uses the
GPL 2.0 or any later version.

Usage
-----

Please refer to the Bitlbee help system:

```
> help mastodon
```

Alternatively, a snapshot of the entries added to the help system by
this plugin are available on the
[help page](https://alexschroeder.ch/cgit/bitlbee-mastodon/tree/doc/HELP.md#top).

Build dependencies
------------------

- `bitlbee` and headers >= 3.5

  If you haven't built Bitlbee yourself you will need to install the
  dev package, usually `bitlbee-dev` or `bitlbee-devel`. If Bitlbee
  was built from source don't forget to do `make install-dev`.

- `glib2` and headers => 2.32

  The library itself is usually installed as a dependency of Bitlbee
  but headers need to be installed separately. In Debian, the package
  containing them is `libglib2.0-dev`.

- `autotools` (if building from git)

  A bit of an overkill, but it works. If you don't have this package,
  try looking for `autoconf` and `libtool`.


Building and Installing
-----------------------

If building from git you will first need to generate the autotools
configuration script and related files by executing the following
command:

```
./autogen.sh
```

After that (or when building from a tarball) you can build as usual:

```
./configure
make
sudo make install
```

🔥 If your Bitlbee's plugindir is in a non-standard location you need to
specify it: `./configure with --with-plugindir=/path/to/plugindir`

🔥 If you're installing this plugin in a system where you didn't build
your own Bitlbee but installed revision 3.5.1 (e.g. on a Debian system
around the end of 2017), you will run into a problem: the plugin will
get installed into `/usr/lib/bitlbee` (`plugindir`) but the
documentation wants to install into `/usr/local/share/bitlbee` instead
of `/usr/share/bitlbee` (`datadir`). As you can tell from
`/usr/lib/pkgconfig/bitlbee.pc`, there is no `datadir` for you. In
this situation, try `./configure --prefix=/usr` and build and install
again.

Debugging
---------

You can enable extra debug output for `bitlbee-mastodon` by setting
the `BITLBEE_DEBUG` environment variable. This will print all traffic
it exchanges with Mastodon servers to STDOUT and there is a lot of it.
To get it on your screen run `bitlbee` in foreground mode:

```
BITLBEE_DEBUG=1 bitlbee -nvD
```

If you need to read your config file from the standard location:

```
BITLBEE_DEBUG=1 sudo -u bitlbee bitlbee -nvD
```

If you need to use a debugger, make a copy of
`/etc/bitlbee/bitlbee.conf` (or simply create an empty file) and
`/var/lib/bitlbee/`. Then run `gdb`, set the breakpoints you want
(answer yes to "Make breakpoint pending on future shared library
load?"), and run it using the options shown:

```
touch bitlbee.conf
sudo cp /var/lib/bitlbee/*.xml .
gdb bitlbee
b mastodon_string_join
y
run -nvD -c bitlbee.conf -d .
```

Note that perhaps you must remove the `-O2` from `CFLAGS` in the
`Makefile` and run `make clean && make && sudo make install` again in
order to build and install the module without any compiler
optimisation.

Then connect with an IRC client as you usually do.

WARNING: there *is* sensitive information in this debug output, such
as auth tokens, your plaintext password and, obviously, your incoming
and outgoing messages. Be sure to remove any information you are not
willing to share before posting it anywhere.

If you are experiencing crashes please refer to
[debugging crashes](https://wiki.bitlbee.org/DebuggingCrashes)
for information on how to get a meaningful backtrace.

Bugs
----

Please report issues using the
[Software Wiki](https://alexschroeder.ch/software/Bitlbee_Mastodon).
For questions, ping **kensanata** on `irc.oftc.net/#bitlbee`.
