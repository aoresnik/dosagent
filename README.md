# DOSAGENT

An agentic LLM harness for MS-DOS: you type a request, Claude (or a local
model server) runs commands and reads and edits files on the DOS machine
through tools, and you approve each action. It talks to the Anthropic
Messages API directly over HTTP(S): packet driver, then Watt-32 TCP/IP,
then BearSSL TLS 1.2. No proxy needed.

Target: 386 or better, 4 MB RAM, DOS 5+ (or FreeDOS), a packet driver.
Built with DJGPP, so it runs in 32-bit protected mode under CWSDPMI and
leaves almost all conventional memory free for the programs it runs.

Tested on a real 386 with 4 MB RAM and MS-DOS 5.0, connected through
EtherSLIP over a serial cable to slirp on a Linux machine, using a
llama.cpp server without TLS: the first reply to a short prompt took about
10 seconds. Also tested in QEMU with FreeDOS 1.4.

![DOSAGENT on MS-DOS 5.0 answering "What kind of machine are you running
on?" using a local model](docs/dosagent.png)

**WARNING: NOT INTENDED TO BE USED AS A PRODUCTION SOLUTION, JUST AS A
CURIOSITY**

Risks:
- Not suitable for serious software development. DOS doesn't have robust
  multitasking and memory protections, so the bugs in the program under
  development may hang the machine or corrupt memory and disk.
- DOS always runs in administrator mode and doesn't have sandboxing support.
  Check any command before letting the agent run it.
- The agent may contain bugs.

Binary releases: https://github.com/aoresnik/dosagent/releases
Building from source and how it works: see
[DEVELOPMENT.md](https://github.com/aoresnik/dosagent/blob/main/DEVELOPMENT.md).

## Installing

Unzip the release archive into a directory such as `C:\DOSAGENT`. It
contains:

- `DOSAGENT.EXE`: the program
- `CWSDPMI.EXE`: the DPMI server DOSAGENT needs (keep it next to
  `DOSAGENT.EXE` or on the `PATH`), with its documentation `CWSDPMI.DOC`
- `DOSAGENT.CFG`: settings; you need to edit this
- `WATTCP.CFG`: network settings, set up for DHCP
- `CACERT.PEM`: trusted root certificates for HTTPS
- `LICENSE.TXT`: the license (Apache 2.0)
- `DOSAGENT.PNG`: the screenshot shown in this README
- `NOTICES.TXT`: third-party copyright notices

You also need a packet driver for your network card, loaded before
DOSAGENT runs, for example `NE2000 0x60 <irq> <io>` in `AUTOEXEC.BAT`.
Without a network card, a serial cable works too: run the EtherSLIP packet
driver on the DOS machine and slirp on the other end. Tested with the
updated EtherSLIP from https://github.com/mbbrutman/EtherSLIP; the
original Crynwr version has not been tried.

If your network has no DHCP server, put a static address in `WATTCP.CFG`.
DOSAGENT finds `WATTCP.CFG` next to `DOSAGENT.EXE`, or in the directory
named by the `WATTCP.CFG` environment variable.

## Configuring

Open `DOSAGENT.CFG` in a text editor (`EDIT DOSAGENT.CFG`). Each setting
is explained in the file. DOSAGENT reads `DOSAGENT.CFG` from the current
directory, or else from the directory of `DOSAGENT.EXE`; `-c FILE` picks a
different one.

### Claude

Set `api_key` to an Anthropic API key. Keys are created in the Claude
Console (https://console.anthropic.com) and billed separately from Claude
Pro/Max subscriptions. You can also leave the key out of the file and
`SET ANTHROPIC_API_KEY=...` before running DOSAGENT.

HTTPS needs the PC's clock to be roughly right, because certificate dates
are checked against it. The clock is taken as UTC unless `TZ` is set (for
example `SET TZ=CET-1`). If the clock can't be fixed, set `tls_date` in
`DOSAGENT.CFG` to today's date.

### Local models (llama.cpp)

`llama-server` from llama.cpp serves the same Messages API at
`/v1/messages`, so DOSAGENT can use a model on another machine on your
LAN. Plain HTTP avoids the slow TLS handshake, and no `CACERT.PEM` is
needed:

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
as they are. The model's chat template must support tool calls. Tested
with Qwen3.8-27B: tool calls, command approval and multi-turn sessions
work.

### Notes for the agent (AGENTS.TXT)

Put instructions and facts the agent should always know into a plain text
file called `AGENTS.TXT`, for example where your compiler is, how to build
and test your project, or what not to touch. DOSAGENT reads `AGENTS.TXT`
from the directory of `DOSAGENT.EXE` (notes for the whole machine) and then
from the current directory (notes for one project), and adds both to the
model's instructions. Where they conflict, the current directory's file
wins. Each file can be up to 32 KB. The files are read at start-up and
again on `/clear`.

## Using

Run `DOSAGENT`. You can also give the first request on the command line:
`DOSAGENT what is in AUTOEXEC.BAT?`

Keys and commands:

- Esc or Ctrl-C interrupts the model.
- Editing a prompt: Left/Right, Home/End, Ctrl-Left/Right (by word), Del,
  Backspace, Ctrl-Backspace (word), Ctrl-K (to the end), Ins (overwrite),
  Esc (clear). Up/Down recall earlier prompts. End a line with `\` to
  continue the prompt on the next line.
- At an approval prompt: Y = yes, N = no (you can type a reason), A = stop
  asking.
- Commands: `/help`, `/clear`, `/confirm`, `/model NAME`,
  `/effort LEVEL`, `/quit`.

Things to know:

- The API key sits in plain text in `DOSAGENT.CFG`.
- The approval prompt is the only safety net, which is why it is on by
  default. `/confirm` or answering A turns it off for the session.
- There is no timeout on commands. A program that waits for keyboard input
  hangs the session until you press keys.
- Programs that write directly to video memory are run with a screen
  capture instead of output redirection; the model asks for it.
- The whole conversation is resent every turn. Use `/clear` when it gets
  long.
- On a real 386, the first HTTPS connection takes a while; later requests
  reuse the session.

## License

DOSAGENT is licensed under the Apache License 2.0; see `LICENSE` (or
`LICENSE.TXT` in the release archive). The release archive also contains
third-party software under its own terms; see `NOTICES.TXT`.
