# DOSAGENT

An agentic LLM harness for MS-DOS: you type a request, Claude runs commands
and reads and edits files on the DOS machine through tools, and you approve
each action. It talks to the Anthropic Messages API directly over HTTPS:
packet driver, then Watt-32 TCP/IP, then BearSSL TLS 1.2. No proxy needed.

Target: 386 or better, 4 MB RAM, DOS 5+ (or FreeDOS), a packet driver.
Built with DJGPP, so it runs in 32-bit protected mode under CWSDPMI and
leaves almost all conventional memory free for the programs it runs.

## Layout

```
src/main.c      REPL, config, system prompt, agent loop, approvals
src/api.c       request building, SSE stream parsing, history
src/http.c      HTTP/1.1 client (keep-alive, chunked)
src/net.c       sockets (BSD on host, Watt-32 on DOS) + BearSSL TLS
src/tools.c     run_command, read_file, write_file, edit_file, list_dir,
                read_screen (DOS only)
src/json.c      small JSON DOM parser and writer
src/charset.c   CP437 <-> UTF-8
src/platform.c  DOS vs POSIX: keyboard, screen, entropy
test/           mock API server, QEMU + FreeDOS test machine
```

## Building

BearSSL and Watt-32 are git submodules in `third_party/`, pinned to the
commits this was tested with. Clone with `git clone --recursive`, or run
`git submodule update --init` in an existing clone. CWSDPMI is not in git:

```
mkdir third_party/cwsdpmi    # put CWSDPMI.EXE from csdpmi7b.zip here
```

Host test build (Linux): `make host` produces `build/host/agent`.

DOS build: `make dos` (needs a DJGPP cross-compiler such as
`i586-pc-msdosdjgpp-gcc` on `PATH`, and `nasm`). Set `DJGPP_PREFIX` if your
toolchain uses a different prefix. The result, `build/dos/`, holds
`AGENT.EXE` and `CWSDPMI.EXE`, ready to copy.

The Makefile compiles Watt-32 from its sources, so its `configur.sh` is not
needed. That script runs prebuilt helper programs from the Watt-32
repository. Two files Watt-32 normally generates by running a DOS program
are kept in `dos/watt32/`. They were produced by building
`util/errnos.c` with DJGPP 2.05 and running it under FreeDOS. Regenerate
them if you change the DJGPP version.

## Installing on DOS

Copy these files to a directory such as `C:\AGENT`:

- `AGENT.EXE`
- `CWSDPMI.EXE` (from csdpmi7b.zip), here or on the `PATH`
- `CACERT.PEM`: a root CA bundle, for example curl's cacert.pem
- `AGENT.CFG`: start from `AGENT.CFG.example` and set your API key

You also need:

- A packet driver, for example `NE2000.COM 0x60 <irq> <io>`.
- `WATTCP.CFG` with `my_ip = dhcp`, or static settings. Point the
  `WATTCP.CFG` environment variable at its directory.

Then run `AGENT`. You can also give the first request on the command line:
`AGENT what is in AUTOEXEC.BAT?`

Keys and commands:

- Esc or Ctrl-C interrupts the model.
- At an approval prompt: Y = yes, N = no (you can type a reason), A = stop
  asking.
- Commands: `/help`, `/clear`, `/confirm`, `/model NAME`,
  `/effort LEVEL`, `/quit`.

## Local models (llama.cpp)

`llama-server` from llama.cpp serves the same Messages API at
`/v1/messages`, so AGENT can use a model on another machine on your LAN.
Plain HTTP avoids the slow TLS handshake, and no `CACERT.PEM` is needed:

```
host=192.168.0.101
port=8080
tls=0
model=local
api_key=none
max_tokens=4096
```

`model` and `api_key` are required but not checked, unless the server runs
with `--api-key`. llama.cpp ignores the Claude-only request fields
(adaptive thinking, effort, fallbacks, caching), so those settings can stay
as they are. The model's chat template must support tool calls. Tested with Qwen3.8-27B: tool calls, command approval
and multi-turn sessions work from the FreeDOS VM.

## Testing

`test/mock_server.py` is a scripted stand-in for the API. It speaks plain
HTTP, so set `tls=0` and point `host`/`port` at it. It checks that the
harness echoes thinking signatures and tool results correctly.

`test/qemu/` sets up a FreeDOS 1.4 machine: 486, 4 MB RAM, NE2000 on QEMU
user networking.

1. Download into `test/qemu/dl/`:
   - FD14-FloppyEdition.zip
   - pktd11.zip (Crynwr)
   - csdpmi7b.zip
   - an mTCP zip
2. Optionally put an `AGENT.CFG` in `test/qemu/`.
3. Run `./mkimages.sh`, then `./run.sh`.

From inside the guest, the host is 10.0.2.2, so the mock server works there
too. For scripted runs, use `run.sh -headless` with `qemuctl.py`
(`screen`, `type`, `wait`).

## Design notes and limitations

**Security**
- The API key sits in plain text in AGENT.CFG.
- There is no sandbox. The approval prompt is the only safety net, which
  is why it is on by default.

**TLS**
- BearSSL, TLS 1.2 only. Session resumption avoids a full key exchange on
  reconnects.
- Certificate dates are checked against the DOS clock, which is taken as
  UTC unless `TZ` is set (e.g. `SET TZ=CET-1`). If the clock is wrong, the
  error shows the certificate's dates; fix the clock or use `tls_date`.
- `TZ` defaults to `UTC0` because, with `TZ` unset, DJGPP's time zone code
  reads uninitialized memory. `time()` was then decades off whenever the
  same program had just run.
- DOS has no OS random source. The generator is seeded from keystroke
  timing, the PIT and the BIOS tick counter, plus `RANDSEED.BIN`, which
  carries entropy from one run to the next. This is adequate for a
  curiosity, but don't treat it as strong.

**History**
- History is append-only and blocks are echoed back exactly as received.
  This keeps thinking-block signatures valid and makes prompt caching
  effective.
- The whole conversation is resent each turn. Use `/clear` when it gets
  long; there is no compaction yet.

**Running commands**
- `run_command` captures stdout and stderr through handle redirection, and
  feeds stdin from NUL.
- Programs that write directly to video memory need `capture_screen`.
- There is no timeout. A program waiting for keyboard input hangs the
  session until you press keys (it is DOS, after all).
- `cd` and drive changes are handled inside the harness, so they persist
  between commands.

**Other limits**
- Tool inputs are buffered by the API rather than streamed (no
  `eager_input_streaming`). A truncated input would be invalid JSON, which
  cannot safely be echoed back in the history.
