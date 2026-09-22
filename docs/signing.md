# Signing

M5 adds digital signatures: signing a PDF with a PKCS#12 key, verifying the signatures a
document already carries, and a visible signature mark that makes no cryptographic claim at
all. Both frontends have all of it — `leht sign` / `leht verify` and the viewer's Sign tool
and Signatures panel.

The level is **PAdES baseline B-B**, and **B-T** when a timestamp authority is given. The
CMS is CAdES-shaped: `/SubFilter /ETSI.CAdES.detached`, signed attributes carrying
content-type, message-digest and ESS `signing-certificate-v2`, and **no CMS signing-time
attribute** — PAdES takes the claimed time from the dictionary's `/M` and forbids a second,
possibly disagreeing one. B-LT (embedded revocation data for long-term validation) is not
in M5.

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
  form". A `/Lock` the document's author put there is not enacted either, and that is a
  gap, not a feature — it is listed under Not yet.
- The digest follows the key: SHA-256, or SHA-384 for a P-384 key (SHA-512 above that), so
  the hash never becomes the weak half of the pair.
- `--tsa URL` timestamps the signature value over HTTP or HTTPS. The reply's imprint and
  nonce are checked, so a replayed response is refused, and the token's own signature is
  verified against the certificate it carries. Whether that authority is *trusted* is
  decided later, at verification, like any other signer.

## Verifying

`leht verify FILE [--trust CA.pem]... [--json]`, and in the viewer a Signatures panel plus a
banner on every signed document.

Four things are reported, and kept apart on purpose:

| Question | What answers it |
|---|---|
| Are these the bytes that were signed? | The message digest over the `/ByteRange`, and the signature over the signed attributes |
| Whose key was it? | The signer's certificate, with `signing-certificate-v2` checked so a substituted certificate with the same key does not pass |
| Do we have any reason to believe that name? | A chain to the system CA bundle plus certificates you added — reported as trusted, not trusted, expired or not yet valid |
| Is what I am looking at what was signed? | Whether the file grew after the signed revision, and whether a later signature covers those bytes too |

Exit codes say the same thing to scripts: **0** all valid and trusted, **4** something is
broken, **5** intact but the signer is not trusted, **6** intact and trusted but the
document was added to afterwards.

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

**Trust is the system's, plus yours.** Fedora's `/etc/pki/tls/certs/ca-bundle.crt` (or the
Debian, openSUSE location), plus certificates added with `--trust` or the viewer's *Trust a
Certificate…*. The EU Trusted List, which is what makes eIDAS qualified signatures
verifiable as such, is **not** consulted: an Estonian ID-card signature will verify as
intact and, unless you add the right roots yourself, as untrusted.

**Revocation is not checked.** A certificate revoked after signing verifies here. That is
PAdES B-LT territory.

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
- A **test PKI is generated at run time** — a root, RSA-2048 and ECDSA P-384 signers, an
  expired certificate, one whose key usage forbids signing — so no private key is ever
  committed. So is a **local RFC 3161 timestamp authority**, which makes B-T testable
  without the network, including a TSA that answers with the wrong nonce.
- Tampering is checked from both ends: a flipped byte inside the signed range, and a
  `/ByteRange` edited to point elsewhere.

## In the viewer

The **Sign** tool drags a box for a visible signature; *More → Sign Invisibly…* skips the
box. The dialog takes the `.p12` and its password, what the signature should show — the
name and date, an image, or a signature **drawn** on a small canvas — the reason and
location, and optionally a timestamp authority (remembered between sessions).

Signing is a save: any unsaved edits go into the same revision the signature covers, the
signed file becomes the document, and the edit log restarts from it. **Undo does not reach
back past a signature**, which is as it should be — undoing into a signed revision could
only invalidate it.

The Signatures panel gives each signature one line — *Valid*, *Intact, signer not trusted*,
*Intact, but the document was changed afterwards*, *Broken* — and the details under it:
signer, issuer, trust, algorithm, claimed time, timestamp, reason, location and the
certificate's SHA-256 fingerprint.

## Not yet

- **PKCS#11**: an Estonian ID-card, or any smartcard, through OpenSC. The `Identity` type
  is the seam — OpenSSL 3 presents a token key as an ordinary `EVP_PKEY`, so a
  `from_pkcs11()` factory is all the PDF side would need. Nothing else changes.
- **B-LT / LTV**: a DSS dictionary with OCSP and CRL data, and document timestamps, so a
  signature stays verifiable after its certificate expires or is revoked.
- **The EU Trusted List**, which is what "qualified" means in practice.
- **Enacting a field's `/Lock`**, and DocMDP certification signatures ("no changes
  allowed").
- **Encrypted documents** cannot be signed: decrypt first.
