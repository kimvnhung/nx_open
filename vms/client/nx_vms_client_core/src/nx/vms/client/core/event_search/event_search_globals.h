// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <QtCore/QMetaType>

namespace nx::vms::client::core {
namespace EventSearch {

Q_NAMESPACE
Q_CLASSINFO("RegisterEnumClassesUnscoped", "false")

enum class FetchDirection
{
    earlier,
    later
};
Q_ENUM_NS(FetchDirection)

enum class FetchResult
{
    complete, //< Successful. There's no more data to fetch.
    incomplete, //< Successful. There's more data to fetch.
    failed, //< Unsuccessful.
    cancelled //< Cancelled.
};
Q_ENUM_NS(FetchResult)

enum class CameraSelection
{
    all,
    layout,
    current,
    custom
};
Q_ENUM_NS(CameraSelection)

enum class TimeSelection
{
    anytime,
    day,
    week,
    month,
    selection
};
Q_ENUM_NS(TimeSelection)

enum class PreviewState
{
    initial,
    busy,
    ready,
    missing
};
Q_ENUM_NS(PreviewState)

void registerQmlType();

inline uint qHash(EventSearch::TimeSelection source)
{
    return uint(source);
}

inline uint qHash(EventSearch::CameraSelection source)
{
    return uint(source);
}

} // namespace EventSearch
} // namespace nx::vms::client::core
