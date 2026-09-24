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

#include "settings-dialog.hpp"
#include "settings-dialog.h"

#include <obs-module.h>
#include <obs-frontend-api.h>

#include "captions-source.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QHeaderView>
#include <QAbstractItemView>
#include <QPushButton>
#include <QLabel>
#include <QMainWindow>
#include <QString>
#include <QStringList>
#include <QTimer>

namespace {

/* Bridges tea_captions_source_for_each()'s plain-C callback into the
 * QTableWidget being populated. */
struct RowFillContext {
	QTableWidget *table;
	int row;
};

void tea_fill_row_cb(const tea_captions_source_info_t *info, void *user)
{
	auto *ctx = static_cast<RowFillContext *>(user);
	QTableWidget *table = ctx->table;
	int row = ctx->row++;

	if (table->rowCount() <= row)
		table->setRowCount(row + 1);

	table->setItem(row, 0, new QTableWidgetItem(QString::fromUtf8(info->source_name ? info->source_name : "?")));
	table->setItem(row, 1, new QTableWidgetItem(QString::fromUtf8(info->status_text ? info->status_text : "")));
	table->setItem(row, 2, new QTableWidgetItem(QString::number(info->dropped_frames)));
	table->setItem(row, 3,
		       new QTableWidgetItem(QString::fromUtf8(obs_module_text(
			       info->connected ? "TeaLiveSubtitle.Dialog.Yes" : "TeaLiveSubtitle.Dialog.No"))));

	QString capabilities;
	if (!info->capabilities_known) {
		capabilities = QString::fromUtf8(obs_module_text("TeaLiveSubtitle.Dialog.Checking"));
	} else {
		capabilities = QString::fromUtf8(obs_module_text(info->supports_partial_transcripts
									 ? "TeaLiveSubtitle.Dialog.PartialYes"
									 : "TeaLiveSubtitle.Dialog.PartialNo"));
		/* Stable-caption diagnostics (errors/violations stay here, never on
		 * the caption canvas). */
		if (!info->stable_captions_enabled)
			capabilities += QStringLiteral("; stable captions: off (setting)");
		else if (info->stable_captions_active)
			capabilities += QStringLiteral("; stable captions: on");
		else if (!info->supports_stable_transcripts)
			capabilities += QStringLiteral("; stable captions: not offered by server (using partial)");
		else
			capabilities += QStringLiteral("; stable captions: not active (using partial)");
		if (info->stable_mismatches > 0)
			capabilities += QStringLiteral("; non-append stable updates ignored: %1")
						.arg((qulonglong)info->stable_mismatches);
	}
	table->setItem(row, 4, new QTableWidgetItem(capabilities));
}

} // namespace

TeaSettingsDialog::TeaSettingsDialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle(QString::fromUtf8(obs_module_text("TeaLiveSubtitle.Dialog.Title")));
	setMinimumSize(640, 320);

	QVBoxLayout *mainLayout = new QVBoxLayout(this);

	noteLabel = new QLabel(QString::fromUtf8(obs_module_text("TeaLiveSubtitle.Dialog.Note")), this);
	noteLabel->setWordWrap(true);
	mainLayout->addWidget(noteLabel);

	sourcesTable = new QTableWidget(0, 5, this);
	QStringList headers;
	headers << QString::fromUtf8(obs_module_text("TeaLiveSubtitle.Dialog.Col.Source"))
		<< QString::fromUtf8(obs_module_text("TeaLiveSubtitle.Dialog.Col.Status"))
		<< QString::fromUtf8(obs_module_text("TeaLiveSubtitle.Dialog.Col.Dropped"))
		<< QString::fromUtf8(obs_module_text("TeaLiveSubtitle.Dialog.Col.Connected"))
		<< QString::fromUtf8(obs_module_text("TeaLiveSubtitle.Dialog.Col.Capabilities"));
	sourcesTable->setHorizontalHeaderLabels(headers);
	sourcesTable->horizontalHeader()->setStretchLastSection(true);
	sourcesTable->verticalHeader()->setVisible(false);
	sourcesTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
	sourcesTable->setSelectionMode(QAbstractItemView::NoSelection);
	mainLayout->addWidget(sourcesTable, 1);

	emptyLabel = new QLabel(QString::fromUtf8(obs_module_text("TeaLiveSubtitle.Dialog.Empty")), this);
	emptyLabel->setVisible(false);
	mainLayout->addWidget(emptyLabel);

	QHBoxLayout *buttonRow = new QHBoxLayout();
	buttonRow->addStretch(1);
	QPushButton *reconnectButton =
		new QPushButton(QString::fromUtf8(obs_module_text("TeaLiveSubtitle.Dialog.ReconnectAll")), this);
	connect(reconnectButton, &QPushButton::clicked, this, &TeaSettingsDialog::reconnectAllClicked);
	buttonRow->addWidget(reconnectButton);
	mainLayout->addLayout(buttonRow);

	refreshTimer = new QTimer(this);
	refreshTimer->setInterval(1000);
	connect(refreshTimer, &QTimer::timeout, this, &TeaSettingsDialog::refresh);
	refreshTimer->start();

	refresh();
}

TeaSettingsDialog::~TeaSettingsDialog()
{
	if (refreshTimer)
		refreshTimer->stop();
}

void TeaSettingsDialog::refresh()
{
	RowFillContext ctx{sourcesTable, 0};
	tea_captions_source_for_each(tea_fill_row_cb, &ctx);
	sourcesTable->setRowCount(ctx.row);

	bool empty = ctx.row == 0;
	sourcesTable->setVisible(!empty);
	emptyLabel->setVisible(empty);
}

void TeaSettingsDialog::reconnectAllClicked()
{
	tea_captions_source_reconnect_all();
	refresh();
}

void tea_show_settings_dialog(void)
{
	QMainWindow *mainWindow = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	TeaSettingsDialog dialog(mainWindow);
	dialog.exec();
}
