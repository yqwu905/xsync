#pragma once

#include "core/Types.h"

#include <QColor>
#include <QObject>
#include <QProxyStyle>

class QApplication;
class QComboBox;
class QDialogButtonBox;
class QWidget;

namespace xsync {

// Fusion proxy style guaranteeing clean cross-platform popup/button/scrollbar hints
// on both Linux (Qt 6.4+) and Windows (Qt 6.8+).
class XsyncStyle : public QProxyStyle {
public:
    XsyncStyle();

    int styleHint(StyleHint hint,
                  const QStyleOption* option = nullptr,
                  const QWidget* widget = nullptr,
                  QStyleHintReturn* returnData = nullptr) const override;
};

struct ProgressBarColors {
    QColor track;
    QColor border;
    QColor fillRunning;
    QColor fillCompleted;
    QColor fillFailed;
    QColor fillPaused;
    QColor textOnTrack;
    QColor textOnFill;
};

struct DragBadgeColors {
    QColor bg;
    QColor border;
    QColor text;
};

// Centralized application style, 3-group QPalette, QSS loader, QComboBox popup polisher,
// and theme-aware semantic color tokens.
class ThemeManager : public QObject {
    Q_OBJECT
public:
    static ThemeManager& instance();

    static void initialize(QApplication& app);
    static void applyTheme(bool dark);
    static bool isDark();

    // Idempotently replaces Fusion's QComboBoxListView/QComboBoxDelegate with a plain
    // QListView (#comboPopupView) + QStyledItemDelegate inside an opaque frameless container
    // (#comboPopupContainer), and also polishes QCompleter popups on editable comboboxes.
    // NEVER called from QProxyStyle::polish().
    static void polishComboBox(QComboBox* combo);
    static void polishTree(QWidget* root);

    // Sets dynamic property "status" ("muted"|"ok"|"warn"|"error") and triggers repolish.
    static void setStatus(QWidget* w, const char* status);

    // Marks QDialogButtonBox::Ok as #primaryBtn + [variant="primary"] and repolishes.
    static void styleDialogButtonBox(QDialogButtonBox* box);

    // Theme-aware semantic colors (WCAG AA compliant in both Dark and Light themes)
    static QColor protocolColor(TransferProtocol protocol);
    static QColor statusColor(TransferStatus status);
    static QColor errorTextColor();
    static ProgressBarColors progressColors();
    static DragBadgeColors dragBadgeColors();
    static QColor dropAccentColor();
    static QColor dropFolderFillColor();

signals:
    void themeChanged(bool dark);

private:
    explicit ThemeManager(QObject* parent = nullptr);
};

}  // namespace xsync
