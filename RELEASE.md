# How to prepare a release

1. verify that `mnews` in `mastodon-help.txt` is up to date
2. change the version number in `configure.ac`
3. change the version number in `mastodon.c:init_plugin_info`
4. tag the commit
5. push the tag to `origin` and `github`
