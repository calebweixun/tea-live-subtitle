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

* ``--deaf``: the server never reports speech: ``speech.started``,
  ``segment.queued`` and every transcript / segment result are dropped on
  the way to the client (a server whose VAD hears no speech in the audio),
  while ``audio.ack`` and ``flow.control`` keep flowing.

Sentence-break (segmentation) knobs. ``session.start.segmentation`` and
``features.segmentation_control`` are being added to the server (docs/04
「切段控制」); until a server with them is in ``--service-dir`` these emulate
the contract around the unchanged server app:

* ``--hide-segmentation-capability``: strips ``features.segmentation_control``
  from ``/v1/capabilities`` -- a server without the feature (the default of
  every server that predates it), whatever ``--service-dir`` contains.
* ``--emulate-segmentation``: advertises
  ``segmentation_control.end_silence_ms = {min 300, max 3000, default 900}``,
  removes ``segmentation`` from the client's ``session.start`` before the
  server app sees it, and reports the requested value back in
  ``session.started.preview_policy.endpoint_silence_ms``. Only the wire
  contract is emulated: the fake VAD does not change its segmentation.
* ``--reject-segmentation-field``: advertises the capability like
  ``--emulate-segmentation`` but renames the field to an unknown key, so the
  server answers ``protocol_error`` (a server that advertises but refuses).

Recognition-hint (context) knobs. ``features.context_biasing`` /
``context_limits``, ``session.start.context``, ``GET /v1/dictionaries`` and
``session.started.context`` are being added to the server (branch
agent/context-hints); these emulate that contract around the unchanged app:

* ``--emulate-context``: advertises ``context_biasing: true`` and
  ``context_limits`` (``--context-limits`` JSON overrides the contract
  values), serves ``GET /v1/dictionaries`` (bearer token required) from
  ``--dictionaries``, removes ``context`` from the client's session.start
  before the server app sees it, checks it like the server would (only the
  four keys, types, limits, a known profile) and echoes
  ``session.started.context``. A context the real server would refuse is
  renamed to an unknown key instead, so the app answers ``protocol_error``
  exactly like a rejection.
* ``--reject-context-field``: advertises like ``--emulate-context`` but always
  refuses the field that way.
* ``--hide-context-capability``: strips ``features.context_biasing`` and
  ``context_limits`` (a server that predates the feature).

Singing knobs (``features.singing_detection``, ``segment.audio_class``,
``transcript.final.audio_class``; a server with the YAMNet asset labels
every segment by default):

* ``--hide-singing-capability``: a server without the feature: strips the
  capability, drops the real labels and the finals' ``audio_class``.
* ``--emulate-singing SPEC``: advertises ``singing_detection: true`` and
  sends scripted ``segment.audio_class`` events instead of the real ones
  (which are dropped). SPEC is a comma list of
  ``INDEX:CLASS@N`` (``+CLASS@M`` adds a ``revision=1`` change): the event
  for segment INDEX goes out right before that segment's next transcript
  event once N of its partials have been sent (``@0``: before any text).
  Unlisted segments get ``speech@1``. Finals carry ``audio_class``.
* ``--tag-segments``: prefixes every transcript text of segment i with
  ``§i`` (stable / partial / final alike, so stable stays append-only), so a
  test can tell which segment a caption came from.

Without these knobs the real implementation is used as is: the harness passes
``TEA_ASR_CONTEXT_HINTS`` / ``TEA_ASR_CONTEXT_PROMPT`` from its environment
into the server config (when the server has those settings), and server
dictionaries are read from ``<state-dir>/dictionaries/<name>.toml``.
"""

from __future__ import annotations

import argparse
import dataclasses
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
    parser.add_argument("--hide-segmentation-capability", action="store_true")
    parser.add_argument("--emulate-segmentation", action="store_true")
    parser.add_argument("--reject-segmentation-field", action="store_true")
    parser.add_argument("--deaf", action="store_true")
    parser.add_argument("--emulate-context", action="store_true")
    parser.add_argument("--reject-context-field", action="store_true")
    parser.add_argument("--hide-context-capability", action="store_true")
    parser.add_argument("--context-limits", default="")
    parser.add_argument("--emulate-singing", default=None)
    parser.add_argument("--hide-singing-capability", action="store_true")
    parser.add_argument("--tag-segments", action="store_true")
    parser.add_argument(
        "--dictionaries",
        default='[{"name":"church","domain":"主日講道","hotwords_count":42,"replacements_count":7},'
                '{"name":"youth","domain":"青年聚會","hotwords_count":5,"replacements_count":0}]',
    )
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

        async def transcribe(self, pcm: bytes, *, language: str = "Chinese", **kwargs):
            # kwargs: system_prompt from a server with recognition hints
            # (TEA_ASR_CONTEXT_PROMPT=1); passed on only when there is one.
            result = await super().transcribe(pcm, language=language, **kwargs)
            chars = max(1, min(len(self.SENTENCE), (len(pcm) // 2) // 3200))
            result["text"] = self.SENTENCE[:chars] + self.GUESSES[self.calls % 2]
            return result

    supervisor = (GrowingSupervisor if args.growing_text else FakeSupervisor)(text=args.text)
    supervisor.last_error = "FAKE_BACKEND: tests/conftest.py FakeSupervisor, no ASR model loaded"

    limiter = AuthRateLimiter()
    for _ in range(args.prefail):
        limiter.record_failure("127.0.0.1")

    extra_config = {}
    # Recognition hints of a server that has them (agent/context-hints):
    # TEA_ASR_CONTEXT_HINTS / TEA_ASR_CONTEXT_PROMPT in this harness's
    # environment, like the real service reads them. An older server has no
    # such fields and ignores the variables.
    config_fields = {f.name for f in dataclasses.fields(ServiceConfig)}
    for env_name, field_name in (("TEA_ASR_CONTEXT_HINTS", "context_hints_enabled"),
                                 ("TEA_ASR_CONTEXT_PROMPT", "context_prompt_enabled"),
                                 ("TEA_ASR_SINGING_DETECTION", "singing_detection_enabled")):
        if field_name in config_fields and os.environ.get(env_name) is not None:
            extra_config[field_name] = os.environ[env_name] == "1"
    config = ServiceConfig(
        revisable_preview=args.revisable,
        max_total_connections=args.max_total_connections,
        max_continuous_sessions=args.max_continuous_sessions,
        idle_unload_s=0,
        **extra_config,
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

    context_limits = {"max_domain_chars": 300, "max_hotwords": 200, "max_hotword_chars": 32,
                      "max_replacements": 500, "max_replacement_chars": 32}
    if args.context_limits:
        context_limits.update(json.loads(args.context_limits))
    dictionaries = json.loads(args.dictionaries)
    advertise_context = args.emulate_context or args.reject_context_field

    def context_problem(ctx) -> str | None:
        """Why the server (contract of agent/context-hints) would refuse it."""
        if not isinstance(ctx, dict):
            return "context must be an object"
        extra = set(ctx) - {"profile", "domain", "hotwords", "replacements"}
        if extra:
            return f"unknown keys {sorted(extra)}"
        lim = context_limits
        if "profile" in ctx and ctx["profile"] not in {d["name"] for d in dictionaries}:
            return "unknown profile"
        if "domain" in ctx and (not isinstance(ctx["domain"], str) or len(ctx["domain"]) > lim["max_domain_chars"]):
            return "domain"
        words = ctx.get("hotwords", [])
        if not isinstance(words, list) or len(words) > lim["max_hotwords"] or any(
                not isinstance(w, str) or not w or len(w) > lim["max_hotword_chars"] for w in words):
            return "hotwords"
        pairs = ctx.get("replacements", [])
        if not isinstance(pairs, list) or len(pairs) > lim["max_replacements"]:
            return "replacements"
        for pair in pairs:
            if (not isinstance(pair, dict) or set(pair) != {"from", "to"} or not isinstance(pair["from"], str)
                    or not pair["from"] or not isinstance(pair["to"], str)
                    or len(pair["from"]) > lim["max_replacement_chars"]
                    or len(pair["to"]) > lim["max_replacement_chars"]):
                return "replacements"
        return None

    # --emulate-singing: {index: [(class, after_partials), ...]}
    singing_plan: dict[int, list[tuple[str, int]]] = {}
    if args.emulate_singing:
        for item in args.emulate_singing.split(","):
            index, steps = item.split(":", 1)
            singing_plan[int(index)] = [(step.split("@")[0], int(step.split("@")[1])) for step in steps.split("+")]

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

        if (scope["type"] == "http" and base["path"] == "/v1/dictionaries" and advertise_context):
            token = (state_dir / "token").read_text().strip()
            ok = headers.get("authorization") == f"Bearer {token}"
            body = json.dumps(dictionaries if ok else {"error": {"code": "unauthenticated"}},
                              ensure_ascii=False).encode()
            status = 200 if ok else 401
            await send({"type": "http.response.start", "status": status,
                        "headers": [(b"content-type", b"application/json"),
                                    (b"content-length", str(len(body)).encode())]})
            await send({"type": "http.response.body", "body": body})
            record({**base, "event": "http", "status": status})
            return

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
                            "segmentation": payload.get("segmentation"),
                            "has_context": "context" in payload,
                            "context": payload.get("context"),
                            "stable_rewritten": bool(args.reject_stable_field and "stable" in payload)})
                    changed = False
                    if args.reject_stable_field and "stable" in payload:
                        payload["stable_unknown_to_this_server"] = payload.pop("stable")
                        changed = True
                    if "segmentation" in payload and args.reject_segmentation_field:
                        payload["segmentation_unknown_to_this_server"] = payload.pop("segmentation")
                        changed = True
                    elif "segmentation" in payload and args.emulate_segmentation:
                        held["end_silence_ms"] = payload.pop("segmentation").get("end_silence_ms")
                        changed = True
                    if "context" in payload and advertise_context:
                        problem = "refused by --reject-context-field" if args.reject_context_field \
                            else context_problem(payload["context"])
                        if problem:
                            record({**base, "event": "ws_context_refused", "why": problem})
                            payload["context_refused_" + problem.replace(" ", "_")[:40]] = payload.pop("context")
                        else:
                            held["context"] = payload.pop("context")
                        changed = True
                    if changed:
                        message = {**message, "text": json.dumps(payload, ensure_ascii=False)}
            return message

        async def wrapped_send(message):
            mtype = message["type"]
            rewrite_caps = (args.hide_stable_capability or args.hide_segmentation_capability
                            or args.emulate_segmentation or args.reject_segmentation_field
                            or advertise_context or args.hide_context_capability
                            or args.emulate_singing is not None or args.hide_singing_capability)
            if (rewrite_caps and base["path"] == "/v1/capabilities"
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
                    features = doc.get("features", {})
                    if args.hide_stable_capability:
                        features.pop("stable_transcripts", None)
                    if args.hide_segmentation_capability:
                        features.pop("segmentation_control", None)
                    if args.emulate_segmentation or args.reject_segmentation_field:
                        features["segmentation_control"] = {"end_silence_ms": {
                            "min": 300, "max": 3000, "default": 900, "default_final_only": 500}}
                    if advertise_context:
                        features["context_biasing"] = True
                        features["context_limits"] = context_limits
                    if args.emulate_singing is not None:
                        features["singing_detection"] = True
                    if args.hide_singing_capability:
                        features.pop("singing_detection", None)
                    if args.hide_context_capability:
                        features.pop("context_biasing", None)
                        features.pop("context_limits", None)
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
                ptype = payload.get("type")
                if ptype == "speech.started" and payload.get("segment_id"):
                    held.setdefault("seg_index", {})[payload["segment_id"]] = payload.get("segment_index", 0)
                if ptype == "segment.audio_class":  # a real server's label
                    dropped = args.emulate_singing is not None or args.hide_singing_capability
                    record({**base, "event": "ws_audio_class", "segment_id": payload.get("segment_id"),
                            "segment_index": payload.get("segment_index"), "class": payload.get("class"),
                            "revision": payload.get("revision"), "emulated": False, "dropped": dropped})
                    if dropped:
                        return
                if (ptype == "transcript.final" and args.hide_singing_capability
                        and "audio_class" in payload):
                    payload.pop("audio_class")
                    message = {**message, "text": json.dumps(payload, ensure_ascii=False)}
                if (ptype in ("transcript.partial", "transcript.stable", "transcript.final")
                        and payload.get("segment_id")):
                    seg_id = payload["segment_id"]
                    index = payload.get("segment_index", held.get("seg_index", {}).get(seg_id, -1))
                    if args.emulate_singing is not None:
                        state = held.setdefault("singing", {}).setdefault(
                            seg_id, {"partials": 0, "next": 0, "class": None})
                        steps = singing_plan.get(index, [("speech", 1)])
                        while state["next"] < len(steps) and (
                                steps[state["next"]][1] <= state["partials"] or ptype == "transcript.final"):
                            cls = steps[state["next"]][0]
                            event = {"type": "segment.audio_class", "session_id": payload.get("session_id"),
                                     "event_id": 0, "segment_id": seg_id, "segment_index": index,
                                     "class": cls, "confidence": 0.99, "revision": state["next"]}
                            await send({"type": "websocket.send", "text": json.dumps(event)})
                            record({**base, "event": "ws_audio_class", "segment_id": seg_id,
                                    "segment_index": index, "class": cls, "revision": state["next"],
                                    "emulated": True})
                            state["class"] = cls
                            state["next"] += 1
                        if ptype == "transcript.partial":
                            state["partials"] += 1
                        if ptype == "transcript.final":
                            payload.pop("audio_class", None)
                            if state["class"]:
                                payload["audio_class"] = state["class"]
                    if args.tag_segments and payload.get("text"):
                        payload["text"] = f"§{index}" + payload["text"]
                    message = {**message, "text": json.dumps(payload, ensure_ascii=False)}
                if args.deaf and payload.get("type") in (
                        "speech.started", "segment.queued", "transcript.partial", "transcript.stable",
                        "transcript.final", "segment.skipped", "segment.error"):
                    record({**base, "event": "ws_dropped_by_deaf", "type": payload.get("type")})
                    return
                if (payload.get("type") == "session.started" and args.emulate_segmentation
                        and held.get("end_silence_ms") is not None and payload.get("preview_policy")):
                    payload["preview_policy"]["endpoint_silence_ms"] = held["end_silence_ms"]
                    message = {**message, "text": json.dumps(payload, ensure_ascii=False)}
                if payload.get("type") == "session.started" and held.get("context") is not None:
                    ctx = held["context"]
                    payload["context"] = {"profile": ctx.get("profile"),
                                          "domain_chars": len(ctx.get("domain", "")),
                                          "hotwords_count": len(ctx.get("hotwords", [])),
                                          "replacements_count": len(ctx.get("replacements", [])),
                                          "prompt_tokens": 17}
                    message = {**message, "text": json.dumps(payload, ensure_ascii=False)}
                if payload.get("type") == "session.started":
                    record({**base, "event": "ws_session_started", "context": payload.get("context"),
                            "endpoint_silence_ms": (payload.get("preview_policy") or {}).get("endpoint_silence_ms")})
                if payload.get("type") == "error":
                    record({**base, "event": "ws_error_event", "code": payload.get("code"),
                            "retryable": payload.get("retryable"), "message": payload.get("message")})
                elif payload.get("type") in ("transcript.partial", "transcript.final"):
                    record({**base, "event": "ws_" + payload["type"].split(".")[1],
                            "segment_id": payload.get("segment_id"), "text": payload.get("text"),
                            "raw_text": payload.get("raw_text"), "warnings": payload.get("warnings"),
                            "audio_class": payload.get("audio_class")})
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
