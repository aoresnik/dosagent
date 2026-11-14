#!/usr/bin/env python3
"""Scripted stand-in for the Messages API, for testing the harness without a
key. Speaks plain HTTP/1.1 with chunked SSE responses and keep-alive.

Turn 1: thinking block + text + a list_dir tool call + a run_command call.
Turn 2: checks that the history echoes the thinking signature and both
        tool results, then writes a file with write_file.
Turn 3: ends the turn with text containing non-ASCII characters.
Every request body is appended to requests.jsonl for inspection.
"""
import json
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer


def sse(event):
    return f"event: {event['type']}\ndata: {json.dumps(event)}\n\n".encode()


def message(blocks, stop_reason):
    out = [sse({"type": "message_start", "message": {
        "id": "msg_mock", "type": "message", "role": "assistant",
        "model": "mock-model", "content": [], "stop_reason": None,
        "usage": {"input_tokens": 100, "output_tokens": 1,
                  "cache_read_input_tokens": 50}}})]
    out.append(sse({"type": "ping"}))
    for i, b in enumerate(blocks):
        kind = b["type"]
        if kind == "thinking":
            out.append(sse({"type": "content_block_start", "index": i,
                            "content_block": {"type": "thinking",
                                              "thinking": "", "signature": ""}}))
            out.append(sse({"type": "content_block_delta", "index": i,
                            "delta": {"type": "thinking_delta",
                                      "thinking": b["thinking"]}}))
            out.append(sse({"type": "content_block_delta", "index": i,
                            "delta": {"type": "signature_delta",
                                      "signature": b["signature"]}}))
        elif kind == "text":
            out.append(sse({"type": "content_block_start", "index": i,
                            "content_block": {"type": "text", "text": ""}}))
            words = b["text"].split(" ")
            for j, w in enumerate(words):
                piece = w if j == 0 else " " + w
                out.append(sse({"type": "content_block_delta", "index": i,
                                "delta": {"type": "text_delta", "text": piece}}))
        elif kind == "tool_use":
            out.append(sse({"type": "content_block_start", "index": i,
                            "content_block": {"type": "tool_use", "id": b["id"],
                                              "name": b["name"], "input": {}}}))
            js = json.dumps(b["input"])
            for k in range(0, len(js), 7):
                out.append(sse({"type": "content_block_delta", "index": i,
                                "delta": {"type": "input_json_delta",
                                          "partial_json": js[k:k + 7]}}))
        out.append(sse({"type": "content_block_stop", "index": i}))
    out.append(sse({"type": "message_delta",
                    "delta": {"stop_reason": stop_reason},
                    "usage": {"output_tokens": 42}}))
    out.append(sse({"type": "message_stop"}))
    return out


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    connections = set()

    def log_message(self, fmt, *args):
        sys.stderr.write("mock: " + (fmt % args) + "\n")

    def do_POST(self):
        Handler.connections.add(id(self.connection))
        body = self.rfile.read(int(self.headers["Content-Length"]))
        req = json.loads(body)
        with open("requests.jsonl", "a") as f:
            f.write(json.dumps({"headers": dict(self.headers), "body": req,
                                "conn": id(self.connection)}) + "\n")
        msgs = req["messages"]
        problems = []
        n_assistant = sum(1 for m in msgs if m["role"] == "assistant")

        if n_assistant == 0:
            events = message([
                {"type": "thinking", "thinking": "Plan: look around.",
                 "signature": "SIG-abc123=="},
                {"type": "text", "text": "Let me look at the directory."},
                {"type": "tool_use", "id": "toolu_1", "name": "list_dir",
                 "input": {"path": "."}},
                {"type": "tool_use", "id": "toolu_2", "name": "run_command",
                 "input": {"command": "echo hello from \"shell\""}},
            ], "tool_use")
        elif n_assistant == 1:
            first = msgs[1]["content"]
            if first[0] != {"type": "thinking", "thinking": "Plan: look around.",
                            "signature": "SIG-abc123=="}:
                problems.append(f"thinking block not echoed exactly: {first[0]}")
            results = msgs[2]["content"]
            ids = [r.get("tool_use_id") for r in results]
            if ids != ["toolu_1", "toolu_2"]:
                problems.append(f"tool results wrong: {ids}")
            if "hello from" not in results[1]["content"]:
                problems.append("command output missing")
            events = message([
                {"type": "text", "text": "Writing a file now."},
                {"type": "tool_use", "id": "toolu_3", "name": "write_file",
                 "input": {"path": "NOTE.TXT",
                           "content": "line one\nline two é─\n"}},
            ], "tool_use")
        else:
            events = message([
                {"type": "text",
                 "text": "Done — café ✓. (Problems, if any, are "
                 "logged by the mock server on stderr.)"},
            ], "end_turn")

        if problems:
            sys.stderr.write("mock PROBLEMS: %s\n" % problems)
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        for ev in events:
            self.wfile.write(b"%x\r\n%s\r\n" % (len(ev), ev))
        self.wfile.write(b"0\r\n\r\n")
        self.wfile.flush()


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8089
    HTTPServer(("127.0.0.1", port), Handler).serve_forever()
