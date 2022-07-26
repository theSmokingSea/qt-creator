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

#include <coreplugin/locator/ilocatorfilter.h>

#include <QIcon>
#include <QMultiMap>
#include <QSet>
#include <QUrl>

#include <atomic>

namespace Help {
namespace Internal {

class HelpIndexFilter final : public Core::ILocatorFilter
{
    Q_OBJECT

public:
    HelpIndexFilter();
    ~HelpIndexFilter() final;

    // ILocatorFilter
    QList<Core::LocatorFilterEntry> matchesFor(QFutureInterface<Core::LocatorFilterEntry> &future,
                                               const QString &entry) override;
    void accept(const Core::LocatorFilterEntry &selection,
                QString *newText, int *selectionStart, int *selectionLength) const override;
    void refresh(QFutureInterface<void> &future) override;

    QStringList allIndices() const;

signals:
    void linksActivated(const QMultiMap<QString, QUrl> &links, const QString &key) const;

private:
    void invalidateCache();

    bool updateCache(QFutureInterface<Core::LocatorFilterEntry> &future,
                     const QStringList &cache, const QString &entry);

    QStringList m_allIndicesCache;
    QStringList m_lastIndicesCache;
    QString m_lastEntry;
    std::atomic_bool m_needsUpdate = true;
    QIcon m_icon;
};

} // namespace Internal
} // namespace Help
