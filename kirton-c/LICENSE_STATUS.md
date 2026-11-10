# Kirton license status

Kirton Core, the interpreter, runtime, and public libraries are distributed
under the Mozilla Public License 2.0 (MPL-2.0). The complete license text is in
[`LICENSE`](LICENSE), and Kirton source files carry
`SPDX-License-Identifier: MPL-2.0` notices where applicable.

## Source and contribution provenance

The repository history available during the 1.3.0 release audit contains the
stable `v1.2.0` commit. Kirton 1.3.0 source changes were present in the working
tree and therefore are not attributable through the existing commit history.
Before public distribution, confirm that the person or organization publishing
the release has the necessary rights to distribute all contributed changes
under MPL-2.0, including any contractor or earlier-contributor work. No
individual copyright holder is asserted by this file.

## Third-party components

`third_party/stb_image.h` is a trimmed header identifying itself as
`stb_image` 2.28. The upstream project offers its components under public
domain terms or the MIT license; this Kirton copy retains its upstream
identification and is not relicensed as MPL-2.0. See
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) for the notice and upstream
reference.

Windows system libraries and the platform C runtime are toolchain/operating
system components. The source inventory found no other vendored dependency in
`third_party/`. This summary is not legal advice and does not replace review of
the provenance of all code being distributed.
