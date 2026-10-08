# License and attribution notices

## Project license

Copyright (c) 2026 Niklaus1911 and contributors.

Except for third-party material identified below or in individual files, the original source code, web assets, build scripts, tests, and documentation in this repository are licensed under **GPL-3.0-or-later**.

This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License in [LICENSE](LICENSE) for more details.

## rc-switch protocol definitions

The protocol timing definitions in `components/rf_codec/rf_codec.cpp` are adapted from [sui77/rc-switch](https://github.com/sui77/rc-switch).

- Copyright (c) 2011 Suat Özgür.
- Original and adapted protocol definitions: LGPL-2.1-or-later.
- Adaptation: definitions transcribed into the native fixed-size codec table in 2026; Arduino GPIO and interrupt code is not included.
- Attribution and modification notice: [components/rf_codec/NOTICE.md](components/rf_codec/NOTICE.md).
- License text: [components/rf_codec/COPYING.LESSER](components/rf_codec/COPYING.LESSER).

The original third-party licensing rights are preserved. The project license applies to the independently written firmware code.

## linenoise

`components/rf_console/rf_console_linenoise.c` and `components/rf_console/rf_console_linenoise.h` are adapted from [antirez/linenoise](https://github.com/antirez/linenoise), under BSD-2-Clause. The original notices remain in both files. The C source attributes Salvatore Sanfilippo's contribution to 2010–2016; the header attributes it to 2010–2014. Both attribute Pieter Noordhuis's contribution to 2010–2013.

The project adaptation in 2026 adds console-output coordination and masked input. The upstream BSD terms are reproduced here for source and binary distributions:

```text
Copyright (c) 2010-2016, Salvatore Sanfilippo <antirez at gmail dot com>
Copyright (c) 2010-2013, Pieter Noordhuis <pcnoordhuis at gmail dot com>

All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

 *  Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.

 *  Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## External build dependencies and firmware releases

ESP-IDF and components obtained through the ESP-IDF component manager retain their own licenses and copyright notices. Their license texts are supplied with their source distributions; this repository does not replace those terms.

When distributing firmware binaries, provide the matching Corresponding Source and necessary build instructions in accordance with the GPL and applicable dependency licenses. Include this notice, the GPL and LGPL license texts, and the applicable dependency notices with the distribution. Identify the exact source revision used for each binary release.
