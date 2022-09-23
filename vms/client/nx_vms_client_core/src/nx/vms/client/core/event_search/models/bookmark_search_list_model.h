// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <common/common_module_aware.h>
#include <nx/vms/client/core/event_search/models/abstract_async_search_list_model.h>

class QnCommonModule;

namespace nx::vms::client::core {

class BookmarkSearchListModel: public AbstractAsyncSearchListModel,
    public QnCommonModuleAware
{
    Q_OBJECT
    using base_type = AbstractAsyncSearchListModel;

    Q_PROPERTY(int referenceItemIndex
        READ referenceItemIndex
        NOTIFY referenceItemIndexChanged)

public:
    explicit BookmarkSearchListModel(
        QnCommonModule* commonModule,
        QObject* parent = nullptr);
    virtual ~BookmarkSearchListModel() override = default;

    virtual TextFilterSetup* textFilter() const override;

    // Clients do not receive any live updates from the server, therefore time periods of interest
    // must be periodically polled for updates by calling this method.
    void dynamicUpdate(const QnTimePeriod& period);

    virtual bool isConstrained() const override;
    virtual bool hasAccessRights() const override;

    int referenceItemIndex() const;

    virtual QHash<int, QByteArray> roleNames() const override;

protected:
    class Private;

    explicit BookmarkSearchListModel(
        QnCommonModule* commonModule,
        CreatePrivate creator,
        QObject* parent = nullptr);

signals:
    void referenceItemIndexChanged();

private:
    Private* const d;
};

} // namespace nx::vms::client::core
