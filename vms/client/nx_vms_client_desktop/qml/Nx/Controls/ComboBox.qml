// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

import QtQuick
import QtQuick.Controls

import Nx.Core
import Nx.Core.Controls
import nx.vms.client.desktop

import "private"

ComboBox
{
    id: control

    readonly property int requiredContentWidth: textInput.contentWidth
        + textInput.leftPadding + indicator.width

    property alias placeholderText: placeholderText.text

    // If true, combobox displays colored rectange to the left of the text.
    // Color values are taken from the model with role valueRole.
    property bool withColorSection: false

    // If true, combobox displays icon to the left of the text.
    // Icon source path are taken from the model with role decorationRole.
    // withIconSection and withColorSection are mutually exclusive.
    property bool withIconSection: false

    property string tabRole: ""
    property real tabSize: 16

    property string enabledRole: ""
    property string decorationRole: "decorationPath"

    property var displayedColor:
    {
        if (!withColorSection || !valueRole)
            return "transparent"

        if (editable && editText != currentText)
            return editText

        return currentValue
    }

    property string decorationPath:
    {
        if (!withIconSection)
            return ""
        if (Array.isArray(control.model))
            return model[currentIndex] ? model[currentIndex][decorationRole] : ""
        const data = NxGlobals.modelData(model.index(currentIndex, 0), decorationRole)
        return data ? data : ""
    }

    property Component menu: Component
    {
        EditContextMenu
        {
            readActionsVisible: textInput.echoMode == TextInput.Normal
            onCutAction: textInput.cut()
            onCopyAction: textInput.copy()
            onPasteAction: textInput.paste()
            onDeleteAction: textInput.remove(textInput.selectionStart, textInput.selectionEnd);
        }
    }

    property Component view: Component
    {
        ListView
        {
            clip: true
            model: control.popup.visible ? control.delegateModel : null
            implicitHeight: contentHeight
            currentIndex: control.highlightedIndex
            boundsBehavior: ListView.StopAtBounds
        }
    }

    implicitWidth: 200
    implicitHeight: 28

    background: backgroundLoader.item
    font.pixelSize: 14

    // TODO: Qt6 'pressed' is readonly
    //pressed: hoverArea.pressed

    // Cancel current edit if an item is re-selected from the popup.
    onActivated: (index) => editText = textAt(index)

    Loader
    {
        id: backgroundLoader

        sourceComponent: control.editable ? textFieldBackground : buttonBackground

        Component
        {
            id: textFieldBackground

            TextFieldBackground { control: parent }
        }

        Component
        {
            id: buttonBackground

            ButtonBackground
            {
                hovered: control.hovered
                pressed: control.pressed

                FocusFrame
                {
                    anchors.fill: parent
                    anchors.margins: 1
                    visible: control.visualFocus
                    color: control.isAccentButton ? ColorTheme.brightText : ColorTheme.highlight
                }
            }
        }
    }

    contentItem: TextInput
    {
        id: textInput

        width: parent.width - indicator.width
        height: parent.height
        leftPadding:
        {
            if (control.withIconSection)
                return (currentIndex === -1) ? 8 : 32
            return control.withColorSection ? 26 : 8
        }
        clip: true

        autoScroll: activeFocus
        selectByMouse: true
        selectionColor: ColorTheme.highlight
        font: control.font
        color: ColorTheme.text
        verticalAlignment: Text.AlignVCenter

        readOnly: !control.editable || control.down
        enabled: control.editable

        text: control.editable ? control.editText : control.displayText

        opacity: control.enabled ? 1.0 : 0.3

        onActiveFocusChanged:
        {
            if (!control.editable)
                return

            if (activeFocus)
                selectAll()
            else
                cursorPosition = 0
        }

        Text
        {
            id: placeholderText

            x: parent.leftPadding
            anchors.verticalCenter: parent.verticalCenter

            color: ColorTheme.text
            opacity: 0.5
            visible: !parent.text && !parent.activeFocus
        }

        Rectangle
        {
            x: 8
            width: 14
            height: 14
            radius: 1
            anchors.verticalCenter: parent.verticalCenter
            visible: control.withColorSection
            color: CoreUtils.getValue(control.displayedColor, "transparent")
            border.color: ColorTheme.transparent(ColorTheme.colors.light1, 0.1)
        }

        ColoredImage
        {
            x: 8
            anchors.verticalCenter: parent.verticalCenter
            visible: control.withIconSection
            sourcePath: control.decorationPath
            sourceSize: Qt.size(20, 20)
            primaryColor: parent.color
        }

        ContextMenuMouseArea
        {
            anchors.fill: parent
            anchors.topMargin: parent.topPadding
            anchors.bottomMargin: parent.bottomPadding
            anchors.leftMargin: parent.leftPadding
            anchors.rightMargin: parent.rightPadding

            menu: control.menu
            parentSelectionStart: textInput.selectionStart
            parentSelectionEnd: textInput.selectionEnd
            onMenuOpened: textInput.select(prevSelectionStart, prevSelectionEnd)
        }
    }

    indicator: Item
    {
        width: 28
        height: control.height

        anchors.right: control.right

        opacity: enabled ? 1.0 : 0.3

        MouseArea
        {
            id: hoverArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.ArrowCursor

            visible: control.editable

            onClicked: control.popup.visible = !control.popup.visible
        }

        Rectangle
        {
            width: 26
            height: 26
            anchors.centerIn: parent
            color:
            {
                if (control.pressed)
                    return ColorTheme.lighter(ColorTheme.shadow, 1)

                if (hoverArea.containsMouse)
                    return ColorTheme.lighter(ColorTheme.shadow, 2)

                return "transparent"
            }

            visible: control.editable
        }

        ArrowIcon
        {
            anchors.centerIn: parent
            color: ColorTheme.text
            rotation: popup.opened ? 180 : 0
        }
    }

    delegate: ItemDelegate
    {
        id: popupItem

        readonly property var itemData: Array.isArray(control.model) ? modelData : model

        height: 24
        width: control.width

        enabled: control.enabledRole ? popupItem.itemData[control.enabledRole] : true

        background: Rectangle
        {
            color: highlightedIndex == index ? ColorTheme.colors.brand_core : ColorTheme.midlight
        }

        contentItem: Text
        {
            anchors.fill: parent
            leftPadding: tab * control.tabSize
                + (control.withIconSection ? 32 : (control.withColorSection ? 26 : 8))

            rightPadding: 8
            elide: Text.ElideRight
            color: highlightedIndex == index ? ColorTheme.colors.brand_contrast : ColorTheme.text
            verticalAlignment: Text.AlignVCenter
            font: control.font

            readonly property real tab: control.tabRole ? popupItem.itemData[control.tabRole] : 0

            text: control.textRole
                ? popupItem.itemData[control.textRole]
                : CoreUtils.getValue(popupItem.itemData, "")

            Rectangle
            {
                x: 8
                width: 14
                height: 14
                radius: 1
                anchors.verticalCenter: parent.verticalCenter
                visible: control.withColorSection

                border.color: ColorTheme.transparent(ColorTheme.colors.light1, 0.1)

                color:
                {
                    if (!control.valueRole || !control.withColorSection)
                        return "transparent"

                    const color = popupItem.itemData[control.valueRole]
                    return CoreUtils.getValue(color, "transparent")
                }
            }

            ColoredImage
            {
                x: 8
                anchors.verticalCenter: parent.verticalCenter
                visible: control.withIconSection
                sourcePath:
                {
                    const icon = popupItem.itemData[control.decorationRole]
                    return icon ? icon : ""
                }
                sourceSize: Qt.size(20, 20)
                primaryColor: parent.color
            }
        }
    }

    popup: Popup
    {
        id: popupObject

        y: control.height
        width: control.width
        implicitHeight: contentItem.implicitHeight + topPadding + bottomPadding
        padding: 0
        topPadding: 2
        bottomPadding: 2

        background: Rectangle
        {
            color: ColorTheme.midlight
            radius: 2
        }

        contentItem: Loader
        {
            sourceComponent: control.view
        }
    }
}
