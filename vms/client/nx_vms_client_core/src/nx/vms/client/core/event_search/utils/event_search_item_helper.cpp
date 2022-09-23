// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "event_search_item_helper.h"

namespace nx::vms::client::core {

std::chrono::milliseconds getTailPointTime(
    const QnTimePeriod& period,
    EventSearch::FetchDirection direction)
{
    return direction == EventSearch::FetchDirection::later
        ? period.endTime()
        : period.startTime();
}

} // namespace nx::vms::client::core

