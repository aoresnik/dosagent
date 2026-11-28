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
10 seconds. Also tested on the same machine over PLIP (a parallel cable),
which is much more comfortable, and in QEMU with FreeDOS 1.4.

![DOSAGENT on MS-DOS 5.0 answering "What kind of machine are you running
on?" using a local model](docs/dosagent.png)

## What it's useful for

DOSAGENT brings to DOS what coding agents do on current systems: the model
looks around, runs programs, reads and changes files and checks the
results, while you watch and approve. Uses that have been tried:

- **Writing small command-line programs** with the compilers and
  interpreters on the machine. Tested with Bywater BASIC (`BWBASIC`, a
  FreeDOS package), Borland Turbo C 2.01 and Borland Turbo Pascal 5.5 (both
  free from the Borland software museum). The agent writes the source,
  runs the compiler, reads its error messages, fixes the code and runs the
  program to check what it prints.
- **Simple administration**, such as installing the NANSI.SYS ANSI console
  driver from its ZIP: the agent unpacked it and loaded it with `DEVLOAD`.
- **Finding out about the machine**: what's in `AUTOEXEC.BAT` and
  `CONFIG.SYS`, how much memory is free, what an old directory contains.
- **Running network programs** such as mTCP's `PING` and `HTGET` for you.

Example requests:

```
Write a Turbo Pascal program that counts the lines, words and characters
in a text file. Compile it and test it on AUTOEXEC.BAT.

Install the ANSI driver in C:\PKG\NANSI.ZIP.

What kind of machine are you running on?
```

Tips:

- Tell the agent where your compilers are and how you like to work in
  `AGENTS.TXT` (see below), so it doesn't have to search every time.
- Use command-line tools such as `TCC` and `TPC` rather than the IDEs: the
  agent can't type into a program that waits for keys.

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
Ethernet is recommended: every request resends the whole conversation, so
a slow link makes each turn wait.

Without a network card, a parallel or serial cable to another machine
works too. PLIP over a parallel cable is the better choice. SLIP over a
serial port is slow: even at 115200 baud it moves about 11 KB a second,
so long conversations take a while to send. For SLIP, run the EtherSLIP
packet driver on the DOS machine and slirp on the other end. Tested with
the updated EtherSLIP from https://github.com/mbbrutman/EtherSLIP; the
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

llama.cpp streams every token in its own packet. Over PLIP or SLIP,
`stream=0` is faster: the whole reply comes at once in full packets, but
nothing is shown until it is complete. `llama-server --reasoning off`
saves more by turning off the model's thinking, at some cost in quality.

Every request also resends the model's reasoning from earlier prompts,
which the server shows to the model again. `keep_thinking=0` drops it
when you type a new prompt, so long sessions send less and use less
memory; the reasoning for the prompt being worked on is kept.

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
`DOSAGENT what is in AUTOEXEC.BAT?` `DOSAGENT --help` (or `/?`) lists the
command-line options.

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
