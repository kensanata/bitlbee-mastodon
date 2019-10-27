# Bitlbee Mastodon
This document was generated from the help text for the plugin.

Mastodon is a free, open-source social network server. A decentralized solution to commercial platforms, it avoids the risks of a single company monopolizing your communication. Anyone can run Mastodon and participate in the social network seamlessly.

* *[news](#news)* - Backwards incompatible changes
* *[register](#register)* - Registering an account
* *[connect](#connect)* - Connecting to an instance
* *[read](#read)* - Reading your timeline
* *[post](#post)* - Posting a new status
* *[undo](#undo)* - Undo and redo
* *[dm](#dm)* - Sending direct messages
* *[context](#context)* - Showing a status in its context
* *[reply](#reply)* - Replying to a status
* *[delete](#delete)* - Deleting a status
* *[favourite](#favourite)* - Favouring a status
* *[follow](#follow)* - Following an account
* *[block](#block)* - Blocking an account
* *[mute](#mute)* - Muting an account
* *[boost](#boost)* - Boosting a status
* *[more](#more)* - Getting more information about things
* *[search](#search)* - Searching for accounts and hashtags
* *[spam](#spam)* - Reporting a status
* *[control](#control)* - Commands in the control channel
* *[hashtag](#hashtag)* - Showing and following a hashtag
* *[public](#public)* - Showing and following the local or federated timeline
* *[lists](#lists)* - Managing lists
* *[filters](#filters)* - Managing filters
* *[notifications](#notifications)* - Showing your notifications
* *[set](#set)* - Settings affecting Mastodon accounts

## news
Use **plugins** in the control channel (**&bitlbee**) to learn which version of the plugin you have.

Incompatible change in **1.4.3**: The URL path is ignored in the base URL (such that we can use /api/v2/search).

Incompatible change in **1.4.0**: If you have subscribed to a hashtag, you need to change your channel settings and prepend the hash. Do this from the control channel (**&bitlbee**). Let's assume you have a channel called #hashtag. It's **room** setting should be **#hashtag**. If it's lacking the initial hash:

> **&lt;kensanata&gt;** channel #hashtag set room  
> **&lt;root&gt;** room = `hashtag'  
> **&lt;kensanata&gt;** channel #hashtag set room #hashtag  
> **&lt;root&gt;** room = `#hashtag'  

Don't forget to save your settings.

## register
You need to register your Mastodon account on an **instance**. See https://instances.social/ if you need help picking an instance. It's a bit like picking a mail server and signing up. Sadly, there is currently no way to do this from IRC. Your need to use a web browser to do it. Once you have the account, see **help account add mastodon** for setting up your account.

## set
These settings will affect Mastodon accounts:

* **set auto_reply_timeout** - replies to most recent messages in the last 3h
* **set base_url** - URL for your Mastodon instance's API
* **set commands** - extra commands available in Mastodon channels
* **set message_length** - limit messages to 500 characters
* **set mode** - create a separate channel for contacts/messages
* **set show_ids** - display the "id" in front of every message
* **set target_url_length** - an URL counts as 23 characters
* **set name** - the name for your account channel
* **set hide_sensitive** - hide content marked as sensitive
* **set sensitive_flag** - text to flag sensitive content with
* **set visibility** - default post privacy
* **set hide_boosts** - hide notifications of people boosting
* **set hide_favourites** - hide notifications of favourites
* **set hide_follows** - hide notifications of follows
* **set hide_mentions** - hide notifications of mentions

Use **help** to learn more about these options.

## set name
> **Type:** string  
> **Scope:** account  
> **Default:** empty  

Without a name set, Mastodon accounts will use host URL and acccount name to create a channel name. This results in a long channel name and if you prefer a shorter channel name, use this setting (when the account is offline) to change it.

> **&lt;kensanata&gt;** account mastodon off  
> **&lt;kensanata&gt;** account mastodon set name masto  
> **&lt;kensanata&gt;** account mastodon on  
> **&lt;kensanata&gt;** save  

## set hide_sensitive
> **Type:** boolean  
> **Scope:** account  
> **Default:** false  
> **Possible Values:** true, false, rot13, advanced_rot13  

By default, sensitive content (content behind a content warning) is simply shown. The content warning is printed, and then the sensitive content is printed.

> **&lt;somebody&gt;** [27] [CW: this is the warning] \*NSFW\* this is the text  

If you set this variable in the control channel (**&bitlbee**), sensitive content is not printed. Instead, you'll see "[hidden: &lt;the URL&gt;]". If you still want to read it, visit the URL.

> **&lt;kensanata&gt;** account mastodon set hide_sensitive true  
> **&lt;kensanata&gt;** save  

Don't forget to save your settings.

The result:

> **&lt;somebody&gt;** [27] [CW: this is the warning] \*NSFW\* [hidden: https://social.nasqueron.org/@kensanata/100133795756949791]  

Additionally, when using **rot13**:

> **&lt;somebody&gt;** [27] [CW: this is the warning] \*NSFW\* guvf vf gur grkg  

And when using **advanced_rot13**:

> **&lt;somebody&gt;** [27] [CW: this is the warning] \*NSFW\*   
> **&lt;somebody&gt;** CW1 guvf vf gur grkg  

All sensitive content is also marked as Not Safe For Work (NSFW) and flagged as such. You can change the text using an option, see **help set sensitive_flag**).

## set sensitive_flag
> **Type:** string  
> **Scope:** account  
> **Default:** "\*NSFW\* "  

This is the text to flag sensitive content with. You can change this setting in the control channel (**&bitlbee**). The default is Not Safe For Work (NSFW). If you wanted to simply use red for the sensitive content, you could use "^C5", for example. Be sure to use an actual Control-C, here. This might be challenging to enter, depending on your IRC client. Sadly, that's how it goes. For more information, see https://www.mirc.com/colors.html.

## set visibility
> **Type:** string  
> **Scope:** account  
> **Default:** "public"  

This is the default visibility of your toots. There are three valid options: "public" (everybody can see your toots), "unlisted" (everybody can see your toots but they are not found on the public timelines), and "private" (only followers can see them). For more information see https://github.com/tootsuite/documentation/blob/master/Using-Mastodon/User-guide.md#toot-privacy. This is used whenever you make a new post. You can override this visibility for new posts in the control channel (**&bitlbee**). See *[post](#post)* for more.

> **&lt;kensanata&gt;** account mastodon set visibility private  
> **&lt;kensanata&gt;** save  

Don't forget to save your settings.

## set hide_boosts
> **Type:** boolean  
> **Scope:** account  
> **Default:** false  
> **Possible Values:** true, false  

By default, you are shown a notification when somebody boosts a status of yours. This setting allows you to turn it off in the control channel (**&bitlbee**).

> **&lt;kensanata&gt;** account mastodon set hide_boosts true  
> **&lt;kensanata&gt;** save  

Don't forget to save your settings.

## set hide_favourites
> **Type:** boolean  
> **Scope:** account  
> **Default:** false  
> **Possible Values:** true, false  

By default, you are shown a notification when somebody favourites a status of yours. This setting allows you to turn it off in the control channel (**&bitlbee**).

> **&lt;kensanata&gt;** account mastodon set hide_favourites true  
> **&lt;kensanata&gt;** save  

Don't forget to save your settings.

## set hide_follows
> **Type:** boolean  
> **Scope:** account  
> **Default:** false  
> **Possible Values:** true, false  

By default, you are shown a notification when somebody follows you. This setting allows you to turn it off in the control channel (**&bitlbee**).

> **&lt;kensanata&gt;** account mastodon set hide_follows true  
> **&lt;kensanata&gt;** save  

Don't forget to save your settings.

## set hide_mentions
> **Type:** boolean  
> **Scope:** account  
> **Default:** false  
> **Possible Values:** true, false  

By default, you are shown a notification when somebody mentions you. This setting allows you to turn it off in the control channel (**&bitlbee**).

> **&lt;kensanata&gt;** account mastodon set hide_mentions true  
> **&lt;kensanata&gt;** save  

Don't forget to save your settings.

## account add mastodon
> **Syntax:** account add mastodon &lt;handle&gt;  

By default all the Mastodon accounts you are following will appear in a new channel named after your Mastodon instance. You can change this behaviour using the **mode** setting (see **help set mode**).

To send toots yourself, just write in the groupchat channel.

Since Mastodon requires OAuth authentication, you should not enter your Mastodon password into BitlBee. The first time you log in, BitlBee will start OAuth authentication. (See **help set oauth**.)

In order to connect to the correct instances, you must most probably change the **base_url** setting. See *[connect](#connect)* for an example.

## connect
In this section, we'll sign in as **@kensanata@mastodon.weaponvsac.space**. This section assumes an existing account on an instance! Replace username and Mastodon server when trying it.

In your **&bitlbee** channel, add a new account, change it's **base_url** to point at your instance, and switch it on:

> **&lt;kensanata&gt;** account add mastodon @kensanata  
> **&lt;root&gt;** Account successfully added with tag mastodon  
> **&lt;kensanata&gt;** account mastodon set base_url https://mastodon.weaponvsac.space  
> **&lt;root&gt;** base_url = `https://mastodon.weaponvsac.space  
> **&lt;kensanata&gt;** account mastodon on  
> **&lt;root&gt;** mastodon - Logging in: Login  
> **&lt;root&gt;** mastodon - Logging in: Parsing application registration response  
> **&lt;root&gt;** mastodon - Logging in: Starting OAuth authentication  

At this point, you'll get contacted by the user **mastodon_oauth** with a big URL that you need to visit using a browser. See *[connect2](#connect2)* for the OAuth authentication.

## connect2
Visit the URL the **mastodon_oauth** user gave you and authenticate the client. You'll get back another very long string. Copy and paste this string:

> **&lt;mastodon_oauth&gt;** Open this URL in your browser to authenticate: https://.......  
> **&lt;mastodon_oauth&gt;** Respond to this message with the returned authorization token.  
> **&lt;kensanata&gt;** \*\*\*\*\*\*\*\*\*\*\*\*\*\*\*\*\*\*\*\*\*\*\*\*\*\*\*\*\*\*\*\*\*\*\*\*\*\*\*\*\*\*\*\*\*\*\*\*\*\*\*\*\*\*\*\*\*\*\*\*\*\*\*\*  

Once you do that, your login should complete in the **&bitlbee** channel:

> **&lt;root&gt;** mastodon2 - Logging in: Requesting OAuth access token  
> **&lt;root&gt;** mastodon2 - Logging in: Connecting  
> **&lt;root&gt;** mastodon2 - Logging in: Verifying credentials  
> **&lt;root&gt;** mastodon2 - Logging in: Getting home timeline  
> **&lt;root&gt;** mastodon2 - Logging in: Logged in  

You should now have a channel called **#mastodon.weaponsvsac.space@localhost** where all the status updates and notifications get shown. We'll call this your **account channel**. See **help set name** to change it's name.

Mastodon gives BitlBee a permanent authentication token, which is saved in your configuration.

You should probably save this configuration.

> **&lt;kensanata&gt;** save  
> **&lt;root&gt;** Configuration saved  

## read
The default **mode** setting is **chat**. This means that each Mastodon account you add results in a new channel in your IRC client.

Use **help set mode** in your Bitlbee control channel (**&bitlbee**) to read up on different modes.

## post
The default **commands** setting is **true**. This means that anything you type is a toot unless it looks like command, in which case it is handled as such. In addition to that, you can use the **post &lt;message&gt;** command. If you set the **commands** setting to **strict**, using the **post** command is mandatory.

Use **help set commands** in your Bitlbee control channel (**&bitlbee**) to read up on the various commands.

New posts get the default visibility: "public". See **help set visibility** for more. You can change the visibility of the next toot by using the **visibility** command.

> **&lt;kensanata&gt;** visibility unlisted  
> **&lt;root&gt;** Next post is unlisted  
> **&lt;kensanata&gt;** Hello!  
> **&lt;root&gt;** You, unlisted: [06] Hello!  

Use **cw &lt;content warning&gt;** to set a content warning for your next reply or post.

> **&lt;kensanata&gt;** cw capitalism  
> **&lt;root&gt;** Next post will get content warning 'capitalism'  
> **&lt;kensanata&gt;** No! Just no!  
> **&lt;root&gt;** You: [09] [CW: capitalism] \*NSFW\* No! Just no!  

When mentioning people in your toots, make sure to qualify them appropriately.

> **&lt;somebody&gt;** I'm using @kensanata@octodon.social's Mastodon plugin for Bitlbee.  

By default the Mastodon server limits your toots to 500 characters. Bitlbee tries to compute the message length based on the various Mastodon rules and prevents you from posting longer messages. Use **help set message_length** in your Bitlbee control channel (**&bitlbee**) to read up on the hairy details. Basically, some aspects of of your message will count for less: URLs, domain names for mentioned user accounts and the like. See **help set target_url_length** for more information on how URLs are counted.

Note also that Bitlbee itself does word-wrapping to limit messages to 425 characters. That is why longer messages may look like extra newlines have been introduced but if you check the status on the web, you'll see that everything is OK.

## undo
Use **undo** and **redo** to undo and redo recent commands. Bitlbee will remember your last 10 Mastodon commands and allows you to undo and redo them.

Use **history** to see the list of commands you can undo. There is a pointer (**&gt;**) showing the current position.

Use **history undo** if you are interested in seeing the commands that will be used to undo what you just did.

## favourite
Use **fav &lt;id|nick&gt;** to favour a status or the last status by a nick. Synonyms: **favourite**, **favorite**, **like**.

Use **unfav &lt;id|nick&gt;** to unfavour a status or the last status by a nick. Synonyms: **unfavourite**, **unfavorite**, **unlike**, **dislike**.

## dm
There are two ways to send a direct message to someone (also known as a DM).

If you are following them, then there exists a handle for them in your IRC client and you can simply **/msg &lt;nick&gt;**

To DM anyone regardless of whether you follow them, use **visibility direct** and then write a post that mentions their long @nick@server account name.

See **help set visibility** for more.

## context
Use **context &lt;id|nick&gt;** to show some context for a status or the last status by a nick. This will display the ancestors and descendants of a status.

Use **timeline &lt;nick&gt;** to show the most recent messages by a nick. Use **more** to show more statuses from the same command.

Use **timeline @&lt;account&gt;** to show the most recent messages by an account that isn't a nick in your channel. Use **more** to show more statuses from the same command.

## reply
If you use the default IRC conventions of starting a message with a nickname and a colon (**:**) or a comma (**,**), then your message will be treated as a reply to that nick's last message. As is custom, the recipient and all the people they mentioned in their toot get mentioned in your reply. This only works if that nick's last message was sent within the last 3h. For more information about this time window use **help set auto_reply_timeout** in your Bitlbee control channel (**&bitlbee**).

You can also reply to an earlier message by referring to its id using the **reply &lt;id&gt; &lt;message&gt;** command. Again, the recipient and all the people they mentioned in their toot get mentioned in your reply.

If you set the **commands** setting to **strict**, using the **reply** command is mandatory.

When replying to a post, your reply uses the same visibility as the original toot unless your default visibility is more restricted: direct &gt; private &gt; unlisted &gt; public. See **help set visibility** for more. You can set a different visibility using the **visibility** command. See *[post](#post)* for more.

When replying to a post, your reply uses the same content warning unless you have set up a different one via **cw**. See *[post](#post)* for more.

## delete
Use **del &lt;id&gt;** to delete a status or your last status. Synonym: **delete**.

## favourite
Use **fav &lt;id|nick&gt;** to favour a status or the last status by a nick. Synonyms: **favourite**, **favorite**, **like**.

Use **unfav &lt;id|nick&gt;** to unfavour a status or the last status by a nick. Synonyms: **unfavourite**, **unfavorite**, **unlike**, **dislike**.

## follow
Use **follow &lt;nick|account&gt;** to follow somebody. This determines the nicks in your channel. Verify the list using **/names**.

Usually you'll be providing a local or remote account to follow. In the background, Bitlbee will run a search for the account you provided and follow the first match. Sometimes there will be nicks in the channel which you are not following, e.g. a nick is automatically added to the channel when a status of theirs mentioning you is shown.

Use **unfollow &lt;nick&gt;** to unfollow a nick. Synonyms: **allow**.

## block
Use **block &lt;nick&gt;** to block a nick on the server. This is independent of your IRC client's **/ignore** command, if available.

Use **unblock &lt;nick&gt;** to unblock a nick.

## mute
Use **mute user &lt;nick&gt;** to mute a nick on the server.

Use **unmute user &lt;nick&gt;** to unmute a nick.

Use **mute &lt;id|nick&gt;** to mute the conversation based on a status or the last status by a nick. Muting a status will prevent replies to it, favourites and replies of it from appearing.

Use **unmute &lt;id|nick&gt;** to unmute the conversation based on a status or the last status by a nick.

## boost
Use **boost &lt;id|nick&gt;** to boost a status or the last status by a nick.

Use **unboost &lt;id|nick&gt;** to unboost a status or the last status by a nick.

## more
Use **url &lt;id|nick&gt;** to get the URL to a status or the last status by a nick.

Use **whois &lt;id|nick&gt;** to show handle and full name by a nick, or of all the nicks mentioned in a status.

Use **bio &lt;nick&gt;** to show the bio of a nick.

Use **pinned &lt;nick&gt;** to show the pinned statuses of a nick. Use **pin &lt;id&gt;** to pin a status to your profile and use **unpin &lt;id&gt;** to unpin a status from your profile.

Use **info instance** to get debug information about your instance.

Use **info user &lt;nick|account&gt;** to get debug information about an account.

Use **info relation &lt;nick|account&gt;** to get debug information about the relation to an account.

Use **info &lt;id|nick&gt;** to get debug information about a status or the last status by a nick.

Use **api &lt;get|put|post|delete&gt; &lt;endpoint&gt; &lt;optional args&gt;** to call the API directly.

> **&lt;kensanata&gt;** api get /lists  
> **&lt;root&gt;** {  
> **&lt;root&gt;**  id: 5  
> **&lt;root&gt;**  title: Bots  
> **&lt;root&gt;** }  
> **&lt;root&gt;** {  
> **&lt;root&gt;**  id: 9  
> **&lt;root&gt;**  title: OSR  
> **&lt;root&gt;** }  
> **&lt;kensanata&gt;** api delete /lists/5  
> **&lt;kensanata&gt;** api get /lists  
> **&lt;root&gt;** {  
> **&lt;root&gt;**  id: 9  
> **&lt;root&gt;**  title: OSR  
> **&lt;root&gt;** }  
> **&lt;kensanata&gt;** api post /lists/9/accounts account_ids[] 13250  

## search
Mastodon allows you to search for accounts, hashtags, and statuses you've written, boosted, favourited or were mentioned in, if your instance has this feature enabled.

Use **search &lt;what&gt;** to search for all these things.

You can also search for a specific status by searching the URL of said status. This sounds strange but it will allow you to boost it, for example.

If you want to show the statuses for a specific account or a hashtag, use **timeline &lt;nick|#hashtag&gt;**.

If you want to subscribe to a particular hashtag, see *[hashtag](#hashtag)* for more.

## spam
Use **report &lt;id|nick&gt; &lt;comment&gt;** to report a status or the last status by a nick. Synonyms:**spam**.

Note that the comment is mandatory. Explain why the status is being reported. The administrator of your instance will see this report and decide what to do about it, if anything.

## control
As we said at the beginning, the default **mode** setting is **chat**. This means that each Mastodon account you add will result in a new channel in your IRC client. All the commands mentioned above are what you type in this "instance channel."

There are some standard root commands that only work in the control channel, **&bitlbee**.

## hashtag
Use **timeline #&lt;hashtag&gt;** to show the most recent messages for a hashtag. Use **more** to show more statuses from the same command.

If you want to follow a hashtag, you need to use the control channel, **&bitlbee**.

Here's how to subscribe to **#hashtag** for the account **mastodon**:

> **&lt;kensanata&gt;** chat add mastodon #hashtag  
> **&lt;kensanata&gt;** channel #hashtag set auto_join true  
> **&lt;kensanata&gt;** /join #hashtag  

Don't forget to **save** your config.

Use **channel list** to see all the channels you have available. See **help chat add** for more information.

Note that where as you can still issue commands in these hashtag channels, the output is going to appear in the original **account channel**.

If you try to subsribe to the same hashtag on a different instance, you'll run into a problem. The solution is to use a different name for the channel and then use **channel &lt;channel&gt; set room #hashtag**.

> **&lt;kensanata&gt;** chat add mastodon2 #hashtag  
> **&lt;root&gt;** A channel named `#hashtag already exists...  
> **&lt;kensanata&gt;** chat add mastodon2 #hashtag2  
> **&lt;kensanata&gt;** channel #hashtag2 set room #hashtag  
> **&lt;kensanata&gt;** channel #hashtag2 set auto_join true  
> **&lt;kensanata&gt;** /join #hashtag2  

## public
Use **timeline local** to show the most recent messages for the local timeline (these are statuses from accounts on your instance). Use **timeline federated** to show the most recent messages for the federated timeline (these are statuses from the local accounts and anybody they are following). Use **more** to show more statuses from the same command.

If you want to follow a hashtag, or the local, or the feredated timeline, you need to use the control channel, **&bitlbee**. The following assumes that your account is called **mastodon**. The **chat add** command takes the parameters **account**, **timeline**, and **channel name**. In the example we're giving the channel a similar name. You can name the channel whatever you want. The important part is that the channel **topic** must be the name of the timeline it is subscribing to.

> **&lt;kensanata&gt;** chat add mastodon local #local  
> **&lt;kensanata&gt;** channel #local set auto_join true  
> **&lt;kensanata&gt;** /join #local  

Or:

> **&lt;kensanata&gt;** chat add mastodon federated #federated  
> **&lt;kensanata&gt;** channel #federated set auto_join true  
> **&lt;kensanata&gt;** /join #federated  

Don't forget to **save** your config.

Note that where as you can still issue commands in these channels, the output is going to appear in the original **account channel**.

## lists
You can make lists of the people you follow. To read the latest messages of people in a list, use **timeline &lt;title&gt;**. If you then create a separate channel for a list, you'll see only statuses of people in that list.

These are the commands available:
> **list** (to see your lists)  
> **list create &lt;title&gt;**  
> **list delete &lt;title&gt;**  
> **list &lt;title&gt;** (to see the accounts in a list)  
> **list add &lt;nick&gt; to &lt;title&gt;**  
> **list remove &lt;nick&gt; from &lt;title&gt;**  
> **list reload** (when you are done changing list memberships)  
> **timeline &lt;title&gt;** (to read statuses from these accounts)  

Example:

> **&lt;somebody&gt;** follow kensanata@octodon.social  
\*\*\* kensanata JOIN
> **&lt;root&gt;** You are now following kensanata.  
> **&lt;somebody&gt;** list create Important people  
> **&lt;root&gt;** Command processed successfully  
> **&lt;somebody&gt;** list add kensanata to Important people  
> **&lt;root&gt;** Command processed successfully  

If you want to follow a list, you need to use the control channel, **&bitlbee**. The following assumes that your account is called **mastodon**. The **chat add** command takes the parameters **account**, **title**, and **channel name**. As list titles may contain spaces, you need to use quotes. You can name the channel whatever you want. The important part is that the channel **topic** must be the title of a list.

> **&lt;kensanata&gt;** chat add mastodon "Important people" #important  
> **&lt;kensanata&gt;** channel #important set auto_join true  
> **&lt;kensanata&gt;** /join #important  

Don't forget to **save** your config.

## filters
You can create filters which remove toots from the output.

These are the commands available:
> **filter** (to load and see your filters)  
> **filter create &lt;phrase&gt;**  
> **filter delete &lt;n&gt;**  

Currently filters are created for all contexts, for whole words, and without an expiration date. Fine grained control over the filters created are planned. Furthermore, for whole words, word edges are weird. For a whole word phrase to match, the beginnin and end of the phrase have to be one of a-z, A-Z, or 0-9, and the character before the phrase and after the phrase must not be one of these. Matching does not care about canonical Unicode form, I'm afraid.

Example:

> **&lt;kensanata&gt;** filter create twitter.com  
> **&lt;root&gt;** Filter created  
> **&lt;kensanata&gt;** filter  
> **&lt;root&gt;**  1. twitter.com (properties: everywhere, server side, whole word)  

Differences from the Mastodon Web UI: case is significant; toots are filtered even if you look at an account timeline or run a search.

## notifications
Use **notifications** to show the most recent notifications again. Use **more** to show more notifications.

Note that there are settigns to hide notifications of a particular kind. Once you do that, the **notifications** and **more** commands may show less output, or none at all, as the display of some notifications is suppressed. See **help set hide_boosts**, **help set hide_favourites**, **help set hide_follows**, and **help set hide_mentions**.

## api
You can send stuff to the Mastodon API yourself, too. Use **api** to do this.

Example:

> **&lt;kensanata&gt;** 23:19 &lt;kensanata&gt; api post /lists?title=test  
> **&lt;root&gt;** id: 635  
> **&lt;root&gt;** title: test  

