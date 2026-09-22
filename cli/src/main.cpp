// SPDX-License-Identifier: AGPL-3.0-or-later
//
// leht(1) -- the headless front end to leht::core.
//
// Hand-rolled argument parsing on purpose: a PDF toolkit that advertises being
// light should not pull in a command-line framework to read a dozen flags.

#include "leht/context.hpp"
#include "leht/document.hpp"
#include "leht/edit.hpp"
#include "leht/error.hpp"
#include "leht/ops/compress.hpp"
#include "leht/ops/encrypt.hpp"
#include "leht/ops/merge.hpp"
#include "leht/ops/pages.hpp"
#include "leht/ops/redact.hpp"
#include "leht/text.hpp"
#include "leht/renderer.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr const char* kUsage =
    "leht -- fast, desktop-neutral PDF toolkit\n"
    "\n"
    "usage: leht <command> [options] <files...>\n"
    "\n"
    "commands:\n"
    "  info      FILE...                      pages, size, metadata, encryption\n"
    "  text      FILE [-p N] [--search TERM]  extract text, or search for TERM\n"
    "  render    FILE -o OUT.png [-p N] [-z Z]  render one page to PNG\n"
    "  merge     FILE... -o OUT.pdf           merge PDFs and images, in order\n"
    "  split     FILE -o 'part-%03d.pdf' [-n N]  split into chunks\n"
    "  extract   FILE -p RANGES -o OUT.pdf    keep only these pages\n"
    "  remove    FILE -p RANGES -o OUT.pdf    drop these pages\n"
    "  rotate    FILE -p RANGES -d DEG -o OUT.pdf   rotate by a multiple of 90\n"
    "  compress  FILE -o OUT.pdf [--preset P] [-q N] [--linearize]\n"
    "  encrypt   FILE -o OUT.pdf [--user-pw PW] [--owner-pw PW] [--method M]\n"
    "  decrypt   FILE -o OUT.pdf [--password PW]\n"
    "  redact    FILE -o OUT.pdf [--text TERM] [--rect P:X0,Y0,X1,Y1]... [-p RANGES]\n"
    "            remove text, images and drawing for good, not just cover them;\n"
    "            exits 3 if TERM still appears somewhere it could not remove\n"
    "\n"
    "options:\n"
    "  -o PATH        output file or pattern\n"
    "  -p SPEC        pages, 1-based and inclusive: \"1-5,8,12-\" (default: all)\n"
    "                 a descending range reverses, so \"5-1\" flips those pages\n"
    "  -z ZOOM        render scale, 1.0 = 72 DPI (default 1.0)\n"
    "  -n COUNT       pages per file when splitting (default 1)\n"
    "  -d DEGREES     rotation, a multiple of 90, may be negative\n"
    "  -q QUALITY     JPEG quality 1-100, overrides the preset\n"
    "  --preset P     lossless | print | ebook | screen   (default ebook)\n"
    "  --method M     aes256 | aes128 | rc4               (default aes256)\n"
    "  --linearize    optimise for progressive web loading\n"
    "  --text TERM    redact every occurrence of TERM (case-insensitive)\n"
    "  --rect P:BOX   redact a box on page P: points from the page's top-left,\n"
    "                 e.g. 1:72,100,300,120. May be repeated\n"
    "  --images M     pixels | remove | keep: what happens to an image under a\n"
    "                 box (default pixels: black out only the covered part)\n"
    "  --no-boxes     leave removed areas blank instead of drawing black boxes\n"
    "\n"
    "leht is free software under the AGPL-3.0-or-later.\n";

/// Parsed command line: flags with values, plus the positional arguments.
struct Args {
    std::string command;
    std::vector<std::string> positional;
    std::map<std::string, std::string> flags;          ///< last value wins
    std::vector<std::pair<std::string, std::string>> all_flags;  ///< every value, in order
    std::vector<std::string> switches;

    /// Every value given for a repeatable flag, in command-line order.
    [[nodiscard]] std::vector<std::string> values(const std::string& name) const {
        std::vector<std::string> out;
        for (const auto& [flag_name, value] : all_flags) {
            if (flag_name == name) {
                out.push_back(value);
            }
        }
        return out;
    }

    [[nodiscard]] bool has_switch(const std::string& name) const {
        for (const std::string& s : switches) {
            if (s == name) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] std::string flag(const std::string& name,
                                   const std::string& fallback = "") const {
        const auto it = flags.find(name);
        return it == flags.end() ? fallback : it->second;
    }

    // Strict parsing. std::atoi and std::atof return 0 for anything they cannot
    // read and ignore trailing junk, so `-d abc` used to rotate by 0 degrees and
    // report success, and `-d 90xyz` silently meant 90. For a tool people put in
    // scripts, a typo must be an error, not a quietly different command.
    [[nodiscard]] int int_flag(const std::string& name, int fallback) const {
        const auto it = flags.find(name);
        if (it == flags.end()) {
            return fallback;
        }
        const std::string& text = it->second;
        errno = 0;
        char* end = nullptr;
        const long value = std::strtol(text.c_str(), &end, 10);
        if (text.empty() || end == text.c_str() || *end != '\0') {
            throw leht::Error(0, name + " expects a whole number, got '" + text + "'");
        }
        if (errno == ERANGE || value < INT_MIN || value > INT_MAX) {
            throw leht::Error(0, name + " is out of range: " + text);
        }
        return static_cast<int>(value);
    }

    [[nodiscard]] float float_flag(const std::string& name, float fallback) const {
        const auto it = flags.find(name);
        if (it == flags.end()) {
            return fallback;
        }
        const std::string& text = it->second;
        errno = 0;
        char* end = nullptr;
        const double value = std::strtod(text.c_str(), &end);
        if (text.empty() || end == text.c_str() || *end != '\0') {
            throw leht::Error(0, name + " expects a number, got '" + text + "'");
        }
        // strtod happily accepts "inf" and "nan"; neither is a meaningful value
        // for any leht flag, and NaN in particular must never reach MuPDF.
        if (errno == ERANGE || !std::isfinite(value)) {
            throw leht::Error(0, name + " must be a finite number, got '" + text + "'");
        }
        return static_cast<float>(value);
    }
};

/// Flags that take a value. Anything else beginning with '-' is a switch.
bool takes_value(const std::string& name) {
    static const std::vector<std::string> kValued{
        "-o", "-p", "-z", "-n", "-d", "-q", "--preset",
        "--method", "--user-pw", "--owner-pw", "--password", "--search",
        "--text", "--rect", "--images"};
    for (const std::string& v : kValued) {
        if (v == name) {
            return true;
        }
    }
    return false;
}

Args parse(int argc, char** argv) {
    Args args;
    if (argc > 1) {
        args.command = argv[1];
    }
    for (int i = 2; i < argc; ++i) {
        const std::string token = argv[i];
        if (token.size() > 1 && token[0] == '-') {
            if (takes_value(token)) {
                if (i + 1 >= argc) {
                    throw leht::Error(0, token + " needs a value");
                }
                args.flags[token] = argv[++i];
                args.all_flags.emplace_back(token, argv[i]);
            } else {
                args.switches.push_back(token);
            }
        } else {
            args.positional.push_back(token);
        }
    }
    return args;
}

std::string human_bytes(std::size_t bytes) {
    static const char* kUnits[] = {"B", "KB", "MB", "GB"};
    auto value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 3) {
        value /= 1024.0;
        ++unit;
    }
    std::array<char, 64> out{};
    std::snprintf(out.data(), out.size(), unit == 0 ? "%.0f %s" : "%.1f %s",
                  value, kUnits[unit]);
    return out.data();
}

std::string require_output(const Args& args) {
    const std::string out = args.flag("-o");
    if (out.empty()) {
        throw leht::Error(0, "this command needs an output path (-o)");
    }
    return out;
}

std::string require_input(const Args& args) {
    if (args.positional.empty()) {
        throw leht::Error(0, "this command needs an input file");
    }
    return args.positional.front();
}

leht::ops::CompressPreset parse_preset(const std::string& name) {
    if (name.empty() || name == "ebook")  { return leht::ops::CompressPreset::Ebook; }
    if (name == "lossless")               { return leht::ops::CompressPreset::Lossless; }
    if (name == "print")                  { return leht::ops::CompressPreset::Print; }
    if (name == "screen")                 { return leht::ops::CompressPreset::Screen; }
    throw leht::Error(0, "unknown preset '" + name +
                             "' (lossless, print, ebook, screen)");
}

leht::ops::Encryption parse_method(const std::string& name) {
    if (name.empty() || name == "aes256") { return leht::ops::Encryption::Aes256; }
    if (name == "aes128")                 { return leht::ops::Encryption::Aes128; }
    if (name == "rc4")                    { return leht::ops::Encryption::Rc4_128; }
    throw leht::Error(0, "unknown method '" + name + "' (aes256, aes128, rc4)");
}

// -- commands ---------------------------------------------------------------

int cmd_text(const leht::Context& ctx, const Args& args) {
    const std::string input = require_input(args);
    leht::Document doc = leht::Document::open(ctx, input);

    const int page = args.int_flag("-p", 0);  // 0 = all pages
    const std::string needle = args.flag("--search");

    const int first = page > 0 ? page : 1;
    const int last = page > 0 ? page : doc.page_count();
    if (page != 0 && (page < 1 || page > doc.page_count())) {
        throw leht::Error(0, "page " + std::to_string(page) + " is out of range (" +
                                 std::to_string(doc.page_count()) + " pages)");
    }

    int total_hits = 0;
    for (int p = first; p <= last; ++p) {
        leht::TextPage tp{ctx, doc, p - 1};
        if (needle.empty()) {
            std::printf("%s", tp.text().c_str());
        } else {
            const auto hits = tp.search(needle);
            for (const auto& hit : hits) {
                if (!hit.quads.empty()) {
                    const leht::TextQuad& q = hit.quads.front();
                    std::printf("page %d  (%.0f, %.0f)\n", p,
                                static_cast<double>(q.min_x()),
                                static_cast<double>(q.min_y()));
                }
            }
            total_hits += static_cast<int>(hits.size());
        }
    }
    if (!needle.empty()) {
        std::fprintf(stderr, "%d match%s for \"%s\"\n", total_hits,
                     total_hits == 1 ? "" : "es", needle.c_str());
    }
    return 0;
}

int cmd_info(const leht::Context& ctx, const Args& args) {
    if (args.positional.empty()) {
        throw leht::Error(0, "info needs at least one file");
    }
    for (const std::string& path : args.positional) {
        leht::Document doc = leht::Document::open(ctx, path);

        std::error_code ec;
        const auto size = fs::file_size(path, ec);
        std::printf("%s\n", path.c_str());
        std::printf("  size       %s\n",
                    human_bytes(ec ? 0 : static_cast<std::size_t>(size)).c_str());

        if (doc.needs_password()) {
            std::printf("  encrypted  yes (locked -- supply a password to read more)\n");
            continue;
        }
        std::printf("  pages      %d\n", doc.page_count());
        std::printf("  encrypted  no\n");

        for (const char* key : {"format", "info:Title", "info:Author",
                                "info:Subject", "info:Creator",
                                "info:Producer", "info:CreationDate"}) {
            if (const auto value = doc.metadata(key); value && !value->empty()) {
                std::printf("  %-10s %s\n", key, value->c_str());
            }
        }
    }
    return 0;
}

int cmd_render(const leht::Context& ctx, const Args& args) {
    const std::string input = require_input(args);
    const std::string output = require_output(args);
    const int page = args.int_flag("-p", 1);
    const float zoom = args.float_flag("-z", 1.0F);

    leht::Document doc = leht::Document::open(ctx, input);
    if (page < 1 || page > doc.page_count()) {
        throw leht::Error(0, "page " + std::to_string(page) + " is out of range (" +
                                 std::to_string(doc.page_count()) + " pages)");
    }

    leht::Renderer renderer{ctx, doc};
    const auto bitmap = renderer.render(page - 1, zoom);
    if (!bitmap) {
        throw leht::Error(0, "render was cancelled");
    }
    leht::write_png(ctx, *bitmap, output);

    std::printf("wrote %s  %dx%d px\n", output.c_str(), bitmap->width,
                bitmap->height);
    return 0;
}

int cmd_merge(const leht::Context& ctx, const Args& args) {
    if (args.positional.empty()) {
        throw leht::Error(0, "merge needs at least one input");
    }
    const std::string output = require_output(args);

    leht::ops::MergeOptions options;
    options.linearize = args.has_switch("--linearize");

    const auto result = leht::ops::merge(ctx, args.positional, output, options);
    std::printf("merged %d input%s into %s  %d pages, %s\n",
                result.inputs_merged, result.inputs_merged == 1 ? "" : "s",
                output.c_str(), result.pages_written,
                human_bytes(result.output_bytes).c_str());
    return 0;
}

int cmd_compress(const leht::Context& ctx, const Args& args) {
    const std::string input = require_input(args);
    const std::string output = require_output(args);

    leht::ops::CompressOptions options;
    options.preset = parse_preset(args.flag("--preset"));
    options.jpeg_quality = args.int_flag("-q", 0);
    options.linearize = args.has_switch("--linearize");

    const auto result = leht::ops::compress(ctx, input, output, options);
    const double saved = result.saved_fraction() * 100.0;

    std::printf("%s -> %s  [%s]\n", input.c_str(), output.c_str(),
                leht::ops::preset_name(options.preset));
    std::printf("  %s -> %s  (%.1f%% %s)\n",
                human_bytes(result.input_bytes).c_str(),
                human_bytes(result.output_bytes).c_str(),
                saved < 0 ? -saved : saved, saved < 0 ? "larger" : "smaller");
    if (result.images_examined > 0) {
        std::printf("  %d of %d images recompressed\n",
                    result.images_recompressed, result.images_examined);
    }
    if (saved <= 0) {
        std::printf("  note: already well compressed; the original is the "
                    "better file\n");
    }
    return 0;
}

int cmd_extract(const leht::Context& ctx, const Args& args) {
    const auto result = leht::ops::extract(ctx, require_input(args),
                                           require_output(args),
                                           args.flag("-p"));
    std::printf("wrote %d page%s, %s\n", result.pages_written,
                result.pages_written == 1 ? "" : "s",
                human_bytes(result.output_bytes).c_str());
    return 0;
}

int cmd_remove(const leht::Context& ctx, const Args& args) {
    const auto result = leht::ops::remove_pages(ctx, require_input(args),
                                                require_output(args),
                                                args.flag("-p"));
    std::printf("wrote %d remaining page%s, %s\n", result.pages_written,
                result.pages_written == 1 ? "" : "s",
                human_bytes(result.output_bytes).c_str());
    return 0;
}

int cmd_rotate(const leht::Context& ctx, const Args& args) {
    const int degrees = args.int_flag("-d", 90);
    const auto result = leht::ops::rotate(ctx, require_input(args),
                                          require_output(args),
                                          args.flag("-p"), degrees);
    std::printf("rotated by %d degrees, wrote %d pages, %s\n", degrees,
                result.pages_written, human_bytes(result.output_bytes).c_str());
    return 0;
}

int cmd_split(const leht::Context& ctx, const Args& args) {
    const auto written = leht::ops::split(ctx, require_input(args),
                                          require_output(args),
                                          args.int_flag("-n", 1));
    for (const std::string& path : written) {
        std::printf("  %s\n", path.c_str());
    }
    std::printf("wrote %zu file%s\n", written.size(),
                written.size() == 1 ? "" : "s");
    return 0;
}

int cmd_encrypt(const leht::Context& ctx, const Args& args) {
    const std::string input = require_input(args);
    const std::string output = require_output(args);

    leht::ops::EncryptOptions options;
    options.user_password = args.flag("--user-pw");
    options.owner_password = args.flag("--owner-pw");
    options.method = parse_method(args.flag("--method"));

    leht::ops::encrypt(ctx, input, output, options);
    std::printf("wrote encrypted %s\n", output.c_str());
    if (options.user_password.empty()) {
        std::printf("  note: no user password, so anyone can open it. The "
                    "permission flags are advisory only.\n");
    }
    return 0;
}

int cmd_decrypt(const leht::Context& ctx, const Args& args) {
    leht::ops::decrypt(ctx, require_input(args), require_output(args),
                       args.flag("--password"));
    std::printf("wrote decrypted %s\n", args.flag("-o").c_str());
    return 0;
}

/// Parses "P:X0,Y0,X1,Y1" (1-based page, points from the top-left).
std::pair<int, leht::Rect> parse_rect(const std::string& spec) {
    const auto fail = [&]() -> std::pair<int, leht::Rect> {
        throw leht::Error(0, "--rect expects PAGE:X0,Y0,X1,Y1, got '" + spec + "'");
    };
    const std::size_t colon = spec.find(':');
    if (colon == std::string::npos) {
        return fail();
    }
    char* end = nullptr;
    errno = 0;
    const long page = std::strtol(spec.c_str(), &end, 10);
    if (end != spec.c_str() + colon || errno == ERANGE || page < 1 || page > INT_MAX) {
        return fail();
    }
    float v[4];
    const char* p = spec.c_str() + colon + 1;
    for (int i = 0; i < 4; ++i) {
        const double d = std::strtod(p, &end);
        if (end == p || !std::isfinite(d) || *end != (i < 3 ? ',' : '\0')) {
            return fail();
        }
        v[i] = static_cast<float>(d);
        p = end + 1;
    }
    const leht::Rect r{v[0], v[1], v[2], v[3]};
    if (r.empty()) {
        throw leht::Error(0, "--rect box encloses nothing: '" + spec + "'");
    }
    return {static_cast<int>(page) - 1, r};
}

leht::ops::RedactImages parse_images(const std::string& name) {
    if (name.empty() || name == "pixels") { return leht::ops::RedactImages::Pixels; }
    if (name == "remove")                 { return leht::ops::RedactImages::Remove; }
    if (name == "keep")                   { return leht::ops::RedactImages::Keep; }
    throw leht::Error(0, "unknown --images mode '" + name +
                             "' (expected pixels, remove or keep)");
}

/// Exit status when the output was written but the term still appears
/// somewhere redaction could not reach: a script must not mistake that for a
/// clean result.
constexpr int kExitRemaining = 3;

int cmd_redact(const leht::Context& ctx, const Args& args) {
    const std::string input = require_input(args);
    const std::string output = require_output(args);
    const std::string term = args.flag("--text");
    const std::vector<std::string> rects = args.values("--rect");
    if (term.empty() && rects.empty()) {
        throw leht::Error(0, "redact needs --text TERM or --rect PAGE:BOX");
    }
    if (!args.flag("-p").empty() && term.empty()) {
        throw leht::Error(0, "-p limits --text; with --rect, name the page in the box");
    }

    leht::ops::RedactOptions options;
    options.images = parse_images(args.flag("--images"));
    options.black_boxes = !args.has_switch("--no-boxes");

    // Parse everything before touching the document.
    std::map<int, std::vector<leht::Rect>> boxes;
    for (const std::string& spec : rects) {
        const auto [page, rect] = parse_rect(spec);
        boxes[page].push_back(rect);
    }

    leht::Document doc = leht::Document::open(ctx, input);
    if (doc.needs_password()) {
        throw leht::Error(0, "document is encrypted; decrypt it first");
    }

    leht::ops::RedactResult total;
    std::vector<int> pages;
    const auto add = [&](const leht::ops::RedactResult& r) {
        total.areas += r.areas;
        total.annotations_removed += r.annotations_removed;
        total.structure_dropped = total.structure_dropped || r.structure_dropped;
        pages.insert(pages.end(), r.pages.begin(), r.pages.end());
    };
    for (const auto& [page, areas] : boxes) {
        add(leht::ops::redact(ctx, doc, page, areas, options));
    }
    if (!term.empty()) {
        const auto r = leht::ops::redact_text(ctx, doc, term, args.flag("-p"), options);
        add(r);
        total.remaining = r.remaining;
    }
    std::sort(pages.begin(), pages.end());
    pages.erase(std::unique(pages.begin(), pages.end()), pages.end());

    doc.save(output, leht::SaveOptions{});

    std::printf("redacted %d area%s on %zu page%s -> %s\n", total.areas,
                total.areas == 1 ? "" : "s", pages.size(), pages.size() == 1 ? "" : "s",
                output.c_str());
    if (total.annotations_removed > 0) {
        std::printf("  removed %d overlapping annotation%s or form field%s\n",
                    total.annotations_removed,
                    total.annotations_removed == 1 ? "" : "s",
                    total.annotations_removed == 1 ? "" : "s");
    }
    if (total.structure_dropped) {
        std::printf("  note: dropped the structure tree (tagged-PDF accessibility), "
                    "since it can repeat page text\n");
    }
    if (!term.empty() && total.areas == 0) {
        std::printf("  no occurrences of \"%s\" on the page%s searched\n", term.c_str(),
                    args.flag("-p").empty() ? "s" : "");
    }
    if (!total.remaining.empty()) {
        std::fprintf(stderr, "leht: warning: \"%s\" still appears in:\n", term.c_str());
        for (const std::string& where : total.remaining) {
            std::fprintf(stderr, "  - %s\n", where.c_str());
        }
        return kExitRemaining;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fputs(kUsage, stderr);
        return 2;
    }

    try {
        const Args args = parse(argc, argv);
        const std::string& cmd = args.command;

        if (cmd == "-h" || cmd == "--help" || cmd == "help") {
            std::fputs(kUsage, stdout);
            return 0;
        }
        if (cmd == "--version" || cmd == "version") {
            std::printf("leht 0.1.0\n");
            return 0;
        }

        leht::Context ctx;

        if (cmd == "info")     { return cmd_info(ctx, args); }
        if (cmd == "text")     { return cmd_text(ctx, args); }
        if (cmd == "render")   { return cmd_render(ctx, args); }
        if (cmd == "merge")    { return cmd_merge(ctx, args); }
        if (cmd == "compress") { return cmd_compress(ctx, args); }
        if (cmd == "extract")  { return cmd_extract(ctx, args); }
        if (cmd == "remove")   { return cmd_remove(ctx, args); }
        if (cmd == "rotate")   { return cmd_rotate(ctx, args); }
        if (cmd == "split")    { return cmd_split(ctx, args); }
        if (cmd == "encrypt")  { return cmd_encrypt(ctx, args); }
        if (cmd == "decrypt")  { return cmd_decrypt(ctx, args); }
        if (cmd == "redact")   { return cmd_redact(ctx, args); }

        std::fprintf(stderr, "leht: unknown command '%s'\n\n", cmd.c_str());
        std::fputs(kUsage, stderr);
        return 2;
    } catch (const leht::Error& e) {
        std::fprintf(stderr, "leht: %s\n", e.what());
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "leht: %s\n", e.what());
        return 1;
    }
}
