// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include "../bookmark_search_list_model.h"

#include <deque>
#include <limits>
#include <memory>

#include <QtCore/QHash>

#include <camera/camera_bookmarks_manager_fwd.h>
#include <core/resource/camera_bookmark.h>
#include <core/resource/resource_fwd.h>

#include <nx/vms/client/core/event_search/models/private/abstract_async_search_list_model_p.h>
#include <nx/vms/client/core/event_search/utils/event_search_item_helper.h>
#include <nx/vms/client/core/event_search/utils/text_filter_setup.h>

class QnUuid;

namespace nx::vms::client::core {

class BookmarkSearchListModel::Private: public AbstractAsyncSearchListModel::Private
{
    Q_OBJECT
    using base_type = AbstractAsyncSearchListModel::Private;

    BookmarkSearchListModel* const q;

public:
    const std::unique_ptr<TextFilterSetup> textFilter{new TextFilterSetup()};

public:
    explicit Private(BookmarkSearchListModel* q);
    virtual ~Private() override;

    virtual int count() const override;
    virtual QVariant data(const QModelIndex& index, int role, bool& handled) const override;

    virtual void clearData() override;
    virtual void truncateToMaximumCount() override;
    virtual void truncateToRelevantTimePeriod() override;

    void dynamicUpdate(const QnTimePeriod& period);

    bool hasAccessRights() const;

    int referenceItemIndex() const;

    void applyFetchedData(
        BookmarkFetchedData fetched,
        FetchDirection direction);

protected:
    virtual rest::Handle requestPrefetch(const QnTimePeriod& period) override;
    virtual bool commitPrefetch(const QnTimePeriod& periodToCommit) override;

private:
    rest::Handle requestBookmarks(
        const QnTimePeriod& period,
        TailBookmarksCallbackType callback);

    int indexOf(const QnUuid& guid) const; //< Logarithmic complexity.

    QnVirtualCameraResourcePtr camera(const QnCameraBookmark& bookmark) const;

    void watchBookmarkChanges();
    void addBookmark(const QnCameraBookmark& bookmark);
    void updateBookmark(const QnCameraBookmark& bookmark);
    void removeBookmark(const QnUuid& id);

private:
    int m_referenceItemIndex = -1;
    QnCameraBookmarkList m_bookmarks;
    BookmarkFetchedData m_fetched;
    QHash<QnUuid, std::chrono::milliseconds> m_guidToTimestamp;
    QHash<rest::Handle, QnTimePeriod> m_updateRequests;
};

} // namespace nx::vms::client::core
