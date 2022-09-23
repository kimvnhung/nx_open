// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "bookmark_search_list_model_p.h"

#include <QtQml/QQmlEngine>

#include <chrono>
#include <vector>

#include <camera/camera_bookmarks_manager.h>
#include <client_core/client_core_module.h>
#include <common/common_meta_types.h>
#include <common/common_module.h>
#include <core/resource/camera_resource.h>
#include <core/resource_access/global_permissions_manager.h>
#include <core/resource_access/resource_access_subject.h>
#include <core/resource_management/resource_pool.h>
#include <nx/utils/datetime.h>
#include <nx/utils/guarded_callback.h>
#include <nx/utils/log/log.h>
#include <nx/utils/pending_operation.h>
#include <nx/utils/range_adapters.h>
#include <nx/utils/scope_guard.h>
#include <nx/vms/client/core/event_search/utils/bookmark_accessor.h>
#include <nx/vms/client/core/event_search/utils/event_search_item_helper.h>
#include <nx/vms/client/core/client_core_globals.h>
#include <nx/vms/client/core/watchers/user_watcher.h>
#include <nx/vms/common/html/html.h>
#include <nx/vms/common/system_context.h>

namespace nx::vms::client::core {

namespace {

using namespace std::chrono;

QString parseHyperlinks(const QString& text)
{
    if (text.isEmpty())
        return "";

    QString result;
    static const QString urlPattern =
        R"(\b((http(s)?:\/\/|www\.)([-A-Z0-9+&@#\/%=~_|$\?!:,.])*([A-Z0-9+&@#\/%=~_|$])))";

    QRegularExpression rx(urlPattern, QRegularExpression::CaseInsensitiveOption);

    int pos = 0;
    int lastPos = 0;

    auto it = rx.globalMatch(text);
    while (it.hasNext())
    {
        const QRegularExpressionMatch match = it.next();

        pos = match.capturedStart();
        result.append(text.mid(lastPos, pos - lastPos));
        lastPos = pos;
        pos += match.capturedLength();
        QString linkText = text.mid(lastPos, pos - lastPos);
        QString link = linkText;
        if (link.startsWith("www."))
            link.prepend("https://");
        result.append(nx::vms::common::html::customLink(linkText, link));
        lastPos = pos;
    }

    result.append(text.mid(lastPos));
    return result;
}

} // namespace

BookmarkSearchListModel::Private::Private(BookmarkSearchListModel* q):
    base_type(q),
    q(q)
{
    connect(textFilter.get(), &TextFilterSetup::textChanged, q, &AbstractSearchListModel::clear);
    watchBookmarkChanges();
}

BookmarkSearchListModel::Private::~Private()
{
}

int BookmarkSearchListModel::Private::count() const
{
    return int(m_bookmarks.size());
}

QVariant BookmarkSearchListModel::Private::data(const QModelIndex& index, int role,
    bool& handled) const
{
    const auto& bookmark = m_bookmarks[index.row()];
    handled = true;

    switch (role)
    {
        case Qt::DisplayRole:
            return bookmark.name;

        case DescriptionTextRole:
            return parseHyperlinks(nx::vms::common::html::toHtml(bookmark.description));

        case TimestampRole:
        case PreviewTimeRole:
            return QVariant::fromValue(bookmark.startTimeMs);

        case TimestampMsRole:
            return QVariant::fromValue(duration_cast<milliseconds>(bookmark.startTimeMs).count());

        case DurationRole:
            return QVariant::fromValue(bookmark.durationMs);

        case DurationMsRole:
            return QVariant::fromValue(duration_cast<milliseconds>(bookmark.durationMs).count());

        case UuidRole:
            return QVariant::fromValue(bookmark.guid);

        case ResourceListRole:
        case DisplayedResourceListRole:
        {
            if (const auto resource = camera(bookmark))
                return QVariant::fromValue(QnResourceList({resource}));

            if (role == DisplayedResourceListRole)
                return QVariant::fromValue(QStringList({QString("<%1>").arg(tr("deleted camera"))}));

            return {};
        }

        case ResourceRole:
            return QVariant::fromValue<QnResourcePtr>(camera(bookmark));

        case RawResourceRole:
        {
            const auto resource = camera(bookmark);
            if (!resource)
                return {};

            QQmlEngine::setObjectOwnership(resource.get(), QQmlEngine::CppOwnership);
            return QVariant::fromValue(resource.get());
        }

        case CameraBookmarkRole:
            return QVariant::fromValue(bookmark);

        case BookmarkTagRole:
            return QVariant::fromValue(bookmark.tags);

        default:
            handled = false;
            return {};
    }
}

void BookmarkSearchListModel::Private::clearData()
{
    ScopedReset reset(q, !m_bookmarks.empty());
    m_bookmarks.clear();
    m_guidToTimestamp.clear();
    m_fetched = {};
    m_updateRequests.clear();

    q->setFetchedTimeWindow({});
}

void BookmarkSearchListModel::Private::truncateToMaximumCount()
{
    const auto itemCleanup =
        [this](const QnCameraBookmark& item) { m_guidToTimestamp.remove(item.guid); };

    q->truncateDataToMaximumCount(m_bookmarks,
        [](const QnCameraBookmark& item) { return item.startTimeMs; },
        itemCleanup);
}

void BookmarkSearchListModel::Private::truncateToRelevantTimePeriod()
{
    const auto itemCleanup =
        [this](const QnCameraBookmark& item) { m_guidToTimestamp.remove(item.guid); };

    const auto timestampGetter =
        [](const QnCameraBookmark& bookmark) { return bookmark.startTimeMs; };

    q->truncateDataToTimePeriod(
        m_bookmarks, timestampGetter, q->relevantTimePeriod(), itemCleanup);
}

rest::Handle BookmarkSearchListModel::Private::requestBookmarks(
    const QnTimePeriod& period,
    TailBookmarksCallbackType callback)
{
    QnCameraBookmarkSearchFilter filter;
    filter.text = textFilter->text();
    filter.limit = q->maximumCount();
    filter.orderBy = QnBookmarkSortOrder(api::BookmarkSortField::startTime, Qt::DescendingOrder);
    filter.startTimeMs = period.startTime();
    filter.endTimeMs = period.endTime();

    for (const auto& camera: q->cameras())
        filter.cameras.insert(camera->getId());

    const auto handler =
        [this, callback](bool success, rest::Handle requestId, QnCameraBookmarkList bookmarks)
        {
            if (!requestId || requestId != currentRequest().id)
                return;

            auto fetched = reorderAroundReferencePoint<BookmarkAccessor>(
                m_bookmarks, bookmarks, FetchDirection::earlier);
            truncateFetchedData(fetched, FetchDirection::earlier, q->maximumCount(),
                /*maxTailSize*/ 0);
            callback(success, requestId, fetched);
        };

    return qnCameraBookmarksManager->getBookmarksAsync(filter, handler);
}

rest::Handle BookmarkSearchListModel::Private::requestPrefetch(const QnTimePeriod& period)
{
    const auto request = currentRequest();

    QnTailBookmarkSearchFilter filter;
    filter.startPointMs = request.direction == EventSearch::FetchDirection::earlier
        ? period.endTime()
        : period.startTime();
    filter.referencePointMs = request.direction == EventSearch::FetchDirection::earlier
        ? period.startTime()
        : period.endTime();
    filter.minStartTimeMs = q->relevantTimePeriod().startTime();

    filter.text = textFilter->text();
    filter.bodyLimit = q->maximumCount();
    filter.tailLimit = request.batchSize;

    for (const auto& camera: q->cameras())
        filter.cameras.insert(camera->getId());

    const auto callback = TailBookmarksCallbackType(nx::utils::guarded(this,
        [this, filter](bool success, int requestId, const BookmarkFetchedData& fetched)
        {
            if (!requestId || requestId != currentRequest().id)
                return;

            if (success)
            {
                m_fetched = fetched;
                const auto targetPeriod = actualPeriod<BookmarkAccessor>(m_fetched.data);
                completePrefetch(targetPeriod, /*success*/ true, m_fetched.data.size());
            }
            else
            {
                const auto targetPeriod = actualPeriod<BookmarkAccessor>(m_bookmarks);
                completePrefetch(targetPeriod, /*success*/ false, 0);
            }
        }));

    return q->rowCount()
        ? qnCameraBookmarksManager->getTailBookmarksAsync(filter, m_bookmarks, callback)
        : requestBookmarks(period, callback);
}

bool BookmarkSearchListModel::Private::commitPrefetch(const QnTimePeriod& /*fetchedPeriod*/)
{
    applyFetchedData(m_fetched, currentRequest().direction);
    m_fetched = {};

    return true;
}

void BookmarkSearchListModel::Private::applyFetchedData(
    BookmarkFetchedData fetched,
    FetchDirection direction)
{
    updateSearchEvents(q,
        m_bookmarks,
        fetched.data.begin() + fetched.body.offset,
        fetched.data.begin() + fetched.body.offset + fetched.body.length);

    insertTail(q, m_bookmarks, fetched.data.begin() + fetched.tail.offset,
        fetched.data.begin() + fetched.tail.offset + fetched.tail.length, direction);

    m_guidToTimestamp.clear();
    for (auto it = fetched.data.cbegin(); it != fetched.data.end(); ++it)
        m_guidToTimestamp[it->guid] = it->startTimeMs;

    const int referenceIndex =
        [direction, fetched]()
        {
            if (direction == FetchDirection::earlier)
                return  std::max(fetched.body.length - 1, 0);

            return fetched.body.length
                ? std::min(fetched.tail.length, int(fetched.data.size() - 1))
                : 0;
        }();

    if (referenceIndex == m_referenceItemIndex)
        return;

    m_referenceItemIndex = referenceIndex;
    emit q->referenceItemIndexChanged();
}

void BookmarkSearchListModel::Private::watchBookmarkChanges()
{
    // TODO: #vkutin Check whether qnCameraBookmarksManager won't emit these signals
    // if current user has no GlobalPermission::viewBookmarks

    connect(qnCameraBookmarksManager, &QnCameraBookmarksManager::bookmarkAdded,
        this, &Private::addBookmark);

    connect(qnCameraBookmarksManager, &QnCameraBookmarksManager::bookmarkUpdated,
        this, &Private::updateBookmark);

    connect(qnCameraBookmarksManager, &QnCameraBookmarksManager::bookmarkRemoved,
        this, &Private::removeBookmark);
}

void BookmarkSearchListModel::Private::addBookmark(const QnCameraBookmark& bookmark)
{
    // Skip bookmarks outside of time range.
    if (!q->fetchedTimeWindow().contains(bookmark.startTimeMs.count()))
        return;

    if (!NX_ASSERT(!m_guidToTimestamp.contains(bookmark.guid), "Bookmark already exists"))
    {
        updateBookmark(bookmark);
        return;
    }

    const auto insertionPos = std::lower_bound(m_bookmarks.cbegin(), m_bookmarks.cend(),
        bookmark.startTimeMs, Predicate<BookmarkAccessor>::lowerBound());

    const auto index = std::distance(m_bookmarks.cbegin(), insertionPos);

    ScopedInsertRows insertRows(q, index, index);
    m_bookmarks.insert(m_bookmarks.begin() + index, bookmark);
    m_guidToTimestamp[bookmark.guid] = bookmark.startTimeMs;
    insertRows.fire();

    if (count() > q->maximumCount())
    {
        NX_VERBOSE(q, "Truncating to maximum count");
        truncateToMaximumCount();
    }
}

void BookmarkSearchListModel::Private::updateBookmark(const QnCameraBookmark& bookmark)
{
    const auto index = indexOf(bookmark.guid);
    if (index < 0)
        return;

    if (m_bookmarks[index].startTimeMs == bookmark.startTimeMs)
    {
        // Update data.
        const auto modelIndex = q->index(index);
        m_bookmarks[index] = bookmark;
        emit q->dataChanged(modelIndex, modelIndex);
    }
    else
    {
        // Normally bookmark timestamp should not change, but handle it anyway.
        removeBookmark(bookmark.guid);
        addBookmark(bookmark);
    }
}

void BookmarkSearchListModel::Private::removeBookmark(const QnUuid& id)
{
    const auto index = indexOf(id);
    if (index < 0)
        return;

    ScopedRemoveRows removeRows(q,  index, index);
    m_bookmarks.erase(m_bookmarks.begin() + index);
    m_guidToTimestamp.remove(id);
}

void BookmarkSearchListModel::Private::dynamicUpdate(const QnTimePeriod& period)
{
    // This function exists until we implement notifying all clients about every bookmark change.

    const auto effectivePeriod = period.intersected(q->fetchedTimeWindow());
    if (effectivePeriod.isEmpty() || q->isFilterDegenerate())
        return;

    const auto callback = TailBookmarksCallbackType(
        [this](bool success, rest::Handle requestId, const BookmarkFetchedData& fetched)
        {
            // It doesn't matter if we receive results limited by maximum count.
            if (success && requestId && m_updateRequests.remove(requestId))
                applyFetchedData(fetched, FetchDirection::earlier);
        });

    NX_VERBOSE(q, "Dynamic update request");

    if (const auto requestId = requestBookmarks(effectivePeriod, callback))
        m_updateRequests[requestId] = effectivePeriod;
}

bool BookmarkSearchListModel::Private::hasAccessRights() const
{
    const auto user = q->commonModule()->instance<UserWatcher>()->user();
    if (!user)
        return false;

    const auto permissionManager = q->commonModule()->globalPermissionsManager();
    return permissionManager->hasGlobalPermission(user, GlobalPermission::viewBookmarks);
}

int BookmarkSearchListModel::Private::referenceItemIndex() const
{
    return m_referenceItemIndex;
}

int BookmarkSearchListModel::Private::indexOf(const QnUuid& guid) const
{
    const auto iter = m_guidToTimestamp.find(guid);
    if (iter == m_guidToTimestamp.end())
        return -1;

    const auto range = std::make_pair(
        std::lower_bound(m_bookmarks.cbegin(), m_bookmarks.cend(), iter.value(), Predicate<BookmarkAccessor>::lowerBound()),
        std::upper_bound(m_bookmarks.cbegin(), m_bookmarks.cend(), iter.value(), Predicate<BookmarkAccessor>::upperBound()));

    const auto pos = std::find_if(range.first, range.second,
        [&guid](const QnCameraBookmark& item) { return item.guid == guid; });

    return pos != range.second ? std::distance(m_bookmarks.cbegin(), pos) : -1;
}

QnVirtualCameraResourcePtr BookmarkSearchListModel::Private::camera(
    const QnCameraBookmark& bookmark) const
{
    const auto user = qnClientCoreModule->commonModule()->instance<UserWatcher>()->user();
    if (!user)
        return {};

    const auto resourcePool = user->systemContext()->resourcePool();

    return resourcePool->getResourceById<QnVirtualCameraResource>(bookmark.cameraId);
}

} // namespace nx::vms::client::core
