/*
tea-live-subtitle
Copyright (C) 2026 Caleb Weixun

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "asr-client.h"
#include "asr-client.hpp"
#include "caption-display.h"

#include <QCryptographicHash>
#include <QDir>
#include <QRandomGenerator>
#include <QJsonDocument>
#include <QJsonArray>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QVariant>
#include <QDir>
#include <QStandardPaths>
#include <QMetaObject>
#include <QNetworkRequest>
#include <QEventLoop>

#include <util/bmem.h>

#include "plugin-support.h"

namespace {
constexpr char kWsGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

/* NEVER pass token/PCM/prompt/transcript text to obs_log -- docs/03
 * forbids logging any of that. Only log protocol-level bookkeeping. */

void appendLeU64(QByteArray &out, uint64_t v)
{
	for (int i = 0; i < 8; i++)
		out.append(char((v >> (8 * i)) & 0xFF));
}
} // namespace

TeaAsrClient::TeaAsrClient(tea_audio_tap_t *tap, tea_caption_state_t *captions) : tap_(tap), captions_(captions)
{
	hintsStatus_.applied_prompt_tokens = -1;
	moveToThread(&thread_);
	thread_.setObjectName(QStringLiteral("tea-asr-client"));
	thread_.start();
}

TeaAsrClient::~TeaAsrClient()
{
	/* Qt::BlockingQueuedConnection has no timeout: if thread_'s event loop
	 * is ever not pumping (edge cases only, e.g. object teardown ordering
	 * during OBS shutdown), the caller -- typically the OBS main thread,
	 * via tea_captions_source_destroy() -- would block forever. Instead,
	 * run our own event loop on the calling thread so it can both receive
	 * the "doStop finished" notification AND time out. */
	wantRunning_ = false;

	bool stoppedInTime = false;
	{
		QEventLoop loop;
		QTimer timeoutTimer;
		timeoutTimer.setSingleShot(true);
		connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);

		bool posted = QMetaObject::invokeMethod(
			this,
			[this, &loop, &stoppedInTime]() {
				doStop();
				stoppedInTime = true;
				loop.quit();
			},
			Qt::QueuedConnection);

		if (posted) {
			timeoutTimer.start(2000);
			loop.exec();
		}
	}

	if (!stoppedInTime) {
		obs_log(LOG_WARNING, "asr-client: doStop() did not complete within 2s during teardown; "
				     "forcing the worker thread down instead of blocking the calling thread");
	}

	thread_.quit();
	if (!thread_.wait(2000)) {
		/* Last resort: a QThread that never quit its event loop cannot be
		 * destroyed safely (Qt explicitly warns about this), but blocking
		 * the caller (likely OBS's main thread) indefinitely is worse.
		 * terminate() is blunt, but only reached once we already know
		 * something is stuck. */
		obs_log(LOG_WARNING, "asr-client: worker thread did not quit within 2s; terminating it");
		thread_.terminate();
		thread_.wait();
	}
	/* The thread is down: a dictionaries request still pending is dropped,
	 * but its caller is told (exactly once, see the header). */
	if (dictDone_) {
		tea_asr_dictionaries_done_t done = dictDone_;
		dictDone_ = nullptr;
		done(dictParam_);
	}
}

void TeaAsrClient::setServer(const QString &host, int port)
{
	QMetaObject::invokeMethod(
		this,
		[this, host, port]() {
			host_ = host;
			port_ = port;
		},
		Qt::QueuedConnection);
}

void TeaAsrClient::setTokenPath(const QString &path)
{
	QMetaObject::invokeMethod(this, [this, path]() { tokenPath_ = path; }, Qt::QueuedConnection);
}

void TeaAsrClient::start()
{
	wantRunning_ = true;
	QMetaObject::invokeMethod(this, "doStart", Qt::QueuedConnection);
}

void TeaAsrClient::stop()
{
	wantRunning_ = false;
	QMetaObject::invokeMethod(this, "doStop", Qt::QueuedConnection);
}

bool TeaAsrClient::isConnected() const
{
	return connected_.load();
}

QString TeaAsrClient::statusText() const
{
	QMutexLocker lock(&statusMutex_);
	return statusText_;
}

uint64_t TeaAsrClient::droppedFrames() const
{
	uint64_t ringDrops = tap_ ? tea_audio_tap_dropped_samples(tap_) : 0;
	return ringDrops + droppedPcmSamples_.load(std::memory_order_relaxed);
}

bool TeaAsrClient::capabilitiesKnown() const
{
	return capabilitiesKnown_.load(std::memory_order_relaxed);
}

bool TeaAsrClient::supportsPartialTranscripts() const
{
	return serverSupportsPartial_.load(std::memory_order_relaxed);
}

bool TeaAsrClient::supportsStableTranscripts() const
{
	return serverSupportsStable_.load(std::memory_order_relaxed);
}

bool TeaAsrClient::stableCaptionsActive() const
{
	return stableActive_.load(std::memory_order_relaxed);
}

void TeaAsrClient::setStableCaptions(bool enabled)
{
	stablePreferred_ = enabled;
}

void TeaAsrClient::setEndSilenceMs(int ms)
{
	endSilencePreferred_ = ms > 0 ? ms : 0;
}

void TeaAsrClient::setTraceDir(const QString &dir)
{
	QMutexLocker lock(&traceMutex_);
	traceDir_ = dir;
}

void TeaAsrClient::setHints(const QString &profile, const QString &domain, const QString &hotwords,
			    const QString &replacements, const QString &filePath)
{
	QMutexLocker lock(&hintsMutex_);
	hints_.profile = profile;
	hints_.domain = domain;
	hints_.hotwords = hotwords;
	hints_.replacements = replacements;
	hints_.filePath = filePath;
}

void TeaAsrClient::hintsStatus(tea_asr_client_hints_status_t *out) const
{
	QMutexLocker lock(&hintsMutex_);
	*out = hintsStatus_;
	out->capability = hintsCapability_.load();
	out->limits = hintsLimits_;
}

void TeaAsrClient::readHintsCapability(const QJsonObject &features)
{
	/* features.context_biasing: absent = a server that predates the
	 * feature; false = present but off (TEA_ASR_CONTEXT_HINTS unset). */
	const QJsonValue biasing = features.value(QStringLiteral("context_biasing"));
	const int capability = biasing.isUndefined() || biasing.isNull() ? TEA_HINTS_CAP_ABSENT
			       : biasing.toBool(false)                   ? TEA_HINTS_CAP_ON
									 : TEA_HINTS_CAP_OFF;
	const QJsonObject limits = features.value(QStringLiteral("context_limits")).toObject();
	tea_hints_limits_t parsed;
	parsed.max_domain_chars = limits.value(QStringLiteral("max_domain_chars")).toInt(0);
	parsed.max_hotwords = limits.value(QStringLiteral("max_hotwords")).toInt(0);
	parsed.max_hotword_chars = limits.value(QStringLiteral("max_hotword_chars")).toInt(0);
	parsed.max_replacements = limits.value(QStringLiteral("max_replacements")).toInt(0);
	parsed.max_replacement_chars = limits.value(QStringLiteral("max_replacement_chars")).toInt(0);
	{
		QMutexLocker lock(&hintsMutex_);
		hintsLimits_ = parsed;
	}
	hintsCapability_ = capability;
}

QJsonObject TeaAsrClient::buildContext()
{
	HintSettings settings;
	tea_hints_limits_t limits;
	{
		QMutexLocker lock(&hintsMutex_);
		settings = hints_;
		limits = hintsLimits_;
	}

	/* The file is read again for every session, so it can be edited (and
	 * grow) outside OBS; it merges before the fields. */
	bool fileError = false;
	QByteArray fileText;
	const QString path = settings.filePath.trimmed();
	if (!path.isEmpty()) {
		QFile file(path);
		if (file.open(QIODevice::ReadOnly)) {
			fileText = file.read(kMaxHintsFileBytes);
		} else {
			fileError = true;
			obs_log(LOG_WARNING, "asr-client: WARN the recognition hints file cannot be read (%s)",
				file.errorString().toUtf8().constData());
		}
	}
	const QByteArray hotwords = settings.hotwords.toUtf8();
	const QByteArray replacements = settings.replacements.toUtf8();
	tea_hints_t hints;
	tea_hints_build(&hints, fileText.isEmpty() ? nullptr : fileText.constData(), hotwords.constData(),
			replacements.constData());

	QString domainText = settings.domain;
	domainText.replace(QStringLiteral("\r\n"), QStringLiteral("\n")).replace(QLatin1Char('\r'), QLatin1Char('\n'));
	const QByteArray domainUtf8 = domainText.toUtf8();
	size_t domainLen = 0;
	const char *domain = tea_hints_domain(domainUtf8.constData(), &domainLen);
	size_t domainKeep = 0;
	tea_hints_report_t report;
	tea_hints_apply_limits(&hints, domain, domainLen, &limits, &domainKeep, &report);

	QJsonObject context;
	const QByteArray profile = settings.profile.trimmed().toUtf8();
	if (!tea_hints_profile_is_none(profile.constData()))
		context.insert(QStringLiteral("profile"), QString::fromUtf8(profile));
	if (domainKeep > 0)
		context.insert(QStringLiteral("domain"), QString::fromUtf8(domain, (qsizetype)domainKeep));
	if (hints.hotword_count > 0) {
		QJsonArray words;
		for (int i = 0; i < hints.hotword_count; i++)
			words.append(QString::fromUtf8(hints.hotwords[i]));
		context.insert(QStringLiteral("hotwords"), words);
	}
	if (hints.pair_count > 0) {
		QJsonArray pairs;
		for (int i = 0; i < hints.pair_count; i++)
			pairs.append(QJsonObject{{QStringLiteral("from"), QString::fromUtf8(hints.pairs[i].from)},
						 {QStringLiteral("to"), QString::fromUtf8(hints.pairs[i].to)}});
		context.insert(QStringLiteral("replacements"), pairs);
	}

	/* Counts only: hint text is prompt text and never goes to the log. */
	if (tea_hints_report_cut(&report))
		obs_log(LOG_WARNING,
			"asr-client: WARN recognition hints cut to the server's limits: domain %d of %d chars, "
			"%d hotwords sent (%d over the limit of %d), %d replacements sent (%d over the limit of %d), "
			"%d entries longer than allowed dropped",
			report.domain_chars - report.domain_cut, report.domain_chars, hints.hotword_count,
			report.hotwords_over, limits.max_hotwords, hints.pair_count, report.replacements_over,
			limits.max_replacements, report.too_long);
	if (hints.invalid_lines > 0)
		obs_log(LOG_WARNING, "asr-client: WARN %d recognition hint replacement line(s) are not \"from => to\"",
			hints.invalid_lines);
	if (!context.isEmpty())
		obs_log(LOG_INFO,
			"asr-client: sending recognition hints: profile=%s domain=%d chars hotwords=%d replacements=%d",
			context.contains(QStringLiteral("profile")) ? "yes" : "no", tea_hints_chars(domain, domainKeep),
			hints.hotword_count, hints.pair_count);

	{
		QMutexLocker lock(&hintsMutex_);
		hintsStatus_.report = report;
		hintsStatus_.invalid_lines = hints.invalid_lines;
		memcpy(hintsStatus_.invalid, hints.invalid, sizeof(hintsStatus_.invalid));
		hintsStatus_.file_error = fileError;
	}
	tea_hints_free(&hints);
	return context;
}

void TeaAsrClient::fetchDictionaries(tea_asr_dictionaries_done_t done, void *param)
{
	{
		QMutexLocker lock(&dictMutex_);
		dictState_ = TEA_DICT_FETCHING;
	}
	if (!QMetaObject::invokeMethod(
		    this, [this, done, param]() { doFetchDictionaries(done, param); }, Qt::QueuedConnection) &&
	    done)
		done(param);
}

void TeaAsrClient::doFetchDictionaries(tea_asr_dictionaries_done_t done, void *param)
{
	if (!monotonic_.isValid())
		monotonic_.start();
	/* A newer request replaces an older one still in flight. */
	if (dictDone_) {
		tea_asr_dictionaries_done_t previous = dictDone_;
		dictDone_ = nullptr;
		previous(dictParam_);
	}
	dictDone_ = done;
	dictParam_ = param;
	const quint64 gen = ++dictGen_;

	/* The token is read without touching the change-detection stamp
	 * readToken() keeps for the connection loop. */
	QString token;
	{
		QFile f(tokenFilePath());
		if (f.open(QIODevice::ReadOnly | QIODevice::Text))
			token = QString::fromUtf8(f.readAll()).trimmed();
	}
	if (token.isEmpty()) {
		finishDictionaries(TEA_DICT_FAILED, {}, QStringLiteral("no token"));
		return;
	}
	if (!nam_)
		nam_ = new QNetworkAccessManager(this);
	QUrl url;
	url.setScheme(QStringLiteral("http"));
	url.setHost(host_);
	url.setPort(port_);
	url.setPath(QStringLiteral("/v1/dictionaries"));
	QNetworkRequest req(url);
	req.setRawHeader("Authorization", "Bearer " + token.toUtf8());
	req.setTransferTimeout(5000);
	QNetworkReply *reply = nam_->get(req);
	reply->setProperty("teaDictGen", QVariant::fromValue(gen));
	connect(reply, &QNetworkReply::finished, this, &TeaAsrClient::onDictionariesReply);
}

void TeaAsrClient::onDictionariesReply()
{
	auto *reply = qobject_cast<QNetworkReply *>(sender());
	if (!reply)
		return;
	reply->deleteLater();
	if (reply->property("teaDictGen").value<quint64>() != dictGen_)
		return; /* superseded; its caller was already told */
	const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
	const QByteArray body = reply->readAll();
	if (httpStatus != 200) {
		const QString why = httpStatus == 0 ? reply->errorString() : QStringLiteral("HTTP %1").arg(httpStatus);
		obs_log(LOG_INFO, "asr-client: GET /v1/dictionaries failed (%s)", why.toUtf8().constData());
		finishDictionaries(TEA_DICT_FAILED, {}, why);
		return;
	}
	const QJsonDocument doc = QJsonDocument::fromJson(body);
	const QJsonArray list = doc.isArray() ? doc.array()
					      : doc.object().value(QStringLiteral("dictionaries")).toArray();
	if (!doc.isArray() && !doc.object().value(QStringLiteral("dictionaries")).isArray()) {
		finishDictionaries(TEA_DICT_FAILED, {}, QStringLiteral("unexpected response"));
		return;
	}
	std::vector<tea_dictionary_entry_t> entries;
	for (const QJsonValue &v : list) {
		const QJsonObject o = v.toObject();
		const QByteArray name = o.value(QStringLiteral("name")).toString().toUtf8();
		if (name.isEmpty() || entries.size() >= TEA_DICT_MAX)
			continue;
		tea_dictionary_entry_t e;
		memset(&e, 0, sizeof(e));
		snprintf(e.name, sizeof(e.name), "%s", name.constData());
		e.hotwords_count = o.value(QStringLiteral("hotwords_count")).toInt(0);
		e.replacements_count = o.value(QStringLiteral("replacements_count")).toInt(0);
		entries.push_back(e);
	}
	obs_log(LOG_INFO, "asr-client: the server offers %d recognition hint dictionaries", (int)entries.size());
	finishDictionaries(TEA_DICT_OK, entries, QString());
}

void TeaAsrClient::finishDictionaries(int state, const std::vector<tea_dictionary_entry_t> &entries,
				      const QString &error)
{
	{
		QMutexLocker lock(&dictMutex_);
		dictState_ = state;
		if (state == TEA_DICT_OK)
			dictEntries_ = entries;
		dictError_ = error;
		dictDoneMs_ = nowMs();
	}
	if (dictDone_) {
		tea_asr_dictionaries_done_t done = dictDone_;
		dictDone_ = nullptr;
		done(dictParam_);
	}
}

int TeaAsrClient::dictionaries(tea_dictionary_entry_t *out, int max, int *count, QString *error, qint64 *ageMs) const
{
	QMutexLocker lock(&dictMutex_);
	int n = 0;
	for (const auto &e : dictEntries_) {
		if (n >= max)
			break;
		out[n++] = e;
	}
	if (count)
		*count = n;
	if (error)
		*error = dictError_;
	if (ageMs)
		*ageMs = dictDoneMs_ < 0 || !monotonic_.isValid() ? -1 : monotonic_.elapsed() - dictDoneMs_;
	return dictState_;
}

void TeaAsrClient::diagnostics(int *connection, int *speech, double *input_dbfs, bool *input_recent,
			       int64_t *ms_since_text) const
{
	const qint64 now = monotonic_.isValid() ? monotonic_.elapsed() : 0;
	const qint64 lastInput = lastInputMs_.load();
	const qint64 lastText = lastTextMsAtomic_.load();
	*connection = connState_.load();
	*speech = speechStateAtomic_.load();
	*input_recent = lastInput >= 0 && now - lastInput <= 1000;
	*input_dbfs = *input_recent ? inputDbfsTenths_.load() / 10.0 : TEA_DBFS_FLOOR;
	*ms_since_text = lastText >= 0 ? now - lastText : -1;
}

/* ---------------- diagnostics (docs/diagnostics.md) ---------------- */

static const char *tea_conn_name(int state)
{
	switch (state) {
	case TEA_CONN_WAITING_AUDIO:
		return "waiting for an audio source";
	case TEA_CONN_WAITING_TOKEN:
		return "waiting for the token file";
	case TEA_CONN_CONNECTING:
		return "connecting";
	case TEA_CONN_CONNECTED:
		return "connected";
	case TEA_CONN_ACTIVE:
		return "session active";
	case TEA_CONN_RECONNECTING:
		return "reconnecting";
	default:
		return "stopped";
	}
}

static const char *tea_speech_name(int state)
{
	switch (state) {
	case TEA_SPEECH_DETECTED:
		return "speech detected";
	case TEA_SPEECH_PROCESSING:
		return "segment processing";
	default:
		return "listening";
	}
}

void TeaAsrClient::setConnState(int state, const QString &reason)
{
	const int before = connState_.exchange(state);
	if (before == state && state != TEA_CONN_RECONNECTING)
		return;
	obs_log(state == TEA_CONN_RECONNECTING || state == TEA_CONN_STOPPED ? LOG_WARNING : LOG_INFO,
		"asr-client: connection %s%s%s", tea_conn_name(state), reason.isEmpty() ? "" : ": ",
		reason.toUtf8().constData());
}

void TeaAsrClient::diagOnSessionStarted(const QJsonObject &started)
{
	const qint64 now = nowMs();
	tea_warn_session_start(&warn_, now);
	tea_heartbeat_reset(&heartbeat_, now);
	tea_speech_reset(&speech_);
	speechStateAtomic_ = TEA_SPEECH_LISTENING;
	lastTextMs_ = -1;
	lastTextMsAtomic_ = -1;
	const QJsonObject policy = started.value(QStringLiteral("preview_policy")).toObject();
	setConnState(TEA_CONN_ACTIVE, QStringLiteral("%1:%2 sent %3; server: transcript_mode=%4 endpoint_silence_ms=%5 "
						     "min_interval_ms=%6")
					      .arg(host_)
					      .arg(port_)
					      .arg(QString::fromUtf8(lastSessionStartLog_))
					      .arg(started.value(QStringLiteral("transcript_mode")).toString())
					      .arg(policy.value(QStringLiteral("endpoint_silence_ms")).toInt(-1))
					      .arg(policy.value(QStringLiteral("min_interval_ms")).toInt(-1)));
}

void TeaAsrClient::diagOnEvent(const QString &type)
{
	const qint64 now = nowMs();
	const QByteArray utf8 = type.toUtf8();
	const int kind = tea_event_kind(utf8.constData());
	heartbeat_.events[kind]++;
	tea_warn_on_server_event(&warn_, now);
	switch (kind) {
	case TEA_EV_SPEECH_STARTED:
		tea_speech_on_started(&speech_);
		tea_warn_on_speech(&warn_);
		break;
	case TEA_EV_SEGMENT_QUEUED:
		tea_speech_on_queued(&speech_);
		break;
	case TEA_EV_PARTIAL:
	case TEA_EV_STABLE:
	case TEA_EV_FINAL:
		lastTextMs_ = now;
		lastTextMsAtomic_ = now;
		tea_warn_on_speech(&warn_);
		if (kind == TEA_EV_FINAL)
			tea_speech_on_segment_done(&speech_);
		break;
	default:
		if (type == QLatin1String("segment.skipped") || type == QLatin1String("segment.error"))
			tea_speech_on_segment_done(&speech_);
		break;
	}
	speechStateAtomic_ = tea_speech_state(&speech_);
}

void TeaAsrClient::logHeartbeat(qint64 now)
{
	const tea_heartbeat_t &h = heartbeat_;
	const double window_s = (double)(now - h.start_ms) / 1000.0;
	const QString sinceText =
		lastTextMs_ >= 0 ? QStringLiteral("%1 s ago").arg((double)(now - lastTextMs_) / 1000.0, 0, 'f', 1)
				 : QStringLiteral("none yet");
	obs_log(LOG_INFO,
		"asr-client: heartbeat %.1fs: sent %llu ms audio in %llu frames, max gap %lld ms, level rms %.1f dBFS "
		"peak %.1f dBFS; events partial=%u stable=%u final=%u speech.started=%u segment.queued=%u "
		"audio.ack=%u other=%u; last text %s; speech state %s",
		window_s, (unsigned long long)(h.samples / 16), (unsigned long long)h.frames,
		(long long)tea_heartbeat_max_gap(&h, now), tea_pcm_level_rms_dbfs(&h.level),
		tea_pcm_level_peak_dbfs(&h.level), h.events[TEA_EV_PARTIAL], h.events[TEA_EV_STABLE],
		h.events[TEA_EV_FINAL], h.events[TEA_EV_SPEECH_STARTED], h.events[TEA_EV_SEGMENT_QUEUED],
		h.events[TEA_EV_AUDIO_ACK], h.events[TEA_EV_OTHER], sinceText.toUtf8().constData(),
		tea_speech_name(tea_speech_state(&speech_)));
}

void TeaAsrClient::diagTick()
{
	const qint64 now = nowMs();
	/* input level for the overlay: last ~0.5 s pulled from the source */
	if (now - inputLevelStartMs_ >= 500) {
		if (inputLevel_.samples > 0)
			inputDbfsTenths_ = (int)lround(tea_pcm_level_rms_dbfs(&inputLevel_) * 10.0);
		tea_pcm_level_reset(&inputLevel_);
		inputLevelStartMs_ = now;
	}
	if (!sessionStarted_)
		return;
	const int due = tea_warn_due(&warn_, now);
	if (due & TEA_WARN_NO_AUDIO)
		obs_log(LOG_WARNING,
			"asr-client: WARN no audio sent for %lld ms while connected -- the audio source is muted, "
			"deactivated, removed or not producing audio",
			(long long)(now - (warn_.last_send_ms >= 0 ? warn_.last_send_ms : warn_.session_start_ms)));
	if (due & TEA_WARN_QUIET)
		obs_log(LOG_WARNING,
			"asr-client: WARN audio is being sent but has been below %.0f dBFS for %lld ms -- the source "
			"is silent or its volume is down",
			TEA_WARN_QUIET_DBFS, (long long)(now - warn_.quiet_since_ms));
	if (due & TEA_WARN_NO_SPEECH)
		obs_log(LOG_WARNING,
			"asr-client: WARN %lld ms of audible audio sent without speech.started or any transcript -- the "
			"server hears no speech in it (music/noise? compare the server's VAD heartbeat)",
			(long long)warn_.audible_without_speech_ms);
	if (due & TEA_WARN_STALLED)
		obs_log(LOG_WARNING,
			"asr-client: WARN sending audio but no event from the server for %lld ms -- the connection or "
			"the server is stalled",
			(long long)(now - warn_.last_server_ms));
	if (tea_heartbeat_due(&heartbeat_, now)) {
		logHeartbeat(now);
		tea_heartbeat_reset(&heartbeat_, now);
	}
}

/* ---------------- optional event trace ---------------- */

void TeaAsrClient::traceOpen(const QJsonObject &started)
{
	QMutexLocker lock(&traceMutex_);
	if (traceDir_.isEmpty() || traceFile_)
		return;
	if (!QDir().mkpath(traceDir_)) {
		obs_log(LOG_WARNING, "asr-client: event trace: cannot create %s", traceDir_.toUtf8().constData());
		return;
	}
	const QString name = QStringLiteral("tea-trace-%1-%2.jsonl")
				     .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")))
				     .arg(sessionId_.left(8));
	traceFile_ = new QFile(QDir(traceDir_).filePath(name));
	if (!traceFile_->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		obs_log(LOG_WARNING, "asr-client: event trace: cannot open %s",
			traceFile_->fileName().toUtf8().constData());
		delete traceFile_;
		traceFile_ = nullptr;
		return;
	}
	traceBytes_ = 0;
	traceT0_ = lastHelloMs_ >= 0 ? lastHelloMs_ : nowMs();
	obs_log(LOG_INFO, "asr-client: event trace for session %s: %s (up to %lld MB)", sessionId_.toUtf8().constData(),
		traceFile_->fileName().toUtf8().constData(), (long long)(kTraceMaxBytes / (1024 * 1024)));
	QJsonObject meta;
	meta.insert(QStringLiteral("session_start"), QJsonDocument::fromJson(lastSessionStart_).object());
	meta.insert(QStringLiteral("server"), QStringLiteral("%1:%2").arg(host_).arg(port_));
	meta.insert(QStringLiteral("captured_at"), QDateTime::currentDateTime().toString(Qt::ISODate));
	QJsonObject header;
	header.insert(QStringLiteral("t_ms"), 0);
	header.insert(QStringLiteral("meta"), meta);
	/* header line (no "event": the replay tool skips it), then what came before */
	QByteArray line = QJsonDocument(header).toJson(QJsonDocument::Compact) + '\n';
	traceFile_->write(line);
	traceBytes_ += line.size();
	lock.unlock();
	if (!lastHello_.isEmpty())
		traceWrite(lastHello_);
	traceWrite(started);
}

void TeaAsrClient::traceWrite(const QJsonObject &event)
{
	QMutexLocker lock(&traceMutex_);
	if (!traceFile_)
		return;
	if (traceBytes_ >= kTraceMaxBytes) {
		obs_log(LOG_WARNING, "asr-client: event trace reached %lld MB, stopped writing: %s",
			(long long)(kTraceMaxBytes / (1024 * 1024)), traceFile_->fileName().toUtf8().constData());
		traceFile_->close();
		delete traceFile_;
		traceFile_ = nullptr;
		return;
	}
	QJsonObject line;
	const bool isHello = event.value(QStringLiteral("type")).toString() == QLatin1String("hello");
	line.insert(QStringLiteral("t_ms"), (double)((isHello ? lastHelloMs_ : nowMs()) - traceT0_));
	line.insert(QStringLiteral("event"), event);
	QByteArray bytes = QJsonDocument(line).toJson(QJsonDocument::Compact) + '\n';
	traceFile_->write(bytes);
	traceBytes_ += bytes.size();
}

void TeaAsrClient::traceClose()
{
	QMutexLocker lock(&traceMutex_);
	if (!traceFile_)
		return;
	traceFile_->close();
	delete traceFile_;
	traceFile_ = nullptr;
}

bool TeaAsrClient::supportsSegmentationControl(int *min_ms, int *max_ms, int *default_ms) const
{
	const bool supported = serverSupportsSegmentation_.load(std::memory_order_relaxed);
	if (min_ms)
		*min_ms = segmentationMin_.load(std::memory_order_relaxed);
	if (max_ms)
		*max_ms = segmentationMax_.load(std::memory_order_relaxed);
	if (default_ms)
		*default_ms = segmentationDefault_.load(std::memory_order_relaxed);
	return supported;
}

int TeaAsrClient::effectiveEndSilenceMs() const
{
	return effectiveEndSilence_.load(std::memory_order_relaxed);
}

void TeaAsrClient::holdOrClearCaptions(bool clearOtherwise)
{
	if (!captions_)
		return;
	if (!stablePreferred_) {
		if (clearOtherwise)
			tea_caption_state_reset(captions_);
		return;
	}
	/* A reconnect must not blank a live caption: keep the committed text
	 * frozen until the next session's text scrolls in under it, but not
	 * forever -- see kStaleCaptionMs. */
	tea_caption_state_hold_for_reconnect(captions_);
	if (staleCaptionTimer_ && !staleCaptionTimer_->isActive())
		staleCaptionTimer_->start(kStaleCaptionMs);
}

void TeaAsrClient::onStaleCaptionTimer()
{
	if (captions_ && !sessionStarted_)
		tea_caption_state_reset(captions_);
}

void TeaAsrClient::setStatus(const QString &text)
{
	{
		QMutexLocker lock(&statusMutex_);
		statusText_ = text;
	}
}

void TeaAsrClient::resetProtocolStateLocked()
{
	recvBuffer_.clear();
	handshakeDone_ = false;
	fragmentActive_ = false;
	fragPayload_.clear();
	helloReceived_ = false;
	sessionStartSent_ = false;
	sessionStarted_ = false;
	sessionId_.clear();
	stableRequested_ = false;
	stableActive_ = false;
	stableMismatchLogged_ = false;
	segmentErrors_ = 0;
	lastSegmentErrorLogMs_ = -1;
	lastPreviewStatus_.clear();
	lastPreviewLogMs_ = -1;
	previewChangesUnlogged_ = 0;
	segmentationRequested_ = 0;
	effectiveEndSilence_ = -1;
	nextSeq_ = 0;
	nextSample_ = 0;
	sendUntilSample_ = 0;
	pendingPcm_.clear();
}

void TeaAsrClient::doStart()
{
	/* An explicit (re)start -- the source's own settings changing or the
	 * user pressing "Reconnect All" -- always deserves a fresh attempt with
	 * a fresh backoff, even if a previous connection gave up permanently
	 * (e.g. a non-retryable server error or a spent auth budget).
	 *
	 * Tear the old attempt down *without* clearing wantRunning_: doStop()
	 * clears it, and calling it here used to leave a restarted client that
	 * never reconnected again after its next disconnect. */
	wantRunning_ = true;
	if (!monotonic_.isValid())
		monotonic_.start();
	if (reconnectTimer_)
		reconnectTimer_->stop();
	if (watchTimer_)
		watchTimer_->stop();
	if (staleCaptionTimer_)
		staleCaptionTimer_->stop();
	teardownSocket(true);
	if (captions_)
		tea_caption_state_reset(captions_);

	errorPolicy_.reset();
	backoff_.reset();
	/* Settings change / "Reconnect All": ask for stable captions again even
	 * if a previous server rejected them. */
	stableRejected_ = false;
	segmentationRejected_ = false;
	contextRejected_ = false;
	{
		QMutexLocker lock(&hintsMutex_);
		hintsStatus_.rejected = false;
		hintsStatus_.reject_reason[0] = '\0';
		hintsStatus_.applied = false;
		hintsStatus_.sent = false;
	}

	if (!pumpTimer_) {
		pumpTimer_ = new QTimer(this);
		pumpTimer_->setInterval(50);
		connect(pumpTimer_, &QTimer::timeout, this, &TeaAsrClient::onPumpTimer);
	}
	pumpTimer_->start();

	if (!reconnectTimer_) {
		reconnectTimer_ = new QTimer(this);
		reconnectTimer_->setSingleShot(true);
		connect(reconnectTimer_, &QTimer::timeout, this, &TeaAsrClient::onReconnectTimer);
	}
	if (!watchTimer_) {
		watchTimer_ = new QTimer(this);
		connect(watchTimer_, &QTimer::timeout, this, &TeaAsrClient::onWatchTimer);
	}
	if (!staleCaptionTimer_) {
		staleCaptionTimer_ = new QTimer(this);
		staleCaptionTimer_->setSingleShot(true);
		connect(staleCaptionTimer_, &QTimer::timeout, this, &TeaAsrClient::onStaleCaptionTimer);
	}

	beginAttempt();
}

void TeaAsrClient::doStop()
{
	wantRunning_ = false;
	if (reconnectTimer_)
		reconnectTimer_->stop();
	if (watchTimer_)
		watchTimer_->stop();
	if (pumpTimer_)
		pumpTimer_->stop();
	if (staleCaptionTimer_)
		staleCaptionTimer_->stop();
	waitMode_ = WaitMode::None;
	attemptGen_++; /* orphan any in-flight preflight reply */

	teardownSocket(true);
	setStatus(QStringLiteral("stopped"));
	setConnState(TEA_CONN_STOPPED, QStringLiteral("stopped by the source"));
	if (captions_)
		tea_caption_state_reset(captions_);
}

void TeaAsrClient::teardownSocket(bool sendClose)
{
	if (socket_) {
		if (sendClose && socket_->state() == QAbstractSocket::ConnectedState && handshakeDone_)
			sendCloseFrame(1000);
		socket_->disconnect(this);
		socket_->abort();
		socket_->deleteLater();
		socket_ = nullptr;
	}
	connected_ = false;
	socketEverConnected_ = false;
	connectStartMs_ = -1;
	lastRxMs_ = -1;
	sessionStartedMs_ = -1;
	traceClose();
	tea_warn_session_end(&warn_);
	tea_speech_reset(&speech_);
	speechStateAtomic_ = TEA_SPEECH_LISTENING;
	resetProtocolStateLocked();
}

void TeaAsrClient::beginAttempt()
{
	if (!wantRunning_)
		return;
	attemptGen_++;
	teardownSocket(false);
	errorPolicy_.beginAttempt();
	capabilitiesKnown_ = false;
	serverSupportsStable_ = false;
	serverSupportsSegmentation_ = false;
	waitMode_ = WaitMode::None;
	if (watchTimer_)
		watchTimer_->stop();

	/* A caption source with no audio source selected would only hold one of
	 * the server's few connection / continuous-session slots
	 * (max_total_connections, max_continuous_sessions) and then be ended by
	 * its idle_timeout every 120 s. Wait locally instead. */
	if (tap_ && !tea_audio_tap_has_source(tap_)) {
		setStatus(QStringLiteral("waiting: no audio source selected for this caption source"));
		setConnState(TEA_CONN_WAITING_AUDIO,
			     QStringLiteral("no audio source selected for this caption source"));
		waitMode_ = WaitMode::AudioSource;
		watchTimer_->start(1000);
		return;
	}

	/* A missing or empty (revoked) token can never authenticate; sending it
	 * anyway would only spend the server's auth-failure budget. */
	const QString token = readToken();
	if (token.isEmpty()) {
		errorPolicy_.observeNoToken(tokenFilePath().toStdString());
		setStatus(QStringLiteral("waiting: %1 (will connect when the token file changes)")
				  .arg(QString::fromStdString(errorPolicy_.lastErrorSummary())));
		setConnState(TEA_CONN_WAITING_TOKEN, QString::fromStdString(errorPolicy_.lastErrorSummary()));
		waitMode_ = WaitMode::TokenFile;
		watchTimer_->start(tea_asr::ReconnectBackoff::kNoTokenPollMs);
		return;
	}

	setStatus(QStringLiteral("checking server (GET /v1/capabilities)"));
	setConnState(TEA_CONN_CONNECTING, QStringLiteral("http://%1:%2").arg(host_).arg(port_));
	fetchCapabilities(token);
}

void TeaAsrClient::failAttempt()
{
	holdOrClearCaptions(false);
	teardownSocket(false);
	scheduleReconnect();
}

void TeaAsrClient::scheduleReconnect()
{
	if (!wantRunning_)
		return;

	if (errorPolicy_.fatal()) {
		/* The server told us -- via the `error` event's own `retryable`
		 * field -- that this will keep failing. Retrying would just burn
		 * one of the server's max_total_connections slots forever while
		 * leaving the user staring at an opaque "reconnecting" label. Stop
		 * and say why instead. */
		const QString fatalReason = QString::fromStdString(errorPolicy_.fatalReason());
		setStatus(fatalReason.isEmpty() ? QStringLiteral("stopped: server rejected this connection")
						: fatalReason);
		setConnState(TEA_CONN_STOPPED,
			     fatalReason.isEmpty() ? QStringLiteral("server rejected this connection") : fatalReason);
		return;
	}

	const tea_asr::FailureClass cls = errorPolicy_.failureClass();
	if (errorPolicy_.lastErrorSummary().empty())
		errorPolicy_.observeTransport("connection lost");
	int delayMs = backoff_.nextDelayMs(cls);

	if (delayMs < 0) {
		/* Auth budget spent: every further try would be one more failure
		 * toward the server's rate limit, for a token that is known bad. */
		setStatus(QStringLiteral("stopped: %1 -- fix the token file (%2) or press Reconnect All; "
					 "retries automatically when the token file changes")
				  .arg(QString::fromStdString(errorPolicy_.lastErrorSummary()), tokenFilePath()));
		setConnState(TEA_CONN_WAITING_TOKEN, QString::fromStdString(errorPolicy_.lastErrorSummary()));
		waitMode_ = WaitMode::TokenFile;
		watchTimer_->start(tea_asr::ReconnectBackoff::kNoTokenPollMs);
		return;
	}

	if (cls == tea_asr::FailureClass::Transient) {
		/* +-10% jitter so several caption sources that lost the same server
		 * do not reconnect in lockstep. */
		delayMs += int(QRandomGenerator::global()->bounded(delayMs / 5 + 1)) - delayMs / 10;
	}

	/* Always surface the real reason alongside the countdown, never a bare
	 * "reconnecting" label that gives the user nothing to act on. */
	setStatus(QString::fromStdString(errorPolicy_.reconnectStatus(delayMs)));
	setConnState(TEA_CONN_RECONNECTING,
		     QStringLiteral("%1; retry in %2 s%3")
			     .arg(QString::fromStdString(errorPolicy_.lastErrorSummary()))
			     .arg((double)delayMs / 1000.0, 0, 'f', 1)
			     .arg(lastCloseCode_ ? QStringLiteral(" (last close code %1)").arg(lastCloseCode_)
						 : QString()));
	lastCloseCode_ = 0;
	reconnectTimer_->start(delayMs);

	if (cls == tea_asr::FailureClass::Auth) {
		/* A fixed token should not have to wait out a 60 s backoff. */
		waitMode_ = WaitMode::TokenFile;
		watchTimer_->start(tea_asr::ReconnectBackoff::kNoTokenPollMs);
	}
}

void TeaAsrClient::onReconnectTimer()
{
	if (!wantRunning_)
		return;
	beginAttempt();
}

void TeaAsrClient::onWatchTimer()
{
	if (!wantRunning_) {
		watchTimer_->stop();
		return;
	}
	switch (waitMode_) {
	case WaitMode::AudioSource:
		if (!tap_ || tea_audio_tap_has_source(tap_))
			beginAttempt();
		break;
	case WaitMode::TokenFile:
		if (tokenFileChanged()) {
			obs_log(LOG_INFO, "asr-client: token file changed; reconnecting");
			backoff_.resetAuth();
			if (reconnectTimer_)
				reconnectTimer_->stop();
			beginAttempt();
		}
		break;
	case WaitMode::None:
		watchTimer_->stop();
		break;
	}
}

/* ---------------- HTTP preflight ---------------- */

void TeaAsrClient::fetchCapabilities(const QString &token)
{
	if (!nam_)
		nam_ = new QNetworkAccessManager(this);

	QUrl url;
	url.setScheme(QStringLiteral("http"));
	url.setHost(host_);
	url.setPort(port_);
	url.setPath(QStringLiteral("/v1/capabilities"));

	/* /v1/capabilities requires the bearer token like every /v1 route. An
	 * unauthenticated probe is answered 401 *and* counted as an auth failure
	 * by the server's rate limiter. */
	QNetworkRequest req(url);
	req.setRawHeader("Authorization", "Bearer " + token.toUtf8());
	req.setTransferTimeout(5000);
	QNetworkReply *reply = nam_->get(req);
	reply->setProperty("teaAttempt", QVariant::fromValue(attemptGen_));
	connect(reply, &QNetworkReply::finished, this, &TeaAsrClient::onCapabilitiesReply);
}

void TeaAsrClient::onCapabilitiesReply()
{
	auto *reply = qobject_cast<QNetworkReply *>(sender());
	if (!reply)
		return;
	reply->deleteLater();
	if (!wantRunning_ || reply->property("teaAttempt").value<quint64>() != attemptGen_)
		return; /* superseded by a newer attempt or a stop */

	const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
	const QByteArray body = reply->readAll();

	if (httpStatus == 0) {
		errorPolicy_.observeTransport(QStringLiteral("server unreachable at %1:%2 (%3)")
						      .arg(host_)
						      .arg(port_)
						      .arg(reply->errorString())
						      .toStdString());
		failAttempt();
		return;
	}

	QJsonObject obj = QJsonDocument::fromJson(body).object();
	if (httpStatus != 200) {
		QJsonObject err = obj.value(QStringLiteral("error")).toObject();
		const std::string code = err.value(QStringLiteral("code")).toString().toStdString();
		const std::string message = err.value(QStringLiteral("message")).toString().toStdString();
		obs_log(LOG_WARNING, "asr-client: preflight GET /v1/capabilities -> HTTP %d (%s)", httpStatus,
			code.c_str());
		errorPolicy_.observePreflightHttp(httpStatus, code, message);
		failAttempt();
		return;
	}

	QJsonObject features = obj.value(QStringLiteral("features")).toObject();
	QJsonObject limits = obj.value(QStringLiteral("limits")).toObject();
	serverSupportsPartial_ = features.value(QStringLiteral("partial_transcripts")).toBool(false);
	/* Optional field: absent (older server, or revisable preview off) means
	 * no transcript.stable, and the client stays on partial/final. */
	serverSupportsStable_ = features.value(QStringLiteral("stable_transcripts")).toBool(false);
	/* Optional too: segmentation_control.end_silence_ms = {min, max, default}.
	 * Absent means the server keeps its own end silence and would reject a
	 * `segmentation` field. */
	const QJsonObject silence = features.value(QStringLiteral("segmentation_control"))
					    .toObject()
					    .value(QStringLiteral("end_silence_ms"))
					    .toObject();
	const int silenceMin = silence.value(QStringLiteral("min")).toInt(0);
	const int silenceMax = silence.value(QStringLiteral("max")).toInt(0);
	segmentationMin_ = silenceMin;
	segmentationMax_ = silenceMax;
	segmentationDefault_ = silence.value(QStringLiteral("default")).toInt(0);
	serverSupportsSegmentation_ = silenceMin > 0 && silenceMax >= silenceMin;
	readHintsCapability(features);
	const int hintsCapability = hintsCapability_.load();
	obs_log(LOG_INFO,
		"asr-client: capabilities: partial_transcripts=%d stable_transcripts=%d segmentation=%s "
		"context_biasing=%s",
		serverSupportsPartial_.load() ? 1 : 0, serverSupportsStable_.load() ? 1 : 0,
		serverSupportsSegmentation_.load()
			? QStringLiteral("%1-%2 ms").arg(silenceMin).arg(silenceMax).toUtf8().constData()
			: "not offered",
		hintsCapability == TEA_HINTS_CAP_ON    ? "on"
		: hintsCapability == TEA_HINTS_CAP_OFF ? "off on the server"
						       : "not offered");
	maxTotalConnections_ = limits.value(QStringLiteral("max_total_connections")).toInt(0);
	/* W9 LAN mode: every response carries this header. Surface it; the
	 * bearer token is travelling in cleartext. */
	insecureLan_ = !reply->rawHeader("X-TEA-ASR-Security").isEmpty();
	capabilitiesKnown_ = true;

	openWebSocket();
}

/* ---------------- token ---------------- */

QString TeaAsrClient::tokenFilePath() const
{
	if (!tokenPath_.isEmpty())
		return tokenPath_;
#if defined(Q_OS_MAC)
	return QDir::homePath() + QStringLiteral("/Library/Application Support/TEA ASR/token");
#else
	return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/TEA ASR/token");
#endif
}

QString TeaAsrClient::readToken()
{
	const QString path = tokenFilePath();
	QFileInfo info(path);
	tokenStampValid_ = info.exists();
	tokenMtime_ = tokenStampValid_ ? info.lastModified() : QDateTime();
	tokenSize_ = tokenStampValid_ ? info.size() : -1;

	QFile f(path);
	if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
		return QString();
	/* Never log file contents; a missing/unreadable token is only ever
	 * surfaced as a status, never as logged text. */
	return QString::fromUtf8(f.readAll()).trimmed();
}

bool TeaAsrClient::tokenFileChanged() const
{
	QFileInfo info(tokenFilePath());
	if (info.exists() != tokenStampValid_)
		return true;
	if (!info.exists())
		return false;
	return info.lastModified() != tokenMtime_ || info.size() != tokenSize_;
}

/* ---------------- WS handshake ---------------- */

void TeaAsrClient::openWebSocket()
{
	setStatus(QStringLiteral("connecting (WebSocket)"));
	socket_ = new QTcpSocket(this);
	connect(socket_, &QTcpSocket::connected, this, &TeaAsrClient::onSocketConnected);
	connect(socket_, &QTcpSocket::readyRead, this, &TeaAsrClient::onSocketReadyRead);
	connect(socket_, &QTcpSocket::disconnected, this, &TeaAsrClient::onSocketDisconnected);
	connect(socket_, &QTcpSocket::errorOccurred, this, &TeaAsrClient::onSocketError);
	connectStartMs_ = nowMs();
	socket_->connectToHost(host_, (quint16)port_);
}

void TeaAsrClient::onSocketConnected()
{
	socketEverConnected_ = true;
	sendHandshakeRequest();
}

void TeaAsrClient::sendHandshakeRequest()
{
	QByteArray keyRaw(16, 0);
	for (int i = 0; i < 16; i++)
		keyRaw[i] = char(QRandomGenerator::global()->bounded(256));
	QByteArray key = keyRaw.toBase64();

	wsAcceptExpected_ = QCryptographicHash::hash(key + QByteArray(kWsGuid), QCryptographicHash::Sha1).toBase64();

	/* No Origin header: the server treats an absent Origin as a native
	 * client, while any Origin must match its browser allowlist. The Host
	 * header's port is ignored by the server's allowlist; IPv6 literals
	 * need brackets to be a well-formed Host. */
	const QByteArray hostHeader = host_.contains(QLatin1Char(':')) ? "[" + host_.toUtf8() + "]" : host_.toUtf8();

	QByteArray req;
	req += "GET /v1/stream HTTP/1.1\r\n";
	req += "Host: " + hostHeader + ":" + QByteArray::number(port_) + "\r\n";
	req += "Upgrade: websocket\r\n";
	req += "Connection: Upgrade\r\n";
	req += "Sec-WebSocket-Key: " + key + "\r\n";
	req += "Sec-WebSocket-Version: 13\r\n";

	/* Re-read: the token may have been rotated since the preflight; the
	 * server follows its token file live. */
	QString token = readToken();
	if (!token.isEmpty())
		req += "Authorization: Bearer " + token.toUtf8() + "\r\n";

	req += "\r\n";
	socket_->write(req);
}

TeaAsrClient::HandshakeResult TeaAsrClient::tryConsumeHandshakeResponse()
{
	int headerEnd = recvBuffer_.indexOf("\r\n\r\n");
	if (headerEnd < 0)
		return HandshakeResult::NeedMore;

	QByteArray header = recvBuffer_.left(headerEnd);
	recvBuffer_.remove(0, headerEnd + 4);

	QByteArray headerLower = header.toLower();
	int statusCode = 0;
	{
		int lineEnd = header.indexOf("\r\n");
		QList<QByteArray> parts = header.left(lineEnd < 0 ? header.size() : lineEnd).split(' ');
		if (parts.size() >= 2 && parts[0].startsWith("HTTP/"))
			statusCode = parts[1].toInt();
	}

	int acceptIdx = headerLower.indexOf("sec-websocket-accept:");
	QByteArray acceptValue;
	if (acceptIdx >= 0) {
		int lineEnd = header.indexOf("\r\n", acceptIdx);
		if (lineEnd < 0)
			lineEnd = header.size();
		QByteArray line = header.mid(acceptIdx + (int)strlen("sec-websocket-accept:"),
					     lineEnd - acceptIdx - (int)strlen("sec-websocket-accept:"));
		acceptValue = line.trimmed();
	}

	if (statusCode != 101) {
		/* Pre-accept rejections (session_limit, and in a race also
		 * unauthenticated/rate_limited) arrive as a reason-less 403. */
		obs_log(LOG_WARNING, "asr-client: WebSocket upgrade rejected with HTTP %d", statusCode);
		errorPolicy_.observeHandshakeRejected(statusCode, maxTotalConnections_);
		failAttempt();
		return HandshakeResult::Rejected;
	}
	if (acceptValue.isEmpty() || acceptValue != wsAcceptExpected_) {
		obs_log(LOG_WARNING, "asr-client: WebSocket handshake invalid (Sec-WebSocket-Accept mismatch)");
		errorPolicy_.observeTransport("WebSocket handshake invalid (Sec-WebSocket-Accept mismatch)");
		failAttempt();
		return HandshakeResult::Rejected;
	}

	if (headerLower.contains("x-tea-asr-security:"))
		insecureLan_ = true;
	handshakeDone_ = true;
	connected_ = true;
	setStatus(QStringLiteral("connected, awaiting hello"));
	setConnState(TEA_CONN_CONNECTED, QStringLiteral("WebSocket open to %1:%2").arg(host_).arg(port_));
	return HandshakeResult::Accepted;
}

/* ---------------- WS framing ---------------- */

void TeaAsrClient::sendWsFrame(uint8_t opcode, const char *payload, qint64 len)
{
	if (!socket_ || socket_->state() != QAbstractSocket::ConnectedState)
		return;

	QByteArray frame;
	frame.append(char(0x80 | (opcode & 0x0F))); /* FIN=1 */

	if (len <= 125) {
		frame.append(char(0x80 | len));
	} else if (len <= 0xFFFF) {
		frame.append(char(0x80 | 126));
		frame.append(char((len >> 8) & 0xFF));
		frame.append(char(len & 0xFF));
	} else {
		frame.append(char(0x80 | 127));
		for (int i = 7; i >= 0; i--)
			frame.append(char((len >> (8 * i)) & 0xFF));
	}

	uint8_t mask[4];
	for (auto &b : mask)
		b = uint8_t(QRandomGenerator::global()->bounded(256));
	frame.append((const char *)mask, 4);

	QByteArray masked;
	masked.resize((int)len);
	for (qint64 i = 0; i < len; i++)
		masked[(int)i] = char(uint8_t(payload[i]) ^ mask[i % 4]);
	frame.append(masked);

	socket_->write(frame);
}

void TeaAsrClient::sendTextFrame(const QByteArray &utf8Json)
{
	sendWsFrame(0x1, utf8Json.constData(), utf8Json.size());
}

void TeaAsrClient::sendBinaryFrame(const QByteArray &bytes)
{
	sendWsFrame(0x2, bytes.constData(), bytes.size());
}

void TeaAsrClient::sendCloseFrame(uint16_t code)
{
	QByteArray payload;
	payload.append(char((code >> 8) & 0xFF));
	payload.append(char(code & 0xFF));
	sendWsFrame(0x8, payload.constData(), payload.size());
}

void TeaAsrClient::onSocketReadyRead()
{
	if (!socket_)
		return;
	recvBuffer_.append(socket_->readAll());
	lastRxMs_ = nowMs();

	if (!handshakeDone_) {
		if (tryConsumeHandshakeResponse() != HandshakeResult::Accepted)
			return; /* need more bytes, or the attempt was already torn down */
				/* fallthrough: any bytes left in recvBuffer_ after the header
		 * are already WS frame data. */
	}

	processIncomingWsBytes();
}

void TeaAsrClient::processIncomingWsBytes()
{
	while (true) {
		if (recvBuffer_.size() < 2)
			return;

		uint8_t b0 = uint8_t(recvBuffer_[0]);
		uint8_t b1 = uint8_t(recvBuffer_[1]);
		bool fin = (b0 & 0x80) != 0;
		uint8_t opcode = b0 & 0x0F;
		bool masked = (b1 & 0x80) != 0; /* server frames shouldn't be masked, but tolerate it */
		uint64_t len = b1 & 0x7F;

		int pos = 2;
		if (len == 126) {
			if (recvBuffer_.size() < pos + 2)
				return;
			len = (uint8_t(recvBuffer_[pos]) << 8) | uint8_t(recvBuffer_[pos + 1]);
			pos += 2;
		} else if (len == 127) {
			if (recvBuffer_.size() < pos + 8)
				return;
			len = 0;
			for (int i = 0; i < 8; i++)
				len = (len << 8) | uint8_t(recvBuffer_[pos + i]);
			pos += 8;
		}

		if (len > kMaxIncomingFramePayloadBytes) {
			/* A server we trust (local TEA ASR service) should never
			 * send anything close to this, but not checking at all
			 * means a misbehaving/compromised server can make
			 * recvBuffer_ grow without bound simply by claiming a
			 * huge length and never finishing the frame. Treat it as
			 * a protocol violation: stop parsing and drop the
			 * connection (a normal reconnect will follow). */
			obs_log(LOG_WARNING,
				"asr-client: incoming WS frame declares %llu byte payload (max %llu); "
				"dropping connection",
				(unsigned long long)len, (unsigned long long)kMaxIncomingFramePayloadBytes);
			recvBuffer_.clear();
			fragmentActive_ = false;
			fragPayload_.clear();
			if (socket_)
				socket_->disconnectFromHost();
			return;
		}

		uint8_t maskKey[4] = {0, 0, 0, 0};
		if (masked) {
			if (recvBuffer_.size() < pos + 4)
				return;
			for (int i = 0; i < 4; i++)
				maskKey[i] = uint8_t(recvBuffer_[pos + i]);
			pos += 4;
		}

		if ((uint64_t)recvBuffer_.size() < (uint64_t)pos + len)
			return; /* incomplete frame, wait for more bytes */

		QByteArray payload = recvBuffer_.mid(pos, (int)len);
		if (masked) {
			for (int i = 0; i < payload.size(); i++)
				payload[i] = char(uint8_t(payload[i]) ^ maskKey[i % 4]);
		}
		recvBuffer_.remove(0, pos + (int)len);

		if (opcode == 0x0) {
			/* continuation */
			fragPayload_.append(payload);
			if (fin) {
				handleWsFrame(fragOpcode_, fragPayload_);
				fragmentActive_ = false;
				fragPayload_.clear();
			}
		} else if (opcode == 0x1 || opcode == 0x2) {
			if (!fin) {
				fragmentActive_ = true;
				fragOpcode_ = opcode;
				fragPayload_ = payload;
			} else {
				handleWsFrame(opcode, payload);
			}
		} else {
			/* control frames (close/ping/pong) are never fragmented */
			handleWsFrame(opcode, payload);
		}
	}
}

void TeaAsrClient::handleWsFrame(uint8_t opcode, const QByteArray &payload)
{
	switch (opcode) {
	case 0x1: { /* text: JSON event */
		QJsonParseError err{};
		QJsonDocument doc = QJsonDocument::fromJson(payload, &err);
		if (err.error != QJsonParseError::NoError || !doc.isObject()) {
			obs_log(LOG_WARNING, "asr-client: dropped malformed JSON event from server");
			return;
		}
		handleJsonMessage(doc.object());
		break;
	}
	case 0x2:
		/* Server never sends binary in this protocol; ignore. */
		break;
	case 0x8: { /* close */
		uint16_t code = 0;
		if (payload.size() >= 2)
			code = (uint16_t(uint8_t(payload[0])) << 8) | uint16_t(uint8_t(payload[1]));
		obs_log(LOG_INFO, "asr-client: server closed the WebSocket (code=%u)", (unsigned)code);
		lastCloseCode_ = code;

		/* RFC 6455 5.5.1: a peer that receives a close frame must send
		 * one back (unless it already initiated the close itself). We
		 * never initiate our own close except from doStop(), which
		 * tears down the socket right after -- so any close we *receive*
		 * here is server-initiated and needs the echo. */
		if (handshakeDone_ && socket_ && socket_->state() == QAbstractSocket::ConnectedState)
			sendCloseFrame(code != 0 ? code : 1000);

		/* tea-asr-service reserves 4029 for admission rejection when the
		 * configured continuous-session limit is already in use. Normally an
		 * `error` event with code `concurrent_session_limit` arrives first and
		 * supplies the server's exact message. Keep that richer reason when it
		 * exists, but also classify the close code itself so a dropped/missing
		 * text frame never degrades into an unexplained reconnect loop. This is
		 * deliberately retryable: another source may disconnect at any time. */
		/* Likewise 4408 (idle_timeout) gets a synthesized, actionable reason
		 * if its error event was lost; any other close code only describes
		 * itself when no error event preceded it (see ErrorPolicy). */
		errorPolicy_.observeClose(code);
		if (!errorPolicy_.lastErrorSummary().empty() && !errorPolicy_.fatal()) {
			setStatus(QStringLiteral("server closed: %1")
					  .arg(QString::fromStdString(errorPolicy_.lastErrorSummary())));
		}

		/* Close code 1013 alone does NOT mean "fatal, don't retry": per
		 * tea-asr-service's errors.py, it covers `queue_full`,
		 * `session_limit` (both server-marked retryable=true -- transient
		 * backlog) and `slow_client` (retryable=false). The preceding
		 * `error` JSON event (handled in handleJsonMessage() below) already
		 * carried the real code, message and retryable flag and is what
		 * decides the error policy's fatal/reason state -- this close frame
		 * is just the transport-level teardown that follows it. Do not
		 * duplicate or override that decision here. Code 4029 is handled
		 * separately above because it has one unambiguous meaning. */
		if (socket_)
			socket_->disconnectFromHost();
		break;
	}
	case 0x9: { /* ping -> pong */
		sendWsFrame(0xA, payload.constData(), payload.size());
		break;
	}
	case 0xA: /* pong (transport-level, not the JSON "pong" event) */
		break;
	default:
		obs_log(LOG_WARNING, "asr-client: unknown WS opcode 0x%x", opcode);
		break;
	}
}

/* ---------------- protocol state machine (docs/04) ---------------- */

void TeaAsrClient::handleJsonMessage(const QJsonObject &obj)
{
	QString type = obj.value(QStringLiteral("type")).toString();
	diagOnEvent(type);
	if (type != QLatin1String("hello") && type != QLatin1String("session.started"))
		traceWrite(obj);

	if (type == QLatin1String("hello")) {
		lastHello_ = obj;
		lastHelloMs_ = nowMs();
		helloReceived_ = true;
		QString modelState = obj.value(QStringLiteral("model_state")).toString();
		setStatus(modelState.isEmpty() ? QStringLiteral("hello received") : modelState);
		maybeSendSessionStart();
		return;
	}

	if (type == QLatin1String("session.started")) {
		sessionStarted_ = true;
		sessionId_ = obj.value(QStringLiteral("session_id")).toString();
		nextSeq_ = (uint64_t)obj.value(QStringLiteral("next_seq")).toDouble(0);
		nextSample_ = (uint64_t)obj.value(QStringLiteral("next_sample")).toDouble(0);
		sendUntilSample_ = (uint64_t)obj.value(QStringLiteral("send_until_sample")).toDouble(0);
		sessionStartedMs_ = nowMs();
		const QString mode = obj.value(QStringLiteral("transcript_mode")).toString();
		stableActive_ = stableRequested_;
		const QJsonValue endpoint = obj.value(QStringLiteral("preview_policy"))
						    .toObject()
						    .value(QStringLiteral("endpoint_silence_ms"));
		effectiveEndSilence_ = endpoint.isDouble() ? endpoint.toInt(-1) : -1;
		if (staleCaptionTimer_)
			staleCaptionTimer_->stop();
		QString status = QStringLiteral("session active");
		if (!mode.isEmpty() && stableRequested_)
			status += QStringLiteral(" (%1, stable captions)").arg(mode);
		else if (!mode.isEmpty())
			status += QStringLiteral(" (%1)").arg(mode);
		else if (stableRequested_)
			status += QStringLiteral(" (stable captions)");
		if (effectiveEndSilence_ > 0)
			status += QStringLiteral(" (sentence break %1 ms)").arg(effectiveEndSilence_.load());
		if (stableRejected_)
			status += QStringLiteral(" -- server rejected stable captions, using partial previews");
		if (segmentationRejected_)
			status += QStringLiteral(" -- server rejected the sentence break setting, using its default");
		{
			/* session.started.context = {profile, domain_chars, hotwords_count,
			 * replacements_count, prompt_tokens?}: what the server applied. */
			const QJsonValue echo = obj.value(QStringLiteral("context"));
			QMutexLocker lock(&hintsMutex_);
			hintsStatus_.applied = contextRequested_ && echo.isObject();
			hintsStatus_.unconfirmed = contextRequested_ && !echo.isObject();
			if (hintsStatus_.applied) {
				const QJsonObject c = echo.toObject();
				snprintf(hintsStatus_.applied_profile, sizeof(hintsStatus_.applied_profile), "%s",
					 c.value(QStringLiteral("profile")).toString().toUtf8().constData());
				hintsStatus_.applied_domain_chars = c.value(QStringLiteral("domain_chars")).toInt(0);
				hintsStatus_.applied_hotwords = c.value(QStringLiteral("hotwords_count")).toInt(0);
				hintsStatus_.applied_replacements =
					c.value(QStringLiteral("replacements_count")).toInt(0);
				hintsStatus_.prompt_applied = c.value(QStringLiteral("prompt_applied")).toBool(false);
				const QJsonValue tokens = c.value(QStringLiteral("prompt_tokens"));
				hintsStatus_.applied_prompt_tokens = tokens.isDouble() ? tokens.toInt(-1) : -1;
				status += QStringLiteral(" (recognition hints: %1 hotwords, %2 replacements)")
						  .arg(hintsStatus_.applied_hotwords)
						  .arg(hintsStatus_.applied_replacements);
				obs_log(LOG_INFO,
					"asr-client: recognition hints applied: profile=%s domain=%d chars hotwords=%d "
					"replacements=%d prompt_applied=%d prompt_tokens=%d",
					hintsStatus_.applied_profile[0] ? "yes" : "none",
					hintsStatus_.applied_domain_chars, hintsStatus_.applied_hotwords,
					hintsStatus_.applied_replacements, hintsStatus_.prompt_applied ? 1 : 0,
					hintsStatus_.applied_prompt_tokens);
			}
		}
		if (contextRejected_)
			status += QStringLiteral(" -- server rejected the recognition hints, running without them");
		if (insecureLan_)
			status += QStringLiteral(
				" -- WARNING: server is in unencrypted LAN mode; token travels in cleartext");
		setStatus(status);
		diagOnSessionStarted(obj);
		traceOpen(obj);
		return;
	}

	if (type == QLatin1String("speech.started") || type == QLatin1String("segment.queued")) {
		/* Speech state for diagnostics only (diagOnEvent() above). */
		return;
	}

	if (type == QLatin1String("flow.control")) {
		uint64_t v = (uint64_t)obj.value(QStringLiteral("send_until_sample")).toDouble(0);
		if (v > sendUntilSample_) /* window never regresses */
			sendUntilSample_ = v;
		return;
	}

	if (type == QLatin1String("audio.ack")) {
		/* No resend-on-nack in v0.1. The server still taking audio keeps a
		 * still-open caption line up (TEA_OPEN_LINE_TIMEOUT_MS). */
		tea_caption_state_on_audio_ack(captions_,
					       (uint64_t)obj.value(QStringLiteral("received_sample")).toDouble(0));
		return;
	}

	if (type == QLatin1String("transcript.partial")) {
		QString segId = obj.value(QStringLiteral("segment_id")).toString();
		uint64_t rev = (uint64_t)obj.value(QStringLiteral("revision")).toDouble(0);
		QString text = obj.value(QStringLiteral("text")).toString();
		tea_caption_state_on_partial(captions_, sessionId_.toUtf8().constData(), segId.toUtf8().constData(),
					     rev, text.toUtf8().constData());
		return;
	}

	if (type == QLatin1String("transcript.final")) {
		QString segId = obj.value(QStringLiteral("segment_id")).toString();
		uint64_t rev = (uint64_t)obj.value(QStringLiteral("revision")).toDouble(1);
		const QJsonValue index = obj.value(QStringLiteral("segment_index"));
		uint64_t segIndex = index.isDouble() && index.toDouble() >= 0 ? (uint64_t)index.toDouble()
									      : TEA_CAPTION_SEGMENT_INDEX_UNKNOWN;
		QString text = obj.value(QStringLiteral("text")).toString();
		tea_caption_state_on_final_indexed(captions_, sessionId_.toUtf8().constData(),
						   segId.toUtf8().constData(), segIndex, rev,
						   text.toUtf8().constData());
		return;
	}

	if (type == QLatin1String("transcript.stable")) {
		/* Display only (docs/04): transcripts/exports stay on
		 * transcript.final. `text` is the segment's whole committed text. */
		QString segId = obj.value(QStringLiteral("segment_id")).toString();
		const QJsonValue index = obj.value(QStringLiteral("segment_index"));
		uint64_t segIndex = index.isDouble() && index.toDouble() >= 0 ? (uint64_t)index.toDouble()
									      : TEA_CAPTION_SEGMENT_INDEX_UNKNOWN;
		uint64_t stableRev = (uint64_t)obj.value(QStringLiteral("stable_revision")).toDouble(0);
		QString text = obj.value(QStringLiteral("text")).toString();
		QString state = obj.value(QStringLiteral("state")).toString();
		const uint64_t mismatchesBefore = tea_caption_state_stable_mismatches(captions_);
		tea_caption_state_on_stable(captions_, sessionId_.toUtf8().constData(), segId.toUtf8().constData(),
					    segIndex, stableRev, text.toUtf8().constData(), state.toUtf8().constData());
		if (!stableMismatchLogged_ && tea_caption_state_stable_mismatches(captions_) != mismatchesBefore) {
			/* Contract violation (old/buggy server): the displayed text was
			 * kept unchanged. Logged once per session; the running count is
			 * in the Tools dialog. Never log the transcript text itself. */
			stableMismatchLogged_ = true;
			obs_log(LOG_WARNING,
				"asr-client: transcript.stable (segment_index=%lld stable_revision=%llu state=%s) "
				"does not extend the displayed text; kept the displayed text unchanged",
				(long long)(segIndex == TEA_CAPTION_SEGMENT_INDEX_UNKNOWN ? -1 : (long long)segIndex),
				(unsigned long long)stableRev, state.toUtf8().constData());
		}
		return;
	}

	if (type == QLatin1String("segment.skipped") || type == QLatin1String("segment.error")) {
		QString segId = obj.value(QStringLiteral("segment_id")).toString();
		tea_caption_state_on_segment_dropped(captions_, sessionId_.toUtf8().constData(),
						     segId.toUtf8().constData());
		if (type == QLatin1String("segment.error")) {
			/* Every segment failing (e.g. code invalid_ipc: the server's
			 * worker is broken) means no captions at all: say so, at most
			 * once per 30 s, with a count. */
			segmentErrors_++;
			const qint64 now = nowMs();
			if (lastSegmentErrorLogMs_ < 0 || now - lastSegmentErrorLogMs_ >= TEA_WARN_REPEAT_MS) {
				obs_log(LOG_WARNING,
					"asr-client: WARN the server could not transcribe a segment: code=%s retryable=%d "
					"(%s); %llu segment error(s) this session",
					obj.value(QStringLiteral("code")).toString().toUtf8().constData(),
					obj.value(QStringLiteral("retryable")).toBool(false) ? 1 : 0,
					obj.value(QStringLiteral("message")).toString().toUtf8().constData(),
					(unsigned long long)segmentErrors_);
				lastSegmentErrorLogMs_ = now;
			}
		}
		return;
	}

	if (type == QLatin1String("preview.status")) {
		/* Server-side preview state. Logged when it changes, at most once per
		 * 30 s (load pauses may come and go), except that a pause for
		 * backend_error is always logged, as a warning. */
		const QString state = obj.value(QStringLiteral("state")).toString();
		const QString reason = obj.value(QStringLiteral("reason")).toString();
		const QString combined = state + QLatin1Char('/') + reason;
		if (combined != lastPreviewStatus_) {
			lastPreviewStatus_ = combined;
			const bool backendError = reason == QLatin1String("backend_error");
			const qint64 now = nowMs();
			if (backendError || lastPreviewLogMs_ < 0 || now - lastPreviewLogMs_ >= TEA_WARN_REPEAT_MS) {
				obs_log(backendError ? LOG_WARNING : LOG_INFO,
					"asr-client: %sserver preview %s: %s (%llu earlier change(s) not logged)",
					backendError ? "WARN " : "", state.toUtf8().constData(),
					reason.toUtf8().constData(), (unsigned long long)previewChangesUnlogged_);
				lastPreviewLogMs_ = now;
				previewChangesUnlogged_ = 0;
			} else {
				previewChangesUnlogged_++;
			}
		}
		return;
	}

	if (type == QLatin1String("session.cancelled")) {
		tea_caption_state_on_session_cancelled(captions_, sessionId_.toUtf8().constData());
		return;
	}

	if (type == QLatin1String("session.stopped")) {
		setStatus(QStringLiteral("session stopped"));
		return;
	}

	if (type == QLatin1String("pong")) {
		return; /* app-level keepalive reply, nothing to update */
	}

	if (type == QLatin1String("error")) {
		QString code = obj.value(QStringLiteral("code")).toString();
		QString message = obj.value(QStringLiteral("message")).toString();
		bool retryable = obj.value(QStringLiteral("retryable")).toBool(true);
		obs_log(LOG_WARNING, "asr-client: server error code=%s retryable=%d", code.toUtf8().constData(),
			retryable ? 1 : 0);

		if (contextRequested_ && !sessionStarted_ &&
		    tea_hints_error_is_context(code.toUtf8().constData(), message.toUtf8().constData())) {
			/* Recognition hints are an optional extra: a server that refuses
			 * them (an unknown profile, a limit, a server that stopped
			 * offering them) gets a new session without them, with every
			 * other setting kept. Checked first: `context` is the newest
			 * session.start field. */
			contextRejected_ = true;
			{
				QMutexLocker lock(&hintsMutex_);
				hintsStatus_.rejected = true;
				snprintf(hintsStatus_.reject_reason, sizeof(hintsStatus_.reject_reason), "%s%s%s",
					 code.toUtf8().constData(), message.isEmpty() ? "" : ": ",
					 message.left(120).toUtf8().constData());
			}
			obs_log(LOG_WARNING,
				"asr-client: WARN server rejected the recognition hints (%s); reconnecting without them",
				code.toUtf8().constData());
			errorPolicy_.observeError(code.toStdString(), message.toStdString(), true);
			setStatus(
				QStringLiteral("server rejected the recognition hints (%1); reconnecting without them")
					.arg(code));
			return;
		}

		if (segmentationRequested_ > 0 && !sessionStarted_ &&
		    (code == QLatin1String("unsupported_option") || code == QLatin1String("protocol_error"))) {
			/* Advertised segmentation_control but refused the field (or the
			 * value): the sentence break setting is optional, so reconnect
			 * with the server's default silence. Checked before `stable`:
			 * it is the newer field, so the likelier culprit. */
			segmentationRejected_ = true;
			obs_log(LOG_WARNING,
				"asr-client: server rejected session.start.segmentation (%s); using its default",
				code.toUtf8().constData());
			errorPolicy_.observeError(code.toStdString(), message.toStdString(), true);
			setStatus(
				QStringLiteral("server rejected the sentence break setting (%1); reconnecting with its "
					       "default")
					.arg(code));
			return;
		}

		if (stableRequested_ && !sessionStarted_ &&
		    (code == QLatin1String("unsupported_option") || code == QLatin1String("protocol_error"))) {
			/* The server advertised stable_transcripts but refused the
			 * `stable` field (an older build answers protocol_error, a
			 * misconfigured one unsupported_option). Stable captions are an
			 * optional extra: reconnect without them instead of treating the
			 * rejection as fatal. If the retry fails the same way, the
			 * regular (possibly fatal) handling below applies. */
			stableRejected_ = true;
			obs_log(LOG_WARNING,
				"asr-client: server rejected stable captions (%s); "
				"falling back to transcript.partial",
				code.toUtf8().constData());
			errorPolicy_.observeError(code.toStdString(), message.toStdString(), true);
			setStatus(QStringLiteral(
					  "server rejected stable captions (%1); reconnecting with partial previews")
					  .arg(code));
			return;
		}

		/* Trust the server's own `retryable` flag exclusively -- do not
		 * hardcode any particular `code` (e.g. "session_limit") as fatal.
		 * Cross-checked against tea-asr-service: "session_limit" means this
		 * session's own pending-segment queue filled up (a transient backlog),
		 * while "concurrent_session_limit" is admission rejection for an
		 * already-full continuous-session pool. Always preserve the server's
		 * code + message instead of inventing a diagnosis. */
		errorPolicy_.observeError(code.toStdString(), message.toStdString(), retryable);
		QString summary = QString::fromStdString(errorPolicy_.lastErrorSummary());

		if (!retryable) {
			setStatus(QString::fromStdString(errorPolicy_.fatalReason()));
		} else {
			setStatus(QStringLiteral("server error: %1").arg(summary));
		}
		/* timeline_gap and other protocol errors close the socket on the
		 * server side right after this; onSocketDisconnected() is what
		 * actually tears down session state and reconnects with a brand
		 * new session, per docs/04 (v0.1 never resumes) -- unless the
		 * error above was marked fatal, in which case scheduleReconnect()
		 * will stop instead of looping. */
		return;
	}

	obs_log(LOG_INFO, "asr-client: unhandled event type '%s'", type.toUtf8().constData());
}

void TeaAsrClient::maybeSendSessionStart()
{
	if (sessionStartSent_ || !helloReceived_)
		return;
	/* The authenticated preflight completes before the WebSocket is even
	 * opened, so transcript_mode is always chosen from real capabilities. */
	sendSessionStart();
}

void TeaAsrClient::sendSessionStart()
{
	if (sessionStartSent_)
		return;
	sessionStartSent_ = true;

	QJsonObject audio;
	audio.insert(QStringLiteral("sample_rate"), 16000);
	audio.insert(QStringLiteral("channels"), 1);
	audio.insert(QStringLiteral("format"), QStringLiteral("pcm_s16le"));

	QJsonObject start;
	start.insert(QStringLiteral("type"), QStringLiteral("session.start"));
	start.insert(QStringLiteral("request_id"), QStringLiteral("start-1"));
	start.insert(QStringLiteral("profile"), QStringLiteral("continuous"));
	start.insert(QStringLiteral("audio"), audio);
	start.insert(QStringLiteral("durable"), false);
	if (serverSupportsPartial_)
		start.insert(QStringLiteral("transcript_mode"), QStringLiteral("revisable"));
	/* `stable` requires transcript_mode=revisable (else unsupported_option),
	 * and is only sent when the server advertises it. */
	stableRequested_ = serverSupportsPartial_ && serverSupportsStable_ && stablePreferred_ && !stableRejected_;
	if (stableRequested_) {
		QJsonObject stable;
		stable.insert(QStringLiteral("agreement"), 2);
		start.insert(QStringLiteral("stable"), stable);
	}
	if (captions_)
		tea_caption_state_set_stable_mode(captions_, stableRequested_);
	/* continuous profile only (it is the only profile this client uses) */
	segmentationRequested_ = segmentationRejected_
					 ? 0
					 : tea_end_silence_request(endSilencePreferred_.load(),
								   serverSupportsSegmentation_.load(),
								   segmentationMin_.load(), segmentationMax_.load());
	if (segmentationRequested_ > 0) {
		QJsonObject segmentation;
		segmentation.insert(QStringLiteral("end_silence_ms"), segmentationRequested_);
		start.insert(QStringLiteral("segmentation"), segmentation);
	}

	/* Recognition hints: only to a server that offers them, and not again
	 * after it refused them (until an explicit restart). */
	contextRequested_ = false;
	if (hintsCapability_.load() == TEA_HINTS_CAP_ON && !contextRejected_) {
		const QJsonObject context = buildContext();
		if (!context.isEmpty()) {
			start.insert(QStringLiteral("context"), context);
			contextRequested_ = true;
		}
	}
	{
		QMutexLocker lock(&hintsMutex_);
		hintsStatus_.sent = contextRequested_;
		hintsStatus_.applied = false;
		hintsStatus_.unconfirmed = false;
	}

	lastSessionStart_ = QJsonDocument(start).toJson(QJsonDocument::Compact);
	sendTextFrame(lastSessionStart_);
	/* The OBS log gets the session.start with the hint text replaced by
	 * counts: hints are prompt text (see the note at the top). */
	if (start.contains(QStringLiteral("context"))) {
		const QJsonObject context = start.value(QStringLiteral("context")).toObject();
		start.insert(QStringLiteral("context"),
			     QJsonObject{{QStringLiteral("profile"), context.contains(QStringLiteral("profile"))},
					 {QStringLiteral("domain_chars"),
					  (int)context.value(QStringLiteral("domain")).toString().toUcs4().size()},
					 {QStringLiteral("hotwords"),
					  (int)context.value(QStringLiteral("hotwords")).toArray().size()},
					 {QStringLiteral("replacements"),
					  (int)context.value(QStringLiteral("replacements")).toArray().size()}});
	}
	lastSessionStartLog_ = QJsonDocument(start).toJson(QJsonDocument::Compact);
}

void TeaAsrClient::onPumpTimer()
{
	pumpAudio();
	diagTick();

	if (!socket_)
		return;
	const qint64 now = nowMs();
	if (!handshakeDone_ && connectStartMs_ >= 0 && now - connectStartMs_ > kHandshakeTimeoutMs) {
		errorPolicy_.observeTransport("WebSocket connect/handshake timed out after 10 s");
		failAttempt();
		return;
	}
	if (handshakeDone_ && lastRxMs_ >= 0 && now - lastRxMs_ > kServerSilenceMs) {
		/* The server pings every 15 s; this long without a single byte means
		 * the peer is gone (sleep, network change) even though TCP has not
		 * noticed yet. */
		obs_log(LOG_WARNING, "asr-client: no data from server for %lld ms; dropping connection",
			(long long)(now - lastRxMs_));
		errorPolicy_.observeTransport("no data from server for 45 s (missed WebSocket heartbeat)");
		failAttempt();
	}
}

void TeaAsrClient::pumpAudio()
{
	if (!tap_)
		return;

	/* Drain whatever the ring buffer has, chunked to the wire's per-frame
	 * PCM limit, regardless of whether we can send yet -- flow control
	 * only gates transmission, never how much we're willing to buffer
	 * from the audio tap (that already has its own drop-oldest ring). */
	int16_t chunkBuf[kMaxFramePcmBytes / sizeof(int16_t)];
	for (int guard = 0; guard < 32; guard++) {
		size_t got = tea_audio_tap_pull_pcm16(tap_, chunkBuf, kMaxFramePcmBytes / sizeof(int16_t));
		if (got == 0)
			break;
		tea_pcm_level_add(&inputLevel_, chunkBuf, got);
		lastInputMs_ = nowMs();

		QByteArray chunk((const char *)chunkBuf, (int)(got * sizeof(int16_t)));
		if (pendingPcm_.size() >= kMaxPendingPcmChunks) {
			uint64_t evictedSamples = (uint64_t)pendingPcm_.front().size() / sizeof(int16_t);
			pendingPcm_.pop_front();
			droppedPcmSamples_.fetch_add(evictedSamples, std::memory_order_relaxed);
		}
		pendingPcm_.push_back(chunk);
	}

	if (!sessionStarted_ || !socket_ || socket_->state() != QAbstractSocket::ConnectedState)
		return;

	while (!pendingPcm_.empty()) {
		const QByteArray &chunk = pendingPcm_.front();
		uint64_t frameSamples = (uint64_t)chunk.size() / sizeof(int16_t);
		uint64_t endSample = nextSample_ + frameSamples;
		if (endSample > sendUntilSample_)
			break; /* wait for more flow-control window */

		QByteArray frame;
		frame.reserve(16 + chunk.size());
		appendLeU64(frame, nextSeq_);
		appendLeU64(frame, nextSample_);
		frame.append(chunk);

		sendBinaryFrame(frame);

		const qint64 now = nowMs();
		const int16_t *pcm = reinterpret_cast<const int16_t *>(chunk.constData());
		tea_pcm_level_t frameLevel;
		tea_pcm_level_reset(&frameLevel);
		tea_pcm_level_add(&frameLevel, pcm, (size_t)frameSamples);
		tea_heartbeat_on_send(&heartbeat_, now, pcm, (size_t)frameSamples);
		tea_warn_on_send(&warn_, now, (int64_t)(frameSamples / 16), tea_pcm_level_rms_dbfs(&frameLevel));

		nextSeq_++;
		nextSample_ = endSample;
		pendingPcm_.pop_front();
	}
}

void TeaAsrClient::onSocketDisconnected()
{
	holdOrClearCaptions(true);

	/* A session that ran for a while resets the backoff; one that failed
	 * quickly (or ended by idle_timeout, i.e. no audio) keeps escalating it,
	 * so a server that accepts then immediately drops us is not hammered. */
	if (sessionStartedMs_ >= 0 && tea_asr::ReconnectBackoff::sessionWasHealthy(nowMs() - sessionStartedMs_,
										   errorPolicy_.lastServerErrorCode()))
		backoff_.reset();

	errorPolicy_.observeTransport(handshakeDone_ ? "connection closed" : "connection closed during handshake");
	failAttempt();
}

void TeaAsrClient::onSocketError(QAbstractSocket::SocketError error)
{
	(void)error;
	if (!socket_)
		return;
	/* A socket that never connected (refused, unreachable, DNS failure)
	 * does not emit disconnected(), so the retry must be scheduled here --
	 * otherwise one refused connect would end reconnecting for good. A
	 * connected socket's error is followed by disconnected(), which
	 * handles it. */
	if (!socketEverConnected_) {
		errorPolicy_.observeTransport(
			QStringLiteral("WebSocket connect failed: %1").arg(socket_->errorString()).toStdString());
		failAttempt();
	}
}

/* ================= plain-C facade ================= */

struct tea_asr_client {
	TeaAsrClient *impl;
};

extern "C" tea_asr_client_t *tea_asr_client_create(tea_audio_tap_t *tap, tea_caption_state_t *captions)
{
	tea_asr_client_t *c = (tea_asr_client_t *)bmalloc(sizeof(tea_asr_client_t));
	c->impl = new TeaAsrClient(tap, captions);
	return c;
}

extern "C" void tea_asr_client_destroy(tea_asr_client_t *client)
{
	if (!client)
		return;
	delete client->impl;
	bfree(client);
}

extern "C" void tea_asr_client_set_server(tea_asr_client_t *client, const char *host, int port)
{
	client->impl->setServer(QString::fromUtf8(host), port);
}

extern "C" void tea_asr_client_set_token_path(tea_asr_client_t *client, const char *token_path)
{
	client->impl->setTokenPath(QString::fromUtf8(token_path ? token_path : ""));
}

extern "C" void tea_asr_client_start(tea_asr_client_t *client)
{
	client->impl->start();
}

extern "C" void tea_asr_client_stop(tea_asr_client_t *client)
{
	client->impl->stop();
}

extern "C" bool tea_asr_client_is_connected(tea_asr_client_t *client)
{
	return client->impl->isConnected();
}

extern "C" char *tea_asr_client_status_text(tea_asr_client_t *client)
{
	QByteArray utf8 = client->impl->statusText().toUtf8();
	return bstrdup(utf8.constData());
}

extern "C" uint64_t tea_asr_client_dropped_audio_frames(tea_asr_client_t *client)
{
	if (!client)
		return 0;
	return client->impl->droppedFrames();
}

extern "C" bool tea_asr_client_capabilities_known(tea_asr_client_t *client)
{
	return client && client->impl->capabilitiesKnown();
}

extern "C" bool tea_asr_client_supports_partial_transcripts(tea_asr_client_t *client)
{
	return client && client->impl->supportsPartialTranscripts();
}

extern "C" void tea_asr_client_set_stable_captions(tea_asr_client_t *client, bool enabled)
{
	if (client)
		client->impl->setStableCaptions(enabled);
}

extern "C" bool tea_asr_client_supports_stable_transcripts(tea_asr_client_t *client)
{
	return client && client->impl->supportsStableTranscripts();
}

extern "C" bool tea_asr_client_stable_captions_active(tea_asr_client_t *client)
{
	return client && client->impl->stableCaptionsActive();
}

extern "C" void tea_asr_client_set_end_silence_ms(tea_asr_client_t *client, int ms)
{
	if (client)
		client->impl->setEndSilenceMs(ms);
}

extern "C" bool tea_asr_client_supports_segmentation(tea_asr_client_t *client, int *min_ms, int *max_ms,
						     int *default_ms)
{
	if (!client) {
		if (min_ms)
			*min_ms = 0;
		if (max_ms)
			*max_ms = 0;
		if (default_ms)
			*default_ms = 0;
		return false;
	}
	return client->impl->supportsSegmentationControl(min_ms, max_ms, default_ms);
}

extern "C" int tea_asr_client_effective_end_silence_ms(tea_asr_client_t *client)
{
	return client ? client->impl->effectiveEndSilenceMs() : -1;
}

extern "C" void tea_asr_client_get_diag(tea_asr_client_t *client, tea_asr_client_diag_t *out)
{
	if (!out)
		return;
	memset(out, 0, sizeof(*out));
	out->input_dbfs = TEA_DBFS_FLOOR;
	out->ms_since_text = -1;
	if (!client)
		return;
	client->impl->diagnostics(&out->connection, &out->speech, &out->input_dbfs, &out->input_recent,
				  &out->ms_since_text);
}

extern "C" void tea_asr_client_set_trace_dir(tea_asr_client_t *client, const char *dir)
{
	if (client)
		client->impl->setTraceDir(QString::fromUtf8(dir ? dir : ""));
}

extern "C" void tea_asr_client_set_hints(tea_asr_client_t *client, const char *profile, const char *domain,
					 const char *hotwords, const char *replacements, const char *file_path)
{
	if (!client)
		return;
	auto text = [](const char *s) {
		return QString::fromUtf8(s ? s : "");
	};
	client->impl->setHints(text(profile), text(domain), text(hotwords), text(replacements), text(file_path));
}

extern "C" void tea_asr_client_get_hints_status(tea_asr_client_t *client, tea_asr_client_hints_status_t *out)
{
	if (!out)
		return;
	memset(out, 0, sizeof(*out));
	out->applied_prompt_tokens = -1;
	if (client)
		client->impl->hintsStatus(out);
}

extern "C" void tea_asr_client_fetch_dictionaries(tea_asr_client_t *client, tea_asr_dictionaries_done_t done,
						  void *param)
{
	if (!client) {
		if (done)
			done(param);
		return;
	}
	client->impl->fetchDictionaries(done, param);
}

extern "C" int tea_asr_client_dictionaries(tea_asr_client_t *client, tea_dictionary_entry_t *out, int max, int *count,
					   char *error, size_t error_size, int64_t *age_ms)
{
	if (count)
		*count = 0;
	if (error && error_size)
		error[0] = '\0';
	if (age_ms)
		*age_ms = -1;
	if (!client)
		return TEA_DICT_UNKNOWN;
	QString why;
	qint64 age = -1;
	const int state = client->impl->dictionaries(out, out ? max : 0, count, &why, &age);
	if (error && error_size)
		snprintf(error, error_size, "%s", why.toUtf8().constData());
	if (age_ms)
		*age_ms = age;
	return state;
}
