#include "musyx/platform.h"

#if MUSY_TARGET == MUSY_TARGET_PC

#include "musyx/assert.h"
#include "musyx/hardware.h"
#include "musyx/sal.h"
#include "musyx/seq.h"
#include "musyx/synth.h"
#include "musyx/synthdata.h"

#include <stdint.h>
#include <string.h>

/* MusyX authoring tools emit big-endian project, pool and sample-directory
 * data, because the runtime was written for GameCube. On a little-endian host
 * every multi-byte field has to be swapped before the data is walked.
 *
 * Everything here runs exactly once per freshly loaded buffer, from
 * sndPushGroup(). That is safe because a client re-reads a group's data from
 * storage ahead of each push; swapping a buffer twice would undo the first
 * pass, so these must not be called on data already in host order.
 *
 * The sample directory additionally changes shape: on disc an entry is the
 * 0x20-byte SDIR_DATA_INTER, but SDIR_DATA holds a real pointer in `addr`, so
 * on a 64-bit host it is wider. salSdirToHost() returns a converted copy
 * rather than editing in place, since the on-disc entries sit inside a
 * caller-owned buffer with no room to grow.
 */

static u16 Swap16(u16 v) { return (u16)((v >> 8) | (v << 8)); }

static u32 Swap32(u32 v) {
  return ((v >> 24) & 0x000000FF) | ((v >> 8) & 0x0000FF00) | ((v << 8) & 0x00FF0000) |
         ((v << 24) & 0xFF000000);
}

#define SWAP16(x) ((x) = Swap16(x))
#define SWAP32(x) ((x) = Swap32(x))

/* Reference lists are u16 IDs terminated by 0xFFFF, optionally using 0x8000 to
 * introduce an inclusive range. 0xFFFF is byte-order invariant, so the
 * terminator can be spotted without swapping first. */
static void SwapIDList(u16* ref) {
  if (ref == NULL) {
    return;
  }

  while (*ref != 0xFFFF) {
    SWAP16(*ref);
    ++ref;
  }
}

/* Program tables: one entry per MIDI program, terminated by index 0xFF. */
static void SwapPageTable(PAGE* page) {
  if (page == NULL) {
    return;
  }
  for (; page->index != 0xFF; ++page) {
    SWAP16(page->macro);
  }
}

/* Per-song MIDI channel setup, terminated by songId 0xFFFF. The channel
 * entries behind it are all single bytes. */
static void SwapMidiSetup(MIDISETUP* ms) {
  if (ms == NULL) {
    return;
  }
  /* 0xFFFF reads the same in either order, so the end is found either way. */
  for (; ms->songId != 0xFFFF; ++ms) {
    SWAP16(ms->songId);
  }
}

/* An FX group keeps its effect table at the first of the trailing offsets;
 * sndFXStart() looks entries up by id, so it has to be in host order before
 * any effect can start. */
static void SwapFXTable(FX_DATA* fd) {
  u16 n;
  u16 i;

  if (fd == NULL) {
    return;
  }

  SWAP16(fd->num);
  n = fd->num;
  for (i = 0; i < n; ++i) {
    SWAP16(fd->fx[i].id);
    SWAP16(fd->fx[i].macro);
    /* the remaining fields are single bytes */
  }
}

void salSwapProjectData(void* prj) {
  GROUP_DATA* g;

  if (prj == NULL) {
    return;
  }

  for (g = (GROUP_DATA*)prj;;) {
    SWAP32(g->nextOff);
    if (g->nextOff == 0xFFFFFFFF) {
      /* Sentinel node: only nextOff is meaningful. */
      break;
    }

    SWAP16(g->id);
    SWAP16(g->type);
    SWAP32(g->macroOff);
    SWAP32(g->sampleOff);
    SWAP32(g->curveOff);
    SWAP32(g->keymapOff);
    SWAP32(g->layerOff);

    /* Which of the trailing offsets are live depends on the group type, and
     * swapping ones the group does not own would corrupt whatever follows.
     * Type 1 is an effect group and uses only the first, as its effect table
     * (see InsertFXTab); type 0 is a song group and uses all three (see
     * seqPlaySong). */
    if (g->type == 1) {
      SWAP32(g->data.song.normpageOff);
      SwapFXTable((FX_DATA*)((u8*)prj + g->data.song.normpageOff));
    } else {
      SWAP32(g->data.song.normpageOff);
      SWAP32(g->data.song.drumpageOff);
      SWAP32(g->data.song.midiSetupOff);
      SwapPageTable((PAGE*)((u8*)prj + g->data.song.normpageOff));
      SwapPageTable((PAGE*)((u8*)prj + g->data.song.drumpageOff));
      SwapMidiSetup((MIDISETUP*)((u8*)prj + g->data.song.midiSetupOff));
    }

    SwapIDList((u16*)((u8*)prj + g->sampleOff));
    SwapIDList((u16*)((u8*)prj + g->macroOff));
    SwapIDList((u16*)((u8*)prj + g->curveOff));
    SwapIDList((u16*)((u8*)prj + g->keymapOff));
    SwapIDList((u16*)((u8*)prj + g->layerOff));

    g = (GROUP_DATA*)((u8*)prj + g->nextOff);
  }
}

/* Pool sections are MEM_DATA chains linked by nextOff, which doubles as the
 * size of the node -- that is what bounds each variable-length payload. */
typedef enum POOL_SECTION {
  POOL_SECTION_MACRO,
  POOL_SECTION_CURVE,
  POOL_SECTION_KEYMAP,
  POOL_SECTION_LAYER
} POOL_SECTION;

static void SwapPoolChain(MEM_DATA* m, POOL_SECTION section) {
  u32 payload;
  u32 i;
  u32 n;

  if (m == NULL) {
    return;
  }

  for (;;) {
    SWAP32(m->nextOff);
    if (m->nextOff == 0xFFFFFFFF) {
      break;
    }

    SWAP16(m->id);
    payload = m->nextOff - offsetof(MEM_DATA, data);

    switch (section) {
    case POOL_SECTION_MACRO: {
      /* MSTEPs are pairs of u32 words holding packed command parameters. */
      u32* w = (u32*)&m->data.cmd[0][0];
      n = payload / sizeof(u32);
      for (i = 0; i < n; ++i) {
        SWAP32(w[i]);
      }
      break;
    }

    case POOL_SECTION_KEYMAP: {
      n = payload / sizeof(KEYMAP);
      for (i = 0; i < n; ++i) {
        SWAP16(m->data.map[i].id);
        m->data.map[i].prioOffset = (s16)Swap16((u16)m->data.map[i].prioOffset);
      }
      break;
    }

    case POOL_SECTION_LAYER: {
      SWAP32(m->data.layer.num);
      n = m->data.layer.num;
      for (i = 0; i < n; ++i) {
        SWAP16(m->data.layer.entry[i].id);
        m->data.layer.entry[i].prioOffset =
            (s16)Swap16((u16)m->data.layer.entry[i].prioOffset);
      }
      break;
    }

    case POOL_SECTION_CURVE:
    default:
      /* Curve tables are plain bytes. */
      break;
    }

    m = (MEM_DATA*)((u8*)m + m->nextOff);
  }
}

void salSwapPoolData(void* pool) {
  POOL_DATA* p = pool;

  if (p == NULL) {
    return;
  }

  SWAP32(p->macroOff);
  SWAP32(p->curveOff);
  SWAP32(p->keymapOff);
  SWAP32(p->layerOff);

  /* A zero offset means the section is absent, not that it sits at the head of
   * the pool. The runtime never dereferences those because the matching ID list
   * is empty, but walking one here would run off into the header. */
  SwapPoolChain(p->macroOff ? (MEM_DATA*)((u8*)p + p->macroOff) : NULL, POOL_SECTION_MACRO);
  SwapPoolChain(p->curveOff ? (MEM_DATA*)((u8*)p + p->curveOff) : NULL, POOL_SECTION_CURVE);
  SwapPoolChain(p->keymapOff ? (MEM_DATA*)((u8*)p + p->keymapOff) : NULL, POOL_SECTION_KEYMAP);
  SwapPoolChain(p->layerOff ? (MEM_DATA*)((u8*)p + p->layerOff) : NULL, POOL_SECTION_LAYER);
}

SDIR_DATA* salSdirToHost(void* sdir) {
  SDIR_DATA_INTER* in = sdir;
  SDIR_DATA* out;
  u32 n;
  u32 i;

  if (in == NULL) {
    return NULL;
  }

  /* 0xFFFF reads the same either way, so the list can be measured before any
   * swapping happens. */
  for (n = 0; in[n].id != 0xFFFF; ++n) {
    ;
  }

  out = salMalloc((n + 1) * sizeof(SDIR_DATA));
  MUSY_ASSERT_MSG(out != NULL, "Could not allocate host-order sample directory");
  if (out == NULL) {
    return NULL;
  }

  for (i = 0; i < n; ++i) {
    out[i].id = Swap16(in[i].id);
    out[i].ref_cnt = Swap16(in[i].ref_cnt);
    out[i].offset = Swap32(in[i].offset);
    /* `addr` is filled in at insert time; the on-disc value is not used. */
    out[i].addr = NULL;
    out[i].header.info = Swap32(in[i].header.info);
    out[i].header.length = Swap32(in[i].header.length);
    out[i].header.loopOffset = Swap32(in[i].header.loopOffset);
    out[i].header.loopLength = Swap32(in[i].header.loopLength);
    out[i].extraData = Swap32(in[i].extraData);
  }

  memset(&out[n], 0, sizeof(SDIR_DATA));
  out[n].id = 0xFFFF;

  return out;
}

void salFreeHostSdir(SDIR_DATA* sdir) {
  if (sdir != NULL) {
    salFree(sdir);
  }
}

#endif
