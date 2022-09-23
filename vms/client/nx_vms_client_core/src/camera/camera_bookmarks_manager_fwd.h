// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <core/resource/resource_fwd.h>
#include <core/resource/camera_bookmark_fwd.h>
#include <nx/vms/client/core/event_search/utils/fetched_data.h>

class QnCameraBookmarksQuery;
class QnCameraBookmarksManager;
class QnCameraBookmarksManagerPrivate;

using OperationCallbackType = std::function<void (bool)>;
using BookmarksCallbackType = std::function<void (bool, int, const QnCameraBookmarkList&)>;
using BookmarkTagsCallbackType = std::function<void (bool, int, const QnCameraBookmarkTagList&)>;

using BookmarkFetchedData = nx::vms::client::core::FetchedData<QnCameraBookmarkList>;
using TailBookmarksCallbackType = std::function<void (
    bool success,
    int requestId,
    const BookmarkFetchedData& data)>;

typedef QnSharedResourcePointer<QnCameraBookmarksQuery> QnCameraBookmarksQueryPtr;
typedef QWeakPointer<QnCameraBookmarksQuery> QnCameraBookmarksQueryWeakPtr;
