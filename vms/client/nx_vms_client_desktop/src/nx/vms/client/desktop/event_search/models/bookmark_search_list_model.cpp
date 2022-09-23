// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "bookmark_search_list_model.h"

#include <QtGui/QPalette>

#include <client/client_globals.h>
#include <nx/vms/client/core/event_search/models/private/bookmark_search_list_model_p.h>
#include <nx/vms/client/desktop/style/skin.h>
#include <ui/help/help_topics.h>

namespace nx::vms::client::desktop {

namespace {

QString iconPath()
{
    return "soft_triggers/user_selectable/bookmark.png";
}

QColor color()
{
    return QPalette().color(QPalette::Light);
}

QPixmap pixmap()
{
    return Skin::colorize(qnSkin->pixmap(iconPath()), color());
}

} // namespace

//-------------------------------------------------------------------------------------------------

class BookmarkSearchListModel::Private: public core::BookmarkSearchListModel::Private
{
    using base_type = core::BookmarkSearchListModel::Private;

public:
    explicit Private(BookmarkSearchListModel* q):
        base_type(q)
    {
    }

    virtual QVariant data(const QModelIndex& index, int role, bool& handled) const override
    {
        switch (role)
        {
            case Qn::DecorationPathRole:
                return iconPath();

            case Qt::DecorationRole:
                return QVariant::fromValue(pixmap());

            case Qt::ForegroundRole:
                return QVariant::fromValue(color());

            case Qn::HelpTopicIdRole:
                return Qn::Bookmarks_Usage_Help;

            default:
                return base_type::data(index, role, handled);
        }
    }
};

BookmarkSearchListModel::BookmarkSearchListModel(
    QnCommonModule* commonModule,
    QObject* parent)
    :
    base_type(commonModule, CreatePrivate([this]() { return new Private(this); }), parent)
{
}

} // namespace nx::vms::client::desktop
