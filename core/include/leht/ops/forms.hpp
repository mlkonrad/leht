// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "leht/edit.hpp"

#include <string>
#include <vector>

namespace leht {
class Context;
class Document;
}  // namespace leht

namespace leht::ops {

enum class FieldType { Text, Checkbox, Radio, Choice, PushButton, Signature, Unknown };

/// One AcroForm field. A field with several widgets (a radio group, or a
/// value repeated on several pages) is listed once, at its first widget.
struct FieldInfo {
    std::string name;  ///< fully qualified: "address.street"
    FieldType type = FieldType::Unknown;
    /// Text, the chosen option, or for checkboxes and radios the on-state
    /// name that is set ("Off" when none is).
    std::string value;
    /// Choice: the options as displayed. Checkbox and radio: the on-state
    /// names a value may take (besides "Off").
    std::vector<std::string> options;
    int page = 0;  ///< 0-based, of the first widget
    Rect rect;     ///< of the first widget
    bool read_only = false;
    bool required = false;
    int max_length = 0;  ///< text: most characters allowed, 0 = no limit
};

/// Every field in the document's AcroForm. Throws leht::Error if the form
/// is XFA-only: an XFA form's fields live in XML that leht does not
/// interpret, and reporting "no fields" would be a lie.
std::vector<FieldInfo> list_fields(const Context& ctx, Document& doc);

/// Sets field `name` to `value` and regenerates its appearance, so every
/// reader shows the new value.
///
///   Text      any text, within the field's maximum length
///   Checkbox  its on-state name, or yes/no, on/off, true/false, 1/0
///   Radio     one of its on-state names, or "Off"
///   Choice    one of the options (as displayed, or its export value); an
///             editable combo box also takes free text
///
/// NEVER RUNS DOCUMENT JAVASCRIPT. Keystroke, validate and calculate actions
/// are ignored -- a form must not be able to execute code -- so a field
/// whose document relies on a script to reformat or recalculate it will not
/// be reformatted.
///
/// A hybrid form carries its values twice, in AcroForm and in XFA; after a
/// change the XFA copy would be stale, and XFA-first readers would show it.
/// So the first change drops the XFA part, leaving the AcroForm form that
/// every reader understands.
///
/// Throws leht::Error for an unknown or read-only field, a push button or
/// signature field, or a value the field cannot take.
void set_field(const Context& ctx, Document& doc, const std::string& name,
               const std::string& value);

/// Bakes every form field (and, if `annotations`, every other annotation)
/// into the page content: what was shown stays shown, but can no longer be
/// edited. Returns how many form fields there were.
int flatten(const Context& ctx, Document& doc, bool annotations = false);

/// Human-readable type name, for CLI output.
[[nodiscard]] const char* field_type_name(FieldType type);

}  // namespace leht::ops
