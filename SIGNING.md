# Verifying releases

Every release from version 1.1.0 onwards ships two files alongside the
packages:

| File | What it is |
|---|---|
| `SHA256SUMS.txt` | SHA-256 checksum of every file in the release |
| `SHA256SUMS.txt.asc` | The maintainer's detached OpenPGP signature over `SHA256SUMS.txt` |

Verifying is a two-step chain: the **signature** proves `SHA256SUMS.txt`
came from the maintainer and hasn't been altered, and the **checksums**
prove your download matches it. Checking a checksum alone only proves the
file matches whatever list sat next to it on the same page, so do both.

Releases before 1.1.0 (including 1.0.0) were not signed; only their
checksums are available.

## The signing key

| | |
|---|---|
| Owner | rightiswrong `<rightiswrong@protonmail.com>` |
| Primary key fingerprint | `C24A C72B AAD8 552D 5048  C890 1BF0 ABA8 6493 ADA3` |
| Algorithm | ed25519 (signing subkey held on a hardware token) |

The public key is in [`KEYS.asc`](KEYS.asc) in this repository and on
[keys.openpgp.org](https://keys.openpgp.org/). **Always compare the
fingerprint** that GnuPG prints with the one above — and, ideally, with one
obtained another way (e.g. the maintainer's GitHub profile) — before trusting
it.

The signing subkey expires periodically and is extended by the maintainer;
if GnuPG says the key has expired, re-import a fresh copy of `KEYS.asc`.

## One-time setup: import the key

On Windows, install [Gpg4win](https://www.gpg4win.org/) (verify its own
download as described on that site). Then, in PowerShell:

```powershell
Invoke-WebRequest -OutFile KEYS.asc https://raw.githubusercontent.com/rightiswrong/sp410-opensource-windows/main/KEYS.asc
gpg --import KEYS.asc
gpg --fingerprint rightiswrong@protonmail.com     # compare with the fingerprint above
```

On Linux/macOS use the same `gpg` commands with `curl -fsSLO <url>`.

## Verifying a download

Put the installer, `SHA256SUMS.txt` and `SHA256SUMS.txt.asc` in one folder
(e.g. `Downloads`), open PowerShell there, then:

```powershell
# 1. Check the signature on the checksum list
gpg --verify SHA256SUMS.txt.asc SHA256SUMS.txt
```

You should see `Good signature from "rightiswrong <rightiswrong@protonmail.com>"`
and a `Primary key fingerprint:` line matching the one above.

```powershell
# 2. Check the installer against the list
$file = Get-ChildItem sp410-opensource-windows-*-setup.exe | Select-Object -First 1
$expected = (Select-String -Path SHA256SUMS.txt -SimpleMatch $file.Name).Line.Split(' ')[0]
$actual   = (Get-FileHash $file -Algorithm SHA256).Hash.ToLower()
if ($actual -eq $expected) { "OK  $($file.Name)" } else { "FAILED  $($file.Name)" }
```

It should print `OK`. On Linux/macOS:
`sha256sum --check --ignore-missing SHA256SUMS.txt`.

### Authenticode (once releases are code-signed)

When releases carry an Authenticode signature, you can also check it:
right-click the installer → **Properties → Digital Signatures**, or

```powershell
Get-AuthenticodeSignature .\sp410-opensource-windows-*-setup.exe | Format-List Status, SignerCertificate
```

`Status` should be `Valid`. This complements, but does not replace, the
OpenPGP check above.

## If verification fails

- **`BAD signature`** — the checksum file was altered or corrupted. Do not
  install. Re-download it from the GitHub release page; if it still fails,
  please open an issue.
- **`Can't check signature: No public key`** — you haven't imported
  `KEYS.asc` yet (see above).
- **`WARNING: This key is not certified with a trusted signature!`** — this
  is normal. It means you haven't personally certified the key. What matters
  is that the line above it says `Good signature` and the fingerprint matches
  the one in this document.
- **A checksum `FAILED`** — the download is incomplete or tampered with.
  Delete it and download again.

## Reporting a problem

If a release is signed by a key with a different fingerprint, or you have
any reason to think the key has been compromised, please report it privately
via GitHub's **Security → Report a vulnerability** on this repository rather
than in a public issue.

## For maintainers: signing a release

1. Run the release workflow as usual and wait for it to publish.
2. Download `SHA256SUMS.txt` from the release and check it matches the
   workflow log.
3. Sign it with the hardware token (PIN + touch):
   ```bash
   gpg --local-user C24AC72BAAD8552D5048C8901BF0ABA86493ADA3 --armor --detach-sign SHA256SUMS.txt
   gpg --verify SHA256SUMS.txt.asc SHA256SUMS.txt
   ```
4. Upload the signature:
   ```bash
   gh release upload vX.Y.Z SHA256SUMS.txt.asc
   ```
