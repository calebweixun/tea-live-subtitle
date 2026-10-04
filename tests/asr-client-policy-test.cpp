#include "asr-error-policy.hpp"
#include "asr-diagnostics.h"
#include "font-default-policy.h"

#include <cmath>
#include <cstdlib>
#include <vector>
#include <cstring>
#include <iostream>
#include <string>

namespace {

void expect(bool condition, const char *message)
{
	if (!condition) {
		std::cerr << "FAIL: " << message << '\n';
		std::exit(EXIT_FAILURE);
	}
}

} // namespace

static bool near_db(double a, double b)
{
	return std::fabs(a - b) < 0.05;
}

static void test_dbfs()
{
	tea_pcm_level_t level;
	tea_pcm_level_reset(&level);
	expect(tea_pcm_level_rms_dbfs(&level) == TEA_DBFS_FLOOR, "no samples: silence floor");
	std::vector<int16_t> zeros(1600, 0);
	tea_pcm_level_add(&level, zeros.data(), zeros.size());
	expect(tea_pcm_level_rms_dbfs(&level) == TEA_DBFS_FLOOR && tea_pcm_level_peak_dbfs(&level) == TEA_DBFS_FLOOR,
	       "digital silence is -120 dBFS, not -inf");

	/* full-scale square wave: 0 dBFS RMS and peak */
	tea_pcm_level_reset(&level);
	std::vector<int16_t> square(1600);
	for (size_t i = 0; i < square.size(); i++)
		square[i] = (i / 8) % 2 ? 32767 : -32768;
	tea_pcm_level_add(&level, square.data(), square.size());
	expect(near_db(tea_pcm_level_peak_dbfs(&level), 0.0) && tea_pcm_level_rms_dbfs(&level) > -0.01,
	       "full-scale square: 0 dBFS");

	/* sine at amplitude 0.3 (the e2e fake tone): rms = 0.3/sqrt(2) -> -13.47 dBFS, peak -10.46 */
	tea_pcm_level_reset(&level);
	std::vector<int16_t> sine(16000);
	for (size_t i = 0; i < sine.size(); i++)
		sine[i] =
			(int16_t)(0.3 * 32767.0 * std::sin(2.0 * 3.14159265358979323846 * 440.0 * (double)i / 16000.0));
	tea_pcm_level_add(&level, sine.data(), sine.size());
	expect(near_db(tea_pcm_level_rms_dbfs(&level), 20.0 * std::log10(0.3 / std::sqrt(2.0))),
	       "sine RMS dBFS = 20 log10(A / sqrt 2)");
	expect(std::fabs(tea_pcm_level_peak_dbfs(&level) - 20.0 * std::log10(0.3)) < 0.1, "sine peak dBFS");
	expect(tea_pcm_level_rms_dbfs(&level) < tea_pcm_level_peak_dbfs(&level), "RMS is below peak");

	/* a -60 dBFS threshold sits at amplitude 0.001 */
	expect(near_db(tea_dbfs_from_amplitude(0.001), -60.0), "0.001 full scale = -60 dBFS");
	expect(tea_dbfs_from_amplitude(1e-9) == TEA_DBFS_FLOOR, "tiny amplitudes clamp to the floor");
}

static void test_speech_state()
{
	tea_speech_state_t s;
	tea_speech_reset(&s);
	expect(tea_speech_state(&s) == TEA_SPEECH_LISTENING, "starts listening");
	tea_speech_on_started(&s);
	expect(tea_speech_state(&s) == TEA_SPEECH_DETECTED, "speech.started: speech detected");
	tea_speech_on_queued(&s);
	expect(tea_speech_state(&s) == TEA_SPEECH_PROCESSING, "segment.queued: the segment is processing");
	tea_speech_on_started(&s);
	expect(tea_speech_state(&s) == TEA_SPEECH_DETECTED, "new speech while the last segment is processed");
	tea_speech_on_queued(&s);
	tea_speech_on_segment_done(&s);
	expect(tea_speech_state(&s) == TEA_SPEECH_PROCESSING, "one of two segments done: still processing");
	tea_speech_on_segment_done(&s);
	tea_speech_on_segment_done(&s); /* a stray final never goes negative */
	expect(tea_speech_state(&s) == TEA_SPEECH_LISTENING && s.pending == 0, "all done: listening again");
	expect(tea_event_kind("speech.started") == TEA_EV_SPEECH_STARTED &&
		       tea_event_kind("segment.queued") == TEA_EV_SEGMENT_QUEUED &&
		       tea_event_kind("audio.ack") == TEA_EV_AUDIO_ACK &&
		       tea_event_kind("transcript.stable") == TEA_EV_STABLE && tea_event_kind("pong") == TEA_EV_OTHER,
	       "event kinds for the heartbeat counters");
}

/* Drives the warning state like asr-client.cpp: a 200 ms frame every 200 ms
 * at `db`, server acks every 100 ms unless `server_quiet`, checked every
 * 50 ms. Returns the times (ms) each warning was due. */
static std::vector<int64_t> run_warn(int flag, int64_t duration_ms, double db, bool send, bool server_quiet,
				     bool speech_events)
{
	tea_warn_state_t w;
	tea_warn_session_start(&w, 0);
	std::vector<int64_t> due_at;
	for (int64_t t = 0; t <= duration_ms; t += 50) {
		if (send && t % 200 == 0)
			tea_warn_on_send(&w, t, 200, db);
		if (!server_quiet && t % 100 == 0)
			tea_warn_on_server_event(&w, t);
		if (speech_events && t % 2000 == 0)
			tea_warn_on_speech(&w);
		if (tea_warn_due(&w, t) & flag)
			due_at.push_back(t);
	}
	return due_at;
}

static void test_warning_timers()
{
	/* no audio at all while connected: after 3 s, then at most every 30 s */
	std::vector<int64_t> no_audio = run_warn(TEA_WARN_NO_AUDIO, 70000, -20.0, false, false, false);
	expect(no_audio.size() == 3 && no_audio[0] > 3000 && no_audio[0] <= 3100 && no_audio[1] - no_audio[0] == 30000,
	       "no-audio warns after 3 s, then once per 30 s");
	expect(run_warn(TEA_WARN_NO_AUDIO, 70000, -20.0, true, false, true).empty(),
	       "no no-audio warning while frames are sent");

	/* silence being sent: after 10 s below -60 dBFS */
	std::vector<int64_t> quiet = run_warn(TEA_WARN_QUIET, 45000, -90.0, true, false, false);
	expect(quiet.size() == 2 && quiet[0] > 10000 && quiet[0] <= 10100 && quiet[1] - quiet[0] == 30000,
	       "quiet warns after 10 s below -60 dBFS, then once per 30 s");
	expect(run_warn(TEA_WARN_QUIET, 45000, -40.0, true, false, true).empty(), "no quiet warning for normal audio");
	expect(run_warn(TEA_WARN_QUIET, 45000, -90.0, false, false, false).empty(),
	       "nothing sent: that is the no-audio warning, not the quiet one");

	/* audible audio, but the server never reports speech */
	std::vector<int64_t> no_speech = run_warn(TEA_WARN_NO_SPEECH, 50000, -20.0, true, false, false);
	expect(no_speech.size() == 2 && no_speech[0] >= 15000 && no_speech[0] <= 15300,
	       "no-speech warns after 15 s of audible audio, rate limited");
	expect(run_warn(TEA_WARN_NO_SPEECH, 50000, -20.0, true, false, true).empty(),
	       "speech.started / transcripts every 2 s: no warning");
	expect(run_warn(TEA_WARN_NO_SPEECH, 50000, -90.0, true, false, false).empty(),
	       "silence is not counted as unheard speech");

	/* stalled server */
	std::vector<int64_t> stalled = run_warn(TEA_WARN_STALLED, 40000, -20.0, true, true, true);
	expect(stalled.size() == 2 && stalled[0] > 5000 && stalled[0] <= 5100 && stalled[1] - stalled[0] == 30000,
	       "stalled warns after 5 s without any server event while sending, then once per 30 s");
	expect(run_warn(TEA_WARN_STALLED, 40000, -20.0, true, false, true).empty(), "acks keep the stall warning away");
	expect(run_warn(TEA_WARN_STALLED, 40000, -20.0, false, true, true).empty(),
	       "not sending: a quiet server is not a stall");

	/* no session: nothing */
	tea_warn_state_t w;
	tea_warn_session_start(&w, 0);
	tea_warn_session_end(&w);
	expect(tea_warn_due(&w, 100000) == 0, "no warnings without a session");
}

static void test_heartbeat()
{
	tea_heartbeat_t h;
	tea_heartbeat_reset(&h, 0);
	std::vector<int16_t> frame(3200, 1000);
	tea_heartbeat_on_send(&h, 200, frame.data(), frame.size());
	tea_heartbeat_on_send(&h, 400, frame.data(), frame.size());
	tea_heartbeat_on_send(&h, 2400, frame.data(), frame.size());
	expect(h.frames == 3 && h.samples / 16 == 600, "frames and ms of audio sent");
	expect(tea_heartbeat_max_gap(&h, 2500) == 2000, "longest gap between sends");
	expect(tea_heartbeat_max_gap(&h, 9500) == 7100, "an ongoing gap counts too");
	expect(!tea_heartbeat_due(&h, 9999) && tea_heartbeat_due(&h, 10000), "one heartbeat every 10 s");
	expect(near_db(tea_pcm_level_rms_dbfs(&h.level), 20.0 * std::log10(1000.0 / 32768.0)), "window level");
}

/* Lazy reconnect: no attempts while the audio source is idle. */
static void test_audio_idle_gate()
{
	using tea_asr::AudioIdleGate;
	using tea_asr::FailureClass;
	AudioIdleGate gate;
	gate.onAudio(1000); /* the WAV plays ... */

	/* ... and ends; 120 s later the server ends the session: idle_timeout */
	expect(gate.shouldWait(FailureClass::Transient, "idle_timeout", 121000), "idle_timeout waits for audio");
	int attempts = 0;
	for (int64_t t = 121000; t < 3 * 3600 * 1000; t += 50)
		attempts += gate.resumeDue(false) ? 1 : 0;
	expect(attempts == 0 && gate.waiting(), "no connection attempt in three hours without audio");
	gate.onAudio(3 * 3600 * 1000);
	expect(gate.resumeDue(true), "audio again: connect at once");
	expect(!gate.waiting() && !gate.resumeDue(true), "only once");

	/* idle_timeout even when the last audio was recent (the server's idle
	 * window is the authority) */
	gate.onAudio(500000);
	expect(gate.shouldWait(FailureClass::Transient, "idle_timeout", 501000), "idle_timeout always waits");

	/* other transient closes: backoff while audio flows ... */
	gate.onAudio(600000);
	expect(!gate.shouldWait(FailureClass::Transient, "", 601000),
	       "a network error / server restart while audio flows keeps the backoff");
	expect(!gate.shouldWait(FailureClass::Transient, "concurrent_session_limit", 602000),
	       "admission limits with audio flowing keep the backoff");
	/* ... wait for audio when the source has been silent for a while */
	expect(gate.shouldWait(FailureClass::Transient, "", 600000 + AudioIdleGate::kNoAudioMs),
	       "any transient close with no audio for a while waits for audio");
	expect(!gate.shouldWait(FailureClass::Transient, "", 600000 + AudioIdleGate::kNoAudioMs - 1),
	       "but not before kNoAudioMs");

	/* never had audio (OBS started with a silent source): no retries either */
	AudioIdleGate fresh;
	expect(fresh.shouldWait(FailureClass::Transient, "", 5000), "no audio ever: wait for it");

	/* the other failure classes keep their own handling, audio or not */
	expect(!fresh.shouldWait(FailureClass::Auth, "unauthenticated", 5000), "auth keeps its schedule");
	expect(!fresh.shouldWait(FailureClass::RateLimited, "rate_limited", 5000), "rate_limited keeps its wait");
	expect(!fresh.shouldWait(FailureClass::Config, "forbidden_origin", 5000), "Host rejection keeps its retries");
	expect(!fresh.shouldWait(FailureClass::Fatal, "x", 5000), "fatal stays stopped");
	expect(!fresh.shouldWait(FailureClass::NoToken, "", 5000), "no token keeps watching the token file");
	expect(!fresh.waiting(), "and is not left waiting");
}

int main()
{
	test_audio_idle_gate();
	using tea_asr::ErrorPolicy;
	using tea_asr::FailureClass;
	using tea_asr::ReconnectBackoff;

	/* Keep the platform policy explicit and testable without an OBS build. */
	expect(std::strcmp(TEA_FONT_FACE_MACOS, "Heiti TC") == 0,
	       "macOS OBS FreeType default must use the visible Heiti TC family");
	expect(std::strcmp(TEA_FONT_FACE_WINDOWS, "Microsoft JhengHei") == 0,
	       "Windows default must cover Traditional Chinese");
	expect(std::strcmp(TEA_FONT_FACE_UNIX, "Noto Sans CJK TC") == 0, "Unix default must cover Traditional Chinese");
#if defined(__APPLE__)
	expect(std::strcmp(TEA_DEFAULT_FONT_FACE, TEA_FONT_FACE_MACOS) == 0, "macOS must select Heiti TC");
#elif defined(_WIN32)
	expect(std::strcmp(TEA_DEFAULT_FONT_FACE, TEA_FONT_FACE_WINDOWS) == 0,
	       "Windows must select Microsoft JhengHei");
#else
	expect(std::strcmp(TEA_DEFAULT_FONT_FACE, TEA_FONT_FACE_UNIX) == 0, "Unix must select Noto Sans CJK TC");
#endif
	expect(std::strcmp(TEA_DEFAULT_FONT_STYLE, "") == 0, "default OBS font style must be present and empty");

	/* The normal admission-rejection sequence is an error event followed by
	 * the transport close.  It must remain visible and retryable. */
	ErrorPolicy policy;
	policy.observeError("concurrent_session_limit", "continuous session limit reached", true);
	expect(!policy.fatal(), "concurrent_session_limit must remain retryable");
	expect(policy.lastErrorSummary() == "concurrent_session_limit: continuous session limit reached",
	       "the server error code and message must be preserved");
	policy.observeClose(4029);
	expect(policy.lastErrorSummary() == "concurrent_session_limit: continuous session limit reached",
	       "4029 must not overwrite the richer preceding error");
	expect(policy.reconnectStatus(1000) ==
		       "reconnecting in 1000ms (reason: concurrent_session_limit: continuous session limit reached)",
	       "retry status must include the admission reason");

	/* If a server/proxy drops the text event, close 4029 still yields an
	 * actionable reason rather than a generic reconnect loop. */
	policy.reset();
	policy.observeClose(4029);
	expect(!policy.fatal(), "4029 fallback must remain retryable");
	expect(policy.lastErrorSummary().find("concurrent_session_limit:") == 0,
	       "4029 must synthesize the admission error code");
	expect(policy.lastErrorSummary().find("stop another live-subtitle source") != std::string::npos,
	       "4029 fallback must tell the user how to recover");

	/* A stale retryable error from another cause must not suppress the
	 * admission fallback.  The close code is authoritative unless the
	 * preceding application error was the matching admission error. */
	policy.reset();
	policy.observeError("queue_full", "temporary backlog", true);
	policy.observeClose(4029);
	expect(policy.lastErrorSummary().find("concurrent_session_limit:") == 0,
	       "4029 must replace an unrelated prior error reason");

	/* 1013 is shared by unrelated transient/fatal server errors and is not
	 * itself an admission signal.  Without a preceding error event it still
	 * describes itself (never a bare "reconnecting"), but claims no cause. */
	policy.reset();
	policy.observeClose(1013);
	expect(policy.lastErrorSummary().find("session_limit") == std::string::npos,
	       "1013 alone must not claim a session limit");
	expect(policy.lastErrorSummary().find("1013") != std::string::npos, "1013 alone must still be visible");
	expect(!policy.fatal() && policy.failureClass() == FailureClass::Transient, "1013 alone stays retryable");

	/* ...and a preceding error event stays authoritative over the close. */
	policy.reset();
	policy.observeError("queue_full", "temporary backlog", true);
	policy.observeClose(1013);
	expect(policy.lastErrorSummary() == "queue_full: temporary backlog", "1013 must not overwrite the error event");

	/* A non-retryable application error still stops reconnecting, proving the
	 * retryable bit remains the source of truth for other error codes. */
	policy.observeError("slow_client", "event queue stayed full", false);
	expect(policy.fatal(), "non-retryable server errors must stop reconnecting");
	expect(policy.fatalReason() == "stopped: server error (slow_client: event queue stayed full)",
	       "fatal status must preserve the server reason");

	/* idle_timeout (server close 4408) is retryable and keeps the server's
	 * own message; a lost error event still yields an actionable reason. */
	policy.reset();
	policy.observeError("idle_timeout", "120 s without audio", true);
	policy.observeClose(4408);
	expect(!policy.fatal(), "idle_timeout must stay retryable");
	expect(policy.lastErrorSummary() == "idle_timeout: 120 s without audio", "4408 keeps the server message");
	policy.reset();
	policy.observeClose(4408);
	expect(policy.lastErrorSummary().find("idle_timeout:") == 0 &&
		       policy.lastErrorSummary().find("audio source") != std::string::npos,
	       "4408 without an error event must point at the audio source");

	/* A new attempt never shows the previous attempt's reason, but a fatal
	 * verdict survives until an explicit reset(). */
	policy.reset();
	policy.observeError("model_incompatible", "bad checkpoint", false);
	policy.beginAttempt();
	expect(policy.fatal() && policy.lastErrorSummary().empty(), "beginAttempt clears reasons, not fatal");
	policy.reset();
	expect(!policy.fatal(), "reset clears fatal");

	/* Preflight (authenticated GET /v1/capabilities) classification. */
	policy.reset();
	policy.observePreflightHttp(401, "unauthenticated", "missing or bad bearer token");
	expect(policy.failureClass() == FailureClass::Auth && !policy.fatal(),
	       "401 is an auth failure, not a fatal server error");
	expect(policy.lastErrorSummary().find("unauthenticated:") == 0, "401 names unauthenticated");
	policy.reset();
	policy.observeError("unauthenticated", "x", false);
	expect(policy.failureClass() == FailureClass::Auth && !policy.fatal(),
	       "an unauthenticated error event is still an auth failure despite retryable=false");
	policy.reset();
	policy.observePreflightHttp(429, "rate_limited", "too many failures");
	expect(policy.failureClass() == FailureClass::RateLimited, "429 rate_limited is its own class");
	policy.reset();
	policy.observePreflightHttp(403, "forbidden_origin", "Host not allowed: example.lan");
	expect(policy.failureClass() == FailureClass::Config, "403 forbidden_origin is a config error");
	expect(policy.lastErrorSummary().find("forbidden_origin:") == 0 &&
		       policy.lastErrorSummary().find("LAN") != std::string::npos,
	       "Host rejection explains the allowlist");
	policy.reset();
	policy.observePreflightHttp(500, "internal_error", "boom");
	expect(policy.failureClass() == FailureClass::Transient, "other HTTP errors are transient");
	expect(policy.lastErrorSummary() == "HTTP 500 from /v1/capabilities (internal_error: boom)",
	       "other HTTP errors keep status, code and message");

	/* A reason-less WS 403 after a good preflight: say what is known. */
	policy.reset();
	policy.observeHandshakeRejected(403, 4);
	expect(policy.failureClass() == FailureClass::Transient,
	       "a rejected upgrade after a good preflight is retryable");
	expect(policy.lastErrorSummary().find("session_limit") != std::string::npos &&
		       policy.lastErrorSummary().find("max_total_connections=4") != std::string::npos,
	       "a rejected upgrade names the likely connection cap");

	policy.reset();
	policy.observeNoToken("/tmp/token");
	expect(policy.failureClass() == FailureClass::NoToken, "an empty/missing token file never contacts the server");

	/* A transport reason never overwrites a more specific one. */
	policy.reset();
	policy.observeError("concurrent_session_limit", "slots in use", true);
	policy.observeTransport("connection closed");
	expect(policy.hasConcurrentSessionError() &&
		       policy.lastErrorSummary() == "concurrent_session_limit: slots in use",
	       "transport teardown keeps the server reason");

	/* Backoff: transient 1,2,4,8,16 s then a 30 s cap. */
	ReconnectBackoff backoff;
	const int expected[] = {1000, 2000, 4000, 8000, 16000, 30000, 30000, 30000};
	for (int e : expected)
		expect(backoff.nextDelayMs(FailureClass::Transient) == e, "transient backoff must double up to 30 s");
	backoff.reset();
	expect(backoff.nextDelayMs(FailureClass::Transient) == 1000, "reset restarts the transient backoff");

	/* Auth: every attempt costs the server one failure. Simulate one source
	 * retrying until it stops and prove it stays far below the server's 10
	 * failures / 60 s even when four sources share the address. */
	backoff.reset();
	long long t = 0;
	long long failures[16];
	int nFailures = 0;
	failures[nFailures++] = t; /* the first rejected attempt */
	for (;;) {
		int d = backoff.nextDelayMs(FailureClass::Auth);
		if (d < 0)
			break;
		expect(d >= 30000, "auth retries must be at least 30 s apart");
		t += d;
		failures[nFailures++] = t;
	}
	expect(nFailures == ReconnectBackoff::kMaxAuthAttempts, "auth retries must stop after the attempt budget");
	int worstWindow = 0;
	for (int i = 0; i < nFailures; i++) {
		int inWindow = 0;
		for (int j = i; j < nFailures; j++)
			if (failures[j] - failures[i] < 60000)
				inWindow++;
		worstWindow = inWindow > worstWindow ? inWindow : worstWindow;
	}
	expect(worstWindow * 4 < 10, "four sources with a bad token must stay under the server's 10/60 s limit");
	backoff.resetAuth();
	expect(backoff.nextDelayMs(FailureClass::Auth) == ReconnectBackoff::kAuthFirstMs,
	       "a changed token file restarts the auth budget");

	expect(backoff.nextDelayMs(FailureClass::RateLimited) > 60000,
	       "rate_limited waits out the server's 60 s window");
	expect(backoff.nextDelayMs(FailureClass::Config) >= 60000, "config errors retry slowly");
	expect(backoff.nextDelayMs(FailureClass::Fatal) < 0, "fatal errors stop retrying");

	expect(ReconnectBackoff::sessionWasHealthy(60000, ""), "a long session resets backoff");
	expect(!ReconnectBackoff::sessionWasHealthy(5000, ""), "a short session does not reset backoff");
	expect(!ReconnectBackoff::sessionWasHealthy(120000, "idle_timeout"),
	       "an idle_timeout session does not reset backoff");

	test_dbfs();
	test_speech_state();
	test_warning_timers();
	test_heartbeat();

	std::cout << "asr-client policy tests passed\n";
	return EXIT_SUCCESS;
}
