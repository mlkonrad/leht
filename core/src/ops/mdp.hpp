// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Internal header: modification detection and prevention (MDP). What a
// certification (DocMDP) or a field lock (FieldMDP) allows, and whether the
// changes made to a document after a signature stayed within it.
#pragma once

#include "mupdf_c.hpp"

#include <string>
#include <vector>

namespace leht::ops::mdp {

/// The fields a FieldMDP lock names: all of them, these, or all but these.
struct Lock {
    bool all = false;
    std::vector<std::string> include;
    std::vector<std::string> exclude;
    /// PDF 2.0 /P on a lock: a DocMDP level from then on (0: none).
    int p = 0;

    [[nodiscard]] bool any() const { return all || !include.empty() || !exclude.empty(); }
    [[nodiscard]] bool covers(const std::string& field) const;
};

/// The DocMDP level `sig` (a signature dictionary) certifies with, from its
/// /Reference: 1, 2 or 3; 0 when it is not a certification.
int certification_of(fz_context* g, pdf_obj* sig);

/// The object number of the certifying signature dictionary (Root/Perms/
/// DocMDP), 0 when the document is not certified.
int certifying_signature(fz_context* g, pdf_document* pdf);

/// The lock `sig` enacts: its FieldMDP /Reference entries, plus the /Lock of
/// the field it signs (`field`, may be null) for signatures written without.
Lock lock_of(fz_context* g, pdf_obj* sig, pdf_obj* field);

struct Judgement {
    std::vector<std::string> problems;  ///< empty: everything was allowed
};

/// Every change made after the revision `sig` was written in, judged against
/// `level` (DocMDP: 1 no changes, 2 form filling and signing, 3 also
/// annotations; 0 none) and `locks` (FieldMDP). Not guarded: call it from
/// C++, it guards its own MuPDF calls, and it restores the document's view of
/// its revisions however it ends.
Judgement judge_changes_after(fz_context* c, pdf_document* pdf, pdf_obj* sig, int level,
                              const std::vector<Lock>& locks);

}  // namespace leht::ops::mdp
