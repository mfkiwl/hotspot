/*
    SPDX-FileCopyrightText: Nate Rogers <nate.rogers@kdab.com>
    SPDX-FileCopyrightText: Milian Wolff <milian.wolff@kdab.com>
    SPDX-FileCopyrightText: 2016-2022 Klarälvdalens Datakonsult AB, a KDAB Group company, info@kdab.com

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "startpage.h"
#include "ui_startpage.h"

#include <QDebug>
#include <QMainWindow>
#include <QPainter>

#include <KFormat>

StartPage::StartPage(QWidget* parent)
    : QWidget(parent)
    , ui(std::make_unique<Ui::StartPage>())
{
    ui->setupUi(this);

    connect(ui->openFileButton, &QAbstractButton::clicked, this, &StartPage::openFileButtonClicked);
    connect(ui->recordDataButton, &QAbstractButton::clicked, this, &StartPage::recordButtonClicked);
    connect(ui->stopParseButton, &QAbstractButton::clicked, this, &StartPage::stopParseButtonClicked);
    connect(ui->pathSettings, &QAbstractButton::clicked, this, &StartPage::pathSettingsButtonClicked);
    ui->openFileButton->setFocus();

    updateBackground();
}

StartPage::~StartPage() = default;

void StartPage::showStartPage()
{
    ui->loadingResultsErrorLabel->hide();
    ui->loadStack->setCurrentWidget(ui->openFilePage);
}

void StartPage::showParseFileProgress()
{
    ui->loadingResultsErrorLabel->hide();
    ui->loadStack->setCurrentWidget(ui->parseProgressPage);

    // Reset maximum to show throbber, we may not get progress notifications
    ui->openFileProgressBar->setMaximum(0);
}

void StartPage::onOpenFileError(const QString& errorMessage)
{
    qWarning() << errorMessage;
    ui->loadingResultsErrorLabel->setText(errorMessage);
    ui->loadingResultsErrorLabel->show();
    ui->loadStack->setCurrentWidget(ui->openFilePage);
}

void StartPage::onParseFileProgress(float percent)
{
    const int scale = 1000;
    if (ui->openFileProgressBar->maximum() == 0) {
        ui->openFileProgressBar->setMaximum(scale);
    }
    ui->openFileProgressBar->setValue(static_cast<int>(percent * scale));
}

void StartPage::onDebugInfoDownloadProgress(const QString& module, const QString& url, qint64 numerator,
                                            qint64 denominator)
{
    if (numerator == denominator) {
        ui->loadStack->setCurrentWidget(ui->parseProgressPage);
        return;
    }

    const auto format = KFormat();
    ui->loadStack->setCurrentWidget(ui->downloadDebugInfoProgressPage);

    ui->downloadDebugInfoProgressLabel->setText(
        tr("Downloading Debug Information for %1 (%2 of %3)")
            .arg(module, format.formatByteSize(numerator, 1, KFormat::MetricBinaryDialect),
                 format.formatByteSize(denominator, 1, KFormat::MetricBinaryDialect)));
    ui->downloadDebugInfoProgressLabel->setToolTip(url);

    if (denominator == 0 || denominator > std::numeric_limits<int>::max()) {
        ui->downloadDebugInfoProgressBar->setRange(0, 0);
        ui->downloadDebugInfoProgressBar->setValue(-1);
    } else {
        ui->downloadDebugInfoProgressBar->setRange(0, static_cast<int>(denominator));
        ui->downloadDebugInfoProgressBar->setValue(static_cast<int>(numerator));
    }
}

void StartPage::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter(this);
    const auto windowRect = rect();
    auto backgroundRect = QRectF(QPointF(0, 0), QSizeF(m_background.size()) / devicePixelRatioF());
    backgroundRect.moveBottomRight(windowRect.bottomRight());
    painter.drawPixmap(backgroundRect.toRect(), m_background);
}

void StartPage::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);

    if (event->type() == QEvent::PaletteChange) {
        updateBackground();
    }
}

void StartPage::updateBackground()
{
    const auto background = palette().window().color();
    const auto foreground = palette().windowText().color();

    if (qGray(background.rgb()) < qGray(foreground.rgb())) {
        // Dark color scheme
        m_background = QPixmap(QStringLiteral(":/images/background_dark.png"));
    } else {
        // Bright color scheme
        m_background = QPixmap(QStringLiteral(":/images/background_bright.png"));
    }
    m_background.setDevicePixelRatio(devicePixelRatioF());
}
