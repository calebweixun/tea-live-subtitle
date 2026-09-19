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
	 * itself an admission signal. */
	policy.reset();
	policy.observeClose(1013);
	expect(policy.lastErrorSummary().empty(), "1013 alone must not claim a session limit");

	/* A non-retryable application error still stops reconnecting, proving the
	 * retryable bit remains the source of truth for other error codes. */
	policy.observeError("slow_client", "event queue stayed full", false);
	expect(policy.fatal(), "non-retryable server errors must stop reconnecting");
	expect(policy.fatalReason() == "stopped: server error (slow_client: event queue stayed full)",
	       "fatal status must preserve the server reason");

	std::cout << "asr-client policy tests passed\n";
	return EXIT_SUCCESS;
}
