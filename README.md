Mastodon plugin for Bitlbee
---------------------------

This plugin is distributed under the [GPLv2 license](LICENSE).

Usage
-----

Please refer to the Bitlbee help system:

```
> help mastodon
```

Build dependencies
------------------

- `bitlbee` and headers >= 3.5

  If using distribution version of Bitlbee you will need to install the dev
  package, usually `bitlbee-dev` or `bitlbee-devel`. If Bitlbee was built from
  source don't forget to do `make install-dev`.

- `glib2` and headers => 2.32

  The library itself is usually installed as a dependency of Bitlbee, headers
  need to be installed separately. In Debian, the package containing them is
  libglib2.0-dev.

- `autotools` (if building from git)

  A bit of an overkill, but autotools is the build system of choice now, sorry.


Building and Installing
-----------------------

If building from git you will first need to generate autotools configuration
script and related files by executing the following command:

```
$ ./autogen.sh
```

After that (or when building from a tarball) you can build as usual:

```
$ ./configure
$ make
$ sudo make install
```

If your Bitlbee's plugindir is in non-standard location you can specify it by
calling ./configure with --with-plugindir=/path/to/plugindir option.

Debugging
---------

You can enable extra debug output for `bitlbee-mastodon`, by setting the
`BITLBEE_DEBUG` environment variable. This will print all traffic it exchanges
with Mastodon servers to STDOUT and there is a lot of it. To get it on your
screen run `bitlbee` by hand in foreground mode:

```
$ BITLBEE_DEBUG=1 bitlbee -nvD
```

Then connect with an IRC client as you usually do.

WARNING: there *is* sensitive information in this debug output, such as auth
tokens, your plaintext password and, obviously, your incoming and outgoing
messages. Be sure to remove any information you are not willing to share before
posting it anywhere.

If you are experiencing crashes please refer to [debugging
crashes](https://wiki.bitlbee.org/DebuggingCrashes) for information on how to
get a meaningful backtrace.

Bugs
----

Please report issues using the [GitHub
tracker](https://github.com/kensanata/bitlbee-mastodon/issues). For questions,
ping kensanata on irc.oftc.net/#bitlbee.
