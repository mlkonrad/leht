// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The EU trusted lists: ETSI TS 119 612 XML, each signed with an enveloped
// XMLDSig (XAdES) signature. Verified with xmlsec1; read with libxml2.
//
// xmlsec1 checks that a signature is correct over what it references. Whether
// it references the right things is the other half, and the classic way to get
// XML signatures wrong ("signature wrapping"): a valid signature over one part
// of a document, while the reader believes another. So before xmlsec1 runs,
// Leht requires the shape the lists actually have -- one signature, a child of
// the root, covering the whole document -- and reads the data from that same
// document afterwards. See check_shape().
//
// RSA-PSS: Fedora 44's xmlsec1 (1.2.41) has none, and Germany signs its list
// with it. Then xmlsec1 still checks every Reference -- transforms,
// canonicalisation, digests, the hard part -- and Leht checks only the final
// signature over the canonical SignedInfo, with OpenSSL. With xmlsec1 1.3 or
// later that path is not used.
#include "leht/trustlist/xml.hpp"

#include "leht/error.hpp"

#include <libxml/c14n.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <xmlsec/crypto.h>
#include <xmlsec/errors.h>
#include <xmlsec/keys.h>
#include <xmlsec/openssl/evp.h>
#include <xmlsec/transforms.h>
#include <xmlsec/xmldsig.h>
#include <xmlsec/xmlsec.h>
#include <xmlsec/xmltree.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <set>

#if XMLSEC_VERSION_MAJOR > 1 || (XMLSEC_VERSION_MAJOR == 1 && XMLSEC_VERSION_MINOR >= 3)
#define LEHT_XMLSEC_HAS_PSS 1
#endif

namespace leht::trustlist {

namespace {

// --- names -----------------------------------------------------------------

constexpr const char* kDs = "http://www.w3.org/2000/09/xmldsig#";
constexpr const char* kTsl = "http://uri.etsi.org/02231/v2#";
constexpr const char* kTslx = "http://uri.etsi.org/02231/v2/additionaltypes#";
constexpr const char* kEcc = "http://uri.etsi.org/TrstSvc/SvcInfoExt/eSigDir-1999-93-EC-TrustedList/#";
constexpr const char* kXades = "http://uri.etsi.org/01903/v1.3.2#";

constexpr const char* kEnveloped = "http://www.w3.org/2000/09/xmldsig#enveloped-signature";
constexpr const char* kSvcType = "http://uri.etsi.org/TrstSvc/Svctype/";
constexpr const char* kSvcStatus = "http://uri.etsi.org/TrstSvc/TrustedList/Svcstatus/";
constexpr const char* kSvcInfoExt = "http://uri.etsi.org/TrstSvc/TrustedList/SvcInfoExt/";
constexpr const char* kTslMime = "application/vnd.etsi.tsl+xml";

/// Largest list read. Germany's, the largest today, is 5.4 MB.
constexpr std::size_t kMaxList = std::size_t{32} << 20;

// --- libxml2 plumbing -------------------------------------------------------

struct DocFree {
    void operator()(xmlDoc* d) const noexcept { xmlFreeDoc(d); }
};
using DocPtr = std::unique_ptr<xmlDoc, DocFree>;

bool is(const xmlNode* n, const char* ns, const char* name) {
    return n != nullptr && n->type == XML_ELEMENT_NODE && n->ns != nullptr &&
           std::strcmp(reinterpret_cast<const char*>(n->ns->href), ns) == 0 &&
           std::strcmp(reinterpret_cast<const char*>(n->name), name) == 0;
}

std::vector<xmlNode*> children(const xmlNode* parent, const char* ns, const char* name) {
    std::vector<xmlNode*> out;
    for (xmlNode* c = parent != nullptr ? parent->children : nullptr; c != nullptr; c = c->next) {
        if (is(c, ns, name)) {
            out.push_back(c);
        }
    }
    return out;
}

xmlNode* child(const xmlNode* parent, const char* ns, const char* name) {
    for (xmlNode* c = parent != nullptr ? parent->children : nullptr; c != nullptr; c = c->next) {
        if (is(c, ns, name)) {
            return c;
        }
    }
    return nullptr;
}

/// child(child(...)): a path of (namespace, name) steps.
xmlNode* path(const xmlNode* from, std::initializer_list<std::pair<const char*, const char*>> steps) {
    const xmlNode* n = from;
    for (const auto& [ns, name] : steps) {
        n = child(n, ns, name);
        if (n == nullptr) {
            return nullptr;
        }
    }
    return const_cast<xmlNode*>(n);
}

std::string text(const xmlNode* n) {
    if (n == nullptr) {
        return {};
    }
    xmlChar* t = xmlNodeGetContent(n);
    std::string out = t != nullptr ? reinterpret_cast<const char*>(t) : "";
    xmlFree(t);
    // Trim: the lists indent freely.
    const auto b = out.find_first_not_of(" \t\r\n");
    const auto e = out.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string{} : out.substr(b, e - b + 1);
}

std::string attr(const xmlNode* n, const char* name) {
    xmlChar* v = n != nullptr ? xmlGetProp(n, reinterpret_cast<const xmlChar*>(name)) : nullptr;
    std::string out = v != nullptr ? reinterpret_cast<const char*>(v) : "";
    xmlFree(v);
    return out;
}

/// A name in the list's own language preference: English when there is one.
std::string name_of(const xmlNode* names) {
    std::string first;
    for (xmlNode* n : children(names, kTsl, "Name")) {
        xmlChar* l = xmlNodeGetLang(n);  // xml:lang
        const bool en = l != nullptr && (std::strcmp(reinterpret_cast<char*>(l), "en") == 0);
        xmlFree(l);
        if (en) {
            return text(n);
        }
        if (first.empty()) {
            first = text(n);
        }
    }
    return first;
}

Bytes base64(const std::string& s) {
    std::string clean;
    clean.reserve(s.size());
    for (const char c : s) {
        if (c != ' ' && c != '\n' && c != '\r' && c != '\t') {
            clean += c;
        }
    }
    if (clean.empty() || clean.size() % 4 != 0 || clean.size() > (8U << 20)) {
        throw Error(0, "not base64");
    }
    Bytes out(clean.size() / 4 * 3);
    const int n = EVP_DecodeBlock(out.data(), reinterpret_cast<const unsigned char*>(clean.data()),
                                  static_cast<int>(clean.size()));
    if (n < 0) {
        throw Error(0, "not base64");
    }
    std::size_t len = static_cast<std::size_t>(n);
    for (std::size_t i = clean.size(); i > 0 && clean[i - 1] == '='; --i) {
        --len;  // EVP_DecodeBlock counts padding as zero bytes
    }
    out.resize(len);
    return out;
}

std::string sha256_hex(const Bytes& der) {
    std::array<unsigned char, 32> md{};
    unsigned int n = 0;
    EVP_Digest(der.data(), der.size(), md.data(), &n, EVP_sha256(), nullptr);
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (unsigned int i = 0; i < n; ++i) {
        out += digits[md[i] >> 4];
        out += digits[md[i] & 0x0F];
    }
    return out;
}

DocPtr parse(const Bytes& bytes) {
    if (bytes.empty() || bytes.size() > kMaxList) {
        throw Error(0, "the list is empty or implausibly large");
    }
    // No network, no DTD loading, no entity substitution: the libxml2
    // defaults, plus NONET. A document with a DTD at all is refused below.
    DocPtr doc{xmlReadMemory(reinterpret_cast<const char*>(bytes.data()),
                             static_cast<int>(bytes.size()), "list.xml", nullptr,
                             XML_PARSE_NONET | XML_PARSE_NOWARNING | XML_PARSE_NOERROR)};
    if (!doc || xmlDocGetRootElement(doc.get()) == nullptr) {
        throw Error(0, "the list is not well-formed XML");
    }
    if (doc->intSubset != nullptr || doc->extSubset != nullptr) {
        throw Error(0, "the list declares a DTD, which no trusted list does");
    }
    return doc;
}

// --- signature ----------------------------------------------------------------

void count_signatures(xmlNode* n, int* found, xmlNode** where) {
    for (xmlNode* c = n; c != nullptr; c = c->next) {
        if (c->type != XML_ELEMENT_NODE) {
            continue;
        }
        if (is(c, kDs, "Signature")) {
            ++*found;
            *where = c;
        }
        count_signatures(c->children, found, where);
    }
}

bool inside(const xmlNode* n, const xmlNode* ancestor) {
    for (; n != nullptr; n = n->parent) {
        if (n == ancestor) {
            return true;
        }
    }
    return false;
}

/// Elements carrying Id="id", anywhere in the document.
void find_ids(xmlNode* n, const std::string& id, std::vector<xmlNode*>* out) {
    for (xmlNode* c = n; c != nullptr; c = c->next) {
        if (c->type == XML_ELEMENT_NODE) {
            if (attr(c, "Id") == id) {
                out->push_back(c);
            }
            find_ids(c->children, id, out);
        }
    }
}

/// The shape every trusted list's signature has, checked before any
/// cryptography, so that what is verified is what is read:
///   - exactly one ds:Signature in the document, a child of the root;
///   - a Reference with URI="" whose transforms include enveloped-signature:
///     the signature covers the whole document;
///   - any other Reference points, by a unique Id, into that same Signature
///     (the XAdES SignedProperties), and nowhere else.
xmlNode* check_shape(xmlDoc* doc) {
    xmlNode* root = xmlDocGetRootElement(doc);
    int found = 0;
    xmlNode* sig = nullptr;
    count_signatures(root, &found, &sig);
    if (found != 1) {
        throw Error(0, found == 0 ? "the list is not signed"
                                  : "the list carries more than one signature");
    }
    if (sig->parent != root) {
        throw Error(0, "the list's signature is not where a trusted list puts it");
    }
    xmlNode* signed_info = child(sig, kDs, "SignedInfo");
    const auto refs = children(signed_info, kDs, "Reference");
    if (refs.empty()) {
        throw Error(0, "the list's signature references nothing");
    }
    bool whole = false;
    for (xmlNode* ref : refs) {
        if (xmlHasProp(ref, reinterpret_cast<const xmlChar*>("URI")) == nullptr) {
            throw Error(0, "a signature reference without a URI");
        }
        const std::string uri = attr(ref, "URI");
        if (uri.empty()) {
            bool enveloped = false;
            for (xmlNode* t : children(child(ref, kDs, "Transforms"), kDs, "Transform")) {
                enveloped = enveloped || attr(t, "Algorithm") == kEnveloped;
            }
            if (!enveloped) {
                throw Error(0, "the list's signature does not cover the whole list");
            }
            whole = true;
            continue;
        }
        if (uri.size() < 2 || uri[0] != '#') {
            throw Error(0, "the list's signature references something outside it");
        }
        std::vector<xmlNode*> targets;
        find_ids(root, uri.substr(1), &targets);
        if (targets.size() != 1 || !inside(targets.front(), sig)) {
            throw Error(0, "the list's signature references something other than its own "
                           "signed properties");
        }
    }
    if (!whole) {
        throw Error(0, "the list's signature does not cover the whole list");
    }
    return sig;
}

/// The certificates in the signature's KeyInfo.
std::vector<Bytes> key_info_certs(const xmlNode* sig) {
    std::vector<Bytes> out;
    for (xmlNode* data : children(child(sig, kDs, "KeyInfo"), kDs, "X509Data")) {
        for (xmlNode* c : children(data, kDs, "X509Certificate")) {
            try {
                out.push_back(base64(text(c)));
            } catch (const Error&) {
            }
        }
    }
    return out;
}

struct DsigFree {
    void operator()(xmlSecDSigCtx* c) const noexcept { xmlSecDSigCtxDestroy(c); }
};
using DsigPtr = std::unique_ptr<xmlSecDSigCtx, DsigFree>;

EVP_PKEY* public_key(const Bytes& der) {
    const unsigned char* p = der.data();
    X509* x = d2i_X509(nullptr, &p, static_cast<long>(der.size()));
    if (x == nullptr) {
        return nullptr;
    }
    EVP_PKEY* k = X509_get_pubkey(x);
    X509_free(x);
    return k;
}

/// A signature context allowing only what the lists use: exclusive or
/// inclusive C14N, the enveloped transform, SHA-2 digests and RSA/ECDSA
/// signatures. No XPath, no XSLT, no base64 transform, no external URIs.
/// `cert` null: no key, for checking References alone (the RSA-PSS path,
/// where xmlsec1 1.2 cannot even hold the key).
DsigPtr make_context(const Bytes* cert) {
    DsigPtr ctx{xmlSecDSigCtxCreate(nullptr)};
    if (!ctx) {
        throw Error(0, "cannot create an XML signature context");
    }
    ctx->flags |= XMLSEC_DSIG_FLAGS_IGNORE_MANIFESTS;
    ctx->enabledReferenceUris = static_cast<xmlSecTransformUriType>(
        xmlSecTransformUriTypeEmpty | xmlSecTransformUriTypeSameDocument);
    for (xmlSecTransformId t :
         {xmlSecTransformExclC14NId, xmlSecTransformExclC14NWithCommentsId,
          xmlSecTransformInclC14NId, xmlSecTransformInclC14N11Id, xmlSecTransformEnvelopedId,
          xmlSecTransformSha256Id, xmlSecTransformSha384Id, xmlSecTransformSha512Id}) {
        if (xmlSecDSigCtxEnableReferenceTransform(ctx.get(), t) < 0) {
            throw Error(0, "cannot configure XML signature verification");
        }
    }
    for (xmlSecTransformId t :
         {xmlSecTransformExclC14NId, xmlSecTransformExclC14NWithCommentsId,
          xmlSecTransformInclC14NId, xmlSecTransformInclC14N11Id, xmlSecTransformRsaSha256Id,
          xmlSecTransformRsaSha384Id, xmlSecTransformRsaSha512Id, xmlSecTransformEcdsaSha256Id,
          xmlSecTransformEcdsaSha384Id, xmlSecTransformEcdsaSha512Id
#ifdef LEHT_XMLSEC_HAS_PSS
          , xmlSecTransformRsaPssSha256Id, xmlSecTransformRsaPssSha384Id,
          xmlSecTransformRsaPssSha512Id
#endif
         }) {
        if (xmlSecDSigCtxEnableSignatureTransform(ctx.get(), t) < 0) {
            throw Error(0, "cannot configure XML signature verification");
        }
    }
    if (cert == nullptr) {
        return ctx;
    }
    // The key is the one Leht chose, never one the document offers.
    EVP_PKEY* pkey = public_key(*cert);
    xmlSecKeyDataPtr data = pkey != nullptr ? xmlSecOpenSSLEvpKeyAdopt(pkey) : nullptr;
    if (data == nullptr) {
        EVP_PKEY_free(pkey);
        throw Error(0, "the signing certificate's key cannot be read");
    }
    xmlSecKeyPtr key = xmlSecKeyCreate();
    if (key == nullptr || xmlSecKeySetValue(key, data) < 0) {
        xmlSecKeyDataDestroy(data);
        xmlSecKeyDestroy(key);
        throw Error(0, "cannot load the signing key");
    }
    ctx->signKey = key;  // the context owns it now
    return ctx;
}

/// RFC 6931 "sha*-rsa-MGF1": RSA-PSS, MGF1 with the same hash, salt as long
/// as the hash. Null when `uri` is not one of them.
const EVP_MD* pss_digest(const std::string& uri) {
    if (uri == "http://www.w3.org/2007/05/xmldsig-more#sha256-rsa-MGF1") {
        return EVP_sha256();
    }
    if (uri == "http://www.w3.org/2007/05/xmldsig-more#sha384-rsa-MGF1") {
        return EVP_sha384();
    }
    if (uri == "http://www.w3.org/2007/05/xmldsig-more#sha512-rsa-MGF1") {
        return EVP_sha512();
    }
    return nullptr;
}

/// The node set C14N sees for SignedInfo: it and everything below it.
int in_signed_info(void* data, xmlNodePtr node, xmlNodePtr parent) {
    const auto* si = static_cast<const xmlNode*>(data);
    const xmlNode* n = node->type == XML_NAMESPACE_DECL ? parent : node;
    if (n != nullptr && n->type == XML_ATTRIBUTE_NODE) {
        n = n->parent;
    }
    return inside(n, si) ? 1 : 0;
}

/// Verification when xmlsec1 lacks the signature algorithm (RSA-PSS): every
/// Reference through xmlsec1, then the signature value over the canonical
/// SignedInfo through OpenSSL.
void verify_pss(xmlDoc* doc, xmlNode* sig, const Bytes& cert, const EVP_MD* md) {
    DsigPtr ctx = make_context(nullptr);
    ctx->operation = xmlSecTransformOperationVerify;
    xmlNode* signed_info = child(sig, kDs, "SignedInfo");
    for (xmlNode* ref : children(signed_info, kDs, "Reference")) {
        xmlSecDSigReferenceCtxPtr r =
            xmlSecDSigReferenceCtxCreate(ctx.get(), xmlSecDSigReferenceOriginSignedInfo);
        if (r == nullptr) {
            throw Error(0, "cannot check the list's signature");
        }
        const int rc = xmlSecDSigReferenceCtxProcessNode(r, ref);
        const bool ok = rc == 0 && r->status == xmlSecDSigStatusSucceeded;
        xmlSecDSigReferenceCtxDestroy(r);
        if (!ok) {
            throw Error(0, "the list does not match its signature");
        }
    }

    // Canonical SignedInfo, with the method it names.
    xmlNode* method = child(signed_info, kDs, "CanonicalizationMethod");
    const std::string alg = attr(method, "Algorithm");
    int mode = -1;
    int comments = 0;
    if (alg == "http://www.w3.org/2001/10/xml-exc-c14n#") {
        mode = XML_C14N_EXCLUSIVE_1_0;
    } else if (alg == "http://www.w3.org/2001/10/xml-exc-c14n#WithComments") {
        mode = XML_C14N_EXCLUSIVE_1_0;
        comments = 1;
    } else if (alg == "http://www.w3.org/TR/2001/REC-xml-c14n-20010315") {
        mode = XML_C14N_1_0;
    } else if (alg == "http://www.w3.org/2006/12/xml-c14n11") {
        mode = XML_C14N_1_1;
    } else {
        throw Error(0, "the list's signature uses an unknown canonicalisation");
    }
    // InclusiveNamespaces PrefixList, for exclusive C14N.
    std::vector<std::string> prefixes;
    for (xmlNode* c = method != nullptr ? method->children : nullptr; c != nullptr; c = c->next) {
        if (c->type == XML_ELEMENT_NODE &&
            std::strcmp(reinterpret_cast<const char*>(c->name), "InclusiveNamespaces") == 0) {
            std::string list = attr(c, "PrefixList");
            std::size_t at = 0;
            while (at < list.size()) {
                const std::size_t end = list.find(' ', at);
                const std::string p = list.substr(at, end == std::string::npos ? end : end - at);
                if (!p.empty()) {
                    prefixes.push_back(p);
                }
                if (end == std::string::npos) {
                    break;
                }
                at = end + 1;
            }
        }
    }
    std::vector<xmlChar*> prefix_ptrs;
    for (std::string& p : prefixes) {
        prefix_ptrs.push_back(reinterpret_cast<xmlChar*>(p.data()));
    }
    prefix_ptrs.push_back(nullptr);
    xmlOutputBufferPtr out = xmlAllocOutputBuffer(nullptr);
    if (out == nullptr) {
        throw Error(0, "out of memory");
    }
    const int c14n = xmlC14NExecute(doc, in_signed_info, signed_info, mode,
                                    prefixes.empty() ? nullptr : prefix_ptrs.data(), comments, out);
    const unsigned char* canon = xmlOutputBufferGetContent(out);
    const std::size_t canon_len = xmlOutputBufferGetSize(out);
    Bytes signed_bytes(canon, canon + canon_len);
    xmlOutputBufferClose(out);
    if (c14n < 0) {
        throw Error(0, "cannot canonicalise the list's signature");
    }

    const Bytes value = base64(text(child(sig, kDs, "SignatureValue")));
    EVP_PKEY* pkey = public_key(cert);
    EVP_MD_CTX* mctx = EVP_MD_CTX_new();
    EVP_PKEY_CTX* pctx = nullptr;
    const bool ok =
        pkey != nullptr && mctx != nullptr &&
        EVP_DigestVerifyInit(mctx, &pctx, md, nullptr, pkey) == 1 &&
        EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) == 1 &&
        EVP_PKEY_CTX_set_rsa_mgf1_md(pctx, md) == 1 &&
        EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, RSA_PSS_SALTLEN_DIGEST) == 1 &&
        EVP_DigestVerify(mctx, value.data(), value.size(), signed_bytes.data(),
                         signed_bytes.size()) == 1;
    EVP_MD_CTX_free(mctx);
    EVP_PKEY_free(pkey);
    if (!ok) {
        throw Error(0, "the list's signature does not verify");
    }
}

/// Verifies `doc`'s signature with a certificate from `candidates` that
/// `allowed` accepts; returns it.
Bytes verify(xmlDoc* doc, const std::function<bool(const Bytes&)>& allowed,
             const std::vector<Bytes>& extra_candidates) {
    xmlNode* sig = check_shape(doc);
    // Ids for the XAdES reference: registered only after check_shape() made
    // sure each one used is unique and inside the signature.
    static const xmlChar* ids[] = {reinterpret_cast<const xmlChar*>("Id"), nullptr};
    xmlSecAddIDs(doc, xmlDocGetRootElement(doc), ids);

    // The signer is the certificate the signature names, and it must be one
    // allowed to sign this list. Only a signature that names none is tried
    // against the allowed certificates themselves.
    std::vector<Bytes> named = key_info_certs(sig);
    std::vector<Bytes> candidates;
    for (Bytes& c : named) {
        if (allowed(c)) {
            candidates.push_back(std::move(c));
        }
    }
    if (named.empty()) {
        for (const Bytes& c : extra_candidates) {
            if (allowed(c)) {
                candidates.push_back(c);
            }
        }
    }
    if (candidates.empty()) {
        throw Error(0, "the list is signed by a certificate not allowed to sign it");
    }
    const std::string method =
        attr(child(child(sig, kDs, "SignedInfo"), kDs, "SignatureMethod"), "Algorithm");
    std::string last = "the list's signature does not verify";
    for (const Bytes& cert : candidates) {
        try {
#ifndef LEHT_XMLSEC_HAS_PSS
            if (const EVP_MD* md = pss_digest(method)) {
                verify_pss(doc, sig, cert, md);
                return cert;
            }
#endif
            DsigPtr ctx = make_context(&cert);
            ctx->flags |= XMLSEC_DSIG_FLAGS_STORE_SIGNEDINFO_REFERENCES;
            if (xmlSecDSigCtxVerify(ctx.get(), sig) == 0 &&
                ctx->status == xmlSecDSigStatusSucceeded) {
                return cert;
            }
            // Which half failed: the content, or the signature over it.
            const xmlSecSize n = xmlSecPtrListGetSize(&ctx->signedInfoReferences);
            for (xmlSecSize i = 0; i < n; ++i) {
                const auto* r = static_cast<xmlSecDSigReferenceCtx*>(
                    xmlSecPtrListGetItem(&ctx->signedInfoReferences, i));
                if (r != nullptr && r->status != xmlSecDSigStatusSucceeded) {
                    last = "the list does not match its signature";
                }
            }
        } catch (const Error& e) {
            last = e.what();
        }
    }
    throw Error(0, last);
}

// --- TS 119 612 ----------------------------------------------------------------

struct Pointer {
    std::string territory;
    std::string url;
    std::string mime;
    std::vector<Bytes> certs;
};

struct Scheme {
    std::string territory;
    std::uint32_t sequence = 0;
    std::int64_t issued = 0;
    std::int64_t next_update = 0;
    std::vector<std::string> uris;  ///< SchemeInformationURI, in order
    std::vector<Pointer> pointers;
};

std::vector<Bytes> digital_ids(const xmlNode* identity) {
    std::vector<Bytes> out;
    for (xmlNode* id : children(identity, kTsl, "DigitalId")) {
        if (xmlNode* c = child(id, kTsl, "X509Certificate")) {
            try {
                out.push_back(base64(text(c)));
            } catch (const Error&) {
            }
        }
    }
    return out;
}

Scheme scheme_of(xmlDoc* doc) {
    xmlNode* root = xmlDocGetRootElement(doc);
    if (!is(root, kTsl, "TrustServiceStatusList")) {
        throw Error(0, "not a trusted list");
    }
    xmlNode* info = child(root, kTsl, "SchemeInformation");
    Scheme s;
    s.territory = text(child(info, kTsl, "SchemeTerritory"));
    s.sequence = static_cast<std::uint32_t>(
        std::strtoul(text(child(info, kTsl, "TSLSequenceNumber")).c_str(), nullptr, 10));
    s.issued = parse_time(text(child(info, kTsl, "ListIssueDateTime")));
    s.next_update = parse_time(text(path(info, {{kTsl, "NextUpdate"}, {kTsl, "dateTime"}})));
    for (xmlNode* u : children(child(info, kTsl, "SchemeInformationURI"), kTsl, "URI")) {
        s.uris.push_back(text(u));
    }
    for (xmlNode* p : children(child(info, kTsl, "PointersToOtherTSL"), kTsl, "OtherTSLPointer")) {
        Pointer ptr;
        ptr.url = text(child(p, kTsl, "TSLLocation"));
        for (xmlNode* identity :
             children(child(p, kTsl, "ServiceDigitalIdentities"), kTsl, "ServiceDigitalIdentity")) {
            for (Bytes& c : digital_ids(identity)) {
                ptr.certs.push_back(std::move(c));
            }
        }
        for (xmlNode* other :
             children(child(p, kTsl, "AdditionalInformation"), kTsl, "OtherInformation")) {
            if (xmlNode* t = child(other, kTsl, "SchemeTerritory")) {
                ptr.territory = text(t);
            }
            if (xmlNode* m = child(other, kTslx, "MimeType")) {
                ptr.mime = text(m);
            }
        }
        s.pointers.push_back(std::move(ptr));
    }
    return s;
}

Criteria criteria_of(const xmlNode* list, int depth) {
    Criteria c;
    const std::string a = attr(list, "assert");
    c.assert = a == "atLeastOne" ? Criteria::Assert::AtLeastOne
               : a == "none"     ? Criteria::Assert::None
                                 : Criteria::Assert::All;
    if (a != "all" && a != "atLeastOne" && a != "none") {
        c.unknown = true;
    }
    if (depth > 6) {
        c.unknown = true;
        return c;
    }
    for (xmlNode* n = list->children; n != nullptr; n = n->next) {
        if (n->type != XML_ELEMENT_NODE) {
            continue;
        }
        if (is(n, kEcc, "KeyUsage")) {
            std::vector<std::pair<std::string, bool>> bits;
            for (xmlNode* b : children(n, kEcc, "KeyUsageBit")) {
                bits.emplace_back(attr(b, "name"), text(b) == "true");
            }
            c.key_usage.push_back(std::move(bits));
        } else if (is(n, kEcc, "PolicySet")) {
            std::vector<std::string> oids;
            for (xmlNode* p : children(n, kEcc, "PolicyIdentifier")) {
                std::string oid = text(child(p, kXades, "Identifier"));
                if (oid.rfind("urn:oid:", 0) == 0) {
                    oid = oid.substr(8);
                }
                if (!oid.empty()) {
                    oids.push_back(oid);
                }
            }
            c.policy_sets.push_back(std::move(oids));
        } else if (is(n, kEcc, "CriteriaList")) {
            c.nested.push_back(criteria_of(n, depth + 1));
        } else if (is(n, kEcc, "Description")) {
            // Prose; nothing to match.
        } else {
            c.unknown = true;  // otherCriteriaList and anything newer
        }
    }
    return c;
}

std::uint32_t qualifier_of(const std::string& uri) {
    if (uri.rfind(kSvcInfoExt, 0) != 0) {
        return 0;
    }
    const std::string q = uri.substr(std::strlen(kSvcInfoExt));
    if (q == "QCStatement") return QcStatement;
    if (q == "NotQualified") return NotQualified;
    if (q == "QCWithQSCD" || q == "QCWithSSCD") return QcWithQscd;
    if (q == "QCNoQSCD" || q == "QCNoSSCD") return QcNoQscd;
    if (q == "QCQSCDStatusAsInCert" || q == "QCSSCDStatusAsInCert") return QcQscdStatusAsInCert;
    if (q == "QCQSCDManagedOnBehalf") return QcQscdManagedOnBehalf;
    if (q == "QCForLegalPerson") return QcForLegalPerson;
    if (q == "QCForESig") return QcForEsig;
    if (q == "QCForESeal") return QcForEseal;
    if (q == "QCForWSA") return QcForWsa;
    return 0;
}

/// A service's status is "granted" in the eIDAS sense, or one of the
/// Directive 1999/93 statuses that TS 119 615 counts the same for a CA/QC.
bool granted(const std::string& status) {
    if (status.rfind(kSvcStatus, 0) == 0) {
        const std::string s = status.substr(std::strlen(kSvcStatus));
        return s == "granted" || s == "recognisedatnationallevel";
    }
    const std::string old = "http://uri.etsi.org/TrstSvc/eSigDir-1999-93-EC-TrustedList/Svcstatus/";
    if (status.rfind(old, 0) == 0) {
        const std::string s = status.substr(old.size());
        return s == "undersupervision" || s == "supervisionincessation" || s == "accredited";
    }
    return false;
}

Phase phase_of(const xmlNode* info, const std::string& want_type) {
    Phase p;
    p.since = parse_time(text(child(info, kTsl, "StatusStartingTime")));
    p.granted = text(child(info, kTsl, "ServiceTypeIdentifier")) == want_type &&
                granted(text(child(info, kTsl, "ServiceStatus")));
    for (xmlNode* ext :
         children(child(info, kTsl, "ServiceInformationExtensions"), kTsl, "Extension")) {
        if (xmlNode* add = child(ext, kTsl, "AdditionalServiceInformation")) {
            const std::string uri = text(child(add, kTsl, "URI"));
            p.for_esig = p.for_esig || uri.ends_with("/ForeSignatures");
            p.for_eseal = p.for_eseal || uri.ends_with("/ForeSeals");
            p.for_web = p.for_web || uri.ends_with("/ForWebSiteAuthentication");
        }
        if (xmlNode* quals = child(ext, kEcc, "Qualifications")) {
            for (xmlNode* el : children(quals, kEcc, "QualificationElement")) {
                Qualification q;
                for (xmlNode* x : children(child(el, kEcc, "Qualifiers"), kEcc, "Qualifier")) {
                    q.qualifiers |= qualifier_of(attr(x, "uri"));
                }
                if (xmlNode* list = child(el, kEcc, "CriteriaList")) {
                    q.criteria = criteria_of(list, 0);
                } else {
                    q.criteria.unknown = true;  // a qualification must say for what
                }
                p.qualifications.push_back(std::move(q));
            }
        }
    }
    return p;
}

std::vector<Service> services_of(xmlDoc* doc, const std::string& territory) {
    std::vector<Service> out;
    xmlNode* root = xmlDocGetRootElement(doc);
    for (xmlNode* tsp :
         children(child(root, kTsl, "TrustServiceProviderList"), kTsl, "TrustServiceProvider")) {
        const std::string provider = name_of(path(tsp, {{kTsl, "TSPInformation"}, {kTsl, "TSPName"}}));
        for (xmlNode* svc : children(child(tsp, kTsl, "TSPServices"), kTsl, "TSPService")) {
            xmlNode* info = child(svc, kTsl, "ServiceInformation");
            const std::string type = text(child(info, kTsl, "ServiceTypeIdentifier"));
            Service s;
            if (type == std::string(kSvcType) + "CA/QC") {
                s.type = Service::Type::CaQc;
            } else if (type == std::string(kSvcType) + "TSA/QTST") {
                s.type = Service::Type::TsaQtst;
            } else {
                continue;
            }
            s.territory = territory;
            s.provider = provider;
            s.name = name_of(child(info, kTsl, "ServiceName"));
            s.certs = digital_ids(child(info, kTsl, "ServiceDigitalIdentity"));
            s.phases.push_back(phase_of(info, type));
            for (xmlNode* h : children(child(svc, kTsl, "ServiceHistory"), kTsl,
                                       "ServiceHistoryInstance")) {
                s.phases.push_back(phase_of(h, type));
                for (Bytes& c : digital_ids(child(h, kTsl, "ServiceDigitalIdentity"))) {
                    if (std::find(s.certs.begin(), s.certs.end(), c) == s.certs.end()) {
                        s.certs.push_back(std::move(c));
                    }
                }
            }
            std::stable_sort(s.phases.begin(), s.phases.end(),
                             [](const Phase& a, const Phase& b) { return a.since > b.since; });
            if (!s.certs.empty()) {
                out.push_back(std::move(s));
            }
        }
    }
    return out;
}

bool http_url(const std::string& u) {
    return u.rfind("https://", 0) == 0 || u.rfind("http://", 0) == 0;
}

std::string origin_of(const std::string& url) {
    const std::size_t start = url.find("://");
    const std::size_t end = start == std::string::npos ? std::string::npos
                                                       : url.find('/', start + 3);
    return end == std::string::npos ? url : url.substr(0, end);
}

}  // namespace

void init() {
    static std::once_flag once;
    std::call_once(once, [] {
        xmlInitParser();
        // Errors are reported through leht::Error, not printed.
        xmlSecErrorsDefaultCallbackEnableOutput(0);
        if (xmlSecInit() < 0 || xmlSecCheckVersion() != 1 || xmlSecOpenSSLInit() < 0) {
            throw Error(0, "cannot initialise xmlsec1");
        }
        // The algorithms verification can need, fetched now: a lazy fetch
        // inside the sandbox would open a file.
        for (const char* name : {"SHA2-256", "SHA2-384", "SHA2-512"}) {
            EVP_MD_free(EVP_MD_fetch(nullptr, name, nullptr));
        }
        // glibc reads /etc/localtime the first time a date is converted, UTC
        // or not; under the worker's seccomp that open is fatal. Now, then.
        tzset();
        (void)parse_time("2026-01-01T00:00:00Z");
    });
}

namespace detail {

std::vector<Service> parse_services_unverified(const Bytes& xml) {
    init();
    DocPtr doc = parse(xml);
    const Scheme s = scheme_of(doc.get());
    return services_of(doc.get(), s.territory);
}

}  // namespace detail

Bytes verify_signature(const Bytes& xml, const std::function<bool(const Bytes& der)>& allowed) {
    init();
    DocPtr doc = parse(xml);
    return verify(doc.get(), allowed, {});
}

Step advance(const Anchor& anchor, const Bytes& lotl_bytes, const std::vector<Fetched>& fetched,
             std::int64_t now) {
    init();
    Step step;
    TrustedList& out = step.result;
    out.built = now;
    out.lotl.territory = "EU";
    out.lotl.url = anchor.lotl_url;
    const auto finish = [&](std::string problem) {
        out.lotl.problem = std::move(problem);
        step.done = true;
        return step;
    };
    std::map<std::string, const Fetched*> have;
    for (const Fetched& f : fetched) {
        have[f.url] = &f;
    }

    DocPtr lotl;
    Scheme scheme;
    try {
        lotl = parse(lotl_bytes);
        scheme = scheme_of(lotl.get());
    } catch (const Error& e) {
        return finish(std::string("the List of Trusted Lists: ") + e.what());
    }

    // The pivots the LOTL names before its Official Journal reference, newest
    // first; they are walked oldest first, from the OJ-published certificates.
    std::vector<std::string> pivots;
    std::string oj;
    for (const std::string& u : scheme.uris) {
        if (u.ends_with(".xml")) {
            pivots.push_back(u);
        } else {
            oj = u;
            break;
        }
    }
    if (oj != anchor.oj_url) {
        return finish("the List of Trusted Lists is signed with certificates published in " +
                      (oj.empty() ? std::string("an Official Journal notice it does not name")
                                  : oj) +
                      "; this Leht knows only " + anchor.oj_url +
                      ". Nothing new is trusted until Leht is updated with that notice's "
                      "certificates.");
    }
    std::reverse(pivots.begin(), pivots.end());
    for (const std::string& p : pivots) {
        if (origin_of(p) != origin_of(anchor.lotl_url)) {
            return finish("the List of Trusted Lists names a pivot elsewhere than its own "
                          "location: " + p);
        }
        if (have.count(p) == 0) {
            step.need.push_back(p);
        }
    }
    if (!step.need.empty()) {
        return step;
    }

    // From the Official Journal's digests, through each pivot, to the LOTL.
    std::function<bool(const Bytes&)> allowed = [&anchor](const Bytes& der) {
        const std::string h = sha256_hex(der);
        return std::find(anchor.sha256.begin(), anchor.sha256.end(), h) != anchor.sha256.end();
    };
    std::vector<Bytes> current;
    const auto eu_certs = [&anchor](const Scheme& s) {
        for (const Pointer& p : s.pointers) {
            if (p.territory == "EU" && p.url == anchor.lotl_url) {
                return p.certs;
            }
        }
        return std::vector<Bytes>{};
    };
    for (const std::string& p : pivots) {
        const Fetched& f = *have.at(p);
        try {
            if (f.body.empty()) {
                throw Error(0, f.error.empty() ? "could not be fetched" : f.error);
            }
            DocPtr doc = parse(f.body);
            (void)verify(doc.get(), allowed, current);
            current = eu_certs(scheme_of(doc.get()));
            if (current.empty()) {
                throw Error(0, "names no signing certificates for the next one");
            }
        } catch (const Error& e) {
            return finish("pivot " + p + ": " + e.what());
        }
        allowed = [set = current](const Bytes& der) {
            return std::find(set.begin(), set.end(), der) != set.end();
        };
    }
    try {
        (void)verify(lotl.get(), allowed, current);
    } catch (const Error& e) {
        return finish(std::string("the List of Trusted Lists: ") + e.what());
    }
    out.lotl.verified = true;
    out.lotl.issued = scheme.issued;
    out.lotl.next_update = scheme.next_update;
    out.lotl.sequence = scheme.sequence;

    // The national lists it points to, in XML.
    std::vector<const Pointer*> national;
    for (const Pointer& p : scheme.pointers) {
        if (p.territory == "EU" || p.mime != kTslMime || !http_url(p.url)) {
            continue;
        }
        national.push_back(&p);
        if (have.count(p.url) == 0) {
            step.need.push_back(p.url);
        }
    }
    if (!step.need.empty()) {
        return step;
    }
    for (const Pointer* p : national) {
        ListStatus st;
        st.territory = p->territory;
        st.url = p->url;
        const Fetched& f = *have.at(p->url);
        try {
            if (f.body.empty()) {
                throw Error(0, f.error.empty() ? "could not be fetched" : f.error);
            }
            DocPtr doc = parse(f.body);
            const std::vector<Bytes>& certs = p->certs;
            (void)verify(
                doc.get(),
                [&certs](const Bytes& der) {
                    return std::find(certs.begin(), certs.end(), der) != certs.end();
                },
                certs);
            const Scheme s = scheme_of(doc.get());
            if (s.territory != p->territory) {
                throw Error(0, "is the list of " + s.territory + ", not " + p->territory);
            }
            st.issued = s.issued;
            st.next_update = s.next_update;
            st.sequence = s.sequence;
            std::vector<Service> services = services_of(doc.get(), p->territory);
            st.services = static_cast<std::uint32_t>(services.size());
            st.verified = true;
            for (Service& svc : services) {
                out.services.push_back(std::move(svc));
            }
        } catch (const Error& e) {
            st.problem = e.what();
        }
        out.lists.push_back(std::move(st));
    }
    step.done = true;
    return step;
}

}  // namespace leht::trustlist
