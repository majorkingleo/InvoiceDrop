import QtQuick

/**
 * The clipboard, which QML does not otherwise have.
 *
 * There is no clipboard object to call in QML; the only writer is
 * `TextEdit.copy()`, and it copies that item's own selection. So every copy in
 * the widget goes through the one hidden text item in here, which is filled and
 * selected on demand. Two copies of this item were the alternative, and the
 * second one is where the trick would be got wrong.
 *
 * It is never visible and never has a size, and neither matters to `copy()`:
 * the operation runs on the document, not on anything that was painted.
 */
Item {
    id: root

    visible: false
    implicitWidth: 0
    implicitHeight: 0

    /// Fills the item, selects it and copies it.
    ///
    /// Returns false and touches nothing when the text is empty, so a caller
    /// cannot wipe the clipboard by copying a value that was never there. Callers
    /// use that: a sum row with no sum leaves the previous clipboard alone
    /// instead of putting an empty string on it.
    function copy(text) {
        const value = String(text);
        if (value.length === 0)
            return false;

        field.text = value;
        field.selectAll();
        field.copy();
        return true;
    }

    TextEdit {
        id: field

        visible: false
        width: 0
        height: 0
        readOnly: true
        // Plain text on purpose: an amount or a path has no markup, and what
        // lands on the clipboard should not have any either. The rich field in
        // `BillDetails.qml` is a different thing, and it is the field's own copy
        // that carries the markup, not this.
        textFormat: TextEdit.PlainText
    }
}
