#pragma once

#include <QDialog>

class QLineEdit;
class QSpinBox;
class QLabel;

/* Phase 1: proves Qt + obs-frontend-api link and load correctly. Only holds
 * the two fields the eventual global connection settings need (server
 * address/port) plus a status label. No networking happens yet -- that is
 * phase 2's asr-client.cpp/hpp. */
class TeaSettingsDialog : public QDialog {
	Q_OBJECT

public:
	explicit TeaSettingsDialog(QWidget *parent = nullptr);
	~TeaSettingsDialog() override = default;

private:
	QLineEdit *serverAddressEdit = nullptr;
	QSpinBox *portSpinBox = nullptr;
	QLabel *statusLabel = nullptr;
};
