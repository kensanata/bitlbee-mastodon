# The History of this Plugin

New features for 1.4.1:

- small improvements to command parsing and feedback
- better handling of multiple mentions
- better handling of direct messages
- possible fix to a crash related to filters

New features for 1.4.0:

- new `filter` command
- new `list` command
- new settings to hide notifications by type

Incompatible change in 1.4.0:

If you have subscribed to a hashtag, you need to change your channel
settings and prepend the hash. Without the hash, the plugin gets
confused and things the channel is for a list of the same name. Do
this from the control channel (&bitlbee). Let's assume you have a
channel called #hashtag. It's *room* setting should be #hashtag. If
it's lacking the initial hash:

> **&lt;kensanata&gt;** channel #hashtag set room  
> **&lt;root&gt;** room = `hashtag'  
> **&lt;kensanata&gt;** channel #hashtag set room #hashtag  
> **&lt;root&gt;** room = `#hashtag'  

There, fixed it.

New features for 1.3.1:

- new `visibility` command
- new `cw` command
- removed support for posting a content warning using CW1

New features for 1.2.0:

- format search results
- new `bio` command
- new `pinned` command
- add all the accounts when replying
- fixed list of accounts in the channel when connecting

New features for 1.1.0:

- new `hide_sensitive` setting

# Beginnings

This plugin started out as a fork of Bitlbee itself with the Mastodon
code being based on a copy of the Twitter code. When it became clear
that my code just wasn't going to get merged, I took a look at the
Facebook and Discord plugins for Bitlbee and decided that it should be
easy to turn my existing code into a plugin. Luckily, that worked as
intended.

– Alex Schroeder
