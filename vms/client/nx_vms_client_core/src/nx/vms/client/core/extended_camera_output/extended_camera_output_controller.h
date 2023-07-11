// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <QtCore/QObject>

#include <nx/utils/impl_ptr.h>
#include <nx/vms/client/core/common/utils/common_module_aware.h>
#include <nx/vms/client/core/network/remote_connection_aware.h>

class QnUuid;

namespace nx::vms::client::core {

class ExtendedCameraOutputController:
    public QObject,
    public CommonModuleAware,
    public RemoteConnectionAware
{
    Q_OBJECT
    using base_type = QObject;

public:
    static void registerQmlType();

    ExtendedCameraOutputController(QObject* parent = nullptr);

    virtual ~ExtendedCameraOutputController() override;

    void setResourceId(const QnUuid& id);

    Q_INVOKABLE int activeOutput() const;
    Q_INVOKABLE bool activateOutput(int  outputId);

signals:
    void outputAdded(int id,
        const QString& iconPath,
        const QString& name);

    void outputRemoved(int id);

    void outputActivated(int outputId, bool success);

private:
    struct Private;
    nx::utils::ImplPtr<Private> d;
};

} // namespace nx::vms::client::core
