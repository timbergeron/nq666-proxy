# nq666-proxy

[![CI](https://github.com/timbergeron/nq666-proxy/actions/workflows/ci.yml/badge.svg)](https://github.com/timbergeron/nq666-proxy/actions/workflows/ci.yml)

`nq666-proxy` is a small, stateful UDP gateway for Linux, macOS, and Windows
that lets original WinQuake and legacy ProQuake clients join a QSS-M server
whose gameplay protocol is FitzQuake 666.

It is not a port forwarder. The program terminates NetQuake's reliable UDP
channel independently on each side and translates the gameplay payloads:

- protocol 666 server info becomes protocol 15;
- 666 entity, baseline, client-data, and sound extensions are reduced to their
  protocol 15 forms;
- WinQuake's 8-bit client movement angles are expanded to the 16-bit angles a
  protocol 666 server expects;
- ProQuake's negotiated 16-bit angle mode is preserved;
- reliable messages are reassembled and refragmented for the legacy 1024-byte
  datagram and 8192-byte message limits;
- server-browser control queries are relayed, with optional address rewriting.

## Build and test

The only build dependency is a C11 compiler and the platform socket headers.
On Linux and macOS:

```sh
make
make check
```

Additional Linux checks are available with GCC:

```sh
make analyze
make sanitize
```

`make analyze` uses GCC's static analyzer; the normal build accepts any C11
compiler supported on Linux or macOS. CI also publishes a universal macOS
artifact containing Apple Silicon and Intel slices.

On Windows, build in an MSYS2 UCRT64 shell with its GCC and Make packages
installed:

```sh
make
make check
```

The resulting executable is `nq666-proxy.exe`. CI publishes a 64-bit Windows
artifact and runs both the unit and UDP process integration suites.

The unit suite includes deterministic malformed-packet fuzzing and covers
server-info downgrade, extended entity and baseline
conversion, model/entity reference filtering, client angle expansion,
unreliable message splitting, and the reliable UDP fragment/ACK sequence. The
integration target starts the real proxy process between a fake protocol-15
client and protocol-666 server and checks query rewriting, handshake rejection,
sanitized `pext` negotiation, ACKs, and translated sign-on.

## Run

Assume QSS-M listens privately on UDP 26000 and the legacy-facing proxy should
listen on UDP 26001:

```sh
./nq666-proxy \
  --server 127.0.0.1:26000 \
  --listen 0.0.0.0:26001 \
  --advertise 203.0.113.10:26001 \
  --verbose
```

Legacy clients then use:

```text
connect 203.0.113.10:26001
```

Open the proxy listener as UDP, not TCP, in the VPS firewall. Keep QSS-M's
upstream port private if modern clients do not need to connect to it directly.
If both modern and legacy players should join, expose both ports: modern
clients use QSS-M's port and old clients use the proxy port.

`--advertise` is needed for connects initiated from WinQuake's server browser.
It rewrites the address in `CCREP_SERVER_INFO`. Direct `connect host:port`
works without it. When `--listen` names a specific IP instead of `0.0.0.0`,
that listener address is advertised automatically. If `--advertise` omits a
port, the bound listener port is appended automatically.

Run `./nq666-proxy --help` for all options.

## systemd installation

```sh
sudo make install
sudo install -m 0644 \
  /usr/local/share/doc/nq666-proxy/nq666-proxy.default \
  /etc/default/nq666-proxy
sudo editor /etc/default/nq666-proxy
sudo systemctl daemon-reload
sudo systemctl enable --now nq666-proxy
sudo systemctl status nq666-proxy
```

Set `NQ666_ARGS` in `/etc/default/nq666-proxy`; add
`--advertise PUBLIC_IP:26001` if players will use the server browser. The unit
runs under a dynamic unprivileged account with filesystem, capability, and
kernel hardening enabled. Re-running `make install` does not overwrite the
active `/etc/default/nq666-proxy` configuration.

## QSS-M configuration

The upstream gameplay protocol must be 666. Either of these server settings is
appropriate:

```text
sv_protocol Base-666
```

or:

```text
sv_protocol FTE+666
```

With `FTE+666`, QSS-M first asks each connecting peer to report extensions.
An original client reports none, through the proxy, so that individual
upstream connection remains on the Base-666 wire format that the translator
supports. The proxy also sanitizes the response to this negotiation, ensuring
that a newer client cannot accidentally enable an extension format on the
translated connection.

If every player can accept protocol 15, QSS-M can instead be run with
`sv_protocol Base-15` and no proxy is necessary. The proxy is for keeping a
666 service while adding a separate legacy entrance.

## Compatibility limits

Protocol translation cannot give a 1996 executable larger internal tables.
The proxy therefore applies conservative client limits:

| Resource | WinQuake-facing limit | Behavior above the limit |
|---|---:|---|
| Scoreboard slots | 16 | Extra scoreboard updates are hidden |
| Models | 255 precaches | Higher model indices render as no model |
| Sounds | 255 precaches | Higher sound events are omitted |
| Dynamic entities | 600 WinQuake / 2048 ProQuake | Higher entities are omitted |
| Static entities | 128 | Additional statics are omitted |
| Light styles | 64 | Higher style updates are omitted |

This is intended for standard Quake and similarly sized multiplayer maps. A
map or mod that fundamentally requires FitzQuake limits may connect but will
have missing objects, sounds, or effects in the old client. FTE replacement
deltas, CSQC, downloads, voice, RMQ protocol 999, and DarkPlaces protocols are
deliberately outside this proxy's scope.

The proxy supports IPv4, matching the original clients. Each legacy client is
given a separate upstream UDP socket and protocol state. Idle sessions expire
after five minutes.

## Operational notes

- Put the proxy close to QSS-M; ideally use `127.0.0.1` for the upstream.
- Start with `--verbose` and inspect translation errors before daemonizing it.
- Translation failures are reported to the affected client before its session
  is closed; they are also logged with the client's address.
- Administrative RCON packets are deliberately not relayed. Manage QSS-M on
  its private listener or through a separate secured channel.
- QSS-M sees the VPS address plus a distinct UDP source port for each proxied
  player. IP-based moderation at QSS-M cannot recover the original player IP.
- The program is a compatibility gateway, not a DDoS filter. Apply normal VPS
  UDP filtering and rate limits in front of it.

## License

This project is licensed under the GNU General Public License, version 2 or
(at your option) any later version. See [LICENSE](LICENSE).
