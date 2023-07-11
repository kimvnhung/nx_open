// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "extended_camera_output_controller.h"

#include <QtQml/QtQml>

#include <api/model/api_ioport_data.h>
#include <api/server_rest_connection.h>
#include <core/resource/security_cam_resource.h>
#include <core/resource_management/resource_pool.h>
#include <nx/utils/guarded_callback.h>
#include <nx/utils/uuid.h>
#include <nx/vms/client/core/extended_camera_output/intercom_helper.h>
#include <nx/vms/event/action_parameters.h>
#include <nx/vms/api/data/event_rule_data.h>

#include <utils/common/delayed.h>

namespace nx::vms::api {

uint qHash(ExtendedCameraOutput value, uint seed)
{
    return ::qHash(static_cast<int>(value), seed);
}

} // namespace nx::vms::api

namespace nx::vms::client::core {

using ExtendedCameraOutput = nx::vms::api::ExtendedCameraOutput;
using ExtendedCameraOutputs = nx::vms::api::ExtendedCameraOutputs;

namespace {

struct OutputDescription
{
    QString iconPath;
    QString name;
};

using DescriptionMap = QMap<ExtendedCameraOutput, OutputDescription>;
using OutputsSet = QSet<ExtendedCameraOutput>;
using OutputNameToIdMap = QMap<QString, QString>;

QString iconPath(const QString& buttonName)
{
    return QStringLiteral("qrc:///images/soft_trigger/user_selectable/%1.png").arg(buttonName);
}

static const DescriptionMap supportedOutputsDescription = {
    {ExtendedCameraOutput::heater,
        {iconPath("heater"), ExtendedCameraOutputController::tr("Heater")}},
    {ExtendedCameraOutput::wiper,
        {iconPath("wiper"), ExtendedCameraOutputController::tr("Wiper")}},
    {ExtendedCameraOutput::powerRelay,
        {iconPath("_door_opened"), ExtendedCameraOutputController::tr("Open Door")}}};

} // namespace

struct ExtendedCameraOutputController::Private
{
    ExtendedCameraOutputController* const q;

    QnResourcePool* const resourcePool;
    QnUuid resourceId;
    QnSecurityCamResourcePtr camera;
    OutputsSet outputs;
    QMap<QString, QString> outputNameToId;
    ExtendedCameraOutput activeOutput = ExtendedCameraOutput::none;

    void reset();
    void updateOutputs();
};

void ExtendedCameraOutputController::Private::reset()
{
    resourceId = {};
    if (camera)
    {
        camera->disconnect(q);
        camera.reset();
    }
    updateOutputs();
}

void ExtendedCameraOutputController::Private::updateOutputs()
{
    const auto newOutputs =
        [this]()
        {
            OutputsSet result;
            if (!camera)
                return result;

            const auto cameraOutputs = camera->extendedOutputs();
            for (const auto& id: supportedOutputsDescription.keys())
            {
                if (cameraOutputs.testFlag(id))
                    result.insert(id);
            }
            return result;
        }();

    const auto addedIds = newOutputs - outputs;
    const auto removedIds = outputs - newOutputs;
    if (outputs == newOutputs)
        return;

    outputNameToId =
        [this]()
        {
            OutputNameToIdMap result;
            if (!camera)
                return result;

            for (const QnIOPortData& portData: camera->ioPortDescriptions())
                result[portData.outputName] = portData.id;

            return result;
        }();

    outputs = newOutputs;

    for (const auto& id: removedIds)
        emit q->outputRemoved(static_cast<int>(id));

    for (const auto& id: addedIds)
    {
        const auto& description = supportedOutputsDescription[id];
        emit q->outputAdded(static_cast<int>(id), description.iconPath, description.name);
    }
}

//-------------------------------------------------------------------------------------------------

void ExtendedCameraOutputController::registerQmlType()
{
    qmlRegisterType<ExtendedCameraOutputController>("nx.vms.client.core", 1, 0,
        "ExtendedCameraOutputController");
}

ExtendedCameraOutputController::ExtendedCameraOutputController(QObject* parent)
    :
    base_type(parent),
    d{new Private{this, resourcePool()}}
{
}

ExtendedCameraOutputController::~ExtendedCameraOutputController()
{
}

void ExtendedCameraOutputController::setResourceId(const QnUuid& id)
{
    if (d->resourceId == id)
        return;

    d->reset();

    if (id.isNull())
        return;

    d->resourceId = id;
    d->camera = d->resourcePool->getResourceById<QnSecurityCamResource>(id);
    if (!NX_ASSERT(d->camera))
        return;

    d->updateOutputs();
    connect(d->camera.get(), &QnResource::propertyChanged, this,
        [this](const QnResourcePtr& /*resource*/,
            const QString& key,
            const QString& /*prevValue*/,
            const QString& /*newValue*/)
        {
            if (key == ResourcePropertyKey::kIoSettings)
                d->updateOutputs();
        });
}

int ExtendedCameraOutputController::activeOutput() const
{
    return static_cast<int>(d->activeOutput);
}

bool ExtendedCameraOutputController::activateOutput(int outputId)
{
    if (!d->camera)
        return false;

    if (d->activeOutput != ExtendedCameraOutput::none)
    {
        NX_ASSERT(false, "Can't activate extended output while another one is active");
        return false;
    }

    const auto outputType = static_cast<ExtendedCameraOutput>(outputId);
    if (!d->outputs.contains(outputType))
        return false;

    nx::vms::event::ActionParameters actionParameters;

    /**
     * In 5.0 opend door software trigger has 6 seconds duration and it should be managed by the
     * client. In 5.1 duration is managed (replaced) by server, and it is safe to send any value.
     */
    actionParameters.durationMs = outputType == ExtendedCameraOutput::powerRelay
        ? IntercomHelper::kOpenedDoorDuration.count()
        : 0;

    actionParameters.relayOutputId =
        [this, outputType]()
        {
            const auto it = d->outputNameToId.find(
                QString::fromStdString(nx::reflect::toString(outputType)));
            return it == d->outputNameToId.end()
                ? QString{}
                : it.value();
        }();

    if (actionParameters.relayOutputId.isEmpty())
        return false;

    d->activeOutput = outputType;

    nx::vms::api::EventActionData actionData;
    actionData.actionType = nx::vms::api::ActionType::cameraOutputAction;
    actionData.toggleState =  nx::vms::api::EventState::active;
    actionData.resourceIds.push_back(d->resourceId);
    actionData.params = QJson::serialized(actionParameters);

    auto callback = nx::utils::guarded(this,
        [this](
            bool success,
            rest::Handle /*requestId*/,
            nx::network::rest::JsonResult result)
        {
            outputActivated(static_cast<int>(d->activeOutput),
                success && result.error == nx::network::rest::Result::NoError);
            d->activeOutput = ExtendedCameraOutput::none;
        });

    if (auto connection = connectedServerApi())
        connection->executeEventAction(actionData, callback, thread());

    return true;
}

} // namespace nx::vms::client::core
