// Copyright (c) 2018 Artur Shepilko
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "pullorpushdialog.h"

#include "constants.h"

#include <utils/layoutbuilder.h>
#include <utils/pathchooser.h>
#include <utils/qtcassert.h>

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QRadioButton>

namespace Fossil {
namespace Internal {

PullOrPushDialog::PullOrPushDialog(Mode mode, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(mode == PullMode ? tr("Pull Source") : tr("Push Destination"));
    resize(600, 0);

    m_defaultButton = new QRadioButton(tr("Default location"));
    m_defaultButton->setChecked(true);

    m_localButton = new QRadioButton(tr("Local filesystem:"));

    m_localPathChooser = new Utils::PathChooser;
    m_localPathChooser->setEnabled(false);
    m_localPathChooser->setExpectedKind(Utils::PathChooser::File);
    m_localPathChooser->setPromptDialogFilter(tr(Constants::FOSSIL_FILE_FILTER));

    m_urlButton = new QRadioButton(tr("Specify URL:"));
    m_urlButton->setToolTip(tr("For example: https://[user[:pass]@]host[:port]/[path]"));

    m_urlLineEdit = new QLineEdit;
    m_urlLineEdit->setEnabled(false);
    m_urlLineEdit->setToolTip(m_urlButton->toolTip());

    m_rememberCheckBox = new QCheckBox(tr("Remember specified location as default"));
    m_rememberCheckBox->setEnabled(false);

    m_privateCheckBox = new QCheckBox(tr("Include private branches"));
    m_privateCheckBox->setToolTip(tr("Allow transfer of private branches."));

    auto buttonBox = new QDialogButtonBox;
    buttonBox->setStandardButtons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

    using namespace Utils::Layouting;
    Column {
        Group {
            title(tr("Remote Location")),
            Form {
                m_defaultButton, br,
                m_localButton, m_localPathChooser, br,
                m_urlButton, m_urlLineEdit, br,
            },
        },
        Group {
            title(tr("Options")),
            Column { m_rememberCheckBox, m_privateCheckBox, },
        },
        buttonBox,
    }.attachTo(this);

    // select URL text in line edit when clicking the radio button
    m_localButton->setFocusProxy(m_localPathChooser);
    m_urlButton->setFocusProxy(m_urlLineEdit);
    connect(m_urlButton, &QRadioButton::clicked, m_urlLineEdit, &QLineEdit::selectAll);
    connect(m_urlButton, &QRadioButton::toggled, m_urlLineEdit, &QLineEdit::setEnabled);
    connect(m_localButton, &QRadioButton::toggled, m_localPathChooser,
            &Utils::PathChooser::setEnabled);
    connect(m_urlButton, &QRadioButton::toggled, m_rememberCheckBox, &QCheckBox::setEnabled);
    connect(m_localButton, &QRadioButton::toggled, m_rememberCheckBox, &QCheckBox::setEnabled);
}

QString PullOrPushDialog::remoteLocation() const
{
    if (m_defaultButton->isChecked())
        return QString();
    if (m_localButton->isChecked())
        return m_localPathChooser->filePath().toString();
    return m_urlLineEdit->text();
}

bool PullOrPushDialog::isRememberOptionEnabled() const
{
    if (m_defaultButton->isChecked())
        return false;
    return m_rememberCheckBox->isChecked();
}

bool PullOrPushDialog::isPrivateOptionEnabled() const
{
    return m_privateCheckBox->isChecked();
}

void PullOrPushDialog::setDefaultRemoteLocation(const QString &url)
{
    m_urlLineEdit->setText(url);
}

void PullOrPushDialog::setLocalBaseDirectory(const QString &dir)
{
    m_localPathChooser->setBaseDirectory(Utils::FilePath::fromString(dir));
}

} // namespace Internal
} // namespace Fossil
