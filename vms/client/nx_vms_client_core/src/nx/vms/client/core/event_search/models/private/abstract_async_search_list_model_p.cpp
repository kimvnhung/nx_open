// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "abstract_async_search_list_model_p.h"

#include <core/resource/camera_resource.h>
#include <utils/common/synctime.h>

#include <nx/utils/guarded_callback.h>
#include <nx/utils/log/log.h>

namespace nx::vms::client::core {

AbstractAsyncSearchListModel::Private::Private(AbstractAsyncSearchListModel* q):
    base_type(),
    q(q)
{
}

AbstractAsyncSearchListModel::Private::~Private()
{
}

bool AbstractAsyncSearchListModel::Private::requestFetch()
{
    return prefetch(nx::utils::guarded(this,
        [this](const QnTimePeriod& fetchedPeriod, EventSearch::FetchResult result)
        {
            defaultPrefetchHandler(fetchedPeriod, result);
        }));
}

bool AbstractAsyncSearchListModel::Private::fetchWindow(const QnTimePeriod& window)
{
    const auto effectiveWindow = window.intersected(q->relevantTimePeriod());
    if (!NX_ASSERT(!effectiveWindow.isEmpty()))
        return false;

    q->clear();

    return prefetchWindow(effectiveWindow, nx::utils::guarded(this,
        [this](const QnTimePeriod& fetchedPeriod, EventSearch::FetchResult result)
        {
            defaultPrefetchHandler(fetchedPeriod, result);
        }));
}

void AbstractAsyncSearchListModel::Private::defaultPrefetchHandler(
    const QnTimePeriod& fetchedPeriod, EventSearch::FetchResult result)
{
    ScopedFetchCommit scopedFetch(q, m_fetchData.info.direction, result);

    q->setFetchedTimeWindow(fetchedPeriod);

    if (result == EventSearch::FetchResult::complete || result == EventSearch::FetchResult::incomplete)
        commit(fetchedPeriod);
}

void AbstractAsyncSearchListModel::Private::cancelPrefetch()
{
    const auto tmp = m_fetchData;
    m_fetchData = {};

    if (tmp.info.id && tmp.handler)
        tmp.handler({}, EventSearch::FetchResult::cancelled);
}

bool AbstractAsyncSearchListModel::Private::prefetch(FetchCompletionHandler completionHandler)
{
    if (fetchInProgress() || !completionHandler)
        return false;

    const auto& fetchedWindow = q->fetchedTimeWindow();
    m_fetchData.info.period = fetchedWindow.isEmpty()
        ? q->relevantTimePeriod()
        : fetchedWindow;
    m_fetchData.info.direction = q->fetchDirection();
    m_fetchData.info.batchSize = q->fetchBatchSize();
    m_fetchData.info.id = requestPrefetch(m_fetchData.info.period);
    if (!m_fetchData.info.id)
        return false;

    NX_VERBOSE(q, "Prefetch id: %1", m_fetchData.info.id);

    m_fetchData.handler = completionHandler;
    emit q->asyncFetchStarted(m_fetchData.info.direction, {});

    return true;
}

bool AbstractAsyncSearchListModel::Private::prefetchWindow(
    const QnTimePeriod& window, FetchCompletionHandler completionHandler)
{
    if (fetchInProgress() || !completionHandler || !q->fetchedTimeWindow().isEmpty())
        return false;

    const auto effectiveWindow(window.intersected(q->relevantTimePeriod()));
    if (effectiveWindow.isEmpty())
        return false;

    m_fetchData.info.direction = EventSearch::FetchDirection::earlier;
    m_fetchData.info.batchSize = q->maximumCount();
    m_fetchData.info.period = effectiveWindow;

    m_fetchData.info.id = requestPrefetch(m_fetchData.info.period);
    if (!m_fetchData.info.id)
        return false;

    NX_VERBOSE(q, "Prefetch id: %1", m_fetchData.info.id);

    m_fetchData.handler = completionHandler;
    emit q->asyncFetchStarted(m_fetchData.info.direction, {});

    return true;
}

void AbstractAsyncSearchListModel::Private::commit(const QnTimePeriod& periodToCommit)
{
    if (!fetchInProgress())
        return;

    NX_VERBOSE(q, "Commit id: %1", m_fetchData.info.id);

    commitPrefetch(periodToCommit);

    if (count() > q->maximumCount())
    {
        NX_VERBOSE(q, "Truncating to maximum count");
        q->truncateToMaximumCount();
    }

    m_fetchData = {};
}

bool AbstractAsyncSearchListModel::Private::fetchInProgress() const
{
    return m_fetchData.info.id != rest::Handle();
}

const AbstractAsyncSearchListModel::Private::FetchInformation&
    AbstractAsyncSearchListModel::Private::currentRequest() const
{
    return m_fetchData.info;
}

void AbstractAsyncSearchListModel::Private::completePrefetch(
    const QnTimePeriod& actuallyFetched, bool success, int fetchedCount)
{
    NX_ASSERT(m_fetchData.info.direction == q->fetchDirection());

    if (fetchedCount == 0)
    {
        NX_VERBOSE(q, "Pre-fetched no items");
    }
    else
    {
        NX_VERBOSE(q, "Pre-fetched %1 items:\n    from: %2\b    to: %3", fetchedCount,
            utils::timestampToDebugString(actuallyFetched.startTimeMs),
            utils::timestampToDebugString(actuallyFetched.endTimeMs()));
    }

    const bool fetchedAll = success && fetchedCount < (q->maximumCount() + m_fetchData.info.batchSize);
    const bool mayGoLive = fetchedAll && m_fetchData.info.direction == EventSearch::FetchDirection::later;
    const auto result = success
        ? (fetchedAll ? EventSearch::FetchResult::complete : EventSearch::FetchResult::incomplete)
        : EventSearch::FetchResult::failed;

    NX_VERBOSE(q, "Fetch result: %1", QVariant::fromValue(result).toString());

    if (NX_ASSERT(m_fetchData.handler))
        m_fetchData.handler(actuallyFetched, result);
    m_fetchData = {};

    // If top is reached, go to live mode.
    if (mayGoLive)
        q->setLive(q->effectiveLiveSupported());
}

} // namespace nx::vms::client::core
