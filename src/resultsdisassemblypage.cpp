/*
    SPDX-FileCopyrightText: Darya Knysh <d.knysh@nips.ru>
    SPDX-FileCopyrightText: Milian Wolff <milian.wolff@kdab.com>
    SPDX-FileCopyrightText: 2016-2022 Klarälvdalens Datakonsult AB, a KDAB Group company, info@kdab.com

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "resultsdisassemblypage.h"
#include "ui_resultsdisassemblypage.h"

#include <QActionGroup>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QListWidgetItem>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QString>
#include <QTemporaryFile>
#include <QTextStream>

#include <KColorScheme>
#include <KStandardAction>

#include "parsers/perf/perfparser.h"
#include "resultsutil.h"

#if KFSyntaxHighlighting_FOUND
#include "highlighter.hpp"
#include <KSyntaxHighlighting/definition.h>
#include <KSyntaxHighlighting/repository.h>
#include <QCompleter>
#include <QStringListModel>
#endif

#include "costheaderview.h"
#include "data.h"
#include "models/codedelegate.h"
#include "models/costdelegate.h"
#include "models/disassemblymodel.h"
#include "models/hashmodel.h"
#include "models/search.h"
#include "models/sourcecodemodel.h"
#include "models/topproxy.h"
#include "models/treemodel.h"
#include "settings.h"

namespace {
template<typename ModelType, typename ResultFound, typename EndReached>
void connectModel(ModelType* model, ResultFound resultFound, EndReached endReached)
{
    QObject::connect(model, &ModelType::resultFound, model, resultFound);
    QObject::connect(model, &ModelType::searchEndReached, model, endReached);
}
}

ResultsDisassemblyPage::ResultsDisassemblyPage(CostContextMenu* costContextMenu, QWidget* parent)
    : QWidget(parent)
    , ui(std::make_unique<Ui::ResultsDisassemblyPage>())
#if KFSyntaxHighlighting_FOUND
    , m_repository(std::make_unique<KSyntaxHighlighting::Repository>())
    , m_disassemblyModel(new DisassemblyModel(m_repository.get(), this))
    , m_sourceCodeModel(new SourceCodeModel(m_repository.get(), this))
#else
    , m_disassemblyModel(new DisassemblyModel(nullptr, this))
    , m_sourceCodeModel(new SourceCodeModel(nullptr, this))
#endif
    , m_disassemblyCostDelegate(new CostDelegate(DisassemblyModel::CostRole, DisassemblyModel::TotalCostRole, this))
    , m_sourceCodeCostDelegate(new CostDelegate(SourceCodeModel::CostRole, SourceCodeModel::TotalCostRole, this))
    , m_disassemblyDelegate(new CodeDelegate(DisassemblyModel::RainbowLineNumberRole, DisassemblyModel::HighlightRole,
                                             DisassemblyModel::SyntaxHighlightRole, this))
    , m_sourceCodeDelegate(new CodeDelegate(SourceCodeModel::RainbowLineNumberRole, SourceCodeModel::HighlightRole,
                                            SourceCodeModel::SyntaxHighlightRole, this))
{
    ui->setupUi(this);
    ui->assemblyView->setModel(m_disassemblyModel);
    ui->assemblyView->setMouseTracking(true);
    ui->assemblyView->setHeader(new CostHeaderView(costContextMenu, this));
    ui->sourceCodeView->setModel(m_sourceCodeModel);
    ui->sourceCodeView->setMouseTracking(true);
    ui->sourceCodeView->setHeader(new CostHeaderView(costContextMenu, this));

    auto settings = Settings::instance();
    m_sourceCodeModel->setSysroot(settings->sysroot());

    connect(settings, &Settings::sysrootChanged, m_sourceCodeModel, &SourceCodeModel::setSysroot);

    auto updateFromDisassembly = [this](const QModelIndex& index) {
        const auto fileLine = m_disassemblyModel->fileLineForIndex(index);
        m_disassemblyModel->updateHighlighting(fileLine.line);
        m_sourceCodeModel->updateHighlighting(fileLine.line);
    };

    auto updateFromSource = [this](const QModelIndex& index) {
        const auto fileLine = m_sourceCodeModel->fileLineForIndex(index);
        m_disassemblyModel->updateHighlighting(fileLine.line);
        m_sourceCodeModel->updateHighlighting(fileLine.line);
    };

    connect(settings, &Settings::sourceCodePathsChanged, this, [this](const QString&) { showDisassembly(); });

    connect(ui->assemblyView, &QTreeView::entered, this, updateFromDisassembly);

    connect(ui->sourceCodeView, &QTreeView::entered, this, updateFromSource);

    ui->sourceCodeView->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(ui->sourceCodeView, &QTreeView::customContextMenuRequested, this, [this](QPoint point) {
        const auto index = ui->sourceCodeView->indexAt(point);
        const auto fileLine = index.data(SourceCodeModel::FileLineRole).value<Data::FileLine>();
        if (!fileLine.isValid())
            return;

        QMenu contextMenu;
        auto* openEditorAction = contextMenu.addAction(QCoreApplication::translate("Util", "Open in Editor"));
        QObject::connect(openEditorAction, &QAction::triggered, &contextMenu,
                         [this, fileLine]() { emit navigateToCode(fileLine.file, fileLine.line, -1); });
        contextMenu.exec(QCursor::pos());
    });

    auto addScrollTo = [](QTreeView* sourceView, QTreeView* destView, auto sourceModel, auto destModel) {
        connect(sourceView, &QTreeView::clicked, sourceView, [=](const QModelIndex& index) {
            const auto fileLine = sourceModel->fileLineForIndex(index);
            if (fileLine.isValid()) {
                destView->scrollTo(destModel->indexForFileLine(fileLine));
            }
        });
    };

    addScrollTo(ui->sourceCodeView, ui->assemblyView, m_sourceCodeModel, m_disassemblyModel);
    addScrollTo(ui->assemblyView, ui->sourceCodeView, m_disassemblyModel, m_sourceCodeModel);

    connect(ui->assemblyView, &QTreeView::doubleClicked, this, [this](const QModelIndex& index) {
        const QString functionName = index.data(DisassemblyModel::LinkedFunctionNameRole).toString();
        if (functionName.isEmpty())
            return;

        const auto functionOffset = index.data(DisassemblyModel::LinkedFunctionOffsetRole).toInt();

        if (m_symbolStack[m_stackIndex].symbol == functionName) {
            ui->assemblyView->scrollTo(m_disassemblyModel->findIndexWithOffset(functionOffset),
                                       QAbstractItemView::ScrollHint::PositionAtTop);
        } else {
            const auto symbol =
                std::find_if(m_callerCalleeResults.entries.keyBegin(), m_callerCalleeResults.entries.keyEnd(),
                             [functionName](const Data::Symbol& symbol) { return symbol.symbol == functionName; });

            if (symbol != m_callerCalleeResults.entries.keyEnd()) {
                m_symbolStack.push_back(*symbol);
                m_stackIndex++;
                emit stackChanged();
            } else {
                ui->symbolNotFound->setText(tr("unkown symbol %1").arg(functionName));
                ui->symbolNotFound->show();
            }
        }
    });

    connect(ui->stackBackButton, &QPushButton::pressed, this, [this] {
        m_stackIndex--;
        if (m_stackIndex < 0)
            m_stackIndex = m_symbolStack.size() - 1;

        emit stackChanged();
    });

    connect(ui->stackNextButton, &QPushButton::pressed, this, [this] {
        m_stackIndex++;
        if (m_stackIndex >= m_symbolStack.size())
            m_stackIndex = 0;

        emit stackChanged();
    });

    connect(this, &ResultsDisassemblyPage::stackChanged, this, [this] {
        ui->stackBackButton->setEnabled(m_stackIndex > 0);
        ui->stackNextButton->setEnabled(m_stackIndex < m_symbolStack.size() - 1);

        ui->stackEntry->setText(m_symbolStack[m_stackIndex].prettySymbol);

        showDisassembly();
    });

    ui->searchEndWidget->hide();
    ui->disasmEndReachedWidget->hide();

    auto setupSearchShortcuts = [this](QPushButton* search, QPushButton* next, QPushButton* prev, QPushButton* close,
                                       QWidget* searchWidget, QLineEdit* edit, QAbstractItemView* view,
                                       KMessageWidget* endReached, auto* model, int additionalRows) {
        searchWidget->hide();

        auto actions = new QActionGroup(view);

        auto findAction = KStandardAction::find(
            this,
            [searchWidget, edit] {
                searchWidget->show();
                edit->setFocus();
            },
            actions);
        findAction->setShortcutContext(Qt::ShortcutContext::WidgetWithChildrenShortcut);
        view->addAction(findAction);

        auto searchNext = [this, model, edit, additionalRows] {
            const auto offset = m_currentSearchIndex.isValid() ? m_currentSearchIndex.row() - additionalRows + 1 : 0;
            model->find(edit->text(), Direction::Forward, offset);
        };

        auto searchPrev = [this, model, edit, additionalRows] {
            const auto offset = m_currentSearchIndex.isValid() ? m_currentSearchIndex.row() - additionalRows - 1 : 0;

            model->find(edit->text(), Direction::Backward, offset);
        };

        auto findNextAction = KStandardAction::findNext(this, searchNext, actions);
        findNextAction->setShortcutContext(Qt::ShortcutContext::WidgetWithChildrenShortcut);
        searchWidget->addAction(findNextAction);
        auto findPrevAction = KStandardAction::findPrev(this, searchPrev, actions);
        findPrevAction->setShortcutContext(Qt::ShortcutContext::WidgetWithChildrenShortcut);
        searchWidget->addAction(findPrevAction);

        connect(edit, &QLineEdit::returnPressed, findNextAction, &QAction::trigger);
        connect(next, &QPushButton::clicked, findNextAction, &QAction::trigger);
        connect(prev, &QPushButton::clicked, findPrevAction, &QAction::trigger);

        connect(search, &QPushButton::clicked, findAction, &QAction::trigger);
        connect(close, &QPushButton::clicked, this, [searchWidget] { searchWidget->hide(); });

        const auto colorScheme = KColorScheme();

        connectModel(
            model,
            [this, edit, view, colorScheme](const QModelIndex& index) {
                auto palette = edit->palette();
                m_currentSearchIndex = index;
                palette.setBrush(QPalette::Text,
                                 index.isValid() ? colorScheme.foreground()
                                                 : colorScheme.foreground(KColorScheme::NegativeText));
                edit->setPalette(palette);
                view->setCurrentIndex(index);

                if (!index.isValid())
                    view->clearSelection();
            },
            [endReached] { endReached->show(); });
    };

    setupSearchShortcuts(ui->searchButton, ui->nextResult, ui->prevResult, ui->closeButton, ui->searchWidget,
                         ui->searchEdit, ui->sourceCodeView, ui->searchEndWidget, m_sourceCodeModel, 1);
    setupSearchShortcuts(ui->disasmSearchButton, ui->disasmNextButton, ui->disasmPrevButton, ui->disasmCloseButton,
                         ui->disasmSearchWidget, ui->disasmSearchEdit, ui->assemblyView, ui->disasmEndReachedWidget,
                         m_disassemblyModel, 0);

#if KFSyntaxHighlighting_FOUND
    QStringList schemes;

    const auto definitions = m_repository->definitions();
    schemes.reserve(definitions.size());

    std::transform(definitions.begin(), definitions.end(), std::back_inserter(schemes),
                   [](const auto& definition) { return definition.name(); });

    auto definitionModel = new QStringListModel(this);
    definitionModel->setStringList(schemes);

    auto connectCompletion = [definitionModel, schemes, this](QComboBox* box, auto* model) {
        auto completer = new QCompleter(this);
        completer->setModel(definitionModel);
        completer->setCaseSensitivity(Qt::CaseInsensitive);
        completer->setCompletionMode(QCompleter::PopupCompletion);
        box->setCompleter(completer);
        box->setModel(definitionModel);
        box->setCurrentText(model->highlighter()->definition());

        connect(box, qOverload<int>(&QComboBox::activated), this, [this, model, box]() {
            model->highlighter()->setDefinition(m_repository->definitionForName(box->currentText()));
        });

        connect(model->highlighter(), &Highlighter::definitionChanged,
                [box](const QString& definition) { box->setCurrentText(definition); });
    };

    connectCompletion(ui->sourceCodeComboBox, m_sourceCodeModel);
    connectCompletion(ui->assemblyComboBox, m_disassemblyModel);
#else
    ui->customSourceCodeHighlighting->setVisible(false);
    ui->customAssemblyHighlighting->setVisible(false);
#endif
}

ResultsDisassemblyPage::~ResultsDisassemblyPage() = default;

void ResultsDisassemblyPage::clear()
{
    m_disassemblyModel->clear();
    m_sourceCodeModel->clear();
}

void ResultsDisassemblyPage::setupAsmViewModel()
{
    ui->sourceCodeView->header()->setStretchLastSection(false);
    ui->sourceCodeView->header()->setSectionResizeMode(SourceCodeModel::SourceCodeLineNumber,
                                                       QHeaderView::ResizeToContents);
    ui->sourceCodeView->header()->setSectionResizeMode(SourceCodeModel::SourceCodeColumn, QHeaderView::Stretch);
    ui->sourceCodeView->setItemDelegateForColumn(SourceCodeModel::SourceCodeColumn, m_sourceCodeDelegate);

    ui->assemblyView->header()->setStretchLastSection(false);
    ui->assemblyView->header()->setSectionResizeMode(DisassemblyModel::AddrColumn, QHeaderView::ResizeToContents);
    ui->assemblyView->header()->setSectionResizeMode(DisassemblyModel::DisassemblyColumn, QHeaderView::Stretch);
    ui->assemblyView->setItemDelegateForColumn(DisassemblyModel::DisassemblyColumn, m_disassemblyDelegate);

    for (int col = DisassemblyModel::COLUMN_COUNT; col < m_disassemblyModel->columnCount(); col++) {
        ui->assemblyView->setColumnWidth(col, 100);
        ui->assemblyView->header()->setSectionResizeMode(col, QHeaderView::Interactive);
        ui->assemblyView->setItemDelegateForColumn(col, m_disassemblyCostDelegate);
    }

    for (int col = SourceCodeModel::COLUMN_COUNT; col < m_sourceCodeModel->columnCount(); col++) {
        ui->sourceCodeView->setColumnWidth(col, 100);
        ui->sourceCodeView->header()->setSectionResizeMode(col, QHeaderView::Interactive);
        ui->sourceCodeView->setItemDelegateForColumn(col, m_sourceCodeCostDelegate);
    }
}

void ResultsDisassemblyPage::showDisassembly()
{
    if (m_symbolStack.isEmpty())
        return;

    const auto& curSymbol = m_symbolStack[m_stackIndex];

    // Show empty tab when selected symbol is not valid
    if (curSymbol.symbol.isEmpty()) {
        clear();
    }

    // TODO: add the ability to configure the arch <-> objdump mapping somehow in the settings
    const auto objdump = [this]() {
        if (!m_objdump.isEmpty())
            return m_objdump;

        if (m_arch.startsWith(QLatin1String("armv8")) || m_arch.startsWith(QLatin1String("aarch64"))) {
            return QStringLiteral("aarch64-linux-gnu-objdump");
        }
        const auto isArm = m_arch.startsWith(QLatin1String("arm"));
        return isArm ? QStringLiteral("arm-linux-gnueabi-objdump") : QStringLiteral("objdump");
    };

    ui->symbolNotFound->hide();

    auto settings = Settings::instance();
    const auto colon = QLatin1Char(':');

    showDisassembly(DisassemblyOutput::disassemble(
        objdump(), m_arch, settings->debugPaths().split(colon), settings->extraLibPaths().split(colon),
        settings->sourceCodePaths().split(colon), settings->sysroot(), curSymbol));
}

void ResultsDisassemblyPage::showDisassembly(const DisassemblyOutput& disassemblyOutput)
{
    m_disassemblyModel->clear();
    m_sourceCodeModel->clear();

    // this function is only called if m_symbolStack is non empty (see above)
    Q_ASSERT(!m_symbolStack.isEmpty());
    const auto& curSymbol = m_symbolStack[m_stackIndex];

#if KFSyntaxHighlighting_FOUND
    m_sourceCodeModel->highlighter()->setDefinition(
        m_repository->definitionForFileName(disassemblyOutput.mainSourceFileName));
    m_disassemblyModel->highlighter()->setDefinition(m_repository->definitionForName(QStringLiteral("GNU Assembler")));
#endif

    const auto& entry = m_callerCalleeResults.entry(curSymbol);

    ui->filenameLabel->setText(disassemblyOutput.mainSourceFileName);
    // don't set tooltip on symbolLabel, as that will be called internally and then get overwritten
    setToolTip(Util::formatTooltip(entry.id, curSymbol, m_callerCalleeResults.selfCosts,
                                   m_callerCalleeResults.inclusiveCosts));

    if (!disassemblyOutput) {
        ui->errorMessage->setText(disassemblyOutput.errorMessage);
        ui->errorMessage->show();
        return;
    }

    ui->errorMessage->hide();

    m_disassemblyModel->setDisassembly(disassemblyOutput, m_callerCalleeResults);
    m_sourceCodeModel->setDisassembly(disassemblyOutput, m_callerCalleeResults);

    ResultsUtil::hideEmptyColumns(m_callerCalleeResults.selfCosts, ui->assemblyView, DisassemblyModel::COLUMN_COUNT);

    ResultsUtil::hideEmptyColumns(m_callerCalleeResults.selfCosts, ui->sourceCodeView, SourceCodeModel::COLUMN_COUNT);

    ResultsUtil::hideEmptyColumns(m_callerCalleeResults.inclusiveCosts, ui->sourceCodeView,
                                  SourceCodeModel::COLUMN_COUNT + m_callerCalleeResults.selfCosts.numTypes());

    // hide self cost for tracepoints in assembly view, this is basically always zero
    ResultsUtil::hideTracepointColumns(m_callerCalleeResults.selfCosts, ui->assemblyView,
                                       DisassemblyModel::COLUMN_COUNT);

    // hide self cost for tracepoints - only show inclusive times instead here
    ResultsUtil::hideTracepointColumns(m_callerCalleeResults.selfCosts, ui->sourceCodeView,
                                       SourceCodeModel::COLUMN_COUNT);

    setupAsmViewModel();
}

void ResultsDisassemblyPage::setSymbol(const Data::Symbol& symbol)
{
    m_stackIndex = 0;
    m_symbolStack.clear();
    m_symbolStack.push_back(symbol);
    emit stackChanged();
}

void ResultsDisassemblyPage::setCostsMap(const Data::CallerCalleeResults& callerCalleeResults)
{
    m_callerCalleeResults = callerCalleeResults;
}

void ResultsDisassemblyPage::setObjdump(const QString& objdump)
{
    m_objdump = objdump;
}

void ResultsDisassemblyPage::setArch(const QString& arch)
{
    m_arch = arch.trimmed().toLower();
}
