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
    def __init__(self, work: Path, port: int, service_dir: Path, extra: list[str]):
        self.work = work
        self.port = port
        self.service_dir = service_dir
        self.extra = extra
        self.state = work / "state"
        self.request_log = work / "requests.jsonl"
        self.proc: subprocess.Popen | None = None

    def start(self) -> None:
        python = self.service_dir / ".venv" / "bin" / "python"
        env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
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
    with (work / "driver.stderr").open("a") as err:
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
    return lines


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
    partials = {r["text"] for r in run.requests if r["event"] == "ws_partial"}
    stray = sorted({line["text"] + line["tail"] for s in after for line in s["snapshot"] if line["tail"]} - partials)
    c.check(not stray, f"every committed text + tail is exactly a partial the server sent ({stray[:3]})")
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
}


class Ctx:
    def __init__(self, driver: Path, service_dir: Path, work: Path):
        self.driver = driver
        self.service_dir = service_dir
        self.work = work
        self._servers: list[FakeServer] = []

    def server(self, extra: list[str]) -> FakeServer:
        srv = FakeServer(self.work, free_port(), self.service_dir, extra)
        srv.start()
        self._servers.append(srv)
        return srv

    def drive(self, srv: FakeServer, token: Path, duration_ms: int, **kw) -> Run:
        lines = run_driver(self.driver, srv.port, token, duration_ms, self.work, **kw)
        return Run(lines=lines, requests=srv.requests())

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
