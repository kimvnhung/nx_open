// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "event_search_globals.h"

#include <QtQml/QtQml>

#include <nx/vms/client/core/client_core_globals.h>

namespace nx::vms::client::core {
namespace EventSearch {

void registerQmlType()
{
    qmlRegisterUncreatableMetaObject(staticMetaObject, "nx.vms.client.core", 1, 0,
        "EventSearch", "EventSearch is a namespace");

    qRegisterMetaType<core::EventSearch::FetchDirection>();
    qRegisterMetaType<core::EventSearch::FetchResult>();
    qRegisterMetaType<core::EventSearch::PreviewState>();
}

} // namespace EventSearch
} // namespace nx::vms::client::desktop
