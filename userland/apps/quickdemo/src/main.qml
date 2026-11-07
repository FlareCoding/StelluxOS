import QtQuick

Window {
    id: root

    required property string scene
    required property bool logsFrameRate

    property int framesSwapped: 0

    width: 640
    height: 480
    visible: true
    title: qsTr("Qt Quick demo")
    color: "#1d2330"

    onFrameSwapped: framesSwapped++

    Loader {
        anchors.fill: parent
        sourceComponent: {
            if (root.scene === "exact") {
                return exactScene
            }

            return root.scene === "text" ? textScene : animatedScene
        }
    }

    Component {
        id: animatedScene

        Item {
            Rectangle {
                x: 120
                y: 120
                width: 160
                height: 160
                color: "#4a90d9"

                RotationAnimation on rotation {
                    from: 0
                    to: 360
                    duration: 4000
                    loops: Animation.Infinite
                }
            }

            Rectangle {
                y: 380
                width: 80
                height: 24
                radius: 12
                color: "#e0a030"

                SequentialAnimation on x {
                    loops: Animation.Infinite

                    NumberAnimation {
                        from: 40
                        to: 520
                        duration: 2000
                        easing.type: Easing.InOutQuad
                    }

                    NumberAnimation {
                        from: 520
                        to: 40
                        duration: 2000
                        easing.type: Easing.InOutQuad
                    }
                }
            }

            Text {
                id: rate

                x: 16
                y: 16
                color: "#e8ecf2"
                font.pixelSize: 18
                text: qsTr("Counting frames")
            }

            Timer {
                interval: 1000
                repeat: true
                running: true

                onTriggered: {
                    rate.text = qsTr("%1 frames per second").arg(root.framesSwapped)
                    if (root.logsFrameRate) {
                        console.log(`quickdemo: ${root.framesSwapped} frames in the last second`)
                    }

                    root.framesSwapped = 0
                }
            }
        }
    }

    // Opaque, unscaled and at whole pixels, so that two renderers' pictures can be compared pixel for pixel
    Component {
        id: exactScene

        Item {
            Repeater {
                model: ["#d94a4a", "#4ad98a", "#4a8ad9", "#d9c84a", "#ffffff"]

                Rectangle {
                    required property int index
                    required property string modelData

                    x: 40 + index * 112
                    y: 40
                    width: 96
                    height: 96
                    color: modelData
                }
            }

            Repeater {
                model: 4

                Image {
                    required property int index

                    x: 40 + index * 144
                    y: 200
                    source: "image://quickdemo/checkerboard"
                    smooth: false
                }
            }
        }
    }

    // Font engine glyphs on both renderers, since the software renderer ignores the render type
    Component {
        id: textScene

        Item {
            Repeater {
                model: [
                    { background: "#ffffff", foreground: "#000000" },
                    { background: "#1d2330", foreground: "#e8ecf2" },
                    { background: "#2a6f97", foreground: "#f2c14e" },
                ]

                Rectangle {
                    required property int index
                    required property var modelData

                    x: 32
                    y: 32 + index * 96
                    width: 576
                    height: 72
                    color: modelData.background

                    Text {
                        x: 16
                        y: 20
                        renderType: Text.NativeRendering
                        font.pixelSize: 24
                        color: parent.modelData.foreground
                        text: "The quick brown fox jumps over the lazy dog"
                    }
                }
            }
        }
    }
}
