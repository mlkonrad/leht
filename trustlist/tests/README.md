# Trusted-list test fixtures

`fixtures.tar.xz` holds three real trusted lists, exactly as published, so the
tests verify signatures that Leht did not make:

| File | Published by | Fetched from | Issued |
|---|---|---|---|
| `eu-lotl.xml` | European Commission (List of Trusted Lists, sequence 395) | https://ec.europa.eu/tools/lotl/eu-lotl.xml | 2026-09-24 |
| `estonian-tsl.xml` | Estonia (RIA) | https://sr.riik.ee/tsl/estonian-tsl.xml | fetched 2026-09-27 |
| `TL-DE.xml` | Germany (Bundesnetzagentur); RSA-PSS signed | https://tl.bundesnetzagentur.de/TL-DE.xml | fetched 2026-09-27 |

They are public documents, published for reuse by relying parties. The tests
treat them as frozen: they check signatures and structure, never dates against
today, so the fixtures do not expire.
