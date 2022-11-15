// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "two_way_audio_availability_watcher.h"

#include <common/common_module.h>
#include <core/resource/camera_resource.h>
#include <core/resource_management/resource_pool.h>
#include <core/resource_access/global_permissions_manager.h>
#include <client_core/client_core_module.h>
#include <nx/vms/license/usage_helper.h>
#include <nx/vms/client/core/watchers/user_watcher.h>
#include <core/resource_access/resource_access_subject.h>

namespace nx::vms::client::core {

struct TwoWayAudioAvailabilityWatcher::Private: public Connective<QObject>
{
    Private(TwoWayAudioAvailabilityWatcher* q);
    void updateTargetDevice();
    void updateAvailability();
    void setAvailable(bool value);

    TwoWayAudioAvailabilityWatcher* const q;
    bool available = false;
    QnVirtualCameraResourcePtr sourceCamera;
    QnVirtualCameraResourcePtr targetCamera;
    QScopedPointer<nx::vms::license::SingleCamLicenseStatusHelper> helper;
};

TwoWayAudioAvailabilityWatcher::Private::Private(TwoWayAudioAvailabilityWatcher* q):
    q(q)
{
}

void TwoWayAudioAvailabilityWatcher::Private::updateTargetDevice()
{
    using namespace nx::vms::license;

    const auto camera =
        [this]() -> QnVirtualCameraResourcePtr
        {
            if (!sourceCamera)
                return {};

            const auto id = sourceCamera->audioOutputDeviceId();
            const auto result = q->resourcePool()->getResourceById<QnVirtualCameraResource>(id);
            return result && result->hasTwoWayAudio()
                ? result
                : sourceCamera;
        }();

    if (camera == targetCamera)
        return;

    if (targetCamera)
        targetCamera->disconnect(this);

    targetCamera = camera;
    helper.reset(targetCamera && targetCamera->isIOModule()
        ? new SingleCamLicenseStatusHelper(targetCamera)
        : nullptr);

    if (targetCamera)
    {
        const auto update = [this]() { updateAvailability(); };
        connect(targetCamera, &QnVirtualCameraResource::statusChanged, this, update);
        connect(targetCamera, &QnSecurityCamResource::twoWayAudioEnabledChanged, this, update);
        connect(targetCamera, &QnSecurityCamResource::audioOutputDeviceIdChanged, this, update);

        if (helper)
            connect(helper, &SingleCamLicenseStatusHelper::licenseStatusChanged, this, update);
    }

    updateAvailability();
    emit q->targetResourceChanged();
}

void TwoWayAudioAvailabilityWatcher::Private::updateAvailability()
{
    const bool isAvailable =
        [this]()
    {
        if (!targetCamera)
            return false;

        if (!targetCamera->isTwoWayAudioEnabled() || !targetCamera->hasTwoWayAudio())
            return false;

        const auto user = q->commonModule()->instance<UserWatcher>()->user();
        if (!user)
            return false;

        const auto manager = q->globalPermissionsManager();
        if (!manager->hasGlobalPermission(user, GlobalPermission::userInput))
            return false;

        if (!targetCamera->isOnline())
            return false;

        if (helper)
            return helper->status() == nx::vms::license::UsageStatus::used;

        return true;
    }();

    setAvailable(isAvailable);
}

void TwoWayAudioAvailabilityWatcher::Private::setAvailable(bool value)
{
    if (available == value)
        return;

    available = value;
    emit q->availabilityChanged();
}

//-------------------------------------------------------------------------------------------------

TwoWayAudioAvailabilityWatcher::TwoWayAudioAvailabilityWatcher(QObject* parent):
    base_type(parent),
    d(new Private(this))
{
    const auto manager = globalPermissionsManager();
    const auto userWatcher = commonModule()->instance<UserWatcher>();

    connect(userWatcher, &UserWatcher::userChanged,
        d.get(), &TwoWayAudioAvailabilityWatcher::Private::updateAvailability);
    connect(manager, &QnGlobalPermissionsManager::globalPermissionsChanged, this,
        [this, userWatcher]
            (const QnResourceAccessSubject& subject, GlobalPermissions /*permissions*/)
        {
            const auto user = userWatcher->user();
            if (subject != user)
                return;

            d->updateAvailability();
        });
}

TwoWayAudioAvailabilityWatcher::~TwoWayAudioAvailabilityWatcher()
{
}

bool TwoWayAudioAvailabilityWatcher::available() const
{
    return d->available;
}

QnUuid TwoWayAudioAvailabilityWatcher::resourceId() const
{
    return d->sourceCamera ? d->sourceCamera->getId() : QnUuid();
}

void TwoWayAudioAvailabilityWatcher::setResourceId(const QnUuid& id)
{
    if (d->sourceCamera && d->sourceCamera->getId() == id)
        return;

    if (d->sourceCamera)
        d->sourceCamera->disconnect(this);

    const auto camera = resourcePool()->getResourceById<QnVirtualCameraResource>(id);
    d->sourceCamera = camera && camera->hasTwoWayAudio() ? camera : QnVirtualCameraResourcePtr();

    if (d->sourceCamera)
    {
        connect(d->sourceCamera.get(), &QnVirtualCameraResource::audioOutputDeviceIdChanged,
            d.get(), &Private::updateTargetDevice);
    }

    d->updateTargetDevice();
    emit resourceIdChanged();
}

QnVirtualCameraResourcePtr TwoWayAudioAvailabilityWatcher::targetResource()
{
    return d->targetCamera;
}

} // namespace nx::vms::client::core

