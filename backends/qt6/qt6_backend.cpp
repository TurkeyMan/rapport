// Qt 6 backend: Qt Widgets for every control, lists as a QTreeView whose
// rows are painted here (as the win32 backend paints its ListView), text
// views as QPlainTextEdit, and windows as QDialogs (Enter presses the
// default button, Esc ends a dialog).
//
// No moc: signals go to lambdas, and events to overrides and event filters.
#include "anyui/backend.h"
#include "qt_painter.h"

#include "builtin_icons.h"

#include <QAbstractTableModel>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QBoxLayout>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHeaderView>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeDatabase>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QTextBlock>
#include <QTimer>
#include <QTreeView>
#include <QDateTime>
#include <QCursor>
#include <QPainter>
#include <QPixmap>
#include <QWheelEvent>
#include <QToolButton>
#include <QToolBar>
#include <QSlider>
#include <QPointingDevice>
#include <QMouseEvent>
#include <QDoubleSpinBox>
#include <QColorDialog>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>

namespace anyui::qt6
{
namespace
{

// ---------------------------------------------------------------- helpers

struct Metrics
{
    float charWidth = 8;
    float lineHeight = 17;
};
Metrics g_metrics;

void initMetrics()
{
    const QFontMetricsF fm(QApplication::font());
    g_metrics.charWidth = float(fm.averageCharWidth());
    g_metrics.lineHeight = float(fm.lineSpacing());
}

int px(Extent e)
{
    switch (e.unit)
    {
    case Extent::Unit::Chars: return int(e.value * g_metrics.charWidth + 0.5f);
    case Extent::Unit::Lines: return int(e.value * g_metrics.lineHeight + 0.5f);
    case Extent::Unit::Px: return int(e.value);
    case Extent::Unit::Auto: break;
    }
    return -1;
}

QString qs(const std::string& s)
{
    return QString::fromUtf8(s.data(), qsizetype(s.size()));
}

// anyui's access keys ("_File", "__" for an underscore) as Qt's ("&File",
// "&&" for an ampersand).
QString mnemonic(const std::string& s)
{
    QString out;
    const QString in = qs(s);
    for (qsizetype i = 0; i < in.size(); ++i)
    {
        const QChar c = in[i];
        if (c == '&')
            out += "&&";
        else if (c == '_' && i + 1 < in.size())
            out += in[++i] == '_' ? QString("_") : QString("&") + in[i];
        else
            out += c;
    }
    return out;
}

// Plain text where Qt would read '&' as an access key.
QString literal(const std::string& s)
{
    return qs(s).replace("&", "&&");
}

Qt::Alignment qtAlign(Align a)
{
    switch (a)
    {
    case Align::Center: return Qt::AlignHCenter;
    case Align::End: return Qt::AlignRight;
    case Align::Start: break;
    }
    return Qt::AlignLeft;
}

// Running under WSL, where WSLg shows windows on the Windows desktop: its
// Wayland can't place windows, and the screen it reports includes the
// Windows taskbar.
bool underWsl()
{
    static const bool wsl = []
    {
        if (std::getenv("WSL_DISTRO_NAME")) return true;
        char buf[256] = {};
        if (FILE* f = std::fopen("/proc/sys/kernel/osrelease", "r"))
        {
            const size_t n = std::fread(buf, 1, sizeof buf - 1, f);
            buf[n] = 0;
            std::fclose(f);
        }
        return std::strstr(buf, "microsoft") || std::strstr(buf, "Microsoft");
    }();
    return wsl;
}

// What WSLg leaves out of the work area: Windows' taskbar along the bottom,
// at Windows 11's height, which covers Windows 10's.
RectF withoutWindowsTaskbar(RectF area)
{
    if (underWsl()) area.h = std::max(0.f, area.h - 48);
    return area;
}

// The font for compact lists and text views: 90% of `f`, rounded to whole
// pixels so glyphs sit on the pixel grid.
QFont compactFont(const QFont& f)
{
    QFont c = f;
    c.setPixelSize(std::max(1, int(std::lround(QFontInfo(f).pixelSize() * 0.9))));
    return c;
}

QFont styledFont(QFont f, const TextStyle& s)
{
    if (s.monospace)
    {
        QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        if (f.pixelSize() > 0)
            mono.setPixelSize(f.pixelSize());
        else
            mono.setPointSizeF(f.pointSizeF());
        f = mono;
    }
    if (s.size > 0) f.setPointSizeF(s.size);
    if (s.bold) f.setBold(true);
    if (s.italic) f.setItalic(true);
    return f;
}

QTextCharFormat charFormat(const QWidget* w, const TextStyle& s)
{
    QTextCharFormat f;
    if (s.fg) f.setForeground(qcolor(resolve(w, *s.fg)));
    if (s.bg) f.setBackground(qcolor(resolve(w, *s.bg)));
    if (s.bold) f.setFontWeight(QFont::Bold);
    if (s.italic) f.setFontItalic(true);
    if (s.monospace)
    {
        f.setFontFamilies(
            QStringList{QFontDatabase::systemFont(QFontDatabase::FixedFont).family()});
        f.setFontFixedPitch(true);
    }
    if (s.size > 0) f.setFontPointSize(s.size);
    return f;
}

// Byte offsets into a UTF-8 string and UTF-16 positions in its QString.
qsizetype utf16At(const std::string& s, size_t byte)
{
    return qs(s.substr(0, std::min(byte, s.size()))).size();
}
size_t utf8At(const QString& s, qsizetype pos)
{
    return size_t(QStringView(s).left(std::clamp<qsizetype>(pos, 0, s.size())).toUtf8().size());
}

// Selected rows in a light tint of the accent, with normal text: on light
// themes the accent's hue at Explorer's lightness, as on win32 (#0078D7
// becomes #CCE8FF); on dark ones a mix into the base colour, as on GTK.
struct SoftColors
{
    QColor selected, hover, hoverSelected;
};

SoftColors softColors(const QWidget* w)
{
    const QPalette pal = w->palette();
    const QColor base = pal.color(QPalette::Active, QPalette::Base);
    const QColor accent = pal.color(QPalette::Active, QPalette::Highlight);
    if (base.lightnessF() >= 0.5)
    {
        auto lit = [&](float l)
        { return QColor::fromHslF(std::max(0.f, accent.hslHueF()), accent.hslSaturationF(), l); };
        return {lit(0.90f), lit(0.95f), lit(0.88f)};
    }
    auto mix = [&](float t)
    {
        return QColor::fromRgbF(base.redF() + (accent.redF() - base.redF()) * t,
                                base.greenF() + (accent.greenF() - base.greenF()) * t,
                                base.blueF() + (accent.blueF() - base.blueF()) * t);
    };
    return {mix(0.38f), mix(0.16f), mix(0.45f)};
}

// List autoscroll: px/s for the pointer `offset` px above (negative) or
// below the anchor. A dead zone, then speed growing faster than the
// distance, so a nudge creeps and a long reach races. (As gtk_common's.)
float autoScrollSpeed(float offset)
{
    const float dead = 8, d = std::abs(offset) - dead;
    if (d <= 0) return 0;
    return (offset < 0 ? -1.f : 1.f) * (d * 6 + d * d * 0.05f);
}

// The autoscroll anchor: a ring with up and down arrows, centred at (x, y).
void paintAutoScrollAnchor(Painter& p, float x, float y)
{
    p.circle({x, y}, 11, Paint{ThemeColor::Base}, Paint{ThemeColor::Mid});
    p.polygon({{x - 4, y - 3}, {x + 4, y - 3}, {x, y - 8}}, Paint{ThemeColor::Text}, std::nullopt,
              1);
    p.polygon({{x - 4, y + 3}, {x + 4, y + 3}, {x, y + 8}}, Paint{ThemeColor::Text}, std::nullopt,
              1);
    p.circle({x, y}, 1.5f, Paint{ThemeColor::Text}, std::nullopt);
}

// A file filter's extension for saving: the first glob's, if it's a plain
// "*.ext" ("*.patch;*.diff" -> "patch"; "*" -> none).
QString defaultExtension(const std::string& pattern)
{
    const std::string first = pattern.substr(0, pattern.find(';'));
    if (!first.starts_with("*.") || first.find_first_of("*?[", 2) != std::string::npos) return {};
    return qs(first.substr(2));
}

Key keyFor(int k)
{
    switch (k)
    {
    case Qt::Key_Escape: return Key::Escape;
    case Qt::Key_Return:
    case Qt::Key_Enter: return Key::Enter;
    case Qt::Key_Tab:
    case Qt::Key_Backtab: return Key::Tab;
    case Qt::Key_Backspace: return Key::Backspace;
    case Qt::Key_Delete: return Key::Delete;
    case Qt::Key_Insert: return Key::Insert;
    case Qt::Key_Space: return Key::Space;
    case Qt::Key_Left: return Key::Left;
    case Qt::Key_Right: return Key::Right;
    case Qt::Key_Up: return Key::Up;
    case Qt::Key_Down: return Key::Down;
    case Qt::Key_Home: return Key::Home;
    case Qt::Key_End: return Key::End;
    case Qt::Key_PageUp: return Key::PageUp;
    case Qt::Key_PageDown: return Key::PageDown;
    default: break;
    }
    if (k >= Qt::Key_F1 && k <= Qt::Key_F12) return Key(int(Key::F1) + (k - Qt::Key_F1));
    return k > 0x20 && k < 0x01000000 ? Key::Character : Key::Other;
}

unsigned modifiersOf(Qt::KeyboardModifiers m)
{
    unsigned out = 0;
    if (m & Qt::ShiftModifier) out |= ModShift;
    if (m & Qt::ControlModifier) out |= ModCtrl;
    if (m & Qt::AltModifier) out |= ModAlt;
    if (m & Qt::MetaModifier) out |= ModMeta;
    return out;
}

// Runs `fn` for each event `target` receives, ahead of the target; true
// consumes the event. Lives as long as the target, or until disarmed.
class EventHook : public QObject
{
public:
    using Fn = std::function<bool(QEvent*)>;
    EventHook(QObject* target, Fn fn)
        : QObject(target)
        , fn_(std::move(fn))
    {
        target->installEventFilter(this);
    }
    bool eventFilter(QObject*, QEvent* e) override { return fn_ && fn_(e); }
    void disarm() { fn_ = nullptr; }

private:
    Fn fn_;
};

void hook(QObject* target, EventHook::Fn fn)
{
    new EventHook(target, std::move(fn));
}

// ---------------------------------------------------------------- icons

// The desktop's icon for a kind of file: the name's MIME type (a trailing
// '/' is a folder) and the icon theme's icon for it. Cached per extension.
QIcon fileTypeIcon(const std::string& name)
{
    static std::unordered_map<std::string, QIcon> cache;
    const bool folder = !name.empty() && name.back() == '/';
    const size_t slash = name.find_last_of('/', folder ? name.size() - 2 : std::string::npos);
    const std::string base = name.substr(slash == std::string::npos ? 0 : slash + 1);
    const size_t dot = base.rfind('.');
    // Extensionless names (Makefile, README) go by the whole name.
    const std::string key = folder                                 ? "/"
                            : dot == std::string::npos || dot == 0 ? base
                                                                   : base.substr(dot);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    QIcon icon;
    if (folder)
        icon = QIcon::fromTheme("folder");
    else
    {
        static const QMimeDatabase db;
        const QMimeType type = db.mimeTypeForFile(qs(base), QMimeDatabase::MatchExtension);
        icon =
            QIcon::fromTheme(type.iconName(), QIcon::fromTheme(type.genericIconName(),
                                                               QIcon::fromTheme("text-x-generic")));
    }
    return cache[key] = icon;
}

QIcon themeIcon(const std::string& name)
{
    static std::unordered_map<std::string, QIcon> cache;
    auto it = cache.find(name);
    if (it != cache.end()) return it->second;
    return cache[name] = QIcon::hasThemeIcon(qs(name)) ? QIcon::fromTheme(qs(name)) : QIcon();
}

// An app-drawn icon, painted at `w`'s device scale in its theme colours.
QIcon drawnIcon(const std::function<void(Painter&, SizeF)>& draw, int size, const QWidget* w)
{
    const qreal dpr = w ? w->devicePixelRatioF() : qApp->devicePixelRatio();
    QPixmap pm(QSize(size, size) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    {
        QPainter p(&pm);
        QtPainter painter(p, w);
        draw(painter, {float(size), float(size)});
    }
    return QIcon(pm);
}

// A named icon the theme has, else the file type's, else the drawing.
QIcon toQIcon(const Icon& icon, int size, const QWidget* w)
{
    if (!icon.name.empty())
    {
        if (QIcon named = themeIcon(icon.name); !named.isNull()) return named;
        // The theme may lack it (WSL's has no view-fullscreen or edit-copy).
        // Lists ask per cell per paint, so each is drawn once.
        static std::map<std::tuple<std::string, int, qreal>, QIcon> drawn;
        const qreal dpr = w ? w->devicePixelRatioF() : qApp->devicePixelRatio();
        auto [it, added] = drawn.try_emplace({icon.name, size, dpr});
        if (added)
        {
            bool known = false;
            QIcon i = drawnIcon([&](Painter& p, SizeF s) { known = common::drawBuiltinIcon(icon.name, p, s); },
                                size, w);
            if (known) it->second = i;
        }
        if (!it->second.isNull()) return it->second;
    }
    if (!icon.file.empty()) return fileTypeIcon(icon.file);
    if (icon.draw) return drawnIcon(icon.draw, size, w);
    return {};
}

// ---------------------------------------------------------------- menus

// Appends `items` to `menu`. Each action keeps its own copy of what it runs,
// so the Menu needn't outlive the QMenu. Shortcuts are display hints (the
// window's onKey handles the keys), so they only work inside the menu.
void fillMenu(QMenu* menu, const Menu& items)
{
    QActionGroup* radios = nullptr;
    for (const MenuItem& item : items)
    {
        if (!item.radio) radios = nullptr;
        if (item.isSeparator())
        {
            menu->addSeparator();
            continue;
        }
        if (!item.children.empty())
        {
            QMenu* sub = menu->addMenu(mnemonic(item.label));
            sub->setEnabled(item.enabled);
            fillMenu(sub, item.children);
            continue;
        }
        QAction* a = menu->addAction(mnemonic(item.label));
        a->setEnabled(item.enabled && item.action);
        if (item.checked)
        {
            a->setCheckable(true);
            a->setChecked(*item.checked);
            if (item.radio)
            {
                if (!radios) radios = new QActionGroup(menu);
                radios->addAction(a);
            }
        }
        if (!item.shortcut.empty())
        {
            a->setShortcut(QKeySequence(qs(item.shortcut), QKeySequence::PortableText));
            a->setShortcutContext(Qt::WidgetShortcut);
            a->setShortcutVisibleInContextMenu(true);
        }
        if (item.action) QObject::connect(a, &QAction::triggered, a, [f = item.action] { f(); });
    }
}

// A context menu at `global`, deleted once it closes (after its action ran:
// Qt hides a menu before triggering the chosen action).
void popupMenu(Menu items, QWidget* parent, QPoint global)
{
    if (items.empty()) return;
    auto* m = new QMenu(parent);
    fillMenu(m, items);
    QObject::connect(m, &QMenu::aboutToHide, m, &QObject::deleteLater);
    m->popup(global);
}

// ---------------------------------------------------------------- widgets

class Part
{
public:
    virtual ~Part() = default;
};

// A widget's anyui interface over its Qt widget. The window deletes its Qt
// widgets later (deleteLater) than these, so Qt-side code that calls back
// into a part checks that it's still there.
template <class Base>
class W : public Base, public Part
{
public:
    explicit W(QWidget* w)
        : w_(w)
    {
    }
    void setEnabled(bool e) override { w_->setEnabled(e); }
    void setVisible(bool v) override
    {
        // In a toolbar, its action shows and hides it.
        if (auto* action = qobject_cast<QAction*>(w_->property("anyui-toolbar-action").value<QObject*>()))
            return action->setVisible(v);
        // Not yet in its parent, showing it would open it as a window; it
        // shows with its parent anyway.
        if (v && !w_->parentWidget()) return;
        w_->setVisible(v);
    }
    void setTooltip(const std::string& t) override { w_->setToolTip(qs(t)); }
    void focus() override { w_->setFocus(Qt::OtherFocusReason); }
    QWidget* widget() const { return w_; }

protected:
    QWidget* w_;
};

class WPlain : public W<anyui::Widget>
{
public:
    using W::W;
};

// A label that can cut its text short with "..." (Label::ellipsize):
// then it asks for no minimum width, and shows as much as fits.
class ElidedLabel : public QLabel
{
public:
    explicit ElidedLabel(bool elide)
        : elide_(elide)
    {
        setTextFormat(Qt::PlainText);
    }
    void setFullText(const QString& t)
    {
        full_ = t;
        refit();
        updateGeometry();
    }
    QSize sizeHint() const override
    {
        if (!elide_) return QLabel::sizeHint();
        return {fontMetrics().horizontalAdvance(full_) + 2 * margin() + 4,
                QLabel::sizeHint().height()};
    }
    QSize minimumSizeHint() const override
    {
        const QSize s = QLabel::minimumSizeHint();
        return elide_ ? QSize(0, s.height()) : s;
    }

protected:
    void resizeEvent(QResizeEvent* e) override
    {
        QLabel::resizeEvent(e);
        refit();
    }

private:
    void refit()
    {
        QLabel::setText(elide_ ? fontMetrics().elidedText(full_, Qt::ElideRight,
                                                          std::max(0, contentsRect().width()))
                               : full_);
    }
    QString full_;
    bool elide_;
};

class WLabel : public W<anyui::Label>
{
public:
    explicit WLabel(const spec::Label& s)
        : W(new ElidedLabel(s.ellipsize))
    {
        if (s.selectable)
        {
            label()->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                             Qt::TextSelectableByKeyboard);
            label()->setCursor(Qt::IBeamCursor);
        }
        label()->setFullText(qs(s.text));
    }
    void setText(const std::string& t) override { label()->setFullText(qs(t)); }

private:
    ElidedLabel* label() const { return static_cast<ElidedLabel*>(w_); }
};

class WButton : public W<anyui::Button>
{
public:
    explicit WButton(QPushButton* b)
        : W(b)
    {
        QObject::connect(b, &QPushButton::clicked, b,
                         [this]
                         {
                             if (click_) click_();
                         });
    }
    void setText(const std::string& t) override { button()->setText(mnemonic(t)); }
    void setIcon(const Icon& icon) override { button()->setIcon(toQIcon(icon, 16, w_)); }
    void onClick(std::function<void()> f) override { click_ = std::move(f); }

private:
    QPushButton* button() const { return static_cast<QPushButton*>(w_); }
    std::function<void()> click_;
};

// A click on the mixed state lands on checked (TortoiseGit's tri-state All
// Branches toggles between on and off); only the app sets it mixed.
class TriCheckBox : public QCheckBox
{
public:
    using QCheckBox::QCheckBox;

protected:
    void nextCheckState() override
    {
        setTristate(false);
        setCheckState(checkState() == Qt::Checked ? Qt::Unchecked : Qt::Checked);
    }
};

class WCheck : public W<anyui::Check>
{
public:
    explicit WCheck(const spec::Check& s)
        : W(new TriCheckBox(mnemonic(s.text)))
    {
        setState(s.state);
        // clicked: the user's changes only.
        QObject::connect(box(), &QCheckBox::clicked, box(),
                         [this]
                         {
                             if (toggled_) toggled_(box()->isChecked());
                         });
    }
    bool checked() const override { return state() == CheckState::Checked; }
    void setChecked(bool v) override { setState(v ? CheckState::Checked : CheckState::Unchecked); }
    CheckState state() const override
    {
        switch (box()->checkState())
        {
        case Qt::Checked: return CheckState::Checked;
        case Qt::PartiallyChecked: return CheckState::Indeterminate;
        case Qt::Unchecked: break;
        }
        return CheckState::Unchecked;
    }
    void setState(CheckState s) override
    {
        box()->setCheckState(s == CheckState::Indeterminate ? Qt::PartiallyChecked
                             : s == CheckState::Checked     ? Qt::Checked
                                                            : Qt::Unchecked);
    }
    void onToggled(std::function<void(bool)> f) override { toggled_ = std::move(f); }

private:
    QCheckBox* box() const { return static_cast<QCheckBox*>(w_); }
    std::function<void(bool)> toggled_;
};

// A push button with a drop-down menu, rebuilt each time it opens. Pushed
// is drawn as checked.
class WMenuButton : public W<anyui::MenuButton>
{
public:
    explicit WMenuButton(const spec::MenuButton& s)
        : W(new QPushButton(mnemonic(s.text)))
    {
        auto* b = button();
        menu_ = new QMenu(b);
        b->setMenu(menu_);
        b->setCheckable(true);
        b->setChecked(s.pushed);
        if (s.icon) setIcon(s.icon);
        QObject::connect(menu_, &QMenu::aboutToShow, menu_,
                         [this]
                         {
                             menu_->clear();
                             if (menuFn_) fillMenu(menu_, menuFn_());
                         });
    }
    void setText(const std::string& t) override { button()->setText(mnemonic(t)); }
    void setIcon(const Icon& icon) override { button()->setIcon(toQIcon(icon, 16, w_)); }
    void setPushed(bool v) override { button()->setChecked(v); }
    void onMenu(std::function<Menu()> f) override { menuFn_ = std::move(f); }

private:
    QPushButton* button() const { return static_cast<QPushButton*>(w_); }
    QMenu* menu_;
    std::function<Menu()> menuFn_;
};

class WProgressBar : public W<anyui::ProgressBar>
{
public:
    WProgressBar()
        : W(new QProgressBar)
    {
        bar()->setRange(0, 1000);
        bar()->setValue(0);
        bar()->setTextVisible(false);
    }
    void setFraction(float f) override
    {
        if (bar()->maximum() == 0) bar()->setRange(0, 1000);
        bar()->setValue(int(std::clamp(f, 0.f, 1.f) * 1000));
    }
    // Qt animates a busy bar by itself.
    void pulse() override { bar()->setRange(0, 0); }

private:
    QProgressBar* bar() const { return static_cast<QProgressBar*>(w_); }
};

class WDatePicker : public W<anyui::DatePicker>
{
public:
    explicit WDatePicker(const spec::DatePicker& s)
        : W(new QDateEdit)
    {
        edit()->setCalendarPopup(true);
        setDate(s.date);
        QObject::connect(edit(), &QDateEdit::dateChanged, edit(),
                         [this](QDate d)
                         {
                             if (!quiet_ && changed_) changed_({d.year(), d.month(), d.day()});
                         });
    }
    Date date() const override
    {
        const QDate d = edit()->date();
        return {d.year(), d.month(), d.day()};
    }
    void setDate(Date d) override
    {
        quiet_ = true;
        edit()->setDate(QDate(d.year, d.month, d.day));
        quiet_ = false;
    }
    void onChanged(std::function<void(Date)> f) override { changed_ = std::move(f); }

private:
    QDateEdit* edit() const { return static_cast<QDateEdit*>(w_); }
    std::function<void(Date)> changed_;
    bool quiet_ = false;
};

// Enter in a line edit presses the dialog's default button (OK), as on
// Windows, unless the app handles Enter itself (a find box): then the
// key stops here.
void handleEnter(QLineEdit* edit, const std::function<void()>* activate)
{
    hook(edit,
         [activate](QEvent* e)
         {
             if (e->type() != QEvent::KeyPress || !*activate) return false;
             const int key = static_cast<QKeyEvent*>(e)->key();
             if (key != Qt::Key_Return && key != Qt::Key_Enter) return false;
             (*activate)();
             return true;
         });
}

class WEntry : public W<anyui::Entry>
{
public:
    explicit WEntry(const spec::Entry& s)
        : W(new QLineEdit(qs(s.text)))
    {
        edit()->setPlaceholderText(qs(s.placeholder));
        QObject::connect(edit(), &QLineEdit::textChanged, edit(),
                         [this](const QString& t)
                         {
                             if (changed_) changed_(t.toStdString());
                         });
        handleEnter(edit(), &activate_);
    }
    std::string text() const override { return edit()->text().toStdString(); }
    void setText(const std::string& t) override { edit()->setText(qs(t)); }
    void setPlaceholder(const std::string& t) override { edit()->setPlaceholderText(qs(t)); }
    void onChanged(std::function<void(const std::string&)> f) override { changed_ = std::move(f); }
    void onActivate(std::function<void()> f) override { activate_ = std::move(f); }

private:
    QLineEdit* edit() const { return static_cast<QLineEdit*>(w_); }
    std::function<void(const std::string&)> changed_;
    std::function<void()> activate_;
};

class WSelect : public W<anyui::Select>
{
public:
    explicit WSelect(const spec::Select& s)
        : W(new QComboBox)
    {
        setItems(s.items);
        setCurrent(s.current);
        QObject::connect(combo(), &QComboBox::currentIndexChanged, combo(),
                         [this](int i)
                         {
                             if (!quiet_ && changed_) changed_(i);
                         });
    }
    void setItems(std::vector<std::string> items) override
    {
        quiet_ = true;
        combo()->clear();
        for (auto& i : items) combo()->addItem(qs(i));
        quiet_ = false;
    }
    int current() const override { return combo()->currentIndex(); }
    void setCurrent(int i) override { combo()->setCurrentIndex(i); }
    void onChanged(std::function<void(int)> f) override { changed_ = std::move(f); }

private:
    QComboBox* combo() const { return static_cast<QComboBox*>(w_); }
    std::function<void(int)> changed_;
    bool quiet_ = false;
};

class WCombo : public W<anyui::Combo>
{
public:
    explicit WCombo(const spec::Combo& s)
        : W(new QComboBox)
    {
        combo()->setEditable(true);
        combo()->setInsertPolicy(QComboBox::NoInsert);
        setItems(s.items);
        setText(s.text);
        QObject::connect(combo()->lineEdit(), &QLineEdit::textChanged, combo(),
                         [this](const QString& t)
                         {
                             if (changed_) changed_(t.toStdString());
                         });
        handleEnter(combo()->lineEdit(), &activate_);
    }
    void setItems(std::vector<std::string> items) override
    {
        const QString text = combo()->currentText();
        combo()->clear();
        for (auto& i : items) combo()->addItem(qs(i));
        combo()->setEditText(text);
    }
    std::string text() const override { return combo()->currentText().toStdString(); }
    void setText(const std::string& t) override { combo()->setEditText(qs(t)); }
    void onChanged(std::function<void(const std::string&)> f) override { changed_ = std::move(f); }
    void onActivate(std::function<void()> f) override { activate_ = std::move(f); }

private:
    QComboBox* combo() const { return static_cast<QComboBox*>(w_); }
    std::function<void(const std::string&)> changed_;
    std::function<void()> activate_;
};

// QPlainTextEdit: lays out lazily, so long diffs open at once. Styled spans
// go in as one pass of inserts, each run with its format.
class WTextView : public W<anyui::TextView>
{
public:
    explicit WTextView(const spec::TextView& s)
        : W(new QPlainTextEdit)
    {
        auto* e = edit();
        QFont f = s.compact ? compactFont(e->font()) : e->font();
        if (s.monospace) f = styledFont(f, TextStyle{.monospace = true});
        e->setFont(f);
        e->setLineWrapMode(s.wrap ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
        e->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        setReadOnly(s.readOnly);
        setText(s.text);
        QObject::connect(e, &QPlainTextEdit::textChanged, e,
                         [this]
                         {
                             if (changed_) changed_();
                         });
    }

    std::string text() const override { return edit()->toPlainText().toStdString(); }

    void setText(const std::string& t, std::vector<Span> spans = {}) override
    {
        auto* doc = edit()->document();
        const bool undo = doc->isUndoRedoEnabled();
        doc->setUndoRedoEnabled(false);
        if (spans.empty())
            edit()->setPlainText(qs(t));
        else
        {
            edit()->clear();
            std::ranges::stable_sort(spans, {}, &Span::begin);
            QTextCursor c(doc);
            c.beginEditBlock();
            size_t at = 0;
            for (const Span& sp : spans)
            {
                const size_t b = std::clamp(sp.begin, at, t.size()),
                             e = std::clamp(sp.end, b, t.size());
                if (b > at) c.insertText(qs(t.substr(at, b - at)), QTextCharFormat());
                if (e > b) c.insertText(qs(t.substr(b, e - b)), charFormat(w_, sp.style));
                at = std::max(at, e);
            }
            if (at < t.size()) c.insertText(qs(t.substr(at)), QTextCharFormat());
            c.endEditBlock();
            edit()->moveCursor(QTextCursor::Start);
        }
        doc->setUndoRedoEnabled(undo);
    }

    void append(const std::string& t, const TextStyle& style) override
    {
        QTextCursor c(edit()->document());
        c.movePosition(QTextCursor::End);
        c.insertText(qs(t), charFormat(w_, style));
    }

    void setReadOnly(bool ro) override
    {
        edit()->setReadOnly(ro);
        // Read-only text is still selectable from the keyboard (Ctrl+A,
        // Shift+arrows), without showing a caret.
        if (ro)
            edit()->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                            Qt::TextSelectableByKeyboard);
        edit()->setCursorWidth(ro ? 0 : 1);
    }

    void onChanged(std::function<void()> f) override { changed_ = std::move(f); }

    // Document positions count UTF-16 units (and one per line break), as
    // toPlainText() does; the API counts UTF-8 bytes.
    std::pair<size_t, size_t> selection() const override
    {
        const QTextCursor c = edit()->textCursor();
        const QString t = edit()->toPlainText();
        return {utf8At(t, c.selectionStart()), utf8At(t, c.selectionEnd())};
    }

    void select(size_t begin, size_t end) override
    {
        const std::string t = text();
        QTextCursor c(edit()->document());
        c.setPosition(int(utf16At(t, begin)));
        c.setPosition(int(utf16At(t, end)), QTextCursor::KeepAnchor);
        edit()->setTextCursor(c);
        edit()->ensureCursorVisible();
    }

private:
    QPlainTextEdit* edit() const { return static_cast<QPlainTextEdit*>(w_); }
    std::function<void()> changed_;
};

// ---------------------------------------------------------------- containers

// Splitter panes: the first layout follows stretch factors (panes without
// stretch at their natural size), or the sizes given before it; after
// that, spare space goes to the stretching panes only.
class WSplitter : public W<anyui::Splitter>
{
public:
    WSplitter(QSplitter* s, std::vector<int> stretch)
        : W(s)
        , stretch_(std::move(stretch))
    {
        // At Show the splitter has had its first size (a top level's layout
        // runs before it shows); a later first size (a page shown later)
        // gets its layout once the splitter has dealt with the resize.
        hook(s,
             [this](QEvent* e)
             {
                 if (laidOut_ || length() <= 1) return false;
                 if (e->type() == QEvent::Show)
                 {
                     laidOut_ = true;
                     firstLayout();
                 }
                 else if (e->type() == QEvent::Resize && splitter()->isVisible())
                 {
                     laidOut_ = true;
                     QTimer::singleShot(0, splitter(), [this] { firstLayout(); });
                 }
                 return false;
             });
    }

    std::vector<float> sizes() const override
    {
        std::vector<float> out;
        for (int s : splitter()->sizes()) out.push_back(float(s));
        return out;
    }

    // Each divider goes at sizes[i] (0: left as it is); the last pane gets
    // the rest.
    void setSizes(const std::vector<float>& s) override
    {
        if (laidOut_)
            apply(s);
        else
            pending_ = s;
    }

private:
    QSplitter* splitter() const { return static_cast<QSplitter*>(w_); }
    int length() const
    {
        return splitter()->orientation() == Qt::Vertical ? w_->height() : w_->width();
    }
    int paneLength() const
    {
        return length() - splitter()->handleWidth() * std::max(0, splitter()->count() - 1);
    }

    void firstLayout()
    {
        if (pending_)
            apply(*pending_);
        else
            initialSplit();
    }

    void initialSplit()
    {
        const int n = splitter()->count();
        int fixed = 0, stretchSum = 0;
        QList<int> sizes(n, 0);
        for (int i = 0; i < n && size_t(i) < stretch_.size(); ++i)
        {
            if (stretch_[size_t(i)] > 0)
            {
                stretchSum += stretch_[size_t(i)];
                continue;
            }
            const QSize hint = splitter()->widget(i)->sizeHint();
            sizes[i] = splitter()->orientation() == Qt::Vertical ? hint.height() : hint.width();
            fixed += sizes[i];
        }
        const int avail = paneLength() - fixed;
        if (avail <= 0 || stretchSum == 0) return;
        for (int i = 0; i < n && size_t(i) < stretch_.size(); ++i)
            if (stretch_[size_t(i)] > 0) sizes[i] = avail * stretch_[size_t(i)] / stretchSum;
        splitter()->setSizes(sizes);
    }

    void apply(const std::vector<float>& s)
    {
        QList<int> sizes = splitter()->sizes();
        const int n = int(sizes.size());
        int used = 0;
        for (int i = 0; i + 1 < n; ++i)
        {
            if (size_t(i) < s.size() && s[size_t(i)] > 0) sizes[i] = int(s[size_t(i)]);
            used += sizes[i];
        }
        if (n) sizes[n - 1] = std::max(0, paneLength() - used);
        splitter()->setSizes(sizes);
    }

    std::vector<int> stretch_;
    std::optional<std::vector<float>> pending_;
    bool laidOut_ = false;
};

// Tab pages built the first time they're shown (layout::Lazy): each starts
// as an empty page with a layout, filled then.
struct LazyPages
{
    std::vector<std::function<Node()>> lazy;
    std::vector<QWidget*> pages;
    std::function<QWidget*(const Node&)> build;

    void ensureBuilt(int i)
    {
        if (i < 0 || size_t(i) >= lazy.size() || !lazy[size_t(i)]) return;
        const auto make = std::move(lazy[size_t(i)]);
        lazy[size_t(i)] = nullptr;
        pages[size_t(i)]->layout()->addWidget(build(make()));
    }
};

class WTabs : public W<anyui::Tabs>
{
public:
    explicit WTabs(QTabWidget* t)
        : W(t)
    {
        QObject::connect(t, &QTabWidget::currentChanged, t,
                         [this](int i)
                         {
                             lazy_.ensureBuilt(i);
                             if (changed_) changed_(i);
                         });
    }
    int current() const override { return tabs()->currentIndex(); }
    void setCurrent(int i) override
    {
        tabs()->setCurrentIndex(i);
        lazy_.ensureBuilt(i);
    }
    void onChanged(std::function<void(int)> f) override { changed_ = std::move(f); }
    void setLazy(LazyPages pages)
    {
        lazy_ = std::move(pages);
        lazy_.ensureBuilt(current());
    }

private:
    QTabWidget* tabs() const { return static_cast<QTabWidget*>(w_); }
    std::function<void(int)> changed_;
    LazyPages lazy_;
};

// Tabs as a list of page names down the left, the page beside it.
class WSideTabs : public W<anyui::Tabs>
{
public:
    WSideTabs(QWidget* box, QListWidget* list, QStackedWidget* stack)
        : W(box)
        , list_(list)
        , stack_(stack)
    {
        QObject::connect(list_, &QListWidget::currentRowChanged, list_,
                         [this](int i)
                         {
                             if (i < 0) return;
                             lazy_.ensureBuilt(i);
                             stack_->setCurrentIndex(i);
                             if (changed_) changed_(i);
                         });
    }
    int current() const override { return std::max(0, list_->currentRow()); }
    void setCurrent(int i) override
    {
        list_->setCurrentRow(i);
        lazy_.ensureBuilt(i);
    }
    void onChanged(std::function<void(int)> f) override { changed_ = std::move(f); }
    void setLazy(LazyPages pages)
    {
        lazy_ = std::move(pages);
        lazy_.ensureBuilt(current());
    }

private:
    QListWidget* list_;
    QStackedWidget* stack_;
    std::function<void(int)> changed_;
    LazyPages lazy_;
};

// ---------------------------------------------------------------- toolbar, values, canvas

class WToolButton : public W<anyui::ToolButton>
{
public:
    explicit WToolButton(const spec::ToolButton& s)
        : W(new QToolButton)
    {
        auto* b = button();
        b->setAutoRaise(true);
        b->setIconSize(QSize(16, 16));
        b->setAccessibleName(qs(s.label));
        if (!s.label.empty()) b->setToolTip(qs(s.label));
        b->setCheckable(s.toggle);
        b->setChecked(s.checked);
        if (s.icon) setIcon(s.icon);
        QObject::connect(b, &QToolButton::clicked, b, [this] { if (click_) click_(); });
        QObject::connect(b, &QToolButton::toggled, b,
                         [this](bool on)
                         {
                             if (!quiet_ && toggled_) toggled_(on);
                         });
    }
    void setIcon(const Icon& icon) override { button()->setIcon(toQIcon(icon, 16, w_)); }
    bool checked() const override { return button()->isChecked(); }
    void setChecked(bool c) override
    {
        quiet_ = true;  // set from code: no onToggled
        button()->setChecked(c);
        quiet_ = false;
    }
    void onClick(std::function<void()> f) override { click_ = std::move(f); }
    void onToggled(std::function<void(bool)> f) override { toggled_ = std::move(f); }

private:
    QToolButton* button() const { return static_cast<QToolButton*>(w_); }
    std::function<void()> click_;
    std::function<void(bool)> toggled_;
    bool quiet_ = false;
};

// A slider over doubles: QSlider counts whole steps from `min`, and an
// optional label shows the value.
class WSlider : public W<anyui::Slider>
{
public:
    explicit WSlider(const spec::Slider& s)
        : W(new QWidget)
        , slider_(new QSlider(Qt::Horizontal))
        , min_(s.min)
        , max_(s.max)
        , step_(s.step > 0 ? s.step : (s.max - s.min) / 1000)
        , digits_(s.digits)
    {
        auto* row = new QHBoxLayout(w_);
        row->setContentsMargins(0, 0, 0, 0);
        row->addWidget(slider_, 1);
        if (s.showValue)
        {
            label_ = new QLabel;
            label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            label_->setMinimumWidth(label_->fontMetrics().horizontalAdvance(QString(digits_ + 5, QChar('0'))));
            row->addWidget(label_);
        }
        slider_->setFocusPolicy(Qt::StrongFocus);
        slider_->setSingleStep(1);
        slider_->setPageStep(std::max(1, int(std::lround(s.page / step_))));
        if (s.marks.size() >= 2)
        {
            slider_->setTickPosition(QSlider::TicksBelow);
            slider_->setTickInterval(std::max(1, int(std::lround((s.marks[1].first - s.marks[0].first) / step_))));
        }
        applyRange();
        setValue(s.value);
        QObject::connect(slider_, &QSlider::valueChanged, slider_,
                         [this](int)
                         {
                             showValue();
                             if (!quiet_ && changed_) changed_(value());
                         });
    }
    double value() const override { return min_ + slider_->value() * step_; }
    void setValue(double v) override
    {
        quiet_ = true;
        slider_->setValue(int(std::lround((std::clamp(v, min_, max_) - min_) / step_)));
        quiet_ = false;
        showValue();
    }
    void setRange(double mn, double mx) override
    {
        const double v = value();
        min_ = mn;
        max_ = mx;
        applyRange();
        setValue(v);
    }
    void onChanged(std::function<void(double)> f) override { changed_ = std::move(f); }

private:
    void applyRange()
    {
        quiet_ = true;
        slider_->setRange(0, std::max(0, int(std::lround((max_ - min_) / step_))));
        quiet_ = false;
    }
    void showValue()
    {
        if (label_) label_->setText(QString::number(value(), 'f', digits_));
    }

    QSlider* slider_;
    QLabel* label_ = nullptr;
    double min_, max_, step_;
    int digits_;
    bool quiet_ = false;
    std::function<void(double)> changed_;
};

class WSpin : public W<anyui::Spin>
{
public:
    explicit WSpin(const spec::Spin& s)
        : W(new QDoubleSpinBox)
    {
        auto* b = box();
        b->setDecimals(s.digits);
        b->setRange(s.min, s.max);
        b->setSingleStep(s.step);
        b->setValue(s.value);
        b->setAccelerated(true);
        QObject::connect(b, &QDoubleSpinBox::valueChanged, b,
                         [this](double v)
                         {
                             if (!quiet_ && changed_) changed_(v);
                         });
    }
    double value() const override { return box()->value(); }
    void setValue(double v) override
    {
        quiet_ = true;
        box()->setValue(v);
        quiet_ = false;
    }
    void setRange(double mn, double mx) override
    {
        quiet_ = true;
        box()->setRange(mn, mx);
        quiet_ = false;
    }
    void onChanged(std::function<void(double)> f) override { changed_ = std::move(f); }

private:
    QDoubleSpinBox* box() const { return static_cast<QDoubleSpinBox*>(w_); }
    bool quiet_ = false;
    std::function<void(double)> changed_;
};

// A button showing the colour; clicking opens Qt's colour chooser.
class WColorButton : public W<anyui::ColorButton>
{
public:
    explicit WColorButton(const spec::ColorButton& s)
        : W(new QPushButton)
        , color_(s.color)
        , alpha_(s.alpha)
        , title_(qs(s.title))
    {
        auto* b = static_cast<QPushButton*>(w_);
        b->setIconSize(QSize(36, 14));
        swatch();
        QObject::connect(b, &QPushButton::clicked, b,
                         [this]
                         {
                             QColorDialog::ColorDialogOptions opts;
                             if (alpha_) opts |= QColorDialog::ShowAlphaChannel;
                             const QColor c = QColorDialog::getColor(qcolor(color_), w_->window(), title_, opts);
                             if (!c.isValid()) return;  // cancelled
                             color_ = fromQColor(c);
                             swatch();
                             if (changed_) changed_(color_);
                         });
    }
    Color color() const override { return color_; }
    void setColor(Color c) override
    {
        color_ = c;
        swatch();
    }
    void onChanged(std::function<void(Color)> f) override { changed_ = std::move(f); }

private:
    void swatch()
    {
        auto* b = static_cast<QPushButton*>(w_);
        const qreal dpr = b->devicePixelRatioF();
        QPixmap pm(QSize(36, 14) * dpr);
        pm.setDevicePixelRatio(dpr);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setPen(b->palette().color(QPalette::Mid));
        p.setBrush(qcolor(color_));
        p.drawRect(QRectF(0.5, 0.5, 35, 13));
        p.end();
        b->setIcon(QIcon(pm));
    }

    Color color_;
    bool alpha_;
    QString title_;
    std::function<void(Color)> changed_;
};

class WCanvas;

// The Qt side of a canvas: painting and input handed to its WCanvas while
// it's there (the window deletes Qt widgets after the parts).
class CanvasWidget : public QWidget
{
public:
    WCanvas* owner = nullptr;

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mousePressEvent(QMouseEvent* e) override { press(e); }
    // Qt's second press of a double click.
    void mouseDoubleClickEvent(QMouseEvent* e) override { press(e); }
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void enterEvent(QEnterEvent* e) override;
    void leaveEvent(QEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override { key(e, true); }
    void keyReleaseEvent(QKeyEvent* e) override { key(e, false); }

private:
    void press(QMouseEvent* e);
    void key(QKeyEvent* e, bool press);
};

int buttonNumber(Qt::MouseButton b)
{
    switch (b)
    {
    case Qt::LeftButton: return 1;
    case Qt::MiddleButton: return 2;
    case Qt::RightButton: return 3;
    case Qt::BackButton: return 4;
    case Qt::ForwardButton: return 5;
    default: return 0;
    }
}

class WCanvas : public W<anyui::Canvas>
{
public:
    explicit WCanvas(const spec::Canvas& s)
        : W(new CanvasWidget)
    {
        auto* c = canvas();
        c->owner = this;
        c->setMouseTracking(true);
        c->setFocusPolicy(s.focusable ? Qt::StrongFocus : Qt::NoFocus);
        c->setAttribute(Qt::WA_OpaquePaintEvent);  // the app paints every pixel
        c->setMinimumSize(1, 1);
        c->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        frames_ = new QTimer(c);
        frames_->setTimerType(Qt::PreciseTimer);
        QObject::connect(frames_, &QTimer::timeout, c, [this] { tick(); });
    }
    ~WCanvas() override
    {
        *alive_ = false;
        canvas()->owner = nullptr;
        frames_->stop();
    }

    void invalidate() override { w_->update(); }
    SizeF size() const override { return {float(w_->width()), float(w_->height())}; }
    float deviceScale() const override { return float(w_->devicePixelRatioF()); }
    void setCursor(Cursor c) override
    {
        switch (c)
        {
        case Cursor::Default: w_->unsetCursor(); break;
        case Cursor::Pointer: w_->setCursor(Qt::PointingHandCursor); break;
        case Cursor::Text: w_->setCursor(Qt::IBeamCursor); break;
        case Cursor::Crosshair: w_->setCursor(Qt::CrossCursor); break;
        case Cursor::Grab: w_->setCursor(Qt::OpenHandCursor); break;
        case Cursor::Grabbing: w_->setCursor(Qt::ClosedHandCursor); break;
        case Cursor::ZoomIn:
        case Cursor::ZoomOut: w_->setCursor(Qt::CrossCursor); break;  // Qt has no zoom cursors
        case Cursor::Move: w_->setCursor(Qt::SizeAllCursor); break;
        case Cursor::Wait: w_->setCursor(Qt::WaitCursor); break;
        case Cursor::Hidden: w_->setCursor(Qt::BlankCursor); break;
        }
    }
    void onPaint(std::function<void(Painter&, SizeF)> f) override { paint_ = std::move(f); }
    void onResize(std::function<void(SizeF)> f) override { resize_ = std::move(f); }
    void onPointer(std::function<bool(const PointerEvent&)> f) override { pointer_ = std::move(f); }
    void onScroll(std::function<bool(const ScrollEvent&)> f) override { scroll_ = std::move(f); }
    void onKey(std::function<bool(const KeyEvent&)> f) override { key_ = std::move(f); }

    // Frames at the screen's refresh rate while anyone's subscribed.
    Subscription onFrame(std::function<void(double)> f) override
    {
        const int id = ++nextFrame_;
        frameFns_.push_back({id, std::move(f)});
        if (!frames_->isActive())
        {
            const QScreen* screen = w_->screen();
            const double hz = screen && screen->refreshRate() > 1 ? screen->refreshRate() : 60;
            frames_->start(std::max(1, int(1000 / hz)));
        }
        return Subscription(
            [this, alive = alive_, id]
            {
                if (!*alive) return;
                std::erase_if(frameFns_, [id](auto& e) { return e.first == id; });
                if (frameFns_.empty()) frames_->stop();
            });
    }

    // From CanvasWidget.
    void paint(QPainter& qp)
    {
        if (!paint_) return;
        QtPainter p(qp, w_);
        paint_(p, size());
    }
    void resized()
    {
        if (resize_) resize_(size());
    }
    bool pointer(const PointerEvent& e) { return pointer_ && pointer_(e); }
    bool scroll(const ScrollEvent& e) { return scroll_ && scroll_(e); }
    bool key(const KeyEvent& e) { return key_ && key_(e); }

    // Presses counted as gtk3 does: each press arrives once, with how many
    // came in quick succession at much the same place.
    int clicks(int button, QPointF pos, quint64 time)
    {
        const bool again = button == lastButton_ && time - lastTime_ <= quint64(QApplication::doubleClickInterval()) &&
                           std::abs(pos.x() - lastPos_.x()) <= QApplication::startDragDistance() &&
                           std::abs(pos.y() - lastPos_.y()) <= QApplication::startDragDistance();
        clicks_ = again ? clicks_ + 1 : 1;
        lastButton_ = button;
        lastTime_ = time;
        lastPos_ = pos;
        return clicks_;
    }

private:
    CanvasWidget* canvas() const { return static_cast<CanvasWidget*>(w_); }
    void tick()
    {
        const double t = double(QDateTime::currentMSecsSinceEpoch()) / 1000.0;
        auto fns = frameFns_;  // callbacks may unsubscribe
        for (auto& [id, fn] : fns)
            if (std::any_of(frameFns_.begin(), frameFns_.end(), [id = id](auto& e) { return e.first == id; })) fn(t);
    }

    std::function<void(Painter&, SizeF)> paint_;
    std::function<void(SizeF)> resize_;
    std::function<bool(const PointerEvent&)> pointer_;
    std::function<bool(const ScrollEvent&)> scroll_;
    std::function<bool(const KeyEvent&)> key_;
    QTimer* frames_;
    std::vector<std::pair<int, std::function<void(double)>>> frameFns_;
    int nextFrame_ = 0;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    int clicks_ = 0, lastButton_ = 0;
    quint64 lastTime_ = 0;
    QPointF lastPos_;
};

void CanvasWidget::paintEvent(QPaintEvent*)
{
    if (!owner) return;
    QPainter p(this);
    owner->paint(p);
}

void CanvasWidget::resizeEvent(QResizeEvent*)
{
    if (owner) owner->resized();
}

void CanvasWidget::press(QMouseEvent* e)
{
    if (!owner) return;
    if (focusPolicy() != Qt::NoFocus && !hasFocus()) setFocus(Qt::MouseFocusReason);
    PointerEvent ev{PointerEvent::Type::Press, {float(e->position().x()), float(e->position().y())}, buttonNumber(e->button()), 1,
                    modifiersOf(e->modifiers())};
    ev.clicks = owner->clicks(ev.button, e->position(), e->timestamp());
    if (owner->pointer(ev)) e->accept();
}

void CanvasWidget::mouseReleaseEvent(QMouseEvent* e)
{
    if (!owner) return;
    owner->pointer({PointerEvent::Type::Release, {float(e->position().x()), float(e->position().y())}, buttonNumber(e->button()), 1,
                    modifiersOf(e->modifiers())});
}

void CanvasWidget::mouseMoveEvent(QMouseEvent* e)
{
    if (!owner) return;
    owner->pointer({PointerEvent::Type::Move, {float(e->position().x()), float(e->position().y())}, 0, 1, modifiersOf(e->modifiers())});
}

void CanvasWidget::enterEvent(QEnterEvent* e)
{
    if (!owner) return;
    owner->pointer({PointerEvent::Type::Enter, {float(e->position().x()), float(e->position().y())}, 0, 1,
                    modifiersOf(QApplication::keyboardModifiers())});
}

void CanvasWidget::leaveEvent(QEvent*)
{
    if (!owner) return;
    const QPointF at = mapFromGlobal(QCursor::pos());
    owner->pointer({PointerEvent::Type::Leave, {float(at.x()), float(at.y())}, 0, 1, modifiersOf(QApplication::keyboardModifiers())});
}

// Wheels report eighths of a degree, 120 to a notch, positive away from the
// user (up, and left for a horizontal wheel); touchpads also report pixels.
void CanvasWidget::wheelEvent(QWheelEvent* e)
{
    if (!owner) return;
    ScrollEvent ev{{float(e->position().x()), float(e->position().y())}, 0, 0, false, modifiersOf(e->modifiers())};
    const bool touchpad = e->pointingDevice() && e->pointingDevice()->type() == QInputDevice::DeviceType::TouchPad;
    if (touchpad && !e->pixelDelta().isNull())
    {
        ev.dx = -float(e->pixelDelta().x());
        ev.dy = -float(e->pixelDelta().y());
        ev.precise = true;
    }
    else
    {
        ev.dx = -float(e->angleDelta().x()) / 120.f;
        ev.dy = -float(e->angleDelta().y()) / 120.f;
    }
    if ((ev.dx != 0 || ev.dy != 0) && owner->scroll(ev))
        e->accept();
    else
        e->ignore();
}

void CanvasWidget::key(QKeyEvent* e, bool press)
{
    if (!owner) return;
    KeyEvent ev;
    ev.press = press;
    ev.key = keyFor(e->key());
    if (ev.key == Key::Character)
    {
        const QString t = e->text().toLower();
        ev.codepoint = !t.isEmpty() && !t[0].isNull() && t[0].isPrint() ? char32_t(t.toUcs4().value(0))
                                                                         : char32_t(QChar(e->key()).toLower().unicode());
    }
    ev.modifiers = modifiersOf(e->modifiers());
    ev.repeat = e->isAutoRepeat();
    if (owner->key(ev))
        e->accept();
    else
        e->ignore();
}

// ---------------------------------------------------------------- list

// The anyui model as Qt sees it: a row count kept in step with the
// notifications, and text for type-ahead and accessibility. The view paints
// rows itself, so nothing else is asked of it.
class RowsModel : public QAbstractTableModel
{
public:
    RowsModel(std::vector<Column> cols, QObject* parent)
        : QAbstractTableModel(parent)
        , cols_(std::move(cols))
    {
    }

    ListModel* model = nullptr;
    int count = 0;

    int rowCount(const QModelIndex& p = {}) const override { return p.isValid() ? 0 : count; }
    int columnCount(const QModelIndex& p = {}) const override
    {
        return p.isValid() ? 0 : int(cols_.size());
    }
    QVariant data(const QModelIndex& i, int role) const override
    {
        if (role != Qt::DisplayRole || !model || size_t(i.row()) >= model->rowCount()) return {};
        return qs(model->text(size_t(i.row()), size_t(i.column())));
    }
    QVariant headerData(int section, Qt::Orientation o, int role) const override
    {
        if (o != Qt::Horizontal || section < 0 || size_t(section) >= cols_.size()) return {};
        if (role == Qt::DisplayRole) return qs(cols_[size_t(section)].title);
        if (role == Qt::TextAlignmentRole)
            return QVariant::fromValue(qtAlign(cols_[size_t(section)].align) | Qt::AlignVCenter);
        return {};
    }
    Qt::ItemFlags flags(const QModelIndex&) const override
    {
        return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemNeverHasChildren;
    }

    // The anyui model has already changed; the count catches up here.
    void reset(int n)
    {
        beginResetModel();
        count = n;
        endResetModel();
    }
    void inserted(int first, int n)
    {
        beginInsertRows({}, first, first + n - 1);
        count += n;
        endInsertRows();
    }
    void removed(int first, int n)
    {
        beginRemoveRows({}, first, first + n - 1);
        count -= n;
        endRemoveRows();
    }
    void changed(int first, int n)
    {
        emit dataChanged(index(first, 0), index(first + n - 1, columnCount() - 1));
    }

private:
    std::vector<Column> cols_;
};

// Rows are one height (QTreeView asks only the first) and painted by
// ListView::drawRow.
class RowDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    int height = 20;
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override
    {
        return {0, height};
    }
    void paint(QPainter*, const QStyleOptionViewItem&, const QModelIndex&) const override {}
};

class WList;

// The QTreeView, handing its painting and input to its WList.
class ListView : public QTreeView
{
public:
    WList* owner = nullptr;

    void baseMousePress(QMouseEvent* e) { QTreeView::mousePressEvent(e); }
    void baseMouseMove(QMouseEvent* e) { QTreeView::mouseMoveEvent(e); }
    void baseMouseRelease(QMouseEvent* e) { QTreeView::mouseReleaseEvent(e); }
    void baseMouseDoubleClick(QMouseEvent* e) { QTreeView::mouseDoubleClickEvent(e); }
    void baseKeyPress(QKeyEvent* e) { QTreeView::keyPressEvent(e); }

protected:
    void drawRow(QPainter* p, const QStyleOptionViewItem& option,
                 const QModelIndex& index) const override;
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;
    bool viewportEvent(QEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void changeEvent(QEvent* e) override;
    void scrollContentsBy(int dx, int dy) override;
};

class WList : public W<anyui::List>, public ListObserver
{
public:
    explicit WList(const spec::List& s)
        : W(new ListView)
        , cols_(s.columns)
        , compact_(s.compact)
        , autoScroll_(s.autoScroll)
    {
        view_ = static_cast<ListView*>(w_);
        view_->owner = this;
        rows_ = new RowsModel(s.columns, view_);
        delegate_ = new RowDelegate(view_);
        view_->setItemDelegate(delegate_);
        view_->setModel(rows_);
        view_->setRootIsDecorated(false);
        view_->setItemsExpandable(false);
        view_->setIndentation(0);
        view_->setUniformRowHeights(true);
        view_->setAllColumnsShowFocus(true);
        view_->setHeaderHidden(!s.headers);
        view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        view_->setSelectionBehavior(QAbstractItemView::SelectRows);
        view_->setSelectionMode(s.selection == SelectionMode::None ? QAbstractItemView::NoSelection
                                : s.selection == SelectionMode::Single
                                    ? QAbstractItemView::SingleSelection
                                    : QAbstractItemView::ExtendedSelection);
        // Pixel scrolling: the thumb and the rows follow the pointer exactly.
        view_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        view_->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
        view_->viewport()->setMouseTracking(true);
        if (compact_) view_->setFont(compactFont(view_->font()));
        if (s.softSelection) soft_ = softColors(view_);
        if (autoScroll_) lists().insert(this);
        firstTextColumn_ =
            int(std::ranges::find_if(cols_, [](const Column& c) { return !c.painted; }) -
                cols_.begin());
        if (size_t(firstTextColumn_) >= cols_.size()) firstTextColumn_ = 0;

        QHeaderView* h = view_->header();
        h->setStretchLastSection(false);
        h->setSectionsMovable(false);
        h->setMinimumSectionSize(8);
        h->setSectionResizeMode(QHeaderView::Interactive);
        h->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        const bool sortable = std::ranges::any_of(cols_, &Column::sortable);
        h->setSectionsClickable(sortable);
        for (size_t i = 0; i < cols_.size(); ++i)
        {
            const Column& c = cols_[i];
            int width = px(c.width);
            if (width < 0)
                width = int((c.painted ? 8 : std::max<size_t>(c.title.size() + 4, 10)) *
                            g_metrics.charWidth);
            h->resizeSection(int(i), width);
            view_->setColumnHidden(int(i), !c.visible);
        }
        h->setContextMenuPolicy(Qt::CustomContextMenu);
        QObject::connect(h, &QHeaderView::customContextMenuRequested, h,
                         [this](QPoint pos)
                         {
                             const int col = view_->header()->logicalIndexAt(pos);
                             if (col >= 0 && headerMenu_)
                                 popupMenu(headerMenu_(size_t(col)), view_->header(),
                                           view_->header()->mapToGlobal(pos));
                         });
        QObject::connect(h, &QHeaderView::sectionClicked, h,
                         [this](int col) { headerClicked(col); });
        QObject::connect(view_->selectionModel(), &QItemSelectionModel::selectionChanged, view_,
                         [this]
                         {
                             if (selChanged_) selChanged_();
                         });
        fontsChanged();
    }

    ~WList() override
    {
        lists().erase(this);
        if (active_ == this) active_ = nullptr;
        if (model_) model_->removeObserver(this);
        rows_->model = nullptr;
        view_->owner = nullptr;
    }

    void setModel(ListModel* m) override
    {
        if (model_) model_->removeObserver(this);
        model_ = m;
        rows_->model = m;
        if (model_) model_->addObserver(this);
        modelReset();
    }

    std::vector<size_t> selection() const override
    {
        std::vector<size_t> out;
        for (const QModelIndex& i : view_->selectionModel()->selectedRows())
            out.push_back(size_t(i.row()));
        std::ranges::sort(out);
        return out;
    }

    std::optional<size_t> current() const override
    {
        const QModelIndex i = view_->currentIndex();
        if (!i.isValid()) return std::nullopt;
        return size_t(i.row());
    }

    void select(const std::vector<size_t>& rowsIn) override
    {
        std::vector<size_t> rows = rowsIn;
        std::ranges::sort(rows);
        QItemSelection sel;
        const int last = rows_->columnCount() - 1;
        for (size_t i = 0; i < rows.size();)
        {
            size_t j = i;
            while (j + 1 < rows.size() && rows[j + 1] <= rows[j] + 1) ++j;
            if (rows[i] < size_t(rows_->count))
                sel.select(rows_->index(int(rows[i]), 0),
                           rows_->index(int(std::min(rows[j], size_t(rows_->count) - 1)), last));
            i = j + 1;
        }
        view_->selectionModel()->select(sel, QItemSelectionModel::ClearAndSelect);
    }

    // The cursor row, selected alone (as GTK's set_cursor does).
    void setCurrent(size_t row) override
    {
        if (row >= size_t(rows_->count)) return;
        view_->selectionModel()->setCurrentIndex(rows_->index(int(row), firstTextColumn_),
                                                 QItemSelectionModel::ClearAndSelect |
                                                     QItemSelectionModel::Rows);
    }

    void scrollTo(size_t row) override
    {
        if (row < size_t(rows_->count))
            view_->scrollTo(rows_->index(int(row), 0), QAbstractItemView::EnsureVisible);
    }

    void setSortIndicator(std::optional<size_t> column, SortOrder order) override
    {
        sortColumn_ = column;
        sortOrder_ = order;
        showSortIndicator();
    }

    std::vector<float> columnWidths() const override
    {
        std::vector<float> out;
        for (size_t i = 0; i < cols_.size(); ++i)
            out.push_back(float(view_->header()->sectionSize(int(i))));
        return out;
    }

    void setColumnWidths(const std::vector<float>& widths) override
    {
        for (size_t i = 0; i < widths.size() && i < cols_.size(); ++i)
            if (widths[i] > 0) view_->header()->resizeSection(int(i), int(widths[i]));
        fitted_ = true;  // restored widths win over stretching
    }

    bool columnVisible(size_t column) const override
    {
        return column < cols_.size() && !view_->isColumnHidden(int(column));
    }
    void setColumnVisible(size_t column, bool v) override
    {
        if (column < cols_.size()) view_->setColumnHidden(int(column), !v);
    }

    void onSelectionChanged(std::function<void()> f) override { selChanged_ = std::move(f); }
    void onActivate(std::function<void(size_t)> f) override { activate_ = std::move(f); }
    void onSortRequested(std::function<void(size_t, SortOrder)> f) override
    {
        sort_ = std::move(f);
    }
    void onContextMenu(std::function<Menu(std::optional<size_t>)> f) override
    {
        contextMenu_ = std::move(f);
    }
    void onHeaderMenu(std::function<Menu(size_t)> f) override { headerMenu_ = std::move(f); }
    void onCellContextMenu(std::function<Menu(size_t, size_t, PointF)> f) override
    {
        cellMenu_ = std::move(f);
    }
    void onReorder(ReorderFn f) override { reorder_ = std::move(f); }
    void setReorderable(bool v) override { reorderable_ = v; }
    void onCellDrag(CellDrag d) override { cellDrag_ = std::move(d); }

    // ListObserver. Big changes reset: Qt's per-row bookkeeping is O(rows).
    void modelReset() override { rows_->reset(model_ ? int(model_->rowCount()) : 0); }
    void rowsChanged(size_t first, size_t n) override
    {
        if (n > kBulk || first + n > size_t(rows_->count))
            view_->viewport()->update();
        else if (n)
            rows_->changed(int(first), int(n));
    }
    void rowsInserted(size_t first, size_t n) override
    {
        if (n > kBulk) return modelReset();
        if (n) rows_->inserted(int(first), int(n));
    }
    void rowsRemoved(size_t first, size_t n) override
    {
        if (n > kBulk) return modelReset();
        if (n) rows_->removed(int(first), int(n));
    }

    // Esc during a drag or an autoscroll cancels it, ahead of any window
    // key handling (which might close the dialog).
    static bool cancelActiveDrag()
    {
        for (WList* l : lists())
            if (l->auto_.on)
            {
                l->stopAutoScroll();
                return true;
            }
        WList* list = active_;
        if (!list) return false;
        list->drag_ = {};
        list->cell_ = {};
        list->dropY_.reset();
        active_ = nullptr;
        list->view_->viewport()->unsetCursor();
        list->view_->viewport()->update();
        return true;
    }

    // ---- called by ListView

    void paintRow(QPainter* p, const QRect& r, int row) const
    {
        if (!model_ || size_t(row) >= model_->rowCount()) return;
        const QPalette pal = view_->palette();
        const bool selected = view_->selectionModel()->isRowSelected(row, QModelIndex());
        const bool hot = hot_ == size_t(row) && soft_;
        const int vw = view_->viewport()->width();
        const QRect band(0, r.y(), vw, r.height());
        // Selection doesn't grey out when the window loses focus.
        if (selected)
            p->fillRect(band, soft_ ? (hot ? soft_->hoverSelected : soft_->selected)
                                    : pal.color(QPalette::Active, QPalette::Highlight));
        else if (hot)
            p->fillRect(band, soft_->hover);
        const bool focused = view_->hasFocus() && view_->currentIndex().row() == row;
        QHeaderView* h = view_->header();
        for (size_t c = 0; c < cols_.size(); ++c)
        {
            if (h->isSectionHidden(int(c))) continue;
            const int x = h->sectionViewportPosition(int(c)), w = h->sectionSize(int(c));
            if (x >= vw || x + w <= 0) continue;
            const QRect cell(x, r.y(), w, r.height());
            if (cols_[c].painted)
                paintCell(p, cell, size_t(row), c, selected, focused);
            else
                textCell(p, cell, size_t(row), c, selected);
        }
    }

    void paintOverlay(QPainter& qp)
    {
        if (dropY_)
        {
            qp.fillRect(QRect(0, *dropY_ - 1, view_->viewport()->width(), 2),
                        view_->palette().color(QPalette::Active, QPalette::Highlight));
        }
        if (cell_.dragging && cell_.target)
        {
            const QRect r = rowRect(*cell_.target).adjusted(1, 1, -2, -2);
            qp.setPen(QPen(view_->palette().color(QPalette::Active, QPalette::Highlight), 2));
            qp.setBrush(Qt::NoBrush);
            qp.drawRect(r);
        }
        if (cell_.dragging && !cell_.start.label.empty())
        {
            QtPainter p(qp, view_);
            TextStyle ts;
            ts.bold = true;
            const SizeF t = p.measure(cell_.start.label, ts);
            const RectF box{float(cell_.pos.x()) + 12, float(cell_.pos.y()) + 4, t.w + 12, t.h + 4};
            p.rect(box, 4, Paint{ThemeColor::Highlight}, Paint{ThemeColor::Mid});
            ts.fg = ThemeColor::HighlightText;
            p.text({box.x + 6, box.y + 2}, cell_.start.label, ts);
        }
        if (auto_.on)
        {
            QtPainter p(qp, view_);
            paintAutoScrollAnchor(p, float(auto_.anchor.x()), float(auto_.anchor.y()));
        }
    }

    // Overlays are drawn in viewport coordinates; a scroll's blit would
    // carry them along with the rows, so the viewport repaints whole.
    bool overlaid() const { return dropY_ || cell_.dragging || auto_.on; }

    void fontsChanged()
    {
        fonts_.clear();
        const QFontMetrics fm(view_->font());
        rowHeight_ = std::max(fm.height() + (compact_ ? 2 : 6), compact_ ? 18 : 22);
        iconSize_ = std::min(16, rowHeight_ - 1);
        delegate_->height = rowHeight_;
        view_->doItemsLayout();
    }

    void paletteChanged()
    {
        if (soft_) soft_ = softColors(view_);
    }

    // Stretch columns share the width the others leave, once, when the
    // list first has a size; after that, widening one column never narrows
    // another (the list scrolls sideways instead).
    void fitStretch()
    {
        const int width = view_->viewport()->width();
        if (fitted_ || width <= 1) return;
        fitted_ = true;
        QHeaderView* h = view_->header();
        int used = 0, stretching = 0;
        for (size_t i = 0; i < cols_.size(); ++i)
        {
            if (h->isSectionHidden(int(i))) continue;
            used += h->sectionSize(int(i));
            stretching += cols_[i].stretch;
        }
        const int spare = width - used;
        if (spare <= 0 || !stretching) return;
        for (size_t i = 0; i < cols_.size(); ++i)
            if (cols_[i].stretch && !h->isSectionHidden(int(i)))
                h->resizeSection(int(i), h->sectionSize(int(i)) + spare / stretching);
    }

    void mousePress(QMouseEvent* e)
    {
        if (auto_.on)
        {
            stopAutoScroll();
            return;
        }
        if (autoScroll_ && e->button() == Qt::MiddleButton)
        {
            startAutoScroll(e->position().toPoint());
            return;
        }
        if (cellPressed(e))
        {
            view_->baseMousePress(e);  // selects as usual; dragging begins on motion
            return;
        }
        if (dragPressed(e)) return;
        view_->baseMousePress(e);
    }

    void mouseMove(QMouseEvent* e)
    {
        const QPoint pos = e->position().toPoint();
        if (auto_.on)
        {
            auto_.y = pos.y();
            return;
        }
        trackHover(e);
        if (cell_.pressed)
        {
            cellMotion(e);
            return;
        }
        if (drag_.pressed && (e->buttons() & Qt::LeftButton))
        {
            dragMotion(pos);
            return;
        }
        view_->baseMouseMove(e);
    }

    void mouseRelease(QMouseEvent* e)
    {
        if (auto_.on)
        {
            if (e->button() == Qt::MiddleButton) stopAutoScroll();
            return;
        }
        if (cell_.pressed && e->button() == Qt::LeftButton)
        {
            CellDragState d = std::move(cell_);
            cell_ = {};
            if (!d.dragging) return view_->baseMouseRelease(e);  // a plain click
            if (active_ == this) active_ = nullptr;
            view_->viewport()->unsetCursor();
            view_->viewport()->update();
            if (d.target && cellDrag_.drop) cellDrag_.drop(d.from, *d.target, d.start.data);
            return;
        }
        if (drag_.pressed && e->button() == Qt::LeftButton)
        {
            Drag d = drag_;
            drag_ = {};
            if (active_ == this) active_ = nullptr;
            if (d.dragging)
            {
                const size_t dest = dropTarget(e->position().toPoint().y());
                dropY_.reset();
                view_->viewport()->unsetCursor();
                view_->viewport()->update();
                if (reorder_ && !isNoopDrop(dest)) reorder_(selection(), dest);
                return;
            }
            if (d.deferSelect)
            {
                select({d.row});
                view_->selectionModel()->setCurrentIndex(rows_->index(int(d.row), firstTextColumn_),
                                                         QItemSelectionModel::NoUpdate);
                return;
            }
        }
        view_->baseMouseRelease(e);
    }

    void mouseDoubleClick(QMouseEvent* e)
    {
        if (e->button() != Qt::LeftButton) return mousePress(e);
        view_->baseMouseDoubleClick(e);
        const QModelIndex i = view_->indexAt(e->position().toPoint());
        if (i.isValid() && activate_) activate_(size_t(i.row()));
    }

    // Enter activates the current row (and stops there: a dialog's default
    // button would take it too).
    void keyPress(QKeyEvent* e)
    {
        if (e->key() == Qt::Key_Escape && cancelActiveDrag()) return;
        if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter)
        {
            if (auto row = current(); row && activate_) activate_(*row);
            e->accept();
            return;
        }
        view_->baseKeyPress(e);
    }

    // Right-click: Qt has already selected the row, or kept a multi-
    // selection that holds it (two revisions to compare).
    void contextMenu(QContextMenuEvent* e)
    {
        std::optional<size_t> row;
        QPoint global = e->globalPos();
        if (e->reason() == QContextMenuEvent::Mouse)
        {
            const QModelIndex i = view_->indexAt(e->pos());
            if (i.isValid())
            {
                row = size_t(i.row());
                const int col = view_->header()->logicalIndexAt(e->pos().x());
                if (cellMenu_ && col >= 0)
                {
                    const QRect r = rowRect(*row);
                    const PointF at{
                        float(e->pos().x() - view_->header()->sectionViewportPosition(col)),
                        float(e->pos().y() - r.y())};
                    popupMenu(cellMenu_(*row, size_t(col), at), view_, global);
                    return;
                }
            }
        }
        else
        {
            row = current();
            const QRect r = row ? rowRect(*row) : QRect();
            global = view_->viewport()->mapToGlobal(row ? QPoint(r.x() + 16, r.bottom())
                                                        : QPoint(16, 16));
        }
        if (contextMenu_) popupMenu(contextMenu_(row), view_, global);
    }

    void leave() { setHot(std::nullopt); }

private:
    static constexpr size_t kBulk = 256;

    QRect rowRect(size_t row) const
    {
        const QRect r = view_->visualRect(rows_->index(int(row), firstTextColumn_));
        return {0, r.y(), view_->viewport()->width(), r.height()};
    }

    std::optional<size_t> rowAt(int y) const
    {
        const QModelIndex i = view_->indexAt(QPoint(0, y));
        if (!i.isValid()) return std::nullopt;
        return size_t(i.row());
    }

    void showSortIndicator()
    {
        QHeaderView* h = view_->header();
        h->setSortIndicatorShown(sortColumn_.has_value());
        if (sortColumn_)
            h->setSortIndicator(int(*sortColumn_), sortOrder_ == SortOrder::Ascending
                                                       ? Qt::AscendingOrder
                                                       : Qt::DescendingOrder);
    }

    // The header flips its own indicator on a click; the app decides.
    void headerClicked(int col)
    {
        const bool asc =
            !(sortColumn_ && *sortColumn_ == size_t(col) && sortOrder_ == SortOrder::Ascending);
        showSortIndicator();
        if (!sort_ || size_t(col) >= cols_.size() || !cols_[size_t(col)].sortable) return;
        sort_(size_t(col), asc ? SortOrder::Ascending : SortOrder::Descending);
    }

    const std::pair<QFont, QFontMetrics>& fontFor(const TextStyle& s) const
    {
        const int key = (s.bold ? 1 : 0) | (s.italic ? 2 : 0) | (s.monospace ? 4 : 0);
        if (s.size > 0)
        {
            static thread_local std::optional<std::pair<QFont, QFontMetrics>> sized;
            const QFont f = styledFont(view_->font(), s);
            sized.emplace(f, QFontMetrics(f));
            return *sized;
        }
        auto it = fonts_.find(key);
        if (it == fonts_.end())
        {
            const QFont f = styledFont(view_->font(), s);
            it = fonts_.emplace(key, std::pair{f, QFontMetrics(f)}).first;
        }
        return it->second;
    }

    void textCell(QPainter* p, const QRect& cell, size_t row, size_t c, bool selected) const
    {
        const TextStyle style = model_->style(row, c);
        const QPalette pal = view_->palette();
        if (style.bg && !selected) p->fillRect(cell, qcolor(resolve(view_, *style.bg)));
        const int pad = compact_ ? 3 : 4;
        QRect area = cell.adjusted(pad, 0, -pad, 0);
        if (const Icon icon = model_->icon(row, c))
        {
            const QIcon qi = toQIcon(icon, iconSize_, view_);
            qi.paint(p, QRect(area.x(), area.y() + (area.height() - iconSize_) / 2, iconSize_,
                              iconSize_));
            area.setLeft(area.left() + iconSize_ + 4);
        }
        const auto& [font, fm] = fontFor(style);
        p->setFont(font);
        p->setPen(style.fg             ? qcolor(resolve(view_, *style.fg))
                  : selected && !soft_ ? pal.color(QPalette::Active, QPalette::HighlightedText)
                                       : pal.color(QPalette::Active, QPalette::Text));
        const QString text = qs(model_->text(row, c));
        p->drawText(area, qtAlign(cols_[c].align) | Qt::AlignVCenter | Qt::TextSingleLine,
                    fm.elidedText(text, Qt::ElideRight, area.width()));
    }

    void paintCell(QPainter* p, const QRect& cell, size_t row, size_t c, bool selected,
                   bool focused) const
    {
        if (auto bg = model_->style(row, c).bg; bg && !selected)
            p->fillRect(cell, qcolor(resolve(view_, *bg)));
        p->save();
        p->setClipRect(cell, Qt::IntersectClip);
        p->translate(cell.topLeft());
        {
            QtPainter painter(*p, view_,
                              selected && !soft_ ? std::optional<Paint>{ThemeColor::HighlightText}
                                                 : std::nullopt);
            if (soft_)
            {
                painter.overrideColor(ThemeColor::Highlight, fromQColor(soft_->selected));
                painter.overrideColor(ThemeColor::HighlightText,
                                      themeColor(view_, ThemeColor::Text));
            }
            model_->paint(row, c, painter,
                          CellInfo{{float(cell.width()), float(cell.height())}, selected, focused});
        }
        p->restore();
    }

    // Soft-selection lists tint the row under the pointer (as on win32 and
    // GTK, and Explorer's and TortoiseGit's lists).
    void setHot(std::optional<size_t> row)
    {
        if (row == hot_) return;
        for (std::optional<size_t> r : {hot_, row})
            if (r) view_->viewport()->update(rowRect(*r));
        hot_ = row;
    }
    void trackHover(QMouseEvent* e)
    {
        if (!soft_ || drag_.dragging || cell_.dragging) return setHot(std::nullopt);
        setHot(rowAt(e->position().toPoint().y()));
    }

    // Drag-reordering (onReorder): press on a row, move past the drag
    // distance, and a line between rows follows the pointer until release.
    struct Drag
    {
        bool pressed = false;
        bool dragging = false;
        bool deferSelect = false;  // pressed inside a multi-selection
        QPoint start;
        size_t row = 0;
    };
    // A CellDrag in progress: from a press `begin` accepted.
    struct CellDragState
    {
        bool pressed = false, dragging = false;
        QPoint origin, pos;  // the press, the pointer now
        size_t from = 0;
        CellDragStart start;
        std::optional<size_t> target;
    };

    bool dragPressed(QMouseEvent* e)
    {
        drag_ = {};
        if (!reorder_ || !reorderable_ || e->button() != Qt::LeftButton ||
            (e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier)))
            return false;
        const QPoint pos = e->position().toPoint();
        auto row = rowAt(pos.y());
        if (!row) return false;
        const bool selected = view_->selectionModel()->isRowSelected(int(*row), QModelIndex());
        drag_ = {true, false, selected && view_->selectionModel()->selectedRows().size() > 1, pos,
                 *row};
        // Keep a multi-selection so it can be dragged; a plain click inside
        // it selects just that row on release instead.
        if (drag_.deferSelect)
        {
            view_->setFocus(Qt::MouseFocusReason);
            return true;
        }
        view_->baseMousePress(e);
        return true;
    }

    bool isNoopDrop(size_t dest) const
    {
        const auto sel = selection();
        if (sel.empty()) return true;
        const bool contiguous = sel.back() - sel.front() + 1 == sel.size();
        return contiguous && dest >= sel.front() && dest <= sel.back() + 1;
    }

    // The row the dragged rows would go in front of; sets the drop line.
    size_t dropTarget(int y)
    {
        const size_t count = size_t(rows_->count);
        size_t dest = count;
        const int vh = view_->viewport()->height();
        if (auto row = rowAt(std::clamp(y, 0, std::max(0, vh - 1))))
        {
            const QRect r = rowRect(*row);
            dest = y < r.center().y() ? *row : *row + 1;
        }
        const bool noop = isNoopDrop(dest);
        view_->viewport()->setCursor(noop ? Qt::ForbiddenCursor : Qt::ClosedHandCursor);
        if (count && !noop)
            dropY_ = dest >= count ? rowRect(count - 1).bottom() + 1 : rowRect(dest).top();
        else
            dropY_.reset();
        return dest;
    }

    void edgeScroll(int y)
    {
        const int edge = 24, vh = view_->viewport()->height();
        if (y < edge || y > vh - edge)
        {
            QScrollBar* bar = view_->verticalScrollBar();
            bar->setValue(bar->value() + (y < edge ? -1 : 1) * rowHeight_);
        }
    }

    void dragMotion(QPoint pos)
    {
        if (!drag_.dragging)
        {
            if ((pos - drag_.start).manhattanLength() < QApplication::startDragDistance()) return;
            drag_.dragging = true;
            active_ = this;
            setHot(std::nullopt);
        }
        dropTarget(pos.y());
        edgeScroll(pos.y());
        view_->viewport()->update();
    }

    // A press on something the app lets you drag out of a cell. Selection
    // still happens as for any click; the drag starts past the threshold.
    bool cellPressed(QMouseEvent* e)
    {
        cell_ = {};
        if (!cellDrag_.begin || e->button() != Qt::LeftButton ||
            (e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier)))
            return false;
        const QPoint pos = e->position().toPoint();
        auto row = rowAt(pos.y());
        const int col = view_->header()->logicalIndexAt(pos.x());
        if (!row || col < 0) return false;
        const QRect r = rowRect(*row);
        auto start =
            cellDrag_.begin(*row, size_t(col),
                            {float(pos.x() - view_->header()->sectionViewportPosition(col)),
                             float(pos.y() - r.y())});
        if (!start) return false;
        cell_.pressed = true;
        cell_.origin = cell_.pos = pos;
        cell_.from = *row;
        cell_.start = std::move(*start);
        return true;
    }

    void cellMotion(QMouseEvent* e)
    {
        if (!(e->buttons() & Qt::LeftButton)) return;
        const QPoint pos = e->position().toPoint();
        cell_.pos = pos;
        if (!cell_.dragging)
        {
            if ((pos - cell_.origin).manhattanLength() < QApplication::startDragDistance()) return;
            cell_.dragging = true;
            active_ = this;
            setHot(std::nullopt);
        }
        cell_.target.reset();
        if (auto row = rowAt(pos.y()))
            if (cellDrag_.canDrop && cellDrag_.canDrop(cell_.from, *row, cell_.start.data))
                cell_.target = row;
        view_->viewport()->setCursor(cell_.target ? Qt::ClosedHandCursor : Qt::ForbiddenCursor);
        edgeScroll(pos.y());
        view_->viewport()->update();
    }

    // Autoscroll (spec.autoScroll): the middle button held, the list
    // scrolls each display frame by the pointer's offset from the anchor.
    void startAutoScroll(QPoint at)
    {
        auto_ = {true, at, at.y(), double(view_->verticalScrollBar()->value())};
        if (!autoTimer_)
        {
            autoTimer_ = new QTimer(view_);
            autoTimer_->setTimerType(Qt::PreciseTimer);
            QObject::connect(autoTimer_, &QTimer::timeout, view_, [this] { autoScrollStep(); });
        }
        const QScreen* screen = view_->screen();
        const double hz = screen && screen->refreshRate() > 1 ? screen->refreshRate() : 60;
        autoTimer_->start(std::max(1, int(1000 / hz)));
        clock_.start();
        view_->viewport()->setCursor(Qt::SizeVerCursor);
        view_->viewport()->update();
    }
    void autoScrollStep()
    {
        const double dt = double(clock_.nsecsElapsed()) / 1e9;
        clock_.restart();
        QScrollBar* bar = view_->verticalScrollBar();
        auto_.pos = std::clamp(auto_.pos + autoScrollSpeed(float(auto_.y - auto_.anchor.y())) * dt,
                               double(bar->minimum()), double(bar->maximum()));
        bar->setValue(int(std::lround(auto_.pos)));
    }
    void stopAutoScroll()
    {
        if (!auto_.on) return;
        if (autoTimer_) autoTimer_->stop();
        auto_ = {};
        view_->viewport()->unsetCursor();
        view_->viewport()->update();
    }

    static std::set<WList*>& lists()  // lists that autoscroll, for Esc
    {
        static std::set<WList*> l;
        return l;
    }
    static inline WList* active_ = nullptr;  // the list a drag is in

    ListView* view_;
    RowsModel* rows_;
    RowDelegate* delegate_;
    ListModel* model_ = nullptr;
    std::vector<Column> cols_;
    bool compact_;
    bool autoScroll_;
    std::optional<SoftColors> soft_;
    int firstTextColumn_ = 0;
    int rowHeight_ = 20, iconSize_ = 16;
    bool fitted_ = false;
    mutable std::map<int, std::pair<QFont, QFontMetrics>> fonts_;
    std::optional<size_t> sortColumn_;
    SortOrder sortOrder_ = SortOrder::Ascending;

    std::function<void()> selChanged_;
    std::function<void(size_t)> activate_;
    std::function<void(size_t, SortOrder)> sort_;
    std::function<Menu(std::optional<size_t>)> contextMenu_;
    std::function<Menu(size_t)> headerMenu_;
    std::function<Menu(size_t, size_t, PointF)> cellMenu_;
    ReorderFn reorder_;
    bool reorderable_ = true;
    CellDrag cellDrag_;
    Drag drag_;
    CellDragState cell_;
    std::optional<int> dropY_;
    std::optional<size_t> hot_;  // the row under the pointer

    struct AutoScroll
    {
        bool on = false;
        QPoint anchor;
        int y = 0;       // the pointer
        double pos = 0;  // scroll position, with the fraction a step leaves
    };
    AutoScroll auto_;
    QTimer* autoTimer_ = nullptr;
    QElapsedTimer clock_;
};

void ListView::drawRow(QPainter* p, const QStyleOptionViewItem& option,
                       const QModelIndex& index) const
{
    if (owner) owner->paintRow(p, option.rect, index.row());
}

void ListView::paintEvent(QPaintEvent* e)
{
    QTreeView::paintEvent(e);
    if (!owner || !owner->overlaid()) return;
    QPainter p(viewport());
    owner->paintOverlay(p);
}

void ListView::mousePressEvent(QMouseEvent* e)
{
    owner ? owner->mousePress(e) : QTreeView::mousePressEvent(e);
}

void ListView::mouseMoveEvent(QMouseEvent* e)
{
    owner ? owner->mouseMove(e) : QTreeView::mouseMoveEvent(e);
}

void ListView::mouseReleaseEvent(QMouseEvent* e)
{
    owner ? owner->mouseRelease(e) : QTreeView::mouseReleaseEvent(e);
}

void ListView::mouseDoubleClickEvent(QMouseEvent* e)
{
    owner ? owner->mouseDoubleClick(e) : QTreeView::mouseDoubleClickEvent(e);
}

void ListView::keyPressEvent(QKeyEvent* e)
{
    owner ? owner->keyPress(e) : QTreeView::keyPressEvent(e);
}

void ListView::contextMenuEvent(QContextMenuEvent* e)
{
    if (owner) owner->contextMenu(e);
}

bool ListView::viewportEvent(QEvent* e)
{
    if (e->type() == QEvent::Leave && owner) owner->leave();
    return QTreeView::viewportEvent(e);
}

void ListView::resizeEvent(QResizeEvent* e)
{
    QTreeView::resizeEvent(e);
    if (owner) owner->fitStretch();
}

void ListView::changeEvent(QEvent* e)
{
    QTreeView::changeEvent(e);
    if (!owner) return;
    if (e->type() == QEvent::FontChange) owner->fontsChanged();
    if (e->type() == QEvent::PaletteChange) owner->paletteChanged();
}

void ListView::scrollContentsBy(int dx, int dy)
{
    QTreeView::scrollContentsBy(dx, dy);
    if (owner && owner->overlaid()) viewport()->update();
}

// ---------------------------------------------------------------- window

class WWindow;

// Every window is a QDialog, for its keys: Enter presses the default
// button, Esc asks to end a dialog. Closing goes through the WWindow.
class Frame : public QDialog
{
public:
    using QDialog::QDialog;
    WWindow* owner = nullptr;
    void reject() override;
    void accept() override {}

protected:
    void closeEvent(QCloseEvent* e) override;
    void changeEvent(QEvent* e) override;
};

// Keys reach the window's onKey before the focused widget: an application
// event filter that takes them as they're sent to the focus widget.
class KeyFilter : public QObject
{
public:
    WWindow* owner = nullptr;
    bool eventFilter(QObject* o, QEvent* e) override;
};

class WWindow : public anyui::Window
{
public:
    WWindow(const WindowSpec& spec, WWindow* parent)
        : dialog_(spec.dialog)
    {
        frame_ = new Frame(parent ? parent->frame_.data() : nullptr);
        frame_->owner = this;
        frame_->setWindowTitle(qs(spec.title));
        frame_->setWindowFlags(dialog_ ? Qt::Dialog : Qt::Window);
        if (dialog_)
            frame_->setWindowModality(parent && spec.modal ? Qt::WindowModal : Qt::NonModal);
        // Snapshot runs are automated: never take the user's keyboard focus.
        if (const char* snap = std::getenv("ANYUI_SNAPSHOT"); snap && *snap)
            frame_->setAttribute(Qt::WA_ShowWithoutActivating);
        layout_ = new QVBoxLayout(frame_);
        layout_->setSpacing(0);
        if (!spec.resizable) layout_->setSizeConstraint(QLayout::SetFixedSize);
        filter_ = new KeyFilter;
        filter_->owner = this;
        qApp->installEventFilter(filter_);
    }

    // Qt widgets go later (deleteLater), as this may run inside one of
    // their signals; until then nothing may reach the parts going now.
    ~WWindow() override
    {
        filter_->owner = nullptr;
        if (qApp) qApp->removeEventFilter(filter_);
        filter_->deleteLater();
        if (lastShown() == frame_) lastShown() = nullptr;
        if (frame_)
        {
            frame_->owner = nullptr;
            for (QObject* o : frame_->findChildren<QObject*>())
            {
                if (auto* h = dynamic_cast<EventHook*>(o)) h->disarm();
                o->blockSignals(true);
            }
            frame_->hide();
            frame_->deleteLater();
        }
        parts.clear();
    }

    void setContent(QWidget* content, const WindowSpec& spec)
    {
        const int m = !spec.margins ? 0 : dialog_ ? 12 : 6;
        layout_->setContentsMargins(m, m, m, m);
        layout_->addWidget(content);
        if (!spec.menuBar.empty()) buildMenuBar(spec.menuBar);
        const int w = px(spec.width), h = px(spec.height);
        if (w > 0 || h > 0)
        {
            const QSize hint = frame_->sizeHint().expandedTo(frame_->minimumSizeHint());
            frame_->resize(w > 0 ? w : hint.width(), h > 0 ? h : hint.height());
        }
    }

    void setTitle(const std::string& t) override { frame_->setWindowTitle(qs(t)); }
    void show() override
    {
        finished_ = false;
        frame_->show();
        if (!frame_->testAttribute(Qt::WA_ShowWithoutActivating))
        {
            frame_->raise();
            frame_->activateWindow();
        }
        snapshotIfAsked();
    }
    void close() override { frame_->hide(); }
    void onFinished(std::function<void(DialogRole)> f) override { onFinished_ = std::move(f); }
    void onCloseRequest(std::function<bool()> f) override { onCloseRequest_ = std::move(f); }

    SizeF size() const override { return {float(frame_->width()), float(frame_->height())}; }
    void resize(SizeF s) override
    {
        frame_->resize(std::max(1, int(s.w)), std::max(1, int(s.h)));
        keepOnScreen();
    }
    void setMinSize(SizeF s) override { frame_->setMinimumSize(int(s.w), int(s.h)); }
    std::optional<PointF> position() const override
    {
        if (!placeable()) return std::nullopt;
        const QPoint p = frame_->frameGeometry().topLeft();
        return PointF{float(p.x()), float(p.y())};
    }
    void move(PointF p) override
    {
        if (!placeable()) return;
        frame_->move(int(p.x), int(p.y));
        keepOnScreen();
    }
    RectF workArea() const override
    {
        const QScreen* s = frame_->screen() ? frame_->screen() : QGuiApplication::primaryScreen();
        if (!s) return {};
        const QRect r = s->availableGeometry();
        return withoutWindowsTaskbar(
            {float(r.x()), float(r.y()), float(r.width()), float(r.height())});
    }

    bool fullscreen() const override { return frame_->isFullScreen(); }
    void setFullscreen(bool on) override
    {
        if (on == frame_->isFullScreen()) return;
        if (on)
        {
            wasMaximized_ = frame_->isMaximized();
            frame_->showFullScreen();
        }
        else if (wasMaximized_)
            frame_->showMaximized();
        else
            frame_->showNormal();
    }
    void onFullscreenChanged(std::function<void(bool)> f) override { onFullscreen_ = std::move(f); }

    void setIcon(const Icon& icon) override
    {
        if (!icon.name.empty())
            if (QIcon named = themeIcon(icon.name); !named.isNull())
            {
                frame_->setWindowIcon(named);
                return;
            }
        if (icon.draw) frame_->setWindowIcon(drawnIcon(icon.draw, 256, frame_));
    }

    void setMenuBarVisible(bool on) override
    {
        menuBarWanted_ = on;
        showMenuBar();
    }

    void onKey(std::function<bool(const KeyEvent&)> f) override { onKey_ = std::move(f); }

    // A dialog button with no handler of its own ends the dialog with its
    // role; OK and Yes are the default, which Enter presses.
    void addRoleButton(QPushButton* b, DialogRole role)
    {
        QObject::connect(b, &QPushButton::clicked, b, [this, role] { finish(role); });
        if (role == DialogRole::Ok || role == DialogRole::Yes) b->setDefault(true);
    }

    QWidget* frame() const { return frame_; }

    std::vector<std::unique_ptr<Part>> parts;

    // ---- from Frame and KeyFilter

    void escape()
    {
        if (dialog_) closeRequested();
    }

    void closeRequested()
    {
        if (onCloseRequest_ && !onCloseRequest_()) return;
        if (dialog_)
            finish(DialogRole::Cancel);
        else
            close();
    }

    void stateChanged()
    {
        const bool fs = frame_->isFullScreen();
        if (fs == fullscreen_) return;
        fullscreen_ = fs;
        showMenuBar();
        if (onFullscreen_) onFullscreen_(fs);
    }

    bool filterKey(QObject* o, QEvent* e)
    {
        if (e->type() != QEvent::KeyPress && e->type() != QEvent::KeyRelease) return false;
        auto* w = qobject_cast<QWidget*>(o);
        QWidget* focus = QApplication::focusWidget();
        if (!w || w != (focus ? focus : frame_.data()) || w->window() != frame_) return false;
        auto* k = static_cast<QKeyEvent*>(e);
        if (k->key() == Qt::Key_Escape && e->type() == QEvent::KeyPress &&
            WList::cancelActiveDrag())
            return true;
        if (!onKey_) return false;
        // While typing, only Escape and the function keys come here.
        auto* plain = qobject_cast<QPlainTextEdit*>(focus);
        const bool typing =
            focus && (qobject_cast<QLineEdit*>(focus) || qobject_cast<QAbstractSpinBox*>(focus) ||
                      (plain && !plain->isReadOnly()));
        const bool notText =
            k->key() == Qt::Key_Escape || (k->key() >= Qt::Key_F1 && k->key() <= Qt::Key_F12);
        if (typing && !notText) return false;
        KeyEvent ev;
        ev.press = e->type() == QEvent::KeyPress;
        ev.key = keyFor(k->key());
        if (ev.key == Key::Character) ev.codepoint = char32_t(QChar::toLower(char32_t(k->key())));
        ev.modifiers = modifiersOf(k->modifiers());
        ev.repeat = k->isAutoRepeat();
        return onKey_(ev);
    }

private:
    // Development aid: ANYUI_SNAPSHOT=file.png saves the window shown last,
    // ANYUI_SNAPSHOT_DELAY ms (default 500) after it appears, then quits.
    static QPointer<QWidget>& lastShown()
    {
        static QPointer<QWidget> w;
        return w;
    }

    void snapshotIfAsked()
    {
        lastShown() = frame_.data();
        static bool taken = false;
        const char* path = std::getenv("ANYUI_SNAPSHOT");
        if (taken || !path || !*path) return;
        taken = true;
        const char* delay = std::getenv("ANYUI_SNAPSHOT_DELAY");
        QTimer::singleShot(delay ? std::atoi(delay) : 500, qApp,
                           []
                           {
                               if (QWidget* w = lastShown())
                                   w->grab().save(qs(std::getenv("ANYUI_SNAPSHOT")));
                               QCoreApplication::exit(0);
                           });
    }

    // Only X11 (and Windows) let apps place their windows.
    static bool placeable() { return QGuiApplication::platformName() != "wayland"; }

    // Keeps the whole frame in the work area: the size shrinks to fit, and
    // the position slides back inside, as on win32. Once shown.
    void keepOnScreen()
    {
        if (!frame_->isVisible() || !placeable()) return;
        const RectF wa = workArea();
        if (wa.w <= 0 || wa.h <= 0) return;
        const QRect f = frame_->frameGeometry();
        const int extraW = f.width() - frame_->width(), extraH = f.height() - frame_->height();
        const int w = std::min(frame_->width(), int(wa.w) - extraW);
        const int h = std::min(frame_->height(), int(wa.h) - extraH);
        if (w != frame_->width() || h != frame_->height())
            frame_->resize(std::max(1, w), std::max(1, h));
        const int x =
            std::clamp(f.x(), int(wa.x), std::max(int(wa.x), int(wa.x + wa.w) - (w + extraW)));
        const int y =
            std::clamp(f.y(), int(wa.y), std::max(int(wa.y), int(wa.y + wa.h) - (h + extraH)));
        if (x != f.x() || y != f.y()) frame_->move(x, y);
    }

    // A menu bar's menus are rebuilt each time one opens, so they show the
    // app's state as it is.
    void buildMenuBar(const std::vector<MenuBarItem>& menus)
    {
        menuBar_ = new QMenuBar(frame_);
        for (const MenuBarItem& item : menus)
        {
            QMenu* menu = menuBar_->addMenu(mnemonic(item.title));
            QObject::connect(menu, &QMenu::aboutToShow, menu,
                             [menu, fn = item.menu]
                             {
                                 menu->clear();
                                 if (fn) fillMenu(menu, fn());
                             });
        }
        layout_->setMenuBar(menuBar_);
    }

    void showMenuBar()
    {
        if (menuBar_) menuBar_->setVisible(menuBarWanted_ && !fullscreen_);
    }

    // The callback may delete this window, so it runs last.
    void finish(DialogRole role)
    {
        close();
        if (finished_) return;
        finished_ = true;
        if (auto f = onFinished_) f(role);
    }

    QPointer<Frame> frame_;
    QVBoxLayout* layout_;
    KeyFilter* filter_;
    QMenuBar* menuBar_ = nullptr;
    bool menuBarWanted_ = true;
    bool dialog_;
    bool finished_ = false;
    bool fullscreen_ = false;
    bool wasMaximized_ = false;
    std::function<void(DialogRole)> onFinished_;
    std::function<bool()> onCloseRequest_;
    std::function<void(bool)> onFullscreen_;
    std::function<bool(const KeyEvent&)> onKey_;
};

void Frame::reject()
{
    if (owner) owner->escape();
}

void Frame::closeEvent(QCloseEvent* e)
{
    e->ignore();
    if (owner) owner->closeRequested();
}

void Frame::changeEvent(QEvent* e)
{
    QDialog::changeEvent(e);
    if (e->type() == QEvent::WindowStateChange && owner) owner->stateChanged();
}

bool KeyFilter::eventFilter(QObject* o, QEvent* e)
{
    return owner && owner->filterKey(o, e);
}

// ---------------------------------------------------------------- builder

class Builder
{
public:
    Builder(WWindow& win, BuildSink& sink)
        : win_(win)
        , sink_(sink)
    {
    }

    QWidget* build(const Node& n, Orientation parentDir)
    {
        QWidget* w = std::visit([&](const auto& s) { return make(n, s, parentDir); }, n.spec);
        // Containers have no interface; one with a handle gets a plain Widget.
        const bool container = std::holds_alternative<spec::Box>(n.spec) ||
                               std::holds_alternative<spec::Form>(n.spec) ||
                               std::holds_alternative<spec::Group>(n.spec) ||
                               std::holds_alternative<spec::Scroll>(n.spec) ||
                               std::holds_alternative<spec::Toolbar>(n.spec);
        if (container && n.common.handle) add<WPlain>(n, w);
        if (int width = px(n.common.width); width > 0) w->setMinimumWidth(width);
        if (int height = px(n.common.height); height > 0) w->setMinimumHeight(height);
        return w;
    }

private:
    static bool isLineControl(const Spec& s)
    {
        return std::holds_alternative<spec::Label>(s) || std::holds_alternative<spec::Button>(s) ||
               std::holds_alternative<spec::Check>(s) || std::holds_alternative<spec::Entry>(s) ||
               std::holds_alternative<spec::Select>(s) || std::holds_alternative<spec::Combo>(s) ||
               std::holds_alternative<spec::MenuButton>(s) ||
               std::holds_alternative<spec::ProgressBar>(s) ||
               std::holds_alternative<spec::DatePicker>(s);
    }

    static bool fillsFormRow(const Spec& s)
    {
        return std::holds_alternative<spec::Entry>(s) || std::holds_alternative<spec::Combo>(s) ||
               std::holds_alternative<spec::TextView>(s) || std::holds_alternative<spec::List>(s) ||
               std::holds_alternative<spec::Canvas>(s) || std::holds_alternative<spec::Box>(s) ||
               std::holds_alternative<spec::Form>(s) || std::holds_alternative<spec::Splitter>(s) ||
               std::holds_alternative<spec::Group>(s) || std::holds_alternative<spec::Scroll>(s) ||
               std::holds_alternative<spec::Tabs>(s);
    }

    template <class T, class... Args>
    T& add(const Node& n, Args&&... args)
    {
        auto* w = new T(std::forward<Args>(args)...);
        win_.parts.emplace_back(w);
        sink_.created(n, *w);
        return *w;
    }

    std::function<QWidget*(const Node&)> pageBuilder()
    {
        return [&win = win_, &sink = sink_](const Node& page)
        { return Builder(win, sink).build(page, Orientation::Vertical); };
    }

    // A page: its content with the usual margin around it.
    static QWidget* pageOf(QWidget* content)
    {
        auto* page = new QWidget;
        auto* l = new QVBoxLayout(page);
        l->setContentsMargins(6, 6, 6, 6);
        if (content) l->addWidget(content);
        return page;
    }

    QWidget* make(const Node&, const spec::Box& s, Orientation)
    {
        auto* box = new QWidget;
        auto* l = new QBoxLayout(s.dir == Orientation::Vertical ? QBoxLayout::TopToBottom
                                                                : QBoxLayout::LeftToRight,
                                 box);
        l->setSpacing(6);
        if (s.margins)
        {
            // A bar along a window edge: the full margin at its ends, half along its sides.
            const bool row = s.dir == Orientation::Horizontal;
            l->setContentsMargins(row ? 6 : 3, row ? 3 : 6, row ? 6 : 3, row ? 3 : 6);
        }
        else
            l->setContentsMargins(0, 0, 0, 0);
        for (auto& c : s.items)
        {
            QWidget* w = build(c, s.dir);
            // Single-line controls keep their natural height when a taller
            // sibling sets the row height.
            const Qt::Alignment align = s.dir == Orientation::Horizontal && isLineControl(c.spec)
                                            ? Qt::AlignVCenter
                                            : Qt::Alignment();
            l->addWidget(w, c.common.stretch, align);
        }
        return box;
    }

    QWidget* make(const Node&, const spec::Form& s, Orientation)
    {
        auto* form = new QWidget;
        auto* l = new QFormLayout(form);
        l->setContentsMargins(0, 0, 0, 0);
        l->setHorizontalSpacing(12);
        l->setVerticalSpacing(6);
        l->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        // Text fields and containers fill the row; selects, spins and the
        // like keep their natural width (as in Windows dialogs) unless the
        // node asks to stretch.
        l->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
        for (size_t i = 0; i < s.fields.size(); ++i)
        {
            QWidget* field = build(s.fields[i], Orientation::Horizontal);
            QSizePolicy policy = field->sizePolicy();
            if (fillsFormRow(s.fields[i].spec) || s.fields[i].common.stretch > 0)
                policy.setHorizontalPolicy(QSizePolicy::Expanding);
            else if (policy.horizontalPolicy() & QSizePolicy::ExpandFlag)
                policy.setHorizontalPolicy(QSizePolicy::Preferred);
            field->setSizePolicy(policy);
            if (s.labels[i].empty())
                l->addRow(field);
            else
            {
                auto* label = new QLabel(qs(s.labels[i]));
                label->setTextFormat(Qt::PlainText);
                l->addRow(label, field);
            }
        }
        return form;
    }

    QWidget* make(const Node& n, const spec::Splitter& s, Orientation)
    {
        auto* sp = new QSplitter(s.dir == Orientation::Vertical ? Qt::Vertical : Qt::Horizontal);
        sp->setChildrenCollapsible(false);
        std::vector<int> stretch;
        for (auto& pane : s.panes)
        {
            sp->addWidget(build(pane, s.dir));
            stretch.push_back(pane.common.stretch);
        }
        // Spare space goes to stretching panes only (to all, when none do).
        if (std::ranges::all_of(stretch, [](int f) { return f == 0; }))
            std::ranges::fill(stretch, 1);
        for (size_t i = 0; i < stretch.size(); ++i) sp->setStretchFactor(int(i), stretch[i]);
        add<WSplitter>(n, sp, stretch);
        return sp;
    }

    QWidget* make(const Node&, const spec::Scroll& s, Orientation)
    {
        auto* area = new QScrollArea;
        area->setFrameShape(QFrame::NoFrame);
        area->setWidgetResizable(true);
        area->setHorizontalScrollBarPolicy(s.horizontal ? Qt::ScrollBarAsNeeded
                                                        : Qt::ScrollBarAlwaysOff);
        if (!s.content.empty()) area->setWidget(build(s.content.front(), Orientation::Vertical));
        return area;
    }

    QWidget* make(const Node&, const spec::Group& s, Orientation)
    {
        auto* group = new QGroupBox(literal(s.title));
        auto* l = new QVBoxLayout(group);
        l->setContentsMargins(6, 6, 6, 6);
        if (!s.content.empty()) l->addWidget(build(s.content.front(), Orientation::Vertical));
        return group;
    }

    QWidget* make(const Node& n, const spec::Tabs& s, Orientation)
    {
        if (s.sidebar) return makeSidebar(n, s);
        auto* tabs = new QTabWidget;
        LazyPages lazy{s.lazy, {}, pageBuilder()};
        for (size_t i = 0; i < s.pages.size(); ++i)
        {
            const bool isLazy = i < s.lazy.size() && s.lazy[i];
            QWidget* page = pageOf(isLazy ? nullptr : build(s.pages[i], Orientation::Vertical));
            tabs->addTab(page, mnemonic(s.titles[i]));
            lazy.pages.push_back(page);
        }
        auto& w = add<WTabs>(n, tabs);
        if (!s.lazy.empty()) w.setLazy(std::move(lazy));
        return tabs;
    }

    // Tabs as a list of page names down the left, the page in a stack beside it.
    QWidget* makeSidebar(const Node& n, const spec::Tabs& s)
    {
        auto* box = new QWidget;
        auto* l = new QHBoxLayout(box);
        l->setContentsMargins(0, 0, 0, 0);
        l->setSpacing(0);
        auto* list = new QListWidget;
        auto* stack = new QStackedWidget;
        LazyPages lazy{s.lazy, {}, pageBuilder()};
        int width = 0;
        for (size_t i = 0; i < s.pages.size(); ++i)
        {
            auto* item = new QListWidgetItem(qs(s.titles[i]).remove('_'), list);
            item->setSizeHint(QSize(0, int(g_metrics.lineHeight) + 16));
            width = std::max(width, list->fontMetrics().horizontalAdvance(item->text()));
            const bool isLazy = i < s.lazy.size() && s.lazy[i];
            QWidget* page = pageOf(isLazy ? nullptr : build(s.pages[i], Orientation::Vertical));
            stack->addWidget(page);
            lazy.pages.push_back(page);
        }
        list->setFixedWidth(width + 48);
        list->setStyleSheet("QListWidget::item { padding-left: 12px; padding-right: 12px; }");
        l->addWidget(list);
        auto* line = new QFrame;
        line->setFrameShape(QFrame::VLine);
        line->setFrameShadow(QFrame::Sunken);
        l->addWidget(line);
        l->addWidget(stack, 1);
        auto& tabs = add<WSideTabs>(n, box, list, stack);
        tabs.setCurrent(0);
        if (!s.lazy.empty()) tabs.setLazy(std::move(lazy));
        return box;
    }

    QWidget* make(const Node&, const spec::Spacer&, Orientation)
    {
        auto* w = new QWidget;
        w->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        return w;
    }

    QWidget* make(const Node&, const spec::Separator&, Orientation parentDir)
    {
        auto* line = new QFrame;
        line->setFrameShape(parentDir == Orientation::Vertical ? QFrame::HLine : QFrame::VLine);
        line->setFrameShadow(QFrame::Sunken);
        return line;
    }

    QWidget* make(const Node& n, const spec::Label& s, Orientation)
    {
        return add<WLabel>(n, s).widget();
    }
    QWidget* make(const Node& n, const spec::MenuButton& s, Orientation)
    {
        return add<WMenuButton>(n, s).widget();
    }
    QWidget* make(const Node& n, const spec::DatePicker& s, Orientation)
    {
        return add<WDatePicker>(n, s).widget();
    }
    QWidget* make(const Node& n, const spec::Check& s, Orientation)
    {
        return add<WCheck>(n, s).widget();
    }
    QWidget* make(const Node& n, const spec::Entry& s, Orientation)
    {
        return add<WEntry>(n, s).widget();
    }
    QWidget* make(const Node& n, const spec::Select& s, Orientation)
    {
        return add<WSelect>(n, s).widget();
    }
    QWidget* make(const Node& n, const spec::Combo& s, Orientation)
    {
        return add<WCombo>(n, s).widget();
    }
    QWidget* make(const Node& n, const spec::TextView& s, Orientation)
    {
        return add<WTextView>(n, s).widget();
    }
    QWidget* make(const Node& n, const spec::List& s, Orientation)
    {
        return add<WList>(n, s).widget();
    }
    QWidget* make(const Node& n, const spec::ProgressBar&, Orientation)
    {
        return add<WProgressBar>(n).widget();
    }

    QWidget* make(const Node& n, const spec::Button& s, Orientation)
    {
        auto* b = new QPushButton(s.iconOnly ? QString() : mnemonic(s.text));
        auto& w = add<WButton>(n, b);
        if (s.icon) w.setIcon(s.icon);
        if (s.iconOnly && n.common.tooltip.empty()) w.setTooltip(s.text);
        b->setFlat(s.flat);
        return b;
    }

    QWidget* make(const Node& n, const spec::ToolButton& s, Orientation)
    {
        return add<WToolButton>(n, s).widget();
    }
    // A QToolBar: separators as its own, stretching spacers as expanding gaps.
    QWidget* make(const Node&, const spec::Toolbar& s, Orientation)
    {
        auto* bar = new QToolBar;
        bar->setMovable(false);
        bar->setFloatable(false);
        bar->setIconSize(QSize(16, 16));
        bar->setToolButtonStyle(Qt::ToolButtonIconOnly);
        for (const Node& item : s.items)
        {
            if (std::holds_alternative<spec::Separator>(item.spec))
            {
                bar->addSeparator();
                continue;
            }
            if (std::holds_alternative<spec::Spacer>(item.spec))
            {
                auto* gap = new QWidget;
                gap->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
                bar->addWidget(gap);
                continue;
            }
            QWidget* w = build(item, Orientation::Horizontal);
            QAction* action = bar->addWidget(w);
            w->setProperty("anyui-toolbar-action", QVariant::fromValue<QObject*>(action));
        }
        return bar;
    }
    QWidget* make(const Node& n, const spec::Slider& s, Orientation)
    {
        return add<WSlider>(n, s).widget();
    }
    QWidget* make(const Node& n, const spec::Spin& s, Orientation)
    {
        return add<WSpin>(n, s).widget();
    }
    QWidget* make(const Node& n, const spec::ColorButton& s, Orientation)
    {
        return add<WColorButton>(n, s).widget();
    }
    QWidget* make(const Node& n, const spec::Canvas& s, Orientation)
    {
        return add<WCanvas>(n, s).widget();
    }

    // QDialogButtonBox puts the roles in the platform's order, with its
    // stock labels (Help at the far start).
    static QDialogButtonBox::StandardButton standardFor(DialogRole r)
    {
        switch (r)
        {
        case DialogRole::Ok: return QDialogButtonBox::Ok;
        case DialogRole::Cancel: return QDialogButtonBox::Cancel;
        case DialogRole::Apply: return QDialogButtonBox::Apply;
        case DialogRole::Close: return QDialogButtonBox::Close;
        case DialogRole::Yes: return QDialogButtonBox::Yes;
        case DialogRole::No: return QDialogButtonBox::No;
        case DialogRole::Help: return QDialogButtonBox::Help;
        case DialogRole::Reset: return QDialogButtonBox::Reset;
        case DialogRole::None: break;
        }
        return QDialogButtonBox::NoButton;
    }

    QWidget* make(const Node&, const spec::Buttons& s, Orientation)
    {
        auto* box = new QDialogButtonBox;
        box->setContentsMargins(0, 6, 0, 0);
        for (auto& b : s.buttons)
        {
            const auto standard = standardFor(b.role);
            QPushButton* pb = standard == QDialogButtonBox::NoButton
                                  ? box->addButton(mnemonic(b.text), QDialogButtonBox::ActionRole)
                                  : box->addButton(standard);
            if (!b.text.empty()) pb->setText(mnemonic(b.text));
            pb->setIcon(QIcon());  // stock labels only, as on GTK and Windows
            auto* w = new WButton(pb);
            win_.parts.emplace_back(w);
            if (!b.onClick) win_.addRoleButton(pb, b.role);
            sink_.created(b, *w);
        }
        return box;
    }

    WWindow& win_;
    BuildSink& sink_;
};

// ---------------------------------------------------------------- backend

class Qt6Backend : public Backend
{
public:
    static Backend* create(int* argc, char** argv)
    {
        QApplication* app = nullptr;  // one made here, deleted with the backend
        if (!qApp)
        {
#if defined(__linux__) || defined(__FreeBSD__)
            // Qt aborts when it has no display; report it so the next backend can try.
            if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY") &&
                !std::getenv("QT_QPA_PLATFORM"))
                return nullptr;
            // Under WSLg, Wayland gives an app no window positions, so nothing
            // could keep a window on screen; its X server does.
            if (underWsl() && std::getenv("DISPLAY") && !std::getenv("QT_QPA_PLATFORM"))
                qputenv("QT_QPA_PLATFORM", "xcb");
            // With XInput 2, WSLg shows no pointer over Qt's windows (hover
            // and clicks still work). Core X input shows it; WSLg's mouse
            // comes through RDP, so XI2's touch and smooth scrolling add
            // nothing there.
            if (underWsl() && !std::getenv("QT_XCB_NO_XI2")) qputenv("QT_XCB_NO_XI2", "1");
#endif
            // Keeps argc: the caller's, alive for the program.
            app = new QApplication(*argc, argv);
        }
        // Outside a desktop that sets one, Qt's icon theme is just hicolor,
        // which has no MIME or action icons.
        if (QIcon::themeName().isEmpty() || QIcon::themeName() == "hicolor")
            for (const char* theme : {"breeze", "Adwaita", "Yaru"})
                if (QDir(QString("/usr/share/icons/") + theme).exists())
                {
                    QIcon::setThemeName(theme);
                    break;
                }
        initMetrics();
        return new Qt6Backend(app);
    }

    // Windows closed once the loop had ended left their widgets to
    // deleteLater, which no loop will run: they go now, and the
    // QApplication before static teardown, where Qt can no longer release
    // the GL contexts its windows' backing stores made (xcb uses GLX).
    ~Qt6Backend() override
    {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        delete app_;
    }

    std::string_view name() const override { return "qt6"; }

    std::unique_ptr<anyui::Window> createWindow(const WindowSpec& spec, anyui::Window* parent,
                                                BuildSink& sink) override
    {
        auto win = std::make_unique<WWindow>(spec, static_cast<WWindow*>(parent));
        win->setContent(Builder(*win, sink).build(spec.content, Orientation::Vertical), spec);
        return win;
    }

    int run() override
    {
        if (quit_) return code_;
        running_ = true;
        QApplication::exec();
        running_ = false;
        return code_;
    }

    void quit(int code) override
    {
        quit_ = true;
        code_ = code;
        if (running_) QCoreApplication::exit(code);
    }

    void post(std::function<void()> f) override
    {
        QMetaObject::invokeMethod(qApp, [f = std::move(f)] { f(); }, Qt::QueuedConnection);
    }

    std::unique_ptr<anyui::Image> createImage(int w, int h, PixelFormat format,
                                              const uint8_t* pixels, size_t stride) override
    {
        return std::make_unique<QtImage>(w, h, format, pixels, stride);
    }

    // `fn` may drop its own subscription; the timer then goes once it returns.
    Subscription timer(int ms, bool repeat, std::function<void()> fn) override
    {
        auto* t = new QTimer;
        t->setSingleShot(!repeat);
        t->setTimerType(Qt::PreciseTimer);
        QObject::connect(t, &QTimer::timeout, t,
                         [t, repeat, fn = std::move(fn)]
                         {
                             if (!repeat) t->deleteLater();
                             fn();
                         });
        t->start(std::max(ms, 0));
        return Subscription(
            [p = QPointer<QTimer>(t)]
            {
                if (!p) return;
                p->stop();
                p->deleteLater();
            });
    }

    void message(anyui::Window* parent, const MessageSpec& m,
                 std::function<void(DialogRole)> done) override
    {
        auto* box = new QMessageBox(parentWidget(parent));
        box->setAttribute(Qt::WA_DeleteOnClose);
        switch (m.icon)
        {
        case MessageSpec::Icon::Info: box->setIcon(QMessageBox::Information); break;
        case MessageSpec::Icon::Warning: box->setIcon(QMessageBox::Warning); break;
        case MessageSpec::Icon::Error: box->setIcon(QMessageBox::Critical); break;
        case MessageSpec::Icon::Question: box->setIcon(QMessageBox::Question); break;
        }
        if (!m.title.empty()) box->setWindowTitle(qs(m.title));
        box->setTextFormat(Qt::PlainText);
        box->setText(qs(m.text));
        if (!m.detail.empty()) box->setInformativeText(qs(m.detail));
        const auto roles = m.buttons.empty() ? std::vector<DialogRole>{DialogRole::Ok} : m.buttons;
        auto buttons = std::make_shared<std::vector<std::pair<QAbstractButton*, DialogRole>>>();
        for (DialogRole r : roles) buttons->push_back({box->addButton(messageButton(r)), r});
        QObject::connect(box, &QMessageBox::finished, box,
                         [box, buttons, done = std::move(done)](int)
                         {
                             DialogRole role = DialogRole::Cancel;
                             for (auto& [b, r] : *buttons)
                                 if (b == box->clickedButton()) role = r;
                             if (done) done(role);
                         });
        box->setWindowModality(parent ? Qt::WindowModal : Qt::ApplicationModal);
        box->show();
    }

    void chooseFiles(anyui::Window* parent, const FileDialogSpec& f,
                     std::function<void(std::vector<std::string>)> done) override
    {
        auto* dlg = new QFileDialog(parentWidget(parent), qs(f.title));
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        switch (f.mode)
        {
        case FileDialogSpec::Mode::Open: dlg->setFileMode(QFileDialog::ExistingFile); break;
        case FileDialogSpec::Mode::OpenMany: dlg->setFileMode(QFileDialog::ExistingFiles); break;
        case FileDialogSpec::Mode::Save:
            dlg->setFileMode(QFileDialog::AnyFile);
            dlg->setAcceptMode(QFileDialog::AcceptSave);
            break;
        case FileDialogSpec::Mode::Folder:
            dlg->setFileMode(QFileDialog::Directory);
            dlg->setOption(QFileDialog::ShowDirsOnly);
            break;
        }
        // "Patches (*.patch *.diff)"; a save typed without an extension gets
        // the chosen filter's, as on Windows.
        QStringList filters;
        auto extensions = std::make_shared<std::map<QString, QString>>();
        for (auto& flt : f.filters)
        {
            const QString name = qs(flt.name) + " (" + qs(flt.pattern).replace(';', ' ') + ")";
            filters << name;
            (*extensions)[name] = defaultExtension(flt.pattern);
        }
        if (!filters.isEmpty())
        {
            dlg->setNameFilters(filters);
            dlg->setDefaultSuffix((*extensions)[filters.front()]);
            QObject::connect(dlg, &QFileDialog::filterSelected, dlg,
                             [dlg, extensions](const QString& flt)
                             { dlg->setDefaultSuffix((*extensions)[flt]); });
        }
        if (!f.initialPath.empty())
        {
            const QFileInfo info(qs(f.initialPath));
            if (info.isDir())
                dlg->setDirectory(info.absoluteFilePath());
            else
            {
                dlg->setDirectory(info.absolutePath());
                dlg->selectFile(info.fileName());
            }
        }
        QObject::connect(dlg, &QFileDialog::finished, dlg,
                         [dlg, done = std::move(done)](int result)
                         {
                             std::vector<std::string> out;
                             if (result == QDialog::Accepted)
                                 for (const QString& s : dlg->selectedFiles())
                                     out.push_back(s.toStdString());
                             if (done) done(std::move(out));
                         });
        dlg->setWindowModality(parent ? Qt::WindowModal : Qt::ApplicationModal);
        dlg->show();
    }

    void setClipboard(const std::string& s) override
    {
        QGuiApplication::clipboard()->setText(qs(s));
    }

    void setClipboardImage(int w, int h, PixelFormat format, const uint8_t* pixels,
                           size_t stride) override
    {
        const QImage::Format f = format == PixelFormat::Rgba8 ? QImage::Format_RGBA8888
                                                              : QImage::Format_ARGB32_Premultiplied;
        // The pixels are the caller's: copied now.
        QGuiApplication::clipboard()->setImage(QImage(pixels, w, h, qsizetype(stride), f).copy());
    }

private:
    static QWidget* parentWidget(anyui::Window* w)
    {
        return w ? static_cast<WWindow*>(w)->frame() : nullptr;
    }

    static QMessageBox::StandardButton messageButton(DialogRole r)
    {
        switch (r)
        {
        case DialogRole::Ok: return QMessageBox::Ok;
        case DialogRole::Cancel: return QMessageBox::Cancel;
        case DialogRole::Apply: return QMessageBox::Apply;
        case DialogRole::Close: return QMessageBox::Close;
        case DialogRole::Yes: return QMessageBox::Yes;
        case DialogRole::No: return QMessageBox::No;
        case DialogRole::Help: return QMessageBox::Help;
        case DialogRole::Reset: return QMessageBox::Reset;
        case DialogRole::None: break;
        }
        return QMessageBox::Ok;
    }

    explicit Qt6Backend(QApplication* app)
        : app_(app)
    {
    }

    QApplication* app_;
    bool quit_ = false;
    bool running_ = false;
    int code_ = 0;
};

}  // namespace
}  // namespace anyui::qt6

ANYUI_BACKEND_ENTRY(qt6, anyui::qt6::Qt6Backend)
