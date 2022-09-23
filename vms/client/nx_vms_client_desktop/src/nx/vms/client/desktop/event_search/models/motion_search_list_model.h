// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <QtCore/QList>
#include <QtGui/QRegion>

#include <nx/vms/client/core/event_search/models/abstract_async_search_list_model.h>

#include <ui/workbench/workbench_context_aware.h>

class QnWorkbenchContext;

namespace nx::vms::client::desktop {

class MotionSearchListModel: public core::AbstractAsyncSearchListModel,
    public QnWorkbenchContextAware
{
    Q_OBJECT
    using base_type = core::AbstractAsyncSearchListModel;

public:
    explicit MotionSearchListModel(
        QnWorkbenchContext* context,
        QObject* parent = nullptr);
    virtual ~MotionSearchListModel() override = default;

    QList<QRegion> filterRegions() const; //< One region per channel.
    void setFilterRegions(const QList<QRegion>& value);

    bool isFilterEmpty() const;

    virtual bool isConstrained() const override;

protected:
    virtual bool isCameraApplicable(const QnVirtualCameraResourcePtr& camera) const override;

private:
    class Private;
    Private* const d;
};

} // namespace nx::vms::client::desktop
