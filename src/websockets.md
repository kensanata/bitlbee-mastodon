# Websockets

Current status:

1. all the timelines are handled via websockets; home, local and
   federated timelines work as expected

2. joining a hashtag timeline and a list timeline doesn't work: every
   5s you'll get a new topic setup message ("root TOPIC BitlBee
   groupchat: "scotland". Please keep in mind that root-commands won't
   work here. Have fun!") and when you part, bitlbee crashes

3. adding people to a list doesn't work

## Specification

[RFC 6455](https://tools.ietf.org/html/rfc6455)

## How to understand Websockets

We need to get an authorisation token. One way to get this is as
follows, registering the client "Bitlbee":

```
curl -X POST -d "client_name=Bitlbee&redirect_uris=urn:ietf:wg:oauth:2.0:oob&scopes=read" \
     -Ss https://octodon.social/api/v1/apps
```

Get back a JSON thing that looks as follows:

```
{
  "id":1878,
  "redirect_uri":"urn:ietf:wg:oauth:2.0:oob",
  "client_id":"CLIENT_ID_HERE",
  "client_secret":"CLIENT_SECRET_HERE"
}
```

Use them, with your username and password:

```
curl -X POST -d "client_id=CLIENT_ID_HERE&client_secret=CLIENT_SECRET_HERE&grant_type=password&username=YOUR_EMAIL&password=YOUR_PASSWORD" -Ss https://octodon.social/oauth/token
```

Get back a JSON thing containg the auth token:

```
{
  "access_token":"ACCESS_TOKEN_HERE",
  "token_type":"bearer",
  "scope":"read",
  "created_at":1492100523
}
```

Now we're getting to the Websocket part. We want to send something
like the following:

```
GET /api/v1/streaming HTTP/1.1 
Host: octodon.social 
Connection: keep-alive, Upgrade 
Upgrade: websocket 
Origin: https://octodon.social
Pragma: no-cache 
Cache-Control: no-cache
Authorization: Bearer ACCESS_TOKEN_HERE
Sec-WebSocket-Version: 13 
Sec-WebSocket-Key: cafUtBCcvi6j8xME85i+oQ== 
User-Agent: BitlBee 3.6 
```

This is how we do it, piping the output through a hex dumper:

```
curl --http1.1 --include \
  --header "Authorization: Bearer ACCESS_TOKEN_HERE" \
  --header "Upgrade: websocket" \
  --header "Origin: https://octodon.social" \
  --header "Sec-WebSocket-Version: 13" \
  --header "Sec-WebSocket-Key: cafUtBCcvi6j8xME85i+oQ==" \
  "https://octodon.social/api/v1/streaming?stream=public"
```

As for the frames themselves, this is from the RFC:

```
Frame format:  
​​
      0                   1                   2                   3
      0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
     +-+-+-+-+-------+-+-------------+-------------------------------+
     |F|R|R|R| opcode|M| Payload len |    Extended payload length    |
     |I|S|S|S|  (4)  |A|     (7)     |             (16/64)           |
     |N|V|V|V|       |S|             |   (if payload len==126/127)   |
     | |1|2|3|       |K|             |                               |
     +-+-+-+-+-------+-+-------------+ - - - - - - - - - - - - - - - +
     |     Extended payload length continued, if payload len == 127  |
     + - - - - - - - - - - - - - - - +-------------------------------+
     |                               |Masking-key, if MASK set to 1  |
     +-------------------------------+-------------------------------+
     | Masking-key (continued)       |          Payload Data         |
     +-------------------------------- - - - - - - - - - - - - - - - +
     :                     Payload Data continued ...                :
     + - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - +
     |                     Payload Data continued ...                |
     +---------------------------------------------------------------+
```

The known opcodes are:

```
  OPCODE_CONTINUATION = 0x0,
  OPCODE_TEXT         = 0x1,
  OPCODE_CLOSE        = 0x8,
  OPCODE_PING         = 0x9,
  OPCODE_PONG         = 0xA,
```

## How to Debug

1. build it: `make`
2. install it: `sudo make install`
3. stop bitlbee completely: `sudo systemctl stop bitlbee && sudo killall bitlbee`
4. start bitlbee: `sudo systemctl start bitlbee`
5. monitor the unit in a separate terminal: `sudo journalctl -fu bitlbee`
6. connect with your IRC client and watch the `&bitlbee` channel

