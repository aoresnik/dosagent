# Changelog

Notable changes to DOSAGENT, in the format of
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versions are the
release tags without the leading `v` (see DEVELOPMENT.md, Releases).

## [Unreleased]

### Added
- `stream=0` in DOSAGENT.CFG gets each reply whole instead of streamed.
  This sends far fewer packets, which helps on PLIP or SLIP with a local
  model, but nothing is shown until the reply is complete.

### Changed
- DOSAGENT.EXE is about 200 KB smaller and needs that much less memory:
  Watt-32's IDNA support (iconv and charset tables, for host names with
  national characters) is no longer built in.

## [0.1beta2] - 2026-11-28

### Added
- `DOSAGENT --help` (also `-h` and `/?`) prints command-line usage.

### Changed
- On DOS, a command chained with `&&` or `||` is no longer run (COMMAND.COM
  would pass the operators to the first program as arguments). The model is
  told why and to run each command on its own or use a batch file. The
  run_command description now states this too, since smaller models
  overlooked it in the system prompt.
- README.md recommends Ethernet, or PLIP over a parallel cable, and notes
  that SLIP over a serial port is slow.

## [0.1beta] - 2026-11-24

### Changed
- README.MD describes what DOSAGENT is useful for, with use cases that
  have been tested.

### Fixed
- The model's instructions said the screen is 80x25 even in other text
  modes, such as 80x50, contradicting the measured size given above them.

## [0.1alpha3] - 2026-11-21

### Added
- Prompt line editing: cursor keys, Home/End, word movement, Delete,
  overwrite mode, and Up/Down history of earlier prompts.
- AGENTS.TXT: notes for the agent, read from the directory of DOSAGENT.EXE
  and from the current directory, and added to the model's instructions.

### Fixed
- Programs with their own TCP/IP stack, such as mTCP's PING and HTGET, got
  no network replies when run through the agent. DOSAGENT now releases the
  packet driver while a command runs.

## [0.1alpha2] - 2026-11-15

### Fixed
- Crash with "Exiting due to signal SIGNOFP" on a 386 or 486SX without an
  FPU. The 387 emulator is now built into DOSAGENT.EXE, so EMU387.DXE is
  not needed.

## [0.1alpha] - 2026-11-15

### Added
- First release: an agentic harness that talks to the Anthropic Messages
  API over a packet driver, Watt-32 TCP/IP and BearSSL TLS 1.2.
- Tools for running commands, reading, writing and editing files, listing
  directories and reading the text screen, each action approved by the
  user.
- Works with llama.cpp's `llama-server` over plain HTTP.
- Release archive with CWSDPMI, a CA bundle, and a WATTCP.CFG set up for
  DHCP.

[Unreleased]: https://github.com/aoresnik/dosagent/compare/v0.1beta2...HEAD
[0.1beta2]: https://github.com/aoresnik/dosagent/compare/v0.1beta...v0.1beta2
[0.1beta]: https://github.com/aoresnik/dosagent/compare/v0.1alpha3...v0.1beta
[0.1alpha3]: https://github.com/aoresnik/dosagent/compare/v0.1alpha2...v0.1alpha3
[0.1alpha2]: https://github.com/aoresnik/dosagent/compare/v0.1alpha...v0.1alpha2
[0.1alpha]: https://github.com/aoresnik/dosagent/releases/tag/v0.1alpha
