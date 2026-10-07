# Security policy

bl2hdr is a DLL that loads into the Borderlands 2 process, patches game functions in memory and creates
Direct3D 12 / DXGI objects. Security problems are taken seriously.

## Supported versions
Only the latest release receives fixes.

## Reporting a vulnerability
Please **do not open a public issue**. Use GitHub's private reporting instead:
**Security → Report a vulnerability** on this repository
(<https://github.com/raspy-corroded3/borderlands2-native-hdr/security/advisories/new>).

Include what you found, how to reproduce it, and the affected version (shown in the first line of
`bl2hdr.log`). You will get an answer as soon as possible; please allow time for a fix before
disclosing publicly.

Examples of what to report: ways a crafted `bl2hdr.ini` or game file could make the DLL run arbitrary
code, memory-safety bugs in the hooks or parsers, or release artifacts that do not match the source.

## Verifying a download
Release builds are produced by GitHub Actions from a tagged commit, and each release lists the SHA-256
of its files and carries a build attestation. To check that a `d3d9.dll` was built from this repository
by its CI (requires the GitHub CLI):
```
gh attestation verify d3d9.dll --repo raspy-corroded3/borderlands2-native-hdr
```
Each release is also scanned on VirusTotal automatically; its notes link to the live reports, and the
README badge shows the latest result ([VIRUSTOTAL.md](https://github.com/raspy-corroded3/borderlands2-native-hdr/blob/badges/VIRUSTOTAL.md)).
Only download the DLL from this repository's Releases page or the official
[Nexus Mods page](https://www.nexusmods.com/borderlands2/mods/685) (uploaded from the same release build).
