/*
 * End-to-end driver: runs the plugin's real TeaAsrClient (src/asr-client.cpp)
 * and real caption state machine (src/caption-state.c) against a live
 * tea-asr-service instance, with a fake paced audio tap instead of OBS.
 *
 * Output is one JSON object per line on stdout:
 *   {"t":ms,"client":i,"status":"..."}        whenever the diagnostics text changes
 *   {"t":ms,"client":i,"caption":"...","partial":bool}  whenever rendered captions change
 *   {"t":ms,"event":"done"}
 * tests/e2e/run_e2e.py parses these and asserts on them.
 *
 * Usage: asr-client-e2e --port N --token PATH [--host H] [--clients K]
 *                       [--duration-ms MS] [--audio speech|dead|none]
 *                       [--restart-at-ms MS]
 */
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QElapsedTimer>
#include <QTimer>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "asr-client.h"
#include "caption-state.h"

extern "C" void fake_audio_tap_set_default_mode(const char *mode);

namespace {

struct Slot {
	tea_audio_tap_t *tap = nullptr;
	tea_caption_state_t *captions = nullptr;
	tea_asr_client_t *client = nullptr;
	std::string lastStatus;
	std::string lastCaption;
	bool lastPartial = false;
};

void emitLine(const QJsonObject &obj)
{
	QByteArray line = QJsonDocument(obj).toJson(QJsonDocument::Compact);
	std::fwrite(line.constData(), 1, (size_t)line.size(), stdout);
	std::fputc('\n', stdout);
	std::fflush(stdout);
}

} // namespace

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);

	std::string host = "127.0.0.1";
	int port = 0;
	std::string token;
	int clients = 1;
	int durationMs = 10000;
	int restartAtMs = -1;
	std::string audio = "speech";

	for (int i = 1; i < argc; i++) {
		auto next = [&](const char *name) -> const char * {
			if (i + 1 >= argc) {
				std::fprintf(stderr, "missing value for %s\n", name);
				std::exit(2);
			}
			return argv[++i];
		};
		if (!std::strcmp(argv[i], "--host"))
			host = next("--host");
		else if (!std::strcmp(argv[i], "--port"))
			port = std::atoi(next("--port"));
		else if (!std::strcmp(argv[i], "--token"))
			token = next("--token");
		else if (!std::strcmp(argv[i], "--clients"))
			clients = std::atoi(next("--clients"));
		else if (!std::strcmp(argv[i], "--duration-ms"))
			durationMs = std::atoi(next("--duration-ms"));
		else if (!std::strcmp(argv[i], "--restart-at-ms"))
			restartAtMs = std::atoi(next("--restart-at-ms"));
		else if (!std::strcmp(argv[i], "--audio"))
			audio = next("--audio");
		else {
			std::fprintf(stderr, "unknown argument %s\n", argv[i]);
			return 2;
		}
	}
	if (port <= 0 || port == 8327 || token.empty()) {
		std::fprintf(stderr, "--port (not 8327, the real service) and --token are required\n");
		return 2;
	}

	fake_audio_tap_set_default_mode(audio.c_str());

	std::vector<Slot> rigs((size_t)clients);
	for (auto &s : rigs) {
		s.tap = tea_audio_tap_create();
		s.captions = tea_caption_state_create();
		tea_caption_state_set_max_lines(s.captions, 3);
		s.client = tea_asr_client_create(s.tap, s.captions);
		tea_asr_client_set_server(s.client, host.c_str(), port);
		tea_asr_client_set_token_path(s.client, token.c_str());
		tea_asr_client_start(s.client);
	}

	QElapsedTimer clock;
	clock.start();

	QTimer poll;
	poll.setInterval(20);
	QObject::connect(&poll, &QTimer::timeout, [&]() {
		for (size_t i = 0; i < rigs.size(); i++) {
			Slot &s = rigs[i];
			char *status = tea_asr_client_status_text(s.client);
			if (s.lastStatus != status) {
				s.lastStatus = status;
				emitLine(QJsonObject{{"t", (double)clock.elapsed()},
						     {"client", (int)i},
						     {"status", QString::fromUtf8(status)},
						     {"connected", tea_asr_client_is_connected(s.client)}});
			}
			std::free(status);

			char *caption = tea_caption_state_render(s.captions);
			bool partial = tea_caption_state_last_line_is_partial(s.captions);
			if (s.lastCaption != caption || s.lastPartial != partial) {
				s.lastCaption = caption;
				s.lastPartial = partial;
				emitLine(QJsonObject{{"t", (double)clock.elapsed()},
						     {"client", (int)i},
						     {"caption", QString::fromUtf8(caption)},
						     {"partial", partial}});
			}
			std::free(caption);
		}
	});
	poll.start();

	if (restartAtMs >= 0) {
		QTimer::singleShot(restartAtMs, [&]() {
			emitLine(QJsonObject{{"t", (double)clock.elapsed()}, {"event", "explicit_restart"}});
			for (auto &s : rigs)
				tea_asr_client_start(s.client);
		});
	}

	QTimer::singleShot(durationMs, [&]() {
		poll.stop();
		for (auto &s : rigs) {
			tea_asr_client_stop(s.client);
			tea_asr_client_destroy(s.client);
			tea_caption_state_destroy(s.captions);
			tea_audio_tap_destroy(s.tap);
		}
		emitLine(QJsonObject{{"t", (double)clock.elapsed()}, {"event", "done"}});
		app.quit();
	});

	return app.exec();
}
