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
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "blink/assert.h"
#include "blink/bitscan.h"
#include "blink/debug.h"
#include "blink/endian.h"
#include "blink/likely.h"
#include "blink/machine.h"
#include "blink/macros.h"
#include "blink/mop.h"
#include "blink/pml4t.h"
#include "blink/stats.h"

// returns 0x80 for each byte that's equal
static u64 CompareEq(u64 x, u64 y) {
  u64 w = x ^ y;
  return (w = ~w & (w - 0x0101010101010101) & 0x8080808080808080);
}

void SetReadAddr(struct Machine *m, i64 addr, u32 size) {
  if (size) {
    m->readaddr = addr;
    m->readsize = size;
  }
}

void SetWriteAddr(struct Machine *m, i64 addr, u32 size) {
  if (size) {
    m->writeaddr = addr;
    m->writesize = size;
  }
}

u8 *GetPageAddress(struct System *s, u64 entry) {
  unassert(entry & PAGE_V);
  unassert(~entry & PAGE_RSRV);
  if (entry & PAGE_HOST) {
    return ToHost(entry & PAGE_TA);
  } else if ((entry & PAGE_TA) + 4096 <= kRealSize) {
    return s->real + (entry & PAGE_TA);
  } else {
    return 0;
  }
}

u64 HandlePageFault(struct Machine *m, u64 entry, u64 table, unsigned index) {
  u64 x;
  u64 page;
  if ((page = AllocatePage(m->system)) != -1) {
    --m->system->memstat.reserved;
    ++m->system->memstat.committed;
    x = (page & (PAGE_TA | PAGE_HOST | PAGE_MAP)) |
        (entry & ~(PAGE_TA | PAGE_RSRV));
    Store64(GetPageAddress(m->system, table) + index * 8, x);
    return x;
  } else {
    return 0;
  }
}

static inline u64 GetTlbKey(i64 page) {
  return (page & 0xff000) >> 12;
}

static void SetTlbEntry(struct Machine *m, unsigned i, struct TlbEntry e) {
  m->tlb.key[i / 8] = (m->tlb.key[i / 8] & ~(0xFFull << (i % 8 * 8))) |
                      GetTlbKey(e.page) << (i % 8 * 8);
  m->tlb.entry[i] = e;
}

static u64 GetTlbEntry(struct Machine *m, i64 page) {
  unsigned i;
  if (m->tlb.entry[0].page == page) {
    STATISTIC(++tlb_hits_1);
    return m->tlb.entry[0].entry;
  }
#if defined(__GNUC__) && defined(__x86_64__)
  typedef unsigned char tlb_key_t
      __attribute__((__vector_size__(16), __aligned__(16)));
  u8 k = GetTlbKey(page);
  i = __builtin_ia32_pmovmskb128(
      *(tlb_key_t *)m->tlb.key ==
      (tlb_key_t){k, k, k, k, k, k, k, k, k, k, k, k, k, k, k, k});
  while (i) {
    unsigned j = bsr(i);
    if (m->tlb.entry[j].page == page) {
      STATISTIC(++tlb_hits_2);
      unassert(j > 0);
      struct TlbEntry e = m->tlb.entry[j];
      SetTlbEntry(m, j, m->tlb.entry[j - 1]);
      SetTlbEntry(m, j - 1, e);
      return e.entry;
    }
    i &= ~(1 << j);
  }
#else
  for (i = 0; i < kTlbEntries / 8; ++i) {
    u64 key = CompareEq(m->tlb.key[i], GetTlbKey(page) * 0x0101010101010101);
    while (key) {
      unsigned j = bsr(key) >> 3;
      unsigned k = i * 8 + j;
      if (m->tlb.entry[k].page == page) {
        STATISTIC(++tlb_hits_2);
        unassert(k > 0);
        struct TlbEntry e = m->tlb.entry[k];
        SetTlbEntry(m, k, m->tlb.entry[k - 1]);
        SetTlbEntry(m, k - 1, e);
        return e.entry;
      }
      key &= ~(0xFFull << (j * 8));
    }
  }
#endif
  return 0;
}

static u64 FindPageTableEntry(struct Machine *m, i64 page) {
  i64 table;
  u64 entry;
  unsigned level, index;
  _Static_assert(IS2POW(kTlbEntries), "");
  _Static_assert(kTlbEntries % 8 == 0, "");
  if (!atomic_load_explicit(&m->invalidated, memory_order_relaxed)) {
    if ((entry = GetTlbEntry(m, page))) {
      return entry;
    }
  } else {
    ResetTlb(m);
    atomic_store_explicit(&m->invalidated, false, memory_order_relaxed);
  }
  if (!(-0x800000000000 <= page && page < 0x800000000000)) return 0;
  STATISTIC(++tlb_misses);
  unassert((entry = m->system->cr3));
  level = 39;
  do {
    table = entry;
    index = (page >> level) & 511;
    entry = Load64(GetPageAddress(m->system, table) + index * 8);
    if (!(entry & PAGE_V)) return 0;
  } while ((level -= 9) >= 12);
  if ((entry & PAGE_RSRV) &&
      !(entry = HandlePageFault(m, entry, table, index))) {
    return 0;
  }
  SetTlbEntry(m, kTlbEntries - 1, (struct TlbEntry){page, entry});
  return entry;
}

u8 *LookupAddress(struct Machine *m, i64 virt) {
  u8 *host;
  u64 entry;
  if (m->mode != XED_MODE_REAL) {
    if (!(entry = FindPageTableEntry(m, virt & -4096))) return 0;
  } else if (virt >= 0 && virt <= 0xffffffff &&
             (virt & 0xffffffff) + 4095 < kRealSize) {
    return m->system->real + virt;
  } else {
    return 0;
  }
  if ((host = GetPageAddress(m->system, entry))) {
    return host + (virt & 4095);
  } else {
    return 0;
  }
}

u8 *GetAddress(struct Machine *m, i64 v) {
  if (HasLinearMapping(m)) return ToHost(v);
  return LookupAddress(m, v);
}

u8 *ResolveAddress(struct Machine *m, i64 v) {
  u8 *r;
  if ((r = GetAddress(m, v))) return r;
  ThrowSegmentationFault(m, v);
}

void VirtualCopy(struct Machine *m, i64 v, char *r, u64 n, bool d) {
  u8 *p;
  u64 k;
  k = 4096 - (v & 4095);
  while (n) {
    k = MIN(k, n);
    p = ResolveAddress(m, v);
    if (d) {
      memcpy(r, p, k);
    } else {
      memcpy(p, r, k);
    }
    n -= k;
    r += k;
    v += k;
    k = 4096;
  }
}

u8 *CopyFromUser(struct Machine *m, void *dst, i64 src, u64 n) {
  VirtualCopy(m, src, (char *)dst, n, true);
  return (u8 *)dst;
}

void CopyFromUserRead(struct Machine *m, void *dst, i64 addr, u64 n) {
  CopyFromUser(m, dst, addr, n);
  SetReadAddr(m, addr, n);
}

void CopyToUser(struct Machine *m, i64 dst, void *src, u64 n) {
  VirtualCopy(m, dst, (char *)src, n, false);
}

void CopyToUserWrite(struct Machine *m, i64 addr, void *src, u64 n) {
  CopyToUser(m, addr, src, n);
  SetWriteAddr(m, addr, n);
}

void CommitStash(struct Machine *m) {
  unassert(m->stashaddr);
  if (m->opcache->writable) {
    CopyToUser(m, m->stashaddr, m->opcache->stash, m->opcache->stashsize);
  }
  m->stashaddr = 0;
}

u8 *ReserveAddress(struct Machine *m, i64 v, size_t n, bool writable) {
  m->reserving = true;
  if ((v & 4095) + n <= 4096) {
    return ResolveAddress(m, v);
  }
  STATISTIC(++page_overlaps);
  m->stashaddr = v;
  m->opcache->stashsize = n;
  m->opcache->writable = writable;
  u8 *r = m->opcache->stash;
  CopyFromUser(m, r, v, n);
  return r;
}

u8 *AccessRam(struct Machine *m, i64 v, size_t n, void *p[2], u8 *tmp,
              bool copy) {
  u8 *a, *b;
  unsigned k;
  unassert(n <= 4096);
  if ((v & 4095) + n <= 4096) {
    return ResolveAddress(m, v);
  }
  STATISTIC(++page_overlaps);
  k = 4096;
  k -= v & 4095;
  unassert(k <= 4096);
  a = ResolveAddress(m, v);
  b = ResolveAddress(m, v + k);
  if (copy) {
    memcpy(tmp, a, k);
    memcpy(tmp + k, b, n - k);
  }
  p[0] = a;
  p[1] = b;
  return tmp;
}

u8 *Load(struct Machine *m, i64 v, size_t n, u8 *b) {
  void *p[2];
  SetReadAddr(m, v, n);
  return AccessRam(m, v, n, p, b, true);
}

u8 *BeginStore(struct Machine *m, i64 v, size_t n, void *p[2], u8 *b) {
  SetWriteAddr(m, v, n);
  return AccessRam(m, v, n, p, b, false);
}

u8 *BeginStoreNp(struct Machine *m, i64 v, size_t n, void *p[2], u8 *b) {
  if (!v) return NULL;
  return BeginStore(m, v, n, p, b);
}

u8 *BeginLoadStore(struct Machine *m, i64 v, size_t n, void *p[2], u8 *b) {
  SetWriteAddr(m, v, n);
  return AccessRam(m, v, n, p, b, true);
}

void EndStore(struct Machine *m, i64 v, size_t n, void *p[2], u8 *b) {
  unsigned k;
  unassert(n <= 4096);
  if ((v & 4095) + n <= 4096) return;
  k = 4096;
  k -= v & 4095;
  unassert(k > n);
  unassert(p[0]);
  unassert(p[1]);
  memcpy(p[0], b, k);
  memcpy(p[1], b + k, n - k);
}

void EndStoreNp(struct Machine *m, i64 v, size_t n, void *p[2], u8 *b) {
  if (v) EndStore(m, v, n, p, b);
}

char *LoadStr(struct Machine *m, i64 addr) {
  size_t have;
  char *copy, *page, *p;
  have = 4096 - (addr & 4095);
  if (!addr) return 0;
  if (!(page = (char *)LookupAddress(m, addr))) return 0;
  if ((p = (char *)memchr(page, '\0', have))) {
    SetReadAddr(m, addr, p - page + 1);
    return page;
  }
  if (!(copy = (char *)malloc(have + 4096))) return 0;
  memcpy(copy, page, have);
  for (;;) {
    if (!(page = (char *)LookupAddress(m, addr + have))) break;
    if ((p = (char *)memccpy(copy + have, page, '\0', 4096))) {
      SetReadAddr(m, addr, have + (p - (copy + have)) + 1);
      m->freelist.p = (void **)realloc(
          m->freelist.p, ++m->freelist.n * sizeof(*m->freelist.p));
      return (char *)(m->freelist.p[m->freelist.n - 1] = copy);
    }
    have += 4096;
    if (!(p = (char *)realloc(copy, have + 4096))) break;
    copy = p;
  }
  free(copy);
  return 0;
}

char **LoadStrList(struct Machine *m, i64 addr) {
  int n;
  u8 b[8];
  char **list;
  for (list = 0, n = 0;;) {
    list = (char **)realloc(list, ++n * sizeof(*list));
    CopyFromUserRead(m, b, addr + n * 8 - 8, 8);
    if (Read64(b)) {
      list[n - 1] = (char *)LoadStr(m, Read64(b));
    } else {
      list[n - 1] = 0;
      break;
    }
  }
  return list;
}
