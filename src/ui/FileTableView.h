#pragma once

#include "core/Types.h"

#include <QMimeData>
#include <QTableView>
#include <optional>
#include <vector>

namespace xsync {

struct XsyncMimePayload {
    EndpointSpec sourceEndpoint;
    QString sourceDir;
    bool isCut = false;
    std::vector<FileEntry> entries;
};

// Table view supporting:
// 1. Direct Drag & Drop from OS Explorer (CF_HDROP / text/uri-list) into Local or Remote panes
// 2. Direct Drag & Drop between xsync panes (Local<->Remote, Remote<->Remote, cross-window!)
// 3. Ctrl+C / Ctrl+X / Ctrl+V seamlessly interoperable with OS Explorer clipboard AND xsync panes
class FileTableView : public QTableView {
    Q_OBJECT
public:
    explicit FileTableView(QWidget* parent = nullptr);

    void setContextInfo(const EndpointSpec& endpoint, const QString& currentDir);
    std::vector<FileEntry> selectedEntries() const;

    void copySelectionToClipboard(bool isCut);
    void pasteFromClipboard();

    // Public MIME helpers (also covered by unit tests)
    static QByteArray encodePayload(const XsyncMimePayload& payload);
    static std::optional<XsyncMimePayload> decodePayload(const QByteArray& bytes);

signals:
    void externalLocalFilesDropped(const QStringList& localPaths, const QString& targetDir);
    void internalEntriesDropped(const xsync::XsyncMimePayload& payload, const QString& targetDir);
    void deleteKeyPressed();
    void renameKeyPressed();
    void backspaceKeyPressed();

protected:
    void startDrag(Qt::DropActions supportedActions) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    QString resolveDropTargetDir(const QPoint& pos) const;
    bool canAcceptMime(const QMimeData* mime) const;

    EndpointSpec endpoint_;
    QString currentDir_;
    bool dragHoverActive_ = false;
    QModelIndex hoveredFolderIndex_;
};

}  // namespace xsync
