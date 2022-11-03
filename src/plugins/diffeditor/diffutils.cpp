static int forBlockNumber(const QMap<int, QPair<int, int>> &chunkInfo, int blockNumber,
                          const std::function<int (int, int, int)> &func)
{
    if (chunkInfo.isEmpty())
        return -1;

    auto it = chunkInfo.upperBound(blockNumber);
    if (it == chunkInfo.constBegin())
        return -1;

    --it;

    if (blockNumber < it.key() + it.value().first)
        return func(it.key(), it.value().first, it.value().second);

    return -1;
}

int DiffChunkInfo::chunkRowForBlockNumber(int blockNumber) const
{
    return forBlockNumber(m_chunkInfo, blockNumber, [blockNumber](int startBlockNumber, int, int)
                          { return blockNumber - startBlockNumber; });
}

int DiffChunkInfo::chunkRowsCountForBlockNumber(int blockNumber) const
{
    return forBlockNumber(m_chunkInfo, blockNumber,
                          [](int, int rowsCount, int) { return rowsCount; });
}

int DiffChunkInfo::chunkIndexForBlockNumber(int blockNumber) const
{
    return forBlockNumber(m_chunkInfo, blockNumber,
                          [](int, int, int chunkIndex) { return chunkIndex; });
}

    return Utils::toSet(selection[LeftSide]).unite(Utils::toSet(selection[RightSide])).size();
    const bool leftLineEqual = leftLines.isEmpty() || leftLines.last().text.isEmpty();
    const bool rightLineEqual = rightLines.isEmpty() || rightLines.last().text.isEmpty();
static void handleLine(const QStringList &newLines, int line, QList<TextLineData> *lines,
static void handleDifference(const QString &text, QList<TextLineData> *lines, int *lineNumber)
    QList<TextLineData> leftData = assemblyRows(leftLines, leftSpans);
    QList<TextLineData> rightData = assemblyRows(rightLines, rightSpans);
        chunkData.startingLineNumber = {leftLineNumber, rightLineNumber};
            if (rowData.line[LeftSide].textLineType == TextLineData::TextLine)
            if (rowData.line[RightSide].textLineType == TextLineData::TextLine)
        return {};
            if (rowData.line[LeftSide].textLineType != TextLineData::Separator
                    || rowData.line[RightSide].textLineType != TextLineData::TextLine)
            if (rowData.line[RightSide].textLineType != TextLineData::Separator
                    || rowData.line[LeftSide].textLineType != TextLineData::TextLine)
                                                   rowData.line[RightSide].text,
            if (rowData.line[LeftSide].textLineType == TextLineData::TextLine)
                leftBuffer.append(rowData.line[LeftSide]);
            if (rowData.line[RightSide].textLineType == TextLineData::TextLine)
                rightBuffer.append(rowData.line[RightSide]);
            + QString::number(chunkData.startingLineNumber[LeftSide] + 1)
            + QString::number(chunkData.startingLineNumber[RightSide] + 1)
        str << fileData.fileInfo[LeftSide].fileName;
        str << fileData.fileInfo[RightSide].fileName;
            str << "diff --git a/" << fileData.fileInfo[LeftSide].fileName
                << " b/" << fileData.fileInfo[RightSide].fileName << '\n';
        str << "index " << fileData.fileInfo[LeftSide].typeInfo << ".." << fileData.fileInfo[RightSide].typeInfo;
        return {};
            *remainingText = {};
    chunkData->startingLineNumber = {leftLineNumber - 1, rightLineNumber - 1};
        fileData.fileInfo[LeftSide].fileName = leftMatch.captured(1);
            fileData.fileInfo[RightSide].fileName = rightMatch.captured(1);
            fileData.fileInfo[LeftSide].fileName = binaryMatch.captured(1);
            fileData.fileInfo[RightSide].fileName = binaryMatch.captured(2);
        return {};
                return {};
        return {};
        fileData->fileInfo[LeftSide].typeInfo = indices.left(dotsPosition).toString();
        fileData->fileInfo[RightSide].typeInfo = indices.mid(dotsPosition + 2, length).toString();
            ? devNull : QLatin1String("a/") + fileData->fileInfo[LeftSide].fileName;
            ? devNull : QLatin1String("b/") + fileData->fileInfo[RightSide].fileName;
        *remainingPatch = {};
        fileData->fileInfo[LeftSide].fileName = fileData->fileInfo[RightSide].fileName = commonFileName.toString();
            fileData->fileInfo[LeftSide].fileName = copyRenameFrom.mid(copyFrom.size()).toString();
            fileData->fileInfo[LeftSide].fileName = copyRenameFrom.mid(renameFrom.size()).toString();
            fileData->fileInfo[RightSide].fileName = copyRenameTo.mid(copyTo.size()).toString();
            fileData->fileInfo[RightSide].fileName = copyRenameTo.mid(renameTo.size()).toString();
            return {};
        return {};
    for (const auto &patchInfo : std::as_const(patches)) {
                return {};
        return {};
int DiffUtils::interpolate(int x, int x1, int x2, int y1, int y2)
{
    if (x1 == x2)
        return x1;
    if (x == x1)
        return y1;
    if (x == x2)
        return y2;
    const int numerator = (y2 - y1) * x + x2 * y1 - x1 * y2;
    const int denominator = x2 - x1;
    return qRound((double)numerator / denominator);
}
