# Third-party notices

HoustonKVM is licensed under the Apache License 2.0 (`LICENSE`, `NOTICE`). This
file lists the software it is built from, and reproduces the copyright notices
and license terms that software asks to travel with it.

## The web interface

`ui/` contains no third-party code: no framework, design system or icon set.
The HTML, CSS and JavaScript are all written for this project, and form
controls are the browser's own.

It does carry two fonts, each the complete variable font exactly as its
authors publish it, served by HoustonKVM itself rather than fetched from the
internet. Each one's licence is beside it in `ui/fonts/`, and the RPM
installs both licences:

| Font | File | Taken from | License |
|---|---|---|---|
| Playfair Display (The Playfair Display Project Authors) | `ui/fonts/playfair-display.ttf` | `ofl/playfairdisplay/PlayfairDisplay[wght].ttf` in github.com/google/fonts | OFL-1.1 |
| Source Sans 3 (Adobe) | `ui/fonts/source-sans-3.woff2` | `WOFF2/VF/SourceSans3VF-Upright.ttf.woff2` in Adobe's release 3.052R, github.com/adobe-fonts/source-sans | OFL-1.1 |

## Libraries the server is built from

HoustonKVM does not modify any of these. `houstonkvm.spec` pins each bundled one
to an exact upstream release or commit, and the archive named there is the
corresponding source. (`scripts/setup-centos9.sh`, used for building from source
by hand, fetches them at their latest upstream version instead.)

Compiled into the server binary:

| Library | Version | License |
|---|---|---|
| uSockets (uNetworking) | commit 86097c4 | Apache-2.0 |
| uWebSockets (uNetworking) | 20.79.0 | Apache-2.0 |
| nlohmann/json (header-only, from the system's `json-devel`) | as packaged | MIT |

For WebRTC video, the EL9 and Fedora packages use the system's
`libdatachannel` and bundle nothing below. Where the distribution has none
(EL10 for now), the package ships its own as a private shared library,
`libdatachannel.so`, in `/usr/lib64/houstonkvm/`:

| Library | Version | License |
|---|---|---|
| libdatachannel | 0.24.5 | MPL-2.0 |
| libjuice | 1.7.2 | MPL-2.0 |
| usrsctp | 0.9.5.0 | BSD-3-Clause |
| libsrtp | 2.8.0 | BSD-3-Clause |
| plog | 1.1.10 | MIT |
| nlohmann/json (libdatachannel's own copy) | commit 55f9368 | MIT |

libdatachannel and libjuice are used unmodified under the Mozilla Public License
2.0. Their source code is available from the upstream archives pinned in
`houstonkvm.spec`, and the license text is at <https://mozilla.org/MPL/2.0/>.
libjuice includes `picohash.h`, which its author placed in the public domain.

The uWebSockets source tree also brings libdeflate (MIT), but HoustonKVM's
build does not enable it, so none of its code is in the binary. uWebSockets
uses the system zlib instead.

Dynamically linked from the operating system and **not** distributed with
HoustonKVM: OpenSSL (Apache-2.0), libsodium (ISC), SQLite (public domain),
libjpeg-turbo (IJG and others), zlib (zlib), OpenH264 (BSD-2-Clause), alsa-lib
(LGPL-2.1-or-later) and Opus (BSD-3-Clause).

## H.264

HoustonKVM encodes H.264 through the system's OpenH264 library and does not
include it. On RHEL-family systems the RPM expects it from Cisco's binary
repository (`epel-cisco-openh264`), because Cisco pays the H.264 patent
royalties for the binaries it distributes itself. That coverage does not extend
to OpenH264 you compile yourself. See Cisco's OpenH264 binary license if this
matters for your use.

## License texts

The Apache License 2.0 that covers uSockets and uWebSockets is the same text as
HoustonKVM's own `LICENSE`. Neither project ships a `NOTICE` file.

### nlohmann/json (both copies)

```text
MIT License

Copyright (c) 2013-2025 Niels Lohmann

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### plog

```text
MIT License

Copyright (c) 2022 Sergey Podobry

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### usrsctp

```text
Copyright (c) 2015,  Randall Stewart and Michael Tuexen
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

* Neither the name of usrsctp nor the names of its
  contributors may be used to endorse or promote products derived from
  this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

### libsrtp

```text
Copyright (c) 2001-2017 Cisco Systems, Inc.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

  Redistributions of source code must retain the above copyright
  notice, this list of conditions and the following disclaimer.

  Redistributions in binary form must reproduce the above
  copyright notice, this list of conditions and the following
  disclaimer in the documentation and/or other materials provided
  with the distribution.

  Neither the name of the Cisco Systems, Inc. nor the names of its
  contributors may be used to endorse or promote products derived
  from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
OF THE POSSIBILITY OF SUCH DAMAGE.
```
