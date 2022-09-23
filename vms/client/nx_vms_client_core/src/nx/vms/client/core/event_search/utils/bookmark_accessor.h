// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <chrono>
#include <core/resource/camera_bookmark.h>

namespace nx::vms::client::core {

/** Bookmark's fileds accessor. Used in the generic event search algorithms. */
struct BookmarkAccessor
{
    using ItemType = QnCameraBookmark;

    static std::chrono::milliseconds timestamp(const QnCameraBookmark& bookmark)
    {
        return bookmark.startTimeMs;
    }

    static QnUuid guid(const QnCameraBookmark& bookmark)
    {
        return bookmark.guid;
    }

    static QString toString(const QnCameraBookmark& bookmark)
    {
        return QString("[%1]: %2").arg(bookmark.guid.toString(), bookmark.name);
    }
};

} // namespace nx::vms::client::core
