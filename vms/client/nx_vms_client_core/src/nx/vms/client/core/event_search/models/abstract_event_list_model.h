// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <chrono>

#include <QtCore/QAbstractListModel>
#include <QtCore/QPointer>

#include <nx/utils/scoped_model_operations.h>

namespace nx::vms::client::core {

class ServerTimeWatcher;

/**
 * Base model for all Event Search data models. Provides action activation via setData.
 */
class AbstractEventListModel:
    public ScopedModelOperations<QAbstractListModel>
{
    Q_OBJECT
    using base_type = ScopedModelOperations<QAbstractListModel>;

public:
    explicit AbstractEventListModel(
        ServerTimeWatcher* serverTimeWatcher,
        QObject* parent = nullptr);

    virtual ~AbstractEventListModel() override = default;

    virtual QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    virtual bool setData(const QModelIndex& index, const QVariant& value, int role) override;

    Q_INVOKABLE QString timestampText(qint64 timestampMs) const;

protected:
    bool isValid(const QModelIndex& index) const;
    virtual QString timestampText(std::chrono::microseconds timestamp) const;

    virtual bool defaultAction(const QModelIndex& index);
    virtual bool activateLink(const QModelIndex& index, const QString& link);

private:
    QPointer<ServerTimeWatcher> m_serverTimeWatcher;
};

} // namespace nx::vms::client::core
