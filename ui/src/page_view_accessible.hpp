// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QString>

class PageView;

/// What a screen reader gets from the page view: a document whose text is
/// the current page's words, so "read all" and moving by word or line work,
/// and whose description says which page it is. The text comes from the
/// worker, asked for only while an assistive tool is listening.
namespace pagereading {

/// Registers the accessible interface for PageView; once per process.
void install();

/// The words of `page`, read by the worker. Kept for the page shown; a reader
/// told the text changed.
void setPageText(PageView* view, int page, const QString& text);

}  // namespace pagereading
