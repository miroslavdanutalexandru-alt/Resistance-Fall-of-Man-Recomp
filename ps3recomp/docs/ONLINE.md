# Online play (psnr)

Titles that matchmake through sceNpMatching2 find each other through a
[psnr](https://github.com/sp00nznet/psnr) server. psnr is a small stand-in for
PSN's rooms and leaderboards. Once players are matched, the title's own
traffic goes directly between them over `sys_net` P2P sockets; the server does
not carry it.

Offline is still the default. Nothing below changes a title unless it is
set.

The runtime's command line takes two flags, anywhere after the program name:

| Flag | What it does |
|---|---|
| `--psnr host[:port]` | Go online with this server: the same as `PS3_NET_ONLINE=1` plus `PSNR_SERVER`. |
| `--username NAME` | The player's name; wins over `PS3_NP_ONLINE_ID`. |

The name follows PSN's online ID rules: 3-16 letters, digits, `-` or `_`,
starting with a letter. Other characters are dropped, and a name that still
doesn't fit is ignored with a message. With no name set, an online game
(`PS3_NET_ONLINE`, with or without a psnr server) uses a random name such as
"ExcitingHippo" (`np_psnr_random_id`), never the OS login name. The name is
saved to `PS3_NP_ID_FILE` (default `online_id.txt` in the working directory)
and reused on later runs, so the player stays the same. The file's first line
is the name: edit it to rename yourself (an invalid name is reported and a
random one used for that run, and the file is left alone). The second line is
a random salt: the account ID, OpenPSID and MAC come from the name plus the
salt, so two players who end up with the same name are still two players.
Keep it when renaming. Offline with no
name, nothing changes ("PS3Player").

| Variable | What it does |
|---|---|
| `PS3_NET_ONLINE=1` | Real host sockets (`libs/network/sysNet.c`), cellNetCtl reports a connection, and the NP manager reports ONLINE (signed in). |
| `PSNR_SERVER=host[:port]` | The psnr server for sceNpMatching2 and sceNpScore. Port 36100 by default. Not needed by titles with their own backend. |
| `PS3_NP_ONLINE_ID=name` | The player's name, if `--username` isn't given. The name also gives the instance its own console identity: the OpenPSID (`sys_ss_get_open_psid`) and MAC are derived from it (with no salt, unlike the saved name). Titles tell consoles apart by these, so two instances with the same name count as one player. Offline with no name, both stay at their old fixed values, so offline saves are untouched. |
| `PS3_NP_ID_FILE=<file>` | Where the online name and its salt are saved and read back. Default `online_id.txt`. Only used online with no name set. |
| `PS3_OPEN_PSID=<32 hex>` | Set the OpenPSID outright instead. |
| `PS3_NET_P2P_PORT=n` | The host port this instance's P2P sockets bind, whatever port the title asks for. Default 3658. psnr hands this port to peers. Two instances on one machine need two ports. |
| `PS3_NP_SIGNALING_DELAY_MS=n` | How long after a member joins that signaling reports them "established". Default 1000. |
| `PS3_NET_TRACE=1` | Log every packet sent, and every packet received (not the empty polls). |
| `PS3_NET_HOSTS=<file>` | Hostname overrides, one `name address` (or `name=address`) per line, `#` comments; the address is an IPv4 address or a hostname the host resolves. Names match case-insensitively. They apply to `gethostbyname` and to DNS queries the title sends itself (see below). Unset by default: nothing is overridden. Applies to `PS3_NET_ONLINE` only. |

## Titles with their own network backend

Not every online title uses `sceNpMatching2`. Some match through their own
middleware (Sony's Medius Client Library, for one) and connect to hardcoded
hostnames of a service that has since been shut down. For those,
`PS3_NET_ONLINE=1` is what matters: it makes `sys_net` real, so the title's
own stack runs against the host network. psnr is not involved.

To send such a title to a community server, list its hostnames in a
`PS3_NET_HOSTS` file. Both ways a title can resolve a name are covered:

- `gethostbyname`: answered from the file; the log shows
  `[sys_net] gethostbyname('name') -> address (PS3_NET_HOSTS)`.
- Its own DNS client: some titles send a UDP query straight to a public
  resolver. A single-question A/IN query to port 53 for a listed name is
  answered locally and never sent (`[sys_net] DNS 'name' -> address
  (answered locally)`). Any other query goes to the real server untouched;
  with `PS3_NET_TRACE=1` the log names those too.

Titles like these usually log in with an NP ticket
(`sceNpManagerRequestTicket`, then `sceNpManagerGetTicket` and
`sceNpManagerGetTicketParam`). Completion is reported as
`SCE_NP_MANAGER_EVENT_GOT_TICKET` through the callback the title registered
with `sceNpManagerRegisterCallback`. The ticket is not SCE-signed. It
carries a `PS3RECOMP-NP-TICKET` header and the player's online ID, and
nothing that identifies the machine; the account ID is a hash of the online
ID. Only a server written to accept such a ticket will take it.

## Community servers with RPCN sign-in (PORT_ONLINE)

Revival servers such as PS Rewired check a real, RPCN-signed NP ticket. A
port can get one with `PORT_ONLINE=1` (off by default; with it unset nothing
here runs). The code is in `libs/network/port_online.c`, and the port's
startup calls `port_online_register()` before the network modules register.

What `PORT_ONLINE=1` does:
- reads `online.ini` (`PORT_ONLINE_CONFIG=path`, else the working directory,
  else next to the exe) and sets `PS3_NET_ONLINE=1`, so sockets are real and
  NP reports ONLINE;
- `host.NAME = IP` lines and `dns = IP` redirect the title's lookups, both
  through `gethostbyname` and the title's own DNS queries (answered locally,
  as above); `host.` lines win over `dns=`;
- cellNetCtl reports the routed LAN address as the IP and `dns=` as the
  primary DNS;
- with `rpcn_user` and `rpcn_password` set, the online ID is the RPCN user,
  and `sceNpManagerRequestTicket` logs in to `rpcn_host` (TLS, port 31313)
  on a worker thread and requests a ticket for the title's service ID.
  GOT_TICKET is reported when it arrives. `sceNpManagerGetTicket` returns
  the server's ticket as is, and `sceNpManagerGetTicketParam` reads its
  fields. A failed sign-in reports GOT_TICKET with `SCE_NP_ERROR_OFFLINE`,
  and GetTicket returns that error. Without an account the made-up ticket
  above stands.

The password is never sent in plain text: RPCN takes
PBKDF2-HMAC-SHA3-256(password, "No matter where you go, everybody's
connected.", 200000 iterations, 32 bytes) as 64 uppercase hex digits, the
value RPCS3 stores. A 64-hex-digit `rpcn_password` is taken as that value.
`PORT_ONLINE_PROBE=1` connects to `rpcn_host` at startup, reads the
server's protocol version (34) and logs it, without logging in. Log lines
start with `[online]`.

Licence: RPCS3 is GPL-2.0, and none of its code is used. The RPCN packet
format, the ticket 2.1 layout and the password derivation were written from
the protocol's observable wire format and the psdevwiki notes. SHA3, HMAC
and PBKDF2 are written here from FIPS 202 / RFC 2104 / RFC 8018, and checked
against Python's hashlib in `tests/test_port_online.c`.

Resistance: Fall of Man runs as two processes (front end and levels) and
switches with `exitspawn` on level loads. Each process signs in again, and
a connection open at the switch is closed. The environment, `PORT_ONLINE`
included, carries over.

## Two players on one machine

```
psnr                                    # server/ in the psnr repo; listens on :36100
PS3_NET_P2P_PORT=3658 ./build/simpsons <EBOOT.elf> --psnr 127.0.0.1 --username homer
PS3_NET_P2P_PORT=3659 ./build/simpsons <EBOOT.elf> --psnr 127.0.0.1 --username bart
```

Redirect stderr to a file and the runtime logs every wait (see
`runtime/ps3_log.h`). Two instances doing that crawl to about 1 fps once the
stage starts, and the title drops the peer as "not responding". Set
`PS3_VERBOSE=0` for play; at that setting both hold 40-50 fps.

## What is implemented

| Module | State |
|---|---|
| sceNpMatching2 | Contexts, one server and one world (both answered locally), and rooms on psnr: search, create/join, leave, kick, room data, room messages. Room, room-message and signaling callbacks are delivered from `cellSysutilCheckCallback`. Signaling reports a member as established as soon as psnr names them. |
| sceNpScore | Title and transaction contexts. Record score, and ranking by range and by NP ID, sync and async, all on psnr leaderboards. |
| sceNpLookup | Profiles are answered locally from the online ID. There is no avatar. |
| cellSysutilAvc2 | Voice chat loads, joins and streams, but carries no audio. |
| sceNp manager | Reports ONLINE when psnr is set, and fires the manager callback once. |
| sys_net | P2P sockets map to plain UDP/TCP on `PS3_NET_P2P_PORT`. There is no vport multiplexing. |

## How it was verified

Simpsons Arcade (NPUB30563) was run as two instances on one machine:
- the host creates a match and the other player finds it with Quick Match;
- both appear in each other's online lobby and pick characters;
- the host starts the game, sends its game setup (58 KB, zlib-compressed by
  the title, over a TCP P2P socket), and both instances play Stage 1
  together; one player's moves show on both screens;
- the room goes through psnr, and game traffic runs peer to peer over UDP
  and TCP P2P sockets.

The setup transfer needs ps3recomp#202: the title's zlib is lifted code, and a
lifter bug left its compressor emitting a 10-byte stream.

Offline, with none of the variables set, Simpsons boots, reports NP offline,
shows its "sign in to post scores" notice, and plays Stage 1.

Tests: `libs/network/tests/test_np_matching2.c` covers response relocation,
attribute packing and the room structures a title reads. `test_sys_net.c`
covers sockets over loopback, including a P2P bind and receive, and two P2P
stream sockets sharing the port with the peer seeing that port on accept.

What it took to get the lobby up, each of which a title will hit:
- **RoomMemberDataInternal layout.** userInfo is at +4 and memberId at +56,
  after the NP ID.
- **Signaling timing.** "Established" is deferred until the host has handled
  the join.
- **Update events for the writer.** The member who writes room internal data
  receives the update event too.
- **Distinct console identity.** Each player needs their own OpenPSID; the
  title hashes it into the key each player announces.
- **P2P sockets.** They bind all interfaces. Stream sockets connect to the
  port in sin_vport, and every stream socket shares the P2P port, so a
  connection comes from the port signaling reported. Accept reports that
  port in sin_vport, the way connect takes it: the title matches the
  connection to a room member by it and drops data from anyone else.
- **libnet errors.** A failing call returns `0x80010200 | errno` in-band.

## Why it is shaped this way

- **One thread.** Callbacks run on the title's polling thread, not on a
  Matching2 worker. psnr replies are pumped from `cellSysutilCheckCallback`,
  so every callback runs where titles already expect sysutil callbacks.
  Score's `PollAsync` pumps too, for titles that poll from a worker.
- **Responses are relocatable images.** An SDK response contains pointers into
  itself, and those only mean something once the title picks the buffer in
  `GetEventData`. Each response is built with its pointer fields recorded, then
  patched as it is copied into the title's buffer.
- **Attributes are opaque to the server.** Matching2 bin and int attributes are
  packed into blobs by the client. psnr stores and forwards bytes, and search
  filtering happens client-side on the rooms psnr lists.
