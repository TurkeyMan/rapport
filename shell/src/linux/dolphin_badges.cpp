// The provider's badges as a KIO overlay icon plugin (KF6 KIO), which Dolphin
// draws over file icons; in <qt plugins>/kf6/overlayicon. Built into each
// extension as a module of its own beside the menu's (one plugin class per
// module), with metadata rapport_add_shell_extension() writes.
//
// KIO asks getOverlays() on its GUI thread, so it answers from what it has
// and asks the provider on a worker; the answer comes back on the GUI thread
// and, where it differs, goes out with overlaysChanged. A badge the provider
// says changed is forgotten and asked again the same way.
#include "adapter.h"

#include <KOverlayIconPlugin>
#include <KPluginFactory>
#include <QMetaObject>
#include <QMutex>
#include <QPointer>
#include <QThreadPool>
#include <QUrl>

#include <unordered_map>
#include <unordered_set>

namespace
{

using namespace rap::shell::adapter;

class badge_overlays;

// The workers that ask the provider, its own: Dolphin and KIO share Qt's
// global pool, and an answer can take a while. Never deleted, since the
// plugin may outlive the statics.
QThreadPool* askers()
{
    static QThreadPool* pool = []
    {
        auto* p = new QThreadPool;
        p->setMaxThreadCount(4);
        return p;
    }();
    return pool;
}

// The plugins alive, for the provider's word that badges changed, which
// comes from its own thread: a process may make more than one.
QMutex alive_lock;
std::unordered_set<badge_overlays*>& alive()
{
    static auto* s = new std::unordered_set<badge_overlays*>;
    return *s;
}

class badge_overlays : public KOverlayIconPlugin
{
    Q_OBJECT

public:
    badge_overlays(QObject* parent, const QVariantList&)
        : KOverlayIconPlugin(parent)
    {
        {
            QMutexLocker lock(&alive_lock);
            alive().insert(this);
        }
        watch_badges(
            [](std::vector<std::filesystem::path> paths)
            {
                QMutexLocker lock(&alive_lock);
                for (badge_overlays* p : alive())
                    QMetaObject::invokeMethod(
                        p, [p, paths] { p->changed(paths); }, Qt::QueuedConnection);
            });
    }

    ~badge_overlays() override
    {
        QMutexLocker lock(&alive_lock);
        alive().erase(this);
    }

    QStringList getOverlays(const QUrl& url) override
    {
        if (!url.isLocalFile() || !has_badges()) return {};
        const std::string path = url.toLocalFile().toStdString();
        if (auto it = known_.find(path); it != known_.end())
            return it->second.empty() ? QStringList()
                                      : QStringList{QString::fromStdString(it->second)};
        ask(path, url);
        return {};
    }

private:
    // Asks on a worker; the answer is kept, and told if it's news.
    void ask(const std::string& path, const QUrl& url)
    {
        if (!asking_.insert(path).second) return;
        QPointer<badge_overlays> self(this);
        askers()->start(
            [self, path, url]
            {
                std::string icon = badge_icon(path);
                QMetaObject::invokeMethod(
                    self,
                    [self, path, url, icon = std::move(icon)]
                    {
                        if (self) self->answered(path, url, icon);
                    },
                    Qt::QueuedConnection);
            });
    }

    void answered(const std::string& path, const QUrl& url, const std::string& icon)
    {
        asking_.erase(path);
        if (known_.size() > 50000) known_.clear();
        auto [it, added] = known_.try_emplace(path, icon);
        if (!added && it->second == icon) return;
        it->second = icon;
        // Nothing was drawn for a path first asked with no badge.
        if (added && icon.empty()) return;
        Q_EMIT overlaysChanged(url, icon.empty() ? QStringList()
                                                 : QStringList{QString::fromStdString(icon)});
    }

    // The provider's word: these paths' badges (or all, for none) changed.
    // For all, those with a badge are asked again (one may have gone), and
    // those without are forgotten, to be asked when Dolphin next shows them.
    void changed(const std::vector<std::filesystem::path>& paths)
    {
        auto again = [&](const std::string& p)
        {
            if (known_.contains(p)) ask(p, QUrl::fromLocalFile(QString::fromStdString(p)));
        };
        if (paths.empty())
        {
            std::vector<std::string> badged;
            for (auto it = known_.begin(); it != known_.end();)
                if (it->second.empty())
                    it = known_.erase(it);
                else
                    badged.push_back((it++)->first);
            for (const std::string& p : badged) again(p);
        }
        else
            for (const auto& p : paths) again(p.string());
    }

    std::unordered_map<std::string, std::string> known_;  // path to icon, or none
    std::unordered_set<std::string> asking_;
};

}  // namespace

K_PLUGIN_CLASS_WITH_JSON(badge_overlays, "rapport-shell-dolphin-badges.json")

#include "dolphin_badges.moc"
