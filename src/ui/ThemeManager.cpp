#include "ui/ThemeManager.h"

#include "ui/IconCache.h"

#include <QApplication>
#include <QComboBox>
#include <QCompleter>
#include <QDialogButtonBox>
#include <QFile>
#include <QFrame>
#include <QGuiApplication>
#include <QListView>
#include <QPalette>
#include <QPushButton>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>
#include <QStyledItemDelegate>
#include <QWidget>
#include <QtGlobal>

static void initXsyncThemeResources() {
    static bool done = false;
    if (!done) {
        Q_INIT_RESOURCE(xsync);
        done = true;
    }
}

namespace xsync {

namespace {

bool g_isDark = true;

QPalette buildDarkPalette() {
    QPalette pal;
    pal.setColor(QPalette::Window, QColor(QStringLiteral("#161922")));
    pal.setColor(QPalette::WindowText, QColor(QStringLiteral("#e2e8f0")));
    pal.setColor(QPalette::Base, QColor(QStringLiteral("#11141c")));
    pal.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#181d28")));
    pal.setColor(QPalette::Text, QColor(QStringLiteral("#f1f5f9")));
    pal.setColor(QPalette::PlaceholderText, QColor(QStringLiteral("#64748b")));
    pal.setColor(QPalette::Button, QColor(QStringLiteral("#262c3b")));
    pal.setColor(QPalette::ButtonText, QColor(QStringLiteral("#e2e8f0")));
    pal.setColor(QPalette::BrightText, QColor(QStringLiteral("#ffffff")));
    pal.setColor(QPalette::Highlight, QColor(QStringLiteral("#2563eb")));
    pal.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#ffffff")));
    pal.setColor(QPalette::Link, QColor(QStringLiteral("#60a5fa")));
    pal.setColor(QPalette::LinkVisited, QColor(QStringLiteral("#a78bfa")));
    pal.setColor(QPalette::ToolTipBase, QColor(QStringLiteral("#1e2433")));
    pal.setColor(QPalette::ToolTipText, QColor(QStringLiteral("#e2e8f0")));
    pal.setColor(QPalette::Light, QColor(QStringLiteral("#363f54")));
    pal.setColor(QPalette::Midlight, QColor(QStringLiteral("#2b3142")));
    pal.setColor(QPalette::Mid, QColor(QStringLiteral("#222836")));
    pal.setColor(QPalette::Dark, QColor(QStringLiteral("#141821")));
    pal.setColor(QPalette::Shadow, QColor(QStringLiteral("#0b0e14")));
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    pal.setColor(QPalette::Accent, QColor(QStringLiteral("#2563eb")));
#endif

    // Inactive group (dual-pane unfocused selection clarity)
    pal.setColor(QPalette::Inactive, QPalette::Highlight, QColor(QStringLiteral("#263759")));
    pal.setColor(QPalette::Inactive, QPalette::HighlightedText, QColor(QStringLiteral("#e2e8f0")));

    // Disabled group
    pal.setColor(QPalette::Disabled, QPalette::Window, QColor(QStringLiteral("#161922")));
    pal.setColor(QPalette::Disabled, QPalette::WindowText, QColor(QStringLiteral("#64748b")));
    pal.setColor(QPalette::Disabled, QPalette::Base, QColor(QStringLiteral("#151923")));
    pal.setColor(QPalette::Disabled, QPalette::Text, QColor(QStringLiteral("#64748b")));
    pal.setColor(QPalette::Disabled, QPalette::PlaceholderText, QColor(QStringLiteral("#475569")));
    pal.setColor(QPalette::Disabled, QPalette::Button, QColor(QStringLiteral("#1b202b")));
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(QStringLiteral("#64748b")));
    pal.setColor(QPalette::Disabled, QPalette::Highlight, QColor(QStringLiteral("#1e293b")));
    pal.setColor(QPalette::Disabled, QPalette::HighlightedText, QColor(QStringLiteral("#64748b")));
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    pal.setColor(QPalette::Disabled, QPalette::Accent, QColor(QStringLiteral("#334155")));
#endif
    return pal;
}

QPalette buildLightPalette() {
    QPalette pal;
    pal.setColor(QPalette::Window, QColor(QStringLiteral("#f4f6fa")));
    pal.setColor(QPalette::WindowText, QColor(QStringLiteral("#0f172a")));
    pal.setColor(QPalette::Base, QColor(QStringLiteral("#ffffff")));
    pal.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#f8fafc")));
    pal.setColor(QPalette::Text, QColor(QStringLiteral("#0f172a")));
    pal.setColor(QPalette::PlaceholderText, QColor(QStringLiteral("#94a3b8")));
    pal.setColor(QPalette::Button, QColor(QStringLiteral("#ffffff")));
    pal.setColor(QPalette::ButtonText, QColor(QStringLiteral("#1e293b")));
    pal.setColor(QPalette::BrightText, QColor(QStringLiteral("#ffffff")));
    pal.setColor(QPalette::Highlight, QColor(QStringLiteral("#2563eb")));
    pal.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#ffffff")));
    pal.setColor(QPalette::Link, QColor(QStringLiteral("#2563eb")));
    pal.setColor(QPalette::LinkVisited, QColor(QStringLiteral("#7c3aed")));
    pal.setColor(QPalette::ToolTipBase, QColor(QStringLiteral("#ffffff")));
    pal.setColor(QPalette::ToolTipText, QColor(QStringLiteral("#0f172a")));
    pal.setColor(QPalette::Light, QColor(QStringLiteral("#ffffff")));
    pal.setColor(QPalette::Midlight, QColor(QStringLiteral("#f1f5f9")));
    pal.setColor(QPalette::Mid, QColor(QStringLiteral("#cbd5e1")));
    pal.setColor(QPalette::Dark, QColor(QStringLiteral("#94a3b8")));
    pal.setColor(QPalette::Shadow, QColor(QStringLiteral("#64748b")));
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    pal.setColor(QPalette::Accent, QColor(QStringLiteral("#2563eb")));
#endif

    // Inactive group (dual-pane unfocused selection clarity)
    pal.setColor(QPalette::Inactive, QPalette::Highlight, QColor(QStringLiteral("#dbeafe")));
    pal.setColor(QPalette::Inactive, QPalette::HighlightedText, QColor(QStringLiteral("#1e293b")));

    // Disabled group
    pal.setColor(QPalette::Disabled, QPalette::Window, QColor(QStringLiteral("#f4f6fa")));
    pal.setColor(QPalette::Disabled, QPalette::WindowText, QColor(QStringLiteral("#94a3b8")));
    pal.setColor(QPalette::Disabled, QPalette::Base, QColor(QStringLiteral("#f1f5f9")));
    pal.setColor(QPalette::Disabled, QPalette::Text, QColor(QStringLiteral("#94a3b8")));
    pal.setColor(QPalette::Disabled, QPalette::PlaceholderText, QColor(QStringLiteral("#cbd5e1")));
    pal.setColor(QPalette::Disabled, QPalette::Button, QColor(QStringLiteral("#f1f5f9")));
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(QStringLiteral("#94a3b8")));
    pal.setColor(QPalette::Disabled, QPalette::Highlight, QColor(QStringLiteral("#e2e8f0")));
    pal.setColor(QPalette::Disabled, QPalette::HighlightedText, QColor(QStringLiteral("#94a3b8")));
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    pal.setColor(QPalette::Disabled, QPalette::Accent, QColor(QStringLiteral("#cbd5e1")));
#endif
    return pal;
}

}  // namespace

// ---------------------------------------------------------------------------
// XsyncStyle
// ---------------------------------------------------------------------------
XsyncStyle::XsyncStyle()
    : QProxyStyle(QStyleFactory::create(QStringLiteral("Fusion"))) {}

int XsyncStyle::styleHint(StyleHint hint,
                          const QStyleOption* option,
                          const QWidget* widget,
                          QStyleHintReturn* returnData) const {
    switch (hint) {
        case QStyle::SH_ComboBox_Popup:
            return 0;
        case QStyle::SH_DialogButtonBox_ButtonsHaveIcons:
            return 0;
        case QStyle::SH_EtchDisabledText:
            return 0;
        case QStyle::SH_ScrollBar_Transient:
            return 0;
        default:
            return QProxyStyle::styleHint(hint, option, widget, returnData);
    }
}

// ---------------------------------------------------------------------------
// ThemeManager
// ---------------------------------------------------------------------------
ThemeManager::ThemeManager(QObject* parent) : QObject(parent) {}

ThemeManager& ThemeManager::instance() {
    static ThemeManager mgr;
    return mgr;
}

void ThemeManager::initialize(QApplication& app) {
    initXsyncThemeResources();
    app.setStyle(new XsyncStyle());
}

void ThemeManager::applyTheme(bool dark) {
    initXsyncThemeResources();
    g_isDark = dark;
    IconCache::setDarkTheme(dark);

    if (!qApp) {
        emit instance().themeChanged(dark);
        return;
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    if (auto* hints = QGuiApplication::styleHints()) {
        hints->setColorScheme(dark ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);
    }
#endif

    // Apply complete 3-group QPalette AFTER setColorScheme and BEFORE setStyleSheet
    qApp->setPalette(dark ? buildDarkPalette() : buildLightPalette());

    QFile qssFile(dark ? QStringLiteral(":/styles/dark.qss") : QStringLiteral(":/styles/light.qss"));
    if (qssFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qApp->setStyleSheet(QString::fromUtf8(qssFile.readAll()));
    }

    emit instance().themeChanged(dark);
}

bool ThemeManager::isDark() {
    return g_isDark;
}

void ThemeManager::polishComboBox(QComboBox* combo) {
    if (!combo || combo->property("_xsync_combo_polished").toBool()) {
        return;
    }
    combo->setProperty("_xsync_combo_polished", true);

    auto* view = new QListView(combo);
    view->setObjectName(QStringLiteral("comboPopupView"));
    view->setFrameShape(QFrame::Box);
    view->setLineWidth(1);
    view->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    view->setUniformItemSizes(true);
    view->setMouseTracking(true);
    view->setSpacing(1);

    // Parent delegate to view (QAbstractItemView::setItemDelegate does NOT take ownership)
    view->setItemDelegate(new QStyledItemDelegate(view));

    // QComboBox reparents view into its QComboBoxPrivateContainer (owned by combo)
    combo->setView(view);
    combo->setMaxVisibleItems(12);

    // Configure opaque container without WA_TranslucentBackground (avoids black corners/shadows)
    if (QWidget* container = view->window(); container && container != combo->window()) {
        container->setObjectName(QStringLiteral("comboPopupContainer"));
        if (auto* frame = qobject_cast<QFrame*>(container)) {
            frame->setFrameShape(QFrame::NoFrame);
            frame->setLineWidth(0);
        }
    }

    // Editable combobox (e.g. SessionEditDialog::groupCombo_): explicitly configure & polish QCompleter popup
    if (combo->isEditable()) {
        if (QCompleter* comp = combo->completer()) {
            comp->setCompletionMode(QCompleter::PopupCompletion);
            comp->setCaseSensitivity(Qt::CaseInsensitive);
            auto* compPopup = new QListView();
            compPopup->setObjectName(QStringLiteral("comboPopupView"));
            compPopup->setFrameShape(QFrame::Box);
            compPopup->setLineWidth(1);
            compPopup->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
            compPopup->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
            compPopup->setUniformItemSizes(true);
            compPopup->setMouseTracking(true);
            compPopup->setSpacing(1);
            compPopup->setItemDelegate(new QStyledItemDelegate(compPopup));
            // QCompleter takes ownership of compPopup
            comp->setPopup(compPopup);
        }
    }
}

void ThemeManager::polishTree(QWidget* root) {
    if (!root) return;
    if (auto* combo = qobject_cast<QComboBox*>(root)) {
        polishComboBox(combo);
    }
    const auto combos = root->findChildren<QComboBox*>();
    for (QComboBox* combo : combos) {
        polishComboBox(combo);
    }
}

void ThemeManager::setStatus(QWidget* w, const char* status) {
    if (!w) return;
    w->setProperty("status", status ? QString::fromLatin1(status) : QString());
    if (QStyle* st = w->style()) {
        st->unpolish(w);
        st->polish(w);
    }
    w->update();
}

void ThemeManager::styleDialogButtonBox(QDialogButtonBox* box) {
    if (!box) return;
    if (QPushButton* okBtn = box->button(QDialogButtonBox::Ok)) {
        okBtn->setObjectName(QStringLiteral("primaryBtn"));
        okBtn->setProperty("variant", QStringLiteral("primary"));
        if (QStyle* st = okBtn->style()) {
            st->unpolish(okBtn);
            st->polish(okBtn);
        }
    }
}

QColor ThemeManager::protocolColor(TransferProtocol protocol) {
    if (g_isDark) {
        if (protocol == TransferProtocol::Rsync || protocol == TransferProtocol::RemoteDirectRsync) {
            return QColor(QStringLiteral("#38bdf8"));
        }
        if (protocol == TransferProtocol::Sftp || protocol == TransferProtocol::RemoteRelaySftp) {
            return QColor(QStringLiteral("#4ade80"));
        }
        return QColor(QStringLiteral("#fbbf24"));
    }
    // Light mode: WCAG 2.1 AA (>= 4.5:1) contrast on #ffffff / #f8fafc
    if (protocol == TransferProtocol::Rsync || protocol == TransferProtocol::RemoteDirectRsync) {
        return QColor(QStringLiteral("#0369a1"));
    }
    if (protocol == TransferProtocol::Sftp || protocol == TransferProtocol::RemoteRelaySftp) {
        return QColor(QStringLiteral("#15803d"));
    }
    return QColor(QStringLiteral("#b45309"));
}

QColor ThemeManager::statusColor(TransferStatus status) {
    if (g_isDark) {
        switch (status) {
            case TransferStatus::Failed: return QColor(QStringLiteral("#f87171"));
            case TransferStatus::Completed: return QColor(QStringLiteral("#4ade80"));
            case TransferStatus::Paused: return QColor(QStringLiteral("#fbbf24"));
            case TransferStatus::Canceled: return QColor(QStringLiteral("#94a3b8"));
            case TransferStatus::Scanning: return QColor(QStringLiteral("#38bdf8"));
            default: return {};
        }
    }
    switch (status) {
        case TransferStatus::Failed: return QColor(QStringLiteral("#b91c1c"));
        case TransferStatus::Completed: return QColor(QStringLiteral("#15803d"));
        case TransferStatus::Paused: return QColor(QStringLiteral("#b45309"));
        case TransferStatus::Canceled: return QColor(QStringLiteral("#475569"));
        case TransferStatus::Scanning: return QColor(QStringLiteral("#0369a1"));
        default: return {};
    }
}

QColor ThemeManager::errorTextColor() {
    return g_isDark ? QColor(QStringLiteral("#f87171")) : QColor(QStringLiteral("#b91c1c"));
}

ProgressBarColors ThemeManager::progressColors() {
    if (g_isDark) {
        return {
            QColor(QStringLiteral("#1e293b")),  // track
            QColor(QStringLiteral("#334155")),  // border
            QColor(QStringLiteral("#2563eb")),  // fillRunning (5.17:1)
            QColor(QStringLiteral("#15803d")),  // fillCompleted (5.02:1)
            QColor(QStringLiteral("#dc2626")),  // fillFailed (4.83:1)
            QColor(QStringLiteral("#b45309")),  // fillPaused (5.02:1)
            QColor(QStringLiteral("#f8fafc")),  // textOnTrack (13.98:1)
            QColor(QStringLiteral("#ffffff"))   // textOnFill
        };
    }
    return {
        QColor(QStringLiteral("#e2e8f0")),  // track
        QColor(QStringLiteral("#cbd5e1")),  // border
        QColor(QStringLiteral("#2563eb")),  // fillRunning (5.17:1)
        QColor(QStringLiteral("#15803d")),  // fillCompleted (5.02:1)
        QColor(QStringLiteral("#dc2626")),  // fillFailed (4.83:1)
        QColor(QStringLiteral("#b45309")),  // fillPaused (5.02:1)
        QColor(QStringLiteral("#0f172a")),  // textOnTrack (14.48:1)
        QColor(QStringLiteral("#ffffff"))   // textOnFill
    };
}

DragBadgeColors ThemeManager::dragBadgeColors() {
    if (g_isDark) {
        return {
            QColor(37, 99, 235, 235),
            QColor(QStringLiteral("#93c5fd")),
            QColor(QStringLiteral("#ffffff"))
        };
    }
    return {
        QColor(37, 99, 235, 235),
        QColor(QStringLiteral("#1d4ed8")),
        QColor(QStringLiteral("#ffffff"))
    };
}

QColor ThemeManager::dropAccentColor() {
    return g_isDark ? QColor(QStringLiteral("#3b82f6")) : QColor(QStringLiteral("#2563eb"));
}

QColor ThemeManager::dropFolderFillColor() {
    return g_isDark ? QColor(59, 130, 246, 56) : QColor(37, 99, 235, 36);
}

}  // namespace xsync
