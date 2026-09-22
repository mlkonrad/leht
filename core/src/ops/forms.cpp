// SPDX-License-Identifier: AGPL-3.0-or-later
#include "leht/ops/forms.hpp"

#include "edit_internal.hpp"
#include "guards.hpp"
#include "leht/context.hpp"
#include "leht/document.hpp"
#include "leht/error.hpp"
#include "mupdf_c.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace leht::ops {

namespace {

/// Longest value a field may be set to; far above any real form entry.
constexpr std::size_t kMaxValue = std::size_t{1} << 20;

/// PDF 1.7 table 230: a combo box that also accepts typed text.
constexpr int kComboIsEditable = 1 << 18;

struct WidgetRef {
    int page = 0;
    pdf_annot* widget = nullptr;  ///< owned by its page, kept loaded in Widgets
    pdf_obj* field = nullptr;     ///< the terminal field this widget belongs to
    std::string name;
};

struct Widgets {
    std::vector<detail::OwnedPage> pages;  ///< keeps every WidgetRef::widget valid
    std::vector<WidgetRef> refs;
};

/// The field a widget belongs to: the widget itself when it carries the
/// field's name (a merged field/widget), else its parent.
pdf_obj* terminal_field(fz_context* g, pdf_obj* widget) {
    if (pdf_dict_get(g, widget, PDF_NAME(T)) != nullptr) {
        return widget;
    }
    pdf_obj* parent = pdf_dict_get(g, widget, PDF_NAME(Parent));
    return parent != nullptr ? parent : widget;
}

/// Frees a string MuPDF allocated, after we have copied it.
struct FzString {
    fz_context* ctx;
    char* ptr = nullptr;
    ~FzString() { fz_free(ctx, ptr); }
};

Widgets all_widgets(fz_context* c, Document& doc) {
    Widgets out;
    const int count = doc.page_count();
    for (int index = 0; index < count; ++index) {
        detail::OwnedPage loaded = detail::load_pdf_page(c, doc, index);
        pdf_page* p = detail::as_pdf_page(c, loaded);

        int n = 0;
        guarded(c, [&](fz_context* g) {
            for (pdf_annot* w = pdf_first_widget(g, p); w != nullptr; w = pdf_next_widget(g, w)) {
                ++n;
            }
        });
        std::vector<pdf_annot*> widgets(static_cast<std::size_t>(n), nullptr);
        pdf_annot** slots = widgets.data();
        guarded(c, [&](fz_context* g) {
            int i = 0;
            for (pdf_annot* w = pdf_first_widget(g, p); w != nullptr && i < n;
                 w = pdf_next_widget(g, w)) {
                slots[i++] = w;
            }
        });
        for (pdf_annot* w : widgets) {
            FzString name{c};
            pdf_obj* field = nullptr;
            guarded(c, [&](fz_context* g) {
                field = terminal_field(g, pdf_annot_obj(g, w));
                name.ptr = pdf_load_field_name(g, field);
            });
            out.refs.push_back({index, w, field, name.ptr != nullptr ? name.ptr : ""});
        }
        out.pages.push_back(std::move(loaded));
    }
    return out;
}

pdf_obj* acroform(fz_context* g, pdf_document* pdf) {
    return pdf_dict_getp(g, pdf_trailer(g, pdf), "Root/AcroForm");
}

void refuse_xfa_only(fz_context* c, pdf_document* pdf, const Widgets& widgets) {
    bool xfa = false;
    guarded(c, [&](fz_context* g) {
        xfa = pdf_dict_get(g, acroform(g, pdf), PDF_NAME(XFA)) != nullptr;
    });
    if (xfa && widgets.refs.empty()) {
        throw Error(0, "this is an XFA form, which leht cannot fill: its fields live in "
                       "XML that only Adobe readers interpret");
    }
}

FieldType to_type(enum pdf_widget_type t) {
    switch (t) {
        case PDF_WIDGET_TYPE_TEXT:        return FieldType::Text;
        case PDF_WIDGET_TYPE_CHECKBOX:    return FieldType::Checkbox;
        case PDF_WIDGET_TYPE_RADIOBUTTON: return FieldType::Radio;
        case PDF_WIDGET_TYPE_COMBOBOX:
        case PDF_WIDGET_TYPE_LISTBOX:     return FieldType::Choice;
        case PDF_WIDGET_TYPE_BUTTON:      return FieldType::PushButton;
        case PDF_WIDGET_TYPE_SIGNATURE:   return FieldType::Signature;
        default:                          return FieldType::Unknown;
    }
}

/// A choice widget's options, as displayed (`exportval` 0) or as exported.
std::vector<std::string> choice_options(fz_context* c, pdf_annot* w, int exportval) {
    int n = 0;
    guarded(c, [&](fz_context* g) { n = pdf_choice_widget_options(g, w, exportval, nullptr); });
    std::vector<const char*> raw(static_cast<std::size_t>(std::max(n, 0)), nullptr);
    const char** slots = raw.data();
    if (n > 0) {
        guarded(c, [&](fz_context* g) { (void)pdf_choice_widget_options(g, w, exportval, slots); });
    }
    std::vector<std::string> out;
    for (const char* s : raw) {
        out.emplace_back(s != nullptr ? s : "");
    }
    return out;
}

/// Everything about one field, from all of its widgets (`refs`, same name).
FieldInfo describe(fz_context* c, const std::vector<const WidgetRef*>& refs) {
    const WidgetRef& first = *refs.front();
    FieldInfo info;
    info.name = first.name;
    info.page = first.page;

    enum pdf_widget_type type = PDF_WIDGET_TYPE_UNKNOWN;
    int flags = 0;
    int max_len = 0;
    const char* value = nullptr;
    fz_rect r{};
    guarded(c, [&](fz_context* g) {
        type = pdf_widget_type(g, first.widget);
        flags = pdf_field_flags(g, first.field);
        value = pdf_field_value(g, first.field);
        r = pdf_bound_widget(g, first.widget);
        if (type == PDF_WIDGET_TYPE_TEXT) {
            max_len = pdf_text_widget_max_len(g, first.widget);
        }
    });
    info.type = to_type(type);
    info.value = value != nullptr ? value : "";
    info.rect = Rect{r.x0, r.y0, r.x1, r.y1};
    info.read_only = (flags & PDF_FIELD_IS_READ_ONLY) != 0;
    info.required = (flags & PDF_FIELD_IS_REQUIRED) != 0;
    info.max_length = std::max(max_len, 0);

    if (info.type == FieldType::Choice) {
        info.options = choice_options(c, first.widget, 0);
    } else if (info.type == FieldType::Checkbox || info.type == FieldType::Radio) {
        for (const WidgetRef* ref : refs) {
            const char* on = nullptr;
            guarded(c, [&](fz_context* g) {
                on = pdf_to_name(g, pdf_button_field_on_state(g, pdf_annot_obj(g, ref->widget)));
            });
            const std::string state = on != nullptr ? on : "";
            if (!state.empty() && state != "Off" &&
                std::find(info.options.begin(), info.options.end(), state) == info.options.end()) {
                info.options.push_back(state);
            }
        }
    }
    return info;
}

/// Widgets grouped by field name, in document order of first appearance.
std::vector<std::vector<const WidgetRef*>> by_field(const Widgets& widgets) {
    std::vector<std::vector<const WidgetRef*>> groups;
    std::map<std::string, std::size_t> index;
    for (const WidgetRef& ref : widgets.refs) {
        const auto [it, fresh] = index.try_emplace(ref.name, groups.size());
        if (fresh) {
            groups.emplace_back();
        }
        groups[it->second].push_back(&ref);
    }
    return groups;
}

std::string lowered(std::string s) {
    for (char& ch : s) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return s;
}

std::size_t utf8_length(const std::string& s) {
    return static_cast<std::size_t>(std::count_if(
        s.begin(), s.end(), [](char ch) { return (static_cast<unsigned char>(ch) & 0xC0) != 0x80; }));
}

std::string joined(const std::vector<std::string>& items) {
    std::string out;
    for (const std::string& item : items) {
        out += (out.empty() ? "" : ", ") + item;
    }
    return out;
}

/// The value to store for `requested`, after checking the field can take it.
std::string resolve_value(fz_context* c, const FieldInfo& info, const WidgetRef& first,
                          const std::string& requested) {
    const std::string what = "field '" + info.name + "'";
    switch (info.type) {
        case FieldType::Text:
            if (info.max_length > 0 &&
                utf8_length(requested) > static_cast<std::size_t>(info.max_length)) {
                throw Error(0, what + " takes at most " + std::to_string(info.max_length) +
                                   " characters");
            }
            return requested;
        case FieldType::Checkbox:
        case FieldType::Radio: {
            if (std::find(info.options.begin(), info.options.end(), requested) !=
                    info.options.end() ||
                requested == "Off") {
                return requested;
            }
            const std::string v = lowered(requested);
            if (info.type == FieldType::Checkbox && !info.options.empty()) {
                if (v == "yes" || v == "on" || v == "true" || v == "1") {
                    return info.options.front();
                }
                if (v == "no" || v == "off" || v == "false" || v == "0" || v.empty()) {
                    return "Off";
                }
            }
            throw Error(0, what + " takes one of: " + joined(info.options) + ", Off");
        }
        case FieldType::Choice: {
            const std::vector<std::string> exports = choice_options(c, first.widget, 1);
            for (std::size_t i = 0; i < info.options.size(); ++i) {
                if (info.options[i] == requested) {
                    return i < exports.size() ? exports[i] : requested;
                }
            }
            if (std::find(exports.begin(), exports.end(), requested) != exports.end()) {
                return requested;
            }
            int flags = 0;
            guarded(c, [&](fz_context* g) { flags = pdf_field_flags(g, first.field); });
            if ((flags & PDF_CH_FIELD_IS_COMBO) != 0 && (flags & kComboIsEditable) != 0) {
                return requested;
            }
            throw Error(0, what + " takes one of: " + joined(info.options));
        }
        case FieldType::PushButton:
            throw Error(0, what + " is a push button, which has no value to set");
        case FieldType::Signature:
            throw Error(0, what + " is a signature field; signing is a separate operation");
        case FieldType::Unknown:
            break;
    }
    throw Error(0, what + " is of a type leht cannot fill");
}

}  // namespace

std::vector<FieldInfo> list_fields(const Context& ctx, Document& doc) {
    fz_context* c = ctx.raw();
    pdf_document* pdf = detail::require_pdf(c, doc);
    const Widgets widgets = all_widgets(c, doc);
    refuse_xfa_only(c, pdf, widgets);
    std::vector<FieldInfo> out;
    for (const auto& group : by_field(widgets)) {
        out.push_back(describe(c, group));
    }
    return out;
}

void set_field(const Context& ctx, Document& doc, const std::string& name,
               const std::string& value) {
    fz_context* c = ctx.raw();
    pdf_document* pdf = detail::require_pdf(c, doc);
    if (value.size() > kMaxValue) {
        throw Error(0, "field value is too long");
    }
    const Widgets widgets = all_widgets(c, doc);
    refuse_xfa_only(c, pdf, widgets);

    std::vector<const WidgetRef*> group;
    for (const WidgetRef& ref : widgets.refs) {
        if (ref.name == name) {
            group.push_back(&ref);
        }
    }
    if (group.empty()) {
        throw Error(0, "no form field named '" + name + "'");
    }
    const FieldInfo info = describe(c, group);
    if (info.read_only) {
        throw Error(0, "field '" + name + "' is read-only");
    }
    const std::string stored = resolve_value(c, info, *group.front(), value);

    pdf_obj* field = group.front()->field;
    const char* text = stored.c_str();
    const bool button = info.type == FieldType::Checkbox || info.type == FieldType::Radio;
    int accepted = 0;
    guarded(c, [&](fz_context* g) {
        // A hybrid form's XFA copy of the values would now be stale.
        pdf_dict_del(g, acroform(g, pdf), PDF_NAME(XFA));
        // ignore_trigger_events = 1: no keystroke, validate or calculate
        // script runs. JavaScript is also never enabled on any document.
        accepted = pdf_set_field_value(g, pdf, field, text, 1);
        // MuPDF 1.28 stores a button's /V as a string. The spec makes it a
        // name, matching the widgets' /AS, and other readers and form-data
        // exports compare the two -- so store it as one.
        if (accepted != 0 && button) {
            pdf_dict_put_name(g, field, PDF_NAME(V), text);
        }
    });
    if (accepted == 0) {
        throw Error(0, "field '" + name + "' did not accept the value");
    }
    for (const WidgetRef* ref : group) {
        pdf_annot* w = ref->widget;
        guarded(c, [&](fz_context* g) { (void)pdf_update_widget(g, w); });
    }
}

int flatten(const Context& ctx, Document& doc, bool annotations) {
    fz_context* c = ctx.raw();
    pdf_document* pdf = detail::require_pdf(c, doc);
    const int fields = static_cast<int>(list_fields(ctx, doc).size());
    const int bake_annots = annotations ? 1 : 0;
    guarded(c, [&](fz_context* g) { pdf_bake_document(g, pdf, bake_annots, 1); });
    return fields;
}

const char* field_type_name(FieldType type) {
    switch (type) {
        case FieldType::Text:       return "text";
        case FieldType::Checkbox:   return "checkbox";
        case FieldType::Radio:      return "radio";
        case FieldType::Choice:     return "choice";
        case FieldType::PushButton: return "button";
        case FieldType::Signature:  return "signature";
        case FieldType::Unknown:    break;
    }
    return "unknown";
}

}  // namespace leht::ops
