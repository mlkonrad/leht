// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Judging what changed after a signature, against a certification (DocMDP)
// or a field lock (FieldMDP).
//
// MuPDF has a validator (pdf_validate_changes), but it only enforces DocMDP
// level 1: at levels 2 and 3 it rejects any change to a page dictionary --
// which is exactly how a new signature's widget, or a new annotation, arrives
// -- so it would call every permitted change a violation. This one looks at
// each object that changed after the signed revision and judges the change by
// what the object IS: a form value, a new signature, an annotation, or the
// page itself.
//
// It reads old revisions the way MuPDF's own validator does: through
// pdf_document::xref_base, which makes the document look as it was in an
// earlier xref section. That works on encrypted files too, with the one
// authentication the document already has.
#include "mdp.hpp"

#include "guards.hpp"
#include "leht/error.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <string>

namespace leht::ops::mdp {

namespace {

using detail::OwnedBuffer;
using detail::OwnedPdfObj;

std::string name_of(fz_context* g, pdf_obj* obj, pdf_obj* key) {
    const char* n = pdf_to_name(g, pdf_dict_get(g, obj, key));
    return n != nullptr ? n : "";
}

/// Keeps the document looking at `base` while alive, and puts it back
/// (through MuPDF's own guard against repairing mid-way) however it ends.
class RevisionView {
public:
    RevisionView(fz_context* c, pdf_document* pdf) : c_(c), pdf_(pdf) {
        guarded(c_, [&](fz_context* g) { pdf_start_throw_on_repair(g, pdf_, &saved_); });
    }
    RevisionView(const RevisionView&) = delete;
    RevisionView& operator=(const RevisionView&) = delete;
    ~RevisionView() {
        try {
            guarded(c_, [&](fz_context* g) { pdf_end_throw_on_repair(g, pdf_, saved_); });
        } catch (...) {
            pdf_->xref_base = saved_;
        }
    }
    [[nodiscard]] int saved() const { return saved_; }

private:
    fz_context* c_;
    pdf_document* pdf_;
    int saved_ = 0;
};

/// What an object is, for judging a change to it.
enum class Role {
    Ignored,      ///< xref and object streams, Info, XMP metadata, Encrypt
    Catalog,
    AcroForm,
    Page,
    PageContent,  ///< a stream in some page's /Contents
    Widget,       ///< a form field's widget annotation
    Field,        ///< a field dictionary without a widget of its own
    Annotation,
    Appearance,   ///< a stream in some annotation's or widget's /AP
    Dss,          ///< PAdES long-term validation data: always allowed
    Other,
};

struct Change {
    int num = 0;
    Role role = Role::Other;
    std::vector<std::string> keys;  ///< the dictionary keys whose values differ
    bool stream_changed = false;
};

/// Keys of `a` and `b` whose values differ (by pdf_objcmp: an indirect
/// reference compares by object number, so a changed target is its own change).
std::vector<std::string> changed_keys(fz_context* g, pdf_obj* a, pdf_obj* b) {
    std::set<std::string> keys;
    for (pdf_obj* d : {a, b}) {
        for (int i = 0; i < pdf_dict_len(g, d); ++i) {
            keys.insert(pdf_to_name(g, pdf_dict_get_key(g, d, i)));
        }
    }
    std::vector<std::string> out;
    for (const std::string& k : keys) {
        if (pdf_objcmp(g, pdf_dict_gets(g, a, k.c_str()), pdf_dict_gets(g, b, k.c_str())) != 0) {
            out.push_back(k);
        }
    }
    return out;
}

bool only(const std::vector<std::string>& keys, std::initializer_list<const char*> allowed) {
    return std::all_of(keys.begin(), keys.end(), [&](const std::string& k) {
        return std::any_of(allowed.begin(), allowed.end(),
                           [&](const char* a) { return k == a; });
    });
}

/// Object numbers in an array of references.
std::set<int> refs_in(fz_context* g, pdf_obj* array) {
    std::set<int> out;
    for (int i = 0; i < pdf_array_len(g, array); ++i) {
        const int n = pdf_to_num(g, pdf_array_get(g, array, i));
        if (n > 0) {
            out.insert(n);
        }
    }
    return out;
}

/// The field type, looking up the /Parent chain as the spec inherits it.
std::string field_type(fz_context* g, pdf_obj* obj) {
    const char* ft = pdf_to_name(g, pdf_dict_get_inheritable(g, obj, PDF_NAME(FT)));
    return ft != nullptr ? ft : "";
}

std::string field_name(fz_context* c, pdf_obj* obj) {
    char* name = nullptr;
    guarded(c, [&](fz_context* g) { name = pdf_load_field_name(g, obj); });
    std::string out = name != nullptr ? name : "";
    fz_free(c, name);
    return out;
}

/// Whether `field`'s value is a document timestamp (PAdES B-LTA), which a
/// certification allows at every level, like the /DSS it protects.
bool is_document_timestamp(fz_context* g, pdf_obj* field) {
    pdf_obj* v = pdf_dict_get_inheritable(g, field, PDF_NAME(V));
    const char* type = pdf_to_name(g, pdf_dict_get(g, v, PDF_NAME(Type)));
    const char* sub = pdf_to_name(g, pdf_dict_get(g, v, PDF_NAME(SubFilter)));
    return (type != nullptr && std::strcmp(type, "DocTimeStamp") == 0) ||
           (sub != nullptr && std::strcmp(sub, "ETSI.RFC3161") == 0);
}

/// Object numbers that belong to the document's /DSS: the dictionary itself
/// and any indirect arrays or /VRI entries in it. Recognised by the catalog's
/// reference, not by /Type, which other tools leave out.
std::set<int> dss_objects(fz_context* g, pdf_document* pdf) {
    std::set<int> out;
    pdf_obj* dss = pdf_dict_getp(g, pdf_trailer(g, pdf), "Root/DSS");
    if (dss == nullptr) {
        return out;
    }
    const auto note = [&](pdf_obj* o) {
        if (pdf_is_indirect(g, o)) {
            out.insert(pdf_to_num(g, o));
        }
    };
    note(dss);
    for (const char* key : {"Certs", "OCSPs", "CRLs", "VRI"}) {
        note(pdf_dict_gets(g, dss, key));
    }
    pdf_obj* vri = pdf_dict_gets(g, dss, "VRI");
    for (int i = 0; i < pdf_dict_len(g, vri) && i < 10000; ++i) {
        pdf_obj* entry = pdf_dict_get_val(g, vri, i);
        note(entry);
        for (const char* key : {"Cert", "OCSP", "CRL"}) {
            note(pdf_dict_gets(g, entry, key));
        }
    }
    return out;
}

}  // namespace

bool Lock::covers(const std::string& field) const {
    const auto named = [&](const std::vector<std::string>& list) {
        return std::find(list.begin(), list.end(), field) != list.end();
    };
    return all || named(include) || (!exclude.empty() && !named(exclude));
}

int certification_of(fz_context* g, pdf_obj* sig) {
    pdf_obj* refs = pdf_dict_get(g, sig, PDF_NAME(Reference));
    for (int i = 0; i < pdf_array_len(g, refs); ++i) {
        pdf_obj* r = pdf_array_get(g, refs, i);
        if (pdf_name_eq(g, pdf_dict_get(g, r, PDF_NAME(TransformMethod)), PDF_NAME(DocMDP))) {
            // /P is optional and 2 when absent.
            pdf_obj* p = pdf_dict_getp(g, r, "TransformParams/P");
            const int level = p != nullptr ? pdf_to_int(g, p) : 2;
            return level >= 1 && level <= 3 ? level : 2;
        }
    }
    return 0;
}

int certifying_signature(fz_context* g, pdf_document* pdf) {
    return pdf_to_num(g, pdf_dict_getp(g, pdf_trailer(g, pdf), "Root/Perms/DocMDP"));
}

Lock lock_of(fz_context* g, pdf_obj* sig, pdf_obj* field) {
    Lock lock;
    const auto merge = [&](pdf_obj* spec) {
        if (spec == nullptr) {
            return;
        }
        pdf_obj* action = pdf_dict_get(g, spec, PDF_NAME(Action));
        std::vector<std::string> names;
        pdf_obj* fields = pdf_dict_get(g, spec, PDF_NAME(Fields));
        for (int i = 0; i < pdf_array_len(g, fields); ++i) {
            const char* s = pdf_to_text_string(g, pdf_array_get(g, fields, i));
            names.emplace_back(s != nullptr ? s : "");
        }
        // The signature's FieldMDP entry and the field's own /Lock usually
        // say the same thing once enacted: merge without repeating a name.
        const auto add = [](std::vector<std::string>& to, const std::vector<std::string>& from) {
            for (const std::string& n : from) {
                if (std::find(to.begin(), to.end(), n) == to.end()) {
                    to.push_back(n);
                }
            }
        };
        if (pdf_name_eq(g, action, PDF_NAME(All))) {
            lock.all = true;
        } else if (pdf_name_eq(g, action, PDF_NAME(Include))) {
            add(lock.include, names);
        } else if (pdf_name_eq(g, action, PDF_NAME(Exclude))) {
            add(lock.exclude, names);
        }
        if (pdf_obj* p = pdf_dict_get(g, spec, PDF_NAME(P)); p != nullptr) {
            const int level = pdf_to_int(g, p);
            if (level >= 1 && level <= 3) {
                lock.p = lock.p == 0 ? level : std::min(lock.p, level);
            }
        }
    };
    pdf_obj* refs = pdf_dict_get(g, sig, PDF_NAME(Reference));
    for (int i = 0; i < pdf_array_len(g, refs); ++i) {
        pdf_obj* r = pdf_array_get(g, refs, i);
        if (pdf_name_eq(g, pdf_dict_get(g, r, PDF_NAME(TransformMethod)), PDF_NAME(FieldMDP))) {
            merge(pdf_dict_get(g, r, PDF_NAME(TransformParams)));
        }
    }
    if (field != nullptr) {
        merge(pdf_dict_get_inheritable(g, field, PDF_NAME(Lock)));
    }
    return lock;
}

Judgement judge_changes_after(fz_context* c, pdf_document* pdf, pdf_obj* sig, int level,
                              const std::vector<Lock>& locks) {
    Judgement out;
    // The levels the locks add (PDF 2.0 /P) tighten the certification.
    for (const Lock& l : locks) {
        if (l.p != 0) {
            level = level == 0 ? l.p : std::min(level, l.p);
        }
    }
    const auto locked_by = [&](const std::string& field) {
        return std::any_of(locks.begin(), locks.end(),
                           [&](const Lock& l) { return l.covers(field); });
    };

    try {
        RevisionView view(c, pdf);
        int version = -1;
        int count = 0;
        int catalog = 0;
        int acroform = 0;
        int info = 0;
        int encrypt = 0;
        guarded(c, [&](fz_context* g) {
            version = pdf_find_version_for_obj(g, pdf, sig);
            pdf->xref_base = 0;
            count = std::min(pdf_xref_len(g, pdf), pdf->max_xref_len);
            pdf_obj* trailer = pdf_trailer(g, pdf);
            catalog = pdf_to_num(g, pdf_dict_get(g, trailer, PDF_NAME(Root)));
            acroform = pdf_to_num(g, pdf_dict_getp(g, trailer, "Root/AcroForm"));
            info = pdf_to_num(g, pdf_dict_get(g, trailer, PDF_NAME(Info)));
            encrypt = pdf_to_num(g, pdf_dict_get(g, trailer, PDF_NAME(Encrypt)));
        });
        if (version <= 0) {
            // The file grew after this signature (that is why it is being
            // judged), yet no later revision can be found: its history was
            // lost, typically to a repair. Say so; never call that permitted.
            out.problems.emplace_back(
                "the file's revisions could not be told apart, so what changed after this "
                "signature could not be judged");
            return out;
        }

        // Which streams are page contents and which are appearances, and
        // whose -- worked out once, from the document as it is now.
        std::map<int, int> content_of;     // stream -> page object
        std::map<int, int> appearance_of;  // stream -> annotation object
        std::set<int> annots;
        std::set<int> widgets;
        std::set<int> dss;
        guarded(c, [&](fz_context* g) {
            pdf->xref_base = 0;
            dss = dss_objects(g, pdf);
            const int pages = pdf_count_pages(g, pdf);
            for (int i = 0; i < pages; ++i) {
                pdf_obj* page = pdf_lookup_page_obj(g, pdf, i);
                const int page_num = pdf_to_num(g, page);
                pdf_obj* contents = pdf_dict_get(g, page, PDF_NAME(Contents));
                if (pdf_is_array(g, contents)) {
                    for (int k = 0; k < pdf_array_len(g, contents); ++k) {
                        content_of[pdf_to_num(g, pdf_array_get(g, contents, k))] = page_num;
                    }
                } else if (contents != nullptr) {
                    content_of[pdf_to_num(g, contents)] = page_num;
                }
                pdf_obj* list = pdf_dict_get(g, page, PDF_NAME(Annots));
                for (int k = 0; k < pdf_array_len(g, list); ++k) {
                    pdf_obj* a = pdf_array_get(g, list, k);
                    const int an = pdf_to_num(g, a);
                    if (pdf_name_eq(g, pdf_dict_get(g, a, PDF_NAME(Subtype)), PDF_NAME(Widget))) {
                        widgets.insert(an);
                    } else {
                        annots.insert(an);
                    }
                    pdf_obj* ap = pdf_dict_get(g, a, PDF_NAME(AP));
                    for (int s = 0; s < pdf_dict_len(g, ap); ++s) {
                        pdf_obj* entry = pdf_dict_get_val(g, ap, s);
                        if (pdf_is_stream(g, entry)) {
                            appearance_of[pdf_to_num(g, entry)] = an;
                        } else {
                            for (int t = 0; t < pdf_dict_len(g, entry); ++t) {
                                appearance_of[pdf_to_num(g, pdf_dict_get_val(g, entry, t))] = an;
                            }
                        }
                    }
                }
            }
        });

        // Every object that a section newer than the signed one defines.
        std::vector<Change> changes;
        for (int num = 1; num < count; ++num) {
            if (pdf->xref_index[num] >= version) {
                continue;  // as it was when signed
            }
            OwnedPdfObj then{c};
            OwnedPdfObj now{c};
            bool existed = false;
            guarded(c, [&](fz_context* g) {
                pdf->xref_base = version;
                existed = pdf_object_exists(g, pdf, num) != 0;
                if (existed) {
                    *then.slot() = pdf_load_object(g, pdf, num);
                }
                pdf->xref_base = 0;
                if (pdf_object_exists(g, pdf, num)) {
                    *now.slot() = pdf_load_object(g, pdf, num);
                }
            });
            if (!existed) {
                continue;  // new: judged by whatever now refers to it
            }
            Change ch;
            ch.num = num;
            std::string type;
            std::string subtype;
            bool field = false;
            guarded(c, [&](fz_context* g) {
                pdf_obj* o = now.get() != nullptr ? now.get() : then.get();
                type = name_of(g, o, PDF_NAME(Type));
                subtype = name_of(g, o, PDF_NAME(Subtype));
                field = pdf_dict_get(g, o, PDF_NAME(FT)) != nullptr ||
                        pdf_dict_get(g, o, PDF_NAME(T)) != nullptr;
                ch.keys = changed_keys(g, then.get(), now.get());
            });
            // A stream's bytes, compared as stored (decrypted, still encoded).
            bool stream = false;
            guarded(c, [&](fz_context* g) { stream = pdf_is_stream(g, now.get()) != 0; });
            if (stream) {
                OwnedBuffer a{c};
                OwnedBuffer b{c};
                guarded(c, [&](fz_context* g) {
                    pdf->xref_base = version;
                    *a.slot() = pdf_load_raw_stream_number(g, pdf, num);
                    pdf->xref_base = 0;
                    *b.slot() = pdf_load_raw_stream_number(g, pdf, num);
                    ch.stream_changed = fz_buffer_storage(g, a.get(), nullptr) !=
                                            fz_buffer_storage(g, b.get(), nullptr) ||
                                        std::memcmp(a.get()->data, b.get()->data,
                                                    a.get()->len) != 0;
                });
            }
            if (ch.keys.empty() && !ch.stream_changed) {
                continue;  // rewritten, not changed
            }
            if (num == info || num == encrypt || type == "XRef" || type == "ObjStm" ||
                type == "Metadata") {
                ch.role = Role::Ignored;
            } else if (dss.count(num) != 0) {
                ch.role = Role::Dss;
            } else if (num == catalog) {
                ch.role = Role::Catalog;
            } else if (num == acroform) {
                ch.role = Role::AcroForm;
            } else if (type == "Page") {
                ch.role = Role::Page;
            } else if (content_of.count(num) != 0) {
                ch.role = Role::PageContent;
            } else if (appearance_of.count(num) != 0) {
                ch.role = Role::Appearance;
            } else if (subtype == "Widget" || widgets.count(num) != 0) {
                ch.role = Role::Widget;
            } else if (annots.count(num) != 0 || type == "Annot") {
                ch.role = Role::Annotation;
            } else if (field) {
                ch.role = Role::Field;
            } else if (dss.count(num) != 0 || type == "DSS" || type == "VRI" ||
                       type == "DocTimeStamp") {
                ch.role = Role::Dss;
            }
            changes.push_back(std::move(ch));
        }

        // The judging, by role. Without a certification (level 0) only field
        // locks apply: the page, its content and its annotations are none of
        // a FieldMDP lock's business.
        const auto add = [&](std::string why) {
            if (std::find(out.problems.begin(), out.problems.end(), why) == out.problems.end()) {
                out.problems.push_back(std::move(why));
            }
        };
        const auto forbid = [&](std::string why) {
            if (level > 0) {
                add(std::move(why));
            }
        };
        const auto annotation_change = [&](const std::string& what) {
            if (level == 1 || level == 2) {
                forbid(what + ", which the certification does not allow");
            }
        };
        const auto signature_added = [&](const std::string& field, pdf_obj* obj) {
            bool timestamp = false;
            guarded(c, [&](fz_context* g) { timestamp = is_document_timestamp(g, obj); });
            if (timestamp) {
                return;  // validation data: allowed at every level
            }
            if (level == 1) {
                forbid("signature '" + field +
                       "' was added, and the certification allows no changes");
            }
        };
        const auto form_filled = [&](pdf_obj* obj) {
            const std::string name = field_name(c, obj);
            std::string type;
            guarded(c, [&](fz_context* g) { type = field_type(g, obj); });
            if (type == "Sig") {
                signature_added(name, obj);
                return;
            }
            if (level == 1) {
                forbid("field '" + name + "' was changed, and the certification allows no "
                       "changes");
            } else if (locked_by(name)) {
                add("field '" + name + "' was changed, but a signature locks it");
            }
        };

        for (const Change& ch : changes) {
            OwnedPdfObj now{c};
            OwnedPdfObj then{c};
            guarded(c, [&](fz_context* g) {
                pdf->xref_base = version;
                *then.slot() = pdf_load_object(g, pdf, ch.num);
                pdf->xref_base = 0;
                *now.slot() = pdf_load_object(g, pdf, ch.num);
            });
            switch (ch.role) {
            case Role::Ignored:
            case Role::Dss:
                break;
            case Role::Catalog:
                for (const std::string& k : ch.keys) {
                    if (k != "AcroForm" && k != "DSS" && k != "Metadata") {
                        forbid("the document catalog's /" + k + " changed");
                    }
                }
                break;
            case Role::AcroForm:
                for (const std::string& k : ch.keys) {
                    if (k == "Fields") {
                        std::set<int> before;
                        std::set<int> after;
                        guarded(c, [&](fz_context* g) {
                            before = refs_in(g, pdf_dict_get(g, then.get(), PDF_NAME(Fields)));
                            after = refs_in(g, pdf_dict_get(g, now.get(), PDF_NAME(Fields)));
                        });
                        for (const int n : after) {
                            if (before.count(n) != 0) {
                                continue;
                            }
                            pdf_obj* added = nullptr;
                            std::string type;
                            guarded(c, [&](fz_context* g) {
                                added = pdf_new_indirect(g, pdf, n, 0);
                                type = field_type(g, added);
                            });
                            const std::string name = field_name(c, added);
                            const bool is_sig = type == "Sig";
                            if (is_sig) {
                                signature_added(name, added);
                            }
                            guarded(c, [&](fz_context* g) { pdf_drop_obj(g, added); });
                            if (!is_sig) {
                                forbid("form field '" + name + "' was added");
                            }
                        }
                        for (const int n : before) {
                            if (after.count(n) == 0) {
                                forbid("a form field was removed");
                            }
                        }
                    } else if (k != "SigFlags" && k != "DR" && k != "NeedAppearances" &&
                               k != "DA") {
                        forbid("the form's /" + k + " changed");
                    }
                }
                break;
            case Role::Page: {
                int page = -1;
                guarded(c, [&](fz_context* g) {
                    pdf_obj* ref = pdf_new_indirect(g, pdf, ch.num, 0);
                    page = pdf_lookup_page_number(g, pdf, ref);
                    pdf_drop_obj(g, ref);
                });
                const std::string where = "page " + std::to_string(page + 1);
                for (const std::string& k : ch.keys) {
                    if (k != "Annots") {
                        forbid(where + "'s /" + k + " changed");
                        continue;
                    }
                    std::set<int> before;
                    std::set<int> after;
                    guarded(c, [&](fz_context* g) {
                        before = refs_in(g, pdf_dict_get(g, then.get(), PDF_NAME(Annots)));
                        after = refs_in(g, pdf_dict_get(g, now.get(), PDF_NAME(Annots)));
                    });
                    for (const int n : after) {
                        if (before.count(n) != 0) {
                            continue;
                        }
                        if (widgets.count(n) != 0) {
                            // A widget arriving with a new signature field is
                            // judged there (AcroForm /Fields); any other new
                            // widget is a new form field.
                            pdf_obj* w = nullptr;
                            std::string type;
                            guarded(c, [&](fz_context* g) {
                                w = pdf_new_indirect(g, pdf, n, 0);
                                type = field_type(g, w);
                            });
                            const std::string name = field_name(c, w);
                            const bool is_sig = type == "Sig";
                            if (is_sig) {
                                signature_added(name, w);
                            }
                            guarded(c, [&](fz_context* g) { pdf_drop_obj(g, w); });
                            if (!is_sig) {
                                forbid("a form field was added on " + where);
                            }
                        } else {
                            annotation_change("an annotation was added on " + where);
                        }
                    }
                    for (const int n : before) {
                        if (after.count(n) == 0) {
                            annotation_change("an annotation was removed from " + where);
                        }
                    }
                }
                break;
            }
            case Role::PageContent: {
                int page = -1;
                guarded(c, [&](fz_context* g) {
                    pdf_obj* p = pdf_new_indirect(g, pdf, content_of.at(ch.num), 0);
                    page = pdf_lookup_page_number(g, pdf, p);
                    pdf_drop_obj(g, p);
                });
                forbid("the content of page " + std::to_string(page + 1) + " changed");
                break;
            }
            case Role::Widget:
            case Role::Field:
                if (only(ch.keys, {"V", "AS", "AP", "MK", "Ff", "DV"})) {
                    form_filled(now.get());
                } else {
                    forbid("form field '" + field_name(c, now.get()) + "' changed its /" +
                           ch.keys.front());
                }
                break;
            case Role::Appearance: {
                const int owner = appearance_of.at(ch.num);
                if (widgets.count(owner) != 0) {
                    pdf_obj* w = nullptr;
                    guarded(c, [&](fz_context* g) { w = pdf_new_indirect(g, pdf, owner, 0); });
                    form_filled(w);
                    guarded(c, [&](fz_context* g) { pdf_drop_obj(g, w); });
                } else {
                    annotation_change("an annotation's appearance changed");
                }
                break;
            }
            case Role::Annotation:
                annotation_change("an annotation was changed");
                break;
            case Role::Other:
                forbid("object " + std::to_string(ch.num) +
                       " (a resource the pages use) changed");
                break;
            }
        }
    } catch (const Error&) {
        out.problems.emplace_back("the file is damaged; its changes could not be judged");
    }
    return out;
}

}  // namespace leht::ops::mdp
