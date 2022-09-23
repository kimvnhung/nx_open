// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

import QtQuick 2.6
import QtMultimedia 5.0

import Nx.Core 1.0

import nx.vms.client.core 1.0

MultiVideoLayout
{
    id: layout

    property MediaPlayer mediaPlayer: null
    property Component channelOverlay: null

    // Implementation.

    property size __sourceSize

    implicitWidth: __sourceSize.width * layoutSize.width
    implicitHeight: __sourceSize.height * layoutSize.height

    function pointInVideo(position)
    {
        for (var i = 0; i != layout.repeater.count; ++i)
        {
            var child = layout.repeater.itemAt(i);
            var mapped = mapToItem(child, position.x, position.y)
            if (mapped.x < 0 || mapped.y < 0 || mapped.x > child.width || mapped.y > child.height)
                continue;

            return true
        }
        return false
    }

    delegate: VideoOutput
    {
        id: videoOutput
        fillMode: VideoOutput.Stretch

        readonly property int index: model.index

        source: MediaPlayerChannelAdapter
        {
            mediaPlayer: layout.mediaPlayer
            channel: index
        }

        onSourceRectChanged:
        {
            if (index === 0 && __sourceSize.width <= 0.0)
                __sourceSize = Qt.size(sourceRect.width, sourceRect.height)
        }

        Loader
        {
            anchors.fill: parent
            sourceComponent: channelOverlay

            onLoaded:
            {
                if (item.hasOwnProperty("index"))
                    item.index = index
            }
        }
    }
}
