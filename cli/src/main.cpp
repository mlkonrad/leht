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
#include "leht/ops/crop.hpp"
#include "leht/ops/encrypt.hpp"
#include "leht/ops/forms.hpp"
#include "leht/ops/annotate.hpp"
#include "leht/ops/merge.hpp"
#include "leht/ops/pages.hpp"
#include "leht/ops/redact.hpp"
#include "leht/ops/sign.hpp"
#include "leht/ops/watermark.hpp"
#include "leht/text.hpp"
#include "leht/renderer.hpp"
#include "leht/crypto/crypto.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iostream>
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
    "  crop      FILE -o OUT.pdf (--box X0,Y0,X1,Y1 | --margins N[,T,R,B]) [-p RANGES]\n"
    "            hides, does not remove: the rest of the page stays in the file\n"
    "  watermark FILE -o OUT.pdf --text TEXT [-p RANGES] [--opacity F] [--angle DEG]\n"
    "            [--size PT] [--color RRGGBB] [--under]\n"
    "  annots    FILE                         list annotations, with their ids\n"
    "  annotate  FILE -o OUT.pdf [--highlight|--underline|--strike TEXT]\n"
    "            [--note P:X,Y:TEXT]... [--freetext P:BOX:TEXT]... [--stamp P:NAME[:BOX]]...\n"
    "            [--move ID:BOX]... [--set-text ID:TEXT]... [--delete ID]...\n"
    "            [--author NAME] [--color RRGGBB] [--size PT]\n"
    "  form      FILE                         list form fields and their values\n"
    "  fill      FILE -o OUT.pdf NAME=VALUE... [--flatten]\n"
    "            fill form fields; never runs the document's JavaScript\n"
    "  sign      FILE -o OUT.pdf (--p12 ID.p12 | --pkcs11 URI|auto)\n"
    "            [--field NAME | --box P:X0,Y0,X1,Y1] [--image IMG] [--name N]\n"
    "            [--reason R] [--location L] [--tsa URL]\n"
    "            sign with a key in a PKCS#12 file or on an ID card (PAdES); the\n"
    "            original bytes are kept and the signature appended, so earlier\n"
    "            signatures stay valid\n"
    "  keys      [--pkcs11-module LIB]        list the signing keys on ID cards and\n"
    "            other PKCS#11 tokens, with the URI --pkcs11 takes\n"
    "  verify    FILE [--trust CA.pem]... [--json]\n"
    "            check every signature: exits 4 broken, 5 untrusted, 6 changed after\n"
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
    "  --box BOX      crop to this box: points from the page's top-left\n"
    "  --margins M    trim M points from every edge, or LEFT,TOP,RIGHT,BOTTOM\n"
    "  --opacity F    watermark opacity, above 0 up to 1 (default 0.15)\n"
    "  --angle DEG    watermark angle, counter-clockwise (default 45)\n"
    "  --size PT      watermark font size; 0 fits the page (default 0)\n"
    "  --under        draw the watermark beneath the page content\n"
    "  --note P:X,Y:TEXT  a sticky note on page P at X,Y (points from top-left)\n"
    "  --freetext P:BOX:TEXT  text written on page P inside BOX, at --size points\n"
    "                 (default 12)\n"
    "  --move ID:BOX  move annotation ID (see annots) so its bounds become BOX; the\n"
    "                 appearance is kept, and highlights cannot move off their text\n"
    "  --set-text ID:TEXT  new words for a free-text annotation or a note\n"
    "  --flatten      bake fields into the page so they can no longer be edited\n"
    "  --stamp P:NAME[:BOX]  a stamp: Approved, Draft, Confidential, Final,\n"
    "                 NotApproved, ForComment, TopSecret, ...; top-right by default\n"
    "  --stamp-image P:IMG:BOX  a picture stamped on page P -- a scanned signature,\n"
    "                 say. It is a picture, not a digital signature; use sign for that\n"
    "  --p12 FILE     PKCS#12 (.p12/.pfx) holding the signing key and certificate\n"
    "  --pkcs11 URI   sign with this key on an ID card or other token (from leht\n"
    "                 keys); auto takes the card's one signing key. For an Estonian\n"
    "                 ID card that is the PIN2 key\n"
    "  --pkcs11-module LIB  use this PKCS#11 library instead of the ones the system\n"
    "                 has registered with p11-kit (OpenSC registers itself)\n"
    "  --password-fd N  read the password or PIN from this descriptor, one line.\n"
    "                 Without it, leht asks on the terminal. NEVER pass one as an\n"
    "                 argument: /proc shows it to every process on the machine\n"
    "  --field NAME   sign this existing, empty signature field\n"
    "  --box P:BOX    place a new visible signature here; otherwise it is invisible\n"
    "  --image IMG    a picture for the signature to show (PNG or JPEG)\n"
    "  --tsa URL      timestamp the signature with this RFC 3161 authority (B-T)\n"
    "  --trust FILE   also trust the certificates in this PEM file, on top of the\n"
    "                 system's\n"
    "  --json         verify: machine-readable output\n"
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
        "--text", "--rect", "--images", "--box", "--margins", "--opacity",
        "--angle", "--size", "--color", "--highlight", "--underline", "--strike",
        "--note", "--stamp", "--delete", "--author", "--move", "--set-text", "--freetext",
        "--p12", "--password-fd", "--field", "--image", "--name", "--reason",
        "--location", "--tsa", "--trust", "--stamp-image", "--pkcs11", "--pkcs11-module"};
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

/// Parses exactly `n` comma-separated finite numbers, or returns false.
bool parse_floats(const char* p, int n, float* out) {
    for (int i = 0; i < n; ++i) {
        char* end = nullptr;
        const double d = std::strtod(p, &end);
        if (end == p || !std::isfinite(d) || *end != (i < n - 1 ? ',' : '\0')) {
            return false;
        }
        out[i] = static_cast<float>(d);
        p = end + 1;
    }
    return true;
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
    if (!parse_floats(spec.c_str() + colon + 1, 4, v)) {
        return fail();
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
        total.signatures_invalidated =
            std::max(total.signatures_invalidated, r.signatures_invalidated);
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
    if (total.signatures_invalidated > 0) {
        std::fprintf(stderr,
                     "leht: warning: this breaks the document's %d signature%s: a redacted "
                     "file is rewritten in full, without the revisions they sign\n",
                     total.signatures_invalidated,
                     total.signatures_invalidated == 1 ? "" : "s");
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

int cmd_crop(const leht::Context& ctx, const Args& args) {
    const std::string input = require_input(args);
    const std::string output = require_output(args);
    const std::string box = args.flag("--box");
    const std::string margins = args.flag("--margins");
    if (box.empty() == margins.empty()) {
        throw leht::Error(0, "crop needs exactly one of --box or --margins");
    }

    leht::Document doc = leht::Document::open(ctx, input);
    int pages = 0;
    if (!box.empty()) {
        float v[4];
        if (!parse_floats(box.c_str(), 4, v)) {
            throw leht::Error(0, "--box expects X0,Y0,X1,Y1, got '" + box + "'");
        }
        pages = leht::ops::crop(ctx, doc, args.flag("-p"), leht::Rect{v[0], v[1], v[2], v[3]});
    } else {
        float v[4];
        leht::ops::Margins m;
        if (parse_floats(margins.c_str(), 1, v)) {
            m = {v[0], v[0], v[0], v[0]};
        } else if (parse_floats(margins.c_str(), 4, v)) {
            m = {v[0], v[1], v[2], v[3]};
        } else {
            throw leht::Error(0, "--margins expects N or LEFT,TOP,RIGHT,BOTTOM, got '" +
                                     margins + "'");
        }
        pages = leht::ops::crop_margins(ctx, doc, args.flag("-p"), m);
    }
    doc.save(output, leht::SaveOptions{});
    std::printf("cropped %d page%s -> %s\n", pages, pages == 1 ? "" : "s", output.c_str());
    std::printf("  note: cropping hides the rest of the page; it is still in the file. "
                "Use 'leht redact' to remove content.\n");
    return 0;
}

/// Parses "RRGGBB" (an optional leading '#') into 0-1 components.
void parse_color(const std::string& text, float out[3]) {
    const std::string hex = !text.empty() && text[0] == '#' ? text.substr(1) : text;
    if (hex.size() != 6 || hex.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
        throw leht::Error(0, "--color expects RRGGBB, got '" + text + "'");
    }
    for (int i = 0; i < 3; ++i) {
        const auto byte = std::strtol(hex.substr(static_cast<std::size_t>(i) * 2, 2).c_str(),
                                      nullptr, 16);
        out[i] = static_cast<float>(byte) / 255.0F;
    }
}

int cmd_watermark(const leht::Context& ctx, const Args& args) {
    const std::string input = require_input(args);
    const std::string output = require_output(args);

    leht::ops::WatermarkOptions options;
    options.text = args.flag("--text");
    if (options.text.empty()) {
        throw leht::Error(0, "watermark needs --text");
    }
    options.opacity = args.float_flag("--opacity", options.opacity);
    options.angle = args.float_flag("--angle", options.angle);
    options.font_size = args.float_flag("--size", options.font_size);
    options.under = args.has_switch("--under");
    if (!args.flag("--color").empty()) {
        parse_color(args.flag("--color"), options.color);
    }

    leht::Document doc = leht::Document::open(ctx, input);
    const int pages = leht::ops::watermark(ctx, doc, args.flag("-p"), options);
    doc.save(output, leht::SaveOptions{});
    std::printf("watermarked %d page%s -> %s\n", pages, pages == 1 ? "" : "s",
                output.c_str());
    return 0;
}

const char* kind_name(leht::ops::AnnotKind k) {
    switch (k) {
        case leht::ops::AnnotKind::Highlight: return "highlight";
        case leht::ops::AnnotKind::Underline: return "underline";
        default: return "strike-out";
    }
}

int cmd_annots(const leht::Context& ctx, const Args& args) {
    leht::Document doc = leht::Document::open(ctx, require_input(args));
    const auto all = leht::ops::list_annotations(ctx, doc);
    if (all.empty()) {
        std::printf("no annotations\n");
        return 0;
    }
    std::printf("%6s %5s  %-10s %s\n", "id", "page", "type", "text");
    for (const auto& a : all) {
        std::string text = a.contents;
        for (char& ch : text) {
            if (ch == '\n' || ch == '\r') {
                ch = ' ';
            }
        }
        if (text.size() > 60) {
            text = text.substr(0, 57) + "...";
        }
        const std::string author = a.author.empty() ? "" : "[" + a.author + "] ";
        std::printf("%6d %5d  %-10s %s%s\n", a.id, a.page + 1, a.type.c_str(), author.c_str(),
                    text.c_str());
    }
    return 0;
}

// --- signing -------------------------------------------------------------

/// Exit codes for `verify`, so a script can tell apart the three ways a
/// signature can fail to mean what a reader hopes it means.
constexpr int kExitBroken = 4;     ///< a signature does not verify
constexpr int kExitUntrusted = 5;  ///< it verifies, but the signer is not trusted
constexpr int kExitChanged = 6;    ///< it verifies and is trusted, but the file grew after it

std::vector<std::uint8_t> read_bytes(const std::string& path, std::size_t limit) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        throw leht::Error(0, "cannot read " + path + ": " + std::strerror(errno));
    }
    std::vector<std::uint8_t> out;
    std::array<std::uint8_t, 65536> buf{};
    std::size_t n = 0;
    while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) {
        out.insert(out.end(), buf.begin(), buf.begin() + static_cast<long>(n));
        if (out.size() > limit) {
            std::fclose(f);
            throw leht::Error(0, path + " is larger than this command expects");
        }
    }
    std::fclose(f);
    return out;
}

/// The PKCS#12 password or a card's PIN. NEVER from the command line: an
/// argument is visible in /proc to every process on the machine, and lands in
/// shell history. From --password-fd when given (one line), else prompted on
/// the terminal with echo off, else read from stdin when that is a pipe.
leht::crypto::Secret read_password(const Args& args, const char* prompt = "PKCS#12 password: ") {
    std::string line;
    const std::string fd_flag = args.flag("--password-fd");
    int fd = -1;
    if (!fd_flag.empty()) {
        fd = args.int_flag("--password-fd", -1);
        if (fd < 0) {
            throw leht::Error(0, "--password-fd expects a file descriptor number");
        }
    } else if (::isatty(STDIN_FILENO) == 0) {
        fd = STDIN_FILENO;
    }
    if (fd >= 0) {
        char ch = 0;
        ssize_t got = 0;
        while ((got = ::read(fd, &ch, 1)) == 1 && ch != '\n') {
            line.push_back(ch);
            if (line.size() > 1024) {
                break;
            }
        }
        if (got < 0) {
            throw leht::Error(0, std::string("cannot read the password: ") + std::strerror(errno));
        }
        return leht::crypto::Secret{std::move(line)};
    }

    termios old{};
    const bool tty = ::tcgetattr(STDIN_FILENO, &old) == 0;
    if (tty) {
        termios quiet = old;
        quiet.c_lflag &= static_cast<tcflag_t>(~ECHO);
        (void)::tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet);
    }
    std::fputs(prompt, stderr);
    std::getline(std::cin, line);
    if (tty) {
        (void)::tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
        std::fputs("\n", stderr);
    }
    return leht::crypto::Secret{std::move(line)};
}

std::string trust_word(leht::crypto::Trust t) {
    switch (t) {
        case leht::crypto::Trust::Trusted:     return "trusted";
        case leht::crypto::Trust::Untrusted:   return "not trusted";
        case leht::crypto::Trust::Expired:     return "certificate expired";
        case leht::crypto::Trust::NotYetValid: return "certificate not yet valid";
        case leht::crypto::Trust::Unknown:     return "not checked";
    }
    return "not checked";
}

std::string local_time(std::int64_t t) {
    const auto tt = static_cast<std::time_t>(t);
    std::tm tm{};
    gmtime_r(&tt, &tm);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &tm);
    return buf;
}

/// JSON string escaping, enough for the fields verify --json prints.
std::string json_string(const std::string& s) {
    std::string out = "\"";
    for (const char raw : s) {
        const auto ch = static_cast<unsigned char>(raw);
        switch (ch) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (ch < 0x20) {
                    char esc[8];
                    std::snprintf(esc, sizeof(esc), "\\u%04x", ch);
                    out += esc;
                } else {
                    out += static_cast<char>(ch);
                }
        }
    }
    return out + "\"";
}

leht::ops::Appearance appearance_from(const Args& args) {
    leht::ops::Appearance a;
    const std::string image = args.flag("--image");
    if (!image.empty()) {
        // 64 MB: a signature graphic is a photograph at worst.
        a.image = read_bytes(image, std::size_t{64} << 20);
    }
    return a;
}

std::string day(std::int64_t t) {
    const auto tt = static_cast<std::time_t>(t);
    std::tm tm{};
    gmtime_r(&tt, &tm);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

std::string pin_state(const leht::crypto::TokenKey& k) {
    if (k.pin_locked) {
        return "blocked -- unblock it with the PUK";
    }
    if (k.pin_final_try) {
        return "one more wrong PIN blocks it";
    }
    if (k.pin_count_low) {
        return "entered wrongly before";
    }
    return k.pinpad ? "entered on the reader" : "typed here";
}

int cmd_keys(const Args& args) {
    const auto keys = leht::crypto::list_token_keys(args.flag("--pkcs11-module"));
    if (keys.empty()) {
        std::fprintf(stderr, "leht: no signing keys found. Is a card reader connected, and the "
                             "card in it?\n");
        return 1;
    }
    for (const leht::crypto::TokenKey& k : keys) {
        std::printf("%s\n", k.uri.c_str());
        std::printf("  token:   %s\n", k.token_label.c_str());
        std::printf("  signer:  %s\n", k.cert.subject.c_str());
        std::printf("  use:     %s\n", k.non_repudiation ? "signing (nonRepudiation)"
                                                         : "authentication, not signing");
        std::printf("  valid:   %s to %s\n", day(k.cert.not_before).c_str(),
                    day(k.cert.not_after).c_str());
        std::printf("  PIN:     %s\n", pin_state(k).c_str());
    }
    return 0;
}

/// `--pkcs11 auto`: the card's signing key, when there is exactly one.
std::string auto_key_uri(const std::string& module) {
    const auto keys = leht::crypto::list_token_keys(module);
    std::vector<const leht::crypto::TokenKey*> signing;
    for (const auto& k : keys) {
        if (k.non_repudiation) {
            signing.push_back(&k);
        }
    }
    if (signing.empty() && keys.size() == 1) {
        signing.push_back(&keys.front());
    }
    if (signing.size() == 1) {
        return signing.front()->uri;
    }
    if (keys.empty()) {
        throw leht::Error(0, "no signing keys found. Is a card reader connected, and the card "
                             "in it?");
    }
    throw leht::Error(0, "more than one key could sign; choose one with --pkcs11 URI (leht keys "
                         "lists them)");
}

leht::crypto::Identity identity_from(const Args& args) {
    const std::string p12_path = args.flag("--p12");
    std::string uri = args.flag("--pkcs11");
    if (p12_path.empty() == uri.empty()) {
        throw leht::Error(0, "sign needs either --p12 ID.p12 (a key in a file) or --pkcs11 "
                             "URI|auto (a key on an ID card or other token)");
    }
    // The key and its password never leave this process.
    if (!p12_path.empty()) {
        return leht::crypto::Identity::from_pkcs12(read_bytes(p12_path, std::size_t{16} << 20),
                                                   read_password(args));
    }
    const std::string module = args.flag("--pkcs11-module");
    if (uri == "auto") {
        uri = auto_key_uri(module);
    }
    return leht::crypto::Identity::from_pkcs11(
        uri,
        [&](const leht::crypto::TokenKey& k) {
            const std::string who =
                k.cert.common_name.empty() ? k.token_label : k.cert.common_name;
            if (k.pin_final_try) {
                std::fprintf(stderr, "leht: careful: one more wrong PIN blocks it\n");
            } else if (k.pin_count_low) {
                std::fprintf(stderr, "leht: note: a wrong PIN was entered before\n");
            }
            if (k.pinpad) {
                std::fprintf(stderr, "Enter the PIN for %s (%s) on the reader's keypad.\n",
                             who.c_str(), k.token_label.c_str());
                return leht::crypto::Secret{};
            }
            const std::string prompt = "PIN for " + who + " (" + k.token_label + "): ";
            return read_password(args, prompt.c_str());
        },
        module);
}

int cmd_sign(const leht::Context& ctx, const Args& args) {
    const std::string input = require_input(args);
    const std::string output = require_output(args);
    std::error_code ec;
    if (fs::exists(output, ec) && fs::equivalent(input, output, ec)) {
        throw leht::Error(0, "sign will not write over its input; give -o another path");
    }

    leht::ops::SignatureRequest request;
    request.field = args.flag("--field");
    request.name = args.flag("--name");
    request.reason = args.flag("--reason");
    request.location = args.flag("--location");
    request.appearance = appearance_from(args);
    const std::string box = args.flag("--box");
    const bool invisible = args.has_switch("--invisible") || (box.empty() && request.field.empty());
    if (!box.empty()) {
        if (!request.field.empty()) {
            throw leht::Error(0, "--box places a new signature; --field signs an existing one");
        }
        const auto [page, rect] = parse_rect(box);
        request.page = page;
        request.rect = rect;
    }
    if (invisible && !args.flag("--image").empty()) {
        throw leht::Error(0, "--image needs somewhere to be drawn: add --box PAGE:X0,Y0,X1,Y1");
    }

    leht::crypto::SignOptions options;
    options.tsa_url = args.flag("--tsa");

    const leht::crypto::Identity identity = identity_from(args);
    const leht::crypto::CertInfo cert = identity.certificate();
    if (request.name.empty()) {
        request.name = cert.common_name;
    }
    request.reserve = leht::crypto::estimate_signature_size(identity, options);

    leht::Document doc = leht::Document::open(ctx, input);
    if (doc.needs_password()) {
        throw leht::Error(0, "document is encrypted; decrypt it first");
    }
    if (!doc.can_save_incrementally()) {
        // A repaired file has no revision to append to, so there is nothing a
        // signature could keep intact. Rewrite it once, then sign that.
        std::fprintf(stderr, "leht: note: this file had to be repaired when opened, so it is "
                             "rewritten in full before signing\n");
        doc.save(output, leht::SaveOptions{leht::SaveOptions::Mode::Full});
        doc = leht::Document::open(ctx, output);
    }

    // Write beside the target, then rename: the same atomic save the rest of
    // leht does, with the signature filled in before the file appears.
    const fs::path target{output};
    const fs::path dir = target.has_parent_path() ? target.parent_path() : fs::path{"."};
    std::string temp = (dir / ("." + target.filename().string() + ".leht-XXXXXX")).string();
    const int fd = ::mkostemp(temp.data(), O_CLOEXEC);
    if (fd < 0) {
        throw leht::Error(0, "cannot create a file beside " + output + ": " +
                                 std::strerror(errno));
    }
    leht::crypto::SignResult result;
    std::string field;
    try {
        const auto prepared = leht::ops::prepare_signature(ctx, doc, request, fd);
        field = prepared.field;
        result = leht::crypto::sign_prepared(fd, prepared.range, identity, options);
        // mkstemp creates 0600; give the signed file what a new file would get.
        // umask(2) can only be read by setting it, which is safe here: the CLI
        // is single-threaded and creates nothing else meanwhile.
        const mode_t mask = ::umask(022);
        (void)::umask(mask);
        (void)::fchmod(fd, 0666 & ~mask);
        if (::fsync(fd) != 0) {
            throw leht::Error(0, std::string("cannot flush the signed file: ") +
                                     std::strerror(errno));
        }
    } catch (...) {
        ::close(fd);
        ::unlink(temp.c_str());
        throw;
    }
    ::close(fd);
    if (::rename(temp.c_str(), output.c_str()) != 0) {
        const int err = errno;
        ::unlink(temp.c_str());
        throw leht::Error(0, "cannot write " + output + ": " + std::strerror(err));
    }

    std::printf("signed %s -> %s\n", input.c_str(), output.c_str());
    std::printf("  signer:    %s\n", cert.subject.c_str());
    std::printf("  field:     %s (%s)\n", field.c_str(),
                invisible ? "invisible" : "visible");
    std::printf("  digest:    %s, %zu bytes of signature in a %zu byte slot\n",
                result.digest.c_str(), result.der_size, result.hole_size);
    if (result.timestamp) {
        std::printf("  timestamp: %s (%s)\n", local_time(*result.timestamp).c_str(),
                    options.tsa_url.c_str());
    } else {
        std::printf("  timestamp: none (PAdES B-B). --tsa URL adds one, which is what\n"
                    "             proves the signature existed before the certificate expired\n");
    }
    const auto now = static_cast<std::int64_t>(std::time(nullptr));
    if (cert.not_after != 0 && cert.not_after < now) {
        std::fprintf(stderr, "leht: warning: the signing certificate expired on %s\n",
                     local_time(cert.not_after).c_str());
    }
    return 0;
}

int cmd_verify(const leht::Context& ctx, const Args& args) {
    const std::string input = require_input(args);
    const bool json = args.has_switch("--json");

    leht::crypto::TrustStore trust = leht::crypto::TrustStore::system();
    for (const std::string& path : args.values("--trust")) {
        const auto bytes = read_bytes(path, std::size_t{16} << 20);
        trust.add_pem(std::string(bytes.begin(), bytes.end()));
    }

    leht::Document doc = leht::Document::open(ctx, input);
    if (doc.needs_password()) {
        throw leht::Error(0, "document is encrypted; decrypt it first");
    }
    const auto signatures = leht::ops::list_signatures(ctx, doc);
    if (signatures.empty()) {
        if (json) {
            std::printf("{\"file\":%s,\"signatures\":[]}\n", json_string(input).c_str());
        } else {
            std::printf("%s: no signatures\n", input.c_str());
        }
        return 0;
    }

    int worst = 0;
    std::string rows;
    for (std::size_t i = 0; i < signatures.size(); ++i) {
        const leht::ops::SignatureInfo& s = signatures[i];
        leht::crypto::CmsReport r;
        if (s.range_ok) {
            r = leht::crypto::verify_cms(s.contents, leht::ops::signed_bytes(ctx, doc, s.range),
                                         trust);
        }
        const bool intact = s.range_ok && r.intact();
        const bool trusted = intact && r.trust == leht::crypto::Trust::Trusted;
        const bool changed = s.changed_after_signing && !s.later_signature_covers_changes;
        if (!intact) {
            worst = std::max(worst, kExitBroken);
        } else if (!trusted) {
            worst = std::max(worst, kExitUntrusted);
        } else if (changed) {
            worst = std::max(worst, kExitChanged);
        }

        if (json) {
            std::string row = "{\"field\":" + json_string(s.field) +
                              ",\"intact\":" + (intact ? "true" : "false") +
                              ",\"trust\":" + json_string(intact ? trust_word(r.trust) : "not checked") +
                              ",\"signer\":" + json_string(r.signer.subject) +
                              ",\"subfilter\":" + json_string(s.subfilter) +
                              ",\"digest\":" + json_string(r.digest) +
                              ",\"claimed_time\":" + json_string(s.claimed_time) +
                              ",\"changed_after_signing\":" + (s.changed_after_signing ? "true" : "false") +
                              ",\"later_signature_covers_changes\":" +
                              (s.later_signature_covers_changes ? "true" : "false") +
                              ",\"page\":" + std::to_string(s.page + 1);
            if (r.timestamp) {
                row += ",\"timestamp\":{\"valid\":" + std::string(r.timestamp->valid ? "true" : "false") +
                       ",\"time\":" + std::to_string(r.timestamp->time) +
                       ",\"authority\":" + json_string(r.timestamp->authority.subject) +
                       ",\"trust\":" + json_string(trust_word(r.timestamp->trust)) + "}";
            }
            const std::string problem = !s.range_ok ? s.range_problem : r.problem;
            if (!problem.empty()) {
                row += ",\"problem\":" + json_string(problem);
            }
            rows += (rows.empty() ? "" : ",") + row + "}";
            continue;
        }

        std::printf("%s: signature %zu of %zu (field %s%s)\n", input.c_str(), i + 1,
                    signatures.size(), s.field.c_str(),
                    s.page >= 0 ? (", page " + std::to_string(s.page + 1)).c_str() : ", invisible");
        if (!s.range_ok) {
            std::printf("  %-10s %s\n", "BROKEN", s.range_problem.c_str());
            continue;
        }
        std::printf("  %-10s %s\n", intact ? "intact" : "BROKEN",
                    intact ? "the signed bytes are exactly what was signed"
                           : r.problem.c_str());
        std::printf("  %-10s %s\n", "signer", r.signer.subject.c_str());
        std::printf("  %-10s %s%s%s\n", "trust", trust_word(r.trust).c_str(),
                    r.trust_detail.empty() ? "" : ": ", r.trust_detail.c_str());
        std::printf("  %-10s %s, %s\n", "algorithm", r.digest.c_str(), s.subfilter.c_str());
        if (!s.claimed_time.empty()) {
            std::printf("  %-10s %s (the signer's own clock)\n", "claimed",
                        s.claimed_time.c_str());
        }
        if (r.timestamp) {
            std::printf("  %-10s %s %s, by %s (%s)\n", "timestamp",
                        r.timestamp->valid ? "verified" : "NOT VALID:",
                        r.timestamp->valid ? local_time(r.timestamp->time).c_str()
                                           : r.timestamp->problem.c_str(),
                        r.timestamp->authority.common_name.c_str(),
                        trust_word(r.timestamp->trust).c_str());
        } else if (intact) {
            std::printf("  %-10s none: nothing proves when this was signed\n", "timestamp");
        }
        if (!s.name.empty() || !s.reason.empty() || !s.location.empty()) {
            std::printf("  %-10s %s%s%s%s%s\n", "says", s.name.c_str(),
                        s.reason.empty() ? "" : " -- ", s.reason.c_str(),
                        s.location.empty() ? "" : " -- ", s.location.c_str());
        }
        if (s.changed_after_signing) {
            std::printf("  %-10s the document was added to after this was signed%s\n", "CHANGED",
                        s.later_signature_covers_changes
                            ? "; a later signature covers those bytes too"
                            : "");
        }
        if (!r.problem.empty() && intact) {
            std::printf("  %-10s %s\n", "note", r.problem.c_str());
        }
    }
    if (json) {
        std::printf("{\"file\":%s,\"signatures\":[%s]}\n", json_string(input).c_str(),
                    rows.c_str());
    }
    return worst;
}


/// Splits "P:REST" into a 0-based page and REST.
std::pair<int, std::string> page_prefix(const std::string& spec, const char* flag) {
    const std::size_t colon = spec.find(':');
    char* end = nullptr;
    errno = 0;
    const long page = std::strtol(spec.c_str(), &end, 10);
    if (colon == std::string::npos || end != spec.c_str() + colon || errno == ERANGE ||
        page < 1 || page > INT_MAX) {
        throw leht::Error(0, std::string(flag) + " must start with a page number and ':', got '" +
                                 spec + "'");
    }
    return {static_cast<int>(page) - 1, spec.substr(colon + 1)};
}

int cmd_annotate(const leht::Context& ctx, const Args& args) {
    using leht::ops::AnnotKind;
    const std::string input = require_input(args);
    const std::string output = require_output(args);

    leht::ops::AnnotSpec base;
    base.author = args.flag("--author");
    const bool custom_color = !args.flag("--color").empty();
    if (custom_color) {
        parse_color(args.flag("--color"), base.color);
    }

    // Parse and check everything before touching the document.
    struct Mark {
        AnnotKind kind;
        std::string text;
    };
    std::vector<Mark> marks;
    for (const auto& [flag, kind] : {std::pair{"--highlight", AnnotKind::Highlight},
                                     std::pair{"--underline", AnnotKind::Underline},
                                     std::pair{"--strike", AnnotKind::StrikeOut}}) {
        for (const std::string& text : args.values(flag)) {
            if (text.empty()) {
                throw leht::Error(0, std::string(flag) + " needs some text to find");
            }
            marks.push_back({kind, text});
        }
    }
    std::vector<std::pair<int, leht::ops::AnnotSpec>> adds;
    for (const std::string& spec : args.values("--note")) {
        const auto [page, rest] = page_prefix(spec, "--note");
        const std::size_t colon = rest.find(':');
        float xy[2];
        if (colon == std::string::npos || !parse_floats(rest.substr(0, colon).c_str(), 2, xy)) {
            throw leht::Error(0, "--note expects PAGE:X,Y:TEXT, got '" + spec + "'");
        }
        leht::ops::AnnotSpec note = base;
        note.kind = AnnotKind::Note;
        note.rect = leht::Rect{xy[0], xy[1], xy[0], xy[1]};
        note.contents = rest.substr(colon + 1);
        adds.emplace_back(page, note);
    }
    std::vector<std::pair<int, std::string>> stamps;
    for (const std::string& spec : args.values("--stamp")) {
        stamps.push_back(page_prefix(spec, "--stamp"));
    }
    // --stamp-image PAGE:PATH:X0,Y0,X1,Y1. The box is split off at the LAST
    // colon, so a path may contain one.
    struct ImageStamp {
        int page;
        std::string path;
        leht::Rect rect;
    };
    std::vector<ImageStamp> image_stamps;
    for (const std::string& spec : args.values("--stamp-image")) {
        const auto [page, rest] = page_prefix(spec, "--stamp-image");
        const std::size_t colon = rest.rfind(':');
        float v[4];
        if (colon == std::string::npos ||
            !parse_floats(rest.c_str() + colon + 1, 4, v)) {
            throw leht::Error(0, "--stamp-image expects PAGE:IMAGE:X0,Y0,X1,Y1, got '" +
                                     spec + "'");
        }
        const leht::Rect rect{v[0], v[1], v[2], v[3]};
        if (rect.empty()) {
            throw leht::Error(0, "--stamp-image box encloses nothing: '" + spec + "'");
        }
        image_stamps.push_back({page, rest.substr(0, colon), rect});
    }
    const auto annot_id = [](const std::string& id, const char* flag) {
        char* end = nullptr;
        errno = 0;
        const long value = std::strtol(id.c_str(), &end, 10);
        if (id.empty() || *end != '\0' || errno == ERANGE || value < 1 || value > INT_MAX) {
            throw leht::Error(0, std::string(flag) + " expects an annotation id, got '" + id +
                                     "'");
        }
        return static_cast<int>(value);
    };
    std::vector<leht::ops::AnnotId> deletes;
    for (const std::string& id : args.values("--delete")) {
        deletes.push_back(annot_id(id, "--delete"));
    }
    // --move ID:X0,Y0,X1,Y1 -- the annotation's new bounds.
    std::vector<std::pair<int, leht::Rect>> moves;
    for (const std::string& spec : args.values("--move")) {
        const std::size_t colon = spec.find(':');
        float v[4];
        if (colon == std::string::npos || !parse_floats(spec.c_str() + colon + 1, 4, v)) {
            throw leht::Error(0, "--move expects ID:X0,Y0,X1,Y1, got '" + spec + "'");
        }
        moves.emplace_back(annot_id(spec.substr(0, colon), "--move"),
                           leht::Rect{v[0], v[1], v[2], v[3]});
    }
    // --set-text ID:TEXT -- new words for free text or a note.
    std::vector<std::pair<int, std::string>> retexts;
    for (const std::string& spec : args.values("--set-text")) {
        const std::size_t colon = spec.find(':');
        if (colon == std::string::npos) {
            throw leht::Error(0, "--set-text expects ID:TEXT, got '" + spec + "'");
        }
        retexts.emplace_back(annot_id(spec.substr(0, colon), "--set-text"),
                             spec.substr(colon + 1));
    }
    // --freetext PAGE:X0,Y0,X1,Y1:TEXT -- text written on the page in a box.
    for (const std::string& spec : args.values("--freetext")) {
        const auto [page, rest] = page_prefix(spec, "--freetext");
        const std::size_t colon = rest.find(':');
        float v[4];
        if (colon == std::string::npos || !parse_floats(rest.substr(0, colon).c_str(), 4, v)) {
            throw leht::Error(0, "--freetext expects PAGE:X0,Y0,X1,Y1:TEXT, got '" + spec + "'");
        }
        leht::ops::AnnotSpec text = base;
        text.kind = AnnotKind::FreeText;
        text.rect = leht::Rect{v[0], v[1], v[2], v[3]};
        text.contents = rest.substr(colon + 1);
        if (!custom_color) {
            text.color[0] = text.color[1] = text.color[2] = 0;  // text is black by default
        }
        text.font_size = args.flag("--size").empty() ? 12 : args.float_flag("--size", 12);
        text.line_width = 0;  // text on the page, not a framed box
        adds.emplace_back(page, text);
    }
    if (marks.empty() && adds.empty() && stamps.empty() && deletes.empty() &&
        image_stamps.empty() && moves.empty() && retexts.empty()) {
        throw leht::Error(0, "annotate needs --highlight, --underline, --strike, --note, "
                             "--freetext, --stamp, --stamp-image, --move, --set-text or "
                             "--delete");
    }

    leht::Document doc = leht::Document::open(ctx, input);
    for (const auto& [page, name_box] : stamps) {
        leht::ops::AnnotSpec stamp = base;
        stamp.kind = AnnotKind::Stamp;
        if (!custom_color) {
            stamp.color[0] = 0.8F;  // stamps are red by convention
            stamp.color[1] = 0.1F;
            stamp.color[2] = 0.1F;
        }
        const std::size_t colon = name_box.find(':');
        stamp.stamp = name_box.substr(0, colon);
        if (colon != std::string::npos) {
            float v[4];
            if (!parse_floats(name_box.c_str() + colon + 1, 4, v)) {
                throw leht::Error(0, "--stamp box expects X0,Y0,X1,Y1");
            }
            stamp.rect = leht::Rect{v[0], v[1], v[2], v[3]};
        } else if (page < doc.page_count()) {
            // Top-right corner, clear of the margin.
            leht::Renderer sizer(ctx, doc);
            const auto size = sizer.page_size(page, 1.0F);
            const auto w = static_cast<float>(size.width);
            stamp.rect = leht::Rect{w - 36 - 180, 36, w - 36, 36 + 54};
        }
        adds.emplace_back(page, stamp);
    }

    int deleted = 0;
    for (const int id : deletes) {
        if (!leht::ops::delete_annotation(ctx, doc, id)) {
            throw leht::Error(0, "no annotation with id " + std::to_string(id) +
                                     " (see 'leht annots')");
        }
        ++deleted;
    }
    int changed = 0;
    for (const auto& [id, rect] : moves) {
        if (!leht::ops::move_annotation(ctx, doc, id, rect)) {
            throw leht::Error(0, "no annotation with id " + std::to_string(id) +
                                     " (see 'leht annots')");
        }
        ++changed;
    }
    for (const auto& [id, text] : retexts) {
        if (!leht::ops::set_annotation_contents(ctx, doc, id, text)) {
            throw leht::Error(0, "no annotation with id " + std::to_string(id) +
                                     " (see 'leht annots')");
        }
        ++changed;
    }
    int added = 0;
    for (const ImageStamp& stamp : image_stamps) {
        leht::ops::Appearance a;
        a.image = read_bytes(stamp.path, std::size_t{64} << 20);
        (void)leht::ops::add_signature_stamp(ctx, doc, stamp.page, stamp.rect, a);
        ++added;
    }
    for (const auto& [page, spec] : adds) {
        (void)leht::ops::add_annotation(ctx, doc, page, spec);
        ++added;
    }
    for (const Mark& m : marks) {
        leht::ops::AnnotSpec spec = base;
        spec.kind = m.kind;
        const auto ids = leht::ops::mark_text(ctx, doc, m.text, args.flag("-p"), spec);
        if (ids.empty()) {
            std::printf("  no occurrences of \"%s\" to %s\n", m.text.c_str(), kind_name(m.kind));
        }
        added += static_cast<int>(ids.size());
    }

    doc.save(output, leht::SaveOptions{});
    std::printf("added %d, changed %d, deleted %d annotation%s -> %s\n", added, changed,
                deleted, added + changed + deleted == 1 ? "" : "s", output.c_str());
    return 0;
}

int cmd_form(const leht::Context& ctx, const Args& args) {
    leht::Document doc = leht::Document::open(ctx, require_input(args));
    const auto fields = leht::ops::list_fields(ctx, doc);
    if (fields.empty()) {
        std::printf("no form fields\n");
        return 0;
    }
    for (const auto& f : fields) {
        std::string flags;
        if (f.read_only) {
            flags += " read-only";
        }
        if (f.required) {
            flags += " required";
        }
        if (f.max_length > 0) {
            flags += " max " + std::to_string(f.max_length);
        }
        std::printf("%-30s %-9s p%-3d = \"%s\"%s\n", f.name.c_str(),
                    leht::ops::field_type_name(f.type), f.page + 1, f.value.c_str(),
                    flags.c_str());
        if (!f.options.empty()) {
            std::string opts;
            for (const std::string& o : f.options) {
                opts += (opts.empty() ? "" : " | ") + o;
            }
            std::printf("%-30s   options: %s\n", "", opts.c_str());
        }
    }
    return 0;
}

int cmd_fill(const leht::Context& ctx, const Args& args) {
    const std::string input = require_input(args);
    const std::string output = require_output(args);
    std::vector<std::pair<std::string, std::string>> values;
    for (std::size_t i = 1; i < args.positional.size(); ++i) {
        const std::string& assignment = args.positional[i];
        const std::size_t eq = assignment.find('=');
        if (eq == std::string::npos || eq == 0) {
            throw leht::Error(0, "expected NAME=VALUE, got '" + assignment + "'");
        }
        values.emplace_back(assignment.substr(0, eq), assignment.substr(eq + 1));
    }
    const bool flat = args.has_switch("--flatten");
    if (values.empty() && !flat) {
        throw leht::Error(0, "fill needs at least one NAME=VALUE (or --flatten)");
    }

    leht::Document doc = leht::Document::open(ctx, input);
    for (const auto& [name, value] : values) {
        leht::ops::set_field(ctx, doc, name, value);
    }
    int flattened = 0;
    if (flat) {
        flattened = leht::ops::flatten(ctx, doc);
    }
    doc.save(output, leht::SaveOptions{});
    std::printf("filled %zu field%s", values.size(), values.size() == 1 ? "" : "s");
    if (flat) {
        std::printf(", flattened %d", flattened);
    }
    std::printf(" -> %s\n", output.c_str());
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
        if (cmd == "crop")     { return cmd_crop(ctx, args); }
        if (cmd == "watermark") { return cmd_watermark(ctx, args); }
        if (cmd == "annots")   { return cmd_annots(ctx, args); }
        if (cmd == "annotate") { return cmd_annotate(ctx, args); }
        if (cmd == "form")     { return cmd_form(ctx, args); }
        if (cmd == "fill")     { return cmd_fill(ctx, args); }
        if (cmd == "sign")     { return cmd_sign(ctx, args); }
        if (cmd == "verify")   { return cmd_verify(ctx, args); }
        if (cmd == "keys")     { return cmd_keys(args); }

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
