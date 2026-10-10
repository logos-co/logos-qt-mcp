// A button that opens a modal Popup, and in it a button that clears a field and closes the Popup.
// "nester" runs a nested event loop (main.cpp) until nestedLoop.quit().
import QtQuick
import QtQuick.Controls.Basic

Item {
    id: root
    property int innerClicks: 0

    Button {
        objectName: "opener"
        text: "Open"
        x: 20; y: 20; width: 120; height: 40
        onClicked: sheet.open()
    }

    TextField {
        id: field
        objectName: "field"
        x: 20; y: 80; width: 200; height: 40
    }

    Button {
        objectName: "nester"
        text: "Nest"
        x: 20; y: 140; width: 120; height: 40
        onClicked: nestedLoop.run()
    }

    Popup {
        id: sheet
        objectName: "sheet"
        modal: true
        x: 220; y: 150; width: 200; height: 120

        Button {
            objectName: "innerButton"
            text: "Done"
            anchors.centerIn: parent
            width: 120; height: 40
            onClicked: { root.innerClicks++; field.text = ""; sheet.close() }
        }
    }
}
