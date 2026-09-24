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

#include <QCryptographicHash>
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
	waitMode_ = WaitMode::None;
	if (watchTimer_)
		watchTimer_->stop();

	/* A caption source with no audio source selected would only hold one of
	 * the server's few connection / continuous-session slots
	 * (max_total_connections, max_continuous_sessions) and then be ended by
	 * its idle_timeout every 120 s. Wait locally instead. */
	if (tap_ && !tea_audio_tap_has_source(tap_)) {
		setStatus(QStringLiteral("waiting: no audio source selected for this caption source"));
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
		waitMode_ = WaitMode::TokenFile;
		watchTimer_->start(tea_asr::ReconnectBackoff::kNoTokenPollMs);
		return;
	}

	setStatus(QStringLiteral("checking server (GET /v1/capabilities)"));
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

	if (type == QLatin1String("hello")) {
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
		if (staleCaptionTimer_)
			staleCaptionTimer_->stop();
		QString status = QStringLiteral("session active");
		if (!mode.isEmpty() && stableRequested_)
			status += QStringLiteral(" (%1, stable captions)").arg(mode);
		else if (!mode.isEmpty())
			status += QStringLiteral(" (%1)").arg(mode);
		else if (stableRequested_)
			status += QStringLiteral(" (stable captions)");
		if (stableRejected_)
			status += QStringLiteral(" -- server rejected stable captions, using partial previews");
		if (insecureLan_)
			status += QStringLiteral(
				" -- WARNING: server is in unencrypted LAN mode; token travels in cleartext");
		setStatus(status);
		return;
	}

	if (type == QLatin1String("flow.control")) {
		uint64_t v = (uint64_t)obj.value(QStringLiteral("send_until_sample")).toDouble(0);
		if (v > sendUntilSample_) /* window never regresses */
			sendUntilSample_ = v;
		return;
	}

	if (type == QLatin1String("audio.ack")) {
		/* Diagnostics only in v0.1 (no resend-on-nack); nothing to do. */
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

	sendTextFrame(QJsonDocument(start).toJson(QJsonDocument::Compact));
}

void TeaAsrClient::onPumpTimer()
{
	pumpAudio();

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
