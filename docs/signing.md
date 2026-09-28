# Signing

M5 adds digital signatures: signing a PDF with a PKCS#12 key or with a key that stays on
an ID card, verifying the signatures a document already carries, and a visible signature
mark that makes no cryptographic claim at all. Both frontends have all of it — `leht sign`
/ `leht verify` and the viewer's Sign tool and Signatures panel.

The level is **PAdES baseline B-B**, and **B-T** when a timestamp authority is given. The
CMS is CAdES-shaped: `/SubFilter /ETSI.CAdES.detached`, signed attributes carrying
content-type, message-digest and ESS `signing-certificate-v2`, and **no CMS signing-time
attribute** — PAdES takes the claimed time from the dictionary's `/M` and forbids a second,
possibly disagreeing one. **B-LT** and **B-LTA** — revocation data embedded for
long-term validation, and document timestamps — came later; see
[Long-term validation](#long-term-validation).

## The key never enters the sandbox

Since M3 the viewer does not parse PDFs; a sandboxed `leht-worker` does. Signing needs the
private key and the PDF's structure, which pulls in opposite directions: the process that
holds your key should not be the one parsing a hostile file, and the process parsing a
hostile file should not hold your key.

So signing is two steps, in two processes:

```
viewer / CLI (trusted, holds the key)         leht-worker (sandboxed, parses the PDF)
  mkstemp beside the target ── Prepare(fd) ──►  writes the document as an incremental
                                                revision, with a signature dictionary
                                                whose /Contents is a hole of zeros;
                                                MuPDF fills in the /ByteRange
                          ◄── ByteRange ───────
  re-checks the hole ON THE FILE'S OWN BYTES:
    it starts at 0, '<' opens the hole and '>' closes it,
    only '0' lies between, and the second span ends exactly
    at the end of the file
  hashes the two spans, builds the CMS, asks the TSA if one was named,
  writes the hex into the hole, fsyncs, renames
```

The trusted side never parses the PDF. It checks one structural fact — *the signed bytes
are every byte of the file except that one hole* — and that check is made against the file,
not against what the worker says about it. A compromised worker can therefore cause an
invalid signature, or no signature; it cannot steer the key onto bytes of its choosing.
`test_worker.cpp` makes a worker lie about the hole and watches the signing refuse it.

Verification runs the other way round. A signature's CMS blob is a string inside the
document, so it is attacker-controlled, and it goes to OpenSSL's ASN.1 parser: that happens
**in the worker**, inside seccomp, and only flat rows of strings and flags come back. The
viewer parses no DER. `fuzz/fuzz_verify.cpp` fuzzes that path.

One consequence is worth knowing about, because it looks like a detail and is not: OpenSSL
loads algorithms lazily, and a lazy load opens a file, which the sandbox answers with
SIGKILL. `leht-worker` therefore fetches every digest, key type and signature algorithm it
could need *before* the sandbox goes up. A test sets `LEHT_WORKER_NO_PRELOAD=1` and checks
that the worker dies without it, so nobody tidies the preload away as redundant.

## Incremental saving

**A signature signs a file, not a document.** Rewriting the file — which is what every save
did before M5 — replaces the bytes a signature covers, and every signature in it becomes
noise. So saving learned to append:

- `SaveOptions::mode` is `Auto` by default: **incremental when the document is signed**, a
  full rewrite otherwise. `Full` and `Incremental` force it either way.
- An incremental save copies the original bytes untouched and appends a revision. The
  original file is a byte-exact prefix of the result, which a test checks.
- **A redacted document is never saved incrementally**, whatever is asked: the earlier
  revisions are exactly what redaction has to drop. Redacting a signed document therefore
  breaks its signatures — the CLI warns, and the viewer asks first.
- A document repaired on open cannot be saved incrementally either (there is no sound
  revision to append to). `leht sign` rewrites such a file once and signs the result,
  saying so.
- After an incremental save, that open `Document` refuses to save again: MuPDF then
  believes the file it was opened from already contains the revision just written, and a
  second save chained `/Prev` into nothing — qpdf reported an xref loop. Reopen the saved
  file instead. The viewer does exactly that after every save.

## What Leht signs, and what it will not claim

`leht sign FILE -o OUT.pdf --p12 ID.p12 [--field NAME | --box P:X0,Y0,X1,Y1] [--image IMG]
[--name N] [--reason R] [--location L] [--tsa URL]`

- The **password is never an argument.** `/proc` shows arguments to every process on the
  machine and shells keep history, so it comes from `--password-fd N` or a terminal prompt
  with echo off.
- A new signature creates its own field (`Signature1`, `Signature2`, …) or fills an
  existing empty one with `--field`. Without `--box`, the signature is invisible.
- **No FieldMDP transform and no field locking.** MuPDF's own signature widgets come with
  `/Lock /All`, which marks every other field read-only, and its signature dictionary
  carries a FieldMDP `/Reference` that locks nothing. Leht writes the dictionary itself and
  omits both: signing a document says "these bytes are mine", not "nobody may fill in this
  form". A `/Lock` the document's author put there *is* enacted — see Certification and
  field locks below.
- The digest follows the key: SHA-256, or SHA-384 for a P-384 key (SHA-512 above that), so
  the hash never becomes the weak half of the pair.
- `--tsa URL` timestamps the signature value over HTTP or HTTPS. The reply's imprint and
  nonce are checked, so a replayed response is refused, and the token's own signature is
  verified against the certificate it carries. Whether that authority is *trusted* is
  decided later, at verification, like any other signer.

## Signing with an ID card

`leht keys` lists the signing keys on every card in every reader, and `leht sign … --pkcs11
auto` signs with the card's one signing key (or `--pkcs11 URI` with the one `keys`
printed). The viewer's Sign dialog has the same choice: *Sign with: ID card or token*.

The key never leaves the card. Leht hands the card a hash and gets a signature value back;
everything else — the CMS, the timestamp, writing into the hole — is the same as for a
PKCS#12 key, and the same split applies: the card is driven from the trusted process, never
from the sandboxed worker.

- **Which cards.** Anything with a PKCS#11 module, found through p11-kit: OpenSC registers
  itself on Fedora, Debian and Ubuntu, so an Estonian ID card needs nothing configured —
  `opensc` and a running `pcscd`. `--pkcs11-module LIB` (or `LEHT_PKCS11_MODULE` for the
  viewer) names a module that did not register itself.
- **PIN1 and PIN2.** An Estonian ID card carries two keys: one for logging in (PIN1) and
  one for signing (PIN2), whose certificate says nonRepudiation. Leht lists the signing key
  first, `auto` only ever picks such a key, and the viewer labels the field *PIN2* when the
  token says so. A signing key that demands its PIN again for every signature
  (`CKA_ALWAYS_AUTHENTICATE`) gets it from the same entry, so one dialog makes one
  signature with one PIN.
- **The PIN** is typed like the `.p12` password — `--password-fd N` or a prompt with echo
  off, never an argument — and is wiped when the signing is done. A PIN inside the URI
  (`pin-value=`) is refused. On a reader with its own keypad Leht asks for nothing and
  says to enter it there.
- **Before it blocks.** A wrong PIN is reported as such, and the card's own warnings are
  passed on: "a few more wrong tries will block it", "one more wrong PIN will block it". A
  blocked PIN is refused before anything is sent to the card; unblocking takes the PUK, in
  DigiDoc4.
- **Checked before it is written.** Leht assembles the CMS around the card's signature
  value itself (OpenSSL signs only with keys it holds), so every signature is parsed back
  and verified against the certificate before a byte goes into the file. A card whose key
  does not match the certificate beside it produces an error, not a broken signature.
- **Trust.** With the EU trusted lists fetched once (`leht trusted-list update`, see
  [The EU trusted lists](#the-eu-trusted-lists)), an ID-card signature verifies as
  *trusted* and as a *qualified electronic signature* with nothing else to do: Estonia's
  list names the ESTEID CAs. Without them it is *intact* but *untrusted* until you add the
  Estonian roots yourself, with `--trust` or *Sign → Trusted Certificates…*.

## Signing with a phone: Smart-ID and Mobile-ID

SK ID Solutions' two phone signatures are qualified signatures like an ID card's — the key
is on the phone (Smart-ID's app) or the SIM card (Mobile-ID) — and Leht signs with both, in
the CLI and the viewer. **For now against SK's demo environment only**, with its test
accounts: real accounts need a relying-party contract with SK, which Leht does not have.

```
leht sign doc.pdf -o signed.pdf --smart-id qr                   # scan the code in the terminal
leht sign doc.pdf -o signed.pdf --smart-id EE:38001085718       # by personal code
leht sign doc.pdf -o signed.pdf --mobile-id +37268000769:60001017869
```

The viewer's Sign dialog has *Sign with: Smart-ID* (a QR code, or a personal code) and
*Mobile-ID* (phone number and personal code). A window then shows the QR code or the
verification code, what is happening, and Cancel.

- **What goes over the network.** To SK only: the relying party's name and UUID, the
  person's identifier (their personal code, or phone number and personal code) and the
  **digest of the signed attributes** — 32 bytes of SHA-256. Never the document, nothing about
  it but that digest and the text shown on the phone ("Sign contract.pdf"). Then, from the
  certificate SK returns, its issuer from the `caIssuers` address it names, so that the
  signature carries the chain a verifier needs. Nothing happens unless the person chooses
  a phone to sign with; there is no background traffic.
- **Smart-ID by QR code** (SK's recommended flow, and the viewer's default): an anonymous
  certificate-choice session shown as a QR code that is **renewed every second**. Its link
  carries an `authCode`, an HMAC keyed with a session secret that never leaves the trusted
  process, so a code cannot be made in advance or by anyone else. Scanning it with the
  Smart-ID app chooses the account; the signature request then goes to that phone as a
  session *linked* to the scan, which it is waiting for — so there is no code to compare.
  The CLI draws the QR code in the terminal with half-block characters, and prints the
  link instead when its output is not a terminal.
- **Smart-ID by personal code, and Mobile-ID:** the phone is asked directly, and a
  **four-digit verification code** is shown, which the phone shows too. Enter the PIN only
  if they match: that is what ties the request on the phone to the one on the screen.
  Smart-ID's code comes from SK (API v3); Mobile-ID's is computed from the digest (6 bits
  from its start, 7 from its end).
- **The signature.** Smart-ID signs with **RSASSA-PSS** (SHA-256, MGF1-SHA-256, a 32-byte
  salt), which SK recommends and which the SignerInfo says as `id-RSASSA-PSS` with its
  parameters; Mobile-ID with ECDSA (P-256 in the demo) or RSA PKCS#1 v1.5. As for a card,
  Leht builds the CMS itself around the value that comes back, and **verifies it against
  the certificate before a byte is written**: a phone that signed something else, with
  another key, or with other parameters than asked is an error, not a broken file. SK's
  reply that the phone signed with a different certificate than it chose is refused too.
- **Refusals and failures** read as what happened: *You declined on your phone*, *The
  phone did not answer in time*, *This phone number has no active Mobile-ID*, *this
  Smart-ID account cannot give a qualified signature* (SK's HTTP 471), and so on. Cancel in
  the viewer, or Ctrl-C in the CLI, stops within about a second. A refusal or a cancel
  exits 3 in the CLI, and in every case **nothing is written**.
- **Trust.** Demo accounts chain to SK's *TEST* CAs, which are on no trusted list: their
  signatures are *intact* but *untrusted* and *not qualified* unless those roots are added
  with `--trust`. A live account would chain to the same qualified CAs as an ID card.
- **HTTP.** SK's Smart-ID service refuses HTTP/1.0, which is all OpenSSL's own client
  speaks, so SK is reached with a small HTTP/1.1 exchange of Leht's own (`http11()` in
  `crypto/src/http.cpp`): TLS verified against the system's CAs with the host name
  checked, one request per connection, a size cap and a timeout, and the status code read
  from the reply — SK's codes carry the message.
- **Pinned.** Beyond the system's CAs and the host name, the chain SK's server presents
  must hold one of two keys built into Leht: DigiCert's issuing CA *Global G2 TLS RSA
  SHA256 2020 CA1* (valid to 2031) or its root, *DigiCert Global Root G2* (to 2038). All
  four of SK's hosts, demo and live, are certified that way (checked 28 September 2026). A
  certificate from any other CA — an intercepting proxy's, or a CA that should not have
  issued one — is refused before a byte is sent, and so is plain `http://` to anything but
  this machine. The CA keys are pinned rather than SK's own certificates on purpose: those
  are renewed every year (the demo Smart-ID one expires on 10 October 2026), and a pinned
  certificate would break signing on SK's schedule, not Leht's. The cost is that DigiCert
  itself is trusted for SK's names; if SK ever changes certificate authority, Leht needs an
  update, and the error says so.
- **A server that hangs up** mid-request is an error, not the end of Leht: writing to a
  closed connection raises SIGPIPE, which kills a process by default, so every HTTP
  exchange (SK, timestamps, OCSP, CRLs, the trusted lists) blocks it on its own thread for
  its duration, without changing it for the rest of the process.

What SK's demo did, checked by hand on 28 September 2026 (`LEHT_SK_DEMO=1 test_sk`): a
Mobile-ID signature with `+37268000769`, end to end; a Smart-ID certificate choice by
personal code, with the certificate and its intermediate; a QR session and its links. SK's
*MOCK* Smart-ID accounts no longer finish a v3 **signature** on their own, so that last step
has been tested against a local imitation of SK only (`crypto/tests/sk_mock.hpp`), which
checks every request as SK documents it — the `authCode` of each QR link included — and
signs as a phone does. A signature through a real phone and SK's demo app has not been
made yet.

For a live deployment, what is missing is SK's side: a contract, and a registered name and
UUID (Preferences → Signing, or `--relying-party-name` and `--relying-party-uuid`), plus
the live addresses, which use the same pins.

## Certification and field locks

An ordinary signature says "I signed this". A **certification** is the author's signature,
and says what others may still do without breaking it:

```
leht sign FILE -o OUT.pdf --p12 ID.p12 --certify no-changes|forms|comments
```

- `no-changes` (DocMDP level 1): nothing at all.
- `forms` (level 2): filling in form fields, and signing.
- `comments` (level 3): those, and annotations.

The signature dictionary gets a DocMDP `/Reference` and the catalog a `/Perms /DocMDP`
entry naming it. A certification must be the **first** signature — Leht refuses to certify
a document that is already signed — and a document certified with no changes allowed takes
no further signature unless `--force`d. In the viewer: *Certify* in the Sign dialog, offered
only while the document has no signature.

A **field lock** is the form author's: a signature field whose `/Lock` names fields (all,
these, or all but these) that signing it should freeze. Leht enacts it by writing a
FieldMDP `/Reference` that mirrors the lock, and by making the locked fields read-only in
the same revision, so every reader stops editing them — and so does Leht's own form
filling.

### What changed, and was it allowed

Whenever a certification or a lock applies, Leht judges every change made after each
signature, object by object, by what the object is:

| change | allowed at |
|---|---|
| a field's value and its widget's appearance (form filling) | level 2 and 3, unless a lock covers the field |
| a new signature field, its widget, and its entry in the form | level 2 and 3 |
| an annotation added, changed or removed | level 3 |
| long-term-validation data (DSS, document timestamps) | always |
| page content, resources, the page tree, anything else | never |

It reads each earlier revision the way MuPDF's own validator does — through the xref section
that revision ended with — which works on encrypted files too. MuPDF's validator itself is
not used: it only enforces level 1, and at levels 2 and 3 treats any change to a page
dictionary as a violation, which is exactly how a permitted new signature or annotation
arrives. At level 1, where MuPDF's judgement is sound, a test checks that the two agree
on every kind of change. The full permission matrix is a test too
(`core/tests/test_mdp.cpp`).

`leht verify` prints the certification and, per signature, either that every later change
was allowed or each one that was not ("the content of page 2 changed", "field 'amount' was
changed, but a signature locks it"), and exits 7. The viewer's Signatures panel says the
same, the banner names the certification, and the tools a certification forbids are
disabled, with the reason in their tooltip; on the CLI an edit a certification forbids is
refused unless `--force`. A file whose revisions cannot be told apart — MuPDF had to repair
it — is reported as not judgeable, never as permitted.

Without a certification or a lock nothing is judged: an ordinary signature still reports
only that the document was *changed after signing* (see above for why there is no verdict
then).

## Encrypted documents

An encrypted PDF is signed as it is, and stays encrypted. The new revision's strings —
`/Reason`, `/Name`, `/M` — are encrypted like the rest of the file; the signature's own
`/Contents` is the one string the standard requires to stay in the clear, and MuPDF's
writer leaves it so. The CLI asks for the document's password after the key's
(`--doc-password-fd N`, or the next line of stdin); the viewer already holds an
authenticated document. `pdfsig -upw` and `qpdf --password` confirm the result in the tests,
for AES-256 and AES-128.

## Verifying

`leht verify FILE [--trust CA.pem]... [--json]`, and in the viewer a Signatures panel plus a
banner on every signed document.

Four things are reported, and kept apart on purpose:

| Question | What answers it |
|---|---|
| Are these the bytes that were signed? | The message digest over the `/ByteRange`, and the signature over the signed attributes |
| Whose key was it? | The signer's certificate, with `signing-certificate-v2` checked so a substituted certificate with the same key does not pass |
| Do we have any reason to believe that name? | A chain to the system CA bundle plus certificates you added — reported as trusted, not trusted, expired or not yet valid |
| Is what I am looking at what was signed? | Whether the file grew after the signed revision, and whether a later signature covers those bytes too — validation data added afterwards does not count |
| Was the certificate revoked? | OCSP responses and CRLs: the document's own `/DSS`, and with `--online` what the certificates' services say now — see [Long-term validation](#long-term-validation) |

Exit codes say the same thing to scripts: **0** all valid and trusted, **4** something is
broken, **5** intact but the signer is not trusted (a certificate revoked before the
signing time included), **6** intact and trusted but the document was added to
afterwards, **7** changed in a way a certification or a field lock forbids, and with
`--require-qualified` **8** not a qualified electronic signature.

An encrypted document is verified with its password: `--doc-password-fd N`, else it is
asked for.

**The byte range is checked before the cryptography.** A signature's `/ByteRange` must be
two spans, start at 0, lie inside the file, and leave out exactly the gap its own
`/Contents` hex string occupies. Without that last check a signature could carry a valid
signature over bytes that are not the ones the blob sits in — the family of "shadow
attacks" on PDF signing. A signature that fails it is reported as broken, whatever OpenSSL
would have said about the blob.

**"Changed after signing" is a fact, not a verdict.** There is deliberately no "were the
later changes permitted?" flag. MuPDF's `pdf_validate_signature()` answers that only in
terms of field locks, and with none set it judges a second signature exactly as it judges a
watermark stamped across the text. What Leht reports instead: the document grew after this
signature, and whether a later signature in the same document covers the new bytes as well.

**Trust is the system's, yours, and the EU's.** Fedora's `/etc/pki/tls/certs/ca-bundle.crt`
(or the Debian, openSUSE location); certificates added with `--trust` or the viewer's
*Sign → Trusted Certificates…*; and, once fetched, the EU trusted lists' qualified CAs and
timestamp authorities — see [The EU trusted lists](#the-eu-trusted-lists). With the lists,
`verify` also says whether each signature is *qualified*.

**Revocation is checked only against data Leht was given.** Without any — no `/DSS` in the
file, no `--online` — `verify` says *revocation: not checked* and keeps its exit code: a
missing answer is reported, not treated as a failure, or every B-B and B-T signature ever
made would fail.

## Long-term validation

A signature has to stay checkable after its certificate expires and after its CA's
revocation services have moved on. PAdES does that in two steps, and Leht makes both:

- **B-LT** — the validation data goes into the document: every certificate in the chains,
  and an OCSP response or CRL for each one, in the catalog's `/DSS` (Document Security
  Store). Checked later, the document answers for itself, offline.
- **B-LTA** — a **document timestamp** over all of it (`/Type /DocTimeStamp`,
  `/SubFilter /ETSI.RFC3161`): an RFC 3161 token on the whole file, proving the data
  existed at that time. It signs nobody's name.

```
leht sign FILE -o OUT.pdf --p12 ID.p12 --tsa URL --ltv    # B-T, then B-LT
leht sign FILE -o OUT.pdf --p12 ID.p12 --tsa URL --lta    # ... then B-LTA
leht ltv  FILE -o OUT.pdf [--tsa URL]                     # for signatures already there
leht verify FILE [--online]
```

`--ltv` needs `--tsa`: validation data proves a certificate was good *at a time*, and only
a timestamp makes that time more than the signer's say-so. `leht ltv` works on any signed
document, other people's signatures included, and **renews**: run it again with `--tsa`
before the last document timestamp's authority certificate expires, and the new timestamp
covers the old one along with fresh data for it.

In the viewer: *Add long-term validation data* in the Sign dialog (with a timestamp
authority), *Sign → Add Long-Term Validation…* for a document already signed, and
*Check Revocation Online* in the Signatures panel (and the Sign menu).

### What goes over the network

Only when asked: `--ltv`, `--lta`, `leht ltv`, `verify --online`, or the viewer's two
actions. Each says which hosts it is about to contact. **What travels is an OCSP request —
a hash of the issuer's name and key and the certificate's serial number — or a plain
download of a CRL. Never the document.** The addresses come from the certificates
themselves (their Authority Information Access and CRL Distribution Points); only
`http://` and `https://` are followed, with no redirects, and replies are capped at 1 MB
for OCSP and 16 MB for a CRL.

In the viewer the split is the same as for signing: the worker, which parses the
document, works out what to ask and parses every reply; the viewer only moves bytes, and a
worker that asks it to fetch anything but an http(s) URL is not believed.

### How revocation is judged

The time that matters is the **trusted time**: the signature's timestamp when it has a
valid one, else now. Each certificate in the chain — the trust anchor left out, since it
is trusted by being in the store — is:

- **revoked** when valid data says it was revoked *before* that time. The signature is
  then reported as untrusted (exit 5), with the date.
- **good** when valid data issued after that time says it was not revoked, or data whose
  validity window (*thisUpdate* to *nextUpdate*) covers that time does. The second case
  matters: many responders pre-compute their answers, so one fetched right after signing is
  often a little older than the signature. A revocation *after* the trusted time is shown
  — *good when signed, revoked later* — and does not undo the signature. That is what
  long-term validation is for.
- **unknown** otherwise: no data, data too old, or data that is not valid. An OCSP response
  counts only if it is signed by the certificate's issuer, or by a responder the issuer
  certified for exactly that (id-kp-OCSPSigning), valid when it answered. A CRL counts
  only if the issuer signed it. The tests try each of those the other way round.

A timestamp authority's own certificate is judged the same way, at its token's time. So
the **newest** document timestamp usually reports its authority as *unknown*: its
validation data was fetched before it existed, and can only go in under the next one.
That is how B-LTA works, not a fault, and `verify` says so; the next `leht ltv --tsa`
adds it.

### What it does not do

- The validation model is a chain checked at one trusted time, not the full ETSI EN 319
  102-1 past-signature-validation algorithm with its per-certificate time sliding.
- No `/VRI` entries are written. PAdES makes them optional; Leht reads them in other
  tools' files.
- No revocation data goes inside the CMS signature (Adobe's
  `adbe-revocationInfoArchival`); it all goes in the `/DSS`, which is where PAdES puts it.

### Certifications and later changes

Validation data and document timestamps are the one thing a certification allows at
**every** level, "no changes" included, and Leht's change checker says so. Nor do they
make a signature *changed after signing*: `verify` reports *validation data was added
afterwards; it changes nothing signed*, and exits 0. A real change after them is still a
change.

A document timestamp is listed with the signatures, as a document timestamp. Before M4,
Leht took one for a CMS signature and reported it **broken**.

## The EU trusted lists

Under eIDAS a **qualified electronic signature** (QES) has the legal effect of a
handwritten one. Whether a signature is one is not something its certificate can simply
claim: each member state publishes a **trusted list** of the trust services it supervises
— which CAs issue qualified certificates, which timestamp authorities are qualified, since
when, and for what — and the Commission publishes the **List of Trusted Lists** (LOTL)
that points to them all.

```
leht trusted-list update     # fetch and verify them (network; nothing else fetches them)
leht trusted-list status     # what is cached, and whether any list is overdue
leht verify FILE [--require-qualified] [--no-trusted-list]
```

`update` fetches the LOTL and every national list it points to — about 30 lists, 28 MB,
naming each host first — verifies them, and keeps what it needs in
`$XDG_CACHE_HOME/leht/trusted-list/` (Leht's own compact form, not the XML). From then on,
offline:

- the qualified CAs **anchor trust** — each only for signatures made while its status was
  *granted*, so a CA withdrawn after you signed does not undo your signature, and one
  withdrawn before does not vouch for it. The qualified timestamp authorities anchor
  timestamps, and neither does the other's job;
- `verify` says, for each signature, one of: **qualified electronic signature (QES)**,
  **qualified electronic seal**, **advanced, with a qualified certificate** (qualified, but
  the key is not on a qualified signature creation device), or **not qualified**, with the
  reason; a timestamp from a qualified TSA says *qualified*. `--require-qualified` exits
  **8** unless every signature is a QES. `--no-trusted-list` leaves it all out.

If a list is overdue (past its *next update*), `status` says so; the cached list is still
used, as a validator should, until you update.

### Where the trust comes from

Leht verifies every list by its XML signature, and trusts exactly one thing without
verifying it: **who may sign the LOTL**. The Commission publishes that in the Official
Journal — currently notice **C/2026/1944** (15 April 2026), giving the SHA-256 digests of
six certificates. Those digests are in `trustlist/src/anchor.cpp`, with the link, so anyone
can compare the two; they were checked against the notice and against the certificates
the LOTL itself carries.

When the Commission changes its signing certificates it publishes a **pivot** — the old
LOTL, archived, naming the new certificates, signed with the old ones. Leht follows pivots
by signature, oldest first, from the Official Journal's certificates to the current LOTL.
If instead a new Official Journal notice appears, the LOTL names it and Leht stops: nothing
new is trusted, the cached list stays, and `update` says a Leht update with the new
notice's digests is needed.

Each national list must be signed by a certificate the LOTL lists for that country, and
say it is that country's. The signature must cover the whole list: exactly one signature,
a child of the root, referencing the whole document and nothing outside its own XAdES
properties — the checks that stop "signature wrapping", where a valid signature over one
part of a document stands beside different content that a reader believes. Lists are
parsed with no DTDs, no entities and no network. xmlsec1 does the XML-signature
cryptography; for RSA-PSS, which Germany uses and xmlsec1 1.2 does not know, xmlsec1 still
checks every reference and Leht checks the signature value with OpenSSL.

### What "qualified" is decided from

For a signature, at its trusted time (the timestamp's, when valid):

- the certificate was issued by a CA on a list as *CA/QC*, with status *granted* then;
- it is qualified — its QcCompliance statement says so, or the list's *QCStatement*
  qualifier does, and the list does not say *NotQualified*;
- it is for signatures (QcType *esign*, the list's *QCForESig*, or a CA that issues only
  for signatures), or for seals;
- its key is on a QSCD — the list's *QCWithQSCD*, or the certificate's QcSSCD statement
  when the list does not say *QCNoQSCD*.

A list's qualifier applies only to the certificates its criteria select — key usage and
certificate policies, as Estonia's use. A criterion Leht cannot check means the qualifier
is **not** applied, never guessed at.

What this is not: the full ETSI TS 119 172-4 validation policies, trust frameworks outside
the EU, the PDF-only lists a few countries also publish, or the revocation of the lists'
own signing certificates.

## The visible mark that is not a signature

`leht annotate --stamp-image P:IMAGE:X0,Y0,X1,Y1`, and in the viewer an image or drawn mark
placed as an annotation. It puts a picture of a signature on the page: a scanned
autograph, say.

It proves nothing about who put it there, and the annotation's own `/Contents` says so — "A
picture, not a digital signature." — so a reader inspecting it is told, not left to assume.
It exists because people genuinely want a signature *image* on a page; it is kept
deliberately separate from `sign`, which is the one that means something.

## How it is verified

Our own code agreeing with itself proves little about a format this old, so:

- **`openssl cms -verify`** checks our CMS with a different implementation of the same
  standard (`test_crypto.cpp`).
- **`pdfsig`** (poppler, NSS underneath) reads the signed PDFs and must say "Signature is
  Valid" for each one — an independent PDF signature verifier, not a library we link
  (`test_pdf_sign.cpp`).
- **`qpdf --check`** reads every file we write.
- **SoftHSM2 stands in for the card** (`test_pkcs11.cpp`, and the viewer's smoke test): a
  token is made in a temporary directory, the test PKI's keys are put on it, and signing
  runs through PKCS#11 exactly as with a card — RSA and P-384, a key that wants its PIN
  for every signature (and a check that SoftHSM really refuses without it), a wrong PIN,
  a certificate that belongs to another key, B-T. **It has not been tried with a real ID
  card in a real reader yet**, so what is said above about Estonian cards comes from their
  documentation, not from a test.
- A **test PKI is generated at run time** — a root, RSA-2048 and ECDSA P-384 signers, an
  expired certificate, one whose key usage forbids signing — so no private key is ever
  committed. So is a **local RFC 3161 timestamp authority**, which makes B-T testable
  without the network, including a TSA that answers with the wrong nonce.
- Tampering is checked from both ends: a flipped byte inside the signed range, and a
  `/ByteRange` edited to point elsewhere.

## In the viewer

The **Sign** tool drags a box for a visible signature; *Sign → Sign Invisibly…* skips the
box. The dialog takes the `.p12` and its password — or a key on an ID card and its PIN,
listed from the cards in the readers, or Smart-ID or Mobile-ID — what the signature should
show — the
name and date, an image, or a signature **drawn** on a small canvas — the reason and
location, and optionally a timestamp authority (remembered between sessions).

Signing is a save: any unsaved edits go into the same revision the signature covers, the
signed file becomes the document, and the edit log restarts from it. **Undo does not reach
back past a signature**, which is as it should be — undoing into a signed revision could
only invalidate it.

The Signatures tab in the left sidebar gives each signature a card: a coloured shield and a
verdict — *Valid*, *Signer not trusted*, *Certificate revoked*, *Changed after signing*,
*Changed in a forbidden way*, *Broken*, the same judgement as `leht verify`'s exit codes — and
a sentence saying what it means, who signed and when (proved by a timestamp, or only claimed),
whether it is qualified, and buttons to go to its page or, for an unknown signer, to the
trusted certificates. A document timestamp has its own card, with the time it proves.
*Technical details* folds out the full record under the cards: signer, issuer, trust,
revocation, algorithm, claimed time, timestamp, reason, location and the certificate's
SHA-256 fingerprint.

With the EU trusted lists, each signature also gets a *Qualified* line — *qualified
electronic signature (QES)*, *qualified electronic seal*, *advanced, with a qualified
certificate*, or *not qualified* with the reason — a qualified timestamp says so, and the
banner says *Qualified electronic signature* when every signature is one. *Sign → Update EU
Trusted Lists…* fetches them, after asking: the viewer only moves the bytes, and a
sandboxed `leht-worker --trusted-list` reads and verifies the XML. The lists are cached
where the CLI keeps them, so either one's update serves both; the banner mentions it when
they are overdue.

## Not yet

- **Smart-ID and Mobile-ID with real accounts.** Only SK's demo environment is wired in;
  see [Signing with a phone](#signing-with-a-phone-smart-id-and-mobile-id) for what a live
  one needs.
