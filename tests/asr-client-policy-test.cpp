#include "asr-error-policy.hpp"
#include "font-default-policy.h"

#include <cstdlib>
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

int main()
{
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

	std::cout << "asr-client policy tests passed\n";
	return EXIT_SUCCESS;
}
