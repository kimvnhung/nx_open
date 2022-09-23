// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "bookmark_search_list_model.h"
#include "private/bookmark_search_list_model_p.h"

#include <nx/vms/client/core/client_core_globals.h>

namespace nx::vms::client::core {

BookmarkSearchListModel::BookmarkSearchListModel(
    QnCommonModule* commonModule,
    QObject* parent)
    :
    BookmarkSearchListModel(commonModule, [this]() { return new Private(this); }, parent)
{
}

BookmarkSearchListModel::BookmarkSearchListModel(
    QnCommonModule* commonModule,
    CreatePrivate creator,
    QObject* parent)
    :
    base_type(commonModule, creator, parent),
    QnCommonModuleAware(commonModule),
    d(qobject_cast<Private*>(base_type::d.data()))
{
}

TextFilterSetup* BookmarkSearchListModel::textFilter() const
{
    return d->textFilter.get();
}

bool BookmarkSearchListModel::isConstrained() const
{
    return !d->textFilter->text().isEmpty() || base_type::isConstrained();
}

bool BookmarkSearchListModel::hasAccessRights() const
{
    return d->hasAccessRights();
}

void BookmarkSearchListModel::dynamicUpdate(const QnTimePeriod& period)
{
    d->dynamicUpdate(period);
}

int BookmarkSearchListModel::referenceItemIndex() const
{
    return d->referenceItemIndex();
}

QHash<int, QByteArray> BookmarkSearchListModel::roleNames() const
{
    auto result = base_type::roleNames();
    result.insert(clientCoreRoleNames());
    return result;
}

} // namespace nx::vms::client::core
