// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <QtCore/QObject>

#include <core/resource/resource_fwd.h>
#include <nx/utils/impl_ptr.h>
#include <nx/vms/client/core/common/utils/common_module_aware.h>
#include <utils/common/connective.h>

class QnUuid;
namespace nx::vms::license { class SingleCamLicenseStatusHelper; }

namespace nx::vms::client::core {

/**
 * Watches on the specified resource for the two way audio transmission availability. Takes into
 * the consideration mapping of the source camera to it's audio output device.
 */
class TwoWayAudioAvailabilityWatcher: public Connective<QObject>, public CommonModuleAware
{
    Q_OBJECT
    using base_type = Connective<QObject>;

    Q_PROPERTY(bool available READ available NOTIFY availabilityChanged)
    Q_PROPERTY(QnUuid resourceId READ resourceId WRITE setResourceId NOTIFY resourceIdChanged)

public:
    TwoWayAudioAvailabilityWatcher(QObject* parent = nullptr);

    virtual ~TwoWayAudioAvailabilityWatcher() override;

    bool available() const;

    QnUuid resourceId() const;
    void setResourceId(const QnUuid& id);

    QnVirtualCameraResourcePtr targetResource();

signals:
    void availabilityChanged();
    void resourceIdChanged();
    void targetResourceChanged();

private:
    struct Private;
    nx::utils::ImplPtr<Private> d;
};

} // namespace nx::vms::client::core
