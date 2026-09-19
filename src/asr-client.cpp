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

void TeaAsrClient::setStatus(const QString &text)
{
	{
		QMutexLocker lock(&statusMutex_);
		statusText_ = text;
	}
	if (captions_)
		tea_caption_state_set_status(captions_, text.toUtf8().constData());
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
	nextSeq_ = 0;
	nextSample_ = 0;
	sendUntilSample_ = 0;
	pendingPcm_.clear();
}

void TeaAsrClient::doStart()
{
	if (socket_) {
		/* Already connecting/connected; a restart is just stop+start. */
		doStop();
	}

	resetProtocolStateLocked();
	connected_ = false;
	reconnectAttempt_ = 0;
	/* An explicit (re)start -- whether from the source's own settings
	 * changing or a user-initiated "reconnect" -- always deserves a fresh
	 * attempt, even if a previous connection gave up permanently (e.g. a
	 * non-retryable server error). */
	fatalNonRetryable_ = false;
	fatalReason_.clear();
	lastErrorSummary_.clear();
	setStatus(QStringLiteral("connecting"));

	socket_ = new QTcpSocket(this);
	connect(socket_, &QTcpSocket::connected, this, &TeaAsrClient::onSocketConnected);
	connect(socket_, &QTcpSocket::readyRead, this, &TeaAsrClient::onSocketReadyRead);
	connect(socket_, &QTcpSocket::disconnected, this, &TeaAsrClient::onSocketDisconnected);
	connect(socket_, &QTcpSocket::errorOccurred, this, &TeaAsrClient::onSocketError);

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

	fetchCapabilities();

	socket_->connectToHost(host_, (quint16)port_);
}

void TeaAsrClient::doStop()
{
	wantRunning_ = false;
	if (reconnectTimer_)
		reconnectTimer_->stop();
	if (pumpTimer_)
		pumpTimer_->stop();

	if (socket_) {
		if (socket_->state() == QAbstractSocket::ConnectedState && handshakeDone_)
			sendCloseFrame(1000);
		socket_->disconnect(this);
		socket_->close();
		socket_->deleteLater();
		socket_ = nullptr;
	}

	connected_ = false;
	resetProtocolStateLocked();
	setStatus(QStringLiteral("stopped"));
	if (captions_)
		tea_caption_state_reset(captions_);
}

void TeaAsrClient::scheduleReconnect()
{
	if (!wantRunning_)
		return;

	if (fatalNonRetryable_) {
		/* The server told us -- via the `error` event's own `retryable`
		 * field -- that this will keep failing. Retrying would just burn
		 * one of docs/04's max_total_connections=5 slots every ~16s
		 * forever, while leaving the user staring at an opaque
		 * "reconnecting" label. Stop and say why instead. */
		setStatus(fatalReason_.isEmpty() ? QStringLiteral("stopped: server rejected this connection")
						 : fatalReason_);
		return;
	}

	int delayMs = 1000 << qMin(reconnectAttempt_, 4); /* 1,2,4,8,16s cap */
	reconnectAttempt_++;
	/* Always surface the real, server-supplied reason (if we have one)
	 * alongside the backoff countdown, instead of a bare "reconnecting"
	 * label that gives the user nothing to act on. Most reasons here are
	 * transient (e.g. this session's own pending-segment queue was
	 * temporarily full) and are expected to clear on their own, so we keep
	 * retrying rather than giving up -- but we never retry silently. */
	setStatus(lastErrorSummary_.isEmpty()
			  ? QStringLiteral("reconnecting in %1ms").arg(delayMs)
			  : QStringLiteral("reconnecting in %1ms (reason: %2)").arg(delayMs).arg(lastErrorSummary_));
	reconnectTimer_->start(delayMs);
}

void TeaAsrClient::onReconnectTimer()
{
	if (!wantRunning_)
		return;
	doStart();
}

/* ---------------- capabilities probe ---------------- */

void TeaAsrClient::fetchCapabilities()
{
	if (!nam_)
		nam_ = new QNetworkAccessManager(this);

	QUrl url;
	url.setScheme(QStringLiteral("http"));
	url.setHost(host_);
	url.setPort(port_);
	url.setPath(QStringLiteral("/v1/capabilities"));

	QNetworkRequest req(url);
	QNetworkReply *reply = nam_->get(req);
	connect(reply, &QNetworkReply::finished, this, &TeaAsrClient::onCapabilitiesReply);
}

void TeaAsrClient::onCapabilitiesReply()
{
	auto *reply = qobject_cast<QNetworkReply *>(sender());
	if (!reply)
		return;
	reply->deleteLater();

	if (reply->error() != QNetworkReply::NoError) {
		capabilitiesKnown_ = true; /* don't block session.start forever */
		serverSupportsPartial_ = false;
		maybeSendSessionStart();
		return;
	}

	QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
	QJsonObject obj = doc.object();
	QJsonObject features = obj.value(QStringLiteral("features")).toObject();
	serverSupportsPartial_ = features.value(QStringLiteral("partial_transcripts")).toBool(false);
	capabilitiesKnown_ = true;
	maybeSendSessionStart();
}

/* ---------------- token ---------------- */

QString TeaAsrClient::readToken() const
{
	QString path = tokenPath_;
	if (path.isEmpty()) {
#if defined(Q_OS_MAC)
		path = QDir::homePath() + QStringLiteral("/Library/Application Support/TEA ASR/token");
#else
		path = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
		       QStringLiteral("/TEA ASR/token");
#endif
	}

	QFile f(path);
	if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
		return QString();
	/* Never log file contents; a missing/unreadable token is only ever
	 * surfaced as a connection failure, never as logged text. */
	return QString::fromUtf8(f.readAll()).trimmed();
}

/* ---------------- WS handshake ---------------- */

void TeaAsrClient::onSocketConnected()
{
	sendHandshakeRequest();
}

void TeaAsrClient::sendHandshakeRequest()
{
	QByteArray keyRaw(16, 0);
	for (int i = 0; i < 16; i++)
		keyRaw[i] = char(QRandomGenerator::global()->bounded(256));
	QByteArray key = keyRaw.toBase64();

	wsAcceptExpected_ = QCryptographicHash::hash(key + QByteArray(kWsGuid), QCryptographicHash::Sha1).toBase64();

	QByteArray req;
	req += "GET /v1/stream HTTP/1.1\r\n";
	req += "Host: " + host_.toUtf8() + ":" + QByteArray::number(port_) + "\r\n";
	req += "Upgrade: websocket\r\n";
	req += "Connection: Upgrade\r\n";
	req += "Sec-WebSocket-Key: " + key + "\r\n";
	req += "Sec-WebSocket-Version: 13\r\n";

	QString token = readToken();
	if (!token.isEmpty())
		req += "Authorization: Bearer " + token.toUtf8() + "\r\n";

	req += "\r\n";
	socket_->write(req);
}

bool TeaAsrClient::tryConsumeHandshakeResponse()
{
	int headerEnd = recvBuffer_.indexOf("\r\n\r\n");
	if (headerEnd < 0)
		return false; /* keep buffering */

	QByteArray header = recvBuffer_.left(headerEnd);
	recvBuffer_.remove(0, headerEnd + 4);

	QByteArray headerLower = header.toLower();
	bool is101 = header.startsWith("HTTP/1.1 101") || header.startsWith("HTTP/1.0 101");

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

	if (!is101 || acceptValue.isEmpty() || acceptValue != wsAcceptExpected_) {
		obs_log(LOG_WARNING, "asr-client: WebSocket handshake rejected/invalid (status/accept mismatch)");
		setStatus(QStringLiteral("handshake failed"));
		socket_->disconnectFromHost();
		return false;
	}

	handshakeDone_ = true;
	connected_ = true;
	reconnectAttempt_ = 0;
	setStatus(QStringLiteral("connected, awaiting hello"));
	return true;
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
	recvBuffer_.append(socket_->readAll());

	if (!handshakeDone_) {
		if (!tryConsumeHandshakeResponse())
			return;
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

		/* Close code 1013 alone does NOT mean "fatal, don't retry": per
		 * tea-asr-service's errors.py, it covers `queue_full`,
		 * `session_limit` (both server-marked retryable=true -- transient
		 * backlog) and `slow_client` (retryable=false). The preceding
		 * `error` JSON event (handled in handleJsonMessage() below) already
		 * carried the real code, message and retryable flag and is what
		 * decides fatalNonRetryable_/lastErrorSummary_ -- this close frame
		 * is just the transport-level teardown that follows it. Do not
		 * duplicate or override that decision here. */
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
		setStatus(QStringLiteral("session active"));
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
		QString text = obj.value(QStringLiteral("text")).toString();
		tea_caption_state_on_final(captions_, sessionId_.toUtf8().constData(), segId.toUtf8().constData(), rev,
					   text.toUtf8().constData());
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

		/* Trust the server's own `retryable` flag exclusively -- do not
		 * hardcode any particular `code` (e.g. "session_limit") as fatal.
		 * Cross-checked against tea-asr-service (docs/m2-reverification.md):
		 * the server does not enforce max_continuous_sessions at all, and
		 * "session_limit" actually means this session's own pending-segment
		 * queue filled up (a transient backlog), which the server itself
		 * marks retryable=true. Inventing a diagnosis the server never sent
		 * (e.g. "another source is connected") would just mislead the user
		 * when the real cause is unrelated. Always show the server's own
		 * code + message verbatim instead. */
		QString summary = message.isEmpty() ? code : QStringLiteral("%1: %2").arg(code, message);
		lastErrorSummary_ = summary;

		if (!retryable) {
			fatalNonRetryable_ = true;
			fatalReason_ = QStringLiteral("stopped: server error (%1)").arg(summary);
			setStatus(fatalReason_);
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
	if (!capabilitiesKnown_) {
		/* Give the local HTTP capabilities probe a brief moment; docs/04
		 * gives us 5s total to send session.start, so waiting a few
		 * hundred ms is safe and avoids guessing wrong about
		 * transcript_mode on a server that actually supports revisable
		 * previews. */
		QTimer::singleShot(300, this, [this]() { sendSessionStart(); });
		return;
	}
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

	sendTextFrame(QJsonDocument(start).toJson(QJsonDocument::Compact));
}

void TeaAsrClient::onPumpTimer()
{
	pumpAudio();
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
	connected_ = false;
	if (captions_) {
		tea_caption_state_reset(captions_);
	}
	setStatus(QStringLiteral("disconnected"));
	resetProtocolStateLocked();

	if (socket_) {
		socket_->deleteLater();
		socket_ = nullptr;
	}

	scheduleReconnect();
}

void TeaAsrClient::onSocketError(QAbstractSocket::SocketError error)
{
	(void)error;
	/* QTcpSocket emits disconnected() after most errors too; if the
	 * connection never got established at all it still fires this and
	 * we schedule a reconnect from here explicitly. */
	if (socket_ && socket_->state() != QAbstractSocket::ConnectedState) {
		setStatus(QStringLiteral("connection error: %1").arg(socket_->errorString()));
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
