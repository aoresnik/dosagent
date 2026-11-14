# DOSAGENT development

How to build DOSAGENT, make release archives, and test it, plus notes on
how it works. For installing and using it, see [README.md](README.md).

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
dos/watt32/     generated Watt-32 errno files (see Building)
dist/           files that only go into the release archive
tools/          version.sh: version and archive name from the git tag
test/           mock API server, QEMU + FreeDOS test machine
.github/        GitHub Actions workflow that builds releases
```

## Building

BearSSL and Watt-32 are git submodules in `third_party/`, pinned to the
commits this was tested with. Clone with `git clone --recursive`, or run
`git submodule update --init` in an existing clone. CWSDPMI is not in git:
the DOS build downloads `csdpmi7b.zip` into `third_party/cwsdpmi/` on first
use and checks its SHA-256.

Host test build (Linux): `make host` produces `build/host/agent`.

DOS build: `make dos` produces `build/dos/DOSAGENT.EXE` and `CWSDPMI.EXE`.
It needs `nasm`, `curl`, `unzip` and a DJGPP cross-compiler such as
`i586-pc-msdosdjgpp-gcc` on `PATH`; set `DJGPP_PREFIX` if your toolchain
uses a different prefix. The releases are built with GCC 12.2 from
[build-djgpp](https://github.com/andrewwutw/build-djgpp) v3.4. Its prebuilt
Linux binaries need the flex runtime library (`libfl2` on Debian/Ubuntu);
the other packages listed there are only for building DJGPP itself. Only its
`bin/` directory needs to be on `PATH`: its `setenv` script also exports
`GCC_EXEC_PREFIX` and puts DJGPP's unprefixed `gcc` first on `PATH`, which
breaks the host compiler.

The Makefile compiles Watt-32 from its sources, so its `configur.sh` is not
needed. That script runs prebuilt helper programs from the Watt-32
repository. Two files Watt-32 normally generates by running a DOS program
are kept in `dos/watt32/`. They were produced by building
`util/errnos.c` with DJGPP 2.05 and running it under FreeDOS. Regenerate
them if you change the DJGPP version.

## Releases

`make dist` builds the release archive in `build/dist/`, so you can check
it before publishing. It contains `DOSAGENT.EXE`, `CWSDPMI.EXE` and
`CWSDPMI.DOC`, `README.md` as `README.MD`, `LICENSE` as `LICENSE.TXT`,
`DOSAGENT.CFG.example` as `DOSAGENT.CFG`, `dist/WATTCP.CFG`, a CA bundle as
`CACERT.PEM` (from `CACERT`, default `/etc/ssl/certs/ca-certificates.crt`)
and `NOTICES.TXT` (`dist/NOTICES.TXT` plus the Watt-32 and BearSSL license
texts). Text files get CRLF line endings.

The version comes from the git tag on HEAD (`tools/version.sh`):

| Tag on HEAD | Version shown | Archive        |
|-------------|---------------|----------------|
| `v0.1a`     | 0.1a          | `DAGNT01A.ZIP` |
| `v1.0b`     | 1.0b          | `DAGNT10B.ZIP` |
| `v1.0`      | 1.0           | `DAGNT10.ZIP`  |
| none        | dev-<commit>  | `DAGNT.ZIP`    |

Major and minor versions are one digit each, so the name fits DOS's 8.3
limit; the qualifier is an optional lowercase letter (a = alpha,
b = beta). Other `v*` tags on HEAD make the build fail.
`make dist TAG=v0.1a` builds exactly what that tag would, without tagging.

GitHub Actions (`.github/workflows/build.yml`) runs `make dist` on every
push and pull request and keeps the archive as a workflow artifact. Pushing
a `v*` tag also creates a GitHub release with the archive attached, marked
as a pre-release when the version has a qualifier:

```
git tag v0.1a
git push origin v0.1a
```

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
2. Optionally put config files (`*.CFG`) in `test/qemu/`. They are copied
   to `C:\AGENT`; run `C:\AGENT\DOSAGENT -c C:\AGENT\NAME.CFG` for one
   other than `DOSAGENT.CFG`.
3. Run `./mkimages.sh`, then `./run.sh`.

From inside the guest, the host is 10.0.2.2, so the mock server works there
too. For scripted runs, use `run.sh -headless` with `qemuctl.py`
(`screen`, `type`, `wait`).

## Design notes and limitations

**TLS**
- BearSSL, TLS 1.2 only. Session resumption avoids a full key exchange on
  reconnects.
- Certificate dates are checked against the DOS clock. If the check fails,
  the error shows the certificate's dates and the connection is not
  retried.
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
- The whole conversation is resent each turn. There is no compaction yet.

**Running commands**
- `run_command` captures stdout and stderr through handle redirection, and
  feeds stdin from NUL. With `capture_screen`, it reads the text screen
  instead, for programs that write to video memory.
- `cd` and drive changes are handled inside the harness, so they persist
  between commands.

**Other limits**
- Tool inputs are buffered by the API rather than streamed (no
  `eager_input_streaming`). A truncated input would be invalid JSON, which
  cannot safely be echoed back in the history.
