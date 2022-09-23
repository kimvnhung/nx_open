// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "abstract_search_list_model.h"

#include <QtQml/QtQml>

#include <chrono>

#include <client/client_message_processor.h>
#include <client_core/client_core_module.h>
#include <common/common_meta_types.h>
#include <common/common_module.h>
#include <core/resource/camera_resource.h>
#include <core/resource/user_resource.h>
#include <core/resource_access/resource_access_subject.h>
#include <core/resource_access/resource_access_manager.h>
#include <utils/common/delayed.h>

#include <nx/vms/client/core/client_core_globals.h>
#include <nx/vms/client/core/event_search/models/abstract_search_list_model.h>
#include <nx/vms/client/core/utils/managed_camera_set.h>
#include <nx/vms/client/core/watchers/server_time_watcher.h>
#include <nx/vms/client/core/watchers/user_watcher.h>
#include <nx/utils/guarded_callback.h>
#include <nx/utils/datetime.h>
#include <nx/utils/log/log.h>
#include <nx/utils/model_row_iterator.h>

namespace nx::vms::client::core {

using namespace std::chrono;

void AbstractSearchListModel::registerQmlType()
{
    qmlRegisterUncreatableType<AbstractSearchListModel>("nx.vms.client.core", 1, 0,
        "AbstractSearchListModel", "Cannot create instance of AbstractSearchListModel");
}

AbstractSearchListModel::AbstractSearchListModel(
    QnCommonModule* commonModule,
    QObject* parent)
    :
    base_type(commonModule->instance<ServerTimeWatcher>(), parent),
    m_userWatcher(commonModule->instance<UserWatcher>()),
    m_cameraSet(new ManagedCameraSet(commonModule->resourcePool(),
        [this](const QnVirtualCameraResourcePtr& camera)
        {
            return isCameraApplicable(camera);
        }))
{
    connect(m_userWatcher, &UserWatcher::userChanged, this,
        nx::utils::guarded(m_userWatcher, [this](const QnUserResourcePtr& user)
        {
            onOnlineChanged(/*isOnline*/ !user.isNull());
        }));

    onOnlineChanged(!m_userWatcher->user().isNull());

    connect(m_cameraSet.data(), &ManagedCameraSet::camerasAboutToBeChanged, this,
        [this]()
        {
            emit camerasAboutToBeChanged({});
            clear();
        });

    connect(m_cameraSet.data(), &ManagedCameraSet::camerasChanged, this,
        [this]()
        {
            NX_ASSERT(isOnline() || m_cameraSet->cameras().isEmpty());
            emit camerasChanged({});
        });
}

AbstractSearchListModel::~AbstractSearchListModel()
{
    // Required here for forward-declared scoped pointer destruction.
}

void AbstractSearchListModel::onOnlineChanged(bool isOnline)
{
    if (m_isOnline == isOnline)
        return;

    m_isOnline = isOnline;

    if (m_isOnline)
    {
        emit dataNeeded();
    }
    else
    {
        m_cameraSet->setMultipleCameras({});
        clear();
    }

    emit isOnlineChanged(m_isOnline, {});
}

bool AbstractSearchListModel::canFetchData() const
{
    if ((!m_offlineAllowed && !m_isOnline)
        || (isLive() && fetchDirection() == EventSearch::FetchDirection::later)
        || m_relevantTimePeriod.isEmpty()
        || !canFetchNow()
        || !hasAccessRights())
    {
        return false;
    }

    if (m_fetchedTimeWindow.isEmpty())
        return true;

    if (fetchDirection() == EventSearch::FetchDirection::earlier)
        return m_fetchedTimeWindow.startTimeMs > m_relevantTimePeriod.startTimeMs;

    NX_ASSERT(fetchDirection() == EventSearch::FetchDirection::later);
    return m_fetchedTimeWindow.endTimeMs() < m_relevantTimePeriod.endTimeMs();
}

void AbstractSearchListModel::fetchData()
{
    if (!canFetchData())
        return;

    if (isFilterDegenerate())
    {
        NX_ASSERT(rowCount() == 0);
        ScopedFetchCommit scopedFetch(this, fetchDirection(), EventSearch::FetchResult::complete);
        setFetchedTimeWindow(m_relevantTimePeriod); ///sss2
    }
    else
    {
        requestFetch();
    }
}

bool AbstractSearchListModel::canFetchNow() const
{
    return true;
}

bool AbstractSearchListModel::isFilterDegenerate() const
{
    return m_relevantTimePeriod.isEmpty()
        || (cameraSet()->type() != ManagedCameraSet::Type::all && cameras().empty());
}

ManagedCameraSet* AbstractSearchListModel::cameraSet() const
{
    return m_cameraSet.data();
}

bool AbstractSearchListModel::isOnline() const
{
    return m_isOnline;
}

QnVirtualCameraResourceSet AbstractSearchListModel::cameras() const
{
    return m_cameraSet->cameras();
}

bool AbstractSearchListModel::fetchInProgress() const
{
    return false;
}

bool AbstractSearchListModel::cancelFetch()
{
    return false;
}

bool AbstractSearchListModel::isConstrained() const
{
    return m_relevantTimePeriod != QnTimePeriod::anytime();
}

bool AbstractSearchListModel::hasAccessRights() const
{
    return true;
}

TextFilterSetup* AbstractSearchListModel::textFilter() const
{
    return nullptr;
}

std::pair<int, int> AbstractSearchListModel::find(milliseconds timestamp) const
{
    if (!fetchedTimeWindow().contains(timestamp))
        return {};

    const auto range = std::equal_range(
        nx::utils::ModelRowIterator::cbegin(TimestampRole, this),
        nx::utils::ModelRowIterator::cend(TimestampRole, this),
        QVariant::fromValue(microseconds(timestamp)),
        [](const QVariant& left, const QVariant& right)
        {
            return right.value<microseconds>() < left.value<microseconds>();
        });

    return {range.first.row(), range.second.row()};
}

const QnTimePeriod& AbstractSearchListModel::relevantTimePeriod() const
{
    return m_relevantTimePeriod;
}

void AbstractSearchListModel::setRelevantTimePeriod(const QnTimePeriod& value)
{
    if (value == m_relevantTimePeriod)
        return;

    NX_VERBOSE(this, "Relevant time period changed.\n"
        "    Old was from: %1\n        to: %2\n    New is from: %3\n        to: %4",
        nx::utils::timestampToDebugString(m_relevantTimePeriod.startTimeMs),
        nx::utils::timestampToDebugString(m_relevantTimePeriod.endTimeMs()),
        nx::utils::timestampToDebugString(value.startTimeMs),
        nx::utils::timestampToDebugString(value.endTimeMs()));

    m_relevantTimePeriod = value;

    if (!m_relevantTimePeriod.isValid() || m_fetchedTimeWindow != m_relevantTimePeriod)
    {
        clear();
    }
    else
    {
        cancelFetch();
        truncateToRelevantTimePeriod();
        setFetchedTimeWindow(m_relevantTimePeriod); ///sss2

        if (!m_relevantTimePeriod.isInfinite())
            setLive(false);

        // TODO: #vkutin Investigate the scenario.
        // Default canFetchData implementation compares fetched time window with relevant time
        // period. But as we assigned one to another a few lines above, it never works.
        if (canFetchData())
            emit dataNeeded();
    }
}

EventSearch::FetchDirection AbstractSearchListModel::fetchDirection() const
{
    return m_fetchDirection;
}

void AbstractSearchListModel::setFetchDirection(EventSearch::FetchDirection value)
{
    if (m_fetchDirection == value)
        return;

    NX_VERBOSE(this, "Set fetch direction to: %1", QVariant::fromValue(value).toString());

    cancelFetch();
    m_fetchDirection = value;
}

int AbstractSearchListModel::maximumCount() const
{
    return m_maximumCount;
}

void AbstractSearchListModel::setMaximumCount(int value)
{
    if (m_maximumCount == value)
        return;

    NX_VERBOSE(this, "Set maximum count to %1", value);

    m_maximumCount = value;

    const bool needToTruncate = m_maximumCount < rowCount();
    if (needToTruncate)
    {
        NX_VERBOSE(this, "Truncating to maximum count");
        truncateToMaximumCount();
    }
}

int AbstractSearchListModel::fetchBatchSize() const
{
    return std::min(m_fetchBatchSize, m_maximumCount);
}

void AbstractSearchListModel::setFetchBatchSize(int value)
{
    NX_VERBOSE(this, "Set fetch batch size to %1", value);
    m_fetchBatchSize = value;
}

bool AbstractSearchListModel::liveSupported() const
{
    return m_liveSupported;
}

bool AbstractSearchListModel::effectiveLiveSupported() const
{
    return liveSupported() && m_relevantTimePeriod.isInfinite();
}

void AbstractSearchListModel::setLiveSupported(bool value)
{
    if (m_liveSupported == value)
        return;

    m_liveSupported = value;
    setLive(m_live && m_liveSupported);

    clear();
}

bool AbstractSearchListModel::isLive() const
{
    return m_liveSupported && m_live;
}

void AbstractSearchListModel::setLive(bool value)
{
    if (value == m_live)
        return;

    if (value && !(NX_ASSERT(liveSupported()) && effectiveLiveSupported()))
        return;

    NX_VERBOSE(this, "Setting live mode to %1", value);
    m_live = value;

    emit liveChanged(m_live, {});
}

bool AbstractSearchListModel::livePaused() const
{
    return m_livePaused;
}

void AbstractSearchListModel::setLivePaused(bool value)
{
    if (m_livePaused == value)
        return;

    NX_VERBOSE(this, "Setting live %1", (value ? "paused" : "unpaused"));
    m_livePaused = value;

    emit livePausedChanged(m_livePaused, {});
}

bool AbstractSearchListModel::offlineAllowed() const
{
    return m_offlineAllowed;
}

void AbstractSearchListModel::setOfflineAllowed(bool value)
{
    if (m_offlineAllowed == value)
        return;

    NX_VERBOSE(this, "Setting offline allowed to %1", value);
    m_offlineAllowed = value;
}

QnTimePeriod AbstractSearchListModel::fetchedTimeWindow() const
{
    return m_fetchedTimeWindow;
}

bool AbstractSearchListModel::isCameraApplicable(const QnVirtualCameraResourcePtr& camera) const
{
    if (!camera || camera->flags().testFlag(Qn::desktop_camera))
        return false;

    const auto user = m_userWatcher->user();
    if (!user)
        return false;

    // TODO: #ynikitenkov Think about QnWorkbencAccessController, LC_VMAX part
    return camera->systemContext()->resourceAccessManager()->hasPermission(
        user, camera, Qn::ReadPermission | Qn::ViewContentPermission);
}

void AbstractSearchListModel::setFetchedTimeWindow(const QnTimePeriod& value)
{
    NX_VERBOSE(this, "Set fetched time window:\n    from: %1\n    to: %2",
        nx::utils::timestampToDebugString(value.startTimeMs),
        nx::utils::timestampToDebugString(value.endTimeMs()));
    m_fetchedTimeWindow = value;
}

void AbstractSearchListModel::addToFetchedTimeWindow(const QnTimePeriod& period)
{
    auto timeWindow = m_fetchedTimeWindow;
    timeWindow.addPeriod(period);
    setFetchedTimeWindow(timeWindow);
}

void AbstractSearchListModel::clear()
{
    NX_VERBOSE(this, "Clear model");

    setFetchDirection(EventSearch::FetchDirection::earlier);
    setFetchedTimeWindow({});
    cancelFetch();
    clearData();

    setLive(effectiveLiveSupported());

    const auto emitDataNeeded =
        [this]()
        {
            if (canFetchData())
            {
                NX_VERBOSE(this, "Emitting dataNeeded() after a clear");
                emit dataNeeded();
            }
        };

    executeLater(emitDataNeeded, this);
}

} // namespace nx::vms::client::core
