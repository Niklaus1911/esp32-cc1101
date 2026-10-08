# Protocol compatibility notice

The 12 protocol timing definitions in this component follow the protocol numbering published by the [`sui77/rc-switch`](https://github.com/sui77/rc-switch) project.

- Upstream copyright: Copyright (c) 2011 Suat Özgür.
- Adapted portion license: LGPL-2.1-or-later.
- Modification notice: protocol definitions were transcribed into the native fixed-size codec table and independently tested for this project in 2026.
- License text: `COPYING.LESSER`.

The GPIO/interrupt implementation from rc-switch is not included. This component provides an independently structured ESP-IDF RMT transport and codec so pulse generation/capture is hardware-timed rather than GPIO-bit-banged. Protocol names and timing factors remain compatible to make captured values familiar to rc-switch users.

The adapted portion is provided without warranty, as described by the LGPL. The independently written firmware code is licensed under GPL-3.0-or-later; see the root `LICENSE` and `NOTICE.md`. The original LGPL rights and notices for the adapted protocol definitions are preserved.
