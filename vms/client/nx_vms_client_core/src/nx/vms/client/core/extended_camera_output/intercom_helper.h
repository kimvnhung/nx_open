// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <chrono>

#include <nx/utils/uuid.h>

namespace nx::vms::client::core {

struct IntercomHelper
{
    static const std::chrono::milliseconds kOpenedDoorDuration;

    static QnUuid intercomOpenDoorRuleId(const QnUuid& cameraId);
};

} // namespace nx::vms::client::core
