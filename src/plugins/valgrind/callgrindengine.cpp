/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
****************************************************************************/

#include "callgrindengine.h"

#include "callgrindtool.h"
#include "valgrindsettings.h"

#include <valgrind/callgrind/callgrindcontroller.h>
#include <valgrind/callgrind/callgrindparser.h>
#include <valgrind/valgrindrunner.h>

#include <debugger/analyzer/analyzermanager.h>

#include <utils/filepath.h>
#include <utils/qtcassert.h>
#include <utils/qtcprocess.h>

using namespace ProjectExplorer;
using namespace Valgrind::Callgrind;
using namespace Utils;

namespace Valgrind {
namespace Internal {

void setupCallgrindRunner(CallgrindToolRunner *);

CallgrindToolRunner::CallgrindToolRunner(RunControl *runControl)
    : ValgrindToolRunner(runControl)
{
    setId("CallgrindToolRunner");

    connect(&m_runner, &ValgrindRunner::finished,
            this, &CallgrindToolRunner::slotFinished);
    connect(&m_parser, &Callgrind::Parser::parserDataReady,
            this, &CallgrindToolRunner::slotFinished);

    connect(&m_controller, &CallgrindController::finished,
            this, &CallgrindToolRunner::controllerFinished);
    connect(&m_controller, &CallgrindController::localParseDataAvailable,
            this, &CallgrindToolRunner::handleLocalParseData);
    connect(&m_controller, &CallgrindController::statusMessage,
            this, &CallgrindToolRunner::showStatusMessage);

    connect(&m_runner, &ValgrindRunner::valgrindStarted,
            &m_controller, &CallgrindController::setValgrindPid);

    connect(&m_runner, &ValgrindRunner::extraProcessFinished, this, [this] {
        triggerParse();
    });

    m_controller.setValgrindRunnable(runControl->runnable());

    static int fileCount = 100;
    m_valgrindOutputFile = runControl->workingDirectory() / QString("callgrind.out.f%1").arg(++fileCount);
    m_controller.setValgrindOutputFile(m_valgrindOutputFile);

    setupCallgrindRunner(this);
}

QStringList CallgrindToolRunner::toolArguments() const
{
    QStringList arguments = {"--tool=callgrind"};

    if (m_settings.enableCacheSim.value())
        arguments << "--cache-sim=yes";

    if (m_settings.enableBranchSim.value())
        arguments << "--branch-sim=yes";

    if (m_settings.collectBusEvents.value())
        arguments << "--collect-bus=yes";

    if (m_settings.collectSystime.value())
        arguments << "--collect-systime=yes";

    if (m_markAsPaused)
        arguments << "--instr-atstart=no";

    // add extra arguments
    if (!m_argumentForToggleCollect.isEmpty())
        arguments << m_argumentForToggleCollect;

    arguments << "--callgrind-out-file=" + m_valgrindOutputFile.path();

    arguments << Utils::ProcessArgs::splitArgs(m_settings.callgrindArguments.value());

    return arguments;
}

QString CallgrindToolRunner::progressTitle() const
{
    return tr("Profiling");
}

void CallgrindToolRunner::start()
{
    appendMessage(tr("Profiling %1").arg(executable().toUserOutput()), Utils::NormalMessageFormat);
    return ValgrindToolRunner::start();
}

void CallgrindToolRunner::dump()
{
    m_controller.run(CallgrindController::Dump);
}

void CallgrindToolRunner::setPaused(bool paused)
{
    if (m_markAsPaused == paused)
        return;

    m_markAsPaused = paused;

    // call controller only if it is attached to a valgrind process
    if (paused)
        pause();
    else
        unpause();
}

void CallgrindToolRunner::setToggleCollectFunction(const QString &toggleCollectFunction)
{
    if (toggleCollectFunction.isEmpty())
        return;

    m_argumentForToggleCollect = "--toggle-collect=" + toggleCollectFunction;
}

void CallgrindToolRunner::reset()
{
    m_controller.run(Callgrind::CallgrindController::ResetEventCounters);
}

void CallgrindToolRunner::pause()
{
    m_controller.run(Callgrind::CallgrindController::Pause);
}

void CallgrindToolRunner::unpause()
{
    m_controller.run(Callgrind::CallgrindController::UnPause);
}

Callgrind::ParseData *CallgrindToolRunner::takeParserData()
{
    return m_parser.takeData();
}

void CallgrindToolRunner::slotFinished()
{
    emit parserDataReady(this);
}

void CallgrindToolRunner::showStatusMessage(const QString &message)
{
    Debugger::showPermanentStatusMessage(message);
}

void CallgrindToolRunner::triggerParse()
{
    m_controller.getLocalDataFile();
}

void CallgrindToolRunner::handleLocalParseData(const FilePath &outputFile)
{
    QTC_ASSERT(outputFile.exists(), return);
    showStatusMessage(tr("Parsing Profile Data..."));
    m_parser.parse(outputFile);
}

void CallgrindToolRunner::controllerFinished(CallgrindController::Option option)
{
    switch (option)
    {
    case CallgrindController::Pause:
        m_paused = true;
        break;
    case CallgrindController::UnPause:
        m_paused = false;
        break;
    case CallgrindController::Dump:
        triggerParse();
        break;
    default:
        break; // do nothing
    }
}

} // Internal
} // Valgrind
