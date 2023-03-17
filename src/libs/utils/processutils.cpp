// Copyright (C) 2021 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "processutils.h"

#ifdef Q_OS_WIN
#ifdef QTCREATOR_PCH_H
#define CALLBACK WINAPI
#endif
#include <qt_windows.h>
#else
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace Utils {

QIODevice::OpenMode ProcessStartHandler::openMode() const
{
    if (m_processMode == ProcessMode::Writer)
        return QIODevice::ReadWrite; // some writers also read data
    if (m_writeData.isEmpty())
        return QIODevice::ReadOnly; // only reading
    return QIODevice::ReadWrite; // initial write and then reading (close the write channel)
}

void ProcessStartHandler::handleProcessStart()
{
    if (m_processMode == ProcessMode::Writer)
        return;
    if (m_writeData.isEmpty())
        m_process->closeWriteChannel();
}

void ProcessStartHandler::handleProcessStarted()
{
    if (!m_writeData.isEmpty()) {
        m_process->write(m_writeData);
        m_writeData = {};
        if (m_processMode == ProcessMode::Reader)
            m_process->closeWriteChannel();
    }
}

void ProcessStartHandler::setNativeArguments(const QString &arguments)
{
#ifdef Q_OS_WIN
    if (!arguments.isEmpty())
        m_process->setNativeArguments(arguments);
#else
    Q_UNUSED(arguments)
#endif // Q_OS_WIN
}

void ProcessStartHandler::setWindowsSpecificStartupFlags(bool belowNormalPriority,
                                                         bool createConsoleWindow)
{
#ifdef Q_OS_WIN
    if (!belowNormalPriority && !createConsoleWindow)
        return;

    m_process->setCreateProcessArgumentsModifier(
        [belowNormalPriority, createConsoleWindow](QProcess::CreateProcessArguments *args) {
            if (createConsoleWindow) {
                args->flags |= CREATE_NEW_CONSOLE;
                args->startupInfo->dwFlags &= ~STARTF_USESTDHANDLES;
            }

            if (belowNormalPriority)
                args->flags |= BELOW_NORMAL_PRIORITY_CLASS;
        });
#else // Q_OS_WIN
    Q_UNUSED(belowNormalPriority)
    Q_UNUSED(createConsoleWindow)
#endif
}

#ifdef Q_OS_WIN
static BOOL sendMessage(UINT message, HWND hwnd, LPARAM lParam)
{
    DWORD dwProcessID;
    GetWindowThreadProcessId(hwnd, &dwProcessID);
    if ((DWORD)lParam == dwProcessID) {
        SendNotifyMessage(hwnd, message, 0, 0);
        return FALSE;
    }
    return TRUE;
}

BOOL CALLBACK sendShutDownMessageToAllWindowsOfProcess_enumWnd(HWND hwnd, LPARAM lParam)
{
    static UINT uiShutDownMessage = RegisterWindowMessage(L"qtcctrlcstub_shutdown");
    return sendMessage(uiShutDownMessage, hwnd, lParam);
}

BOOL CALLBACK sendInterruptMessageToAllWindowsOfProcess_enumWnd(HWND hwnd, LPARAM lParam)
{
    static UINT uiInterruptMessage = RegisterWindowMessage(L"qtcctrlcstub_interrupt");
    return sendMessage(uiInterruptMessage, hwnd, lParam);
}
#endif

ProcessHelper::ProcessHelper(QObject *parent)
    : QProcess(parent), m_processStartHandler(this)
{
#if defined(Q_OS_UNIX)
    bool needSetsid = m_unixTerminalDisabled;
#  if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    if (needSetsid) {
        setUnixProcessParameters(QProcess::UnixProcessFlag::CreateNewSession);
        needSetsid = false;
    }
#  endif

    if (!m_lowPriority && !needSetsid)
        return;
    setChildProcessModifier([=, this] {
        // nice value range is -20 to +19 where -20 is highest, 0 default and +19 is lowest
        if (m_lowPriority) {
            errno = 0;
            if (::nice(5) == -1 && errno != 0)
                perror("Failed to set nice value");
        }

        // Disable terminal by becoming a session leader.
        if (needSetsid)
            setsid();
    });
#endif
}

void ProcessHelper::setUseCtrlCStub(bool enabled)
{
    m_useCtrlCStub = enabled;
}

void ProcessHelper::terminateProcess()
{
#ifdef Q_OS_WIN
    if (m_useCtrlCStub)
        EnumWindows(sendShutDownMessageToAllWindowsOfProcess_enumWnd, processId());
    else
        terminate();
#else
    terminate();
#endif
}

void ProcessHelper::terminateProcess(QProcess *process)
{
    ProcessHelper *helper = qobject_cast<ProcessHelper *>(process);
    if (helper)
        helper->terminateProcess();
    else
        process->terminate();
}

void ProcessHelper::interruptPid(qint64 pid)
{
#ifdef Q_OS_WIN
    EnumWindows(sendInterruptMessageToAllWindowsOfProcess_enumWnd, pid);
#else
    ::kill(pid, SIGINT);
#endif
}

void ProcessHelper::interruptProcess(QProcess *process)
{
    ProcessHelper *helper = qobject_cast<ProcessHelper *>(process);
    if (helper && helper->m_useCtrlCStub)
        ProcessHelper::interruptPid(process->processId());
}

} // namespace Utils
