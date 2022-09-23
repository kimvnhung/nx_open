// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <nx/utils/impl_ptr.h>
#include <nx/vms/client/core/event_search/utils/common_object_search_setup.h>

class QnWorkbenchContext;

namespace nx::vms::client::desktop {

class CommonObjectSearchSetup: public core::CommonObjectSearchSetup
{
    Q_OBJECT
    using base_type = core::CommonObjectSearchSetup;

public:
    explicit CommonObjectSearchSetup(QObject* parent = nullptr);

    virtual ~CommonObjectSearchSetup() override;

    QnWorkbenchContext* context() const;

    void setContext(QnWorkbenchContext* value);

    virtual bool selectCameras(QnUuidSet& selectedCameras) override;
    virtual QnVirtualCameraResourcePtr currentResource() const override;
    virtual QnVirtualCameraResourceSet currentLayoutCameras() const override;
    virtual void clearTimelineSelection() const override;

signals:
    void contextChanged();

private:
    struct Private;
    nx::utils::ImplPtr<Private> d;
};

} // namespace nx::vms::client::desktop
