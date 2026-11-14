#!/usr/bin/env python3
"""Drive the QEMU test machine through its monitor socket.

  qemuctl.py screen              print the 80x25 text screen
  qemuctl.py type 'DIR'          type text (use \\n for Enter)
  qemuctl.py wait 'C:\\WORK>' 60  wait until the text appears on screen
  qemuctl.py quit                stop QEMU
"""
import os
import socket
import sys
import time

SOCK = os.path.join(os.path.dirname(os.path.abspath(__file__)), "monitor.sock")

KEYS = {
    " ": "spc", "\n": "ret", "\\": "backslash", "/": "slash", ".": "dot",
    ",": "comma", "-": "minus", "=": "equal", ";": "semicolon",
    "'": "apostrophe", "[": "bracket_left", "]": "bracket_right",
    "`": "grave_accent", "\t": "tab", "\x1b": "esc",
}
SHIFTED = {
    ":": "semicolon", "\"": "apostrophe", "_": "minus", "+": "equal",
    "?": "slash", ">": "dot", "<": "comma", "|": "backslash", "!": "1",
    "@": "2", "#": "3", "$": "4", "%": "5", "^": "6", "&": "7", "*": "8",
    "(": "9", ")": "0", "{": "bracket_left", "}": "bracket_right",
    "~": "grave_accent",
}


def command(cmd):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(SOCK)
    s.settimeout(5)
    out = b""

    def read_prompt():
        nonlocal out
        buf = b""
        while not buf.endswith(b"(qemu) "):
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
        return buf

    read_prompt()
    s.sendall(cmd.encode() + b"\n")
    out = read_prompt()
    s.close()
    return out.decode(errors="replace")


def screen():
    # Read 4000 bytes of VGA text memory (char, attribute pairs).
    raw = command("xp /2000hx 0xb8000")
    words = []
    for line in raw.splitlines():
        if ":" not in line or not line.strip().startswith("00000000000b8"):
            continue
        for w in line.split(":", 1)[1].split():
            words.append(int(w, 16))
    chars = bytes(w & 0xFF for w in words).decode("cp437")
    rows = [chars[i:i + 80].rstrip() for i in range(0, len(chars), 80)]
    return "\n".join(rows[:25])


def type_text(text):
    for ch in text:
        if ch in KEYS:
            key = KEYS[ch]
        elif ch in SHIFTED:
            key = "shift-" + SHIFTED[ch]
        elif ch.isupper():
            key = "shift-" + ch.lower()
        else:
            key = ch
        command("sendkey " + key)
        time.sleep(0.03)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    op = sys.argv[1]
    if op == "screen":
        print(screen())
    elif op == "type":
        type_text(sys.argv[2].encode().decode("unicode_escape"))
    elif op == "wait":
        deadline = time.time() + float(sys.argv[3] if len(sys.argv) > 3 else 30)
        while time.time() < deadline:
            scr = screen()
            if sys.argv[2] in scr:
                print(scr)
                return 0
            time.sleep(1)
        print(screen())
        print("TIMEOUT waiting for %r" % sys.argv[2], file=sys.stderr)
        return 1
    elif op == "quit":
        try:
            command("quit")
        except OSError:
            pass
    else:
        print(command(" ".join(sys.argv[1:])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
