#!/usr/bin/env python3
"""End-to-end compatibility tests: plugin ASR client <-> current tea-asr-service.

Each scenario starts the *current* server app (tests/e2e/fake_asr_server.py,
test fake backend, never a real model) on a free loopback port, runs the
plugin's real TeaAsrClient through tests/e2e/driver.cpp, then asserts on both
sides: what the plugin showed (status text / rendered captions) and what the
server saw (every HTTP request and WS upgrade, from the request log).

Requirements (not available in CI, which is why this is separate from
protocol-tests.yaml):
  * a tea-asr-service checkout with its .venv   (TEA_ASR_SERVICE_DIR)
  * the driver built from tests/e2e/CMakeLists.txt with a desktop Qt6

Usage:
  python3 tests/e2e/run_e2e.py --driver <builddir>/asr-client-e2e [--only a,b] [--list]
"""

from __future__ import annotations

import argparse
import json
import re
import os
import secrets
import signal
import socket
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_SERVICE_DIR = Path(os.environ.get("TEA_ASR_SERVICE_DIR", HERE.parents[3] / "tea-asr-service"))


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    if port == 8327:  # never the real service's port
        return free_port()
    return port


def atomic_write(path: Path, text: str) -> None:
    tmp = path.with_suffix(".tmp")
    tmp.write_text(text)
    os.chmod(tmp, 0o600)
    os.replace(tmp, path)


class FakeServer:
    def __init__(self, work: Path, port: int, service_dir: Path, extra: list[str], env: dict | None = None):
        self.work = work
        self.port = port
        self.service_dir = service_dir
        self.extra = extra
        self.env = env or {}
        self.state = work / "state"
        self.request_log = work / "requests.jsonl"
        self.proc: subprocess.Popen | None = None

    def start(self) -> None:
        python = self.service_dir / ".venv" / "bin" / "python"
        env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1", **self.env)
        stderr = (self.work / "server.stderr").open("a")
        self.proc = subprocess.Popen(
            [str(python), str(HERE / "fake_asr_server.py"), "--service-dir", str(self.service_dir),
             "--port", str(self.port), "--state-dir", str(self.state),
             "--request-log", str(self.request_log), *self.extra],
            stdout=stderr, stderr=stderr, env=env,
        )
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            if (self.state / "token").exists():
                try:
                    with socket.create_connection(("127.0.0.1", self.port), timeout=0.2):
                        return
                except OSError:
                    pass
            if self.proc.poll() is not None:
                raise RuntimeError(f"fake server exited: see {self.work / 'server.stderr'}")
            time.sleep(0.1)
        raise RuntimeError("fake server did not come up")

    def stop(self) -> None:
        if self.proc and self.proc.poll() is None:
            self.proc.send_signal(signal.SIGINT)
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        self.proc = None

    @property
    def token_file(self) -> Path:
        return self.state / "token"

    def requests(self) -> list[dict]:
        if not self.request_log.exists():
            return []
        return [json.loads(line) for line in self.request_log.read_text().splitlines() if line]


@dataclass
class Run:
    lines: list[dict]
    requests: list[dict]
    notes: list[str] = field(default_factory=list)
    log: list[str] = field(default_factory=list)  # the client's obs_log lines (what OBS would log)

    def log_lines(self, needle: str) -> list[str]:
        return [l for l in self.log if needle in l]

    def statuses(self, client: int = 0) -> list[tuple[float, str]]:
        return [(l["t"], l["status"]) for l in self.lines if l.get("client") == client and "status" in l]

    def captions(self, client: int = 0) -> list[dict]:
        return [l for l in self.lines if l.get("client") == client and "caption" in l]

    def snapshots(self, client: int = 0) -> list[dict]:
        return [l for l in self.lines if l.get("client") == client and "snapshot" in l]

    def event_time(self, name: str) -> float | None:
        return next((l["t"] for l in self.lines if l.get("event") == name), None)

    def any_status(self, needle: str, client: int = 0) -> bool:
        return any(needle in s for _, s in self.statuses(client))

    def attempts(self) -> list[float]:
        """Server-side times of connection attempts (preflight GET or WS upgrade)."""
        out = []
        for r in self.requests:
            if r["event"] == "http" and r["path"] == "/v1/capabilities":
                out.append(r["t"])
            elif r["event"] in ("ws_accept", "ws_reject"):
                out.append(r["t"])
        return sorted(out)

    def auth_failures(self) -> list[float]:
        out = []
        for r in self.requests:
            if r["event"] == "http" and r.get("status") == 401:
                out.append(r["t"])
            elif r["event"] == "ws_reject" and r.get("reason") == "unauthenticated":
                out.append(r["t"])
        return sorted(out)

    def rate_limited(self) -> list[float]:
        return sorted(
            r["t"] for r in self.requests
            if (r["event"] == "http" and r.get("status") == 429)
            or (r["event"] == "ws_reject" and r.get("reason") == "rate_limited")
        )


class Checker:
    def __init__(self, name: str):
        self.name = name
        self.failures: list[str] = []
        self.passes: list[str] = []

    def check(self, cond: bool, what: str) -> None:
        (self.passes if cond else self.failures).append(what)
        print(f"    [{'ok' if cond else 'FAIL'}] {what}")


def run_driver(driver: Path, port: int, token: Path, duration_ms: int, work: Path, *,
               host: str = "127.0.0.1", clients: int = 1, audio: str = "speech",
               restart_at_ms: int | None = None, during=None, stable: str = "on",
               extra_args: tuple[str, ...] = ()) -> list[dict]:
    cmd = [str(driver), "--host", host, "--port", str(port), "--token", str(token),
           "--clients", str(clients), "--duration-ms", str(duration_ms), "--audio", audio,
           "--stable", stable, *extra_args]
    if restart_at_ms is not None:
        cmd += ["--restart-at-ms", str(restart_at_ms)]
    log_path = work / "driver.stderr"
    offset = log_path.stat().st_size if log_path.exists() else 0
    with log_path.open("a") as err:
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=err, text=True)
        if during is not None:
            during()
        out, _ = proc.communicate(timeout=duration_ms / 1000 + 30)
    lines = []
    for line in out.splitlines():
        try:
            lines.append(json.loads(line))
        except ValueError:
            pass
    (work / "driver.jsonl").write_text(out)
    with log_path.open("r", encoding="utf-8", errors="replace") as fh:
        fh.seek(offset)
        log = fh.read().splitlines()
    return lines, log


def max_in_window(times: list[float], window: float) -> int:
    best = 0
    for i, t in enumerate(times):
        best = max(best, sum(1 for u in times[i:] if u - t < window))
    return best


def gaps(times: list[float]) -> list[float]:
    return [round(b - a, 1) for a, b in zip(times, times[1:])]


# --------------------------------------------------------------------------- scenarios


def sc_happy(ctx) -> Checker:
    c = Checker("happy_revisable")
    srv = ctx.server(["--revisable"])
    # The legacy partial-replace path (stable captions setting off); the
    # stable path has its own scenarios below.
    run = ctx.drive(srv, srv.token_file, 12000, stable="off")
    caps = [r for r in run.requests if r["event"] == "http" and r["path"] == "/v1/capabilities"]
    ws = [r for r in run.requests if r["event"] in ("ws_accept", "ws_reject")]
    c.check(bool(caps) and all(r["has_auth"] for r in caps) and caps[0]["status"] == 200,
            f"capabilities probe is authenticated and succeeds: {[(r['has_auth'], r['status']) for r in caps]}")
    c.check(bool(ws) and ws[0]["event"] == "ws_accept", f"WS upgrade accepted: {[r['event'] for r in ws]}")
    c.check(all(r["origin"] is None for r in ws), "WS upgrade carries no Origin header (native client)")
    c.check(all(r["host"] == f"127.0.0.1:{srv.port}" for r in run.requests),
            f"Host header is 127.0.0.1:<port>: {sorted({r['host'] for r in run.requests})}")
    c.check(run.any_status("session active"), "status reaches 'session active'")
    caps_lines = run.captions()
    c.check(any(l["partial"] and "測試文字" in l["caption"] for l in caps_lines),
            "a transcript.partial preview was rendered")
    c.check(any((not l["partial"]) and "測試文字" in l["caption"] for l in caps_lines),
            "a transcript.final was rendered")
    c.check(not any("reconnect" in s or "error" in s for _, s in run.statuses()),
            "no error/reconnect during a healthy session")
    c.check(all("session" not in l["caption"] and "connect" not in l["caption"] for l in caps_lines),
            "diagnostics never enter the caption text")
    run.notes.append(f"statuses={[s for _, s in run.statuses()]}")
    return c, run


def sc_heartbeat_long_session(ctx) -> Checker:
    c = Checker("heartbeat_long_session")
    srv = ctx.server(["--revisable"])
    # > 15 s ping interval + 30 s pong timeout (server) and > the client's own
    # 45 s silence watchdog: the session must survive on WS ping/pong alone.
    run = ctx.drive(srv, srv.token_file, 70000)
    accepts = [r for r in run.requests if r["event"] == "ws_accept"]
    closes = [r for r in run.requests if r["event"] == "ws_close"]
    c.check(len(accepts) == 1 and not closes, f"one connection for 70 s, never closed ({len(accepts)} accepts, {closes})")
    finals = [l for l in run.captions() if not l["partial"] and "測試文字" in l["caption"]]
    last_final = max((l["t"] for l in finals), default=0)
    c.check(last_final > 60000, f"finals still arriving after 60 s (last at {last_final} ms)")
    c.check(not any("reconnect" in s for _, s in run.statuses()), "no reconnect")
    return c, run


def sc_final_only(ctx) -> Checker:
    c = Checker("final_only_server")
    srv = ctx.server([])  # revisable preview off
    run = ctx.drive(srv, srv.token_file, 9000)
    lines = run.captions()
    c.check(run.any_status("session active"), "status reaches 'session active'")
    c.check(any("測試文字" in l["caption"] and not l["partial"] for l in lines), "finals rendered")
    c.check(not any(l["partial"] for l in lines), "no partial previews when the server does not offer them")
    return c, run


def sc_bad_token(ctx) -> Checker:
    c = Checker("bad_token_no_rate_limit")
    srv = ctx.server([])
    wrong = ctx.work / "wrong-token"
    atomic_write(wrong, "definitely-not-the-token")
    run = ctx.drive(srv, wrong, 75000)
    fails = run.auth_failures()
    c.check(max_in_window(fails, 60.0) < 10,
            f"auth failures in any 60 s window stay below the server limit of 10 (got {max_in_window(fails, 60.0)}; times={fails})")
    c.check(not run.rate_limited(), f"the plugin never drives itself into rate_limited (429s at {run.rate_limited()})")
    c.check(run.any_status("unauthenticated"), "status names the real reason (unauthenticated)")
    c.check(not any(r["event"].startswith("ws_") for r in run.requests),
            "no WS upgrade is attempted with a token the preflight already rejected")
    run.notes.append(f"attempt gaps={gaps(run.attempts())}")
    return c, run


def sc_revoked_then_rotated(ctx) -> Checker:
    c = Checker("revoked_then_rotated")
    srv = ctx.server(["--revisable"])
    atomic_write(srv.token_file, "")  # `tea-asr token revoke`

    def rotate_later():
        time.sleep(6)
        atomic_write(srv.token_file, secrets.token_urlsafe(32))  # `tea-asr token rotate`

    run = ctx.drive(srv, srv.token_file, 14000, during=rotate_later)
    before = [r for r in run.requests if r["t"] < 5.5]
    c.check(not before, f"while the token file is empty the plugin sends nothing ({len(before)} requests)")
    c.check(run.any_status("token"), "status explains the missing/empty token")
    c.check(not run.auth_failures(), f"no auth failures at all ({run.auth_failures()})")
    active = [t for t, s in run.statuses() if "session active" in s]
    c.check(bool(active) and active[0] < 11000, f"reconnects promptly after rotation (session active at {active[:1]} ms)")
    return c, run


def sc_rotation_mid_session(ctx) -> Checker:
    c = Checker("rotation_mid_session_and_server_restart")
    srv = ctx.server(["--revisable"])

    def rotate_and_restart():
        time.sleep(4)
        atomic_write(srv.token_file, secrets.token_urlsafe(32))
        time.sleep(2)
        srv.stop()
        time.sleep(3)
        srv.start()

    run = ctx.drive(srv, srv.token_file, 24000, during=rotate_and_restart)
    active = [t for t, s in run.statuses() if "session active" in s]
    c.check(len(active) >= 2, f"session active before and again after the restart (at {active} ms)")
    c.check(not run.auth_failures(), "the rotated token is picked up from the file: zero auth failures")
    c.check(run.any_status("reconnecting"), "the disconnect is visible (reconnecting ...)")
    return c, run


def sc_reconnect_all_then_server_restart(ctx) -> Checker:
    c = Checker("reconnect_all_then_server_restart")
    srv = ctx.server([])

    def restart_server():
        time.sleep(6)
        srv.stop()
        time.sleep(3)
        srv.start()

    # t=3 s: explicit restart (settings change / "Reconnect All"), then the
    # server goes away for ~3 s. Auto-reconnect must still be armed.
    run = ctx.drive(srv, srv.token_file, 22000, restart_at_ms=3000, during=restart_server)
    active = [t for t, s in run.statuses() if "session active" in s]
    c.check(any(t > 9000 for t in active), f"reconnects after the server restart (session active at {active} ms)")
    return c, run


def sc_host_rejected(ctx) -> Checker:
    c = Checker("host_rejected")
    srv = ctx.server([])
    run = ctx.drive(srv, srv.token_file, 20000, host="localhost.")
    rej = [r for r in run.requests if r["event"] == "http" and r.get("status") == 403]
    c.check(bool(rej), f"server rejected Host {sorted({r['host'] for r in run.requests})} with 403")
    c.check(run.any_status("forbidden_origin"), "status names forbidden_origin")
    c.check(len(run.attempts()) <= 2, f"no retry storm on a config error ({len(run.attempts())} attempts in 20 s)")
    c.check(not any(r["event"].startswith("ws_") for r in run.requests), "no WS attempt after Host rejection")
    return c, run


def sc_idle_timeout(ctx) -> Checker:
    c = Checker("idle_timeout_4408")
    srv = ctx.server(["--idle-timeout-s", "3"])
    run = ctx.drive(srv, srv.token_file, 30000, audio="dead")
    closes = [r for r in run.requests if r["event"] == "ws_close" and r.get("close_code") == 4408]
    c.check(bool(closes), f"server closed with 4408 ({len(closes)} times)")
    c.check(run.any_status("idle_timeout"), "status names idle_timeout")
    accepts = sorted(r["t"] for r in run.requests if r["event"] == "ws_accept")
    g = gaps(accepts)
    c.check(len(g) >= 2 and g[-1] > g[0] + 1.5, f"reconnect interval grows between idle sessions (gaps={g})")
    return c, run


def sc_connection_cap(ctx) -> Checker:
    c = Checker("connection_cap_session_limit")
    srv = ctx.server(["--max-total-connections", "1"])
    run = ctx.drive(srv, srv.token_file, 32000, clients=2)
    rejects = sorted(r["t"] for r in run.requests if r["event"] == "ws_reject" and r.get("reason") == "session_limit")
    c.check(bool(rejects), f"server rejected the 2nd connection pre-accept with session_limit ({len(rejects)} times)")
    loser = 1 if run.any_status("session active", 0) else 0
    c.check(run.any_status("session active", 1 - loser), "the first client streams normally")
    c.check(run.any_status("session_limit", loser), "the rejected client shows session_limit / connection cap")
    c.check(len(rejects) <= 7, f"rejected client backs off (gaps={gaps(rejects)})")
    return c, run


def sc_concurrent_limit(ctx) -> Checker:
    c = Checker("concurrent_session_limit_4029")
    srv = ctx.server(["--max-continuous-sessions", "1"])
    run = ctx.drive(srv, srv.token_file, 32000, clients=2)
    errs = sorted(r["t"] for r in run.requests
                  if r["event"] == "ws_error_event" and r.get("code") == "concurrent_session_limit")
    c.check(bool(errs), f"server sent concurrent_session_limit ({len(errs)} times)")
    loser = 1 if run.any_status("session active", 0) else 0
    c.check(run.any_status("concurrent_session_limit", loser), "the rejected client shows the admission reason")
    c.check(len(errs) <= 7, f"rejected client backs off (gaps={gaps(errs)})")
    return c, run


def sc_rate_limited(ctx) -> Checker:
    c = Checker("rate_limited_429")
    srv = ctx.server(["--prefail", "10"])
    run = ctx.drive(srv, srv.token_file, 85000)
    rl = run.rate_limited()
    c.check(bool(rl), f"server answered rate_limited ({rl})")
    c.check(run.any_status("rate_limited"), "status names rate_limited")
    during_block = [t for t in run.attempts() if rl and rl[0] < t < rl[0] + 55]
    c.check(not during_block, f"no further attempts while the 60 s window is still blocked ({during_block})")
    c.check(run.any_status("session active"), "connects once the window has expired")
    return c, run


def sc_no_audio_source(ctx) -> Checker:
    c = Checker("no_audio_source")
    srv = ctx.server([])
    run = ctx.drive(srv, srv.token_file, 6000, audio="none")
    c.check(not run.requests, f"a source with no audio selected holds no server slot ({len(run.requests)} requests)")
    c.check(run.any_status("audio source"), "status says an audio source must be selected")
    return c, run


# ------------------------------------------------------------ stable captions


GUESSES = "嗯啊"  # fake_asr_server.py --growing-text trailing guess characters


DRIVER_MAX_LINES = 3  # driver.cpp: tea_caption_state_set_max_lines(..., 3)


def caption_rewrites(lines: list[dict]) -> list[tuple[str, str]]:
    """Consecutive caption snapshots where shown text was changed or removed.

    Allowed between two snapshots (20 ms apart): every shown line keeps its
    text and may only grow at its end, new lines appear at the bottom, and
    once the window is full the top line may scroll off (one per snapshot).
    Anything else -- a character replaced, a line shortened or removed, the
    canvas cleared -- is a rewrite, i.e. visible flicker.
    """
    bad = []
    prev: list[str] = []
    for l in lines:
        cur = l["caption"].split("\n") if l["caption"] else []
        scrolls = [0, 1] if len(prev) >= DRIVER_MAX_LINES else [0]
        ok = len(cur) >= len(prev) and any(
            all(i < len(cur) and cur[i].startswith(p) for i, p in enumerate(prev[k:])) for k in scrolls)
        if not ok:
            bad.append(("\n".join(prev), l["caption"]))
        prev = cur
    return bad


def partial_rewrites(requests: list[dict]) -> int:
    """Server-side: partials that did not extend the previous partial of their segment."""
    last: dict[str, str] = {}
    n = 0
    for r in requests:
        if r["event"] == "ws_partial":
            old = last.get(r["segment_id"])
            if old is not None and not r["text"].startswith(old):
                n += 1
            last[r["segment_id"]] = r["text"]
    return n


def done_mismatches(run: Run) -> list:
    return next((l.get("stable_mismatches") for l in run.lines if l.get("event") == "done"), None)


def sc_stable_append_only(ctx) -> Checker:
    c = Checker("stable_append_only")
    srv = ctx.server(["--revisable", "--growing-text"])
    run = ctx.drive(srv, srv.token_file, 14000)
    starts = [r for r in run.requests if r["event"] == "ws_session_start"]
    c.check(bool(starts) and starts[0]["transcript_mode"] == "revisable" and starts[0]["stable"] == {"agreement": 2},
            f"session.start asked for revisable + stable agreement 2: {[(r['transcript_mode'], r['stable']) for r in starts]}")
    stables = [r for r in run.requests if r["event"] == "ws_stable"]
    states = [r["state"] for r in stables]
    c.check(states.count("open") >= 2 and "final" in states,
            f"server sent transcript.stable open + closing events ({len(stables)}: {sorted(set(states))})")
    grew = {}
    for r in stables:
        grew.setdefault(r["segment_id"], []).append(r["text"])
    c.check(any(len(set(v)) >= 3 for v in grew.values()),
            f"committed text grew over several stable events within a segment "
            f"(max distinct per segment {max((len(set(v)) for v in grew.values()), default=0)})")
    pr = partial_rewrites(run.requests)
    c.check(pr > 0, f"the fixture's partials really rewrite characters ({pr} rewriting partials)")
    caps = run.captions()
    c.check(any(l["caption"] for l in caps), f"captions rendered ({len(caps)} snapshots)")
    bad = caption_rewrites(caps)
    c.check(not bad, f"rendered captions only ever append (rewrites: {bad[:3]})")
    committed = {r["text"] for r in stables} | {r["text"] for r in run.requests if r["event"] == "ws_final"}
    shown = {line for l in caps for line in l["caption"].split("\n") if line}
    stray = sorted(shown - committed)
    c.check(not stray, f"every rendered line is committed stable/final text, never a partial tail ({stray[:3]})")
    c.check(any("🍵" in l["caption"] for l in caps), "a 4-byte emoji tail was appended and rendered")
    c.check(run.any_status("stable captions"), "Tools status reports stable captions active")
    c.check(done_mismatches(run) == [0], f"no non-append stable values ({done_mismatches(run)})")
    c.check(all("session" not in l["caption"] and "connect" not in l["caption"] for l in caps),
            "diagnostics never enter the caption text")

    # Same server and audio with the setting off: the legacy partial preview
    # does rewrite shown text, so the append-only check above is meaningful.
    legacy = ctx.drive(srv, srv.token_file, 9000, stable="off")
    new_starts = [r for r in legacy.requests if r["event"] == "ws_session_start"][len(starts):]
    c.check(bool(new_starts) and all(r["stable"] is None for r in new_starts),
            f"setting off: session.start carries no stable ({[r['stable'] for r in new_starts]})")
    legacy_bad = caption_rewrites(legacy.captions())
    c.check(bool(legacy_bad), f"setting off: partial previews rewrite shown text ({len(legacy_bad)} rewrites)")
    run.notes.append(f"stable events={len(stables)} partial rewrites={pr} legacy caption rewrites={len(legacy_bad)}")
    return c, run


def sc_stable_fallback_no_capability(ctx) -> Checker:
    c = Checker("stable_fallback_no_capability")
    srv = ctx.server(["--revisable", "--growing-text", "--hide-stable-capability"])
    run = ctx.drive(srv, srv.token_file, 10000)  # setting on (default)
    starts = [r for r in run.requests if r["event"] == "ws_session_start"]
    c.check(bool(starts) and all(r["transcript_mode"] == "revisable" and r["stable"] is None for r in starts),
            f"no stable requested when the server does not advertise it: {[(r['transcript_mode'], r['stable']) for r in starts]}")
    c.check(not [r for r in run.requests if r["event"] == "ws_stable"], "no transcript.stable sent")
    c.check(run.any_status("session active") and not run.any_status("stable captions"),
            "connects normally, status does not claim stable captions")
    caps = run.captions()
    c.check(any(l["partial"] and l["caption"][-1:] in GUESSES for l in caps),
            "falls back to the partial preview (unconfirmed tail shown, as before)")
    c.check(any(not l["partial"] and l["caption"] for l in caps), "finals rendered")
    c.check(bool(caption_rewrites(caps)), "legacy partial-replace behaviour is unchanged")
    return c, run


def sc_stable_rejected_downgrade(ctx) -> Checker:
    c = Checker("stable_rejected_downgrade")
    srv = ctx.server(["--revisable", "--reject-stable-field"])
    run = ctx.drive(srv, srv.token_file, 12000)
    starts = [r for r in run.requests if r["event"] == "ws_session_start"]
    errs = [r for r in run.requests if r["event"] == "ws_error_event"]
    c.check(bool(starts) and starts[0]["stable"] == {"agreement": 2},
            "first session.start asks for stable (server advertises it)")
    c.check(any(r["code"] == "protocol_error" for r in errs), f"server rejects it like an old server: {errs[:2]}")
    c.check(len(starts) >= 2 and starts[1]["stable"] is None and starts[1]["transcript_mode"] == "revisable",
            f"next attempt drops stable, keeps revisable: {[(r['transcript_mode'], r['stable']) for r in starts]}")
    c.check(run.any_status("rejected stable captions"), "Tools status explains the downgrade (on the active session)")
    c.check(run.any_status("session active") and not run.any_status("stopped"),
            "still connects; the rejection is not fatal")
    c.check(any(l["partial"] and "測試文字" in l["caption"] for l in run.captions()),
            "partial previews rendered after the downgrade")
    return c, run


def sc_stable_reconnect_hold(ctx) -> Checker:
    c = Checker("stable_reconnect_hold")
    srv = ctx.server(["--revisable", "--growing-text"])
    marks: dict[str, float] = {}
    t0 = time.monotonic()

    def outages():
        time.sleep(7)
        marks["stop1"] = (time.monotonic() - t0) * 1000
        srv.stop()
        time.sleep(3)
        srv.start()
        marks["start1"] = (time.monotonic() - t0) * 1000
        time.sleep(8)
        marks["stop2"] = (time.monotonic() - t0) * 1000
        srv.stop()
        time.sleep(14)
        srv.start()
        marks["start2"] = (time.monotonic() - t0) * 1000

    # Reconnect backoff has grown to ~16 s by the second outage: leave room.
    run = ctx.drive(srv, srv.token_file, 55000, during=outages)
    caps = run.captions()
    active = [t for t, s in run.statuses() if "session active" in s]
    c.check(len([t for t in active if t > marks["start1"] - 500]) >= 1 and active[0] < marks["stop1"],
            f"session active before and after the short outage (at {active} ms, marks={marks})")
    first_text = next((l["t"] for l in caps if l["caption"]), None)
    short_window = [l for l in caps if first_text is not None and first_text <= l["t"] < marks["stop2"] + 8000]
    blanks = [l["t"] for l in short_window if not l["caption"]]
    c.check(first_text is not None and first_text < marks["stop1"] and not blanks,
            f"a 3 s outage never blanks the caption (first text {first_text} ms, blanks at {blanks})")
    bad = caption_rewrites(short_window)
    c.check(not bad, f"across the reconnect the caption only appends/scrolls ({bad[:2]})")
    held = [l for l in caps if marks["stop1"] < l["t"] < marks["start1"]]
    before = [l for l in caps if l["t"] <= marks["stop1"]]
    c.check(not held or (before and held[-1]["caption"]),
            "during the outage the last committed text stays on screen")
    cleared = [l["t"] for l in caps if not l["caption"] and marks["stop2"] + 8000 <= l["t"] <= marks["stop2"] + 14000]
    c.check(bool(cleared), f"a long outage clears the frozen caption after ~10 s (cleared at {cleared}, "
                           f"server stopped at {round(marks['stop2'])} ms)")
    c.check(any(l["caption"] for l in caps if l["t"] > marks["start2"]), "captions come back after the long outage")
    return c, run


def sc_stable_tail_display_toggle(ctx) -> Checker:
    """Unstable tail + display-only settings changed mid-session.

    The OBS source turns "show not-yet-confirmed text" and the line count into
    plain caption-state calls (no reconnect). The driver makes the same calls
    5 s into a running session; the server must see exactly one session, and
    the committed text of every line must still only ever grow.
    """
    c = Checker("stable_tail_display_toggle")
    srv = ctx.server(["--revisable", "--growing-text"])
    run = ctx.drive(srv, srv.token_file, 14000, extra_args=("--tail", "off", "--toggle-display-at-ms", "5000"))
    toggle = run.event_time("display_toggle")
    starts = [r for r in run.requests if r["event"] == "ws_session_start"]
    accepts = [r for r in run.requests if r["event"] == "ws_accept"]
    c.check(toggle is not None, f"display settings were changed mid-session (at {toggle} ms)")
    c.check(len(starts) == 1 and len(accepts) == 1,
            f"one WebSocket and one session.start for the whole run, display change included "
            f"(accepts={len(accepts)}, starts={len(starts)})")
    c.check(bool(starts) and starts[0]["stable"] == {"agreement": 2}, "the session asked for stable captions")
    snaps = run.snapshots()
    before = [s for s in snaps if toggle is not None and s["t"] < toggle]
    after = [s for s in snaps if toggle is not None and s["t"] >= toggle]
    c.check(any(line["text"] for s in before for line in s["snapshot"]) and
            all(line["tail"] is None for s in before for line in s["snapshot"]),
            "before the toggle: committed text only, no tail")
    tails = [line["tail"] for s in after for line in s["snapshot"] if line["tail"]]
    c.check(bool(tails), f"after the toggle the not-yet-confirmed tail is shown ({len(tails)} tails)")
    partials = [r["text"] for r in run.requests if r["event"] == "ws_partial"]

    def from_a_partial(text: str, tail: str) -> bool:
        # The tail is what some partial has after as many characters as the
        # committed text (a prefix-matching partial is the common case);
        # the hypothesis' closing punctuation may be left out.
        return any(p[len(text):].startswith(tail) for p in partials)

    stray = sorted({(line["text"], line["tail"]) for s in after for line in s["snapshot"]
                    if line["tail"] and not from_a_partial(line["text"], line["tail"])})
    c.check(not stray, f"every tail comes from a partial the server sent ({stray[:3]})")
    shrunk = []
    shown_before: dict[str, str] = {}
    for s in after:
        for line in s["snapshot"]:
            shown = line["text"] + (line["tail"] or "")
            old = shown_before.get(line["key"])
            if old is not None and len(shown) < len(old) and old.startswith(shown):
                shrunk.append((old, shown))
            shown_before[line["key"]] = shown
    c.check(not shrunk, f"shown text (committed + tail) is never cut back ({shrunk[:2]})")
    committed = {r["text"] for r in run.requests if r["event"] in ("ws_stable", "ws_final")} | {""}
    bad_committed = sorted({line["text"] for s in snaps for line in s["snapshot"]} - committed)
    c.check(not bad_committed, f"committed text is only ever stable/final text, never a partial ({bad_committed[:3]})")
    rewrites = []
    seen: dict[str, str] = {}
    for s in snaps:
        for line in s["snapshot"]:
            old = seen.get(line["key"])
            if old is not None and not line["text"].startswith(old):
                rewrites.append((old, line["text"]))
            seen[line["key"]] = line["text"]
    c.check(not rewrites, f"per line, committed text only appends even while the tail changes ({rewrites[:2]})")
    caps_before = [l for l in run.captions() if toggle is not None and l["t"] < toggle]
    c.check(not caption_rewrites(caps_before), "rendered captions stay append-only")
    c.check(done_mismatches(run) == [0], f"no non-append stable values ({done_mismatches(run)})")
    run.notes.append(f"snapshots={len(snaps)} tails={len(tails)} partials={len(partials)}")
    return c, run


def segmentation_starts(run: Run) -> list:
    return [r.get("segmentation") for r in run.requests if r["event"] == "ws_session_start"]


def done_field(run: Run, name: str):
    return next((l.get(name) for l in run.lines if l.get("event") == "done"), None)


def sc_segmentation_absent(ctx) -> Checker:
    """Sentence break set, server without segmentation_control (every server
    that predates docs/04「切段控制」): the field is never sent."""
    c = Checker("segmentation_absent")
    srv = ctx.server(["--revisable", "--growing-text", "--hide-segmentation-capability"])
    run = ctx.drive(srv, srv.token_file, 8000, extra_args=("--end-silence-ms", "870"))
    segs = segmentation_starts(run)
    c.check(bool(segs) and all(sg is None for sg in segs),
            f"session.start carries no segmentation when the server does not advertise it ({segs})")
    c.check(run.any_status("session active") and not run.any_status("rejected"),
            "connects normally; nothing is reported as rejected")
    c.check(done_field(run, "segmentation_supported") == [False], "the client knows the server does not support it")
    c.check(any(l["caption"] for l in run.captions()), "captions rendered")
    return c, run


def sc_segmentation_emulated(ctx) -> Checker:
    """Server advertises segmentation_control (contract emulated around the
    current server app, see fake_asr_server.py --emulate-segmentation)."""
    c = Checker("segmentation_emulated")
    srv = ctx.server(["--revisable", "--growing-text", "--emulate-segmentation"])
    run = ctx.drive(srv, srv.token_file, 6000, extra_args=("--end-silence-ms", "870"))
    segs = segmentation_starts(run)
    c.check(segs[:1] == [{"end_silence_ms": 870}], f"the setting is sent as-is inside the range ({segs})")
    c.check(done_field(run, "end_silence_effective") == [870],
            f"the value in effect is read from session.started ({done_field(run, 'end_silence_effective')})")
    c.check(run.any_status("sentence break 870 ms"), "Tools status shows the sentence break in effect")
    clamped = ctx.drive(srv, srv.token_file, 5000, extra_args=("--end-silence-ms", "5000"))
    segs2 = segmentation_starts(clamped)[len(segs):]
    c.check(segs2[:1] == [{"end_silence_ms": 3000}], f"values above the advertised max are clamped ({segs2})")
    default = ctx.drive(srv, srv.token_file, 5000, extra_args=("--end-silence-ms", "0"))
    segs3 = segmentation_starts(default)[len(segs) + len(segs2):]
    c.check(bool(segs3) and all(sg is None for sg in segs3), f"0 = server default: no field ({segs3})")
    run.notes.append("wire contract only: the fake VAD does not segment differently")
    return c, run


def sc_segmentation_rejected(ctx) -> Checker:
    """Server advertises segmentation_control but refuses the field: reconnect
    with the server's default instead of failing."""
    c = Checker("segmentation_rejected")
    srv = ctx.server(["--revisable", "--growing-text", "--reject-segmentation-field"])
    run = ctx.drive(srv, srv.token_file, 12000, extra_args=("--end-silence-ms", "870"))
    segs = segmentation_starts(run)
    errs = [r for r in run.requests if r["event"] == "ws_error_event"]
    c.check(segs[:1] == [{"end_silence_ms": 870}], f"first session.start asks for it ({segs})")
    c.check(any(r["code"] == "protocol_error" for r in errs), f"server rejects it ({errs[:2]})")
    c.check(len(segs) >= 2 and segs[1] is None, f"next attempt drops only the segmentation field ({segs})")
    starts = [r for r in run.requests if r["event"] == "ws_session_start"]
    c.check(len(starts) >= 2 and starts[1]["stable"] == {"agreement": 2}, "stable captions are kept")
    c.check(run.any_status("rejected the sentence break"), "Tools status explains the fallback")
    c.check(run.any_status("session active"), "still connects")
    return c, run


def sc_segmentation_real(ctx) -> Checker:
    """Against the server in --service-dir as-is. Needs a server with
    docs/04「切段控制」; with an older one this documents that the field is
    not sent and the scenario passes as skipped."""
    c = Checker("segmentation_real")
    srv = ctx.server(["--revisable", "--growing-text"])
    run = ctx.drive(srv, srv.token_file, 6000, extra_args=("--end-silence-ms", "870"))
    supported = done_field(run, "segmentation_supported") == [True]
    segs = segmentation_starts(run)
    if not supported:
        c.check(bool(segs) and all(sg is None for sg in segs),
                f"SKIPPED (server without segmentation_control): field not sent ({segs})")
        run.notes.append("needs the server with session.start.segmentation to run for real")
        return c, run
    c.check(segs[:1] == [{"end_silence_ms": 870}], f"sent to a real supporting server ({segs})")
    c.check(done_field(run, "end_silence_effective") == [870], "the server reports 870 ms in effect")
    return c, run


# --------------------------------------------------------------------------- recognition hints

HINT_KEYS = {"profile", "domain", "hotwords", "replacements"}
HINTS_HOTWORDS = "聖靈\n# 註解\n\n以弗所書\r\n"
HINTS_REPLACEMENTS = "盛家 => 聖經\r\n聖家=>聖經\n恩點\t恩典\n# 註解 => 不算\n呃 =>\n"
HINTS_FILE = "\ufeff# 講道提示檔\n[專有詞]\n以弗所書\n哥林多前書\n[對照表]\n盛家 => 聖經課\n星際 => 聖經\n"
HINTS_DOMAIN = "  主日講道：以弗所書\n第二章  "
# what the plugin must send for the settings above: the file first, then the
# fields (hotwords unioned, a field pair overriding the file's by `from`)
HINTS_EXPECTED = {
    "profile": "church",
    "domain": "主日講道：以弗所書\n第二章",
    "hotwords": ["以弗所書", "哥林多前書", "聖靈"],
    "replacements": [{"from": "盛家", "to": "聖經"}, {"from": "星際", "to": "聖經"}, {"from": "聖家", "to": "聖經"},
                     {"from": "恩點", "to": "恩典"}, {"from": "呃", "to": ""}],
}


def hint_args(ctx, profile: str = "church", fetch_at_ms: int | None = 1500) -> tuple[str, ...]:
    (ctx.work / "hotwords.txt").write_text(HINTS_HOTWORDS, encoding="utf-8")
    (ctx.work / "replacements.txt").write_text(HINTS_REPLACEMENTS, encoding="utf-8")
    (ctx.work / "hints.txt").write_text(HINTS_FILE, encoding="utf-8")
    args = ["--hints-profile", profile, "--hints-domain", HINTS_DOMAIN,
            "--hints-hotwords-file", str(ctx.work / "hotwords.txt"),
            "--hints-replacements-file", str(ctx.work / "replacements.txt"),
            "--hints-file", str(ctx.work / "hints.txt")]
    if fetch_at_ms is not None:
        args += ["--fetch-dictionaries-at-ms", str(fetch_at_ms)]
    return tuple(args)


def hints_done(run: Run) -> dict:
    return (done_field(run, "hints") or [{}])[0]


def session_starts(run: Run) -> list[dict]:
    return [r for r in run.requests if r["event"] == "ws_session_start"]


def dictionary_lines(run: Run) -> list[dict]:
    return [l for l in run.lines if l.get("event") == "dictionaries"]


def no_hint_text_logged(run: Run) -> bool:
    words = ["聖靈", "以弗所書", "哥林多前書", "盛家", "聖經", "恩典", "主日講道"]
    return not any(w in line for w in words for line in run.log)


def sc_hints_on(ctx) -> Checker:
    """Server offers context_biasing: the hints go out in session.start,
    well-formed and equal to the settings (file merged first), the echo is
    read back, and the dictionary list is fetched."""
    c = Checker("hints_on")
    srv = ctx.server(["--revisable", "--growing-text", "--emulate-context"])
    run = ctx.drive(srv, srv.token_file, 8000, extra_args=hint_args(ctx))
    starts = session_starts(run)
    first = starts[0] if starts else {}
    context = first.get("context")
    c.check(hints_done(run).get("capability") == 3, f"the client saw context_biasing on ({hints_done(run)})")
    c.check(first.get("has_context") is True and isinstance(context, dict) and set(context) <= HINT_KEYS,
            f"session.start carries a context with only the contract's keys ({sorted(context or {})})")
    c.check(context == HINTS_EXPECTED, f"the context matches the settings, file merged first ({context})")
    c.check(not any(r["event"] == "ws_context_refused" for r in run.requests), "the server accepts it")
    c.check(len(starts) == 1 and first.get("stable") == {"agreement": 2}, "one session, stable captions kept")
    h = hints_done(run)
    c.check(h.get("applied") is True and h.get("applied_profile") == "church" and h.get("applied_hotwords") == 3
            and h.get("applied_replacements") == 5, f"session.started's echo is read back ({h})")
    c.check(run.any_status("recognition hints: 3 hotwords, 5 replacements"), "the Tools status shows it")
    dl = dictionary_lines(run)
    names = [e["name"] for e in (dl[0]["entries"] if dl else [])]
    c.check(bool(dl) and dl[0]["state"] == 2 and names == ["church", "youth"],
            f"GET /v1/dictionaries is fetched and parsed ({dl[:1]})")
    got = [r for r in run.requests if r["event"] == "http" and r["path"] == "/v1/dictionaries"]
    c.check(bool(got) and got[0]["has_auth"] and got[0]["status"] == 200, f"with the bearer token ({got[:1]})")
    c.check(run.log_lines("sending recognition hints: profile=yes domain=13 chars hotwords=3 replacements=5") != [],
            "the OBS log has the counts")
    c.check(no_hint_text_logged(run), "no hint text in the OBS log")
    return c, run


def sc_hints_limits(ctx) -> Checker:
    """Small advertised limits: the client cuts to them (never sends what the
    server would refuse), reports it, and logs a WARN with counts."""
    c = Checker("hints_limits")
    limits = {"max_domain_chars": 4, "max_hotwords": 2, "max_hotword_chars": 4,
              "max_replacements": 3, "max_replacement_chars": 2}
    srv = ctx.server(["--revisable", "--growing-text", "--emulate-context", "--context-limits", json.dumps(limits)])
    run = ctx.drive(srv, srv.token_file, 6000, extra_args=hint_args(ctx, fetch_at_ms=None))
    starts = session_starts(run)
    context = starts[0].get("context") if starts else None
    c.check(not any(r["event"] == "ws_context_refused" for r in run.requests),
            "what is sent fits the limits (the server accepts it)")
    c.check(bool(context) and context.get("domain") == "主日講道", f"the domain is cut to 4 characters ({context})")
    c.check(bool(context) and context.get("hotwords") == ["以弗所書", "聖靈"],
            f"hotwords longer than 4 chars dropped, then cut to 2 ({(context or {}).get('hotwords')})")
    c.check(bool(context) and [p["from"] for p in context.get("replacements", [])] == ["盛家", "星際", "聖家"],
            f"replacements cut to the first 3 ({(context or {}).get('replacements')})")
    h = hints_done(run)
    c.check(h.get("domain_cut") == 9 and h.get("too_long") == 1 and h.get("replacements_over") == 2,
            f"the client reports what it cut ({h})")
    warn = run.log_lines("WARN recognition hints cut to the server's limits")
    c.check(len(warn) == 1 and "domain 4 of 13 chars" in warn[0], f"one WARN with counts ({warn})")
    c.check(no_hint_text_logged(run), "no hint text in the OBS log")
    return c, run


def sc_hints_rejected(ctx) -> Checker:
    """The server refuses the context (an unknown profile): reconnect without
    it, keep every other setting, and say why."""
    c = Checker("hints_rejected")
    srv = ctx.server(["--revisable", "--growing-text", "--emulate-context"])
    run = ctx.drive(srv, srv.token_file, 12000, extra_args=hint_args(ctx, profile="nosuch", fetch_at_ms=None))
    starts = session_starts(run)
    refused = [r for r in run.requests if r["event"] == "ws_context_refused"]
    errs = [r for r in run.requests if r["event"] == "ws_error_event"]
    c.check(bool(starts) and starts[0].get("has_context") is True, "the first session.start carries the context")
    c.check(bool(refused) and refused[0]["why"] == "unknown profile" and bool(errs),
            f"the server refuses it ({refused[:1]}, {[(e['code']) for e in errs[:1]]})")
    c.check(len(starts) >= 2 and starts[1].get("has_context") is False, "the next attempt goes without it")
    c.check(len(starts) >= 2 and starts[1].get("stable") == {"agreement": 2}, "stable captions are kept")
    c.check(run.any_status("rejected the recognition hints"), "the status says why")
    c.check(run.any_status("session active"), "and the session runs")
    h = hints_done(run)
    c.check(h.get("rejected") is True and h.get("reject_reason", "") != "", f"the hints status has the reason ({h})")
    c.check(len(run.log_lines("WARN server rejected the recognition hints")) == 1, "one WARN in the OBS log")
    return c, run


def sc_hints_off(ctx) -> Checker:
    """Against the server in --service-dir as-is (context_biasing off or not
    known): hints configured, but no context is ever sent and nothing breaks."""
    c = Checker("hints_off")
    srv = ctx.server(["--revisable", "--growing-text"])
    run = ctx.drive(srv, srv.token_file, 6000, extra_args=hint_args(ctx))
    starts = session_starts(run)
    cap = hints_done(run).get("capability")
    if cap == 3:
        c.check(True, "SKIPPED: this server has context_biasing on (hints_on covers it)")
        run.notes.append("the server in --service-dir enables context_biasing")
        return c, run
    c.check(cap in (1, 2), f"the client saw the feature off or absent ({cap})")
    c.check(bool(starts) and all(r.get("has_context") is False for r in starts),
            f"no context field is sent ({[r.get('has_context') for r in starts]})")
    c.check(run.any_status("session active"), "the session runs")
    dl = dictionary_lines(run)
    c.check(bool(dl) and dl[0]["state"] in (2, 3), f"a dictionary request still ends, never hangs ({dl[:1]})")
    return c, run


def sc_hints_absent(ctx) -> Checker:
    """A server that predates the feature (no context_biasing field)."""
    c = Checker("hints_absent")
    srv = ctx.server(["--revisable", "--growing-text", "--hide-context-capability"])
    run = ctx.drive(srv, srv.token_file, 5000, extra_args=hint_args(ctx, fetch_at_ms=None))
    starts = session_starts(run)
    c.check(hints_done(run).get("capability") == 1, f"the client saw the feature absent ({hints_done(run)})")
    c.check(bool(starts) and all(r.get("has_context") is False for r in starts), "no context field is sent")
    c.check(run.any_status("session active"), "the session runs")
    return c, run


# The real server implementation (TEA_ASR_CONTEXT_HINTS=1, no emulation). A
# server dictionary in the harness's support dir; the fake backend's
# --growing-text sentence contains 公園 and 散步, so the replacements show up
# in real finals.
REAL_DICTIONARY = """domain = "主日講道"
hotwords = ["聖經", "以弗所書"]

[[replacements]]
from = "公園"
to = "花園"
"""
REAL_FIELD_REPLACEMENTS = "散步 => 散心\n"


def real_hints_server(ctx, prompt: bool = False) -> FakeServer:
    dictionaries = ctx.work / "state" / "dictionaries"
    dictionaries.mkdir(parents=True, exist_ok=True)
    (dictionaries / "church.toml").write_text(REAL_DICTIONARY, encoding="utf-8")
    env = {"TEA_ASR_CONTEXT_HINTS": "1", "TEA_ASR_CONTEXT_PROMPT": "1" if prompt else "0"}
    return ctx.server(["--revisable", "--growing-text"], env=env)


def real_hint_args(ctx, profile: str) -> tuple[str, ...]:
    (ctx.work / "real-hotwords.txt").write_text("聖靈\n", encoding="utf-8")
    (ctx.work / "real-replacements.txt").write_text(REAL_FIELD_REPLACEMENTS, encoding="utf-8")
    return ("--hints-profile", profile, "--hints-domain", "主日講道：以弗所書",
            "--hints-hotwords-file", str(ctx.work / "real-hotwords.txt"),
            "--hints-replacements-file", str(ctx.work / "real-replacements.txt"),
            "--fetch-dictionaries-at-ms", "1500")


def skip_without_real_hints(c: Checker, run: Run) -> bool:
    if hints_done(run).get("capability") == 3:
        return False
    c.check(True, f"SKIPPED: the server in --service-dir has no recognition hints ({hints_done(run)})")
    run.notes.append("needs the server with TEA_ASR_CONTEXT_HINTS (agent/context-hints) to run for real")
    return True


def sc_hints_real(ctx) -> Checker:
    """Against the real implementation: capability and limits, the real
    /v1/dictionaries shape, the context accepted, the echo (prompt_applied),
    and replacements applied in the finals."""
    c = Checker("hints_real")
    srv = real_hints_server(ctx)
    run = ctx.drive(srv, srv.token_file, 12000, extra_args=real_hint_args(ctx, "church"))
    if skip_without_real_hints(c, run):
        return c, run
    h = hints_done(run)
    c.check(h.get("max_hotwords") == 200, f"the advertised limits are read ({h})")
    dl = dictionary_lines(run)
    c.check(bool(dl) and dl[0]["state"] == 2 and
            dl[0]["entries"] == [{"name": "church", "hotwords_count": 2, "replacements_count": 1}],
            f"the real /v1/dictionaries answer is parsed ({dl[:1]})")
    starts = session_starts(run)
    c.check(len(starts) == 1 and starts[0].get("context") == {
        "profile": "church", "domain": "主日講道：以弗所書", "hotwords": ["聖靈"],
        "replacements": [{"from": "散步", "to": "散心"}]}, f"the context sent ({starts[:1]})")
    errs = [r for r in run.requests if r["event"] == "ws_error_event"]
    c.check(not errs and run.any_status("session active"), f"the real server accepts it ({errs[:1]})")
    echo = next((r.get("context") for r in run.requests if r["event"] == "ws_session_started"), None)
    c.check(echo == {"profile": "church", "domain_chars": 9, "hotwords_count": 3, "replacements_count": 2,
                     "prompt_applied": False}, f"session.started.context: profile merged first ({echo})")
    c.check(h.get("applied") is True and h.get("applied_hotwords") == 3 and h.get("applied_replacements") == 2
            and h.get("prompt_applied") is False, f"the client reads the echo back ({h})")
    finals = [r for r in run.requests if r["event"] == "ws_final"]
    replaced = [r for r in finals if "花園" in (r.get("text") or "")]
    c.check(bool(replaced) and all("公園" not in (r.get("text") or "") for r in finals),
            f"the profile's replacement is applied to finals ({[r.get('text') for r in finals][:3]})")
    c.check(any("公園" in (r.get("raw_text") or "") for r in replaced), "raw_text keeps what was heard")
    c.check(any("replacements_applied" in (r.get("warnings") or []) for r in replaced),
            "finals carry the replacements_applied warning")
    c.check(bool(replaced) and all("花園散心" in (r.get("text") or "") for r in replaced),
            "the field's replacement is applied too, merged with the profile's")
    shown = [l.get("caption", "") for l in run.captions()]
    c.check(any("花園" in t for t in shown) and not any("公園" in t for t in shown),
            "the plugin shows the replaced text, never the heard one")
    c.check(done_field(run, "stable_mismatches") == [0], "replaced previews keep the stable contract")
    c.check(no_hint_text_logged(run), "no hint text in the OBS log")
    return c, run


def sc_hints_real_prompt(ctx) -> Checker:
    """Real server with TEA_ASR_CONTEXT_PROMPT=1: the echo says the prompt is applied."""
    c = Checker("hints_real_prompt")
    srv = real_hints_server(ctx, prompt=True)
    run = ctx.drive(srv, srv.token_file, 6000, extra_args=real_hint_args(ctx, "church"))
    if skip_without_real_hints(c, run):
        return c, run
    echo = next((r.get("context") for r in run.requests if r["event"] == "ws_session_started"), None)
    c.check(bool(echo) and echo.get("prompt_applied") is True, f"prompt_applied=true ({echo})")
    c.check(hints_done(run).get("prompt_applied") is True, f"the client reads it ({hints_done(run)})")
    c.check(run.any_status("session active") and bool([r for r in run.requests if r["event"] == "ws_final"]),
            "the session runs and finals arrive with the prompt")
    return c, run


def sc_hints_real_unknown_profile(ctx) -> Checker:
    """Real server, a profile it does not have: its real rejection is
    attributed to the context and the client reconnects without it."""
    c = Checker("hints_real_unknown_profile")
    srv = real_hints_server(ctx)
    run = ctx.drive(srv, srv.token_file, 12000, extra_args=real_hint_args(ctx, "nosuch"))
    if skip_without_real_hints(c, run):
        return c, run
    starts = session_starts(run)
    errs = [r for r in run.requests if r["event"] == "ws_error_event"]
    closes = [r.get("close_code") for r in run.requests if r["event"] == "ws_close"]
    c.check(bool(starts) and starts[0].get("has_context") is True, "the first session.start carries the context")
    c.check(bool(errs) and errs[0]["code"] == "unsupported_option", f"the real rejection ({errs[:1]}, close {closes[:1]})")
    c.check(len(starts) >= 2 and starts[1].get("has_context") is False and starts[1].get("stable") == {"agreement": 2},
            "the next attempt goes without the context, other settings kept")
    h = hints_done(run)
    c.check(h.get("rejected") is True and "unsupported_option" in h.get("reject_reason", ""),
            f"attributed to the context, with the server's reason ({h.get('reject_reason')})")
    c.check(run.any_status("session active"), "and the session runs")
    return c, run


# --------------------------------------------------------------------------- singing / pause

TAG = re.compile(r"§(\d+)")


def caption_tags(run: Run, t_from: float = -1, t_to: float = 1e12) -> set[int]:
    """Segment tags (--tag-segments) in what the plugin renders, in (t_from, t_to]."""
    out: set[int] = set()
    for l in run.captions():
        if t_from < l["t"] <= t_to:
            out |= {int(m) for m in TAG.findall(l.get("caption", ""))}
    return out


def snapshot_tags(run: Run, t_to: float = 1e12) -> set[int]:
    """Tags of every snapshot line (hidden ones too) up to t_to."""
    out: set[int] = set()
    for s in run.snapshots():
        if s["t"] <= t_to:
            for line in s["snapshot"]:
                out |= {int(m) for m in TAG.findall(line["text"] + (line["tail"] or ""))}
    return out


def audio_class_events(run: Run, emulated: bool | None = None) -> list[dict]:
    """Labels the client got (scripted ones, or the real server's)."""
    return [r for r in run.requests if r["event"] == "ws_audio_class" and not r.get("dropped")
            and (emulated is None or r.get("emulated") == emulated)]


SINGING_SERVER = ["--revisable", "--growing-text", "--tag-segments"]


def sc_singing_auto(ctx) -> Checker:
    """Automatic: a segment labelled singing before any text never shows; one
    labelled after its first words were shown is hidden from then on; speech
    shows as always."""
    c = Checker("singing_auto")
    srv = ctx.server(SINGING_SERVER + ["--emulate-singing", "1:singing@0,2:singing@3"])
    run = ctx.drive(srv, srv.token_file, 18000, extra_args=("--tail", "on"))
    c.check(done_field(run, "singing_supported") == [True], "the client saw singing_detection")
    labels = {(r["segment_index"], r["class"]) for r in audio_class_events(run, True)}
    c.check({(1, "singing"), (2, "singing")} <= labels, f"the server labelled segments 1 and 2 singing ({sorted(labels)})")
    shown = caption_tags(run)
    c.check(1 not in shown, f"segment 1 (singing before any text) is never shown ({sorted(shown)})")
    c.check(0 in shown and 3 in shown, f"speech segments are shown ({sorted(shown)})")
    last = run.captions()[-1]["caption"] if run.captions() else ""
    c.check("§2" not in last and "§1" not in last, f"no sung text on screen at the end ({last!r})")
    hidden2 = [line for s in run.snapshots() for line in s["snapshot"]
               if "§2" in line["text"] + (line["tail"] or "") and line["hidden"]]
    c.check(bool(hidden2), "segment 2 is flagged hidden once labelled (the display fades it out)")
    c.check(done_field(run, "singing_segments") == [2], f"two singing segments ({done_field(run, 'singing_segments')})")
    c.check(len([r for r in run.requests if r["event"] == "ws_accept"]) == 1, "no reconnect")
    return c, run


def sc_singing_show(ctx) -> Checker:
    """'Always show': the same singing segments are shown."""
    c = Checker("singing_show")
    srv = ctx.server(SINGING_SERVER + ["--emulate-singing", "1:singing@0"])
    run = ctx.drive(srv, srv.token_file, 12000, extra_args=("--tail", "on", "--singing", "show"))
    shown = caption_tags(run)
    c.check(done_field(run, "singing_supported") == [True] and 1 in shown,
            f"segment 1 is labelled singing and shown anyway ({sorted(shown)})")
    c.check(not any(line["hidden"] for s in run.snapshots() for line in s["snapshot"]), "nothing is hidden")
    return c, run


def sc_singing_revision(ctx) -> Checker:
    """A revision back to speech shows the segment, like a new line."""
    c = Checker("singing_revision")
    srv = ctx.server(SINGING_SERVER + ["--emulate-singing", "1:singing@0+speech@4"])
    run = ctx.drive(srv, srv.token_file, 12000, extra_args=("--tail", "on"))
    revs = [(r["class"], r["revision"]) for r in audio_class_events(run, True) if r["segment_index"] == 1]
    c.check(revs[:2] == [("singing", 0), ("speech", 1)], f"labelled singing, then revised to speech ({revs})")
    states = [line["hidden"] for s in run.snapshots() for line in s["snapshot"]
              if "§1" in line["text"] + (line["tail"] or "")]
    first_shown = states.index(False) if False in states else -1
    c.check(bool(states) and states[0] is True and first_shown > 0,
            f"hidden first, shown after the revision ({states[:3]} ... )")
    c.check(1 in caption_tags(run), "segment 1 is rendered after the revision")
    return c, run


def sc_singing_absent(ctx) -> Checker:
    """A server without singing_detection (an older one, or one missing the
    YAMNet asset): 'automatic' shows everything."""
    c = Checker("singing_absent")
    srv = ctx.server(SINGING_SERVER + ["--hide-singing-capability"])
    run = ctx.drive(srv, srv.token_file, 12000, extra_args=("--tail", "on"))
    c.check(done_field(run, "singing_supported") == [False], "the client saw no singing_detection")
    shown = caption_tags(run)
    c.check({0, 1} <= shown, f"every segment is shown ({sorted(shown)})")
    c.check(not any(line["hidden"] for s in run.snapshots() for line in s["snapshot"]), "nothing is hidden")
    return c, run


def sc_pause_resume(ctx) -> Checker:
    """Manual pause (hotkey / button) hides everything at once without
    touching the session; resuming shows only segments that start after it."""
    c = Checker("pause_resume")
    srv = ctx.server(SINGING_SERVER)
    run = ctx.drive(srv, srv.token_file, 18000,
                    extra_args=("--tail", "on", "--pause-at-ms", "5000", "--resume-at-ms", "10000"))
    t_pause, t_resume = run.event_time("pause"), run.event_time("resume")
    c.check(t_pause is not None and t_resume is not None, f"paused at {t_pause} ms, resumed at {t_resume} ms")
    # the render changes in the same poll as the pause (same ms) and stays empty
    during = [l["caption"] for l in run.captions() if t_pause is not None and t_pause <= l["t"] <= (t_resume or 0)]
    c.check(bool(during) and all(text == "" for text in during), f"nothing is rendered while paused ({during[:3]})")
    before_resume = snapshot_tags(run, t_resume or 0)
    after = caption_tags(run, t_resume or 0)
    c.check(bool(after) and not (after & before_resume),
            f"after the resume only new segments show ({sorted(after)}; known before: {sorted(before_resume)})")
    c.check(len([r for r in run.requests if r["event"] == "ws_accept"]) == 1 and len(session_starts(run)) == 1,
            "the session never reconnects for the pause")
    finals = [r for r in run.requests if r["event"] == "ws_final"]
    c.check(len(finals) >= 3, f"recognition kept running through the pause ({len(finals)} finals)")
    return c, run


def sc_singing_real(ctx) -> Checker:
    """The real implementation (on by default when the YAMNet asset is
    there; TEA_ASR_SINGING_DETECTION=1 forces the setting on): its
    segment.audio_class events are read and singing segments are hidden.
    Skipped when the server in --service-dir does not have the feature."""
    c = Checker("singing_real")
    srv = ctx.server(SINGING_SERVER, env={"TEA_ASR_SINGING_DETECTION": "1"})
    run = ctx.drive(srv, srv.token_file, 14000, extra_args=("--tail", "on"))
    if done_field(run, "singing_supported") != [True]:
        c.check(True, "SKIPPED: the server in --service-dir has no singing detection")
        run.notes.append("needs the server with TEA_ASR_SINGING_DETECTION (agent/singing-detect) to run for real")
        return c, run
    events = audio_class_events(run, False)
    c.check(bool(events), f"the real server sends segment.audio_class ({events[:2]})")
    ever_singing = {r["segment_index"] for r in events if r["class"] == "singing"}
    c.check(done_field(run, "singing_segments") == [len(ever_singing)],
            f"the client counted the segments labelled singing ({sorted(ever_singing)})")
    last: dict[int, str] = {}
    for r in events:
        last[r["segment_index"]] = r["class"]
    sung = {i for i, cls in last.items() if cls == "singing"}
    final_caption = run.captions()[-1]["caption"] if run.captions() else ""
    on_screen = {int(m) for m in TAG.findall(final_caption)}
    c.check(not (on_screen & sung), f"no segment labelled singing is on screen at the end (labels {last}, "
            f"shown {sorted(on_screen)})")
    return c, run


# A private recording of congregational singing (not in git): 16 kHz mono WAV.
MUSIC_WAV = Path(os.environ.get("TEA_E2E_MUSIC_WAV",
                                "/Users/c2leb/Codes/tea-asr-service/.soak/audio/music-0123.wav"))


def sc_singing_real_music(ctx) -> Checker:
    """The real detector on real singing: the plugin plays a recording of a
    congregation singing; the server's own YAMNet labels decide what shows.
    Skipped without the recording or without the feature."""
    c = Checker("singing_real_music")
    if not MUSIC_WAV.exists():
        c.check(True, f"SKIPPED: no singing recording at {MUSIC_WAV} (set TEA_E2E_MUSIC_WAV)")
        return c, Run(lines=[], requests=[], notes=["needs a singing recording"])
    srv = ctx.server(SINGING_SERVER)
    run = ctx.drive(srv, srv.token_file, 40000, audio=f"file:{MUSIC_WAV}", extra_args=("--tail", "on"))
    if done_field(run, "singing_supported") != [True]:
        c.check(True, "SKIPPED: the server in --service-dir has no singing detection")
        run.notes.append("needs the server with singing detection (YAMNet asset) to run for real")
        return c, run
    events = audio_class_events(run, False)
    last: dict[int, str] = {}
    for r in events:
        last[r["segment_index"]] = r["class"]
    sung = {i for i, cls in last.items() if cls == "singing"}
    c.check(bool(sung), f"the real detector labels the singing ({last})")
    final_caption = run.captions()[-1]["caption"] if run.captions() else ""
    on_screen = {int(m) for m in TAG.findall(final_caption)}
    c.check(bool(sung) and not (on_screen & sung), f"no sung segment is on screen at the end (shown {sorted(on_screen)})")
    hidden = {int(m) for s in run.snapshots() for line in s["snapshot"] if line["hidden"]
              for m in TAG.findall(line["text"] + (line["tail"] or ""))}
    c.check(sung <= hidden, f"every sung segment is flagged hidden ({sorted(hidden)})")
    return c, run


# --------------------------------------------------------------------------- diagnostics

HEARTBEAT = "asr-client: heartbeat"
WARN_NO_AUDIO = "WARN no audio sent"
WARN_QUIET = "WARN audio is being sent but has been below"
WARN_NO_SPEECH = "server hears no speech"
WARN_STALLED = "WARN sending audio but no event from the server"
ALL_WARNS = (WARN_NO_AUDIO, WARN_QUIET, WARN_NO_SPEECH, WARN_STALLED)


def warn_counts(run: Run) -> dict[str, int]:
    return {w: len(run.log_lines(w)) for w in ALL_WARNS}


def sc_diag_heartbeat(ctx) -> Checker:
    """Normal speech: connection lines, one heartbeat per 10 s, no warnings,
    and speech.started / segment.queued are no longer logged as unhandled."""
    c = Checker("diag_heartbeat")
    srv = ctx.server(["--revisable", "--growing-text"])
    run = ctx.drive(srv, srv.token_file, 23000)
    beats = run.log_lines(HEARTBEAT)
    c.check(len(beats) == 2, f"one heartbeat line per 10 s in a 23 s run ({len(beats)})")
    c.check(bool(beats) and all("ms audio in" in b and "dBFS" in b and "audio.ack=" in b and "speech.started=" in b
                                and "last text" in b and "max gap" in b for b in beats),
            "heartbeat: audio sent, longest gap, RMS/peak dBFS, events by type, time since the last text")
    level_ok = any(" rms -1" in b for b in beats)  # the fake tone: 0.3 amplitude, 2/3 duty -> about -15 dBFS
    c.check(level_ok, f"heartbeat level matches the fake tone ({beats[:1]})")
    c.check(bool(run.log_lines("connection connecting")) and bool(run.log_lines("connection connected"))
            and bool(run.log_lines("connection session active")),
            "connection state lines: connecting, connected, session active")
    active = run.log_lines("connection session active")
    c.check(bool(active) and '"type":"session.start"' in active[0] and "endpoint_silence_ms=" in active[0],
            "session start line shows the session.start actually sent and the server's parameters")
    c.check(not run.log_lines("unhandled event type 'speech.started'")
            and not run.log_lines("unhandled event type 'segment.queued'"),
            "speech.started / segment.queued are handled, not logged as unhandled")
    c.check(sum(warn_counts(run).values()) == 0, f"no warnings for normal speech ({warn_counts(run)})")
    run.notes.append(beats[0].split("] ", 1)[-1] if beats else "no heartbeat")
    return c, run


def sc_diag_silence(ctx) -> Checker:
    c = Checker("diag_silence")
    srv = ctx.server(["--revisable"])
    run = ctx.drive(srv, srv.token_file, 16000, audio="silence")
    counts = warn_counts(run)
    c.check(counts[WARN_QUIET] == 1, f"silent audio: one below -60 dBFS warning, rate limited ({counts})")
    c.check(counts[WARN_NO_AUDIO] == 0 and counts[WARN_NO_SPEECH] == 0 and counts[WARN_STALLED] == 0,
            f"no other warning ({counts})")
    beats = run.log_lines(HEARTBEAT)
    c.check(bool(beats) and "rms -120.0 dBFS" in beats[0], f"heartbeat shows digital silence ({beats[:1]})")
    run.notes.append(run.log_lines(WARN_QUIET)[0].split("] ", 1)[-1] if run.log_lines(WARN_QUIET) else "-")
    return c, run


def sc_diag_no_audio(ctx) -> Checker:
    c = Checker("diag_no_audio")
    srv = ctx.server(["--revisable"])
    run = ctx.drive(srv, srv.token_file, 38000, audio="dead")
    counts = warn_counts(run)
    warns = run.log_lines(WARN_NO_AUDIO)
    c.check(counts[WARN_NO_AUDIO] == 2, f"no audio: warned after 3 s and again 30 s later, not more ({counts})")
    c.check(counts[WARN_QUIET] == 0 and counts[WARN_NO_SPEECH] == 0 and counts[WARN_STALLED] == 0,
            f"no other warning ({counts})")
    beats = run.log_lines(HEARTBEAT)
    c.check(bool(beats) and "sent 0 ms audio in 0 frames" in beats[0], f"heartbeat shows nothing sent ({beats[:1]})")
    run.notes.append(warns[0].split("] ", 1)[-1] if warns else "-")
    return c, run


def sc_diag_no_speech(ctx) -> Checker:
    c = Checker("diag_no_speech")
    srv = ctx.server(["--revisable", "--deaf"])
    run = ctx.drive(srv, srv.token_file, 30000)
    counts = warn_counts(run)
    dropped = [r for r in run.requests if r["event"] == "ws_dropped_by_deaf"]
    c.check(counts[WARN_NO_SPEECH] == 1, f"audible audio, server reports no speech: one warning ({counts})")
    c.check(counts[WARN_NO_AUDIO] == 0 and counts[WARN_QUIET] == 0 and counts[WARN_STALLED] == 0,
            f"no other warning ({counts})")
    c.check(not any(r["type"] == "audio.ack" for r in dropped), "acks still flow (so it is not a stall)")
    beats = run.log_lines(HEARTBEAT)
    c.check(bool(beats) and "speech.started=0" in beats[-1] and "last text none yet" in beats[-1],
            f"heartbeat: audio sent, no speech.started, no text ({beats[-1:]})")
    run.notes.append(run.log_lines(WARN_NO_SPEECH)[0].split("] ", 1)[-1] if run.log_lines(WARN_NO_SPEECH) else "-")
    return c, run


def sc_diag_trace(ctx) -> Checker:
    import shutil
    c = Checker("diag_trace")
    srv = ctx.server(["--revisable", "--growing-text"])
    off_dir = ctx.work / "trace-off"
    on_dir = ctx.work / "trace-on"
    for d in (off_dir, on_dir):
        shutil.rmtree(d, ignore_errors=True)
    ctx.drive(srv, srv.token_file, 5000)
    c.check(not off_dir.exists() and not on_dir.exists(), "no trace file when the setting is off")
    run = ctx.drive(srv, srv.token_file, 9000, extra_args=("--trace-dir", str(on_dir)))
    files = sorted(on_dir.glob("tea-trace-*.jsonl")) if on_dir.exists() else []
    c.check(len(files) == 1, f"one trace file per session ({[f.name for f in files]})")
    logged = run.log_lines("event trace for session")
    c.check(bool(files) and bool(logged) and str(files[0]) in logged[0], "the trace path is logged at session start")
    events = []
    valid = bool(files)
    for line in (files[0].read_text().splitlines() if files else []):
        try:
            obj = json.loads(line)
        except ValueError:
            valid = False
            continue
        if "event" in obj:
            valid = valid and isinstance(obj.get("t_ms"), (int, float))
            events.append(obj["event"].get("type"))
    c.check(valid, "every line is JSON with a numeric t_ms")
    c.check(events[:2] == ["hello", "session.started"] and "transcript.stable" in events
            and "transcript.final" in events and "audio.ack" in events,
            f"it holds the whole event timeline ({sorted(set(events))})")
    replay = ctx.driver.parent / "caption-replay"
    ok = False
    if files and replay.exists():
        out = subprocess.run([str(replay), str(files[0]), "--quiet"], capture_output=True, text=True, timeout=60)
        ok = out.returncode == 0 and "# summary:" in out.stdout
        run.notes.append(out.stdout.strip().splitlines()[-1][:160] if out.stdout.strip() else "no replay output")
    c.check(ok, "tests/replay reads it")
    return c, run


SCENARIOS = {
    "happy": sc_happy,
    "final_only": sc_final_only,
    "heartbeat": sc_heartbeat_long_session,
    "no_audio": sc_no_audio_source,
    "host": sc_host_rejected,
    "revoked": sc_revoked_then_rotated,
    "rotation": sc_rotation_mid_session,
    "reconnect_all": sc_reconnect_all_then_server_restart,
    "idle": sc_idle_timeout,
    "cap": sc_connection_cap,
    "concurrent": sc_concurrent_limit,
    "bad_token": sc_bad_token,
    "rate_limited": sc_rate_limited,
    "stable": sc_stable_append_only,
    "stable_fallback": sc_stable_fallback_no_capability,
    "stable_rejected": sc_stable_rejected_downgrade,
    "stable_hold": sc_stable_reconnect_hold,
    "stable_tail": sc_stable_tail_display_toggle,
    "segmentation_absent": sc_segmentation_absent,
    "segmentation_emulated": sc_segmentation_emulated,
    "segmentation_rejected": sc_segmentation_rejected,
    "segmentation_real": sc_segmentation_real,
    "hints_on": sc_hints_on,
    "hints_limits": sc_hints_limits,
    "hints_rejected": sc_hints_rejected,
    "hints_off": sc_hints_off,
    "hints_absent": sc_hints_absent,
    "hints_real": sc_hints_real,
    "hints_real_prompt": sc_hints_real_prompt,
    "hints_real_unknown_profile": sc_hints_real_unknown_profile,
    "singing_auto": sc_singing_auto,
    "singing_show": sc_singing_show,
    "singing_revision": sc_singing_revision,
    "singing_absent": sc_singing_absent,
    "pause_resume": sc_pause_resume,
    "singing_real": sc_singing_real,
    "singing_real_music": sc_singing_real_music,
    "diag_heartbeat": sc_diag_heartbeat,
    "diag_silence": sc_diag_silence,
    "diag_no_audio": sc_diag_no_audio,
    "diag_no_speech": sc_diag_no_speech,
    "diag_trace": sc_diag_trace,
}


class Ctx:
    def __init__(self, driver: Path, service_dir: Path, work: Path):
        self.driver = driver
        self.service_dir = service_dir
        self.work = work
        self._servers: list[FakeServer] = []

    def server(self, extra: list[str], env: dict | None = None) -> FakeServer:
        srv = FakeServer(self.work, free_port(), self.service_dir, extra, env)
        srv.start()
        self._servers.append(srv)
        return srv

    def drive(self, srv: FakeServer, token: Path, duration_ms: int, **kw) -> Run:
        lines, log = run_driver(self.driver, srv.port, token, duration_ms, self.work, **kw)
        return Run(lines=lines, requests=srv.requests(), log=log)

    def close(self) -> None:
        for s in self._servers:
            s.stop()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--driver", required=True, type=Path)
    ap.add_argument("--service-dir", type=Path, default=DEFAULT_SERVICE_DIR)
    ap.add_argument("--only", default="")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--keep", type=Path, default=None, help="directory to keep per-scenario logs in")
    args = ap.parse_args()
    if args.list:
        print("\n".join(SCENARIOS))
        return 0
    if not (args.service_dir / ".venv" / "bin" / "python").exists():
        print(f"SKIP: no tea-asr-service venv at {args.service_dir}", file=sys.stderr)
        return 0
    names = [n for n in args.only.split(",") if n] or list(SCENARIOS)
    base = args.keep or Path(tempfile.mkdtemp(prefix="tea-e2e-"))
    base.mkdir(parents=True, exist_ok=True)
    failed = []
    for name in names:
        work = base / name
        work.mkdir(parents=True, exist_ok=True)
        for f in work.glob("*"):
            if f.is_file():
                f.unlink()
        print(f"== {name}")
        ctx = Ctx(args.driver.resolve(), args.service_dir, work)
        try:
            checker, run = SCENARIOS[name](ctx)
        finally:
            ctx.close()
        for n in run.notes:
            print(f"    note: {n}")
        if checker.failures:
            failed.append(name)
        print(f"   {'PASS' if not checker.failures else 'FAIL'} {name} "
              f"({len(checker.passes)} ok, {len(checker.failures)} failed); logs in {work}")
    print(f"\nsummary: {len(names) - len(failed)}/{len(names)} scenarios passed" +
          (f"; failed: {', '.join(failed)}" if failed else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
