#pragma once

#include <QDialog>

class QLabel;
class QTableWidget;
class QTimer;

/*
 * Tools-menu diagnostics dialog.
 *
 * docs/08-obs-plugin.md section 5 sketches this as holding *global*
 * connection settings (server URL, token path, audio source, ...) because
 * that spec assumes a single global connection shared by every caption
 * source. This plugin deliberately does not implement that (see the
 * architecture comment at the top of captions-source.c): each
 * tea_live_subtitle_source instance owns its own connection, and its
 * server/token/audio-source settings live in that source's own Properties
 * panel instead.
 *
 * What this dialog *does* still owe the user, and does implement, is the
 * rest of section 5's list applied per-instance: connection status, dropped-
 * frame stats, a reconnect action, and a capabilities/protocol readout --
 * for every live caption source at once, since there's no other place in
 * the UI that shows more than one source simultaneously.
 */
class TeaSettingsDialog : public QDialog {
	Q_OBJECT

public:
	explicit TeaSettingsDialog(QWidget *parent = nullptr);
	~TeaSettingsDialog() override;

private slots:
	void refresh();
	void reconnectAllClicked();

private:
	QLabel *noteLabel = nullptr;
	QTableWidget *sourcesTable = nullptr;
	QLabel *emptyLabel = nullptr;
	QTimer *refreshTimer = nullptr;
};
