// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <QtCore/QHash>
#include <QtCore/qnamespace.h>

#include <string_view>

namespace nx::vms::client::core {

enum CoreItemDataRole
{
    FirstCoreItemDataRole = Qt::UserRole,

    TimestampRole,                  /**< Role for timestamp in microseconds since epoch (std::chrono::microseconds). */
    ResourceNameRole,               /**< Role for resource name. Value of type QString. */
    ResourceRole,                   /**< Role for QnResourcePtr. */
    RawResourceRole,                /**< Role for QnResource*. */
    ResourceListRole,               /**< Resource list (QnResourceList). */
    LayoutResourceRole,             /**< Role for QnLayoutResourcePtr. */
    MediaServerResourceRole,        /**< Role for QnMediaServerResourcePtr. */
    ResourceStatusRole,             /**< Role for resource status. Value of type int (nx::vms::api::ResourceStatus). */

    DescriptionTextRole,            /**< Role for generic description text (QString). */
    TimestampTextRole,              /**< Role for timestamp text (QString). */
    DisplayedResourceListRole,      /**< Resource list displayed in a Right Panel tile (QnResourceList or QStringList). */
    PreviewTimeRole,                /**< Role for camera preview time in microseconds since epoch (std::chrono::microseconds). */
    TimestampMsRole,                /**< Role for some timestamp, in milliseconds since epoch (std::chrono::milliseconds). */
    UuidRole,                       /**< Role for target uuid. Used in LoadVideowallMatrixAction. */
    DurationRole,                   /**< Role for duration in microseconds (std::chrono::microseconds). */
    DurationMsRole,                   /**< Role for duration in microseconds (std::chrono::microseconds). */

    CameraBookmarkRole,             /**< Role for the selected camera bookmark (if any). Used in Edit/RemoveBookmarkAction */
    BookmarkTagRole,                /**< Role for bookmark tag. Used in OpenBookmarksSearchAction */

    /**
     * Model notification roles. Do not necessarily pass any data but implement item-related
     * view-to-model notifications via setData which can be proxied.
     */
    DefaultNotificationRole,        /**< Role to perform default item action (no data). */
    ActivateLinkRole,               /**< Role to parse and follow hyperlink (QString). */

    CoreItemDataRoleCount
};

//** Returns role id to text name mapping for the some roles. */
QHash<int, QByteArray> clientCoreRoleNames();

std::string toString(CoreItemDataRole value);
bool fromString(const std::string_view& str, CoreItemDataRole* value);

} // nx::vms::client::core
