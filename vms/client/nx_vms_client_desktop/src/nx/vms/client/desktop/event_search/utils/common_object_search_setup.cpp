// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "common_object_search_setup.h"

#include <core/resource/camera_resource.h>
#include <nx/utils/scoped_connections.h>
#include <nx/vms/client/desktop/resource_dialogs/camera_selection_dialog.h>
#include <ui/workbench/workbench.h>
#include <ui/workbench/workbench_context.h>
#include <ui/workbench/workbench_item.h>
#include <ui/workbench/workbench_layout.h>
#include <ui/workbench/workbench_navigator.h>

namespace nx::vms::client::desktop {

struct CommonObjectSearchSetup::Private
{
    QnWorkbenchContext* context = nullptr;
    nx::utils::ScopedConnections contextConnections;
};

CommonObjectSearchSetup::CommonObjectSearchSetup(QObject* parent):
    base_type(parent),
    d(new Private())
{
}

CommonObjectSearchSetup::~CommonObjectSearchSetup()
{
}

QnWorkbenchContext* CommonObjectSearchSetup::context() const
{
    return d->context;
}

void CommonObjectSearchSetup::setContext(QnWorkbenchContext* value)
{
    if (d->context == value)
        return;

    d->contextConnections.reset();
    d->context = value;
    setCommonModule(d->context ? d->context->commonModule() : nullptr);
    emit contextChanged();

    if (!d->context)
        return;

    const auto navigator = d->context->navigator();
    d->contextConnections << connect(
        navigator,
        &QnWorkbenchNavigator::timeSelectionChanged,
        this,
        &CommonObjectSearchSetup::handleTimelineSelectionChanged);

    const auto camerasUpdaterFor =
        [this](core::EventSearch::CameraSelection mode)
        {
            return [this, mode]() { updateRelevantCamerasForMode(mode); };
        };

    d->contextConnections
        << connect(d->context->navigator(), &QnWorkbenchNavigator::currentResourceChanged,
               this, camerasUpdaterFor(core::EventSearch::CameraSelection::current));

    d->contextConnections <<
        connect(d->context->workbench(), &QnWorkbench::currentLayoutChanged,
            this, camerasUpdaterFor(core::EventSearch::CameraSelection::layout));

    d->contextConnections <<
        connect(d->context->workbench(), &QnWorkbench::currentLayoutItemsChanged,
            this, camerasUpdaterFor(core::EventSearch::CameraSelection::layout));
}

bool CommonObjectSearchSetup::selectCameras(QnUuidSet& selectedCameras)
{
    return d->context && CameraSelectionDialog::selectCameras<CameraSelectionDialog::DummyPolicy>(
        selectedCameras, d->context->mainWindowWidget());
}

QnVirtualCameraResourcePtr CommonObjectSearchSetup::currentResource() const
{
    if (!d->context)
        return {};

    return d->context->navigator()->currentResource().dynamicCast<QnVirtualCameraResource>();
}

QnVirtualCameraResourceSet CommonObjectSearchSetup::currentLayoutCameras() const
{
    if (!d->context)
        return {};

    QnVirtualCameraResourceSet cameras;
    if (auto workbenchLayout = d->context->workbench()->currentLayout())
    {
        for (const auto& item: workbenchLayout->items())
        {
            if (const auto camera = item->resource().dynamicCast<QnVirtualCameraResource>())
                cameras.insert(camera);
        }
    }
    return cameras;
}

void CommonObjectSearchSetup::clearTimelineSelection() const
{
    if (d->context)
        d->context->navigator()->clearTimelineSelection();
}

} // namespace nx::vms::client::desktop
