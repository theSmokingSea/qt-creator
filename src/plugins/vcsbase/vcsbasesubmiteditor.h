// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0+ OR GPL-3.0 WITH Qt-GPL-exception-1.0

#pragma once

#include "vcsbase_global.h"

#include <coreplugin/editormanager/ieditor.h>

#include <utils/aspects.h>

#include <QAbstractItemView>

QT_BEGIN_NAMESPACE
class QAction;
class QIcon;
QT_END_NAMESPACE

namespace VcsBase {
namespace Internal { class SubmitEditorFile; }

class SubmitEditorWidget;
class SubmitFileModel;
class VcsBasePluginPrivate;
class VcsBaseSubmitEditorPrivate;

class VCSBASE_EXPORT VcsBaseSubmitEditorParameters
{
public:
    const char *mimeType;
    const char *id;
    const char *displayName;
    enum DiffType { DiffRows, DiffFiles } diffType;
};

class VCSBASE_EXPORT VcsBaseSubmitEditor : public Core::IEditor
{
    Q_OBJECT

protected:
    explicit VcsBaseSubmitEditor(SubmitEditorWidget *editorWidget);

public:
    // Register the actions with the submit editor widget.
    void registerActions(QAction *editorUndoAction,  QAction *editorRedoAction,
                         QAction *submitAction = nullptr, QAction *diffAction = nullptr);

    ~VcsBaseSubmitEditor() override;

    // A utility routine to be called when closing a submit editor.
    // Runs checks on the message and prompts according to configuration.
    // Force prompt should be true if it is invoked by closing an editor
    // as opposed to invoking the "Submit" button.
    // 'promptSetting' points to a bool variable containing the plugin's
    // prompt setting. The user can uncheck it from the message box.
    enum PromptSubmitResult { SubmitConfirmed, SubmitCanceled, SubmitDiscarded };
    PromptSubmitResult promptSubmit(VcsBasePluginPrivate *plugin);

    QAbstractItemView::SelectionMode fileListSelectionMode() const;
    void setFileListSelectionMode(QAbstractItemView::SelectionMode sm);

    // 'Commit' action enabled despite empty file list
    bool isEmptyFileListEnabled() const;
    void setEmptyFileListEnabled(bool e);

    bool lineWrap() const;
    void setLineWrap(bool);

    int lineWrapWidth() const;
    void setLineWrapWidth(int);

    QString checkScriptWorkingDirectory() const;
    void setCheckScriptWorkingDirectory(const Utils::FilePath &);

    Core::IDocument *document() const override;

    QWidget *toolBar() override;

    QStringList checkedFiles() const;

    void setFileModel(SubmitFileModel *m);
    SubmitFileModel *fileModel() const;
    virtual void updateFileModel() { }
    QStringList rowsToFiles(const QList<int> &rows) const;

    // Utilities returning some predefined icons for actions
    static QIcon diffIcon();
    static QIcon submitIcon();

    // Reduce a list of untracked files reported by a VCS down to the files
    // that are actually part of the current project(s).
    static void filterUntrackedFilesOfProject(const QString &repositoryDirectory, QStringList *untrackedFiles);

signals:
    void diffSelectedFiles(const QStringList &files);
    void diffSelectedRows(const QList<int> &rows);
    void fileContentsChanged();

protected:
    /* These hooks allow for modifying the contents that goes to
     * the file. The default implementation uses the text
     * of the description editor. */
    virtual QByteArray fileContents() const;
    virtual bool setFileContents(const QByteArray &contents);

    QString description() const;
    void setDescription(const QString &text);

    void setDescriptionMandatory(bool v);
    bool isDescriptionMandatory() const;

private:
    friend class VcsSubmitEditorFactory; // for setParameters()
    void setParameters(const VcsBaseSubmitEditorParameters &parameters);

    void slotDiffSelectedVcsFiles(const QList<int> &rawList);
    void slotCheckSubmitMessage();
    void slotInsertNickName();
    void slotSetFieldNickName(int);
    void slotUpdateEditorSettings();

    void createUserFields(const QString &fieldConfigFile);
    bool checkSubmitMessage(QString *errorMessage) const;
    bool runSubmitMessageCheckScript(const QString &script, QString *errorMessage) const;
    QString promptForNickName();

    VcsBaseSubmitEditorPrivate *d = nullptr;

    friend class Internal::SubmitEditorFile; // for the file contents
};

} // namespace VcsBase
