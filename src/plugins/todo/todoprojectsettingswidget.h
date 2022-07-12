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

#pragma once

#include <projectexplorer/projectsettingswidget.h>

#include <QWidget>

QT_BEGIN_NAMESPACE
class QListWidget;
class QListWidgetItem;
class QPushButton;
QT_END_NAMESPACE

namespace ProjectExplorer { class Project; }

namespace Todo {
namespace Internal {

class TodoProjectSettingsWidget : public ProjectExplorer::ProjectSettingsWidget
{
    Q_OBJECT

public:
    explicit TodoProjectSettingsWidget(ProjectExplorer::Project *project);
    ~TodoProjectSettingsWidget() override;

signals:
    void projectSettingsChanged();

private:
    void addExcludedPatternButtonClicked();
    void removeExcludedPatternButtonClicked();
    void setExcludedPatternsButtonsEnabled();
    void excludedPatternChanged(QListWidgetItem *item);
    QListWidgetItem *addToExcludedPatternsList(const QString &pattern);
    void loadSettings();
    void saveSettings();
    void prepareItem(QListWidgetItem *item) const;

    ProjectExplorer::Project *m_project;
    QListWidget *m_excludedPatternsList;
    QPushButton *m_removeExcludedPatternButton;
};

} // namespace Internal
} // namespace Todo
