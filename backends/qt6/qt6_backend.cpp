// Qt 6 backend: Qt Widgets for every control, lists as a QTreeView whose
// rows are painted here (as the win32 backend paints its list_view), text
// views as QPlainTextEdit, and windows as QDialogs (Enter presses the
// default button, Esc ends a dialog).
//
// No moc: signals go to lambdas, and events to overrides and event filters.
#include "rapport/backend.h"
#include "rapport/i18n.h"
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
#if RAPPORT_QT_PRINT
#include <QPrintDialog>
#include <QPrinter>
#endif
#include <QLibraryInfo>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
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
#include <QTranslator>
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
#include <QDragEnterEvent>
#include <QDropEvent>
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

namespace rap::qt6
{
namespace
{

// -- helpers ------------------------------------------------------------------

struct metrics
{
    float char_width = 8;
    float line_height = 17;
};
metrics g_metrics;

void init_metrics()
{
    const QFontMetricsF fm(QApplication::font());
    // The alphabet's mean, as Windows' dialog units and Pango's approximate
    // width have it; the font's own average is about a fifth narrower than
    // text, so fields sized in characters cut their text off.
    g_metrics.char_width =
        float(fm.horizontalAdvance("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ") / 52);
    g_metrics.line_height = float(fm.lineSpacing());
}

int px(extent e)
{
    switch (e.unit)
    {
    case extent::unit::chars: return int(e.value * g_metrics.char_width + 0.5f);
    case extent::unit::lines: return int(e.value * g_metrics.line_height + 0.5f);
    case extent::unit::px: return int(e.value);
    case extent::unit::auto_: break;
    }
    return -1;
}

QString qs(const std::string& s)
{
    return QString::fromUtf8(s.data(), qsizetype(s.size()));
}

// rapport's access keys ("_File", "__" for an underscore) as Qt's ("&File",
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

Qt::Alignment qt_align(align a)
{
    switch (a)
    {
    case align::center: return Qt::AlignHCenter;
    case align::end: return Qt::AlignRight;
    case align::start: break;
    }
    return Qt::AlignLeft;
}

// Running under WSL, where WSLg shows windows on the Windows desktop: its
// Wayland can't place windows, and the screen it reports includes the
// Windows taskbar.
bool under_wsl()
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
rect_f without_windows_taskbar(rect_f area)
{
    if (under_wsl()) area.h = std::max(0.f, area.h - 48);
    return area;
}

// The font for compact lists and text views: 90% of `f`, rounded to whole
// pixels so glyphs sit on the pixel grid.
QFont compact_font(const QFont& f)
{
    QFont c = f;
    c.setPixelSize(std::max(1, int(std::lround(QFontInfo(f).pixelSize() * 0.9))));
    return c;
}

QFont styled_font(QFont f, const text_style& s)
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

QTextCharFormat char_format(const QWidget* w, const text_style& s)
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
qsizetype utf16_at(const std::string& s, size_t byte)
{
    return qs(s.substr(0, std::min(byte, s.size()))).size();
}
size_t utf8_at(const QString& s, qsizetype pos)
{
    return size_t(QStringView(s).left(std::clamp<qsizetype>(pos, 0, s.size())).toUtf8().size());
}

// Qt's GTK 3 theme can give QComboBox a palette of its own whose text
// barely shows (white on near-white under Numix) while the application's
// is right. Where a combo's text is below 3:1 against its background
// (WCAG's floor for large text), it takes the application palette's roles:
// the theme's colours, not ours. Checked once polished, when the class
// palette arrives, and again whenever the theme or style changes.
double luminance(const QColor& c)
{
    auto channel = [](double v)
    { return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * channel(c.redF()) + 0.7152 * channel(c.greenF()) + 0.0722 * channel(c.blueF());
}

double contrast(const QColor& a, const QColor& b)
{
    const double la = luminance(a), lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

class readable_combo : public QObject
{
public:
    explicit readable_combo(QComboBox* c)
        : QObject(c)
        , combo_(c)
    {
        c->installEventFilter(this);
    }

    bool eventFilter(QObject*, QEvent* e) override
    {
        const auto t = e->type();
        if (t == QEvent::Polish || t == QEvent::ApplicationPaletteChange ||
            t == QEvent::StyleChange)
            QTimer::singleShot(0, this, [this] { check(); });
        return false;
    }

private:
    void check()
    {
        // The theme's palette again, then corrected only where it needs it.
        if (fixed_) combo_->setPalette(QPalette());
        fixed_ = false;
        QPalette p = combo_->palette();
        const QPalette app = QApplication::palette();
        for (auto group : {QPalette::Active, QPalette::Inactive})
            for (auto [text, back] : {std::pair{QPalette::ButtonText, QPalette::Button},
                                      std::pair{QPalette::Text, QPalette::Base}})
                if (contrast(p.color(group, text), p.color(group, back)) < 3)
                {
                    p.setColor(group, text, app.color(group, text));
                    if (contrast(p.color(group, text), p.color(group, back)) < 3)
                        p.setColor(group, back, app.color(group, back));
                    fixed_ = true;
                }
        if (fixed_) combo_->setPalette(p);
    }

    QComboBox* combo_;
    bool fixed_ = false;
};

// Selected rows in a light tint of the accent, with normal text: on light
// themes the accent's hue at Explorer's lightness, as on win32 (#0078D7
// becomes #CCE8FF); on dark ones a mix into the base colour, as on GTK.
struct soft_colors
{
    QColor selected, hover, hover_selected;
};

soft_colors soft_colors_for(const QWidget* w)
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
float auto_scroll_speed(float offset)
{
    const float dead = 8, d = std::abs(offset) - dead;
    if (d <= 0) return 0;
    return (offset < 0 ? -1.f : 1.f) * (d * 6 + d * d * 0.05f);
}

// The autoscroll anchor: a ring with up and down arrows, centred at (x, y).
void paint_auto_scroll_anchor(painter& p, float x, float y)
{
    p.circle({x, y}, 11, paint{theme_color::base}, paint{theme_color::mid});
    p.polygon({{x - 4, y - 3}, {x + 4, y - 3}, {x, y - 8}}, paint{theme_color::text}, std::nullopt,
              1);
    p.polygon({{x - 4, y + 3}, {x + 4, y + 3}, {x, y + 8}}, paint{theme_color::text}, std::nullopt,
              1);
    p.circle({x, y}, 1.5f, paint{theme_color::text}, std::nullopt);
}

// A file filter's extension for saving: the first glob's, if it's a plain
// "*.ext" ("*.patch;*.diff" -> "patch"; "*" -> none).
QString default_extension(const std::string& pattern)
{
    const std::string first = pattern.substr(0, pattern.find(';'));
    if (!first.starts_with("*.") || first.find_first_of("*?[", 2) != std::string::npos) return {};
    return qs(first.substr(2));
}

key key_for(int k)
{
    switch (k)
    {
    case Qt::Key_Escape: return key::escape;
    case Qt::Key_Return:
    case Qt::Key_Enter: return key::enter;
    case Qt::Key_Tab:
    case Qt::Key_Backtab: return key::tab;
    case Qt::Key_Backspace: return key::backspace;
    case Qt::Key_Delete: return key::delete_;
    case Qt::Key_Insert: return key::insert;
    case Qt::Key_Space: return key::space;
    case Qt::Key_Left: return key::left;
    case Qt::Key_Right: return key::right;
    case Qt::Key_Up: return key::up;
    case Qt::Key_Down: return key::down;
    case Qt::Key_Home: return key::home;
    case Qt::Key_End: return key::end;
    case Qt::Key_PageUp: return key::page_up;
    case Qt::Key_PageDown: return key::page_down;
    default: break;
    }
    if (k >= Qt::Key_F1 && k <= Qt::Key_F12) return key(int(key::f1) + (k - Qt::Key_F1));
    return k > 0x20 && k < 0x01000000 ? key::character : key::other;
}

unsigned modifiers_of(Qt::KeyboardModifiers m)
{
    unsigned out = 0;
    if (m & Qt::ShiftModifier) out |= mod_shift;
    if (m & Qt::ControlModifier) out |= mod_ctrl;
    if (m & Qt::AltModifier) out |= mod_alt;
    if (m & Qt::MetaModifier) out |= mod_meta;
    return out;
}

// Runs `fn` for each event `target` receives, ahead of the target; true
// consumes the event. Lives as long as the target, or until disarmed.
class event_hook : public QObject
{
public:
    using fn = std::function<bool(QEvent*)>;
    event_hook(QObject* target, fn fn)
        : QObject(target)
        , fn_(std::move(fn))
    {
        target->installEventFilter(this);
    }
    bool eventFilter(QObject*, QEvent* e) override { return fn_ && fn_(e); }
    void disarm() { fn_ = nullptr; }

private:
    fn fn_;
};

void hook(QObject* target, event_hook::fn fn)
{
    new event_hook(target, std::move(fn));
}

// -- icons --------------------------------------------------------------------

// The desktop's icon for a kind of file: the name's MIME type (a trailing
// '/' is a folder) and the icon theme's icon for it. Cached per extension.
QIcon file_type_icon(const std::string& name)
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

QIcon theme_icon(const std::string& name)
{
    static std::unordered_map<std::string, QIcon> cache;
    auto it = cache.find(name);
    if (it != cache.end()) return it->second;
    return cache[name] = QIcon::hasThemeIcon(qs(name)) ? QIcon::fromTheme(qs(name)) : QIcon();
}

// An app-drawn icon, painted at `w`'s device scale in its theme colours.
QIcon drawn_icon(const std::function<void(rap::painter&, rap::size_f)>& draw, int size, const QWidget* w)
{
    const qreal dpr = w ? w->devicePixelRatioF() : qApp->devicePixelRatio();
    QPixmap pm(QSize(size, size) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    {
        QPainter p(&pm);
        qt_painter painter(p, w);
        draw(painter, {float(size), float(size)});
    }
    return QIcon(pm);
}

// A named icon the theme has, else the file type's, else the drawing.
QIcon to_q_icon(const icon& icon, int size, const QWidget* w)
{
    if (!icon.name.empty())
    {
        if (QIcon named = theme_icon(icon.name); !named.isNull()) return named;
        // The theme may lack it (WSL's has no view-fullscreen or edit-copy).
        // Lists ask per cell per paint, so each is drawn once.
        static std::map<std::tuple<std::string, int, qreal>, QIcon> drawn;
        const qreal dpr = w ? w->devicePixelRatioF() : qApp->devicePixelRatio();
        auto [it, added] = drawn.try_emplace({icon.name, size, dpr});
        if (added)
        {
            bool known = false;
            QIcon i = drawn_icon([&](painter& p, size_f s) { known = builtin::draw_builtin_icon(icon.name, p, s); },
                                size, w);
            if (known) it->second = i;
        }
        if (!it->second.isNull()) return it->second;
    }
    if (!icon.file.empty()) return file_type_icon(icon.file);
    if (icon.draw) return drawn_icon(icon.draw, size, w);
    // A name nothing has: the theme's image-missing, as GTK shows, never a
    // blank icon-only button.
    if (!icon.name.empty()) return theme_icon("image-missing");
    return {};
}

// -- menus --------------------------------------------------------------------

// Appends `items` to `menu`. Each action keeps its own copy of what it runs,
// so the Menu needn't outlive the QMenu. Shortcuts are display hints (the
// window's on_key handles the keys), so they only work inside the menu.
void fill_menu(QMenu* menu, const rap::menu& items)
{
    QActionGroup* radios = nullptr;
    for (const menu_item& item : items)
    {
        if (!item.radio) radios = nullptr;
        if (item.is_separator())
        {
            menu->addSeparator();
            continue;
        }
        if (!item.children.empty())
        {
            QMenu* sub = menu->addMenu(mnemonic(item.label));
            sub->setEnabled(item.enabled);
            fill_menu(sub, item.children);
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
void show_popup_menu(menu items, QWidget* parent, QPoint global)
{
    if (items.empty()) return;
    auto* m = new QMenu(parent);
    fill_menu(m, items);
    QObject::connect(m, &QMenu::aboutToHide, m, &QObject::deleteLater);
    m->popup(global);
}

// -- widgets ------------------------------------------------------------------

class part
{
public:
    virtual ~part() = default;
};

// A widget's rapport interface over its Qt widget. The window deletes its Qt
// widgets later (deleteLater) than these, so Qt-side code that calls back
// into a part checks that it's still there.
template <class Base>
class widget_base : public Base, public part
{
public:
    explicit widget_base(QWidget* w)
        : w_(w)
    {
    }
    void set_enabled(bool e) override { w_->setEnabled(e); }
    void set_visible(bool v) override
    {
        // In a toolbar, its action shows and hides it.
        if (auto* action = qobject_cast<QAction*>(w_->property("rapport-toolbar-action").value<QObject*>()))
            return action->setVisible(v);
        // Not yet in its parent, showing it would open it as a window; it
        // shows with its parent anyway.
        if (v && !w_->parentWidget()) return;
        w_->setVisible(v);
        // A form field's row label goes with it.
        if (auto* label = qobject_cast<QWidget*>(w_->property("rapport-form-label").value<QObject*>()))
            label->setVisible(v);
    }
    void set_tooltip(const std::string& t) override { w_->setToolTip(qs(t)); }
    void focus() override { w_->setFocus(Qt::OtherFocusReason); }
    bool has_focus() const override
    {
        const QWidget* f = QApplication::focusWidget();
        return f && (f == w_ || w_->isAncestorOf(f));
    }
    QWidget* widget() const { return w_; }

protected:
    QWidget* w_;
};

class plain_impl : public widget_base<rap::widget>
{
public:
    using widget_base::widget_base;
};

// A label that can cut its text short with "..." (label::ellipsize):
// then it asks for no minimum width, and shows as much as fits.
class elided_label : public QLabel
{
public:
    explicit elided_label(bool elide)
        : elide_(elide)
    {
        setTextFormat(Qt::PlainText);
    }
    void set_full_text(const QString& t)
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

class label_impl : public widget_base<rap::label>
{
public:
    explicit label_impl(const spec::label& s)
        : widget_base(with_icon(new elided_label(s.ellipsize), s))
    {
        if (s.link)
        {
            // A rich-text link: the style's link colour, focus and Enter.
            link_ = true;
            label()->setTextFormat(Qt::RichText);
            label()->setTextInteractionFlags(Qt::LinksAccessibleByMouse |
                                             Qt::LinksAccessibleByKeyboard);
            QObject::connect(label(), &QLabel::linkActivated, label(),
                             [f = s.on_click](const QString&)
                             {
                                 if (f) f();
                             });
        }
        else if (s.selectable)
        {
            label()->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                             Qt::TextSelectableByKeyboard);
            label()->setCursor(Qt::IBeamCursor);
        }
        set_text(s.text);
    }
    void set_text(const std::string& t) override
    {
        if (link_)
            // Through the full text the label re-applies on resize.
            label()->set_full_text("<a href=\"rapport:\">" + qs(t).toHtmlEscaped() + "</a>");
        else
            label()->set_full_text(qs(t));
    }

private:
    // With an icon, the label sits after it in a row of its own; on a link
    // the icon takes the link's click and pointer.
    static QWidget* with_icon(elided_label* text, const spec::label& s)
    {
        if (!s.icon) return text;
        const QIcon icon = to_q_icon(s.icon, 16, nullptr);
        if (icon.isNull()) return text;
        auto* row = new QWidget;
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(4);
        auto* image = new QLabel;
        image->setPixmap(icon.pixmap(QSize(16, 16), qApp->devicePixelRatio()));
        layout->addWidget(image);
        layout->addWidget(text, 1);
        if (s.link && s.on_click)
        {
            image->setCursor(Qt::PointingHandCursor);
            image->installEventFilter(new click_filter(image, s.on_click));
        }
        return row;
    }
    // A press and release of the left button on the icon.
    class click_filter : public QObject
    {
    public:
        click_filter(QObject* parent, std::function<void()> f)
            : QObject(parent)
            , f_(std::move(f))
        {
        }
        bool eventFilter(QObject*, QEvent* e) override
        {
            if (e->type() != QEvent::MouseButtonRelease ||
                static_cast<QMouseEvent*>(e)->button() != Qt::LeftButton)
                return false;
            f_();
            return true;
        }

    private:
        std::function<void()> f_;
    };
    elided_label* label() const
    {
        if (auto* l = dynamic_cast<elided_label*>(w_)) return l;
        // No moc, so findChild can't name elided_label: its QLabels, cast.
        for (QLabel* l : w_->findChildren<QLabel*>())
            if (auto* e = dynamic_cast<elided_label*>(l)) return e;
        return nullptr;
    }
    bool link_ = false;
};

class button_impl : public widget_base<rap::button>
{
public:
    explicit button_impl(QPushButton* b)
        : widget_base(b)
    {
        QObject::connect(b, &QPushButton::clicked, b,
                         [this]
                         {
                             if (click_) click_();
                         });
    }
    void set_text(const std::string& t) override { button()->setText(mnemonic(t)); }
    void set_icon(const icon& icon) override { button()->setIcon(to_q_icon(icon, 16, w_)); }
    void on_click(std::function<void()> f) override { click_ = std::move(f); }

private:
    QPushButton* button() const { return static_cast<QPushButton*>(w_); }
    std::function<void()> click_;
};

// A click on the mixed state lands on checked (TortoiseGit's tri-state All
// Branches toggles between on and off); only the app sets it mixed.
class tri_check_box : public QCheckBox
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

class check_impl : public widget_base<rap::check>
{
public:
    explicit check_impl(const spec::check& s)
        : widget_base(new tri_check_box(mnemonic(s.text)))
    {
        set_state(s.state);
        // clicked: the user's changes only.
        QObject::connect(box(), &QCheckBox::clicked, box(),
                         [this]
                         {
                             if (toggled_) toggled_(box()->isChecked());
                         });
    }
    bool checked() const override { return state() == check_state::checked; }
    void set_checked(bool v) override { set_state(v ? check_state::checked : check_state::unchecked); }
    check_state state() const override
    {
        switch (box()->checkState())
        {
        case Qt::Checked: return check_state::checked;
        case Qt::PartiallyChecked: return check_state::indeterminate;
        case Qt::Unchecked: break;
        }
        return check_state::unchecked;
    }
    void set_state(check_state s) override
    {
        box()->setCheckState(s == check_state::indeterminate ? Qt::PartiallyChecked
                             : s == check_state::checked     ? Qt::Checked
                                                            : Qt::Unchecked);
    }
    void on_toggled(std::function<void(bool)> f) override { toggled_ = std::move(f); }

private:
    QCheckBox* box() const { return static_cast<QCheckBox*>(w_); }
    std::function<void(bool)> toggled_;
};

// A push button with a drop-down menu, rebuilt each time it opens. Pushed
// is drawn as checked.
class menu_button_impl : public widget_base<rap::menu_button>
{
public:
    explicit menu_button_impl(const spec::menu_button& s)
        : widget_base(new QPushButton(mnemonic(s.text)))
    {
        auto* b = button();
        menu_ = new QMenu(b);
        b->setMenu(menu_);
        b->setCheckable(true);
        b->setChecked(s.pushed);
        if (s.icon) set_icon(s.icon);
        QObject::connect(menu_, &QMenu::aboutToShow, menu_,
                         [this]
                         {
                             menu_->clear();
                             if (menu_fn_) fill_menu(menu_, menu_fn_());
                         });
    }
    void set_text(const std::string& t) override { button()->setText(mnemonic(t)); }
    void set_icon(const icon& icon) override { button()->setIcon(to_q_icon(icon, 16, w_)); }
    void set_pushed(bool v) override { button()->setChecked(v); }
    void on_menu(std::function<menu()> f) override { menu_fn_ = std::move(f); }

private:
    QPushButton* button() const { return static_cast<QPushButton*>(w_); }
    QMenu* menu_;
    std::function<menu()> menu_fn_;
};

class progress_bar_impl : public widget_base<rap::progress_bar>
{
public:
    progress_bar_impl()
        : widget_base(new QProgressBar)
    {
        bar()->setRange(0, 1000);
        bar()->setValue(0);
        bar()->setTextVisible(false);
    }
    void set_fraction(float f) override
    {
        if (bar()->maximum() == 0) bar()->setRange(0, 1000);
        bar()->setValue(int(std::clamp(f, 0.f, 1.f) * 1000));
    }
    // Qt animates a busy bar by itself.
    void pulse() override { bar()->setRange(0, 0); }

private:
    QProgressBar* bar() const { return static_cast<QProgressBar*>(w_); }
};

class date_picker_impl : public widget_base<rap::date_picker>
{
public:
    explicit date_picker_impl(const spec::date_picker& s)
        : widget_base(new QDateEdit)
    {
        edit()->setCalendarPopup(true);
        set_date(s.date);
        QObject::connect(edit(), &QDateEdit::dateChanged, edit(),
                         [this](QDate d)
                         {
                             if (!quiet_ && changed_) changed_({d.year(), d.month(), d.day()});
                         });
    }
    rap::date date() const override
    {
        const QDate d = edit()->date();
        return {d.year(), d.month(), d.day()};
    }
    void set_date(rap::date d) override
    {
        quiet_ = true;
        edit()->setDate(QDate(d.year, d.month, d.day));
        quiet_ = false;
    }
    void on_changed(std::function<void(rap::date)> f) override { changed_ = std::move(f); }

private:
    QDateEdit* edit() const { return static_cast<QDateEdit*>(w_); }
    std::function<void(rap::date)> changed_;
    bool quiet_ = false;
};

// Enter in a line edit presses the dialog's default button (OK), as on
// Windows, unless the app handles Enter itself (a find box): then the
// key stops here.
void handle_enter(QLineEdit* edit, const std::function<void()>* activate)
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

class entry_impl : public widget_base<rap::entry>
{
public:
    explicit entry_impl(const spec::entry& s)
        : widget_base(new QLineEdit(qs(s.text)))
    {
        if (s.password) edit()->setEchoMode(QLineEdit::Password);
        edit()->setPlaceholderText(qs(s.placeholder));
        QObject::connect(edit(), &QLineEdit::textChanged, edit(),
                         [this](const QString& t)
                         {
                             if (changed_) changed_(t.toStdString());
                         });
        handle_enter(edit(), &activate_);
    }
    std::string text() const override { return edit()->text().toStdString(); }
    void set_text(const std::string& t) override { edit()->setText(qs(t)); }
    void set_placeholder(const std::string& t) override { edit()->setPlaceholderText(qs(t)); }
    void set_read_only(bool v) override
    {
        edit()->setReadOnly(v);
        update_base();
    }
    // Frameless keeps the framed height (the frame's space, as GTK keeps
    // it), so giving the frame back doesn't move the layout.
    void set_frame(bool v) override
    {
        if (!v && edit()->hasFrame()) edit()->setMinimumHeight(edit()->sizeHint().height());
        edit()->setFrame(v);
        if (v) edit()->setMinimumHeight(0);
        update_base();
    }
    void on_changed(std::function<void(const std::string&)> f) override { changed_ = std::move(f); }
    void on_activate(std::function<void()> f) override { activate_ = std::move(f); }

private:
    QLineEdit* edit() const { return static_cast<QLineEdit*>(w_); }
    // Read-only and frameless, it reads as text on the window: its Base is
    // the palette's Window; otherwise the palette it inherits.
    void update_base()
    {
        if (edit()->isReadOnly() && !edit()->hasFrame())
        {
            QPalette p = edit()->palette();
            p.setBrush(QPalette::Base, p.brush(QPalette::Window));
            edit()->setPalette(p);
        }
        else
            edit()->setPalette(QPalette());
    }
    std::function<void(const std::string&)> changed_;
    std::function<void()> activate_;
};

class drop_down_impl : public widget_base<rap::drop_down>
{
public:
    explicit drop_down_impl(const spec::drop_down& s)
        : widget_base(new QComboBox)
    {
        new readable_combo(combo());
        set_items(s.items);
        set_current(s.current);
        QObject::connect(combo(), &QComboBox::currentIndexChanged, combo(),
                         [this](int i)
                         {
                             if (!quiet_ && changed_) changed_(i);
                         });
    }
    void set_items(std::vector<std::string> items) override
    {
        quiet_ = true;
        combo()->clear();
        for (auto& i : items) combo()->addItem(qs(i));
        quiet_ = false;
    }
    int current() const override { return combo()->currentIndex(); }
    void set_current(int i) override { combo()->setCurrentIndex(i); }
    void on_changed(std::function<void(int)> f) override { changed_ = std::move(f); }

private:
    QComboBox* combo() const { return static_cast<QComboBox*>(w_); }
    std::function<void(int)> changed_;
    bool quiet_ = false;
};

class combo_impl : public widget_base<rap::combo>
{
public:
    explicit combo_impl(const spec::combo& s)
        : widget_base(new QComboBox)
    {
        new readable_combo(combo());
        combo()->setEditable(true);
        combo()->setInsertPolicy(QComboBox::NoInsert);
        set_items(s.items);
        set_text(s.text);
        QObject::connect(combo()->lineEdit(), &QLineEdit::textChanged, combo(),
                         [this](const QString& t)
                         {
                             if (changed_) changed_(t.toStdString());
                         });
        handle_enter(combo()->lineEdit(), &activate_);
    }
    void set_items(std::vector<std::string> items) override
    {
        const QString text = combo()->currentText();
        combo()->clear();
        for (auto& i : items) combo()->addItem(qs(i));
        combo()->setEditText(text);
    }
    std::string text() const override { return combo()->currentText().toStdString(); }
    void set_text(const std::string& t) override { combo()->setEditText(qs(t)); }
    void on_changed(std::function<void(const std::string&)> f) override { changed_ = std::move(f); }
    void on_activate(std::function<void()> f) override { activate_ = std::move(f); }

private:
    QComboBox* combo() const { return static_cast<QComboBox*>(w_); }
    std::function<void(const std::string&)> changed_;
    std::function<void()> activate_;
};

// QPlainTextEdit: lays out lazily, so long diffs open at once. Styled spans
// go in as one pass of inserts, each run with its format.
class text_view_impl : public widget_base<rap::text_view>
{
public:
    explicit text_view_impl(const spec::text_view& s)
        : widget_base(new QPlainTextEdit)
    {
        auto* e = edit();
        QFont f = s.compact ? compact_font(e->font()) : e->font();
        if (s.monospace) f = styled_font(f, text_style{.monospace = true});
        e->setFont(f);
        e->setLineWrapMode(s.wrap ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
        e->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        set_read_only(s.read_only);
        set_text(s.text);
        QObject::connect(e, &QPlainTextEdit::textChanged, e,
                         [this]
                         {
                             if (changed_) changed_();
                         });
    }

    std::string text() const override { return edit()->toPlainText().toStdString(); }

    void set_text(const std::string& t, std::vector<span> spans = {}) override
    {
        auto* doc = edit()->document();
        const bool undo = doc->isUndoRedoEnabled();
        doc->setUndoRedoEnabled(false);
        if (spans.empty())
            edit()->setPlainText(qs(t));
        else
        {
            edit()->clear();
            std::ranges::stable_sort(spans, {}, &span::begin);
            QTextCursor c(doc);
            c.beginEditBlock();
            size_t at = 0;
            for (const span& sp : spans)
            {
                const size_t b = std::clamp(sp.begin, at, t.size()),
                             e = std::clamp(sp.end, b, t.size());
                if (b > at) c.insertText(qs(t.substr(at, b - at)), QTextCharFormat());
                if (e > b) c.insertText(qs(t.substr(b, e - b)), char_format(w_, sp.style));
                at = std::max(at, e);
            }
            if (at < t.size()) c.insertText(qs(t.substr(at)), QTextCharFormat());
            c.endEditBlock();
            edit()->moveCursor(QTextCursor::Start);
        }
        doc->setUndoRedoEnabled(undo);
    }

    void append(const std::string& t, const text_style& style) override
    {
        QTextCursor c(edit()->document());
        c.movePosition(QTextCursor::End);
        c.insertText(qs(t), char_format(w_, style));
    }

    void set_read_only(bool ro) override
    {
        edit()->setReadOnly(ro);
        // Read-only text is still selectable from the keyboard (Ctrl+A,
        // Shift+arrows), without showing a caret.
        if (ro)
            edit()->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                            Qt::TextSelectableByKeyboard);
        edit()->setCursorWidth(ro ? 0 : 1);
    }

    void on_changed(std::function<void()> f) override { changed_ = std::move(f); }

    // Document positions count UTF-16 units (and one per line break), as
    // toPlainText() does; the API counts UTF-8 bytes.
    std::pair<size_t, size_t> selection() const override
    {
        const QTextCursor c = edit()->textCursor();
        const QString t = edit()->toPlainText();
        return {utf8_at(t, c.selectionStart()), utf8_at(t, c.selectionEnd())};
    }

    void select(size_t begin, size_t end) override
    {
        const std::string t = text();
        QTextCursor c(edit()->document());
        c.setPosition(int(utf16_at(t, begin)));
        c.setPosition(int(utf16_at(t, end)), QTextCursor::KeepAnchor);
        edit()->setTextCursor(c);
        edit()->ensureCursorVisible();
    }

private:
    QPlainTextEdit* edit() const { return static_cast<QPlainTextEdit*>(w_); }
    std::function<void()> changed_;
};

// -- containers ---------------------------------------------------------------

// Splitter panes: the first layout follows stretch factors (panes without
// stretch at their natural size), or the sizes given before it; after
// that, spare space goes to the stretching panes only.
class splitter_impl : public widget_base<rap::splitter>
{
public:
    splitter_impl(QSplitter* s, std::vector<int> stretch)
        : widget_base(s)
        , stretch_(std::move(stretch))
    {
        // At Show the splitter has had its first size (a top level's layout
        // runs before it shows); a later first size (a page shown later)
        // gets its layout once the splitter has dealt with the resize.
        hook(s,
             [this](QEvent* e)
             {
                 // Sizes set for a window about to grow (a pane shown and the
                 // window widened in the same step) take their place once it
                 // has; QSplitter would give the growth to the stretching pane.
                 // The new size is in place before the event, so this frame
                 // is laid out with them.
                 if (laid_out_ && wanted_ && e->type() == QEvent::Resize) apply(*wanted_);
                 if (laid_out_ || length() <= 1) return false;
                 if (e->type() == QEvent::Show)
                 {
                     laid_out_ = true;
                     first_layout();
                 }
                 else if (e->type() == QEvent::Resize && splitter()->isVisible())
                 {
                     laid_out_ = true;
                     QTimer::singleShot(0, splitter(), [this] { first_layout(); });
                 }
                 return false;
             });
        // Only a drag moves a handle (set_sizes doesn't): it's reported when
        // the button comes up.
        QObject::connect(s, &QSplitter::splitterMoved, s,
                         [this]
                         {
                             wanted_.reset();
                             dragged_ = true;
                         });
    }

    void on_moved(std::function<void()> f) override
    {
        const bool first = !moved_;
        moved_ = std::move(f);
        if (!first) return;
        for (int i = 1; i < splitter()->count(); ++i)  // handle(0) is never shown
            hook(splitter()->handle(i),
                 [this](QEvent* e)
                 {
                     if (e->type() == QEvent::MouseButtonRelease && dragged_)
                     {
                         dragged_ = false;
                         if (moved_) moved_();
                     }
                     return false;
                 });
    }

    float divider() const override { return float(splitter()->handleWidth()); }
    std::vector<float> sizes() const override
    {
        std::vector<float> out;
        for (int s : splitter()->sizes()) out.push_back(float(s));
        return out;
    }

    // Each divider goes at sizes[i] (0: left as it is); the last pane gets
    // the rest.
    void set_sizes(const std::vector<float>& s) override
    {
        if (laid_out_)
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
    int pane_length() const
    {
        return length() - splitter()->handleWidth() * std::max(0, splitter()->count() - 1);
    }

    void first_layout()
    {
        if (pending_)
            apply(*pending_);
        else
            initial_split();
    }

    void initial_split()
    {
        const int n = splitter()->count();
        int fixed = 0, stretch_sum = 0;
        QList<int> sizes(n, 0);
        for (int i = 0; i < n && size_t(i) < stretch_.size(); ++i)
        {
            if (stretch_[size_t(i)] > 0)
            {
                stretch_sum += stretch_[size_t(i)];
                continue;
            }
            const QSize hint = splitter()->widget(i)->sizeHint();
            sizes[i] = splitter()->orientation() == Qt::Vertical ? hint.height() : hint.width();
            fixed += sizes[i];
        }
        const int avail = pane_length() - fixed;
        if (avail <= 0 || stretch_sum == 0) return;
        for (int i = 0; i < n && size_t(i) < stretch_.size(); ++i)
            if (stretch_[size_t(i)] > 0) sizes[i] = avail * stretch_[size_t(i)] / stretch_sum;
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
        const int rest = pane_length() - used;
        if (n) sizes[n - 1] = std::max(0, rest);
        // Kept until the last pane has room for at least its minimum.
        const QWidget* last = n ? splitter()->widget(n - 1) : nullptr;
        const QSize min = last ? last->minimumSizeHint() : QSize();
        if (last && last->isVisibleTo(splitter()) &&
            rest < std::max(1, splitter()->orientation() == Qt::Vertical ? min.height() : min.width()))
            wanted_ = s;
        else
            wanted_.reset();
        splitter()->setSizes(sizes);
    }

    std::vector<int> stretch_;
    std::optional<std::vector<float>> wanted_;
    std::function<void()> moved_;
    bool dragged_ = false;
    std::optional<std::vector<float>> pending_;
    bool laid_out_ = false;
};

// A page's content at its top, as GTK packs it, unless something in it
// grows to fill the height (a list, a text view): a box of rows would
// otherwise share the page's spare height out between them.
void add_page_content(QWidget* page, QWidget* content)
{
    auto* l = static_cast<QBoxLayout*>(page->layout());
    l->addWidget(content);
    const bool grows =
        (content->sizePolicy().verticalPolicy() & QSizePolicy::ExpandFlag) ||
        (content->layout() && (content->layout()->expandingDirections() & Qt::Vertical));
    if (!grows) l->addStretch(1);
}

// Tab pages built the first time they're shown (layout::lazy): each starts
// as an empty page with a layout, filled then.
struct lazy_pages
{
    std::vector<std::function<node()>> lazy;
    std::vector<QWidget*> pages;
    std::function<QWidget*(const node&)> build;

    void ensure_built(int i)
    {
        if (i < 0 || size_t(i) >= lazy.size() || !lazy[size_t(i)]) return;
        const auto make = std::move(lazy[size_t(i)]);
        lazy[size_t(i)] = nullptr;
        add_page_content(pages[size_t(i)], build(make()));
    }
};

class tabs_impl : public widget_base<rap::tabs>
{
public:
    explicit tabs_impl(QTabWidget* t)
        : widget_base(t)
    {
        QObject::connect(t, &QTabWidget::currentChanged, t,
                         [this](int i)
                         {
                             lazy_.ensure_built(i);
                             if (changed_) changed_(i);
                         });
    }
    int current() const override { return tabs()->currentIndex(); }
    void set_current(int i) override
    {
        tabs()->setCurrentIndex(i);
        lazy_.ensure_built(i);
    }
    void on_changed(std::function<void(int)> f) override { changed_ = std::move(f); }
    void set_lazy(lazy_pages pages)
    {
        lazy_ = std::move(pages);
        lazy_.ensure_built(current());
    }

private:
    QTabWidget* tabs() const { return static_cast<QTabWidget*>(w_); }
    std::function<void(int)> changed_;
    lazy_pages lazy_;
};

// Tabs as a list of page names down the left, the page beside it.
class side_tabs_impl : public widget_base<rap::tabs>
{
public:
    side_tabs_impl(QWidget* box, QListWidget* list, QStackedWidget* stack)
        : widget_base(box)
        , list_(list)
        , stack_(stack)
    {
        QObject::connect(list_, &QListWidget::currentRowChanged, list_,
                         [this](int i)
                         {
                             if (i < 0) return;
                             lazy_.ensure_built(i);
                             stack_->setCurrentIndex(i);
                             if (changed_) changed_(i);
                         });
    }
    int current() const override { return std::max(0, list_->currentRow()); }
    void set_current(int i) override
    {
        list_->setCurrentRow(i);
        lazy_.ensure_built(i);
    }
    void on_changed(std::function<void(int)> f) override { changed_ = std::move(f); }
    void set_lazy(lazy_pages pages)
    {
        lazy_ = std::move(pages);
        lazy_.ensure_built(current());
    }

private:
    QListWidget* list_;
    QStackedWidget* stack_;
    std::function<void(int)> changed_;
    lazy_pages lazy_;
};

// -- toolbar, values, canvas --------------------------------------------------

class tool_button_impl : public widget_base<rap::tool_button>
{
public:
    explicit tool_button_impl(const spec::tool_button& s)
        : widget_base(new QToolButton)
    {
        auto* b = button();
        b->setAutoRaise(true);
        b->setIconSize(QSize(16, 16));
        b->setAccessibleName(qs(s.label));
        if (!s.label.empty()) b->setToolTip(qs(s.label));
        b->setCheckable(s.toggle);
        b->setChecked(s.checked);
        if (s.icon) set_icon(s.icon);
        QObject::connect(b, &QToolButton::clicked, b, [this] { if (click_) click_(); });
        QObject::connect(b, &QToolButton::toggled, b,
                         [this](bool on)
                         {
                             if (!quiet_ && toggled_) toggled_(on);
                         });
    }
    void set_icon(const icon& icon) override { button()->setIcon(to_q_icon(icon, 16, w_)); }
    bool checked() const override { return button()->isChecked(); }
    void set_checked(bool c) override
    {
        quiet_ = true;  // set from code: no on_toggled
        button()->setChecked(c);
        quiet_ = false;
    }
    void on_click(std::function<void()> f) override { click_ = std::move(f); }
    void on_toggled(std::function<void(bool)> f) override { toggled_ = std::move(f); }

private:
    QToolButton* button() const { return static_cast<QToolButton*>(w_); }
    std::function<void()> click_;
    std::function<void(bool)> toggled_;
    bool quiet_ = false;
};

// A slider over doubles: QSlider counts whole steps from `min`, and an
// optional label shows the value.
class slider_impl : public widget_base<rap::slider>
{
public:
    explicit slider_impl(const spec::slider& s)
        : widget_base(new QWidget)
        , slider_(new QSlider(Qt::Horizontal))
        , min_(s.min)
        , max_(s.max)
        , step_(s.step > 0 ? s.step : (s.max - s.min) / 1000)
        , digits_(s.digits)
    {
        auto* row = new QHBoxLayout(w_);
        row->setContentsMargins(0, 0, 0, 0);
        row->addWidget(slider_, 1);
        if (s.show_value)
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
        apply_range();
        set_value(s.value);
        QObject::connect(slider_, &QSlider::valueChanged, slider_,
                         [this](int)
                         {
                             show_value();
                             if (!quiet_ && changed_) changed_(value());
                         });
    }
    double value() const override { return min_ + slider_->value() * step_; }
    void set_value(double v) override
    {
        quiet_ = true;
        slider_->setValue(int(std::lround((std::clamp(v, min_, max_) - min_) / step_)));
        quiet_ = false;
        show_value();
    }
    void set_range(double mn, double mx) override
    {
        const double v = value();
        min_ = mn;
        max_ = mx;
        apply_range();
        set_value(v);
    }
    void on_changed(std::function<void(double)> f) override { changed_ = std::move(f); }

private:
    void apply_range()
    {
        quiet_ = true;
        slider_->setRange(0, std::max(0, int(std::lround((max_ - min_) / step_))));
        quiet_ = false;
    }
    void show_value()
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

class spin_impl : public widget_base<rap::spin>
{
public:
    explicit spin_impl(const spec::spin& s)
        : widget_base(new QDoubleSpinBox)
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
    void set_value(double v) override
    {
        quiet_ = true;
        box()->setValue(v);
        quiet_ = false;
    }
    void set_range(double mn, double mx) override
    {
        quiet_ = true;
        box()->setRange(mn, mx);
        quiet_ = false;
    }
    void on_changed(std::function<void(double)> f) override { changed_ = std::move(f); }

private:
    QDoubleSpinBox* box() const { return static_cast<QDoubleSpinBox*>(w_); }
    bool quiet_ = false;
    std::function<void(double)> changed_;
};

// A button showing the colour; clicking opens Qt's colour chooser.
class color_button_impl : public widget_base<rap::color_button>
{
public:
    explicit color_button_impl(const spec::color_button& s)
        : widget_base(new QPushButton)
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
                             color_ = from_q_color(c);
                             swatch();
                             if (changed_) changed_(color_);
                         });
    }
    rap::color color() const override { return color_; }
    void set_color(rap::color c) override
    {
        color_ = c;
        swatch();
    }
    void on_changed(std::function<void(rap::color)> f) override { changed_ = std::move(f); }

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

    rap::color color_;
    bool alpha_;
    QString title_;
    std::function<void(rap::color)> changed_;
};

class canvas_impl;

// The Qt side of a canvas: painting and input handed to its WCanvas while
// it's there (the window deletes Qt widgets after the parts).
class canvas_widget : public QWidget
{
public:
    canvas_impl* owner = nullptr;

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void showEvent(QShowEvent*) override;
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

int button_number(Qt::MouseButton b)
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

class canvas_impl : public widget_base<rap::canvas>
{
public:
    explicit canvas_impl(const spec::canvas& s)
        : widget_base(new canvas_widget)
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
    ~canvas_impl() override
    {
        *alive_ = false;
        canvas()->owner = nullptr;
        frames_->stop();
    }

    void invalidate() override { w_->update(); }
    // Nothing until it has been laid out and shown, as on GTK: before that a
    // Qt widget has a placeholder size (640x480), which an app sizing its
    // window round the canvas would take for real.
    size_f size() const override
    {
        return laid_out_ ? size_f{float(w_->width()), float(w_->height())} : size_f{};
    }
    float device_scale() const override { return float(w_->devicePixelRatioF()); }
    void set_cursor(cursor c) override
    {
        switch (c)
        {
        case cursor::default_: w_->unsetCursor(); break;
        case cursor::pointer: w_->setCursor(Qt::PointingHandCursor); break;
        case cursor::text: w_->setCursor(Qt::IBeamCursor); break;
        case cursor::crosshair: w_->setCursor(Qt::CrossCursor); break;
        case cursor::grab: w_->setCursor(Qt::OpenHandCursor); break;
        case cursor::grabbing: w_->setCursor(Qt::ClosedHandCursor); break;
        case cursor::zoom_in:
        case cursor::zoom_out: w_->setCursor(Qt::CrossCursor); break;  // Qt has no zoom cursors
        case cursor::move: w_->setCursor(Qt::SizeAllCursor); break;
        case cursor::wait: w_->setCursor(Qt::WaitCursor); break;
        case cursor::hidden: w_->setCursor(Qt::BlankCursor); break;
        case cursor::resize_ew: w_->setCursor(Qt::SizeHorCursor); break;
        case cursor::resize_ns: w_->setCursor(Qt::SizeVerCursor); break;
        case cursor::resize_nwse: w_->setCursor(Qt::SizeFDiagCursor); break;
        case cursor::resize_nesw: w_->setCursor(Qt::SizeBDiagCursor); break;
        }
    }
    void on_paint(std::function<void(rap::painter&, rap::size_f)> f) override { paint_ = std::move(f); }
    void on_resize(std::function<void(size_f)> f) override { resize_ = std::move(f); }
    void on_pointer(std::function<bool(const pointer_event&)> f) override { pointer_ = std::move(f); }
    void on_scroll(std::function<bool(const scroll_event&)> f) override { scroll_ = std::move(f); }
    void on_key(std::function<bool(const key_event&)> f) override { key_ = std::move(f); }

    // Frames at the screen's refresh rate while anyone's subscribed.
    subscription on_frame(std::function<void(double)> f) override
    {
        const int id = ++next_frame_;
        frame_fns_.push_back({id, std::move(f)});
        if (!frames_->isActive())
        {
            const QScreen* screen = w_->screen();
            const double hz = screen && screen->refreshRate() > 1 ? screen->refreshRate() : 60;
            frames_->start(std::max(1, int(1000 / hz)));
        }
        return subscription(
            [this, alive = alive_, id]
            {
                if (!*alive) return;
                std::erase_if(frame_fns_, [id](auto& e) { return e.first == id; });
                if (frame_fns_.empty()) frames_->stop();
            });
    }

    // From canvas_widget.
    void paint(QPainter& qp)
    {
        // A new device scale is a resize in device pixels (canvas::on_resize),
        // told before the frame that uses it. Qt sends child widgets nothing
        // when it changes: on Wayland the compositor's fractional scale
        // arrives after the window shows, replacing the whole number Qt
        // starts with, and it changes on a move to another monitor.
        const float scale = float(w_->devicePixelRatioF());
        if (laid_out_ && scale != scale_)
        {
            scale_ = scale;
            resized();
        }
        if (!paint_) return;
        qt_painter p(qp, w_);
        paint_(p, size());
    }
    void resized()
    {
        if (laid_out_ && resize_) resize_(size());
    }
    // Shown, so laid out: its size is real from now on.
    void shown()
    {
        if (laid_out_) return;
        laid_out_ = true;
        scale_ = float(w_->devicePixelRatioF());
        resized();
    }
    bool pointer(const pointer_event& e) { return pointer_ && pointer_(e); }
    bool scroll(const scroll_event& e) { return scroll_ && scroll_(e); }
    bool key(const key_event& e) { return key_ && key_(e); }

    // Presses counted as gtk3 does: each press arrives once, with how many
    // came in quick succession at much the same place.
    int clicks(int button, QPointF pos, quint64 time)
    {
        const bool again = button == last_button_ && time - last_time_ <= quint64(QApplication::doubleClickInterval()) &&
                           std::abs(pos.x() - last_pos_.x()) <= QApplication::startDragDistance() &&
                           std::abs(pos.y() - last_pos_.y()) <= QApplication::startDragDistance();
        clicks_ = again ? clicks_ + 1 : 1;
        last_button_ = button;
        last_time_ = time;
        last_pos_ = pos;
        return clicks_;
    }

private:
    bool laid_out_ = false;
    float scale_ = 0;  // the device scale the app last heard of
    canvas_widget* canvas() const { return static_cast<canvas_widget*>(w_); }
    void tick()
    {
        const double t = double(QDateTime::currentMSecsSinceEpoch()) / 1000.0;
        auto fns = frame_fns_;  // callbacks may unsubscribe
        for (auto& [id, fn] : fns)
            if (std::any_of(frame_fns_.begin(), frame_fns_.end(), [id = id](auto& e) { return e.first == id; })) fn(t);
    }

    std::function<void(rap::painter&, rap::size_f)> paint_;
    std::function<void(size_f)> resize_;
    std::function<bool(const pointer_event&)> pointer_;
    std::function<bool(const scroll_event&)> scroll_;
    std::function<bool(const key_event&)> key_;
    QTimer* frames_;
    std::vector<std::pair<int, std::function<void(double)>>> frame_fns_;
    int next_frame_ = 0;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    int clicks_ = 0, last_button_ = 0;
    quint64 last_time_ = 0;
    QPointF last_pos_;
};

void canvas_widget::paintEvent(QPaintEvent*)
{
    if (!owner) return;
    QPainter p(this);
    owner->paint(p);
}

void canvas_widget::resizeEvent(QResizeEvent*)
{
    if (owner) owner->resized();
}

void canvas_widget::showEvent(QShowEvent*)
{
    if (owner) owner->shown();
}

void canvas_widget::press(QMouseEvent* e)
{
    if (!owner) return;
    if (focusPolicy() != Qt::NoFocus && !hasFocus()) setFocus(Qt::MouseFocusReason);
    pointer_event ev{pointer_event_type::press, {float(e->position().x()), float(e->position().y())}, button_number(e->button()), 1,
                    modifiers_of(e->modifiers())};
    ev.clicks = owner->clicks(ev.button, e->position(), e->timestamp());
    if (owner->pointer(ev)) e->accept();
}

void canvas_widget::mouseReleaseEvent(QMouseEvent* e)
{
    if (!owner) return;
    owner->pointer({pointer_event_type::release, {float(e->position().x()), float(e->position().y())}, button_number(e->button()), 1,
                    modifiers_of(e->modifiers())});
}

void canvas_widget::mouseMoveEvent(QMouseEvent* e)
{
    if (!owner) return;
    owner->pointer({pointer_event_type::move, {float(e->position().x()), float(e->position().y())}, 0, 1, modifiers_of(e->modifiers())});
}

void canvas_widget::enterEvent(QEnterEvent* e)
{
    if (!owner) return;
    owner->pointer({pointer_event_type::enter, {float(e->position().x()), float(e->position().y())}, 0, 1,
                    modifiers_of(QApplication::keyboardModifiers())});
}

void canvas_widget::leaveEvent(QEvent*)
{
    if (!owner) return;
    const QPointF at = mapFromGlobal(QCursor::pos());
    owner->pointer({pointer_event_type::leave, {float(at.x()), float(at.y())}, 0, 1, modifiers_of(QApplication::keyboardModifiers())});
}

// Wheels report eighths of a degree, 120 to a notch, positive away from the
// user (up, and left for a horizontal wheel); touchpads also report pixels.
void canvas_widget::wheelEvent(QWheelEvent* e)
{
    if (!owner) return;
    scroll_event ev{{float(e->position().x()), float(e->position().y())}, 0, 0, false, modifiers_of(e->modifiers())};
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

void canvas_widget::key(QKeyEvent* e, bool press)
{
    if (!owner) return;
    key_event ev;
    ev.press = press;
    ev.key = key_for(e->key());
    if (ev.key == key::character)
    {
        const QString t = e->text().toLower();
        ev.codepoint = !t.isEmpty() && !t[0].isNull() && t[0].isPrint() ? char32_t(t.toUcs4().value(0))
                                                                         : char32_t(QChar(e->key()).toLower().unicode());
    }
    ev.modifiers = modifiers_of(e->modifiers());
    ev.repeat = e->isAutoRepeat();
    if (owner->key(ev))
        e->accept();
    else
        e->ignore();
}

// -- list ---------------------------------------------------------------------

// The rapport model as Qt sees it: a row count kept in step with the
// notifications, and text for type-ahead and accessibility. The view paints
// rows itself, so nothing else is asked of it.
class rows_model : public QAbstractTableModel
{
public:
    rows_model(std::vector<column> cols, QObject* parent)
        : QAbstractTableModel(parent)
        , cols_(std::move(cols))
    {
    }

    list_model* model = nullptr;
    int count = 0;

    int rowCount(const QModelIndex& p = {}) const override { return p.isValid() ? 0 : count; }
    int columnCount(const QModelIndex& p = {}) const override
    {
        return p.isValid() ? 0 : int(cols_.size());
    }
    QVariant data(const QModelIndex& i, int role) const override
    {
        if (role != Qt::DisplayRole || !model || size_t(i.row()) >= model->row_count()) return {};
        return qs(model->text(size_t(i.row()), size_t(i.column())));
    }
    QVariant headerData(int section, Qt::Orientation o, int role) const override
    {
        if (o != Qt::Horizontal || section < 0 || size_t(section) >= cols_.size()) return {};
        if (role == Qt::DisplayRole) return qs(cols_[size_t(section)].title);
        if (role == Qt::TextAlignmentRole)
            return QVariant::fromValue(qt_align(cols_[size_t(section)].align) | Qt::AlignVCenter);
        return {};
    }
    Qt::ItemFlags flags(const QModelIndex&) const override
    {
        return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemNeverHasChildren;
    }

    // The rapport model has already changed; the count catches up here.
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
    std::vector<column> cols_;
};

// Rows are one height (QTreeView asks only the first) and painted by
// list_view::drawRow.
class row_delegate : public QStyledItemDelegate
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

class list_impl;

// The QTreeView, handing its painting and input to its WList.
class list_view : public QTreeView
{
public:
    list_impl* owner = nullptr;

    void base_mouse_press(QMouseEvent* e) { QTreeView::mousePressEvent(e); }
    void base_mouse_move(QMouseEvent* e) { QTreeView::mouseMoveEvent(e); }
    void base_mouse_release(QMouseEvent* e) { QTreeView::mouseReleaseEvent(e); }
    void base_mouse_double_click(QMouseEvent* e) { QTreeView::mouseDoubleClickEvent(e); }
    void base_key_press(QKeyEvent* e) { QTreeView::keyPressEvent(e); }

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

class list_impl : public widget_base<rap::list>, public list_observer
{
public:
    explicit list_impl(const spec::list& s)
        : widget_base(new list_view)
        , cols_(s.columns)
        , compact_(s.compact)
        , auto_scroll_(s.auto_scroll)
        , checks_(s.checkboxes)
    {
        view_ = static_cast<list_view*>(w_);
        view_->owner = this;
        rows_ = new rows_model(s.columns, view_);
        delegate_ = new row_delegate(view_);
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
        view_->setSelectionMode(s.selection == selection_mode::none ? QAbstractItemView::NoSelection
                                : s.selection == selection_mode::single
                                    ? QAbstractItemView::SingleSelection
                                    : QAbstractItemView::ExtendedSelection);
        // Pixel scrolling: the thumb and the rows follow the pointer exactly.
        view_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        view_->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
        view_->viewport()->setMouseTracking(true);
        if (compact_) view_->setFont(compact_font(view_->font()));
        if (s.soft_selection) soft_ = soft_colors_for(view_);
        if (auto_scroll_) lists().insert(this);
        first_text_column_ =
            int(std::ranges::find_if(cols_, [](const column& c) { return !c.painted; }) -
                cols_.begin());
        if (size_t(first_text_column_) >= cols_.size()) first_text_column_ = 0;

        QHeaderView* h = view_->header();
        h->setStretchLastSection(false);
        h->setSectionsMovable(true);  // the user's column order: column_order()
        h->setMinimumSectionSize(8);
        h->setSectionResizeMode(QHeaderView::Interactive);
        h->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        const bool sortable = std::ranges::any_of(cols_, &column::sortable);
        h->setSectionsClickable(sortable);
        for (size_t i = 0; i < cols_.size(); ++i)
        {
            const column& c = cols_[i];
            int width = px(c.width);
            if (width < 0)
                width = int((c.painted ? 8 : std::max<size_t>(c.title.size() + 4, 10)) *
                            g_metrics.char_width);
            h->resizeSection(int(i), width);
            view_->setColumnHidden(int(i), !c.visible);
        }
        stretch_last();
        // A column dragged to the end (or away from it) changes which fills.
        QObject::connect(h, &QHeaderView::sectionMoved, h, [this] { stretch_last(); });
        h->setContextMenuPolicy(Qt::CustomContextMenu);
        QObject::connect(h, &QHeaderView::customContextMenuRequested, h,
                         [this](QPoint pos)
                         {
                             const int col = view_->header()->logicalIndexAt(pos);
                             if (col >= 0 && header_menu_)
                                 show_popup_menu(header_menu_(size_t(col)), view_->header(),
                                           view_->header()->mapToGlobal(pos));
                         });
        QObject::connect(h, &QHeaderView::sectionClicked, h,
                         [this](int col) { header_clicked(col); });
        QObject::connect(view_->selectionModel(), &QItemSelectionModel::selectionChanged, view_,
                         [this]
                         {
                             if (sel_changed_) sel_changed_();
                         });
        fonts_changed();
    }

    ~list_impl() override
    {
        lists().erase(this);
        if (active == this) active = nullptr;
        if (model_) model_->remove_observer(this);
        rows_->model = nullptr;
        view_->owner = nullptr;
    }

    void set_model(list_model* m) override
    {
        if (model_) model_->remove_observer(this);
        model_ = m;
        rows_->model = m;
        if (model_) model_->add_observer(this);
        model_reset();
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

    void select(const std::vector<size_t>& rows_in) override
    {
        std::vector<size_t> rows = rows_in;
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
    void set_current(size_t row) override
    {
        if (row >= size_t(rows_->count)) return;
        view_->selectionModel()->setCurrentIndex(rows_->index(int(row), first_text_column_),
                                                 QItemSelectionModel::ClearAndSelect |
                                                     QItemSelectionModel::Rows);
    }

    void scroll_to(size_t row) override
    {
        if (row < size_t(rows_->count))
            view_->scrollTo(rows_->index(int(row), 0), QAbstractItemView::EnsureVisible);
    }

    void set_sort_indicator(std::optional<size_t> column, sort_order order) override
    {
        sort_column_ = column;
        sort_order_ = order;
        show_sort_indicator();
    }

    std::vector<float> column_widths() const override
    {
        std::vector<float> out;
        for (size_t i = 0; i < cols_.size(); ++i)
            out.push_back(float(view_->header()->sectionSize(int(i))));
        return out;
    }

    std::vector<size_t> column_order() const override
    {
        std::vector<size_t> out;
        for (int v = 0; v < int(cols_.size()); ++v)
            out.push_back(size_t(view_->header()->logicalIndex(v)));
        return out;
    }
    void set_column_order(const std::vector<size_t>& order) override
    {
        if (!is_column_order(order, cols_.size())) return;
        QHeaderView* h = view_->header();
        for (size_t v = 0; v < order.size(); ++v)
            h->moveSection(h->visualIndex(int(order[v])), int(v));
    }

    void set_column_widths(const std::vector<float>& widths) override
    {
        for (size_t i = 0; i < widths.size() && i < cols_.size(); ++i)
            if (widths[i] > 0) view_->header()->resizeSection(int(i), int(widths[i]));
        fitted_ = true;  // restored widths win over stretching
    }

    bool column_visible(size_t column) const override
    {
        return column < cols_.size() && !view_->isColumnHidden(int(column));
    }
    void set_column_visible(size_t column, bool v) override
    {
        if (column < cols_.size()) view_->setColumnHidden(int(column), !v);
        stretch_last();
    }

    void on_selection_changed(std::function<void()> f) override { sel_changed_ = std::move(f); }
    void on_activate(std::function<void(size_t)> f) override { activate_ = std::move(f); }
    void on_sort_requested(std::function<void(size_t, sort_order)> f) override
    {
        sort_ = std::move(f);
    }
    void on_context_menu(std::function<menu(std::optional<size_t>)> f) override
    {
        context_menu_ = std::move(f);
    }
    void on_header_menu(std::function<menu(size_t)> f) override { header_menu_ = std::move(f); }
    void on_cell_context_menu(std::function<menu(size_t, size_t, point_f)> f) override
    {
        cell_menu_ = std::move(f);
    }
    void on_reorder(reorder_fn f) override { reorder_ = std::move(f); }
    void set_reorderable(bool v) override { reorderable_ = v; }
    void on_cell_drag(cell_drag d) override { cell_drag_ = std::move(d); }

    // list_observer. Big changes reset: Qt's per-row bookkeeping is O(rows).
    void model_reset() override { rows_->reset(model_ ? int(model_->row_count()) : 0); }
    void rows_changed(size_t first, size_t n) override
    {
        if (n > bulk || first + n > size_t(rows_->count))
            view_->viewport()->update();
        else if (n)
            rows_->changed(int(first), int(n));
    }
    void rows_inserted(size_t first, size_t n) override
    {
        if (n > bulk) return model_reset();
        if (n) rows_->inserted(int(first), int(n));
    }
    void rows_removed(size_t first, size_t n) override
    {
        if (n > bulk) return model_reset();
        if (n) rows_->removed(int(first), int(n));
    }

    // Esc during a drag or an autoscroll cancels it, ahead of any window
    // key handling (which might close the dialog).
    static bool cancel_active_drag()
    {
        for (list_impl* l : lists())
            if (l->auto_.on)
            {
                l->stop_auto_scroll();
                return true;
            }
        list_impl* list = active;
        if (!list) return false;
        list->drag_ = {};
        list->cell_ = {};
        list->drop_y_.reset();
        active = nullptr;
        list->view_->viewport()->unsetCursor();
        list->view_->viewport()->update();
        return true;
    }

    // -- called by list_view --------------------------------------------------

    void paint_row(QPainter* p, const QRect& r, int row) const
    {
        if (!model_ || size_t(row) >= model_->row_count()) return;
        const QPalette pal = view_->palette();
        const bool selected = view_->selectionModel()->isRowSelected(row, QModelIndex());
        const bool hot = hot_ == size_t(row) && soft_;
        const int vw = view_->viewport()->width();
        const QRect band(0, r.y(), vw, r.height());
        // Selection doesn't grey out when the window loses focus.
        if (selected)
            p->fillRect(band, soft_ ? (hot ? soft_->hover_selected : soft_->selected)
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
                paint_cell(p, cell, size_t(row), c, selected, focused);
            else
                text_cell(p, cell, size_t(row), c, selected);
        }
    }

    void paint_overlay(QPainter& qp)
    {
        if (drop_y_)
        {
            qp.fillRect(QRect(0, *drop_y_ - 1, view_->viewport()->width(), 2),
                        view_->palette().color(QPalette::Active, QPalette::Highlight));
        }
        if (cell_.dragging && cell_.target)
        {
            const QRect r = row_rect(*cell_.target).adjusted(1, 1, -2, -2);
            qp.setPen(QPen(view_->palette().color(QPalette::Active, QPalette::Highlight), 2));
            qp.setBrush(Qt::NoBrush);
            qp.drawRect(r);
        }
        if (cell_.dragging && !cell_.start.label.empty())
        {
            qt_painter p(qp, view_);
            text_style ts;
            ts.bold = true;
            const size_f t = p.measure(cell_.start.label, ts);
            const rect_f box{float(cell_.pos.x()) + 12, float(cell_.pos.y()) + 4, t.w + 12, t.h + 4};
            p.rect(box, 4, paint{theme_color::highlight}, paint{theme_color::mid});
            ts.fg = theme_color::highlight_text;
            p.text({box.x + 6, box.y + 2}, cell_.start.label, ts);
        }
        if (auto_.on)
        {
            qt_painter p(qp, view_);
            paint_auto_scroll_anchor(p, float(auto_.anchor.x()), float(auto_.anchor.y()));
        }
    }

    // Overlays are drawn in viewport coordinates; a scroll's blit would
    // carry them along with the rows, so the viewport repaints whole.
    bool overlaid() const { return drop_y_ || cell_.dragging || auto_.on; }

    void fonts_changed()
    {
        fonts_.clear();
        const QFontMetrics fm(view_->font());
        // Compact is a smaller font, not less space round it.
        row_height_ = std::max(fm.height() + 6, 22);
        icon_size_ = std::min(16, row_height_ - 1);
        delegate_->height = row_height_;
        view_->doItemsLayout();
    }

    void palette_changed()
    {
        if (soft_) soft_ = soft_colors_for(view_);
    }

    // A stretching last column keeps the list's right edge: it takes what
    // the others leave as the list resizes, and gives way as they widen.
    void stretch_last()
    {
        const int last = last_visible_column();
        view_->header()->setStretchLastSection(last >= 0 && cols_[size_t(last)].stretch);
    }
    // The last as shown, which a dragged header order can make any of them.
    int last_visible_column() const
    {
        const QHeaderView* h = view_->header();
        for (int v = h->count() - 1; v >= 0; --v)
            if (const int i = h->logicalIndex(v); i >= 0 && !h->isSectionHidden(i)) return i;
        return -1;
    }

    // Other stretch columns share the width the others leave, once, when
    // the list first has a size; after that, widening one column never
    // narrows another (the list scrolls sideways instead).
    void fit_stretch()
    {
        const int width = view_->viewport()->width();
        if (fitted_ || width <= 1) return;
        fitted_ = true;
        QHeaderView* h = view_->header();
        int used = 0, stretching = 0;
        const int filled = h->stretchLastSection() ? last_visible_column() : -1;  // looks after itself
        for (size_t i = 0; i < cols_.size(); ++i)
        {
            if (h->isSectionHidden(int(i))) continue;
            stretching += cols_[i].stretch;  // the filled one has its share left to it
            if (int(i) != filled) used += h->sectionSize(int(i));
        }
        const int spare = width - used;
        if (spare <= 0 || !stretching) return;
        for (size_t i = 0; i < cols_.size(); ++i)
            if (cols_[i].stretch && !h->isSectionHidden(int(i)) && int(i) != filled)
                h->resizeSection(int(i), h->sectionSize(int(i)) + spare / stretching);
    }

    void mouse_press(QMouseEvent* e)
    {
        if (auto_.on)
        {
            stop_auto_scroll();
            return;
        }
        if (auto_scroll_ && e->button() == Qt::MiddleButton)
        {
            start_auto_scroll(e->position().toPoint());
            return;
        }
        // A click on a row's box toggles it and leaves the selection.
        if (checks_ && e->button() == Qt::LeftButton && model_)
        {
            const QModelIndex i = view_->indexAt(e->position().toPoint());
            if (i.isValid() && i.column() == 0 &&
                check_rect(view_->visualRect(i)).contains(e->position().toPoint()))
            {
                model_->toggle_checked(size_t(i.row()));
                e->accept();
                return;
            }
        }
        if (cell_pressed(e))
        {
            view_->base_mouse_press(e);  // selects as usual; dragging begins on motion
            return;
        }
        if (drag_pressed(e)) return;
        view_->base_mouse_press(e);
    }

    void mouse_move(QMouseEvent* e)
    {
        const QPoint pos = e->position().toPoint();
        if (auto_.on)
        {
            auto_.y = pos.y();
            return;
        }
        track_hover(e);
        if (cell_.pressed)
        {
            cell_motion(e);
            return;
        }
        if (drag_.pressed && (e->buttons() & Qt::LeftButton))
        {
            drag_motion(pos);
            return;
        }
        // No sweeping a selection with the button held, as in Windows'
        // report lists and the GTK backends (Qt's default extends it):
        // dragging is for labels and reordering; Shift and Ctrl select.
        if (e->buttons() & Qt::LeftButton) return;
        view_->base_mouse_move(e);
    }

    void mouse_release(QMouseEvent* e)
    {
        if (auto_.on)
        {
            if (e->button() == Qt::MiddleButton) stop_auto_scroll();
            return;
        }
        if (cell_.pressed && e->button() == Qt::LeftButton)
        {
            cell_drag_state d = std::move(cell_);
            cell_ = {};
            if (!d.dragging) return view_->base_mouse_release(e);  // a plain click
            if (active == this) active = nullptr;
            view_->viewport()->unsetCursor();
            view_->viewport()->update();
            if (d.target && cell_drag_.drop) cell_drag_.drop(d.from, *d.target, d.start.data);
            return;
        }
        if (drag_.pressed && e->button() == Qt::LeftButton)
        {
            drag d = drag_;
            drag_ = {};
            if (active == this) active = nullptr;
            if (d.dragging)
            {
                const size_t dest = drop_target(e->position().toPoint().y());
                drop_y_.reset();
                view_->viewport()->unsetCursor();
                view_->viewport()->update();
                if (reorder_ && !is_noop_drop(dest)) reorder_(selection(), dest);
                return;
            }
            if (d.defer_select)
            {
                select({d.row});
                view_->selectionModel()->setCurrentIndex(rows_->index(int(d.row), first_text_column_),
                                                         QItemSelectionModel::NoUpdate);
                return;
            }
        }
        view_->base_mouse_release(e);
    }

    void mouse_double_click(QMouseEvent* e)
    {
        if (e->button() != Qt::LeftButton) return mouse_press(e);
        view_->base_mouse_double_click(e);
        const QModelIndex i = view_->indexAt(e->position().toPoint());
        if (i.isValid() && activate_) activate_(size_t(i.row()));
    }

    // Enter activates the current row (and stops there: a dialog's default
    // button would take it too).
    void key_press(QKeyEvent* e)
    {
        if (e->key() == Qt::Key_Escape && cancel_active_drag()) return;
        if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter)
        {
            if (auto row = current(); row && activate_) activate_(*row);
            e->accept();
            return;
        }
        if (checks_ && e->key() == Qt::Key_Space && !e->modifiers() && model_)
        {
            if (auto row = current()) model_->toggle_checked(*row);
            e->accept();
            return;
        }
        view_->base_key_press(e);
    }

    // Right-click: Qt has already selected the row, or kept a multi-
    // selection that holds it (two revisions to compare).
    void context_menu(QContextMenuEvent* e)
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
                if (cell_menu_ && col >= 0)
                {
                    const QRect r = row_rect(*row);
                    const point_f at{
                        float(e->pos().x() - view_->header()->sectionViewportPosition(col)),
                        float(e->pos().y() - r.y())};
                    show_popup_menu(cell_menu_(*row, size_t(col), at), view_, global);
                    return;
                }
            }
        }
        else
        {
            row = current();
            const QRect r = row ? row_rect(*row) : QRect();
            global = view_->viewport()->mapToGlobal(row ? QPoint(r.x() + 16, r.bottom())
                                                        : QPoint(16, 16));
        }
        if (context_menu_) show_popup_menu(context_menu_(row), view_, global);
    }

    void leave() { set_hot(std::nullopt); }

private:
    static constexpr size_t bulk = 256;

    QRect row_rect(size_t row) const
    {
        const QRect r = view_->visualRect(rows_->index(int(row), first_text_column_));
        return {0, r.y(), view_->viewport()->width(), r.height()};
    }

    std::optional<size_t> row_at(int y) const
    {
        const QModelIndex i = view_->indexAt(QPoint(0, y));
        if (!i.isValid()) return std::nullopt;
        return size_t(i.row());
    }

    void show_sort_indicator()
    {
        QHeaderView* h = view_->header();
        h->setSortIndicatorShown(sort_column_.has_value());
        if (sort_column_)
            h->setSortIndicator(int(*sort_column_), sort_order_ == sort_order::ascending
                                                       ? Qt::AscendingOrder
                                                       : Qt::DescendingOrder);
    }

    // The header flips its own indicator on a click; the app decides.
    void header_clicked(int col)
    {
        const bool asc =
            !(sort_column_ && *sort_column_ == size_t(col) && sort_order_ == sort_order::ascending);
        show_sort_indicator();
        if (!sort_ || size_t(col) >= cols_.size() || !cols_[size_t(col)].sortable) return;
        sort_(size_t(col), asc ? sort_order::ascending : sort_order::descending);
    }

    const std::pair<QFont, QFontMetrics>& font_for(const text_style& s) const
    {
        const int key = (s.bold ? 1 : 0) | (s.italic ? 2 : 0) | (s.monospace ? 4 : 0);
        if (s.size > 0)
        {
            static thread_local std::optional<std::pair<QFont, QFontMetrics>> sized;
            const QFont f = styled_font(view_->font(), s);
            sized.emplace(f, QFontMetrics(f));
            return *sized;
        }
        auto it = fonts_.find(key);
        if (it == fonts_.end())
        {
            const QFont f = styled_font(view_->font(), s);
            it = fonts_.emplace(key, std::pair{f, QFontMetrics(f)}).first;
        }
        return it->second;
    }

    void text_cell(QPainter* p, const QRect& cell, size_t row, size_t c, bool selected) const
    {
        const text_style style = model_->style(row, c);
        const QPalette pal = view_->palette();
        if (style.bg && !selected) p->fillRect(cell, qcolor(resolve(view_, *style.bg)));
        const int pad = compact_ ? 3 : 4;
        QRect area = cell.adjusted(pad, 0, -pad, 0);
        if (checks_ && c == 0) area.setLeft(draw_check(p, cell, row) + 4);
        if (const icon icon = model_->icon(row, c))
        {
            const QIcon qi = to_q_icon(icon, icon_size_, view_);
            qi.paint(p, QRect(area.x(), area.y() + (area.height() - icon_size_) / 2, icon_size_,
                              icon_size_));
            area.setLeft(area.left() + icon_size_ + 4);
        }
        const auto& [font, fm] = font_for(style);
        p->setFont(font);
        p->setPen(style.fg             ? qcolor(resolve(view_, *style.fg))
                  : selected && !soft_ ? pal.color(QPalette::Active, QPalette::HighlightedText)
                                       : pal.color(QPalette::Active, QPalette::Text));
        const QString text = qs(model_->text(row, c));
        p->drawText(area, qt_align(cols_[c].align) | Qt::AlignVCenter | Qt::TextSingleLine,
                    fm.elidedText(text, Qt::ElideRight, area.width()));
    }

    // The style's own item check box, at the start of column 0's cell.
    QRect check_rect(const QRect& cell) const
    {
        QStyleOptionViewItem opt;
        opt.initFrom(view_);
        opt.rect = cell;
        opt.features = QStyleOptionViewItem::HasCheckIndicator;
        QRect r =
            view_->style()->subElementRect(QStyle::SE_ItemViewItemCheckIndicator, &opt, view_);
        const int pad = compact_ ? 3 : 4;
        r.moveTo(cell.left() + pad, cell.top() + (cell.height() - r.height()) / 2);
        return r;
    }
    // Returns the box's right edge.
    int draw_check(QPainter* p, const QRect& cell, size_t row) const
    {
        QStyleOptionViewItem opt;
        opt.initFrom(view_);
        opt.rect = check_rect(cell);
        const check_state st = model_->checked(row);
        opt.state |= st == check_state::checked         ? QStyle::State_On
                     : st == check_state::indeterminate ? QStyle::State_NoChange
                                                        : QStyle::State_Off;
        view_->style()->drawPrimitive(QStyle::PE_IndicatorItemViewItemCheck, &opt, p, view_);
        return opt.rect.right();
    }

    void paint_cell(QPainter* p, const QRect& cell, size_t row, size_t c, bool selected,
                   bool focused) const
    {
        if (auto bg = model_->style(row, c).bg; bg && !selected)
            p->fillRect(cell, qcolor(resolve(view_, *bg)));
        p->save();
        p->setClipRect(cell, Qt::IntersectClip);
        p->translate(cell.topLeft());
        {
            qt_painter painter(*p, view_,
                              selected && !soft_ ? std::optional<paint>{theme_color::highlight_text}
                                                 : std::nullopt);
            if (soft_)
            {
                painter.override_color(theme_color::highlight, from_q_color(soft_->selected));
                painter.override_color(theme_color::highlight_text,
                                      theme_color_of(view_, theme_color::text));
            }
            model_->paint(row, c, painter,
                          cell_info{{float(cell.width()), float(cell.height())}, selected, focused});
        }
        p->restore();
    }

    // Soft-selection lists tint the row under the pointer (as on win32 and
    // GTK, and Explorer's and TortoiseGit's lists).
    void set_hot(std::optional<size_t> row)
    {
        if (row == hot_) return;
        for (std::optional<size_t> r : {hot_, row})
            if (r) view_->viewport()->update(row_rect(*r));
        hot_ = row;
    }
    void track_hover(QMouseEvent* e)
    {
        if (!soft_ || drag_.dragging || cell_.dragging) return set_hot(std::nullopt);
        set_hot(row_at(e->position().toPoint().y()));
    }

    // Drag-reordering (on_reorder): press on a row, move past the drag
    // distance, and a line between rows follows the pointer until release.
    struct drag
    {
        bool pressed = false;
        bool dragging = false;
        bool defer_select = false;  // pressed inside a multi-selection
        QPoint start;
        size_t row = 0;
    };
    // A cell_drag in progress: from a press `begin` accepted.
    struct cell_drag_state
    {
        bool pressed = false, dragging = false;
        QPoint origin, pos;  // the press, the pointer now
        size_t from = 0;
        cell_drag_start start;
        std::optional<size_t> target;
    };

    bool drag_pressed(QMouseEvent* e)
    {
        drag_ = {};
        if (!reorder_ || !reorderable_ || e->button() != Qt::LeftButton ||
            (e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier)))
            return false;
        const QPoint pos = e->position().toPoint();
        auto row = row_at(pos.y());
        if (!row) return false;
        const bool selected = view_->selectionModel()->isRowSelected(int(*row), QModelIndex());
        drag_ = {true, false, selected && view_->selectionModel()->selectedRows().size() > 1, pos,
                 *row};
        // Keep a multi-selection so it can be dragged; a plain click inside
        // it selects just that row on release instead.
        if (drag_.defer_select)
        {
            view_->setFocus(Qt::MouseFocusReason);
            return true;
        }
        view_->base_mouse_press(e);
        return true;
    }

    bool is_noop_drop(size_t dest) const
    {
        const auto sel = selection();
        if (sel.empty()) return true;
        const bool contiguous = sel.back() - sel.front() + 1 == sel.size();
        return contiguous && dest >= sel.front() && dest <= sel.back() + 1;
    }

    // The row the dragged rows would go in front of; sets the drop line.
    size_t drop_target(int y)
    {
        const size_t count = size_t(rows_->count);
        size_t dest = count;
        const int vh = view_->viewport()->height();
        if (auto row = row_at(std::clamp(y, 0, std::max(0, vh - 1))))
        {
            const QRect r = row_rect(*row);
            dest = y < r.center().y() ? *row : *row + 1;
        }
        const bool noop = is_noop_drop(dest);
        view_->viewport()->setCursor(noop ? Qt::ForbiddenCursor : Qt::ClosedHandCursor);
        if (count && !noop)
            drop_y_ = dest >= count ? row_rect(count - 1).bottom() + 1 : row_rect(dest).top();
        else
            drop_y_.reset();
        return dest;
    }

    void edge_scroll(int y)
    {
        const int edge = 24, vh = view_->viewport()->height();
        if (y < edge || y > vh - edge)
        {
            QScrollBar* bar = view_->verticalScrollBar();
            bar->setValue(bar->value() + (y < edge ? -1 : 1) * row_height_);
        }
    }

    void drag_motion(QPoint pos)
    {
        if (!drag_.dragging)
        {
            if ((pos - drag_.start).manhattanLength() < QApplication::startDragDistance()) return;
            drag_.dragging = true;
            active = this;
            set_hot(std::nullopt);
        }
        drop_target(pos.y());
        edge_scroll(pos.y());
        view_->viewport()->update();
    }

    // A press on something the app lets you drag out of a cell. Selection
    // still happens as for any click; the drag starts past the threshold.
    bool cell_pressed(QMouseEvent* e)
    {
        cell_ = {};
        if (!cell_drag_.begin || e->button() != Qt::LeftButton ||
            (e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier)))
            return false;
        const QPoint pos = e->position().toPoint();
        auto row = row_at(pos.y());
        const int col = view_->header()->logicalIndexAt(pos.x());
        if (!row || col < 0) return false;
        const QRect r = row_rect(*row);
        auto start =
            cell_drag_.begin(*row, size_t(col),
                            {float(pos.x() - view_->header()->sectionViewportPosition(col)),
                             float(pos.y() - r.y())});
        if (!start) return false;
        cell_.pressed = true;
        cell_.origin = cell_.pos = pos;
        cell_.from = *row;
        cell_.start = std::move(*start);
        return true;
    }

    void cell_motion(QMouseEvent* e)
    {
        if (!(e->buttons() & Qt::LeftButton)) return;
        const QPoint pos = e->position().toPoint();
        cell_.pos = pos;
        if (!cell_.dragging)
        {
            if ((pos - cell_.origin).manhattanLength() < QApplication::startDragDistance()) return;
            cell_.dragging = true;
            active = this;
            set_hot(std::nullopt);
        }
        cell_.target.reset();
        if (auto row = row_at(pos.y()))
            if (cell_drag_.can_drop && cell_drag_.can_drop(cell_.from, *row, cell_.start.data))
                cell_.target = row;
        view_->viewport()->setCursor(cell_.target ? Qt::ClosedHandCursor : Qt::ForbiddenCursor);
        edge_scroll(pos.y());
        view_->viewport()->update();
    }

    // Autoscroll (spec.auto_scroll): the middle button held, the list
    // scrolls each display frame by the pointer's offset from the anchor.
    void start_auto_scroll(QPoint at)
    {
        auto_ = {true, at, at.y(), double(view_->verticalScrollBar()->value())};
        if (!auto_timer_)
        {
            auto_timer_ = new QTimer(view_);
            auto_timer_->setTimerType(Qt::PreciseTimer);
            QObject::connect(auto_timer_, &QTimer::timeout, view_, [this] { auto_scroll_step(); });
        }
        const QScreen* screen = view_->screen();
        const double hz = screen && screen->refreshRate() > 1 ? screen->refreshRate() : 60;
        auto_timer_->start(std::max(1, int(1000 / hz)));
        clock_.start();
        view_->viewport()->setCursor(Qt::SizeVerCursor);
        view_->viewport()->update();
    }
    void auto_scroll_step()
    {
        const double dt = double(clock_.nsecsElapsed()) / 1e9;
        clock_.restart();
        QScrollBar* bar = view_->verticalScrollBar();
        auto_.pos = std::clamp(auto_.pos + auto_scroll_speed(float(auto_.y - auto_.anchor.y())) * dt,
                               double(bar->minimum()), double(bar->maximum()));
        bar->setValue(int(std::lround(auto_.pos)));
    }
    void stop_auto_scroll()
    {
        if (!auto_.on) return;
        if (auto_timer_) auto_timer_->stop();
        auto_ = {};
        view_->viewport()->unsetCursor();
        view_->viewport()->update();
    }

    static std::set<list_impl*>& lists()  // lists that autoscroll, for Esc
    {
        static std::set<list_impl*> l;
        return l;
    }
    static inline list_impl* active = nullptr;  // the list a drag is in

    list_view* view_;
    rows_model* rows_;
    row_delegate* delegate_;
    list_model* model_ = nullptr;
    std::vector<column> cols_;
    bool compact_;
    bool checks_ = false;  // a checkbox at the start of column 0
    bool auto_scroll_;
    std::optional<soft_colors> soft_;
    int first_text_column_ = 0;
    int row_height_ = 20, icon_size_ = 16;
    bool fitted_ = false;
    mutable std::map<int, std::pair<QFont, QFontMetrics>> fonts_;
    std::optional<size_t> sort_column_;
    sort_order sort_order_ = sort_order::ascending;

    std::function<void()> sel_changed_;
    std::function<void(size_t)> activate_;
    std::function<void(size_t, sort_order)> sort_;
    std::function<menu(std::optional<size_t>)> context_menu_;
    std::function<menu(size_t)> header_menu_;
    std::function<menu(size_t, size_t, point_f)> cell_menu_;
    reorder_fn reorder_;
    bool reorderable_ = true;
    cell_drag cell_drag_;
    drag drag_;
    cell_drag_state cell_;
    std::optional<int> drop_y_;
    std::optional<size_t> hot_;  // the row under the pointer

    struct auto_scroll_state
    {
        bool on = false;
        QPoint anchor;
        int y = 0;       // the pointer
        double pos = 0;  // scroll position, with the fraction a step leaves
    };
    auto_scroll_state auto_;
    QTimer* auto_timer_ = nullptr;
    QElapsedTimer clock_;
};

void list_view::drawRow(QPainter* p, const QStyleOptionViewItem& option,
                       const QModelIndex& index) const
{
    if (owner) owner->paint_row(p, option.rect, index.row());
}

void list_view::paintEvent(QPaintEvent* e)
{
    QTreeView::paintEvent(e);
    if (!owner || !owner->overlaid()) return;
    QPainter p(viewport());
    owner->paint_overlay(p);
}

void list_view::mousePressEvent(QMouseEvent* e)
{
    owner ? owner->mouse_press(e) : QTreeView::mousePressEvent(e);
}

void list_view::mouseMoveEvent(QMouseEvent* e)
{
    owner ? owner->mouse_move(e) : QTreeView::mouseMoveEvent(e);
}

void list_view::mouseReleaseEvent(QMouseEvent* e)
{
    owner ? owner->mouse_release(e) : QTreeView::mouseReleaseEvent(e);
}

void list_view::mouseDoubleClickEvent(QMouseEvent* e)
{
    owner ? owner->mouse_double_click(e) : QTreeView::mouseDoubleClickEvent(e);
}

void list_view::keyPressEvent(QKeyEvent* e)
{
    owner ? owner->key_press(e) : QTreeView::keyPressEvent(e);
}

void list_view::contextMenuEvent(QContextMenuEvent* e)
{
    if (owner) owner->context_menu(e);
}

bool list_view::viewportEvent(QEvent* e)
{
    if (e->type() == QEvent::Leave && owner) owner->leave();
    return QTreeView::viewportEvent(e);
}

void list_view::resizeEvent(QResizeEvent* e)
{
    QTreeView::resizeEvent(e);
    if (owner) owner->fit_stretch();
}

void list_view::changeEvent(QEvent* e)
{
    QTreeView::changeEvent(e);
    if (!owner) return;
    if (e->type() == QEvent::FontChange) owner->fonts_changed();
    if (e->type() == QEvent::PaletteChange) owner->palette_changed();
}

void list_view::scrollContentsBy(int dx, int dy)
{
    QTreeView::scrollContentsBy(dx, dy);
    if (owner && owner->overlaid()) viewport()->update();
}

// -- window -------------------------------------------------------------------

class window_impl;

// Every window is a QDialog, for its keys: Enter presses the default
// button, Esc asks to end a dialog. Closing goes through the WWindow.
class frame_widget : public QDialog
{
public:
    using QDialog::QDialog;
    window_impl* owner = nullptr;
    void reject() override;
    void accept() override {}

protected:
    void closeEvent(QCloseEvent* e) override;
    void changeEvent(QEvent* e) override;
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dropEvent(QDropEvent* e) override;
};

// Keys reach the window's on_key before the focused widget: an application
// event filter that takes them as they're sent to the focus widget.
class key_filter : public QObject
{
public:
    window_impl* owner = nullptr;
    bool eventFilter(QObject* o, QEvent* e) override;
};

class window_impl : public rap::window
{
public:
    window_impl(const window_spec& spec, window_impl* parent)
        : dialog_(spec.dialog)
    {
        frame_ = new frame_widget(parent ? parent->frame_.data() : nullptr);
        frame_->owner = this;
        frame_->setWindowTitle(qs(spec.title));
        frame_->setWindowFlags(dialog_ ? Qt::Dialog : Qt::Window);
        if (dialog_)
            frame_->setWindowModality(parent && spec.modal ? Qt::WindowModal : Qt::NonModal);
        // Snapshot runs are automated: never take the user's keyboard focus.
        if (const char* snap = std::getenv("RAPPORT_SNAPSHOT"); snap && *snap)
            frame_->setAttribute(Qt::WA_ShowWithoutActivating);
        layout_ = new QVBoxLayout(frame_);
        layout_->setSpacing(0);
        filter_ = new key_filter;
        filter_->owner = this;
        qApp->installEventFilter(filter_);
    }

    // Qt widgets go later (deleteLater), as this may run inside one of
    // their signals; until then nothing may reach the parts going now.
    ~window_impl() override
    {
        filter_->owner = nullptr;
        if (qApp) qApp->removeEventFilter(filter_);
        filter_->deleteLater();
        if (last_shown() == frame_) last_shown() = nullptr;
        if (frame_)
        {
            frame_->owner = nullptr;
            for (QObject* o : frame_->findChildren<QObject*>())
            {
                if (auto* h = dynamic_cast<event_hook*>(o)) h->disarm();
                o->blockSignals(true);
            }
            frame_->hide();
            frame_->deleteLater();
        }
        parts.clear();
    }

    void set_content(QWidget* content, const window_spec& spec)
    {
        const int m = !spec.margins ? 0 : dialog_ ? 12 : 6;
        layout_->setContentsMargins(m, m, m, m);
        layout_->addWidget(content);
        if (!spec.menu_bar.empty()) build_menu_bar(spec.menu_bar);
        const int w = px(spec.width), h = px(spec.height);
        frame_->ensurePolished();  // the style's fonts and margins, before sizing by them
        layout_->activate();
        const QSize hint = frame_->sizeHint().expandedTo(frame_->minimumSizeHint());
        const QSize size(w > 0 ? w : hint.width(), h > 0 ? h : hint.height());
        // Fixed at the size it opens at (the spec's, else its content's), not
        // at whatever its content asks for later (QLayout::SetFixedSize
        // would follow a page shown or hidden).
        if (!spec.resizable)
            frame_->setFixedSize(size);
        else if (w > 0 || h > 0)
            frame_->resize(size);
    }

    void set_title(const std::string& t) override { frame_->setWindowTitle(qs(t)); }
    void show() override
    {
        finished_ = false;
        frame_->show();
        if (!frame_->testAttribute(Qt::WA_ShowWithoutActivating))
        {
            frame_->raise();
            frame_->activateWindow();
        }
        snapshot_if_asked();
    }
    void close() override { frame_->hide(); }
    void on_finished(std::function<void(dialog_role)> f) override { on_finished_ = std::move(f); }
    void on_close_request(std::function<bool()> f) override { on_close_request_ = std::move(f); }
    // Drops on any widget that doesn't take them itself reach the frame.
    void on_files_dropped(std::function<void(const std::vector<std::string>&)> f) override
    {
        on_files_dropped_ = std::move(f);
        frame_->setAcceptDrops(bool(on_files_dropped_));
    }
    bool takes_files() const { return bool(on_files_dropped_); }
    void files_dropped(const std::vector<std::string>& paths)
    {
        if (on_files_dropped_) on_files_dropped_(paths);
    }

    size_f size() const override { return {float(frame_->width()), float(frame_->height())}; }
    void resize(size_f s) override
    {
        frame_->resize(std::max(1, int(s.w)), std::max(1, int(s.h)));
        keep_on_screen();
    }
    void set_min_size(size_f s) override { frame_->setMinimumSize(int(s.w), int(s.h)); }
    std::optional<point_f> position() const override
    {
        if (!placeable()) return std::nullopt;
        const QPoint p = frame_->frameGeometry().topLeft();
        return point_f{float(p.x()), float(p.y())};
    }
    void move(point_f p) override
    {
        if (!placeable()) return;
        frame_->move(int(p.x), int(p.y));
        keep_on_screen();
    }
    rect_f work_area() const override
    {
        const QScreen* s = frame_->screen() ? frame_->screen() : QGuiApplication::primaryScreen();
        if (!s) return {};
        const QRect r = s->availableGeometry();
        return without_windows_taskbar(
            {float(r.x()), float(r.y()), float(r.width()), float(r.height())});
    }

    bool fullscreen() const override { return frame_->isFullScreen(); }
    bool maximized() const override { return frame_->isMaximized(); }
    void set_fullscreen(bool on) override
    {
        if (on == frame_->isFullScreen()) return;
        if (on)
        {
            was_maximized_ = frame_->isMaximized();
            frame_->showFullScreen();
        }
        else if (was_maximized_)
            frame_->showMaximized();
        else
            frame_->showNormal();
    }
    void on_fullscreen_changed(std::function<void(bool)> f) override { on_fullscreen_ = std::move(f); }

    void set_icon(const icon& icon) override
    {
        if (!icon.name.empty())
            if (QIcon named = theme_icon(icon.name); !named.isNull())
            {
                frame_->setWindowIcon(named);
                return;
            }
        if (icon.draw) frame_->setWindowIcon(drawn_icon(icon.draw, 256, frame_));
    }

    void set_menu_bar_visible(bool on) override
    {
        menu_bar_wanted_ = on;
        show_menu_bar();
    }

    void on_key(std::function<bool(const key_event&)> f) override { on_key_ = std::move(f); }

    // A dialog button with no handler of its own ends the dialog with its
    // role; OK and Yes are the default, which Enter presses.
    void add_role_button(QPushButton* b, dialog_role role)
    {
        QObject::connect(b, &QPushButton::clicked, b, [this, role] { finish(role); });
        if (role == dialog_role::ok || role == dialog_role::yes) b->setDefault(true);
    }

    QWidget* frame() const { return frame_; }

    std::vector<std::unique_ptr<part>> parts;

    // -- from Frame and key_filter --------------------------------------------

    void escape()
    {
        if (dialog_) close_requested();
    }

    void close_requested()
    {
        if (on_close_request_ && !on_close_request_()) return;
        if (dialog_)
            finish(dialog_role::cancel);
        else
            close();
    }

    void state_changed()
    {
        const bool fs = frame_->isFullScreen();
        if (fs == fullscreen_) return;
        fullscreen_ = fs;
        show_menu_bar();
        if (on_fullscreen_) on_fullscreen_(fs);
    }

    bool filter_key(QObject* o, QEvent* e)
    {
        if (e->type() != QEvent::KeyPress && e->type() != QEvent::KeyRelease) return false;
        auto* w = qobject_cast<QWidget*>(o);
        QWidget* focus = QApplication::focusWidget();
        if (!w || w != (focus ? focus : frame_.data()) || w->window() != frame_) return false;
        auto* k = static_cast<QKeyEvent*>(e);
        if (k->key() == Qt::Key_Escape && e->type() == QEvent::KeyPress &&
            list_impl::cancel_active_drag())
            return true;
        if (!on_key_) return false;
        // While typing, only Escape and the function keys come here.
        auto* plain = qobject_cast<QPlainTextEdit*>(focus);
        const bool typing =
            focus && (qobject_cast<QLineEdit*>(focus) || qobject_cast<QAbstractSpinBox*>(focus) ||
                      (plain && !plain->isReadOnly()));
        const bool not_text =
            k->key() == Qt::Key_Escape || (k->key() >= Qt::Key_F1 && k->key() <= Qt::Key_F12);
        if (typing && !not_text) return false;
        key_event ev;
        ev.press = e->type() == QEvent::KeyPress;
        ev.key = key_for(k->key());
        if (ev.key == key::character) ev.codepoint = char32_t(QChar::toLower(char32_t(k->key())));
        ev.modifiers = modifiers_of(k->modifiers());
        ev.repeat = k->isAutoRepeat();
        return on_key_(ev);
    }

    // The window shown last, as drawn now, to a PNG.
    static bool save_last_shown(const std::string& path)
    {
        QWidget* w = last_shown();
        return w && w->grab().save(qs(path));
    }

private:
    // Development aid: RAPPORT_SNAPSHOT=file.png saves the window shown last,
    // RAPPORT_SNAPSHOT_DELAY ms (default 500) after it appears, then quits.
    static QPointer<QWidget>& last_shown()
    {
        static QPointer<QWidget> w;
        return w;
    }

    void snapshot_if_asked()
    {
        last_shown() = frame_.data();
        static bool taken = false;
        const char* path = std::getenv("RAPPORT_SNAPSHOT");
        if (taken || !path || !*path) return;
        taken = true;
        const char* delay = std::getenv("RAPPORT_SNAPSHOT_DELAY");
        QTimer::singleShot(delay ? std::atoi(delay) : 500, qApp,
                           []
                           {
                               save_last_shown(std::getenv("RAPPORT_SNAPSHOT"));
                               QCoreApplication::exit(0);
                           });
    }

    // Only X11 (and Windows) let apps place their windows.
    static bool placeable() { return QGuiApplication::platformName() != "wayland"; }

    // The frame as it shows. Some window managers (weston's, under WSLg)
    // count an invisible shadow as frame: margins equal on the left, right
    // and bottom and wider than a border are taken for one, and left out.
    QRect visible_frame() const
    {
        const QRect f = frame_->frameGeometry(), g = frame_->geometry();
        const int l = g.left() - f.left(), r = f.right() - g.right(), b = f.bottom() - g.bottom();
        const int shadow = l == r && r == b && l > 8 ? l : 0;
        return f.adjusted(shadow, shadow, -shadow, -shadow);
    }

    // Keeps the whole frame in the work area: the size shrinks to fit, and
    // the position slides back inside, as on win32. Once shown.
    void keep_on_screen()
    {
        if (!frame_->isVisible() || !placeable()) return;
        const rect_f wa = work_area();
        if (wa.w <= 0 || wa.h <= 0) return;
        const QRect f = visible_frame();
        const int extra_w = f.width() - frame_->width(), extra_h = f.height() - frame_->height();
        const int w = std::min(frame_->width(), int(wa.w) - extra_w);
        const int h = std::min(frame_->height(), int(wa.h) - extra_h);
        if (w != frame_->width() || h != frame_->height())
            frame_->resize(std::max(1, w), std::max(1, h));
        const int x =
            std::clamp(f.x(), int(wa.x), std::max(int(wa.x), int(wa.x + wa.w) - (w + extra_w)));
        const int y =
            std::clamp(f.y(), int(wa.y), std::max(int(wa.y), int(wa.y + wa.h) - (h + extra_h)));
        // move() places the outer frame, shadow and all.
        const QPoint outer = frame_->frameGeometry().topLeft();
        if (x != f.x() || y != f.y()) frame_->move(outer.x() + x - f.x(), outer.y() + y - f.y());
    }

    // A menu bar's menus are rebuilt each time one opens, so they show the
    // app's state as it is.
    void build_menu_bar(const std::vector<menu_bar_item>& menus)
    {
        menu_bar_ = new QMenuBar(frame_);
        for (const menu_bar_item& item : menus)
        {
            QMenu* menu = menu_bar_->addMenu(mnemonic(item.title));
            QObject::connect(menu, &QMenu::aboutToShow, menu,
                             [menu, fn = item.menu]
                             {
                                 menu->clear();
                                 if (fn) fill_menu(menu, fn());
                             });
        }
        layout_->setMenuBar(menu_bar_);
    }

    void show_menu_bar()
    {
        if (menu_bar_) menu_bar_->setVisible(menu_bar_wanted_ && !fullscreen_);
    }

    // The callback may delete this window, so it runs last.
    void finish(dialog_role role)
    {
        close();
        if (finished_) return;
        finished_ = true;
        if (auto f = on_finished_) f(role);
    }

    QPointer<frame_widget> frame_;
    QVBoxLayout* layout_;
    key_filter* filter_;
    QMenuBar* menu_bar_ = nullptr;
    bool menu_bar_wanted_ = true;
    bool dialog_;
    bool finished_ = false;
    bool fullscreen_ = false;
    bool was_maximized_ = false;
    std::function<void(dialog_role)> on_finished_;
    std::function<bool()> on_close_request_;
    std::function<void(const std::vector<std::string>&)> on_files_dropped_;
    std::function<void(bool)> on_fullscreen_;
    std::function<bool(const key_event&)> on_key_;
};

void frame_widget::reject()
{
    if (owner) owner->escape();
}

void frame_widget::closeEvent(QCloseEvent* e)
{
    e->ignore();
    if (owner) owner->close_requested();
}

// The local files a drag carries (none for other drags).
std::vector<std::string> local_files(const QMimeData* m)
{
    std::vector<std::string> paths;
    if (m)
        for (const QUrl& url : m->urls())
            if (url.isLocalFile()) paths.push_back(url.toLocalFile().toStdString());
    return paths;
}

void frame_widget::dragEnterEvent(QDragEnterEvent* e)
{
    if (owner && owner->takes_files() && !local_files(e->mimeData()).empty())
        e->acceptProposedAction();
}

void frame_widget::dropEvent(QDropEvent* e)
{
    const std::vector<std::string> paths = local_files(e->mimeData());
    if (!owner || paths.empty()) return;
    e->acceptProposedAction();
    owner->files_dropped(paths);
}

void frame_widget::changeEvent(QEvent* e)
{
    QDialog::changeEvent(e);
    if (e->type() == QEvent::WindowStateChange && owner) owner->state_changed();
}

bool key_filter::eventFilter(QObject* o, QEvent* e)
{
    return owner && owner->filter_key(o, e);
}

// -- builder ------------------------------------------------------------------

class builder
{
public:
    builder(window_impl& win, build_sink& sink)
        : win_(win)
        , sink_(sink)
    {
    }

    QWidget* build(const node& n, orientation parent_dir)
    {
        QWidget* w = std::visit([&](const auto& s) { return make(n, s, parent_dir); }, n.spec);
        // Containers have no interface; one with a handle gets a plain Widget.
        const bool container = std::holds_alternative<spec::box>(n.spec) ||
                               std::holds_alternative<spec::form>(n.spec) ||
                               std::holds_alternative<spec::group>(n.spec) ||
                               std::holds_alternative<spec::scroll>(n.spec) ||
                               std::holds_alternative<spec::toolbar>(n.spec);
        // A widget for a container with a handle, or one created hidden.
        if (container && (n.common.handle || !n.common.visible)) add<plain_impl>(n, w);
        if (int width = px(n.common.width); width > 0) w->setMinimumWidth(width);
        if (int height = px(n.common.height); height > 0) w->setMinimumHeight(height);
        return w;
    }

private:
    static bool is_line_control(const rap::node_spec& s)
    {
        return std::holds_alternative<spec::label>(s) || std::holds_alternative<spec::button>(s) ||
               std::holds_alternative<spec::check>(s) || std::holds_alternative<spec::entry>(s) ||
               std::holds_alternative<spec::drop_down>(s) || std::holds_alternative<spec::combo>(s) ||
               std::holds_alternative<spec::menu_button>(s) ||
               std::holds_alternative<spec::progress_bar>(s) ||
               std::holds_alternative<spec::date_picker>(s);
    }

    static bool fills_form_row(const rap::node_spec& s)
    {
        return std::holds_alternative<spec::entry>(s) || std::holds_alternative<spec::combo>(s) ||
               std::holds_alternative<spec::text_view>(s) || std::holds_alternative<spec::list>(s) ||
               std::holds_alternative<spec::canvas>(s) || std::holds_alternative<spec::box>(s) ||
               std::holds_alternative<spec::form>(s) || std::holds_alternative<spec::splitter>(s) ||
               std::holds_alternative<spec::group>(s) || std::holds_alternative<spec::scroll>(s) ||
               std::holds_alternative<spec::tabs>(s);
    }

    template <class T, class... Args>
    T& add(const node& n, Args&&... args)
    {
        auto* w = new T(std::forward<Args>(args)...);
        win_.parts.emplace_back(w);
        sink_.created(n, *w);
        return *w;
    }

    std::function<QWidget*(const node&)> page_builder()
    {
        return [&win = win_, &sink = sink_](const node& page)
        { return builder(win, sink).build(page, orientation::vertical); };
    }

    // A page: its content with the usual margin around it.
    static QWidget* page_of(QWidget* content)
    {
        auto* page = new QWidget;
        auto* l = new QVBoxLayout(page);
        l->setContentsMargins(6, 6, 6, 6);
        if (content) add_page_content(page, content);
        return page;
    }

    QWidget* make(const node& n, const spec::box& s, orientation o)
    {
        // Controls here are cheap to make: a deferred box is built now.
        if (s.deferred) return make(n, spec::box{s.dir, {s.deferred()}, s.margins, {}}, o);
        auto* box = new QWidget;
        auto* l = new QBoxLayout(s.dir == orientation::vertical ? QBoxLayout::TopToBottom
                                                                : QBoxLayout::LeftToRight,
                                 box);
        l->setSpacing(6);
        if (s.margins)
        {
            // A bar along a window edge: the full margin at its ends, half along its sides.
            const bool row = s.dir == orientation::horizontal;
            l->setContentsMargins(row ? 6 : 3, row ? 3 : 6, row ? 6 : 3, row ? 3 : 6);
        }
        else
            l->setContentsMargins(0, 0, 0, 0);
        for (auto& c : s.items)
        {
            QWidget* w = build(c, s.dir);
            // Single-line controls keep their natural height when a taller
            // sibling sets the row height.
            const Qt::Alignment align = s.dir == orientation::horizontal && is_line_control(c.spec)
                                            ? Qt::AlignVCenter
                                            : Qt::Alignment();
            l->addWidget(w, c.common.stretch, align);
        }
        return box;
    }

    QWidget* make(const node&, const spec::form& s, orientation)
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
            QWidget* field = build(s.fields[i], orientation::horizontal);
            QSizePolicy policy = field->sizePolicy();
            if (fills_form_row(s.fields[i].spec) || s.fields[i].common.stretch > 0)
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
                // set_visible on the field shows and hides its label too.
                field->setProperty("rapport-form-label", QVariant::fromValue<QObject*>(label));
                if (!s.fields[i].common.visible) label->setVisible(false);
                l->addRow(label, field);
            }
        }
        return form;
    }

    QWidget* make(const node& n, const spec::splitter& s, orientation)
    {
        auto* sp = new QSplitter(s.dir == orientation::vertical ? Qt::Vertical : Qt::Horizontal);
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
        add<splitter_impl>(n, sp, stretch);
        return sp;
    }

    QWidget* make(const node&, const spec::scroll& s, orientation)
    {
        auto* area = new QScrollArea;
        area->setFrameShape(QFrame::NoFrame);
        area->setWidgetResizable(true);
        area->setHorizontalScrollBarPolicy(s.horizontal ? Qt::ScrollBarAsNeeded
                                                        : Qt::ScrollBarAlwaysOff);
        if (!s.content.empty()) area->setWidget(build(s.content.front(), orientation::vertical));
        return area;
    }

    QWidget* make(const node&, const spec::group& s, orientation)
    {
        auto* group = new QGroupBox(literal(s.title));
        auto* l = new QVBoxLayout(group);
        l->setContentsMargins(6, 6, 6, 6);
        if (!s.content.empty()) l->addWidget(build(s.content.front(), orientation::vertical));
        return group;
    }

    QWidget* make(const node& n, const spec::tabs& s, orientation)
    {
        if (s.sidebar) return make_sidebar(n, s);
        auto* tabs = new QTabWidget;
        lazy_pages lazy{s.lazy, {}, page_builder()};
        for (size_t i = 0; i < s.pages.size(); ++i)
        {
            const bool is_lazy = i < s.lazy.size() && s.lazy[i];
            QWidget* page = page_of(is_lazy ? nullptr : build(s.pages[i], orientation::vertical));
            tabs->addTab(page, mnemonic(s.titles[i]));
            lazy.pages.push_back(page);
        }
        auto& w = add<tabs_impl>(n, tabs);
        if (!s.lazy.empty()) w.set_lazy(std::move(lazy));
        return tabs;
    }

    // Tabs as a list of page names down the left, the page in a stack beside it.
    QWidget* make_sidebar(const node& n, const spec::tabs& s)
    {
        auto* box = new QWidget;
        auto* l = new QHBoxLayout(box);
        l->setContentsMargins(0, 0, 0, 0);
        l->setSpacing(0);
        auto* list = new QListWidget;
        auto* stack = new QStackedWidget;
        lazy_pages lazy{s.lazy, {}, page_builder()};
        int width = 0;
        for (size_t i = 0; i < s.pages.size(); ++i)
        {
            auto* item = new QListWidgetItem(qs(s.titles[i]).remove('_'), list);
            item->setSizeHint(QSize(0, int(g_metrics.line_height) + 16));
            width = std::max(width, list->fontMetrics().horizontalAdvance(item->text()));
            const bool is_lazy = i < s.lazy.size() && s.lazy[i];
            QWidget* page = page_of(is_lazy ? nullptr : build(s.pages[i], orientation::vertical));
            stack->addWidget(page);
            lazy.pages.push_back(page);
        }
        list->setFixedWidth(width + 48);
        // No focus outline round the current page's title: it's selected.
        list->setStyleSheet("QListWidget { outline: 0; } "
                            "QListWidget::item { padding-left: 12px; padding-right: 12px; }");
        l->addWidget(list);
        auto* line = new QFrame;
        line->setFrameShape(QFrame::VLine);
        line->setFrameShadow(QFrame::Sunken);
        l->addWidget(line);
        l->addWidget(stack, 1);
        auto& tabs = add<side_tabs_impl>(n, box, list, stack);
        tabs.set_current(0);
        if (!s.lazy.empty()) tabs.set_lazy(std::move(lazy));
        return box;
    }

    QWidget* make(const node&, const spec::spacer&, orientation)
    {
        auto* w = new QWidget;
        w->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        return w;
    }

    QWidget* make(const node&, const spec::separator&, orientation parent_dir)
    {
        auto* line = new QFrame;
        line->setFrameShape(parent_dir == orientation::vertical ? QFrame::HLine : QFrame::VLine);
        line->setFrameShadow(QFrame::Sunken);
        return line;
    }

    QWidget* make(const node& n, const spec::label& s, orientation)
    {
        return add<label_impl>(n, s).widget();
    }
    QWidget* make(const node& n, const spec::menu_button& s, orientation)
    {
        return add<menu_button_impl>(n, s).widget();
    }
    QWidget* make(const node& n, const spec::date_picker& s, orientation)
    {
        return add<date_picker_impl>(n, s).widget();
    }
    QWidget* make(const node& n, const spec::check& s, orientation)
    {
        return add<check_impl>(n, s).widget();
    }
    QWidget* make(const node& n, const spec::entry& s, orientation)
    {
        return add<entry_impl>(n, s).widget();
    }
    QWidget* make(const node& n, const spec::drop_down& s, orientation)
    {
        return add<drop_down_impl>(n, s).widget();
    }
    QWidget* make(const node& n, const spec::combo& s, orientation)
    {
        return add<combo_impl>(n, s).widget();
    }
    QWidget* make(const node& n, const spec::text_view& s, orientation)
    {
        return add<text_view_impl>(n, s).widget();
    }
    QWidget* make(const node& n, const spec::list& s, orientation)
    {
        return add<list_impl>(n, s).widget();
    }
    QWidget* make(const node& n, const spec::progress_bar&, orientation)
    {
        return add<progress_bar_impl>(n).widget();
    }

    QWidget* make(const node& n, const spec::button& s, orientation)
    {
        auto* b = new QPushButton(s.icon_only ? QString() : mnemonic(s.text));
        auto& w = add<button_impl>(n, b);
        if (s.icon) w.set_icon(s.icon);
        if (s.icon_only && n.common.tooltip.empty()) w.set_tooltip(s.text);
        b->setFlat(s.flat);
        return b;
    }

    QWidget* make(const node& n, const spec::tool_button& s, orientation)
    {
        return add<tool_button_impl>(n, s).widget();
    }
    // A QToolBar: separators as its own, stretching spacers as expanding gaps.
    QWidget* make(const node&, const spec::toolbar& s, orientation)
    {
        auto* bar = new QToolBar;
        bar->setMovable(false);
        bar->setFloatable(false);
        bar->setIconSize(QSize(16, 16));
        bar->setToolButtonStyle(Qt::ToolButtonIconOnly);
        for (const node& item : s.items)
        {
            if (std::holds_alternative<spec::separator>(item.spec))
            {
                bar->addSeparator();
                continue;
            }
            if (std::holds_alternative<spec::spacer>(item.spec))
            {
                auto* gap = new QWidget;
                gap->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
                bar->addWidget(gap);
                continue;
            }
            QWidget* w = build(item, orientation::horizontal);
            // Only spacers take the toolbar's spare width: a slider (in a
            // box or not) keeps its own width rather than sharing it.
            if (item.common.stretch == 0)
                w->setSizePolicy(QSizePolicy::Fixed, w->sizePolicy().verticalPolicy());
            QAction* action = bar->addWidget(w);
            w->setProperty("rapport-toolbar-action", QVariant::fromValue<QObject*>(action));
        }
        return bar;
    }
    QWidget* make(const node& n, const spec::slider& s, orientation)
    {
        return add<slider_impl>(n, s).widget();
    }
    QWidget* make(const node& n, const spec::spin& s, orientation)
    {
        return add<spin_impl>(n, s).widget();
    }
    QWidget* make(const node& n, const spec::color_button& s, orientation)
    {
        return add<color_button_impl>(n, s).widget();
    }
    QWidget* make(const node& n, const spec::canvas& s, orientation)
    {
        return add<canvas_impl>(n, s).widget();
    }

    // QDialogButtonBox puts the roles in the platform's order, with its
    // stock labels (Help at the far start).
    static QDialogButtonBox::StandardButton standard_for(dialog_role r)
    {
        switch (r)
        {
        case dialog_role::ok: return QDialogButtonBox::Ok;
        case dialog_role::cancel: return QDialogButtonBox::Cancel;
        case dialog_role::apply: return QDialogButtonBox::Apply;
        case dialog_role::close: return QDialogButtonBox::Close;
        case dialog_role::yes: return QDialogButtonBox::Yes;
        case dialog_role::no: return QDialogButtonBox::No;
        case dialog_role::help: return QDialogButtonBox::Help;
        case dialog_role::reset: return QDialogButtonBox::Reset;
        case dialog_role::none: break;
        }
        return QDialogButtonBox::NoButton;
    }

    QWidget* make(const node&, const spec::buttons& s, orientation)
    {
        auto* box = new QDialogButtonBox;
        box->setContentsMargins(0, 6, 0, 0);
        for (auto& b : s.buttons)
        {
            const auto standard = standard_for(b.role);
            QPushButton* pb = standard == QDialogButtonBox::NoButton
                                  ? box->addButton(mnemonic(b.text), QDialogButtonBox::ActionRole)
                                  : box->addButton(standard);
            if (!b.text.empty()) pb->setText(mnemonic(b.text));
            pb->setIcon(QIcon());  // stock labels only, as on GTK and Windows
            auto* w = new button_impl(pb);
            win_.parts.emplace_back(w);
            if (!b.on_click) win_.add_role_button(pb, b.role);
            sink_.created(b, *w);
        }
        return box;
    }

    window_impl& win_;
    build_sink& sink_;
};

// -- backend ------------------------------------------------------------------

class qt6_backend : public backend
{
public:
    static backend* create(int* argc, char** argv)
    {
        QApplication* app = nullptr;  // one made here, deleted with the backend
        if (!qApp)
        {
#if defined(__linux__) || defined(__FreeBSD__)
            // Qt aborts when it has no display; report it so the next backend can try.
            if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY") &&
                !std::getenv("QT_QPA_PLATFORM"))
                return nullptr;
            // Under WSLg, Qt keeps its own choice, Wayland: on its X11 the
            // pointer comes and goes over Qt's windows and doesn't change
            // shape. (Wayland can't place windows, but WSL is for testing.)
#endif
            // Keeps argc: the caller's, alive for the program.
            app = new QApplication(*argc, argv);
            // Qt's own text (standard buttons, its dialogs) in the chosen
            // language, from the translations Qt installs.
            if (!rap::i18n::languages().empty())
            {
                QStringList tags;
                for (const std::string& tag : rap::i18n::languages()) tags << QString::fromStdString(tag);
                auto* qt = new QTranslator(app);
                if (qt->load(QLocale(tags.front()), "qtbase", "_",
                             QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
                    app->installTranslator(qt);
            }
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
        init_metrics();
        return new qt6_backend(app);
    }

    // Windows closed once the loop had ended left their widgets to
    // deleteLater, which no loop will run: they go now, and the
    // QApplication before static teardown, where Qt can no longer release
    // the GL contexts its windows' backing stores made (xcb uses GLX).
    ~qt6_backend() override
    {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        delete app_;
    }

    std::string_view name() const override { return "qt6"; }

    std::unique_ptr<rap::window> create_window(const window_spec& spec, rap::window* parent,
                                                build_sink& sink) override
    {
        auto win = std::make_unique<window_impl>(spec, static_cast<window_impl*>(parent));
        win->set_content(builder(*win, sink).build(spec.content, orientation::vertical), spec);
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

    std::unique_ptr<rap::image> create_image(int w, int h, pixel_format format,
                                              const uint8_t* pixels, size_t stride) override
    {
        return std::make_unique<qt_image>(w, h, format, pixels, stride);
    }

    // `fn` may drop its own subscription; the timer then goes once it returns.
    subscription timer(int ms, bool repeat, std::function<void()> fn) override
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
        return subscription(
            [p = QPointer<QTimer>(t)]
            {
                if (!p) return;
                p->stop();
                p->deleteLater();
            });
    }

    void message(rap::window* parent, const message_spec& m,
                 std::function<void(dialog_role)> done) override
    {
        auto* box = new QMessageBox(parent_widget(parent));
        box->setAttribute(Qt::WA_DeleteOnClose);
        switch (m.icon)
        {
        case message_spec::icon::info: box->setIcon(QMessageBox::Information); break;
        case message_spec::icon::warning: box->setIcon(QMessageBox::Warning); break;
        case message_spec::icon::error: box->setIcon(QMessageBox::Critical); break;
        case message_spec::icon::question: box->setIcon(QMessageBox::Question); break;
        }
        if (!m.title.empty()) box->setWindowTitle(qs(m.title));
        box->setTextFormat(Qt::PlainText);
        box->setText(qs(m.text));
        if (!m.detail.empty()) box->setInformativeText(qs(m.detail));
        const auto roles = m.buttons.empty() ? std::vector<dialog_role>{dialog_role::ok} : m.buttons;
        auto buttons = std::make_shared<std::vector<std::pair<QAbstractButton*, dialog_role>>>();
        for (dialog_role r : roles) buttons->push_back({box->addButton(message_button(r)), r});
        QObject::connect(box, &QMessageBox::finished, box,
                         [box, buttons, done = std::move(done)](int)
                         {
                             dialog_role role = dialog_role::cancel;
                             for (auto& [b, r] : *buttons)
                                 if (b == box->clickedButton()) role = r;
                             if (done) done(role);
                         });
        box->setWindowModality(parent ? Qt::WindowModal : Qt::ApplicationModal);
        box->show();
    }

    void choose_files(rap::window* parent, const file_dialog_spec& f,
                     std::function<void(std::vector<std::string>)> done) override
    {
        auto* dlg = new QFileDialog(parent_widget(parent), qs(f.title));
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        switch (f.mode)
        {
        case file_dialog_spec::mode::open: dlg->setFileMode(QFileDialog::ExistingFile); break;
        case file_dialog_spec::mode::open_many: dlg->setFileMode(QFileDialog::ExistingFiles); break;
        case file_dialog_spec::mode::save:
            dlg->setFileMode(QFileDialog::AnyFile);
            dlg->setAcceptMode(QFileDialog::AcceptSave);
            break;
        case file_dialog_spec::mode::folder:
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
            (*extensions)[name] = default_extension(flt.pattern);
        }
        if (!filters.isEmpty())
        {
            dlg->setNameFilters(filters);
            dlg->setDefaultSuffix((*extensions)[filters.front()]);
            QObject::connect(dlg, &QFileDialog::filterSelected, dlg,
                             [dlg, extensions](const QString& flt)
                             { dlg->setDefaultSuffix((*extensions)[flt]); });
        }
        if (!f.initial_path.empty())
        {
            const QFileInfo info(qs(f.initial_path));
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

    bool save_snapshot(const std::string& path) override { return window_impl::save_last_shown(path); }

    // Qt's print dialog and QPrinter, once the caller has returned. With
    // RAPPORT_PRINT_TO, a .pdf (QPrinter's own) or .png pages, no dialog.
    void print(rap::window* parent, print_job job) override
    {
        QWidget* over = parent ? static_cast<window_impl*>(parent)->frame() : nullptr;
        post([over, job = std::move(job)]() mutable { run_print(over, job); });
    }
    static void run_print(QWidget* parent, print_job& job)
    {
        auto finish = [&](bool ok, std::string error)
        {
            if (job.done) job.done(ok, std::move(error));
        };
        if (!job.draw_page) return finish(false, i18n::tr("rapport", "nothing to print"));
        const QString to = qEnvironmentVariable("RAPPORT_PRINT_TO");
        if (to.endsWith(".png", Qt::CaseInsensitive)) return print_to_png(to, job, finish);
#if RAPPORT_QT_PRINT
        QPrinter printer(QPrinter::HighResolution);
        printer.setDocName(job.title.empty() ? QString(i18n::tr("rapport", "Document")) : qs(job.title));
        if (job.orientation != page_orientation::automatic)
            printer.setPageOrientation(job.orientation == page_orientation::landscape
                                           ? QPageLayout::Landscape
                                           : QPageLayout::Portrait);
        if (to.endsWith(".pdf", Qt::CaseInsensitive))
        {
            printer.setOutputFormat(QPrinter::PdfFormat);
            printer.setOutputFileName(to);
        }
        else
        {
            QPrintDialog dialog(&printer, parent);
            if (dialog.exec() != QDialog::Accepted) return finish(false, {});
        }
        const QRectF area = printer.pageRect(QPrinter::Point);
        const size_f printable{float(area.width()), float(area.height())};
        int pages = job.pages;
        if (job.on_begin)
            if (const int n = job.on_begin(printable); n >= 0) pages = n;
        // The dialog's page range, from 1; none is all of them.
        int first = 0, last = pages - 1;
        if (printer.fromPage() > 0)
        {
            first = std::max(printer.fromPage() - 1, 0);
            last = std::min(printer.toPage() - 1, pages - 1);
        }
        QPainter qp;
        if (!qp.begin(&printer)) return finish(false, i18n::tr("rapport", "printing failed"));
        const float dpi = float(printer.resolution());
        for (int page = first; page <= last; ++page)
        {
            if (page > first) printer.newPage();
            qp.save();
            qp.scale(dpi / 72, dpi / 72);
            qt_painter p(qp, nullptr);
            p.for_print(dpi / 72, 72.f / float(printer.logicalDpiY()));
            job.draw_page(p, page, printable);
            qp.restore();
        }
        const bool ok = qp.end();
        finish(ok, ok ? std::string() : std::string(i18n::tr("rapport", "printing failed")));
#else
        (void)parent;
        finish(false, i18n::tr("rapport", "printing isn't available with this toolkit yet"));
#endif
    }
    // RAPPORT_PRINT_TO=out.png: each page as A4 at 144 dpi, no dialog, as on
    // Windows: for tests.
    template <class Finish>
    static void print_to_png(const QString& path, print_job& job, Finish& finish)
    {
        const bool landscape = job.orientation == page_orientation::landscape;
        const size_f paper = landscape ? size_f{842, 595} : size_f{595, 842};
        const float dpi = 144;
        int pages = job.pages;
        if (job.on_begin)
            if (const int n = job.on_begin(paper); n >= 0) pages = n;
        bool ok = true;
        for (int page = 0; page < pages && ok; ++page)
        {
            QImage image(int(std::lround(paper.w * dpi / 72)), int(std::lround(paper.h * dpi / 72)),
                         QImage::Format_RGB32);
            image.fill(Qt::white);
            {
                QPainter qp(&image);
                qp.scale(dpi / 72, dpi / 72);
                qt_painter p(qp, nullptr);
                p.for_print(dpi / 72, 72.f / float(image.logicalDpiY()));
                job.draw_page(p, page, paper);
            }
            QString out = path;
            if (page > 0)
            {
                const int dot = path.lastIndexOf('.');
                const QString n = "-" + QString::number(page + 1);
                out = dot < 0 ? path + n : path.left(dot) + n + path.mid(dot);
            }
            ok = image.save(out);
        }
        finish(ok, ok ? std::string()
                      : i18n::format(i18n::tr("rapport", "couldn't write {0}"), path.toStdString()));
    }
    void set_clipboard(const std::string& s) override
    {
        QGuiApplication::clipboard()->setText(qs(s));
    }

    void set_clipboard_image(int w, int h, pixel_format format, const uint8_t* pixels,
                           size_t stride) override
    {
        const QImage::Format f = format == pixel_format::rgba8 ? QImage::Format_RGBA8888
                                                              : QImage::Format_ARGB32_Premultiplied;
        // The pixels are the caller's: copied now.
        QGuiApplication::clipboard()->setImage(QImage(pixels, w, h, qsizetype(stride), f).copy());
    }

private:
    static QWidget* parent_widget(rap::window* w)
    {
        return w ? static_cast<window_impl*>(w)->frame() : nullptr;
    }

    static QMessageBox::StandardButton message_button(dialog_role r)
    {
        switch (r)
        {
        case dialog_role::ok: return QMessageBox::Ok;
        case dialog_role::cancel: return QMessageBox::Cancel;
        case dialog_role::apply: return QMessageBox::Apply;
        case dialog_role::close: return QMessageBox::Close;
        case dialog_role::yes: return QMessageBox::Yes;
        case dialog_role::no: return QMessageBox::No;
        case dialog_role::help: return QMessageBox::Help;
        case dialog_role::reset: return QMessageBox::Reset;
        case dialog_role::none: break;
        }
        return QMessageBox::Ok;
    }

    explicit qt6_backend(QApplication* app)
        : app_(app)
    {
    }

    QApplication* app_;
    bool quit_ = false;
    bool running_ = false;
    int code_ = 0;
};

}  // namespace
}  // namespace rap::qt6

RAPPORT_BACKEND_ENTRY(qt6, rap::qt6::qt6_backend)
