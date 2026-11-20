`miniz.c` and `miniz.h` are the unchanged amalgamated sources of miniz
3.1.2 by Rich Geldreich, from the release asset `miniz-3.1.2.zip`
(SHA-256 `F0446D863F9C19926AD9483C523FDC42E42B8D4A6A431D27E09D49C79A140D9A`),
retrieved on 2026-11-10. The upstream license is retained in `LICENSE`.

| File | SHA-256 |
|---|---|
| `miniz.c` | `E2C1AEB66EEF9191D8C3FEB164DB2DEF2335A61D039BF04ED849F6B042433B30` |
| `miniz.h` | `B53B62ED122E559B8F679E3CB787A0B0035FE87A58F909DA0E44931678F4E85F` |

EDVR uses its zlib-format deflate to compress the eye dump's binary files
(`edvr_logs\eyes\*.bin` and the draw ledger in `edvr_logs\pool\`), which
`tools\dump_io.py` reads with Python's standard `zlib` module.

The graphics DLL embeds the copyright notice and license as RCDATA resources
40067 and 40068 (`notice.rc`, ours). Release archives also carry the license
as `MINIZ-LICENSE.txt`.

Source:
https://github.com/richgel999/miniz/releases/tag/3.1.2
