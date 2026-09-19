#pragma once

#include <QObject>
#include <QString>
#include <QByteArray>
#include <QTcpSocket>
#include <QTimer>
#include <QThread>
#include <QMutex>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonObject>

#include <atomic>
#include <cstdint>
#include <deque>

#include "audio-tap.h"
#include "caption-state.h"

/*
 * Hand-rolled RFC 6455 client over QTcpSocket.
 *
 * Why not a WebSocket library: OBS's bundled Qt6 has no QtWebSockets
 * module (confirmed against the installed OBS.app/Contents/Frameworks),
 * and vendoring a third-party WS library (e.g. IXWebSocket) means getting
 * it to build cleanly across the macOS/Windows/Linux CI matrix used by
 * this repo's GitHub Actions workflows, on top of codesign/notarize
 * concerns on macOS. QTcpSocket + QCryptographicHash (for the handshake's
 * SHA-1) are already linked in via Qt6::Network/Qt6::Core, so this adds
 * zero new external dependencies and zero new toolchain surface for CI to
 * get wrong. The wire protocol we need (docs/04) is a single unmasked
 * server -> client stream, masked client -> server text/binary frames, and
 * server-initiated pings -- small enough to implement and keep correct by
 * hand.
 *
 * Runs entirely on its own QThread (created in the constructor, all Qt
 * network objects created lazily on that thread) so a stalled/slow server
 * never touches OBS's audio or graphics threads. External callers only
 * ever post cross-thread signals/queued invokes into this object.
 */
class TeaAsrClient : public QObject {
	Q_OBJECT

public:
	TeaAsrClient(tea_audio_tap_t *tap, tea_caption_state_t *captions);
	~TeaAsrClient() override;

	void setServer(const QString &host, int port);
	void setTokenPath(const QString &path);

	void start();
	void stop();

	bool isConnected() const;
	QString statusText() const;

	/* Samples/frames known to have been lost: the audio-tap ring buffer's
	 * overflow/contention drops (audio thread couldn't keep up) plus this
	 * client's own pendingPcm_ queue overflow (network thread couldn't
	 * keep up with flow control / a slow server). Never fabricated -- see
	 * tea_asr_client_dropped_audio_frames(). */
	uint64_t droppedFrames() const;

	/* For the Tools-menu settings dialog's capabilities display
	 * (docs/08 section 5). Safe to read from any thread. */
	bool capabilitiesKnown() const;
	bool supportsPartialTranscripts() const;

public slots:
	void doStart();
	void doStop();

private slots:
	void onSocketConnected();
	void onSocketReadyRead();
	void onSocketDisconnected();
	void onSocketError(QAbstractSocket::SocketError error);
	void onPumpTimer();
	void onReconnectTimer();
	void onCapabilitiesReply();

private:
	/* --- setup / lifecycle --- */
	void resetProtocolStateLocked();
	void scheduleReconnect();
	void setStatus(const QString &text);

	/* --- HTTP capabilities probe (for partial_transcripts) --- */
	void fetchCapabilities();

	/* --- WebSocket handshake --- */
	void sendHandshakeRequest();
	bool tryConsumeHandshakeResponse();

	/* --- WS framing --- */
	void sendWsFrame(uint8_t opcode, const char *payload, qint64 len);
	void sendTextFrame(const QByteArray &utf8Json);
	void sendBinaryFrame(const QByteArray &bytes);
	void sendCloseFrame(uint16_t code);
	void processIncomingWsBytes();
	void handleWsFrame(uint8_t opcode, const QByteArray &payload);

	/* --- protocol (docs/04) --- */
	void handleJsonMessage(const QJsonObject &obj);
	void maybeSendSessionStart();
	void sendSessionStart();
	void pumpAudio();
	QString readToken() const;

	tea_audio_tap_t *tap_;
	tea_caption_state_t *captions_;

	QThread thread_;

	QString host_ = QStringLiteral("127.0.0.1");
	int port_ = 8327;
	QString tokenPath_;

	QTcpSocket *socket_ = nullptr;
	QTimer *pumpTimer_ = nullptr;
	QTimer *reconnectTimer_ = nullptr;
	QNetworkAccessManager *nam_ = nullptr;

	QByteArray recvBuffer_;
	bool handshakeDone_ = false;
	QByteArray wsAcceptExpected_;

	/* RFC6455 fragmentation reassembly (server -> client). */
	bool fragmentActive_ = false;
	uint8_t fragOpcode_ = 0;
	QByteArray fragPayload_;

	bool capabilitiesRequested_ = false;
	/* Both read from capabilitiesKnown()/supportsPartialTranscripts() on
	 * whatever thread the settings dialog lives on, written only from
	 * this client's own worker thread in onCapabilitiesReply(). */
	std::atomic<bool> capabilitiesKnown_{false};
	std::atomic<bool> serverSupportsPartial_{false};

	bool helloReceived_ = false;
	bool sessionStartSent_ = false;
	bool sessionStarted_ = false;
	QString sessionId_;

	uint64_t nextSeq_ = 0;
	uint64_t nextSample_ = 0;
	uint64_t sendUntilSample_ = 0;

	static const size_t kMaxFramePcmBytes = 6400; /* docs/04 hard limit */
	std::deque<QByteArray> pendingPcm_;
	static const size_t kMaxPendingPcmChunks = 512; /* ~a few seconds of audio */
	/* Count of individual PCM samples (not chunks) evicted from
	 * pendingPcm_ because the network thread couldn't send fast enough.
	 * std::atomic because droppedFrames() is read from whatever thread
	 * calls tea_asr_client_dropped_audio_frames() (e.g. the OBS UI
	 * thread), while pumpAudio() writes it on this client's own thread. */
	std::atomic<uint64_t> droppedPcmSamples_{0};

	/* Upper bound on a single incoming WS frame's declared payload length
	 * (RFC 6455 allows up to 2^63-1). This server only ever sends small
	 * JSON text events, so anything beyond a generous margin is treated as
	 * a protocol violation rather than buffered indefinitely -- otherwise
	 * a misbehaving/compromised server could claim a huge length and make
	 * recvBuffer_ grow without bound while we wait for bytes that may
	 * never come. */
	static const uint64_t kMaxIncomingFramePayloadBytes = 16u * 1024u * 1024u;

	int reconnectAttempt_ = 0;

	/* Set once the server (or the WS layer) has told us this connection is
	 * not worth retrying -- currently: an `error` event with
	 * retryable=false, an `error` event with code "session_limit" (server
	 * enforces docs/04's max_continuous_sessions=1), or a close frame with
	 * code 1013 (over capacity). scheduleReconnect() honors this instead
	 * of retrying forever against a server that will keep refusing us.
	 * Cleared on the next explicit start() so the user can always force a
	 * fresh attempt (e.g. after closing the other caption source). */
	bool fatalNonRetryable_ = false;
	QString fatalReason_;

	mutable QMutex statusMutex_;
	QString statusText_;
	std::atomic<bool> connected_{false};

	std::atomic<bool> wantRunning_{false};
};
