// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Synthetic trusted lists for tests: a LOTL or a national list in the
// TS 119 612 shape, signed with xmlsec1 by a test-PKI key, so a test can make
// exactly the pivot, the service history or the qualifier it needs. The real
// lists in fixtures.tar.xz check the other direction: signatures Leht did not
// make.
#pragma once

#include "test_pki.hpp"

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <xmlsec/crypto.h>
#include <xmlsec/keys.h>
#include <xmlsec/openssl/evp.h>
#include <xmlsec/xmldsig.h>
#include <xmlsec/xmlsec.h>
#include <xmlsec/xmltree.h>

#include <string>
#include <vector>

namespace leht::test::tl {

inline std::string b64(const crypto::Bytes& der) {
    std::string out(4 * ((der.size() + 2) / 3) + 1, '\0');
    const int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), der.data(),
                                  static_cast<int>(der.size()));
    out.resize(static_cast<std::size_t>(n));
    return out;
}

inline crypto::Bytes der(const Cert& c) {
    unsigned char* p = nullptr;
    const int n = i2d_X509(c.p, &p);
    crypto::Bytes out(p, p + n);
    OPENSSL_free(p);
    return out;
}

inline std::string sha256_hex(const Cert& c) {
    const crypto::Bytes d = der(c);
    unsigned char md[32];
    unsigned int n = 0;
    EVP_Digest(d.data(), d.size(), md, &n, EVP_sha256(), nullptr);
    std::string out;
    static const char digits[] = "0123456789abcdef";
    for (unsigned int i = 0; i < n; ++i) {
        out += digits[md[i] >> 4];
        out += digits[md[i] & 0x0F];
    }
    return out;
}

inline const char* kNs =
    "xmlns:tsl=\"http://uri.etsi.org/02231/v2#\" "
    "xmlns:tslx=\"http://uri.etsi.org/02231/v2/additionaltypes#\" "
    "xmlns:ecc=\"http://uri.etsi.org/TrstSvc/SvcInfoExt/eSigDir-1999-93-EC-TrustedList/#\" "
    "xmlns:xades=\"http://uri.etsi.org/01903/v1.3.2#\"";

struct Pointer {
    std::string territory;
    std::string url;
    std::vector<const Cert*> certs;
    std::string mime = "application/vnd.etsi.tsl+xml";
};

inline std::string pointers(const std::vector<Pointer>& ps) {
    std::string out = "<tsl:PointersToOtherTSL>";
    for (const Pointer& p : ps) {
        out += "<tsl:OtherTSLPointer><tsl:ServiceDigitalIdentities>";
        for (const Cert* c : p.certs) {
            out += "<tsl:ServiceDigitalIdentity><tsl:DigitalId><tsl:X509Certificate>" + b64(der(*c)) +
                   "</tsl:X509Certificate></tsl:DigitalId></tsl:ServiceDigitalIdentity>";
        }
        out += "</tsl:ServiceDigitalIdentities><tsl:TSLLocation>" + p.url +
               "</tsl:TSLLocation><tsl:AdditionalInformation>"
               "<tsl:OtherInformation><tsl:SchemeTerritory>" + p.territory +
               "</tsl:SchemeTerritory></tsl:OtherInformation>"
               "<tsl:OtherInformation><tslx:MimeType>" + p.mime +
               "</tslx:MimeType></tsl:OtherInformation>"
               "</tsl:AdditionalInformation></tsl:OtherTSLPointer>";
    }
    return out + "</tsl:PointersToOtherTSL>";
}

inline std::string scheme(const std::string& territory, const std::vector<std::string>& uris,
                          const std::string& pointer_xml, int sequence = 7) {
    std::string out = "<tsl:SchemeInformation><tsl:TSLVersionIdentifier>6</tsl:TSLVersionIdentifier>"
                      "<tsl:TSLSequenceNumber>" + std::to_string(sequence) +
                      "</tsl:TSLSequenceNumber><tsl:SchemeTerritory>" + territory +
                      "</tsl:SchemeTerritory><tsl:SchemeInformationURI>";
    for (const std::string& u : uris) {
        out += "<tsl:URI xml:lang=\"en\">" + u + "</tsl:URI>";
    }
    out += "</tsl:SchemeInformationURI>" + pointer_xml +
           "<tsl:ListIssueDateTime>2026-01-01T00:00:00Z</tsl:ListIssueDateTime>"
           "<tsl:NextUpdate><tsl:dateTime>2027-01-01T00:00:00Z</tsl:dateTime></tsl:NextUpdate>"
           "</tsl:SchemeInformation>";
    return out;
}

/// One period of a service in a synthetic list.
struct PhaseSpec {
    std::string since = "2020-01-01T00:00:00Z";
    std::string status = "granted";  ///< the Svcstatus suffix
    std::vector<std::string> additional;  ///< "ForeSignatures", "ForeSeals"
    /// Each: (qualifier suffixes, CriteriaList XML body). An empty body means
    /// a criteria list that matches everything with the policy "any".
    std::vector<std::pair<std::vector<std::string>, std::string>> qualifications;
};

struct ServiceSpec {
    std::string type = "CA/QC";  ///< or "TSA/QTST"
    std::string name = "Test qualified CA";
    const Cert* cert = nullptr;
    std::vector<PhaseSpec> phases;  ///< newest first
};

inline std::string phase_xml(const std::string& type, const std::string& name, const Cert* cert,
                             const PhaseSpec& p, bool history) {
    std::string ext;
    for (const std::string& a : p.additional) {
        ext += "<tsl:Extension Critical=\"false\"><tsl:AdditionalServiceInformation><tsl:URI "
               "xml:lang=\"en\">http://uri.etsi.org/TrstSvc/TrustedList/SvcInfoExt/" + a +
               "</tsl:URI></tsl:AdditionalServiceInformation></tsl:Extension>";
    }
    if (!p.qualifications.empty()) {
        ext += "<tsl:Extension Critical=\"true\"><ecc:Qualifications>";
        for (const auto& [quals, criteria] : p.qualifications) {
            ext += "<ecc:QualificationElement><ecc:Qualifiers>";
            for (const std::string& q : quals) {
                ext += "<ecc:Qualifier uri=\"http://uri.etsi.org/TrstSvc/TrustedList/SvcInfoExt/" + q +
                       "\"/>";
            }
            ext += "</ecc:Qualifiers><ecc:CriteriaList assert=\"atLeastOne\">" +
                   (criteria.empty() ? std::string("<ecc:KeyUsage><ecc:KeyUsageBit "
                                                   "name=\"nonRepudiation\">true</ecc:KeyUsageBit>"
                                                   "</ecc:KeyUsage><ecc:KeyUsage><ecc:KeyUsageBit "
                                                   "name=\"digitalSignature\">true</ecc:KeyUsageBit>"
                                                   "</ecc:KeyUsage>")
                                     : criteria) +
                   "</ecc:CriteriaList></ecc:QualificationElement>";
        }
        ext += "</ecc:Qualifications></tsl:Extension>";
    }
    const char* tag = history ? "ServiceHistoryInstance" : "ServiceInformation";
    std::string out = std::string("<tsl:") + tag + "><tsl:ServiceTypeIdentifier>http://uri.etsi.org/"
                      "TrstSvc/Svctype/" + type + "</tsl:ServiceTypeIdentifier><tsl:ServiceName>"
                      "<tsl:Name xml:lang=\"en\">" + name + "</tsl:Name></tsl:ServiceName>"
                      "<tsl:ServiceDigitalIdentity><tsl:DigitalId><tsl:X509Certificate>" +
                      b64(der(*cert)) + "</tsl:X509Certificate></tsl:DigitalId>"
                      "</tsl:ServiceDigitalIdentity><tsl:ServiceStatus>http://uri.etsi.org/"
                      "TrstSvc/TrustedList/Svcstatus/" + p.status + "</tsl:ServiceStatus>"
                      "<tsl:StatusStartingTime>" + p.since + "</tsl:StatusStartingTime>";
    if (!ext.empty()) {
        out += "<tsl:ServiceInformationExtensions>" + ext + "</tsl:ServiceInformationExtensions>";
    }
    return out + "</tsl:" + tag + ">";
}

inline std::string providers(const std::vector<ServiceSpec>& services) {
    std::string out = "<tsl:TrustServiceProviderList><tsl:TrustServiceProvider><tsl:TSPInformation>"
                      "<tsl:TSPName><tsl:Name xml:lang=\"en\">Leht Test TSP</tsl:Name></tsl:TSPName>"
                      "</tsl:TSPInformation><tsl:TSPServices>";
    for (const ServiceSpec& s : services) {
        out += "<tsl:TSPService>" + phase_xml(s.type, s.name, s.cert, s.phases.front(), false);
        if (s.phases.size() > 1) {
            out += "<tsl:ServiceHistory>";
            for (std::size_t i = 1; i < s.phases.size(); ++i) {
                out += phase_xml(s.type, s.name, s.cert, s.phases[i], true);
            }
            out += "</tsl:ServiceHistory>";
        }
        out += "</tsl:TSPService>";
    }
    return out + "</tsl:TSPServices></tsl:TrustServiceProvider></tsl:TrustServiceProviderList>";
}

inline std::string unsigned_list(const std::string& body, const Cert& signer) {
    return std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                       "<tsl:TrustServiceStatusList ") + kNs + " TSLTag=\"http://uri.etsi.org/"
           "19612/TSLTag\">" + body +
           "<ds:Signature xmlns:ds=\"http://www.w3.org/2000/09/xmldsig#\" Id=\"sig\">"
           "<ds:SignedInfo><ds:CanonicalizationMethod "
           "Algorithm=\"http://www.w3.org/2001/10/xml-exc-c14n#\"/><ds:SignatureMethod "
           "Algorithm=\"http://www.w3.org/2001/04/xmldsig-more#rsa-sha256\"/>"
           "<ds:Reference URI=\"\"><ds:Transforms><ds:Transform "
           "Algorithm=\"http://www.w3.org/2000/09/xmldsig#enveloped-signature\"/><ds:Transform "
           "Algorithm=\"http://www.w3.org/2001/10/xml-exc-c14n#\"/></ds:Transforms>"
           "<ds:DigestMethod Algorithm=\"http://www.w3.org/2001/04/xmlenc#sha256\"/>"
           "<ds:DigestValue></ds:DigestValue></ds:Reference></ds:SignedInfo>"
           "<ds:SignatureValue></ds:SignatureValue><ds:KeyInfo><ds:X509Data><ds:X509Certificate>" +
           b64(der(signer)) + "</ds:X509Certificate></ds:X509Data></ds:KeyInfo></ds:Signature>"
           "</tsl:TrustServiceStatusList>";
}

/// Signs `xml`'s signature template with `key`. Returns the signed document.
inline crypto::Bytes sign(const std::string& xml, const Key& key) {
    xmlDoc* doc = xmlReadMemory(xml.data(), static_cast<int>(xml.size()), "t.xml", nullptr, 0);
    if (doc == nullptr) {
        die("synthetic list does not parse");
    }
    xmlNode* sig = xmlSecFindNode(xmlDocGetRootElement(doc), xmlSecNodeSignature, xmlSecDSigNs);
    xmlSecDSigCtxPtr ctx = xmlSecDSigCtxCreate(nullptr);
    EVP_PKEY_up_ref(key.p);
    xmlSecKeyDataPtr data = xmlSecOpenSSLEvpKeyAdopt(key.p);
    ctx->signKey = xmlSecKeyCreate();
    xmlSecKeySetValue(ctx->signKey, data);
    if (sig == nullptr || xmlSecDSigCtxSign(ctx, sig) < 0) {
        die("xmlsec1 could not sign the synthetic list");
    }
    xmlSecDSigCtxDestroy(ctx);
    xmlChar* out = nullptr;
    int n = 0;
    xmlDocDumpMemory(doc, &out, &n);
    crypto::Bytes bytes(out, out + n);
    xmlFree(out);
    xmlFreeDoc(doc);
    return bytes;
}

}  // namespace leht::test::tl
