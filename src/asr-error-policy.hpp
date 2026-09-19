#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace tea_asr {

/*
 * The stream client receives an application-level error event before the
 * server closes the WebSocket.  Keep this small policy object independent of
 * Qt and OBS so the admission/reconnect contract can be tested in CI without
 * downloading OBS or configuring the plugin's full CMake build.
 */
class ErrorPolicy {
public:
	void reset()
	{
		fatal_ = false;
		fatalReason_.clear();
		lastServerErrorCode_.clear();
		lastErrorSummary_.clear();
	}

	void observeError(std::string_view code, std::string_view message, bool retryable)
	{
		lastServerErrorCode_.assign(code);
		lastErrorSummary_.assign(code);
		if (!message.empty()) {
			lastErrorSummary_ += ": ";
			lastErrorSummary_.append(message);
		}

		/* The server's retryable bit is authoritative.  In particular,
		 * concurrent_session_limit is retryable because another source may
		 * release a continuous-session slot later. */
		if (!retryable) {
			fatal_ = true;
			fatalReason_ = "stopped: server error (" + lastErrorSummary_ + ")";
		}
	}

	void observeClose(uint16_t code)
	{
		/* 4029 is the dedicated admission-rejection close code.  The error
		 * event normally precedes it; only synthesize a reason when that event
		 * was lost so a reconnect is still actionable to the user. */
		if (code == 4029 && lastServerErrorCode_ != "concurrent_session_limit") {
			lastServerErrorCode_ = "concurrent_session_limit";
			lastErrorSummary_ = "concurrent_session_limit: all continuous-session slots are in use; "
					    "stop another live-subtitle source or raise the server limit";
		}
	}

	bool fatal() const { return fatal_; }
	bool hasConcurrentSessionError() const { return lastServerErrorCode_ == "concurrent_session_limit"; }
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
	bool fatal_ = false;
	std::string fatalReason_;
	std::string lastServerErrorCode_;
	std::string lastErrorSummary_;
};

} // namespace tea_asr
