/****************************************************************************
**
** Copyright (C) 2014 Digia Plc
** All rights reserved.
** For any questions to Digia, please use contact form at http://qt.digia.com
**
** This file is part of the Qt Creator Enterprise Auto Test Add-on.
**
** Licensees holding valid Qt Enterprise licenses may use this file in
** accordance with the Qt Enterprise License Agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia.
**
** If you have questions regarding the use of this file, please use
** contact form at http://qt.digia.com
**
****************************************************************************/

#include "testresultspane.h"
#include "testresultmodel.h"
#include "testresultdelegate.h"
#include "testrunner.h"
#include "testtreemodel.h"

#include <coreplugin/coreconstants.h>
#include <coreplugin/icontext.h>

#include <texteditor/texteditor.h>

#include <utils/itemviews.h>
#include <utils/theme/theme.h>

#include <QDebug>
#include <QHBoxLayout>
#include <QToolButton>
#include <QVBoxLayout>

namespace Autotest {
namespace Internal {

TestResultsPane::TestResultsPane(QObject *parent) :
    Core::IOutputPane(parent),
    m_context(new Core::IContext(this))
{
    m_outputWidget = new QWidget;
    QVBoxLayout *outputLayout = new QVBoxLayout;
    outputLayout->setMargin(0);
    outputLayout->setSpacing(0);
    m_outputWidget->setLayout(outputLayout);

    QPalette pal;
    pal.setColor(QPalette::Window,
                 Utils::creatorTheme()->color(Utils::Theme::SearchResultWidgetBackgroundColor));
    pal.setColor(QPalette::WindowText,
                 Utils::creatorTheme()->color(Utils::Theme::SearchResultWidgetTextColor));
    m_summaryWidget = new QFrame;
    m_summaryWidget->setPalette(pal);
    m_summaryWidget->setAutoFillBackground(true);
    QHBoxLayout *layout = new QHBoxLayout;
    layout->setMargin(6);
    m_summaryWidget->setLayout(layout);
    m_summaryLabel = new QLabel;
    m_summaryLabel->setPalette(pal);
    layout->addWidget(m_summaryLabel);
    m_summaryWidget->setVisible(false);

    outputLayout->addWidget(m_summaryWidget);

    m_listView = new Utils::ListView(m_outputWidget);
    m_model = new TestResultModel(this);
    m_filterModel = new TestResultFilterModel(m_model, this);
    m_filterModel->setDynamicSortFilter(true);
    m_listView->setModel(m_filterModel);
    TestResultDelegate *trd = new TestResultDelegate(this);
    m_listView->setItemDelegate(trd);

    outputLayout->addWidget(m_listView);

    createToolButtons();

    connect(m_listView, &Utils::ListView::activated, this, &TestResultsPane::onItemActivated);
    connect(m_listView->selectionModel(), &QItemSelectionModel::currentChanged,
            trd, &TestResultDelegate::currentChanged);
    connect(TestRunner::instance(), &TestRunner::testRunStarted,
            this, &TestResultsPane::onTestRunStarted);
    connect(TestRunner::instance(), &TestRunner::testRunFinished,
            this, &TestResultsPane::onTestRunFinished);
    connect(TestTreeModel::instance(), &TestTreeModel::testTreeModelChanged,
            this, &TestResultsPane::onTestTreeModelChanged);
}

void TestResultsPane::createToolButtons()
{
    m_runAll = new QToolButton(m_listView);
    m_runAll->setIcon(QIcon(QLatin1String(":/images/run.png")));
    m_runAll->setToolTip(tr("Run All Tests"));
    connect(m_runAll, &QToolButton::clicked, this, &TestResultsPane::onRunAllTriggered);

    m_runSelected = new QToolButton(m_listView);
    m_runSelected->setIcon(QIcon(QLatin1String(":/images/runselected.png")));
    m_runSelected->setToolTip(tr("Run Selected Tests"));
    connect(m_runSelected, &QToolButton::clicked, this, &TestResultsPane::onRunSelectedTriggered);

    m_stopTestRun = new QToolButton(m_listView);
    m_stopTestRun->setIcon(QIcon(QLatin1String(":/images/stop.png")));
    m_stopTestRun->setToolTip(tr("Stop Test Run"));
    m_stopTestRun->setEnabled(false);
    connect(m_stopTestRun, &QToolButton::clicked, TestRunner::instance(), &TestRunner::stopTestRun);

    m_filterButton = new QToolButton(m_listView);
    m_filterButton->setIcon(QIcon(QLatin1String(Core::Constants::ICON_FILTER)));
    m_filterButton->setToolTip(tr("Filter Test Results"));
    m_filterButton->setProperty("noArrow", true);
    m_filterButton->setAutoRaise(true);
    m_filterButton->setPopupMode(QToolButton::InstantPopup);
    m_filterMenu = new QMenu(m_filterButton);
    initializeFilterMenu();
    connect(m_filterMenu, &QMenu::aboutToShow, this, &TestResultsPane::updateFilterMenu);
    connect(m_filterMenu, &QMenu::triggered, this, &TestResultsPane::filterMenuTriggered);
    m_filterButton->setMenu(m_filterMenu);
}

static TestResultsPane *m_instance = 0;

TestResultsPane *TestResultsPane::instance()
{
    if (!m_instance)
        m_instance = new TestResultsPane;
    return m_instance;
}

TestResultsPane::~TestResultsPane()
{
    delete m_listView;
    m_instance = 0;
}

void TestResultsPane::addTestResult(const TestResult &result)
{
    m_model->addTestResult(result);
    if (!m_listView->isVisible())
        popup(Core::IOutputPane::NoModeSwitch);
    flash();
    navigateStateChanged();
}

QWidget *TestResultsPane::outputWidget(QWidget *parent)
{
    if (m_outputWidget) {
        m_outputWidget->setParent(parent);
    } else {
        qDebug() << "This should not happen...";
    }
    return m_outputWidget;
}

QList<QWidget *> TestResultsPane::toolBarWidgets() const
{
    return QList<QWidget *>() << m_runAll << m_runSelected << m_stopTestRun << m_filterButton;
}

QString TestResultsPane::displayName() const
{
    return tr("Test Results");
}

int TestResultsPane::priorityInStatusBar() const
{
    return -666;
}

void TestResultsPane::clearContents()
{
    m_filterModel->clearTestResults();
    navigateStateChanged();
    m_summaryWidget->setVisible(false);
}

void TestResultsPane::visibilityChanged(bool)
{
}

void TestResultsPane::setFocus()
{
}

bool TestResultsPane::hasFocus() const
{
    return m_listView->hasFocus();
}

bool TestResultsPane::canFocus() const
{
    return true;
}

bool TestResultsPane::canNavigate() const
{
    return true;
}

bool TestResultsPane::canNext() const
{
    return m_filterModel->hasResults();
}

bool TestResultsPane::canPrevious() const
{
    return m_filterModel->hasResults();
}

void TestResultsPane::goToNext()
{
    if (!canNext())
        return;

    QModelIndex currentIndex = m_listView->currentIndex();
    if (currentIndex.isValid()) {
        int row = currentIndex.row() + 1;
        if (row == m_filterModel->rowCount(QModelIndex()))
            row = 0;
        currentIndex = m_filterModel->index(row, 0, QModelIndex());
    } else {
        currentIndex = m_filterModel->index(0, 0, QModelIndex());
    }
    m_listView->setCurrentIndex(currentIndex);
    onItemActivated(currentIndex);
}

void TestResultsPane::goToPrev()
{
    if (!canPrevious())
        return;

    QModelIndex currentIndex = m_listView->currentIndex();
    if (currentIndex.isValid()) {
        int row = currentIndex.row() - 1;
        if (row < 0)
            row = m_filterModel->rowCount(QModelIndex()) - 1;
        currentIndex = m_filterModel->index(row, 0, QModelIndex());
    } else {
        currentIndex = m_filterModel->index(m_filterModel->rowCount(QModelIndex()) - 1, 0, QModelIndex());
    }
    m_listView->setCurrentIndex(currentIndex);
    onItemActivated(currentIndex);
}

void TestResultsPane::onItemActivated(const QModelIndex &index)
{
    if (!index.isValid())
        return;

    TestResult tr = m_filterModel->testResult(index);
    if (!tr.fileName().isEmpty())
        Core::EditorManager::openEditorAt(tr.fileName(), tr.line(), 0);
}

void TestResultsPane::onRunAllTriggered()
{
    TestRunner *runner = TestRunner::instance();
    runner->setSelectedTests(TestTreeModel::instance()->getAllTestCases());
    runner->runTests();
}

void TestResultsPane::onRunSelectedTriggered()
{
    TestRunner *runner = TestRunner::instance();
    runner->setSelectedTests(TestTreeModel::instance()->getSelectedTests());
    runner->runTests();
}

void TestResultsPane::initializeFilterMenu()
{
    QMap<ResultType, QString> textAndType;
    textAndType.insert(ResultType::PASS, tr("Pass"));
    textAndType.insert(ResultType::FAIL, tr("Fail"));
    textAndType.insert(ResultType::EXPECTED_FAIL, tr("Expected Fail"));
    textAndType.insert(ResultType::UNEXPECTED_PASS, tr("Unexpected Pass"));
    textAndType.insert(ResultType::SKIP, tr("Skip"));
    textAndType.insert(ResultType::MESSAGE_DEBUG, tr("Debug Messages"));
    textAndType.insert(ResultType::MESSAGE_WARN, tr("Warning Messages"));
    textAndType.insert(ResultType::MESSAGE_INTERNAL, tr("Internal Messages"));
    foreach (ResultType result, textAndType.keys()) {
        QAction *action = new QAction(m_filterMenu);
        action->setText(textAndType.value(result));
        action->setCheckable(true);
        action->setChecked(true);
        action->setData(result);
        m_filterMenu->addAction(action);
    }
}

void TestResultsPane::updateSummaryLabel()
{
    QString labelText = QString::fromLatin1("<p><b>Test Summary:</b>&nbsp;&nbsp; %1 %2, %3 %4")
            .arg(QString::number(m_model->resultTypeCount(ResultType::PASS)), tr("passes"),
                 QString::number(m_model->resultTypeCount(ResultType::FAIL)), tr("fails"));
    int count = m_model->resultTypeCount(ResultType::UNEXPECTED_PASS);
    if (count)
        labelText.append(QString::fromLatin1(", %1 %2")
                         .arg(QString::number(count), tr("unexpected passes")));
    count = m_model->resultTypeCount(ResultType::EXPECTED_FAIL);
    if (count)
        labelText.append(QString::fromLatin1(", %1 %2")
                         .arg(QString::number(count), tr("expected fails")));
    labelText.append(QLatin1String(".</p>"));
    m_summaryLabel->setText(labelText);
}

void TestResultsPane::updateFilterMenu()
{
    foreach (QAction *action, m_filterMenu->actions()) {
        action->setEnabled(m_model->hasResultType(
                               static_cast<ResultType>(action->data().value<int>())));
    }
}

void TestResultsPane::filterMenuTriggered(QAction *action)
{
    m_filterModel->toggleTestResultType(static_cast<ResultType>(action->data().value<int>()));
    navigateStateChanged();
}

void TestResultsPane::onTestRunStarted()
{
    m_stopTestRun->setEnabled(true);
    m_runAll->setEnabled(false);
    m_runSelected->setEnabled(false);
    m_summaryWidget->setVisible(false);
}

void TestResultsPane::onTestRunFinished()
{
    m_stopTestRun->setEnabled(false);
    m_runAll->setEnabled(true);
    m_runSelected->setEnabled(true);
    updateSummaryLabel();
    m_summaryWidget->setVisible(true);
}

void TestResultsPane::onTestTreeModelChanged()
{
    bool enable = TestTreeModel::instance()->hasTests();
    m_runAll->setEnabled(enable);
    m_runSelected->setEnabled(enable);
}

} // namespace Internal
} // namespace Autotest
