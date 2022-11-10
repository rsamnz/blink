/*-*- mode:c;indent-tabs-mode:nil;c-basic-offset:2;tab-width:8;coding:utf-8 -*-│
│vi: set net ft=c ts=2 sts=2 sw=2 fenc=utf-8                                :vi│
╞══════════════════════════════════════════════════════════════════════════════╡
│ Copyright 2022 Justine Alexandra Roberts Tunney                              │
│                                                                              │
│ Permission to use, copy, modify, and/or distribute this software for         │
│ any purpose with or without fee is hereby granted, provided that the         │
│ above copyright notice and this permission notice appear in all copies.      │
│                                                                              │
│ THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL                │
│ WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED                │
│ WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE             │
│ AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL         │
│ DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR        │
│ PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER               │
│ TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR             │
│ PERFORMANCE OF THIS SOFTWARE.                                                │
╚─────────────────────────────────────────────────────────────────────────────*/
#include "blink/address.h"
#include "blink/builtin.h"
#include "blink/endian.h"
#include "blink/modrm.h"
#include "blink/throw.h"
#include "blink/x86.h"

uint64_t AddressOb(struct Machine *m, uint32_t rde) {
  return AddSegment(m, rde, m->xedd->op.disp, m->ds);
}

uint8_t *GetSegment(struct Machine *m, uint32_t rde, int s) {
  switch (s & 7) {
    case 0:
      return m->es;
    case 1:
      return m->cs;
    case 2:
      return m->ss;
    case 3:
      return m->ds;
    case 4:
      return m->fs;
    case 5:
      return m->gs;
    case 6:
    case 7:
      OpUd(m, rde);
    default:
      __builtin_unreachable();
  }
}

uint64_t DataSegment(struct Machine *m, uint32_t rde, uint64_t i) {
  return AddSegment(m, rde, i, m->ds);
}

uint64_t AddressSi(struct Machine *m, uint32_t rde) {
  switch (Eamode(rde)) {
    case XED_MODE_LONG:
      return DataSegment(m, rde, Read64(m->si));
    case XED_MODE_REAL:
      return DataSegment(m, rde, Read16(m->si));
    case XED_MODE_LEGACY:
      return DataSegment(m, rde, Read32(m->si));
    default:
      __builtin_unreachable();
  }
}

uint64_t AddressDi(struct Machine *m, uint32_t rde) {
  uint64_t i = Read64(m->es);
  switch (Eamode(rde)) {
    case XED_MODE_LONG:
      return i + Read64(m->di);
    case XED_MODE_REAL:
      return i + Read16(m->di);
    case XED_MODE_LEGACY:
      return i + Read32(m->di);
    default:
      __builtin_unreachable();
  }
}