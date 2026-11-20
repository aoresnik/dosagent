# Changelog

Notable changes to DOSAGENT, in the format of
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versions are the
release tags without the leading `v` (see DEVELOPMENT.md, Releases).

## [Unreleased]

### Added
- Prompt line editing: cursor keys, Home/End, word movement, Delete,
  overwrite mode, and Up/Down history of earlier prompts.

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

[Unreleased]: https://github.com/aoresnik/dosagent/compare/v0.1alpha2...HEAD
[0.1alpha2]: https://github.com/aoresnik/dosagent/compare/v0.1alpha...v0.1alpha2
[0.1alpha]: https://github.com/aoresnik/dosagent/releases/tag/v0.1alpha
