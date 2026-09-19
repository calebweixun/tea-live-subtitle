#include "settings-dialog.hpp"
#include "settings-dialog.h"

#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QVBoxLayout>
#include <QFormLayout>
#include <QLineEdit>
#include <QSpinBox>
#include <QLabel>
#include <QMainWindow>
#include <QString>

TeaSettingsDialog::TeaSettingsDialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle(QString::fromUtf8(obs_module_text("TeaLiveSubtitle.Dialog.Title")));
	setMinimumSize(360, 160);

	QVBoxLayout *mainLayout = new QVBoxLayout(this);
	QFormLayout *formLayout = new QFormLayout();

	serverAddressEdit = new QLineEdit(this);
	serverAddressEdit->setText(QStringLiteral("127.0.0.1"));
	formLayout->addRow(QString::fromUtf8(obs_module_text("TeaLiveSubtitle.Dialog.ServerAddress")),
			    serverAddressEdit);

	portSpinBox = new QSpinBox(this);
	portSpinBox->setRange(1, 65535);
	portSpinBox->setValue(8327);
	formLayout->addRow(QString::fromUtf8(obs_module_text("TeaLiveSubtitle.Dialog.Port")), portSpinBox);

	mainLayout->addLayout(formLayout);

	/* Phase 1 does not connect to anything yet, so the status label is
	 * always the "not connected" placeholder. The WebSocket client that
	 * updates this live is phase 2. */
	statusLabel = new QLabel(QString::fromUtf8(obs_module_text("TeaLiveSubtitle.Dialog.StatusNotConnected")),
				  this);
	mainLayout->addWidget(statusLabel);
}

void tea_show_settings_dialog(void)
{
	QMainWindow *mainWindow = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	TeaSettingsDialog dialog(mainWindow);
	dialog.exec();
}
