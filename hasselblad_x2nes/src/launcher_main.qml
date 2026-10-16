/* X2D II game launcher — display layer only.
 * Input is handled by modgame.so reading the touchscreen directly
 * (eagle-shell gives the "camera-gui"-classed surface no input focus),
 * so this QML carries NO interaction logic - it just draws the menu.
 * Layout constants must match the tap-region table in modgame.c.
 * __ROMNAMES__ is replaced by deploy_gui.py.
 */
import QtQuick
import QtQuick.Window

Window {
    id: root
    objectName: "MainWindow"
    color: "#101018"
    width: 1024
    height: 768
    visibility: Window.FullScreen

    property var romNames: __ROMNAMES__

    Rectangle {
        width: root.width
        height: 100
        color: "#181830"
        Text {
            x: 44
            anchors.verticalCenter: parent.verticalCenter
            text: "GAME"
            color: "#e8e8f0"
            font.pixelSize: 48
            font.bold: true
        }
        Text {
            x: parent.width - 340
            anchors.verticalCenter: parent.verticalCenter
            text: "touch to play"
            color: "#606088"
            font.pixelSize: 24
        }
    }

    // big GAME tile -> rom 0 (Mario)
    Rectangle {
        x: 132; y: 130; width: 350; height: 250; radius: 22
        color: "#1c1c2e"
        border.color: "#7f8fff"; border.width: 3
        Text {
            x: 0; y: 66; width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: "GAME"; color: "white"; font.pixelSize: 54; font.bold: true
        }
        Text {
            x: 0; y: 150; width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: root.romNames.length > 0 ? root.romNames[0] : "no rom"
            color: "#a0a0c0"; font.pixelSize: 24
        }
    }

    // selftest tile
    Rectangle {
        x: 542; y: 130; width: 350; height: 115; radius: 18
        color: "#1c1c2e"
        border.color: "#38d078"; border.width: 2
        Text { anchors.centerIn: parent; text: "SELFTEST"; color: "white"; font.pixelSize: 34; font.bold: true }
    }

    // exit tile -> back to stock camera GUI
    Rectangle {
        x: 542; y: 265; width: 350; height: 115; radius: 18
        color: "#1c1c2e"
        border.color: "#d03838"; border.width: 2
        Text { anchors.centerIn: parent; text: "EXIT - CAMERA"; color: "white"; font.pixelSize: 30; font.bold: true }
    }

    // rom rows (rom 0 repeats the GAME tile entry for completeness)
    Column {
        x: 162; y: 400; width: 700; spacing: 10
        Repeater {
            model: root.romNames
            Rectangle {
                width: 700; height: 60; radius: 12
                color: "#1c1c2e"
                border.color: "#3a4058"; border.width: 1
                Text {
                    x: 26
                    anchors.verticalCenter: parent.verticalCenter
                    text: index + ". " + modelData
                    color: "#e0e0f0"; font.pixelSize: 24
                }
                Text {
                    x: parent.width - 120
                    anchors.verticalCenter: parent.verticalCenter
                    text: "PLAY"
                    color: "#38b0d0"; font.pixelSize: 22; font.bold: true
                }
            }
        }
    }
}
