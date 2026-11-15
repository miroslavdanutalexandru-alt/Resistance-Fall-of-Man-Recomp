# Multiplayer (client side)

The server bundle (lobby, SVO, ticket login, DME game server) lives in its own
repo: Resistance-Fall-of-Man-Recomp-Server. The public server is
`r1.rfom.online` (net_hosts.txt maps the Sony hostnames to it); for a server
on this PC map them to 127.0.0.1 instead.

Here:

- `trace_run.bat` — run the game online, windowed, with network tracing
  (logs\net_trace.log, logs\mp_game.log).
- `run_player2.bat` — a second copy on the same PC as another player.
- `find_string_refs.py`, `ser_dump.py` — find the lifted code that uses a
  string / print a Medius message layout from its (de)serializer.

`r1_root.der` is the public server's TLS root (a certificate, no private
key); the game patches it in on every start (project/main.cpp). For your own
server, put its `data\svo_root.der` at `logs\mp\svo_root.der`: trace_run.bat
patches that one in instead.
