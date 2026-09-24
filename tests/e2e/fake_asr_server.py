"""Run the *current* tea-asr-service app on loopback with its TEST fake backend.

This is a test harness for the plugin, not a way to run the product:

* It must be executed with the tea-asr-service virtualenv's interpreter
  (``$TEA_ASR_SERVICE_DIR/.venv/bin/python``) so the real, current
  ``tea_asr.api.app.create_app`` -- HTTP Host allowlist, WS Origin check,
  bearer auth, rate limiter, connection admission, idle timeout -- is what the
  plugin talks to.
* Inference is the server repo's own ``tests/conftest.py::FakeSupervisor`` and
  segmentation is ``FakeVad``. No MLX model is loaded (another process may be
  using the GPU for measurements). docs/06-handoff.md constraint 1 requires a
  test fake to be opted into explicitly and labelled in status, so the fake is
  only reachable through this script and ``/v1/status.last_error`` carries a
  ``FAKE_BACKEND`` marker, and a banner goes to stderr.
* Nothing is written to the user's ``~/Library`` TEA ASR directories: the token
  file and log paths live under ``--state-dir``.

Every HTTP request / WS upgrade outcome is appended to ``--request-log`` as one
JSON line so the plugin tests can count retries and auth failures from the
server's point of view. The client's ``session.start`` options and every
``transcript.partial`` / ``transcript.stable`` / ``transcript.final`` the server
sends are logged too (fixture text only), for the stable-caption scenarios.

Stable-caption knobs (all in this harness process; the server repo is not
modified):

* ``--growing-text``: the fake inference returns a prefix of a fixed sentence
  whose length grows with the audio it was given, plus one trailing "guess"
  character that flips on every call. Consecutive partials therefore rewrite
  their last character (what makes a partial-mode subtitle jump), while the
  server's real LocalAgreement tracker commits the part they agree on.
* ``--hide-stable-capability``: strips ``features.stable_transcripts`` from
  ``/v1/capabilities`` -- an older server that has revisable previews only.
* ``--reject-stable-field``: renames ``stable`` in the client's
  ``session.start`` to an unknown key, so the current server's extra=forbid
  answers ``protocol_error`` exactly like a server that predates the field.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--service-dir", default=os.environ.get("TEA_ASR_SERVICE_DIR"))
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--state-dir", required=True)
    parser.add_argument("--request-log", required=True)
    parser.add_argument("--revisable", action="store_true")
    parser.add_argument("--max-total-connections", type=int, default=4)
    parser.add_argument("--max-continuous-sessions", type=int, default=2)
    parser.add_argument("--idle-timeout-s", type=float, default=None)
    parser.add_argument(
        "--prefail",
        type=int,
        default=0,
        help="record this many auth failures for 127.0.0.1 before serving (rate-limit scenario)",
    )
    parser.add_argument("--text", default="測試文字")
    parser.add_argument("--growing-text", action="store_true")
    parser.add_argument("--hide-stable-capability", action="store_true")
    parser.add_argument("--reject-stable-field", action="store_true")
    args = parser.parse_args()

    if args.port == 8327:
        print("refusing to use 8327: that is the real service's port", file=sys.stderr)
        return 2
    if not args.service_dir:
        print("--service-dir or TEA_ASR_SERVICE_DIR is required", file=sys.stderr)
        return 2
    service_dir = Path(args.service_dir).resolve()
    sys.path.insert(0, str(service_dir))  # for `tests.conftest`
    sys.path.insert(0, str(service_dir / "src"))

    import uvicorn

    import tea_asr.api.stream as stream_module
    from tea_asr.api.app import create_app
    from tea_asr.config import AppPaths, ServiceConfig, TokenAuthenticator
    from tea_asr.rate_limit import AuthRateLimiter
    from tests.conftest import FakeSupervisor, FakeVad

    if args.idle_timeout_s is not None:
        # Module-level constant read at call time by StreamSession; patched in
        # this harness process only (the server repo is not modified).
        stream_module.IDLE_TIMEOUT_S = args.idle_timeout_s

    state_dir = Path(args.state_dir)
    state_dir.mkdir(parents=True, exist_ok=True)
    paths = AppPaths(support=state_dir, logs=state_dir / "logs")
    paths.logs.mkdir(parents=True, exist_ok=True)

    class GrowingSupervisor(FakeSupervisor):
        #: 0.2 s of audio per character; mixes 3-byte CJK and a 4-byte emoji.
        SENTENCE = "今天天氣很好我們一起去公園散步吧🍵然後喝杯茶再回家休息一下就好了謝謝大家"
        GUESSES = "嗯啊"

        async def transcribe(self, pcm: bytes, *, language: str = "Chinese"):
            result = await super().transcribe(pcm, language=language)
            chars = max(1, min(len(self.SENTENCE), (len(pcm) // 2) // 3200))
            result["text"] = self.SENTENCE[:chars] + self.GUESSES[self.calls % 2]
            return result

    supervisor = (GrowingSupervisor if args.growing_text else FakeSupervisor)(text=args.text)
    supervisor.last_error = "FAKE_BACKEND: tests/conftest.py FakeSupervisor, no ASR model loaded"

    limiter = AuthRateLimiter()
    for _ in range(args.prefail):
        limiter.record_failure("127.0.0.1")

    config = ServiceConfig(
        revisable_preview=args.revisable,
        max_total_connections=args.max_total_connections,
        max_continuous_sessions=args.max_continuous_sessions,
        idle_unload_s=0,
    )
    app = create_app(
        Path("unused-fake-backend"),
        supervisor=supervisor,
        config=config,
        vad_model=FakeVad(),
        token_authenticator=TokenAuthenticator(paths),
        rate_limiter=limiter,
        paths=paths,
    )

    log_path = Path(args.request_log)
    start = time.monotonic()

    def record(entry: dict) -> None:
        entry["t"] = round(time.monotonic() - start, 3)
        with log_path.open("a", encoding="utf-8") as fh:
            fh.write(json.dumps(entry, ensure_ascii=False) + "\n")

    async def logged(scope, receive, send):  # pure ASGI wrapper
        if scope["type"] not in ("http", "websocket"):
            await app(scope, receive, send)
            return
        headers = {k.decode().lower(): v.decode(errors="replace") for k, v in scope.get("headers", [])}
        base = {
            "kind": scope["type"],
            "path": scope.get("path"),
            "host": headers.get("host"),
            "origin": headers.get("origin"),
            "has_auth": "authorization" in headers,
        }
        outcome: dict = {}
        held: dict = {}

        async def wrapped_receive():
            message = await receive()
            if message.get("type") == "websocket.receive" and message.get("text"):
                try:
                    payload = json.loads(message["text"])
                except ValueError:
                    payload = {}
                if payload.get("type") == "session.start":
                    record({**base, "event": "ws_session_start",
                            "transcript_mode": payload.get("transcript_mode"),
                            "stable": payload.get("stable"),
                            "stable_rewritten": bool(args.reject_stable_field and "stable" in payload)})
                    if args.reject_stable_field and "stable" in payload:
                        payload["stable_unknown_to_this_server"] = payload.pop("stable")
                        message = {**message, "text": json.dumps(payload, ensure_ascii=False)}
            return message

        async def wrapped_send(message):
            mtype = message["type"]
            if (args.hide_stable_capability and base["path"] == "/v1/capabilities"
                    and mtype in ("http.response.start", "http.response.body")):
                # Buffer the whole response, drop the optional feature, fix
                # content-length, then pass it on.
                if mtype == "http.response.start":
                    held["start"] = message
                    held["body"] = b""
                    return
                held["body"] += message.get("body", b"")
                if message.get("more_body"):
                    return
                body = held["body"]
                try:
                    doc = json.loads(body)
                    doc.get("features", {}).pop("stable_transcripts", None)
                    body = json.dumps(doc, ensure_ascii=False).encode()
                except ValueError:
                    pass
                start_msg = held["start"]
                headers = [(k, v) for k, v in start_msg.get("headers", []) if k.lower() != b"content-length"]
                headers.append((b"content-length", str(len(body)).encode()))
                outcome["status"] = start_msg["status"]
                await send({**start_msg, "headers": headers})
                await send({"type": "http.response.body", "body": body})
                return
            if mtype == "http.response.start":
                outcome["status"] = message["status"]
            elif mtype == "websocket.accept" and "result" not in outcome:
                outcome["result"] = "accepted"
                record({**base, "event": "ws_accept"})
            elif mtype == "websocket.close":
                if "result" not in outcome:
                    # Closed before accept: uvicorn turns this into HTTP 403.
                    outcome["result"] = "rejected"
                    record({**base, "event": "ws_reject", "close_code": message.get("code"),
                            "reason": message.get("reason")})
                else:
                    record({**base, "event": "ws_close", "close_code": message.get("code")})
            elif mtype == "websocket.send" and message.get("text"):
                try:
                    payload = json.loads(message["text"])
                except ValueError:
                    payload = {}
                if payload.get("type") == "error":
                    record({**base, "event": "ws_error_event", "code": payload.get("code"),
                            "retryable": payload.get("retryable")})
                elif payload.get("type") in ("transcript.partial", "transcript.final"):
                    record({**base, "event": "ws_" + payload["type"].split(".")[1],
                            "segment_id": payload.get("segment_id"), "text": payload.get("text")})
                elif payload.get("type") == "transcript.stable":
                    record({**base, "event": "ws_stable", "segment_id": payload.get("segment_id"),
                            "segment_index": payload.get("segment_index"),
                            "stable_revision": payload.get("stable_revision"),
                            "state": payload.get("state"), "text": payload.get("text")})
            await send(message)

        try:
            await app(scope, wrapped_receive if scope["type"] == "websocket" else receive, wrapped_send)
        finally:
            if scope["type"] == "http":
                record({**base, "event": "http", "status": outcome.get("status")})

    print(
        f"*** FAKE BACKEND (tests/conftest.py FakeSupervisor + FakeVad) on 127.0.0.1:{args.port} "
        f"-- NOT a real ASR model ***",
        file=sys.stderr,
        flush=True,
    )
    uvicorn.run(
        logged,
        host="127.0.0.1",
        port=args.port,
        log_level="warning",
        # Same heartbeat as tea_asr.cli (docs/04 "WS 心跳").
        ws_ping_interval=15,
        ws_ping_timeout=30,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
