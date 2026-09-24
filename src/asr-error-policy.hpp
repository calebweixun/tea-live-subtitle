#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

namespace tea_asr {

/*
 * Qt/OBS-free connection policy for the TEA ASR stream client, so the
 * admission/reconnect contract can be tested in CI without downloading OBS.
 *
 * Wire facts this encodes (tea-asr-service docs/04-api.md "目前真實契約"):
 *  - Every /v1 HTTP route, including /v1/capabilities, needs the bearer token.
 *    Every failed auth (HTTP 401 or a rejected WS upgrade) counts toward the
 *    server's per-address limit of 10 failures per 60 s, after which it
 *    answers `rate_limited` to everything -- even a correct token.
 *  - The server rejects unauthenticated / rate_limited / forbidden_origin /
 *    session_limit WS upgrades *before* accept(). The documented close codes
 *    (1008/1013) never reach a real client: uvicorn turns a pre-accept close
 *    into a bare `HTTP/1.1 403` with an empty body. Only an authenticated
 *    HTTP request can tell those reasons apart, which is why the client does
 *    an authenticated GET /v1/capabilities preflight before every WS attempt.
 *  - After accept(), session-level failures arrive as an `error` event
 *    followed by a close: 4029 concurrent_session_limit, 4408 idle_timeout,
 *    1013 queue_full/session_limit/slow_client/rate_limited, 1008 protocol,
 *    1012 timeline_gap.
 */
enum class FailureClass {
	Transient,   /* server down/busy, admission, idle, dead peer: back off, keep trying */
	Auth,        /* token rejected (401): slow retries, then stop; each try costs a server failure */
	RateLimited, /* server is throttling this address: wait out its window */
	Config,      /* Host rejected (forbidden_origin): won't fix itself; slow retries */
	NoToken,     /* token file missing/empty (e.g. revoked): don't contact the server at all */
	Fatal,       /* server said retryable=false: stop until an explicit restart */
};

class ErrorPolicy {
public:
	void reset()
	{
		fatal_ = false;
		fatalReason_.clear();
		beginAttempt();
	}

	/* Called at the start of every connection attempt: reasons describe the
	 * attempt that failed, never a stale earlier one. `fatal` survives until
	 * an explicit reset(). */
	void beginAttempt()
	{
		lastServerErrorCode_.clear();
		lastErrorSummary_.clear();
		class_ = FailureClass::Transient;
	}

	void observeError(std::string_view code, std::string_view message, bool retryable)
	{
		lastServerErrorCode_.assign(code);
		setSummary(code, message);

		if (code == "unauthenticated") {
			class_ = FailureClass::Auth;
			return;
		}
		if (code == "rate_limited") {
			class_ = FailureClass::RateLimited;
			return;
		}
		if (code == "forbidden_origin") {
			class_ = FailureClass::Config;
			return;
		}

		/* Otherwise the server's retryable bit is authoritative.  In
		 * particular, concurrent_session_limit and idle_timeout are
		 * retryable: a slot may free up, audio may come back. */
		if (!retryable) {
			fatal_ = true;
			class_ = FailureClass::Fatal;
			fatalReason_ = "stopped: server error (" + lastErrorSummary_ + ")";
		} else {
			class_ = FailureClass::Transient;
		}
	}

	void observeClose(uint16_t code)
	{
		/* 4029 is the dedicated admission-rejection close code.  The error
		 * event normally precedes it; only synthesize a reason when that event
		 * was lost so a reconnect is still actionable to the user. */
		if (code == 4029) {
			if (lastServerErrorCode_ != "concurrent_session_limit") {
				lastServerErrorCode_ = "concurrent_session_limit";
				lastErrorSummary_ =
					"concurrent_session_limit: all continuous-session slots are in use; "
					"stop another live-subtitle source or raise the server limit";
				class_ = FailureClass::Transient;
			}
			return;
		}
		if (code == 4408) {
			if (lastServerErrorCode_ != "idle_timeout") {
				lastServerErrorCode_ = "idle_timeout";
				lastErrorSummary_ =
					"idle_timeout: the server received no audio for its idle window "
					"and ended the session; check that the audio source is producing audio";
				class_ = FailureClass::Transient;
			}
			return;
		}

		/* Every other close code is ambiguous on its own (1013 alone covers
		 * queue_full, session_limit, slow_client and rate_limited), so the
		 * preceding error event stays authoritative.  Only when none arrived do
		 * we describe the close itself -- without inventing a specific cause --
		 * so the retry status is never a bare "reconnecting". */
		if (!lastServerErrorCode_.empty())
			return;
		switch (code) {
		case 1013:
			lastErrorSummary_ = "server closed the connection with 1013 (try again later: server busy)";
			break;
		case 1008:
			lastErrorSummary_ = "server closed the connection with 1008 (policy/protocol violation)";
			break;
		case 1012:
			lastErrorSummary_ = "server closed the connection with 1012 (restart or timeline gap; "
					    "a new session is required)";
			break;
		default:
			lastErrorSummary_ = "server closed the connection (code " + std::to_string(code) + ")";
			break;
		}
		class_ = FailureClass::Transient;
	}

	/* Result of the authenticated GET /v1/capabilities preflight when it did
	 * produce an HTTP status (0 < status, status != 200). `code`/`message`
	 * come from the server's error envelope when present. */
	void observePreflightHttp(int status, std::string_view code, std::string_view message)
	{
		if (status == 401 || code == "unauthenticated") {
			lastServerErrorCode_ = "unauthenticated";
			setSummary("unauthenticated", message.empty() ? "bearer token rejected" : message);
			lastErrorSummary_ += " (check the token file)";
			class_ = FailureClass::Auth;
		} else if (status == 429 && (code.empty() || code == "rate_limited")) {
			lastServerErrorCode_ = "rate_limited";
			setSummary("rate_limited", message.empty() ? "too many failed auth attempts" : message);
			lastErrorSummary_ += " (server throttles this address for up to 60 s)";
			class_ = FailureClass::RateLimited;
		} else if (status == 403 || code == "forbidden_origin") {
			lastServerErrorCode_ = "forbidden_origin";
			setSummary("forbidden_origin", message.empty() ? "Host header rejected" : message);
			lastErrorSummary_ += " (the server accepts Host 127.0.0.1/localhost only, unless its LAN "
					     "mode is enabled)";
			class_ = FailureClass::Config;
		} else {
			lastServerErrorCode_.assign(code);
			lastErrorSummary_ = "HTTP " + std::to_string(status) + " from /v1/capabilities";
			if (!code.empty() || !message.empty()) {
				lastErrorSummary_ += " (";
				lastErrorSummary_.append(code);
				if (!code.empty() && !message.empty())
					lastErrorSummary_ += ": ";
				lastErrorSummary_.append(message);
				lastErrorSummary_ += ')';
			}
			class_ = FailureClass::Transient;
		}
	}

	/* The WS upgrade got a non-101 answer even though the authenticated
	 * preflight just succeeded. With the current server that is almost
	 * always the connection cap, but the 403 carries no reason, so say what
	 * is known rather than claiming certainty. */
	void observeHandshakeRejected(int httpStatus, int maxTotalConnections)
	{
		lastServerErrorCode_ = "handshake_rejected";
		lastErrorSummary_ = "WebSocket upgrade rejected (HTTP " + std::to_string(httpStatus) + ")";
		if (httpStatus == 403) {
			lastErrorSummary_ += ": most likely session_limit -- the server connection cap";
			if (maxTotalConnections > 0)
				lastErrorSummary_ +=
					" (max_total_connections=" + std::to_string(maxTotalConnections) + ")";
			lastErrorSummary_ += " is in use by other clients";
		}
		class_ = FailureClass::Transient;
	}

	void observeTransport(std::string_view what)
	{
		if (!lastErrorSummary_.empty())
			return; /* keep the more specific reason already recorded */
		lastErrorSummary_.assign(what);
		class_ = FailureClass::Transient;
	}

	void observeNoToken(std::string_view path)
	{
		lastServerErrorCode_.clear();
		lastErrorSummary_ = "no token: the token file is missing or empty (revoked?): ";
		lastErrorSummary_.append(path);
		class_ = FailureClass::NoToken;
	}

	bool fatal() const { return fatal_; }
	FailureClass failureClass() const { return fatal_ ? FailureClass::Fatal : class_; }
	bool hasConcurrentSessionError() const { return lastServerErrorCode_ == "concurrent_session_limit"; }
	const std::string &lastServerErrorCode() const { return lastServerErrorCode_; }
	const std::string &fatalReason() const { return fatalReason_; }
	const std::string &lastErrorSummary() const { return lastErrorSummary_; }

	std::string reconnectStatus(int delayMs) const
	{
		std::string status = "reconnecting in " + std::to_string(delayMs) + "ms";
		if (!lastErrorSummary_.empty()) {
			status += " (reason: ";
			status += lastErrorSummary_;
			status += ')';
		}
		return status;
	}

private:
	void setSummary(std::string_view code, std::string_view message)
	{
		lastErrorSummary_.assign(code);
		if (!message.empty()) {
			lastErrorSummary_ += ": ";
			lastErrorSummary_.append(message);
		}
	}

	bool fatal_ = false;
	FailureClass class_ = FailureClass::Transient;
	std::string fatalReason_;
	std::string lastServerErrorCode_;
	std::string lastErrorSummary_;
};

/*
 * Reconnect delays per failure class. Budget against the server's auth rate
 * limit (10 failures / 60 s per source address, shared by every caption
 * source in this OBS process): an Auth attempt costs exactly one failure
 * (the preflight; no WS upgrade follows a rejected preflight) and Auth
 * attempts are >= 30 s apart, so one source spends at most 2 failures per
 * 60 s and stops after kMaxAuthAttempts; four sources stay at <= 8.
 * RateLimited requests are not counted by the server, but retrying before
 * its window expires is pointless, so wait longer than the window.
 */
class ReconnectBackoff {
public:
	static constexpr int kTransientBaseMs = 1000;
	static constexpr int kTransientCapMs = 30000;
	static constexpr int kAuthFirstMs = 30000;
	static constexpr int kAuthCapMs = 60000;
	static constexpr int kMaxAuthAttempts = 5;
	static constexpr int kRateLimitedMs = 65000;
	static constexpr int kConfigMs = 60000;
	static constexpr int kNoTokenPollMs = 2000;
	/* A session that ran at least this long without an idle_timeout counts
	 * as healthy, so its eventual disconnect starts backoff from scratch. */
	static constexpr int64_t kHealthySessionMs = 30000;

	void reset()
	{
		transientAttempts_ = 0;
		authAttempts_ = 0;
	}

	/* Token file changed: the next Auth failure is about a new token. */
	void resetAuth() { authAttempts_ = 0; }

	/* Delay before the next attempt, or -1 to stop retrying automatically
	 * (Fatal, or the Auth budget is spent). NoToken never reaches the server,
	 * so its "delay" is just how often the token file is re-checked. */
	int nextDelayMs(FailureClass cls)
	{
		switch (cls) {
		case FailureClass::Transient: {
			int shift = std::min(transientAttempts_, 5);
			transientAttempts_++;
			return std::min(kTransientBaseMs << shift, kTransientCapMs);
		}
		case FailureClass::Auth:
			authAttempts_++;
			if (authAttempts_ >= kMaxAuthAttempts)
				return -1;
			return authAttempts_ == 1 ? kAuthFirstMs : kAuthCapMs;
		case FailureClass::RateLimited:
			return kRateLimitedMs;
		case FailureClass::Config:
			return kConfigMs;
		case FailureClass::NoToken:
			return kNoTokenPollMs;
		case FailureClass::Fatal:
			return -1;
		}
		return kTransientCapMs;
	}

	int authAttempts() const { return authAttempts_; }

	static bool sessionWasHealthy(int64_t activeMs, const std::string &lastServerErrorCode)
	{
		return activeMs >= kHealthySessionMs && lastServerErrorCode != "idle_timeout";
	}

private:
	int transientAttempts_ = 0;
	int authAttempts_ = 0;
};

} // namespace tea_asr
