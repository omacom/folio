import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// GVfs's password request while connecting to another machine whose keys
// aren't set up, or its question about a server it can't vouch for (a key
// seen for the first time, a certificate that can't be verified), put to
// the user in the window. The wording comes from GVfs, already translated.
// Or a host's shared folders (smb://nas), to pick the one to open.
FolioDialog {
    id: prompt

    readonly property var mounter: backend.mounter
    readonly property bool picking: mounter.picking
    readonly property bool question: mounter.choices.length > 0 && !picking

    title: picking ? qsTr("Shared folders") : question ? qsTr("Check the server") : qsTr("Sign in")
    cardWidth: 600
    spacing: 18
    visible: mounter.asking

    // Each request starts fresh, whichever way the last one closed
    // (Connect, Cancel, Escape or GVfs giving up), so a password typed for
    // one machine is never offered to the next, nor kept once it's closed.
    // A question takes the keyboard with none of its answers picked, so Tab
    // goes to them and nothing is answered by a stray Enter.
    onVisibleChanged: {
        password.text = ""
        if (visible && (question || picking)) {
            forceActiveFocus()
        } else if (visible) {
            user.text = mounter.user
            password.forceActiveFocus()
        }
    }

    function submit() {
        prompt.mounter.providePassword(user.text, password.text)
    }

    // A click outside the card does nothing: GVfs waits for an answer,
    // which only the buttons or Escape give.
    Shortcut { sequence: "Escape"; enabled: prompt.visible; context: Qt.WindowShortcut; onActivated: prompt.mounter.cancel() }

    // Aligned explicitly, so that it's mirrored: GVfs may word it in
    // another language than Folio's.
    FolioText {
        Layout.fillWidth: true
        horizontalAlignment: Text.AlignLeft
        text: prompt.mounter.prompt
        color: theme.foreground
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
    }

    PlainField {
        id: user
        visible: !prompt.question && !prompt.picking
        Layout.fillWidth: true
        placeholderText: qsTr("User")
    }
    PlainField {
        id: password
        visible: !prompt.question && !prompt.picking
        Layout.fillWidth: true
        placeholderText: qsTr("Password")
        echoMode: TextInput.Password
        onAccepted: prompt.submit()
    }

    // A host can share many folders, so they wrap rather than run off the
    // card, and past a few rows they scroll, keeping Cancel in the window.
    Flickable {
        id: shares
        visible: prompt.picking
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(flow.implicitHeight, 240)
        contentHeight: flow.implicitHeight
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        ScrollBar.vertical: FolioScrollBar { }

        Flow {
            id: flow
            width: shares.width
            spacing: 10
            Repeater {
                model: prompt.picking ? prompt.mounter.choices : []
                PlainButton {
                    required property string modelData
                    required property int index
                    text: modelData
                    // A name wider than the card is shortened in its
                    // middle, so shares alike at the start still differ.
                    width: Math.min(implicitWidth, shares.width)
                    elide: Text.ElideMiddle
                    onClicked: prompt.mounter.choose(index)
                    // Tabbed to below or above what's shown, it's scrolled to.
                    onActiveFocusChanged: {
                        if (!activeFocus)
                            return
                        if (y < shares.contentY)
                            shares.contentY = y
                        else if (y + height > shares.contentY + shares.height)
                            shares.contentY = y + height - shares.height
                    }
                }
            }
        }
    }

    RowLayout {
        Layout.alignment: Qt.AlignRight
        spacing: 10

        // A question's own answers, none of them the default: going ahead
        // with a server that couldn't be checked takes a deliberate click.
        Repeater {
            model: prompt.question ? prompt.mounter.choices : []
            PlainButton {
                required property string modelData
                required property int index
                text: modelData
                onClicked: prompt.mounter.choose(index)
            }
        }
        PlainButton {
            visible: !prompt.question
            text: qsTr("Cancel")
            onClicked: prompt.mounter.cancel()
        }
        PlainButton {
            visible: !prompt.question && !prompt.picking
            text: qsTr("Connect")
            primary: true
            onClicked: prompt.submit()
        }
    }
}
