// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <common/common_module_aware.h>
#include <nx/vms/client/core/event_search/models/bookmark_search_list_model.h>

class QnCommonModule;

namespace nx::vms::client::desktop {

class BookmarkSearchListModel: public core::BookmarkSearchListModel
{
    Q_OBJECT
    using base_type = core::BookmarkSearchListModel;

public:
    explicit BookmarkSearchListModel(
        QnCommonModule* commonModule,
        QObject* parent = nullptr);

private:
    class Private;
};

} // namespace nx::vms::client::desktop
