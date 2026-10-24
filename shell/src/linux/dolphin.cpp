// The provider as a KDE file item action plugin (KF6 KIO), which Dolphin and
// other KIO file managers show in their context menus; in
// <qt plugins>/kf6/kfileitemaction, listed under Dolphin's Configure >
// Context Menu. Compiled into each extension rather than kept in a library:
// the plugin's metadata, the extension's name and id, is built into the class
// from rapport-shell-dolphin.json, which rapport_add_shell_extension() writes
// for each extension.
#include "adapter.h"

#include <KAbstractFileItemActionPlugin>
#include <KFileItem>
#include <KFileItemListProperties>
#include <KPluginFactory>
#include <QAction>
#include <QGuiApplication>
#include <QIcon>
#include <QMenu>

namespace
{

using namespace rap::shell::adapter;
using rap::shell::menu_item;

QIcon to_icon(const std::string& icon)
{
    const std::string spec = icon_spec(icon);
    if (spec.empty()) return {};
    const QString s = QString::fromStdString(spec);
    return spec.front() == '/' ? QIcon(s) : QIcon::fromTheme(s);
}

void add_actions(QWidget* menu_parent, QObject* owner, QList<QAction*>& out,
                 const std::vector<menu_item>& items, const std::shared_ptr<request>& r)
{
    for (const menu_item& mi : items)
    {
        auto* action = new QAction(owner);
        out.push_back(action);
        if (mi.is_separator())
        {
            action->setSeparator(true);
            continue;
        }
        // Qt's mnemonics are the provider's: '&', and "&&" for a literal one.
        action->setText(QString::fromStdString(mi.label));
        action->setIcon(to_icon(mi.icon));
        action->setEnabled(mi.enabled);
        if (!mi.submenu.empty())
        {
            auto* sub = new QMenu(menu_parent);
            QList<QAction*> children;
            add_actions(menu_parent, sub, children, mi.submenu, r);
            sub->addActions(children);
            action->setMenu(sub);
            continue;
        }
        QObject::connect(action, &QAction::triggered, action, [r, id = mi.id] { choose(*r, id); });
    }
}

class file_item_action : public KAbstractFileItemActionPlugin
{
    Q_OBJECT

public:
    file_item_action(QObject* parent, const QVariantList&)
        : KAbstractFileItemActionPlugin(parent)
    {
    }

    QList<QAction*> actions(const KFileItemListProperties& props, QWidget* parent) override
    {
        QList<QAction*> out;
        try
        {
            // A right-click on a folder's empty space comes as that folder,
            // like a click on the folder itself.
            rap::shell::selection sel;
            for (const KFileItem& item : props.items())
            {
                const QUrl url = item.mostLocalUrl();
                if (!url.isLocalFile()) return {};
                sel.paths.emplace_back(url.toLocalFile().toStdString());
            }
            if (sel.paths.empty()) return {};
            sel.folder = sel.paths.front().parent_path();
            sel.extended = QGuiApplication::queryKeyboardModifiers().testFlag(Qt::ShiftModifier);
            std::vector<menu_item> items;
            auto r = ask(std::move(sel), items, true);
            if (r) add_actions(parent, parent, out, items, r);
        }
        catch (...)
        {
            for (QAction* a : out) delete a;
            out.clear();
        }
        return out;
    }
};

}  // namespace

K_PLUGIN_CLASS_WITH_JSON(file_item_action, "rapport-shell-dolphin.json")

#include "dolphin.moc"
