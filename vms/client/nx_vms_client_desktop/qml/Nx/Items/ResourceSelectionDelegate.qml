// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

import QtQuick 2.15

import Nx 1.0
import Nx.Controls 1.0

import nx.vms.client.core 1.0
import nx.vms.client.desktop 1.0

Item
{
    id: delegateItem

    required property int selectionMode
    required property bool showExtraInfo
    required property bool showResourceStatus

    property bool customSelectionIndicator: false
    property bool wholeRowToggleable: true //< Clicks on any place of a row toggle selection.

    property var nextCheckState: //< Not used in exclusive selection mode.
        (checkState => checkState === Qt.Checked ? Qt.Unchecked : Qt.Checked)

    readonly property var resource: (model && model.resource) || null
    readonly property int nodeType: (model && model.nodeType) || -1

    readonly property bool isSeparator: nodeType == ResourceTree.NodeType.separator
        || nodeType == ResourceTree.NodeType.localSeparator

    readonly property real availableWidth: width - indicator.width

    implicitHeight: 20 //< Just a sensible default.

    implicitWidth: isSeparator
        ? 0
        : (contentRow.implicitWidth + indicator.width + contentRow.spacing)

    Row
    {
        id: contentRow

        height: delegateItem.height
        spacing: 4

        Image
        {
            id: icon

            anchors.verticalCenter: contentRow.verticalCenter

            readonly property int imageState: delegateItem.highlighted
                ? ResourceTree.ItemState.selected
                : ResourceTree.ItemState.normal

            source: (model && model.iconKey)
                ? `image://resource/${model.iconKey}/${imageState}`
                : ""

            sourceSize: Qt.size(20, 20)

            Row
            {
                id: extras

                spacing: 0
                height: parent.height
                x: -(width + (NxGlobals.hasChildren(modelIndex) ? 20 : 0))
                visible: delegateItem.showResourceStatus

                readonly property int flags: (model && model.cameraExtraStatus) || 0

                Image
                {
                    id: problemsIcon
                    visible: extras.flags & ResourceTree.CameraExtraStatusFlag.buggy
                    source: "qrc:///skin/tree/buggy.png"
                }

                Image
                {
                    id: recordingIcon

                    source:
                    {
                        if (extras.flags & ResourceTree.CameraExtraStatusFlag.recording)
                            return "qrc:///skin/tree/recording.png"

                        if (extras.flags & ResourceTree.CameraExtraStatusFlag.scheduled)
                            return "qrc:///skin/tree/scheduled.png"

                        if (extras.flags & ResourceTree.CameraExtraStatusFlag.hasArchive)
                            return "qrc:///skin/tree/has_archive.png"

                        return ""
                    }
                }
            }
        }

        Text
        {
            id: name

            text: (model && model.display) || ""
            textFormat: Text.PlainText
            font.weight: Font.DemiBold
            height: parent.height
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight

            width: Math.min(implicitWidth, delegateItem.availableWidth - x)
            color: delegateItem.color
        }

        Text
        {
            id: extraInfo

            text: (delegateItem.showExtraInfo && model && model.extraInfo) || ""
            textFormat: Text.PlainText
            font.weight: Font.Normal
            height: parent.height
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
            leftPadding: 1

            width: Math.min(implicitWidth, delegateItem.availableWidth - x)
            color: ColorTheme.colors.dark17
        }
    }

    Row
    {
        id: indicator

        leftPadding: 8
        rightPadding: 8

        visible: !!indicatorLoader.item
        width: indicatorLoader.item ? implicitWidth : 0
        height: delegateItem.height
        x: delegateItem.width - width

        readonly property int checkState: (model && model.checkState) ?? 0

        Loader
        {
            id: indicatorLoader

            active: !delegateItem.isSeparator
            baselineOffset: name.baselineOffset

            Component
            {
                id: checkBoxComponent

                CheckBoxImage
                {
                    checkState: indicator.checkState
                    anchors.baseline: indicatorLoader.baseline
                }
            }

            Component
            {
                id: radioButtonComponent

                RadioButtonImage
                {
                    checked: indicator.checkState === Qt.Checked
                    anchors.baseline: indicatorLoader.baseline
                }
            }

            sourceComponent:
            {
                if (delegateItem.customSelectionIndicator)
                    return null

                switch (delegateItem.selectionMode)
                {
                    case ResourceTree.ResourceSelection.single:
                        return delegateItem.resource ? checkBoxComponent : null

                    case ResourceTree.ResourceSelection.multiple:
                        return checkBoxComponent

                    case ResourceTree.ResourceSelection.exclusive:
                        return delegateItem.resource ? radioButtonComponent : null

                    default:
                        return null
                }
            }
        }
    }

    MouseArea
    {
        id: checkStateToggler

        anchors.fill: delegateItem
        acceptedButtons: Qt.LeftButton
        visible: !delegateItem.isSeparator && delegateItem.wholeRowToggleable

        onClicked:
        {
            model.checkState = selectionMode === ResourceTree.ResourceSelection.exclusive
                ? Qt.Checked
                : delegateItem.nextCheckState(indicator.checkState)
        }
    }

    Rectangle
    {
        id: separatorLine

        height: 1
        anchors.verticalCenter: parent.verticalCenter
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: -itemIndent

        color: ColorTheme.transparent(ColorTheme.colors.dark8, 0.4)
        visible: delegateItem.isSeparator
    }

    readonly property bool highlighted: indicator.checkState !== Qt.Unchecked

    readonly property color color: highlighted
        ? ColorTheme.colors.light4
        : ColorTheme.colors.light10
}
