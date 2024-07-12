// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "cmakebuildsystem.h"

#include "builddirparameters.h"
#include "cmakebuildconfiguration.h"
#include "cmakebuildstep.h"
#include "cmakebuildtarget.h"
#include "cmakekitaspect.h"
#include "cmakeprocess.h"
#include "cmakeproject.h"
#include "cmakeprojectconstants.h"
#include "cmakeprojectmanagertr.h"
#include "cmakespecificsettings.h"
#include "cmaketoolmanager.h"
#include "projecttreehelper.h"

// #include <android/androidconstants.h>

#include <coreplugin/icore.h>
#include <coreplugin/documentmanager.h>
#include <coreplugin/editormanager/editormanager.h>
#include <coreplugin/messagemanager.h>
#include <coreplugin/progressmanager/progressmanager.h>

#include <projectexplorer/buildmanager.h>
#include <projectexplorer/extracompiler.h>
#include <projectexplorer/kitaspects.h>
#include <projectexplorer/projectexplorer.h>
#include <projectexplorer/projectexplorerconstants.h>
#include <projectexplorer/projectupdater.h>
#include <projectexplorer/target.h>
#include <projectexplorer/taskhub.h>

#include <texteditor/texteditor.h>
#include <texteditor/textdocument.h>

#include <qmljs/qmljsmodelmanagerinterface.h>

// #include <qtapplicationmanager/appmanagerconstants.h>

#include <qtsupport/qtcppkitinfo.h>
#include <qtsupport/qtsupportconstants.h>

#include <utils/algorithm.h>
#include <utils/checkablemessagebox.h>
#include <utils/macroexpander.h>
#include <utils/mimeconstants.h>
#include <utils/qtcprocess.h>
#include <utils/qtcassert.h>

#include <QClipboard>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>

using namespace ProjectExplorer;
using namespace TextEditor;
using namespace Utils;

namespace CMakeProjectManager::Internal {

static Q_LOGGING_CATEGORY(cmakeBuildSystemLog, "qtc.cmake.buildsystem", QtWarningMsg);

// --------------------------------------------------------------------
// CMakeBuildSystem:
// --------------------------------------------------------------------

CMakeBuildSystem::CMakeBuildSystem(CMakeBuildConfiguration *bc)
    : BuildSystem(bc)
    , m_cppCodeModelUpdater(ProjectUpdaterFactory::createCppProjectUpdater())
{
    // TreeScanner:
    connect(&m_treeScanner, &TreeScanner::finished,
            this, &CMakeBuildSystem::handleTreeScanningFinished);

    m_treeScanner.setFilter([this](const MimeType &mimeType, const FilePath &fn) {
        // Mime checks requires more resources, so keep it last in check list
        auto isIgnored = TreeScanner::isWellKnownBinary(mimeType, fn);

        // Cache mime check result for speed up
        if (!isIgnored) {
            if (auto it = m_mimeBinaryCache.get<std::optional<bool>>(
                    [mimeType](const QHash<QString, bool> &cache) -> std::optional<bool> {
                        auto cache_it = cache.find(mimeType.name());
                        if (cache_it != cache.end())
                            return *cache_it;
                        return {};
                    })) {
                isIgnored = *it;
            } else {
                isIgnored = TreeScanner::isMimeBinary(mimeType, fn);
                m_mimeBinaryCache.writeLocked()->insert(mimeType.name(), isIgnored);
            }
        }

        return isIgnored;
    });

    m_treeScanner.setTypeFactory([](const MimeType &mimeType, const FilePath &fn) {
        auto type = TreeScanner::genericFileType(mimeType, fn);
        if (type == FileType::Unknown) {
            if (mimeType.isValid()) {
                const QString mt = mimeType.name();
                if (mt == Utils::Constants::CMAKE_PROJECT_MIMETYPE
                    || mt == Utils::Constants::CMAKE_MIMETYPE)
                    type = FileType::Project;
            }
        }
        return type;
    });

    connect(&m_reader, &FileApiReader::configurationStarted, this, [this] {
        clearError(ForceEnabledChanged::True);
    });

    connect(&m_reader,
            &FileApiReader::dataAvailable,
            this,
            &CMakeBuildSystem::handleParsingSucceeded);
    connect(&m_reader, &FileApiReader::errorOccurred, this, &CMakeBuildSystem::handleParsingFailed);
    connect(&m_reader, &FileApiReader::dirty, this, &CMakeBuildSystem::becameDirty);
    connect(&m_reader, &FileApiReader::debuggingStarted, this, &BuildSystem::debuggingStarted);

    wireUpConnections();

    m_isMultiConfig = CMakeGeneratorKitAspect::isMultiConfigGenerator(bc->kit());
}

CMakeBuildSystem::~CMakeBuildSystem()
{
    if (!m_treeScanner.isFinished()) {
        auto future = m_treeScanner.future();
        future.cancel();
        future.waitForFinished();
    }

    delete m_cppCodeModelUpdater;
    qDeleteAll(m_extraCompilers);
}

void CMakeBuildSystem::triggerParsing()
{
    qCDebug(cmakeBuildSystemLog) << buildConfiguration()->displayName() << "Parsing has been triggered";

    if (!buildConfiguration()->isActive()) {
        qCDebug(cmakeBuildSystemLog)
            << "Parsing has been triggered: SKIPPING since BC is not active -- clearing state.";
        stopParsingAndClearState();
        return; // ignore request, this build configuration is not active!
    }

    auto guard = guardParsingRun();

    if (!guard.guardsProject()) {
        // This can legitimately trigger if e.g. Build->Run CMake
        // is selected while this here is already running.

        // Stop old parse run and keep that ParseGuard!
        qCDebug(cmakeBuildSystemLog) << "Stopping current parsing run!";
        stopParsingAndClearState();
    } else {
        // Use new ParseGuard
        m_currentGuard = std::move(guard);
    }
    QTC_ASSERT(!m_reader.isParsing(), return );

    qCDebug(cmakeBuildSystemLog) << "ParseGuard acquired.";

    int reparseParameters = takeReparseParameters();

    m_waitingForParse = true;
    m_combinedScanAndParseResult = true;

    QTC_ASSERT(m_parameters.isValid(), return );

    TaskHub::clearTasks(ProjectExplorer::Constants::TASK_CATEGORY_BUILDSYSTEM);

    qCDebug(cmakeBuildSystemLog) << "Parse called with flags:"
                                 << reparseParametersString(reparseParameters);

    const FilePath cache = m_parameters.buildDirectory.pathAppended(Constants::CMAKE_CACHE_TXT);
    if (!cache.exists()) {
        reparseParameters |= REPARSE_FORCE_INITIAL_CONFIGURATION | REPARSE_FORCE_CMAKE_RUN;
        qCDebug(cmakeBuildSystemLog)
            << "No" << cache
            << "file found, new flags:" << reparseParametersString(reparseParameters);
    }

    if ((0 == (reparseParameters & REPARSE_FORCE_EXTRA_CONFIGURATION))
        && mustApplyConfigurationChangesArguments(m_parameters)) {
        reparseParameters |= REPARSE_FORCE_CMAKE_RUN | REPARSE_FORCE_EXTRA_CONFIGURATION;
    }

    // The code model will be updated after the CMake run. There is no need to have an
    // active code model updater when the next one will be triggered.
    m_cppCodeModelUpdater->cancel();

    const CMakeTool *tool = m_parameters.cmakeTool();
    CMakeTool::Version version = tool ? tool->version() : CMakeTool::Version();
    const bool isDebuggable = (version.major == 3 && version.minor >= 27) || version.major > 3;

    qCDebug(cmakeBuildSystemLog) << "Asking reader to parse";
    m_reader.parse(reparseParameters & REPARSE_FORCE_CMAKE_RUN,
                   reparseParameters & REPARSE_FORCE_INITIAL_CONFIGURATION,
                   reparseParameters & REPARSE_FORCE_EXTRA_CONFIGURATION,
                   (reparseParameters & REPARSE_DEBUG) && isDebuggable,
                   reparseParameters & REPARSE_PROFILING);
}

void CMakeBuildSystem::requestDebugging()
{
    qCDebug(cmakeBuildSystemLog) << "Requesting parse due to \"Rescan Project\" command";
    reparse(REPARSE_FORCE_CMAKE_RUN | REPARSE_FORCE_EXTRA_CONFIGURATION | REPARSE_URGENT
            | REPARSE_DEBUG);
}

bool CMakeBuildSystem::supportsAction(Node *context, ProjectAction action, const Node *node) const
{
    const auto cmakeTarget = dynamic_cast<CMakeTargetNode *>(context);
    if (cmakeTarget) {
        const auto buildTarget = Utils::findOrDefault(m_buildTargets,
                                                      [cmakeTarget](const CMakeBuildTarget &bt) {
                                                          return bt.title
                                                                 == cmakeTarget->buildKey();
                                                      });
        if (buildTarget.targetType != UtilityType)
            return action == ProjectAction::AddNewFile || action == ProjectAction::AddExistingFile
                   || action == ProjectAction::AddExistingDirectory
                   || action == ProjectAction::Rename || action == ProjectAction::RemoveFile;
    }

    return BuildSystem::supportsAction(context, action, node);
}

static QString relativeFilePaths(const FilePaths &filePaths, const FilePath &projectDir)
{
    return Utils::transform(filePaths, [projectDir](const FilePath &path) {
        return path.canonicalPath().relativePathFrom(projectDir).cleanPath().toString();
    }).join(' ');
};

static QString newFilesForFunction(const std::string &cmakeFunction,
                                   const FilePaths &filePaths,
                                   const FilePath &projDir)
{
    if (cmakeFunction == "qt_add_qml_module" || cmakeFunction == "qt6_add_qml_module") {
        FilePaths sourceFiles;
        FilePaths resourceFiles;
        FilePaths qmlFiles;

        for (const auto &file : filePaths) {
            using namespace Utils::Constants;
            const auto mimeType = Utils::mimeTypeForFile(file);
            if (mimeType.matchesName(CPP_SOURCE_MIMETYPE)
                || mimeType.matchesName(CPP_HEADER_MIMETYPE)
                || mimeType.matchesName(OBJECTIVE_C_SOURCE_MIMETYPE)
                || mimeType.matchesName(OBJECTIVE_CPP_SOURCE_MIMETYPE)) {
                sourceFiles << file;
            } else if (mimeType.matchesName(QML_MIMETYPE)
                       || mimeType.matchesName(QMLUI_MIMETYPE)
                       || mimeType.matchesName(QMLPROJECT_MIMETYPE)
                       || mimeType.matchesName(JS_MIMETYPE)
                       || mimeType.matchesName(JSON_MIMETYPE)) {
                qmlFiles << file;
            } else {
                resourceFiles << file;
            }
        }

        QStringList result;
        if (!sourceFiles.isEmpty())
            result << QString("SOURCES %1").arg(relativeFilePaths(sourceFiles, projDir));
        if (!resourceFiles.isEmpty())
            result << QString("RESOURCES %1").arg(relativeFilePaths(resourceFiles, projDir));
        if (!qmlFiles.isEmpty())
            result << QString("QML_FILES %1").arg(relativeFilePaths(qmlFiles, projDir));

        return result.join("\n");
    }

    return relativeFilePaths(filePaths, projDir);
}

static std::optional<Link> cmakeFileForBuildKey(const QString &buildKey,
                                                const QList<CMakeBuildTarget> &targets)
{
    auto target = Utils::findOrDefault(targets, [buildKey](const CMakeBuildTarget &target) {
        return target.title == buildKey;
    });
    if (target.backtrace.isEmpty()) {
        qCCritical(cmakeBuildSystemLog) << "target.backtrace for" << buildKey << "is empty."
                                        << "The location where to add the files is unknown.";
        return std::nullopt;
    }
    return std::make_optional(Link(target.backtrace.last().path, target.backtrace.last().line));
}

static std::optional<cmListFile> getUncachedCMakeListFile(const FilePath &targetCMakeFile)
{
    // Have a fresh look at the CMake file, not relying on a cached value
    Core::DocumentManager::saveModifiedDocumentSilently(
        Core::DocumentModel::documentForFilePath(targetCMakeFile));
    expected_str<QByteArray> fileContent = targetCMakeFile.fileContents();
    cmListFile cmakeListFile;
    std::string errorString;
    if (fileContent) {
        fileContent = fileContent->replace("\r\n", "\n");
        if (!cmakeListFile.ParseString(fileContent->toStdString(),
                                       targetCMakeFile.fileName().toStdString(),
                                       errorString)) {
            qCCritical(cmakeBuildSystemLog).noquote() << targetCMakeFile.toUserOutput()
                                                      << "failed to parse! Error:"
                                                      << QString::fromStdString(errorString);
            return std::nullopt;
        }
    }
    return std::make_optional(cmakeListFile);
}

static std::optional<cmListFileFunction> findFunction(
        const cmListFile &cmakeListFile, std::function<bool(const cmListFileFunction &)> pred,
        bool reverse = false)
{
    if (reverse) {
        auto function = std::find_if(cmakeListFile.Functions.rbegin(),
                                     cmakeListFile.Functions.rend(), pred);
        if (function == cmakeListFile.Functions.rend())
            return std::nullopt;
        return std::make_optional(*function);
    }
    auto function
            = std::find_if(cmakeListFile.Functions.begin(), cmakeListFile.Functions.end(), pred);
    if (function == cmakeListFile.Functions.end())
        return std::nullopt;
    return std::make_optional(*function);
}

struct SnippetAndLocation
{
    QString snippet;
    long line = -1;
    long column = -1;
};

static SnippetAndLocation generateSnippetAndLocationForSources(
        const QString &newSourceFiles,
        const cmListFile &cmakeListFile,
        const cmListFileFunction &function,
        const QString &targetName)
{
    static QSet<std::string> knownFunctions{"add_executable",
                                            "add_library",
                                            "qt_add_executable",
                                            "qt_add_library",
                                            "qt6_add_executable",
                                            "qt6_add_library",
                                            "qt_add_qml_module",
                                            "qt6_add_qml_module"};
    SnippetAndLocation result;
    int extraChars = 0;
    auto afterFunctionLastArgument =
        [&result, &extraChars, newSourceFiles](const auto &f) {
            auto lastArgument = f.Arguments().back();
            result.line = lastArgument.Line;
            result.column = lastArgument.Column + static_cast<int>(lastArgument.Value.size()) - 1;
            result.snippet = QString("\n%1").arg(newSourceFiles);
            // Take into consideration the quotes
            if (lastArgument.Delim == cmListFileArgument::Quoted)
                extraChars = 2;
        };
    if (knownFunctions.contains(function.LowerCaseName())) {
        afterFunctionLastArgument(function);
    } else {
        const std::string target_name = targetName.toStdString();
        auto targetSources = [target_name](const auto &func) {
            return func.LowerCaseName() == "target_sources"
                    && func.Arguments().size() && func.Arguments().front().Value == target_name;
        };
        std::optional<cmListFileFunction> targetSourcesFunc = findFunction(cmakeListFile,
                                                                           targetSources);
        if (!targetSourcesFunc.has_value()) {
            result.line = function.LineEnd() + 1;
            result.column = 0;
            result.snippet = QString("\ntarget_sources(%1\n  PRIVATE\n    %2\n)\n")
                                 .arg(targetName)
                                 .arg(newSourceFiles);
        } else {
            afterFunctionLastArgument(*targetSourcesFunc);
        }
    }
    if (extraChars)
        result.line += extraChars;
    return result;
}
static expected_str<bool> insertSnippetSilently(const FilePath &cmakeFile,
                                                const SnippetAndLocation &snippetLocation)
{
    BaseTextEditor *editor = qobject_cast<BaseTextEditor *>(Core::EditorManager::openEditorAt(
        {cmakeFile, int(snippetLocation.line), int(snippetLocation.column)},
        Constants::CMAKE_EDITOR_ID,
        Core::EditorManager::DoNotMakeVisible | Core::EditorManager::DoNotChangeCurrentEditor));
    if (!editor) {
        return make_unexpected("BaseTextEditor cannot be obtained for " + cmakeFile.toUserOutput()
                               + ":" + QString::number(snippetLocation.line) + ":"
                               + QString::number(snippetLocation.column));
    }
    editor->insert(snippetLocation.snippet);
    editor->editorWidget()->autoIndent();
    if (!Core::DocumentManager::saveDocument(editor->document()))
        return make_unexpected("Changes to " + cmakeFile.toUserOutput() + " could not be saved.");
    return true;
}

static void findLastRelevantArgument(const cmListFileFunction &function,
                                     int minimumArgPos,
                                     const QSet<QString> &lowerCaseStopParams,
                                     QString *lastRelevantArg,
                                     int *lastRelevantPos)
{
    const std::vector<cmListFileArgument> args = function.Arguments();
    *lastRelevantPos = int(args.size()) - 1;
    for (int i = minimumArgPos, end = int(args.size()); i < end; ++i) {
        const QString lowerArg = QString::fromStdString(args.at(i).Value).toLower();
        if (lowerCaseStopParams.contains(lowerArg)) {
            *lastRelevantPos = i - 1;
            break;
        }
        *lastRelevantArg = lowerArg;
    }
}

static std::optional<cmListFileFunction> findSetFunctionFor(const cmListFile &cmakeListFile,
                                                            const QString &lowerVariableName)
{
    auto findSetFunc = [lowerVariableName](const auto &func) {
        if (func.LowerCaseName() != "set")
            return false;
        std::vector<cmListFileArgument> args = func.Arguments();
        return args.size()
                && QString::fromStdString(args.front().Value).toLower() == lowerVariableName;
    };
    return findFunction(cmakeListFile, findSetFunc);
}

static std::optional<cmListFileFunction> handleTSAddVariant(const cmListFile &cmakeListFile,
                                                            const QSet<QString> &lowerFunctionNames,
                                                            std::optional<QString> targetName,
                                                            const QSet<QString> &stopParams,
                                                            int *lastArgumentPos)
{
    std::optional<cmListFileFunction> function;
    auto currentFunc = findFunction(cmakeListFile, [lowerFunctionNames, targetName](const auto &func) {
        auto lower = QString::fromStdString(func.LowerCaseName());
        if (lowerFunctionNames.contains(lower)) {
            if (!targetName)
                return true;
            const std::vector<cmListFileArgument> args = func.Arguments();
            if (args.size())
                return *targetName == QString::fromStdString(args.front().Value);
        }
        return false;
    });
    if (currentFunc) {
        QString lastRelevant;
        const int argsMinimumPos = targetName.has_value() ? 2 : 1;

        findLastRelevantArgument(*currentFunc, argsMinimumPos, stopParams,
                                 &lastRelevant, lastArgumentPos);
        // handle argument
        if (!lastRelevant.isEmpty() && lastRelevant.startsWith('$')) {
            QString var = lastRelevant.mid(1);
            if (var.startsWith('{') && var.endsWith('}'))
                var = var.mid(1, var.size() - 2);
            if (!var.isEmpty()) {
                std::optional<cmListFileFunction> setFunc = findSetFunctionFor(cmakeListFile, var);
                if (setFunc) {
                    function = *setFunc;
                    *lastArgumentPos = int(function->Arguments().size()) - 1;
                }
            }
        }
        if (!function.has_value()) // no variable used or we failed to find respective SET()
            function = currentFunc;
    }
    return function;
}

static std::optional<cmListFileFunction> handleQtAddTranslations(const cmListFile &cmakeListFile,
                                                                 std::optional<QString> targetName,
                                                                 int *lastArgumentPos)
{
    const QSet<QString> stopParams{"resource_prefix", "output_targets",
                                   "qm_files_output_variable", "sources", "include_directories",
                                   "lupdate_options", "lrelease_options"};
    return handleTSAddVariant(cmakeListFile, {"qt6_add_translations", "qt_add_translations"},
                              targetName, stopParams, lastArgumentPos);
}

static std::optional<cmListFileFunction> handleQtAddLupdate(const cmListFile &cmakeListFile,
                                                            std::optional<QString> targetName,
                                                            int *lastArgumentPos)
{
    const QSet<QString> stopParams{"sources", "include_directories", "no_global_target", "options"};
    return handleTSAddVariant(cmakeListFile, {"qt6_add_lupdate", "qt_add_lupdate"},
                              targetName, stopParams, lastArgumentPos);
}

static std::optional<cmListFileFunction> handleQtCreateTranslation(const cmListFile &cmakeListFile,
                                                                   int *lastArgumentPos)
{
    return handleTSAddVariant(cmakeListFile, {"qt_create_translation", "qt5_create_translation"},
                              std::nullopt, {"options"}, lastArgumentPos);
}


static expected_str<bool> insertQtAddTranslations(const cmListFile &cmakeListFile,
                                                  const FilePath &targetCmakeFile,
                                                  const QString &targetName,
                                                  int targetDefinitionLine,
                                                  const QString &filesToAdd,
                                                  int qtMajorVersion,
                                                  bool addLinguist)
{
    std::optional<cmListFileFunction> function
        = findFunction(cmakeListFile, [targetDefinitionLine](const auto &func) {
              return func.Line() == targetDefinitionLine;
          });
    if (!function.has_value())
        return false;

    // FIXME: room for improvement
    // * this just updates "the current cmake path" for e.g. conditional setups like
    //   differentiating between desktop and device build config we do not update all
    QString snippet;
    if (qtMajorVersion == 5)
        snippet = QString("\nqt_create_translation(QM_FILES %1)\n").arg(filesToAdd);
    else
        snippet = QString("\nqt_add_translations(%1 TS_FILES %2)\n").arg(targetName, filesToAdd);

    const int insertionLine = function->LineEnd() + 1;
    expected_str<bool> inserted = insertSnippetSilently(targetCmakeFile,
                                                        {snippet, insertionLine, 0});
    if (!inserted || !addLinguist)
        return inserted;

    function = findFunction(cmakeListFile, [](const auto &func) {
        return func.LowerCaseName() == "find_package";
    }, /* reverse = */ true);
    if (!function.has_value()) {
        qCCritical(cmakeBuildSystemLog) << "Failed to find a find_package().";
        return inserted; // we just fail to insert LinguistTool, but otherwise succeeded
    }
    if (insertionLine < function->LineEnd() + 1) {
        qCCritical(cmakeBuildSystemLog) << "find_package() calls after old insertion. "
                                           "Refusing to process.";
        return inserted; // we just fail to insert LinguistTool, but otherwise succeeded
    }

    snippet = QString("find_package(Qt%1 REQUIRED COMPONENTS LinguistTools)\n").arg(qtMajorVersion);
    return insertSnippetSilently(targetCmakeFile, {snippet, function->LineEnd() + 1, 0});
}

bool CMakeBuildSystem::addTsFiles(Node *context, const FilePaths &filePaths, FilePaths *notAdded)
{
    if (notAdded)
        notAdded->append(filePaths);

    if (auto n = dynamic_cast<CMakeTargetNode *>(context)) {
        const QString targetName = n->buildKey();
        const std::optional<Link> cmakeFile = cmakeFileForBuildKey(targetName, buildTargets());
        if (!cmakeFile.has_value())
            return false;

        const FilePath targetCMakeFile = cmakeFile->targetFilePath;
        std::optional<cmListFile> cmakeListFile = getUncachedCMakeListFile(targetCMakeFile);
        if (!cmakeListFile.has_value())
            return false;

        int lastArgumentPos = -1;
        std::optional<cmListFileFunction> function
                = handleQtAddTranslations(*cmakeListFile, targetName, &lastArgumentPos);
        if (!function.has_value())
            function = handleQtAddLupdate(*cmakeListFile, targetName, &lastArgumentPos);
        if (!function.has_value())
            function = handleQtCreateTranslation(*cmakeListFile, &lastArgumentPos);

        const QString filesToAdd = relativeFilePaths(filePaths, n->filePath().canonicalPath());
        bool linguistToolsMissing = false;
        int qtMajorVersion = -1;
        if (!function.has_value()) {
            if (auto qt = m_findPackagesFilesHash.value("Qt6Core"); qt.hasValidTarget())
                qtMajorVersion = 6;
            else if (auto qt = m_findPackagesFilesHash.value("Qt5Core"); qt.hasValidTarget())
                qtMajorVersion = 5;

            if (qtMajorVersion != -1) {
                const QString linguistTools = QString("Qt%1LinguistTools").arg(qtMajorVersion);
                auto linguist = m_findPackagesFilesHash.value(linguistTools);
                linguistToolsMissing = !linguist.hasValidTarget();
            }

            // we failed to find any pre-existing, add one ourself
            expected_str<bool> inserted = insertQtAddTranslations(*cmakeListFile,
                                                                  targetCMakeFile,
                                                                  targetName,
                                                                  cmakeFile->targetLine,
                                                                  filesToAdd,
                                                                  qtMajorVersion,
                                                                  linguistToolsMissing);
            if (!inserted)
                qCCritical(cmakeBuildSystemLog) << inserted.error();
            else if (notAdded)
                notAdded->removeIf([filePaths](const FilePath &p) { return filePaths.contains(p); });

            return inserted.value_or(false);
        }

        auto lastArgument = function->Arguments().at(lastArgumentPos);
        const int lastArgLength = static_cast<int>(lastArgument.Value.size()) - 1;
        SnippetAndLocation snippetLocation{QString("\n%1").arg(filesToAdd),
                                           lastArgument.Line, lastArgument.Column + lastArgLength};
        // Take into consideration the quotes
        if (lastArgument.Delim == cmListFileArgument::Quoted)
            snippetLocation.column += 2;

        expected_str<bool> inserted = insertSnippetSilently(targetCMakeFile, snippetLocation);
        if (!inserted) {
            qCCritical(cmakeBuildSystemLog) << inserted.error();
            return false;
        }

        if (notAdded)
            notAdded->removeIf([filePaths](const FilePath &p) { return filePaths.contains(p); });
        return true;
    }
    return false;
}

static bool isGlobbingFunction(const cmListFile &cmakeListFile, const cmListFileFunction &func)
{
    // Check if the filename is part of globbing variable result
    const auto globFunctions = std::get<0>(
        Utils::partition(cmakeListFile.Functions, [](const auto &f) {
            return f.LowerCaseName() == "file" && f.Arguments().size() > 2
                   && (f.Arguments().front().Value == "GLOB"
                       || f.Arguments().front().Value == "GLOB_RECURSE");
        }));

    const auto globVariables = Utils::transform<QSet>(globFunctions, [](const auto &func) {
        return std::string("${") + func.Arguments()[1].Value + "}";
    });

    return Utils::anyOf(func.Arguments(), [globVariables](const auto &arg) {
        return globVariables.contains(arg.Value);
    });
}

bool CMakeBuildSystem::addSrcFiles(Node *context, const FilePaths &filePaths, FilePaths *notAdded)
{
    if (notAdded)
        notAdded->append(filePaths);

    if (auto n = dynamic_cast<CMakeTargetNode *>(context)) {
        const QString targetName = n->buildKey();
        const std::optional<Link> cmakeFile = cmakeFileForBuildKey(targetName, buildTargets());
        if (!cmakeFile)
            return false;

        const FilePath targetCMakeFile = cmakeFile->targetFilePath;
        const int targetDefinitionLine = cmakeFile->targetLine;

        std::optional<cmListFile> cmakeListFile = getUncachedCMakeListFile(targetCMakeFile);
        if (!cmakeListFile)
            return false;

        std::optional<cmListFileFunction> function
                = findFunction(*cmakeListFile, [targetDefinitionLine](const auto &func) {
            return func.Line() == targetDefinitionLine;
        });
        if (!function.has_value()) {
            qCCritical(cmakeBuildSystemLog) << "Function that defined the target" << targetName
                                            << "could not be found at" << targetDefinitionLine;
            return false;
        }

        const bool haveGlobbing = isGlobbingFunction(cmakeListFile.value(), function.value());
        n->setVisibleAfterAddFileAction(!haveGlobbing);
        if (haveGlobbing && settings(project()).autorunCMake()) {
            runCMake();
        } else {
            const std::string target_name = function->Arguments().front().Value;
            auto qtAddModule = [target_name](const auto &func) {
                return (func.LowerCaseName() == "qt_add_qml_module"
                        || func.LowerCaseName() == "qt6_add_qml_module")
                        && func.Arguments().front().Value == target_name;
            };
            // Special case: when qt_add_executable and qt_add_qml_module use the same target name
            // then qt_add_qml_module function should be used
            function = findFunction(*cmakeListFile, qtAddModule).value_or(*function);

            const QString newSourceFiles = newFilesForFunction(function->LowerCaseName(),
                                                               filePaths,
                                                               n->filePath().canonicalPath());

            const SnippetAndLocation snippetLocation = generateSnippetAndLocationForSources(
                        newSourceFiles, *cmakeListFile, *function, targetName);
            expected_str<bool> inserted = insertSnippetSilently(targetCMakeFile, snippetLocation);
            if (!inserted) {
                qCCritical(cmakeBuildSystemLog) << inserted.error();
                return false;
            }
        }

        if (notAdded)
            notAdded->removeIf([filePaths](const FilePath &p) { return filePaths.contains(p); });
        return true;
    }

    return false;
}

bool CMakeBuildSystem::addFiles(Node *context, const FilePaths &filePaths, FilePaths *notAdded)
{
    FilePaths tsFiles, srcFiles;
    std::tie(tsFiles, srcFiles) = Utils::partition(filePaths, [](const FilePath &fp) {
        return Utils::mimeTypeForFile(fp.toString()).name() == Utils::Constants::LINGUIST_MIMETYPE;
    });
    bool success = true;
    if (!srcFiles.isEmpty())
        success = addSrcFiles(context, srcFiles, notAdded);

    if (!tsFiles.isEmpty())
        success = addTsFiles(context, tsFiles, notAdded) || success;

    if (success)
        return true;
    return BuildSystem::addFiles(context, filePaths, notAdded);
}

std::optional<CMakeBuildSystem::ProjectFileArgumentPosition>
CMakeBuildSystem::projectFileArgumentPosition(const QString &targetName, const QString &fileName)
{
    const std::optional<Link> cmakeFile = cmakeFileForBuildKey(targetName, buildTargets());
    if (!cmakeFile)
        return std::nullopt;

    const FilePath targetCMakeFile = cmakeFile->targetFilePath;
    const int targetDefinitionLine = cmakeFile->targetLine;

    std::optional<cmListFile> cmakeListFile = getUncachedCMakeListFile(targetCMakeFile);
    if (!cmakeListFile)
        return std::nullopt;

    std::optional<cmListFileFunction> function
            = findFunction(*cmakeListFile, [targetDefinitionLine](const auto &func) {
        return func.Line() == targetDefinitionLine;
    });
    if (!function.has_value()) {
        qCCritical(cmakeBuildSystemLog) << "Function that defined the target" << targetName
                                        << "could not be found at" << targetDefinitionLine;
        return std::nullopt;
    }

    const std::string target_name = targetName.toStdString();
    auto targetSourcesFunc = findFunction(*cmakeListFile, [target_name](const auto &func) {
        return func.LowerCaseName() == "target_sources" && func.Arguments().size() > 1
               && func.Arguments().front().Value == target_name;
    });

    auto addQmlModuleFunc = findFunction(*cmakeListFile, [target_name](const auto &func) {
        return (func.LowerCaseName() == "qt_add_qml_module"
                || func.LowerCaseName() == "qt6_add_qml_module")
               && func.Arguments().size() > 1 && func.Arguments().front().Value == target_name;
    });

    auto setSourceFilePropFunc = findFunction(*cmakeListFile, [](const auto &func) {
        return func.LowerCaseName() == "set_source_files_properties";
    });

    for (const auto &func : {function, targetSourcesFunc, addQmlModuleFunc, setSourceFilePropFunc}) {
        if (!func.has_value())
            continue;
        auto filePathArgument = Utils::findOrDefault(
            func->Arguments(), [file_name = fileName.toStdString()](const auto &arg) {
                return arg.Value == file_name;
            });

        if (!filePathArgument.Value.empty()) {
            return ProjectFileArgumentPosition{filePathArgument, targetCMakeFile, fileName};
        } else {
            // Check if the filename is part of globbing variable result
            const auto haveGlobbing = isGlobbingFunction(cmakeListFile.value(), func.value());
            if (haveGlobbing) {
                return ProjectFileArgumentPosition{filePathArgument,
                                                   targetCMakeFile,
                                                   fileName,
                                                   true};
            }

            // Check if the filename is part of a variable set by the user
            const auto setFunctions = std::get<0>(
                Utils::partition(cmakeListFile->Functions, [](const auto &f) {
                    return f.LowerCaseName() == "set" && f.Arguments().size() > 1;
                }));

            for (const auto &arg : func->Arguments()) {
                auto matchedFunctions = Utils::filtered(setFunctions, [arg](const auto &f) {
                    return arg.Value == std::string("${") + f.Arguments()[0].Value + "}";
                });

                for (const auto &f : matchedFunctions) {
                    filePathArgument = Utils::findOrDefault(f.Arguments(),
                                                            [file_name = fileName.toStdString()](
                                                                const auto &arg) {
                                                                return arg.Value == file_name;
                                                            });

                    if (!filePathArgument.Value.empty()) {
                        return ProjectFileArgumentPosition{filePathArgument,
                                                           targetCMakeFile,
                                                           fileName};
                    }
                }
            }
        }
    }

    return std::nullopt;
}

RemovedFilesFromProject CMakeBuildSystem::removeFiles(Node *context,
                                                      const FilePaths &filePaths,
                                                      FilePaths *notRemoved)
{
    FilePaths badFiles;
    if (auto n = dynamic_cast<CMakeTargetNode *>(context)) {
        const FilePath projDir = n->filePath().canonicalPath();
        const QString targetName = n->buildKey();

        bool haveGlobbing = false;
        for (const auto &file : filePaths) {
            const QString fileName
                = file.canonicalPath().relativePathFrom(projDir).cleanPath().toString();

            auto filePos = projectFileArgumentPosition(targetName, fileName);
            if (filePos) {
                if (!filePos.value().cmakeFile.exists()) {
                    badFiles << file;

                    qCCritical(cmakeBuildSystemLog).noquote()
                        << "File" << filePos.value().cmakeFile.path() << "does not exist.";
                    continue;
                }

                if (filePos.value().fromGlobbing) {
                    haveGlobbing = true;
                    continue;
                }

                BaseTextEditor *editor = qobject_cast<BaseTextEditor *>(
                    Core::EditorManager::openEditorAt(
                        {filePos.value().cmakeFile,
                         static_cast<int>(filePos.value().argumentPosition.Line),
                         static_cast<int>(filePos.value().argumentPosition.Column - 1)},
                        Constants::CMAKE_EDITOR_ID,
                        Core::EditorManager::DoNotMakeVisible
                            | Core::EditorManager::DoNotChangeCurrentEditor));
                if (!editor) {
                    badFiles << file;

                    qCCritical(cmakeBuildSystemLog).noquote()
                        << "BaseTextEditor cannot be obtained for"
                        << filePos.value().cmakeFile.path() << filePos.value().argumentPosition.Line
                        << int(filePos.value().argumentPosition.Column - 1);
                    continue;
                }

                // If quotes were used for the source file, remove the quotes too
                int extraChars = 0;
                if (filePos->argumentPosition.Delim == cmListFileArgument::Quoted)
                    extraChars = 2;

                editor->replace(filePos.value().relativeFileName.length() + extraChars, "");

                editor->editorWidget()->autoIndent();
                if (!Core::DocumentManager::saveDocument(editor->document())) {
                    badFiles << file;

                    qCCritical(cmakeBuildSystemLog).noquote()
                        << "Changes to" << filePos.value().cmakeFile.path()
                        << "could not be saved.";
                    continue;
                }
            } else {
                badFiles << file;
            }
        }

        if (notRemoved && !badFiles.isEmpty())
            *notRemoved = badFiles;

        if (haveGlobbing && settings(project()).autorunCMake())
            runCMake();

        return badFiles.isEmpty() ? RemovedFilesFromProject::Ok : RemovedFilesFromProject::Error;
    }

    return RemovedFilesFromProject::Error;
}

bool CMakeBuildSystem::canRenameFile(Node *context,
                                     const FilePath &oldFilePath,
                                     const FilePath &newFilePath)
{
    // "canRenameFile" will cause an actual rename after the function call.
    // This will make the a sequence like
    //    canonicalPath().relativePathFrom(projDir).cleanPath().toString()
    // to fail if the file doesn't exist on disk
    // therefore cache the results for the subsequent "renameFile" call
    // where oldFilePath has already been renamed as newFilePath.

    if (auto n = dynamic_cast<CMakeTargetNode *>(context)) {
        const FilePath projDir = n->filePath().canonicalPath();
        const QString oldRelPathName
            = oldFilePath.canonicalPath().relativePathFrom(projDir).cleanPath().toString();

        const QString targetName = n->buildKey();

        const QString key
            = QStringList{projDir.path(), targetName, oldFilePath.path(), newFilePath.path()}
                  .join(";");

        auto filePos = projectFileArgumentPosition(targetName, oldRelPathName);
        if (!filePos)
            return false;

        m_filesToBeRenamed.insert(key, filePos.value());
        return true;
    }
    return false;
}

bool CMakeBuildSystem::renameFile(Node *context,
                                  const FilePath &oldFilePath,
                                  const FilePath &newFilePath)
{
    if (auto n = dynamic_cast<CMakeTargetNode *>(context)) {
        const FilePath projDir = n->filePath().canonicalPath();
        const FilePath newRelPath = newFilePath.canonicalPath().relativePathFrom(projDir).cleanPath();
        const QString newRelPathName = newRelPath.toString();

        const QString targetName = n->buildKey();
        const QString key
            = QStringList{projDir.path(), targetName, oldFilePath.path(), newFilePath.path()}.join(
                ";");

        std::optional<CMakeBuildSystem::ProjectFileArgumentPosition> fileToRename
            = m_filesToBeRenamed.take(key);
        if (!fileToRename->cmakeFile.exists()) {
            qCCritical(cmakeBuildSystemLog).noquote()
                << "File" << fileToRename->cmakeFile.path() << "does not exist.";
            return false;
        }

        bool haveGlobbing = false;
        do {
            if (!fileToRename->fromGlobbing) {
                BaseTextEditor *editor = qobject_cast<BaseTextEditor *>(
                    Core::EditorManager::openEditorAt(
                        {fileToRename->cmakeFile,
                         static_cast<int>(fileToRename->argumentPosition.Line),
                         static_cast<int>(fileToRename->argumentPosition.Column - 1)},
                        Constants::CMAKE_EDITOR_ID,
                        Core::EditorManager::DoNotMakeVisible
                            | Core::EditorManager::DoNotChangeCurrentEditor));
                if (!editor) {
                    qCCritical(cmakeBuildSystemLog).noquote()
                        << "BaseTextEditor cannot be obtained for" << fileToRename->cmakeFile.path()
                        << fileToRename->argumentPosition.Line
                        << int(fileToRename->argumentPosition.Column);
                    return false;
                }

                // If quotes were used for the source file, skip the starting quote
                if (fileToRename->argumentPosition.Delim == cmListFileArgument::Quoted)
                    editor->setCursorPosition(editor->position() + 1);

                editor->replace(fileToRename->relativeFileName.length(), newRelPathName);

                editor->editorWidget()->autoIndent();
                if (!Core::DocumentManager::saveDocument(editor->document())) {
                    qCCritical(cmakeBuildSystemLog).noquote()
                        << "Changes to" << fileToRename->cmakeFile.path() << "could not be saved.";
                    return false;
                }
            } else {
                haveGlobbing = true;
            }

            // Try the next occurrence. This can happen if set_source_file_properties is used
            fileToRename = projectFileArgumentPosition(targetName, fileToRename->relativeFileName);
        } while (fileToRename && !fileToRename->fromGlobbing);

        if (haveGlobbing && settings(project()).autorunCMake())
            runCMake();

        return true;
    }

    return false;
}

FilePaths CMakeBuildSystem::filesGeneratedFrom(const FilePath &sourceFile) const
{
    FilePath project = projectDirectory();
    FilePath baseDirectory = sourceFile.parentDir();

    while (baseDirectory.isChildOf(project)) {
        const FilePath cmakeListsTxt = baseDirectory.pathAppended(Constants::CMAKE_LISTS_TXT);
        if (cmakeListsTxt.exists())
            break;
        baseDirectory = baseDirectory.parentDir();
    }

    const FilePath relativePath = baseDirectory.relativePathFrom(project);
    FilePath generatedFilePath = buildConfiguration()->buildDirectory().resolvePath(relativePath);

    if (sourceFile.suffix() == "ui") {
        const QString generatedFileName = "ui_" + sourceFile.completeBaseName() + ".h";

        auto targetNode = this->project()->nodeForFilePath(sourceFile);
        while (targetNode && !dynamic_cast<const CMakeTargetNode *>(targetNode))
            targetNode = targetNode->parentFolderNode();

        FilePaths generatedFilePaths;
        if (targetNode) {
            const QString autogenSignature = targetNode->buildKey() + "_autogen/include";

            // If AUTOUIC reports the generated header file name, use that path
            generatedFilePaths = this->project()->files(
                [autogenSignature, generatedFileName](const Node *n) {
                    const FilePath filePath = n->filePath();
                    if (!filePath.contains(autogenSignature))
                        return false;

                    return Project::GeneratedFiles(n) && filePath.endsWith(generatedFileName);
                });
        }

        if (generatedFilePaths.empty())
            generatedFilePaths = {generatedFilePath.pathAppended(generatedFileName)};

        return generatedFilePaths;
    }
    if (sourceFile.suffix() == "scxml") {
        generatedFilePath = generatedFilePath.pathAppended(sourceFile.completeBaseName());
        return {generatedFilePath.stringAppended(".h"), generatedFilePath.stringAppended(".cpp")};
    }

    // TODO: Other types will be added when adapters for their compilers become available.
    return {};
}

QString CMakeBuildSystem::reparseParametersString(int reparseFlags)
{
    QString result;
    if (reparseFlags == REPARSE_DEFAULT) {
        result = "<NONE>";
    } else {
        if (reparseFlags & REPARSE_URGENT)
            result += " URGENT";
        if (reparseFlags & REPARSE_FORCE_CMAKE_RUN)
            result += " FORCE_CMAKE_RUN";
        if (reparseFlags & REPARSE_FORCE_INITIAL_CONFIGURATION)
            result += " FORCE_CONFIG";
    }
    return result.trimmed();
}

void CMakeBuildSystem::reparse(int reparseParameters)
{
    setParametersAndRequestParse(BuildDirParameters(this), reparseParameters);
}

void CMakeBuildSystem::setParametersAndRequestParse(const BuildDirParameters &parameters,
                                                    const int reparseParameters)
{
    project()->clearIssues();

    qCDebug(cmakeBuildSystemLog) << buildConfiguration()->displayName()
                                 << "setting parameters and requesting reparse"
                                 << reparseParametersString(reparseParameters);

    if (!buildConfiguration()->isActive()) {
        qCDebug(cmakeBuildSystemLog) << "setting parameters and requesting reparse: SKIPPING since BC is not active -- clearing state.";
        stopParsingAndClearState();
        return; // ignore request, this build configuration is not active!
    }

    const CMakeTool *tool = parameters.cmakeTool();
    if (!tool || !tool->isValid()) {
        TaskHub::addTask(
            BuildSystemTask(Task::Error,
                            Tr::tr("The kit needs to define a CMake tool to parse this project.")));
        return;
    }
    if (!tool->hasFileApi()) {
        TaskHub::addTask(
            BuildSystemTask(Task::Error,
                            CMakeKitAspect::msgUnsupportedVersion(tool->version().fullVersion)));
        return;
    }
    QTC_ASSERT(parameters.isValid(), return );

    m_parameters = parameters;
    ensureBuildDirectory(parameters);
    updateReparseParameters(reparseParameters);

    m_reader.setParameters(m_parameters);

    if (reparseParameters & REPARSE_URGENT) {
        qCDebug(cmakeBuildSystemLog) << "calling requestReparse";
        requestParse();
    } else {
        qCDebug(cmakeBuildSystemLog) << "calling requestDelayedReparse";
        requestDelayedParse();
    }
}

bool CMakeBuildSystem::mustApplyConfigurationChangesArguments(const BuildDirParameters &parameters) const
{
    if (parameters.configurationChangesArguments.isEmpty())
        return false;

    int answer = QMessageBox::question(Core::ICore::dialogParent(),
                                       Tr::tr("Apply configuration changes?"),
                                       "<p>" + Tr::tr("Run CMake with configuration changes?")
                                       + "</p><pre>"
                                       + parameters.configurationChangesArguments.join("\n")
                                       + "</pre>",
                                       QMessageBox::Apply | QMessageBox::Discard,
                                       QMessageBox::Apply);
    return answer == QMessageBox::Apply;
}

void CMakeBuildSystem::runCMake()
{
    qCDebug(cmakeBuildSystemLog) << "Requesting parse due \"Run CMake\" command";
    reparse(REPARSE_FORCE_CMAKE_RUN | REPARSE_URGENT);
}

void CMakeBuildSystem::runCMakeAndScanProjectTree()
{
    qCDebug(cmakeBuildSystemLog) << "Requesting parse due to \"Rescan Project\" command";
    reparse(REPARSE_FORCE_CMAKE_RUN | REPARSE_URGENT);
}

void CMakeBuildSystem::runCMakeWithExtraArguments()
{
    qCDebug(cmakeBuildSystemLog) << "Requesting parse due to \"Rescan Project\" command";
    reparse(REPARSE_FORCE_CMAKE_RUN | REPARSE_FORCE_EXTRA_CONFIGURATION | REPARSE_URGENT);
}

void CMakeBuildSystem::runCMakeWithProfiling()
{
    qCDebug(cmakeBuildSystemLog) << "Requesting parse due \"CMake Profiler\" command";
    reparse(REPARSE_FORCE_CMAKE_RUN | REPARSE_URGENT | REPARSE_FORCE_EXTRA_CONFIGURATION
            | REPARSE_PROFILING);
}

void CMakeBuildSystem::stopCMakeRun()
{
    qCDebug(cmakeBuildSystemLog) << buildConfiguration()->displayName()
                                 << "stopping CMake's run";
    m_reader.stopCMakeRun();
}

void CMakeBuildSystem::buildCMakeTarget(const QString &buildTarget)
{
    QTC_ASSERT(!buildTarget.isEmpty(), return);
    if (ProjectExplorerPlugin::saveModifiedFiles())
        cmakeBuildConfiguration()->buildTarget(buildTarget);
}

bool CMakeBuildSystem::persistCMakeState()
{
    BuildDirParameters parameters(this);
    QTC_ASSERT(parameters.isValid(), return false);

    const bool hadBuildDirectory = parameters.buildDirectory.exists();
    ensureBuildDirectory(parameters);

    int reparseFlags = REPARSE_DEFAULT;
    qCDebug(cmakeBuildSystemLog) << "Checking whether build system needs to be persisted:"
                                 << "buildDir:" << parameters.buildDirectory
                                 << "Has extraargs:" << !parameters.configurationChangesArguments.isEmpty();

    if (mustApplyConfigurationChangesArguments(parameters)) {
        reparseFlags = REPARSE_FORCE_EXTRA_CONFIGURATION;
        qCDebug(cmakeBuildSystemLog) << "   -> must run CMake with extra arguments.";
    }
    if (!hadBuildDirectory) {
        reparseFlags = REPARSE_FORCE_INITIAL_CONFIGURATION;
        qCDebug(cmakeBuildSystemLog) << "   -> must run CMake with initial arguments.";
    }

    if (reparseFlags == REPARSE_DEFAULT)
        return false;

    qCDebug(cmakeBuildSystemLog) << "Requesting parse to persist CMake State";
    setParametersAndRequestParse(parameters,
                                 REPARSE_URGENT | REPARSE_FORCE_CMAKE_RUN | reparseFlags);
    return true;
}

void CMakeBuildSystem::clearCMakeCache()
{
    QTC_ASSERT(m_parameters.isValid(), return );
    QTC_ASSERT(!m_isHandlingError, return );

    stopParsingAndClearState();

    const FilePath pathsToDelete[] = {
        m_parameters.buildDirectory / Constants::CMAKE_CACHE_TXT,
        m_parameters.buildDirectory / Constants::CMAKE_CACHE_TXT_PREV,
        m_parameters.buildDirectory / "CMakeFiles",
        m_parameters.buildDirectory / ".cmake/api/v1/reply",
        m_parameters.buildDirectory / ".cmake/api/v1/reply.prev",
        m_parameters.buildDirectory / Constants::PACKAGE_MANAGER_DIR,
        m_parameters.buildDirectory / "conan-dependencies",
        m_parameters.buildDirectory / "vcpkg-dependencies"
    };

    for (const FilePath &path : pathsToDelete)
        path.removeRecursively();

    emit configurationCleared();
}

void CMakeBuildSystem::combineScanAndParse(bool restoredFromBackup)
{
    if (buildConfiguration()->isActive()) {
        if (m_waitingForParse)
            return;

        if (m_combinedScanAndParseResult) {
            updateProjectData();
            m_currentGuard.markAsSuccess();

            if (restoredFromBackup)
                project()->addIssue(
                    CMakeProject::IssueType::Warning,
                    Tr::tr("<b>CMake configuration failed<b>"
                           "<p>The backup of the previous configuration has been restored.</p>"
                           "<p>Issues and \"Projects > Build\" settings "
                           "show more information about the failure.</p>"));

            m_reader.resetData();

            m_currentGuard = {};
            m_testNames.clear();

            emitBuildSystemUpdated();

            runCTest();
        } else {
            updateFallbackProjectData();

            project()->addIssue(CMakeProject::IssueType::Warning,
                                Tr::tr("<b>Failed to load project<b>"
                                       "<p>Issues and \"Projects > Build\" settings "
                                       "show more information about the failure.</p>"));
        }
    }
}

void CMakeBuildSystem::checkAndReportError(QString &errorMessage)
{
    if (!errorMessage.isEmpty()) {
        setError(errorMessage);
        errorMessage.clear();
    }
}

static QSet<FilePath> projectFilesToWatch(const QSet<CMakeFileInfo> &cmakeFiles)
{
    return Utils::transform(Utils::filtered(cmakeFiles,
                                            [](const CMakeFileInfo &info) {
                                                return !info.isGenerated;
                                            }),
                            [](const CMakeFileInfo &info) { return info.path; });
}

void CMakeBuildSystem::updateProjectData()
{
    qCDebug(cmakeBuildSystemLog) << "Updating CMake project data";

    QTC_ASSERT(m_treeScanner.isFinished() && !m_reader.isParsing(), return );

    buildConfiguration()->project()->setExtraProjectFiles(projectFilesToWatch(m_cmakeFiles));

    CMakeConfig patchedConfig = configurationFromCMake();
    {
        QSet<QString> res;
        QStringList apps;
        for (const auto &target : std::as_const(m_buildTargets)) {
            if (target.targetType == DynamicLibraryType) {
                res.insert(target.executable.parentDir().toString());
                apps.push_back(target.executable.toUserOutput());
            }
            // ### shall we add also the ExecutableType ?
        }
        {
            CMakeConfigItem paths;
            // paths.key = Android::Constants::ANDROID_SO_LIBS_PATHS;
            paths.values = Utils::toList(res);
            patchedConfig.append(paths);
        }

        apps.sort();
        {
            CMakeConfigItem appsPaths;
            appsPaths.key = "TARGETS_BUILD_PATH";
            appsPaths.values = apps;
            patchedConfig.append(appsPaths);
        }
    }

    Project *p = project();
    {
        auto newRoot = m_reader.rootProjectNode();
        if (newRoot) {
            setRootProjectNode(std::move(newRoot));

            if (QTC_GUARD(p->rootProjectNode())) {
                const QString nodeName = p->rootProjectNode()->displayName();
                p->setDisplayName(nodeName);

                // set config on target nodes
                const QSet<QString> buildKeys = Utils::transform<QSet>(m_buildTargets,
                                                                       &CMakeBuildTarget::title);
                p->rootProjectNode()->forEachProjectNode(
                    [patchedConfig, buildKeys](const ProjectNode *node) {
                        if (buildKeys.contains(node->buildKey())) {
                            auto targetNode = const_cast<CMakeTargetNode *>(
                                dynamic_cast<const CMakeTargetNode *>(node));
                            if (QTC_GUARD(targetNode))
                                targetNode->setConfig(patchedConfig);
                        }
                    });
            }
        }
    }

    {
        qDeleteAll(m_extraCompilers);
        m_extraCompilers = findExtraCompilers();
        qCDebug(cmakeBuildSystemLog) << "Extra compilers created.";
    }

    QtSupport::CppKitInfo kitInfo(kit());
    QTC_ASSERT(kitInfo.isValid(), return );

    struct QtMajorToPkgNames
    {
        QtMajorVersion major = QtMajorVersion::None;
        QStringList pkgNames;
    };

    auto qtVersionFromCMake = [this](const QList<QtMajorToPkgNames> &mapping) {
        for (const QtMajorToPkgNames &m : mapping) {
            for (const QString &pkgName : m.pkgNames) {
                auto qt = m_findPackagesFilesHash.value(pkgName);
                if (qt.hasValidTarget())
                    return m.major;
            }
        }
        return QtMajorVersion::None;
    };

    QtMajorVersion qtVersion = qtVersionFromCMake(
        {{QtMajorVersion::Qt6, {"Qt6", "Qt6Core"}},
         {QtMajorVersion::Qt5, {"Qt5", "Qt5Core"}},
         {QtMajorVersion::Qt4, {"Qt4", "Qt4Core"}}});

    QString errorMessage;
    RawProjectParts rpps = m_reader.createRawProjectParts(errorMessage);
    if (!errorMessage.isEmpty())
        setError(errorMessage);
    qCDebug(cmakeBuildSystemLog) << "Raw project parts created." << errorMessage;

    for (RawProjectPart &rpp : rpps) {
        rpp.setQtVersion(qtVersion);
        const FilePath includeFileBaseDir = buildConfiguration()->buildDirectory();
        QStringList cxxFlags = rpp.flagsForCxx.commandLineFlags;
        QStringList cFlags = rpp.flagsForC.commandLineFlags;
        addTargetFlagForIos(cFlags, cxxFlags, this, [this] {
            return m_configurationFromCMake.stringValueOf("CMAKE_OSX_DEPLOYMENT_TARGET");
        });
        if (kitInfo.cxxToolchain)
            rpp.setFlagsForCxx({kitInfo.cxxToolchain, cxxFlags, includeFileBaseDir});
        if (kitInfo.cToolchain)
            rpp.setFlagsForC({kitInfo.cToolchain, cFlags, includeFileBaseDir});
    }

    m_cppCodeModelUpdater->update({p, kitInfo, buildConfiguration()->environment(), rpps},
                                  m_extraCompilers);

    {
        const bool mergedHeaderPathsAndQmlImportPaths = kit()->value(
                    QtSupport::Constants::KIT_HAS_MERGED_HEADER_PATHS_WITH_QML_IMPORT_PATHS, false).toBool();
        QStringList extraHeaderPaths;
        QList<QByteArray> moduleMappings;
        for (const RawProjectPart &rpp : std::as_const(rpps)) {
            FilePath moduleMapFile = buildConfiguration()->buildDirectory()
                    .pathAppended("qml_module_mappings/" + rpp.buildSystemTarget);
            if (expected_str<QByteArray> content = moduleMapFile.fileContents()) {
                auto lines = content->split('\n');
                for (const QByteArray &line : lines) {
                    if (!line.isEmpty())
                        moduleMappings.append(line.simplified());
                }
            }

            if (mergedHeaderPathsAndQmlImportPaths) {
                for (const auto &headerPath : rpp.headerPaths) {
                    if (headerPath.type == HeaderPathType::User || headerPath.type == HeaderPathType::System)
                        extraHeaderPaths.append(headerPath.path);
                }
            }
        }
        updateQmlJSCodeModel(extraHeaderPaths, moduleMappings);
    }
    updateInitialCMakeExpandableVars();

    emit buildConfiguration()->buildTypeChanged();

    qCDebug(cmakeBuildSystemLog) << "All CMake project data up to date.";
}

void CMakeBuildSystem::handleTreeScanningFinished()
{
    TreeScanner::Result result = m_treeScanner.release();
    m_allFiles = result.folderNode;
    qDeleteAll(result.allFiles);

    updateFileSystemNodes();
}

void CMakeBuildSystem::updateFileSystemNodes()
{
    auto newRoot = std::make_unique<CMakeProjectNode>(m_parameters.sourceDirectory);
    newRoot->setDisplayName(m_parameters.sourceDirectory.fileName());

    if (!m_reader.topCmakeFile().isEmpty()) {
        auto node = std::make_unique<FileNode>(m_reader.topCmakeFile(), FileType::Project);
        node->setIsGenerated(false);

        std::vector<std::unique_ptr<FileNode>> fileNodes;
        fileNodes.emplace_back(std::move(node));

        addCMakeLists(newRoot.get(), std::move(fileNodes));
    }

    if (m_allFiles)
        addFileSystemNodes(newRoot.get(), m_allFiles);
    setRootProjectNode(std::move(newRoot));

    m_reader.resetData();

    m_currentGuard = {};
    emitBuildSystemUpdated();

    qCDebug(cmakeBuildSystemLog) << "All fallback CMake project data up to date.";
}

void CMakeBuildSystem::updateFallbackProjectData()
{
    qCDebug(cmakeBuildSystemLog) << "Updating fallback CMake project data";
    qCDebug(cmakeBuildSystemLog) << "Starting TreeScanner";
    QTC_CHECK(m_treeScanner.isFinished());
    if (m_treeScanner.asyncScanForFiles(projectDirectory()))
        Core::ProgressManager::addTask(m_treeScanner.future(),
                                       Tr::tr("Scan \"%1\" project tree")
                                           .arg(project()->displayName()),
                                       "CMake.Scan.Tree");

    // A failed configuration could be the result of an compiler update
    // which then would cause CMake to fail. Make sure to offer an upgrade path
    // to the new Kit compiler values.
    updateInitialCMakeExpandableVars();
}

void CMakeBuildSystem::updateCMakeConfiguration(QString &errorMessage)
{
    CMakeConfig cmakeConfig = m_reader.takeParsedConfiguration(errorMessage);
    for (auto &ci : cmakeConfig)
        ci.inCMakeCache = true;
    if (!errorMessage.isEmpty()) {
        const CMakeConfig changes = configurationChanges();
        for (const auto &ci : changes) {
            if (ci.isInitial)
                continue;
            const bool haveConfigItem = Utils::contains(cmakeConfig, [ci](const CMakeConfigItem& i) {
                return i.key == ci.key;
            });
            if (!haveConfigItem)
                cmakeConfig.append(ci);
        }
    }

    const bool hasAndroidTargetBuildDirSupport
        = CMakeConfigItem::toBool(
              cmakeConfig.stringValueOf("QT_INTERNAL_ANDROID_TARGET_BUILD_DIR_SUPPORT"))
              .value_or(false);

    const bool useAndroidTargetBuildDir
        = CMakeConfigItem::toBool(cmakeConfig.stringValueOf("QT_USE_TARGET_ANDROID_BUILD_DIR"))
              .value_or(false);

    // project()->setExtraData(Android::Constants::AndroidBuildTargetDirSupport,
    //                         QVariant::fromValue(hasAndroidTargetBuildDirSupport));
    // project()->setExtraData(Android::Constants::UseAndroidBuildTargetDir,
    //                         QVariant::fromValue(useAndroidTargetBuildDir));

    QVariantList packageTargets;
    for (const CMakeBuildTarget &buildTarget : buildTargets()) {
        bool isBuiltinPackage = false;
        bool isInstallablePackage = false;
        for (const ProjectExplorer::FolderNode::LocationInfo &bs : buildTarget.backtrace) {
            if (bs.displayName == "qt6_am_create_builtin_package")
                isBuiltinPackage = true;
            else if (bs.displayName == "qt6_am_create_installable_package")
                isInstallablePackage = true;
        }

        if (!isBuiltinPackage && !isInstallablePackage)
            continue;

        QVariantMap packageTarget;
        for (const FilePath &sourceFile : buildTarget.sourceFiles) {
            if (sourceFile.fileName() == "info.yaml") {
                packageTarget.insert("manifestFilePath", QVariant::fromValue(sourceFile.absoluteFilePath()));
                packageTarget.insert("cmakeTarget", buildTarget.title);
                packageTarget.insert("isBuiltinPackage", isBuiltinPackage);
                for (const FilePath &osf : buildTarget.sourceFiles) {
                    if (osf.fileName().endsWith(".ampkg.rule")) {
                        packageTarget.insert("packageFilePath", QVariant::fromValue(osf.absoluteFilePath().chopped(5)));
                    }
                }
            }
        }
        packageTargets.append(packageTarget);
    }
    // project()->setExtraData(AppManager::Constants::APPMAN_PACKAGE_TARGETS, packageTargets);

    setConfigurationFromCMake(cmakeConfig);
}

void CMakeBuildSystem::handleParsingSucceeded(bool restoredFromBackup)
{
    if (!buildConfiguration()->isActive()) {
        stopParsingAndClearState();
        return;
    }

    clearError();

    QString errorMessage;
    {
        m_buildTargets = Utils::transform(CMakeBuildStep::specialTargets(m_reader.usesAllCapsTargets()), [this](const QString &t) {
            CMakeBuildTarget result;
            result.title = t;
            result.workingDirectory = m_parameters.buildDirectory;
            result.sourceDirectory = m_parameters.sourceDirectory;
            return result;
        });
        m_buildTargets += m_reader.takeBuildTargets(errorMessage);
        m_cmakeFiles = m_reader.takeCMakeFileInfos(errorMessage);
        setupCMakeSymbolsHash();

        checkAndReportError(errorMessage);
    }

    {
        updateCMakeConfiguration(errorMessage);
        checkAndReportError(errorMessage);
    }

    if (const CMakeTool *tool = m_parameters.cmakeTool())
        m_ctestPath = tool->cmakeExecutable().withNewPath(m_reader.ctestPath());

    setApplicationTargets(appTargets());

    // Note: This is practically always wrong and resulting in an empty view.
    // Setting the real data is triggered from a successful run of a
    // MakeInstallStep.
    setDeploymentData(deploymentDataFromFile());

    QTC_ASSERT(m_waitingForParse, return );
    m_waitingForParse = false;

    combineScanAndParse(restoredFromBackup);
}

void CMakeBuildSystem::handleParsingFailed(const QString &msg)
{
    setError(msg);

    QString errorMessage;
    updateCMakeConfiguration(errorMessage);
    // ignore errorMessage here, we already got one.

    m_ctestPath.clear();

    QTC_CHECK(m_waitingForParse);
    m_waitingForParse = false;
    m_combinedScanAndParseResult = false;

    combineScanAndParse(false);
}

void CMakeBuildSystem::wireUpConnections()
{
    // At this point the entire project will be fully configured, so let's connect everything and
    // trigger an initial parser run

    // Became active/inactive:
    connect(target(), &Target::activeBuildConfigurationChanged, this, [this] {
        // Build configuration has changed:
        qCDebug(cmakeBuildSystemLog) << "Requesting parse due to active BC changed";
        reparse(CMakeBuildSystem::REPARSE_DEFAULT);
    });
    connect(project(), &Project::activeTargetChanged, this, [this] {
        // Build configuration has changed:
        qCDebug(cmakeBuildSystemLog) << "Requesting parse due to active target changed";
        reparse(CMakeBuildSystem::REPARSE_DEFAULT);
    });

    // BuildConfiguration changed:
    connect(buildConfiguration(), &BuildConfiguration::environmentChanged, this, [this] {
        // The environment on our BC has changed, force CMake run to catch up with possible changes
        qCDebug(cmakeBuildSystemLog) << "Requesting parse due to environment change";
        reparse(CMakeBuildSystem::REPARSE_FORCE_CMAKE_RUN);
    });
    connect(buildConfiguration(), &BuildConfiguration::buildDirectoryChanged, this, [this] {
        // The build directory of our BC has changed:
        // Does the directory contain a CMakeCache ? Existing build, just parse
        // No CMakeCache? Run with initial arguments!
        qCDebug(cmakeBuildSystemLog) << "Requesting parse due to build directory change";
        const BuildDirParameters parameters(this);
        const FilePath cmakeCacheTxt = parameters.buildDirectory.pathAppended(
            Constants::CMAKE_CACHE_TXT);
        const bool hasCMakeCache = cmakeCacheTxt.exists();
        const auto options = ReparseParameters(
                    hasCMakeCache
                    ? REPARSE_DEFAULT
                    : (REPARSE_FORCE_INITIAL_CONFIGURATION | REPARSE_FORCE_CMAKE_RUN));
        if (hasCMakeCache) {
            QString errorMessage;
            const CMakeConfig config = CMakeConfig::fromFile(cmakeCacheTxt, &errorMessage);
            if (!config.isEmpty() && errorMessage.isEmpty()) {
                QString cmakeBuildTypeName = config.stringValueOf("CMAKE_BUILD_TYPE");
                cmakeBuildConfiguration()->setCMakeBuildType(cmakeBuildTypeName, true);
            }
        }
        reparse(options);
    });

    connect(project(), &Project::projectFileIsDirty, this, [this] {
        const bool isBuilding = BuildManager::isBuilding(project());
        if (buildConfiguration()->isActive() && !isParsing() && !isBuilding) {
            if (settings(project()).autorunCMake()) {
                qCDebug(cmakeBuildSystemLog) << "Requesting parse due to dirty project file";
                reparse(CMakeBuildSystem::REPARSE_FORCE_CMAKE_RUN);
            }
        }
    });

    // Force initial parsing run:
    if (buildConfiguration()->isActive()) {
        qCDebug(cmakeBuildSystemLog) << "Initial run:";
        reparse(CMakeBuildSystem::REPARSE_DEFAULT);
    }
}

void CMakeBuildSystem::setupCMakeSymbolsHash()
{
    m_cmakeSymbolsHash.clear();

    m_projectKeywords.functions.clear();
    m_projectKeywords.variables.clear();

    auto handleFunctionMacroOption = [&](const CMakeFileInfo &cmakeFile,
                                         const cmListFileFunction &func) {
        if (func.LowerCaseName() != "function" && func.LowerCaseName() != "macro"
            && func.LowerCaseName() != "option")
            return;

        if (func.Arguments().size() == 0)
            return;
        auto arg = func.Arguments()[0];

        Utils::Link link;
        link.targetFilePath = cmakeFile.path;
        link.targetLine = arg.Line;
        link.targetColumn = arg.Column - 1;
        m_cmakeSymbolsHash.insert(QString::fromUtf8(arg.Value), link);

        if (func.LowerCaseName() == "option")
            m_projectKeywords.variables[QString::fromUtf8(arg.Value)] = FilePath();
        else
            m_projectKeywords.functions[QString::fromUtf8(arg.Value)] = FilePath();
    };

    m_projectImportedTargets.clear();
    auto handleImportedTargets = [&](const CMakeFileInfo &cmakeFile,
                                     const cmListFileFunction &func) {
        if (func.LowerCaseName() != "add_library")
            return;

        if (func.Arguments().size() == 0)
            return;
        auto arg = func.Arguments()[0];
        const QString targetName = QString::fromUtf8(arg.Value);

        const bool haveImported = Utils::contains(func.Arguments(), [](const auto &arg) {
            return arg.Value == "IMPORTED";
        });
        if (haveImported && !targetName.contains("${")) {
            m_projectImportedTargets << targetName;

            // Allow navigation to the imported target
            Utils::Link link;
            link.targetFilePath = cmakeFile.path;
            link.targetLine = arg.Line;
            link.targetColumn = arg.Column - 1;
            m_cmakeSymbolsHash.insert(targetName, link);
        }
    };

    // Handle project targets, unfortunately the CMake file-api doesn't deliver the
    // column of the target, just the line. Make sure to find it out
    QHash<FilePath, QPair<int, QString>> projectTargetsSourceAndLine;
    for (const auto &target : std::as_const(buildTargets())) {
        if (target.targetType == TargetType::UtilityType)
            continue;
        if (target.backtrace.isEmpty())
            continue;

        projectTargetsSourceAndLine.insert(target.backtrace.last().path,
                                           {target.backtrace.last().line, target.title});
    }
    auto handleProjectTargets = [&](const CMakeFileInfo &cmakeFile, const cmListFileFunction &func) {
        const auto it = projectTargetsSourceAndLine.find(cmakeFile.path);
        if (it == projectTargetsSourceAndLine.end() || it->first != func.Line())
            return;

        if (func.Arguments().size() == 0)
            return;
        auto arg = func.Arguments()[0];

        Utils::Link link;
        link.targetFilePath = cmakeFile.path;
        link.targetLine = arg.Line;
        link.targetColumn = arg.Column - 1;
        m_cmakeSymbolsHash.insert(it->second, link);
    };

    // Gather the exported variables for the Find<Package> CMake packages
    m_projectFindPackageVariables.clear();

    const std::string fphsFunctionName = "find_package_handle_standard_args";
    CMakeKeywords keywords;
    if (auto tool = CMakeKitAspect::cmakeTool(target()->kit()))
        keywords = tool->keywords();
    QSet<std::string> fphsFunctionArgs;
    if (keywords.functionArgs.contains(QString::fromStdString(fphsFunctionName))) {
        const QList<std::string> args
            = Utils::transform(keywords.functionArgs.value(QString::fromStdString(fphsFunctionName)),
                               &QString::toStdString);
        fphsFunctionArgs = Utils::toSet(args);
    }

    auto handleFindPackageVariables = [&](const CMakeFileInfo &cmakeFile, const cmListFileFunction &func) {
        if (func.LowerCaseName() != fphsFunctionName)
            return;

        if (func.Arguments().size() == 0)
            return;
        auto firstArgument = func.Arguments()[0];
        const auto filteredArguments = Utils::filtered(func.Arguments(), [&](const auto &arg) {
            return !fphsFunctionArgs.contains(arg.Value) && arg != firstArgument;
        });

        for (const auto &arg : filteredArguments) {
            const QString value = QString::fromUtf8(arg.Value);
            if (value.contains("${") || (value.startsWith('"') && value.endsWith('"'))
                || (value.startsWith("'") && value.endsWith("'")))
                continue;

            m_projectFindPackageVariables << value;

            Utils::Link link;
            link.targetFilePath = cmakeFile.path;
            link.targetLine = arg.Line;
            link.targetColumn = arg.Column - 1;
            m_cmakeSymbolsHash.insert(value, link);
        }
    };

    // Prepare a hash with all .cmake files
    m_dotCMakeFilesHash.clear();
    auto handleDotCMakeFiles = [&](const CMakeFileInfo &cmakeFile) {
        if (cmakeFile.path.suffix() == "cmake") {
            Utils::Link link;
            link.targetFilePath = cmakeFile.path;
            link.targetLine = 1;
            link.targetColumn = 0;
            m_dotCMakeFilesHash.insert(cmakeFile.path.completeBaseName(), link);
        }
    };

    // Gather all Find<Package>.cmake and <Package>Config.cmake / <Package>-config.cmake files
    m_findPackagesFilesHash.clear();
    auto handleFindPackageCMakeFiles = [&](const CMakeFileInfo &cmakeFile) {
        const QString fileName = cmakeFile.path.fileName();

        const QString findPackageName = [fileName]() -> QString {
            auto findIdx = fileName.indexOf("Find");
            auto endsWithCMakeIdx = fileName.lastIndexOf(".cmake");
            if (findIdx == 0 && endsWithCMakeIdx > 0)
                return fileName.mid(4, endsWithCMakeIdx - 4);
            return QString();
        }();

        const QString configPackageName = [fileName]() -> QString {
            auto configCMakeIdx = fileName.lastIndexOf("Config.cmake");
            if (configCMakeIdx > 0)
                return fileName.left(configCMakeIdx);
            auto dashConfigCMakeIdx = fileName.lastIndexOf("-config.cmake");
            if (dashConfigCMakeIdx > 0)
                return fileName.left(dashConfigCMakeIdx);
            return QString();
        }();

        if (!findPackageName.isEmpty() || !configPackageName.isEmpty()) {
            Utils::Link link;
            link.targetFilePath = cmakeFile.path;
            link.targetLine = 1;
            link.targetColumn = 0;
            m_findPackagesFilesHash.insert(!findPackageName.isEmpty() ? findPackageName
                                                                      : configPackageName,
                                           link);
        }
    };

    for (const auto &cmakeFile : std::as_const(m_cmakeFiles)) {
        for (const auto &func : cmakeFile.cmakeListFile.Functions) {
            handleFunctionMacroOption(cmakeFile, func);
            handleImportedTargets(cmakeFile, func);
            handleProjectTargets(cmakeFile, func);
            handleFindPackageVariables(cmakeFile, func);
        }
        handleDotCMakeFiles(cmakeFile);
        handleFindPackageCMakeFiles(cmakeFile);
    }

    m_projectFindPackageVariables.removeDuplicates();
}

void CMakeBuildSystem::ensureBuildDirectory(const BuildDirParameters &parameters)
{
    const FilePath bdir = parameters.buildDirectory;

    if (!buildConfiguration()->createBuildDirectory()) {
        handleParsingFailed(Tr::tr("Failed to create build directory \"%1\".").arg(bdir.toUserOutput()));
        return;
    }

    const CMakeTool *tool = parameters.cmakeTool();
    if (!tool) {
        handleParsingFailed(Tr::tr("No CMake tool set up in kit."));
        return;
    }

    if (tool->cmakeExecutable().needsDevice()) {
        if (!tool->cmakeExecutable().ensureReachable(bdir)) {
            // Make sure that the build directory is available on the device.
            handleParsingFailed(
                Tr::tr("The remote CMake executable cannot write to the local build directory."));
        }
    }
}

void CMakeBuildSystem::stopParsingAndClearState()
{
    qCDebug(cmakeBuildSystemLog) << buildConfiguration()->displayName()
                                 << "stopping parsing run!";
    m_reader.stop();
    m_reader.resetData();
}

void CMakeBuildSystem::becameDirty()
{
    qCDebug(cmakeBuildSystemLog) << "CMakeBuildSystem: becameDirty was triggered.";
    if (isParsing())
        return;

    reparse(REPARSE_DEFAULT);
}

void CMakeBuildSystem::updateReparseParameters(const int parameters)
{
    m_reparseParameters |= parameters;
}

int CMakeBuildSystem::takeReparseParameters()
{
    int result = m_reparseParameters;
    m_reparseParameters = REPARSE_DEFAULT;
    return result;
}

void CMakeBuildSystem::runCTest()
{
    if (!m_error.isEmpty() || m_ctestPath.isEmpty()) {
        qCDebug(cmakeBuildSystemLog) << "Cancel ctest run after failed cmake run";
        emit testInformationUpdated();
        return;
    }
    qCDebug(cmakeBuildSystemLog) << "Requesting ctest run after cmake run";

    const BuildDirParameters parameters(this);
    QTC_ASSERT(parameters.isValid(), return);

    ensureBuildDirectory(parameters);
    m_ctestProcess.reset(new Process);
    m_ctestProcess->setEnvironment(buildConfiguration()->environment());
    m_ctestProcess->setWorkingDirectory(parameters.buildDirectory);
    m_ctestProcess->setCommand({m_ctestPath, { "-N", "--show-only=json-v1"}});
    connect(m_ctestProcess.get(), &Process::done, this, [this] {
        if (m_ctestProcess->result() == ProcessResult::FinishedWithSuccess) {
            const QJsonDocument json = QJsonDocument::fromJson(m_ctestProcess->rawStdOut());
            if (!json.isEmpty() && json.isObject()) {
                const QJsonObject jsonObj = json.object();
                const QJsonObject btGraph = jsonObj.value("backtraceGraph").toObject();
                const QJsonArray cmakelists = btGraph.value("files").toArray();
                const QJsonArray nodes = btGraph.value("nodes").toArray();
                const QJsonArray tests = jsonObj.value("tests").toArray();
                int counter = 0;
                for (const auto &testVal : tests) {
                    ++counter;
                    const QJsonObject test = testVal.toObject();
                    QTC_ASSERT(!test.isEmpty(), continue);
                    int file = -1;
                    int line = -1;
                    const int bt = test.value("backtrace").toInt(-1);
                    // we may have no real backtrace due to different registering
                    if (bt != -1) {
                        QSet<int> seen;
                        std::function<QJsonObject(int)> findAncestor = [&](int index){
                            QJsonObject node = nodes.at(index).toObject();
                            const int parent = node.value("parent").toInt(-1);
                            if (parent < 0 || !Utils::insert(seen, parent))
                                return node;
                            return findAncestor(parent);
                        };
                        const QJsonObject btRef = findAncestor(bt);
                        file = btRef.value("file").toInt(-1);
                        line = btRef.value("line").toInt(-1);
                    }
                    // we may have no CMakeLists.txt file reference due to different registering
                    const FilePath cmakeFile = file != -1
                            ? FilePath::fromString(cmakelists.at(file).toString()) : FilePath();
                    m_testNames.append({ test.value("name").toString(), counter, cmakeFile, line });
                }
            }
        }
        emit testInformationUpdated();
    });
    m_ctestProcess->start();
}

CMakeBuildConfiguration *CMakeBuildSystem::cmakeBuildConfiguration() const
{
    return static_cast<CMakeBuildConfiguration *>(BuildSystem::buildConfiguration());
}

static FilePaths librarySearchPaths(const CMakeBuildSystem *bs, const QString &buildKey)
{
    const CMakeBuildTarget cmakeBuildTarget
        = Utils::findOrDefault(bs->buildTargets(), [buildKey](const auto &target) {
              return target.title == buildKey && target.targetType != UtilityType;
          });

    return cmakeBuildTarget.libraryDirectories;
}

const QList<BuildTargetInfo> CMakeBuildSystem::appTargets() const
{
    const CMakeConfig &cm = configurationFromCMake();
    QString emulator = cm.stringValueOf("CMAKE_CROSSCOMPILING_EMULATOR");

    QList<BuildTargetInfo> appTargetList;
    const bool forAndroid =false;
        // DeviceTypeKitAspect::deviceTypeId(kit())
        //                     == Android::Constants::ANDROID_DEVICE_TYPE;
    for (const CMakeBuildTarget &ct : m_buildTargets) {
        if (CMakeBuildSystem::filteredOutTarget(ct))
            continue;

        if (ct.targetType == ExecutableType || (forAndroid && ct.targetType == DynamicLibraryType)) {
            const QString buildKey = ct.title;

            BuildTargetInfo bti;
            bti.displayName = ct.title;
            if (ct.launchers.size() > 0)
                bti.launchers = ct.launchers;
            else if (!emulator.isEmpty()) {
                // fallback for cmake < 3.29
                QStringList args = emulator.split(";");
                FilePath command = FilePath::fromString(args.takeFirst());
                LauncherInfo launcherInfo = { "emulator", command, args };
                bti.launchers.append(Launcher(launcherInfo, ct.sourceDirectory));
            }
            bti.targetFilePath = ct.executable;
            bti.projectFilePath = ct.sourceDirectory.cleanPath();
            bti.workingDirectory = ct.workingDirectory;
            bti.buildKey = buildKey;
            bti.usesTerminal = !ct.linksToQtGui;
            bti.isQtcRunnable = ct.qtcRunnable;

            // Workaround for QTCREATORBUG-19354:
            bti.runEnvModifier = [this, buildKey](Environment &env, bool enabled) {
                if (enabled)
                    env.prependOrSetLibrarySearchPaths(librarySearchPaths(this, buildKey));
            };

            appTargetList.append(bti);
        }
    }

    return appTargetList;
}

QStringList CMakeBuildSystem::buildTargetTitles() const
{
    auto nonAutogenTargets = filtered(m_buildTargets, [](const CMakeBuildTarget &target){
        return !CMakeBuildSystem::filteredOutTarget(target);
    });
    return transform(nonAutogenTargets, &CMakeBuildTarget::title);
}

const QList<CMakeBuildTarget> &CMakeBuildSystem::buildTargets() const
{
    return m_buildTargets;
}

bool CMakeBuildSystem::filteredOutTarget(const CMakeBuildTarget &target)
{
    return target.title.endsWith("_autogen") ||
           target.title.endsWith("_autogen_timestamp_deps");
}

bool CMakeBuildSystem::isMultiConfig() const
{
    return m_isMultiConfig;
}

void CMakeBuildSystem::setIsMultiConfig(bool isMultiConfig)
{
    m_isMultiConfig = isMultiConfig;
}

bool CMakeBuildSystem::isMultiConfigReader() const
{
    return m_reader.isMultiConfig();
}

bool CMakeBuildSystem::usesAllCapsTargets() const
{
    return m_reader.usesAllCapsTargets();
}

CMakeProject *CMakeBuildSystem::project() const
{
    return static_cast<CMakeProject *>(ProjectExplorer::BuildSystem::project());
}

const QList<TestCaseInfo> CMakeBuildSystem::testcasesInfo() const
{
    return m_testNames;
}

CommandLine CMakeBuildSystem::commandLineForTests(const QList<QString> &tests,
                                                  const QStringList &options) const
{
    const QSet<QString> testsSet = Utils::toSet(tests);
    const auto current = Utils::transform<QSet<QString>>(m_testNames, &TestCaseInfo::name);
    if (tests.isEmpty() || current == testsSet)
        return {m_ctestPath, options};

    QString testNumbers("0,0,0"); // start, end, stride
    for (const TestCaseInfo &info : m_testNames) {
        if (testsSet.contains(info.name))
            testNumbers += QString(",%1").arg(info.number);
    }
    return {m_ctestPath, {options, "-I", testNumbers}};
}

DeploymentData CMakeBuildSystem::deploymentDataFromFile() const
{
    DeploymentData result;

    FilePath sourceDir = project()->projectDirectory();
    FilePath buildDir = buildConfiguration()->buildDirectory();

    QString deploymentPrefix;
    FilePath deploymentFilePath = sourceDir.pathAppended("QtCreatorDeployment.txt");

    bool hasDeploymentFile = deploymentFilePath.exists();
    if (!hasDeploymentFile) {
        deploymentFilePath = buildDir.pathAppended("QtCreatorDeployment.txt");
        hasDeploymentFile = deploymentFilePath.exists();
    }
    if (!hasDeploymentFile)
        return result;

    deploymentPrefix = result.addFilesFromDeploymentFile(deploymentFilePath, sourceDir);
    for (const CMakeBuildTarget &ct : m_buildTargets) {
        if (ct.targetType == ExecutableType || ct.targetType == DynamicLibraryType) {
            if (!ct.executable.isEmpty()
                    && result.deployableForLocalFile(ct.executable).localFilePath() != ct.executable) {
                result.addFile(ct.executable,
                               deploymentPrefix + buildDir.relativeChildPath(ct.executable).toString(),
                               DeployableFile::TypeExecutable);
            }
        }
    }

    return result;
}

QList<ExtraCompiler *> CMakeBuildSystem::findExtraCompilers()
{
    qCDebug(cmakeBuildSystemLog) << "Finding Extra Compilers: start.";

    QList<ExtraCompiler *> extraCompilers;
    const QList<ExtraCompilerFactory *> factories = ExtraCompilerFactory::extraCompilerFactories();

    qCDebug(cmakeBuildSystemLog) << "Finding Extra Compilers: Got factories.";

    const QSet<QString> fileExtensions = Utils::transform<QSet>(factories,
                                                                &ExtraCompilerFactory::sourceTag);

    qCDebug(cmakeBuildSystemLog) << "Finding Extra Compilers: Got file extensions:"
                                 << fileExtensions;

    // Find all files generated by any of the extra compilers, in a rather crude way.
    Project *p = project();
    const FilePaths fileList = p->files([&fileExtensions](const Node *n) {
        if (!Project::SourceFiles(n) || !n->isEnabled()) // isEnabled excludes nodes from the file system tree
            return false;
        const QString suffix = n->filePath().suffix();
        return !suffix.isEmpty() && fileExtensions.contains(suffix);
    });

    qCDebug(cmakeBuildSystemLog) << "Finding Extra Compilers: Got list of files to check.";

    // Generate the necessary information:
    for (const FilePath &file : fileList) {
        qCDebug(cmakeBuildSystemLog)
            << "Finding Extra Compilers: Processing" << file.toUserOutput();
        ExtraCompilerFactory *factory = Utils::findOrDefault(factories,
                                                             [&file](const ExtraCompilerFactory *f) {
                                                                 return file.endsWith(
                                                                     '.' + f->sourceTag());
                                                             });
        QTC_ASSERT(factory, continue);

        FilePaths generated = filesGeneratedFrom(file);
        qCDebug(cmakeBuildSystemLog)
            << "Finding Extra Compilers:     generated files:" << generated;
        if (generated.isEmpty())
            continue;

        extraCompilers.append(factory->create(p, file, generated));
        qCDebug(cmakeBuildSystemLog)
            << "Finding Extra Compilers:     done with" << file.toUserOutput();
    }

    qCDebug(cmakeBuildSystemLog) << "Finding Extra Compilers: done.";

    return extraCompilers;
}

void CMakeBuildSystem::updateQmlJSCodeModel(const QStringList &extraHeaderPaths,
                                            const QList<QByteArray> &moduleMappings)
{
    QmlJS::ModelManagerInterface *modelManager = QmlJS::ModelManagerInterface::instance();

    if (!modelManager)
        return;

    Project *p = project();
    QmlJS::ModelManagerInterface::ProjectInfo projectInfo
        = modelManager->defaultProjectInfoForProject(p, p->files(Project::HiddenRccFolders));

    projectInfo.importPaths.clear();

    auto addImports = [&projectInfo](const QString &imports) {
        const QStringList importList = CMakeConfigItem::cmakeSplitValue(imports);
        for (const QString &import : importList)
            projectInfo.importPaths.maybeInsert(FilePath::fromUserInput(import), QmlJS::Dialect::Qml);
    };

    const CMakeConfig &cm = configurationFromCMake();
    addImports(cm.stringValueOf("QML_IMPORT_PATH"));
    addImports(kit()->value(QtSupport::Constants::KIT_QML_IMPORT_PATH).toString());

    for (const QString &extraHeaderPath : extraHeaderPaths)
        projectInfo.importPaths.maybeInsert(FilePath::fromString(extraHeaderPath),
                                            QmlJS::Dialect::Qml);

    for (const QByteArray &mm : moduleMappings) {
        auto kvPair = mm.split('=');
        if (kvPair.size() != 2)
            continue;
        QString from = QString::fromUtf8(kvPair.at(0).trimmed());
        QString to = QString::fromUtf8(kvPair.at(1).trimmed());
        if (!from.isEmpty() && !to.isEmpty() && from != to) {
            // The QML code-model does not support sub-projects, so if there are multiple mappings for a single module,
            // choose the shortest one.
            if (projectInfo.moduleMappings.contains(from)) {
                if (to.size() < projectInfo.moduleMappings.value(from).size())
                    projectInfo.moduleMappings.insert(from, to);
            } else {
                projectInfo.moduleMappings.insert(from, to);
            }
        }
    }

    project()->setProjectLanguage(ProjectExplorer::Constants::QMLJS_LANGUAGE_ID,
                                  !projectInfo.sourceFiles.isEmpty());
    modelManager->updateProjectInfo(projectInfo, p);
}

void CMakeBuildSystem::updateInitialCMakeExpandableVars()
{
    const CMakeConfig &cm = configurationFromCMake();
    const CMakeConfig &initialConfig =
        cmakeBuildConfiguration()->initialCMakeArguments.cmakeConfiguration();

    CMakeConfig config;

    const FilePath projectDirectory = project()->projectDirectory();
    const auto samePath = [projectDirectory](const FilePath &first, const FilePath &second) {
        // if a path is relative, resolve it relative to the project directory
        // this is not 100% correct since CMake resolve them to CMAKE_CURRENT_SOURCE_DIR
        // depending on context, but we cannot do better here
        return first == second
               || projectDirectory.resolvePath(first)
                      == projectDirectory.resolvePath(second)
               || projectDirectory.resolvePath(first).canonicalPath()
                      == projectDirectory.resolvePath(second).canonicalPath();
    };

    // Replace path values that do not  exist on file system
    const QByteArrayList singlePathList = {
        "CMAKE_C_COMPILER",
        "CMAKE_CXX_COMPILER",
        "QT_QMAKE_EXECUTABLE",
        "QT_HOST_PATH",
        "CMAKE_TOOLCHAIN_FILE"
    };
    for (const auto &var : singlePathList) {
        auto it = std::find_if(cm.cbegin(), cm.cend(), [var](const CMakeConfigItem &item) {
            return item.key == var && !item.isInitial;
        });

        if (it != cm.cend()) {
            const QByteArray initialValue = initialConfig.expandedValueOf(kit(), var).toUtf8();
            const FilePath initialPath = FilePath::fromUserInput(QString::fromUtf8(initialValue));
            const FilePath path = FilePath::fromUserInput(QString::fromUtf8(it->value));

            if (!initialValue.isEmpty() && !samePath(path, initialPath) && !path.exists()) {
                CMakeConfigItem item(*it);
                item.value = initialValue;

                config << item;
            }
        }
    }

    // Prepend new values to existing path lists
    const QByteArrayList multiplePathList = {
        "CMAKE_PREFIX_PATH",
        "CMAKE_FIND_ROOT_PATH"
    };
    for (const auto &var : multiplePathList) {
        auto it = std::find_if(cm.cbegin(), cm.cend(), [var](const CMakeConfigItem &item) {
            return item.key == var && !item.isInitial;
        });

        if (it != cm.cend()) {
            const QByteArrayList initialValueList = initialConfig.expandedValueOf(kit(), var).toUtf8().split(';');

            for (const auto &initialValue: initialValueList) {
                const FilePath initialPath = FilePath::fromUserInput(QString::fromUtf8(initialValue));

                const bool pathIsContained
                    = Utils::contains(it->value.split(';'), [samePath, initialPath](const QByteArray &p) {
                          return samePath(FilePath::fromUserInput(QString::fromUtf8(p)), initialPath);
                      });
                if (!initialValue.isEmpty() && !pathIsContained) {
                    CMakeConfigItem item(*it);
                    item.value = initialValue;
                    item.value.append(";");
                    item.value.append(it->value);

                    config << item;
                }
            }
        }
    }

    // Handle MSVC C/C++ compiler update, by udating also the linker, otherwise projects
    // will fail to compile by using a linker that doesn't exist
    const FilePath cxxCompiler = config.filePathValueOf("CMAKE_CXX_COMPILER");
    if (!cxxCompiler.isEmpty() && cxxCompiler.fileName() == "cl.exe") {
        const FilePath linker = cm.filePathValueOf("CMAKE_LINKER");
        if (!linker.exists())
            config << CMakeConfigItem(
                "CMAKE_LINKER",
                CMakeConfigItem::FILEPATH,
                cxxCompiler.parentDir().pathAppended(linker.fileName()).path().toUtf8());
    }

    if (!config.isEmpty())
        emit configurationChanged(config);
}

MakeInstallCommand CMakeBuildSystem::makeInstallCommand(const FilePath &installRoot) const
{
    MakeInstallCommand cmd;
    if (CMakeTool *tool = CMakeKitAspect::cmakeTool(target()->kit()))
        cmd.command.setExecutable(tool->cmakeExecutable());

    QString installTarget = "install";
    if (usesAllCapsTargets())
        installTarget = "INSTALL";

    FilePath buildDirectory = ".";
    Project *project = nullptr;
    if (auto bc = buildConfiguration()) {
        buildDirectory = bc->buildDirectory();
        project = bc->project();
    }

    cmd.command.addArg("--build");
    cmd.command.addArg(CMakeToolManager::mappedFilePath(project, buildDirectory).path());
    cmd.command.addArg("--target");
    cmd.command.addArg(installTarget);

    if (isMultiConfigReader())
        cmd.command.addArgs({"--config", cmakeBuildType()});

    cmd.environment.set("DESTDIR", installRoot.nativePath());
    return cmd;
}

QList<QPair<Id, QString>> CMakeBuildSystem::generators() const
{
    if (!buildConfiguration())
        return {};
    const CMakeTool * const cmakeTool
            = CMakeKitAspect::cmakeTool(buildConfiguration()->target()->kit());
    if (!cmakeTool)
        return {};
    QList<QPair<Id, QString>> result;
    const QList<CMakeTool::Generator> &generators = cmakeTool->supportedGenerators();
    for (const CMakeTool::Generator &generator : generators) {
        result << qMakePair(Id::fromSetting(generator.name),
                            Tr::tr("%1 (via cmake)").arg(generator.name));
    }
    return result;
}

void CMakeBuildSystem::runGenerator(Id id)
{
    QTC_ASSERT(cmakeBuildConfiguration(), return);
    const auto showError = [](const QString &detail) {
        Core::MessageManager::writeDisrupting(
            addCMakePrefix(Tr::tr("cmake generator failed: %1.").arg(detail)));
    };
    const CMakeTool * const cmakeTool
            = CMakeKitAspect::cmakeTool(buildConfiguration()->target()->kit());
    if (!cmakeTool) {
        showError(Tr::tr("Kit does not have a cmake binary set."));
        return;
    }
    const QString generator = id.toSetting().toString();
    const FilePath outDir = buildConfiguration()->buildDirectory()
            / ("qtc_" + FileUtils::fileSystemFriendlyName(generator));
    if (!outDir.ensureWritableDir()) {
        showError(Tr::tr("Cannot create output directory \"%1\".").arg(outDir.toString()));
        return;
    }
    CommandLine cmdLine(cmakeTool->cmakeExecutable(), {"-S", buildConfiguration()->target()
                        ->project()->projectDirectory().toUserOutput(), "-G", generator});
    if (!cmdLine.executable().isExecutableFile()) {
        showError(Tr::tr("No valid cmake executable."));
        return;
    }
    const auto itemFilter = [](const CMakeConfigItem &item) {
        return !item.isNull()
                && item.type != CMakeConfigItem::STATIC
                && item.type != CMakeConfigItem::INTERNAL
                && !item.key.contains("GENERATOR");
    };
    QList<CMakeConfigItem> configItems = Utils::filtered(m_configurationChanges.toList(),
                                                         itemFilter);
    const QList<CMakeConfigItem> initialConfigItems
            = Utils::filtered(cmakeBuildConfiguration()->initialCMakeArguments.cmakeConfiguration().toList(),
                          itemFilter);
    for (const CMakeConfigItem &item : std::as_const(initialConfigItems)) {
        if (!Utils::contains(configItems, [&item](const CMakeConfigItem &existingItem) {
            return existingItem.key == item.key;
        })) {
            configItems << item;
        }
    }
    for (const CMakeConfigItem &item : std::as_const(configItems))
        cmdLine.addArg(item.toArgument(buildConfiguration()->macroExpander()));

    cmdLine.addArgs(cmakeBuildConfiguration()->additionalCMakeOptions(), CommandLine::Raw);

    const auto proc = new Process(this);
    connect(proc, &Process::done, proc, &Process::deleteLater);
    connect(proc, &Process::readyReadStandardOutput, this, [proc] {
        Core::MessageManager::writeFlashing(
            addCMakePrefix(QString::fromLocal8Bit(proc->readAllRawStandardOutput()).split('\n')));
    });
    connect(proc, &Process::readyReadStandardError, this, [proc] {
        Core::MessageManager::writeDisrupting(
            addCMakePrefix(QString::fromLocal8Bit(proc->readAllRawStandardError()).split('\n')));
    });
    proc->setWorkingDirectory(outDir);
    proc->setEnvironment(buildConfiguration()->environment());
    proc->setCommand(cmdLine);
    Core::MessageManager::writeFlashing(addCMakePrefix(
        Tr::tr("Running in \"%1\": %2.").arg(outDir.toUserOutput(), cmdLine.toUserOutput())));
    proc->start();
}

ExtraCompiler *CMakeBuildSystem::findExtraCompiler(const ExtraCompilerFilter &filter) const
{
    return Utils::findOrDefault(m_extraCompilers, filter);
}

} // CMakeProjectManager::Internal
