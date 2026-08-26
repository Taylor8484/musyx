#include "musyx/platform.h"

#if MUSY_TARGET == MUSY_TARGET_PC

#include "musyx/assert.h"
#include "musyx/hardware.h"
#include "musyx/sal.h"
#include "musyx/seq.h"
#include "musyx/stream.h"
#include "musyx/synth.h"
#include "musyx/synthdata.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The project, pool and sample-directory data this backend loads is
 * big-endian, so on a little-endian host every multi-byte field has to be
 * swapped before the data is walked.
 *
 * That is a property of the data, not of MusyX: the authoring tools do not
 * universally emit big-endian output. Byte order is decided by the SoundTool
 * export plugin the data was built with, and MusyX shipped on little-endian
 * targets too. These conversions therefore belong to loading GameCube-authored
 * data on a little-endian host -- which is what this backend does -- rather
 * than to the PC target as such. A PC build fed little-endian data would want
 * them skipped.
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

/* ---------------------------------------------------------------------------
 * Arrangement (song) data
 *
 * Unlike the project, pool and sample directory, an arrangement never passes
 * through sndPushGroup() -- the client loads it and hands the pointer straight
 * to sndSeqPlay(). So the conversion is exported instead, and the client calls
 * it once per freshly loaded arrangement (see src/msm/msmmus.c).
 *
 * The file has no table of contents, so the only way to know how long the
 * variable-length parts are is to walk them exactly the way the sequencer does:
 *
 *   ARR header  22 words, no byte fields.
 *   mTrack      MTRACK_DATA pairs, ended by a time of -1.
 *   tTab        64 track offsets; each track is TENTRY records ended by a
 *               pattern id of 0xFFFF, with 0xFFFE marking a loop back.
 *   pTab        pattern offsets. Its length is recorded nowhere, so only the
 *               entries the tracks actually name are converted -- the runtime
 *               never dereferences the others.
 *   patterns    a 3-word header followed by variable-width note records.
 *
 * The pitch-bend and modulation streams hanging off a pattern are deliberately
 * left alone: GetStreamValue() decodes them a byte at a time, so they carry no
 * byte order of their own.
 *
 * Every read is bounded by the stored size of the arrangement, so data that
 * does not match these assumptions fails the call rather than walking off the
 * buffer.
 */

typedef struct ARR_WALK {
  u8* base;
  u32 size;
  bool ok;
} ARR_WALK;

static u16 Read16At(const u8* p) { return (u16)(((u16)p[0] << 8) | p[1]); }

static u32 Read32At(const u8* p) {
  return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

/* Byte-wise so that an odd offset inside a pattern cannot trap. */
static void Swap16At(u8* p) {
  u8 t = p[0];
  p[0] = p[1];
  p[1] = t;
}

static void Swap32At(u8* p) {
  u8 t;
  t = p[0];
  p[0] = p[3];
  p[3] = t;
  t = p[1];
  p[1] = p[2];
  p[2] = t;
}

/* A zero offset means "absent" everywhere in this format, so it is rejected
 * here rather than silently aliasing the header. */
static u8* ArrAt(ARR_WALK* w, u32 offset, u32 len) {
  if (!w->ok) {
    return NULL;
  }
  if (offset == 0 || offset >= w->size || len > w->size - offset) {
    MUSY_REPORT("musyx: arrangement offset 0x%X (%u bytes) is outside the %u byte file.\n",
               (unsigned)offset, (unsigned)len, (unsigned)w->size);
    w->ok = false;
    return NULL;
  }
  return w->base + offset;
}

/* Note records are 6 bytes, except the two control forms the sequencer steps
 * over 4 bytes at a time (see GenerateNextTrackEvent). Only `time` -- and, for
 * a real note, `length` -- are multi-byte. */
static void SwapNoteData(ARR_WALK* w, u32 offset) {
  for (;;) {
    u8 key;
    u8 velocity;
    u8* p = ArrAt(w, offset, 4);

    if (p == NULL) {
      return;
    }

    key = p[2];
    velocity = p[3];
    if (key == 0xFF && velocity == 0xFF) {
      return; /* end of pattern */
    }

    Swap16At(p); /* time */

    if ((key & 0x80) != 0 || (key | velocity) == 0) {
      offset += 4;
      continue;
    }

    if (ArrAt(w, offset, 6) == NULL) {
      return;
    }
    Swap16At(w->base + offset + offsetof(NOTE_DATA, length));
    offset += 6;
  }
}

static void SwapPattern(ARR_WALK* w, u32 offset) {
  u8* p = ArrAt(w, offset, sizeof(SEQ_PATTERN));
  u32 pitchBend;
  u32 modulation;

  if (p == NULL) {
    return;
  }

  pitchBend = Read32At(p + offsetof(SEQ_PATTERN, pitchBend));
  modulation = Read32At(p + offsetof(SEQ_PATTERN, modulation));

  Swap32At(p + offsetof(SEQ_PATTERN, headerLen));
  Swap32At(p + offsetof(SEQ_PATTERN, pitchBend));
  Swap32At(p + offsetof(SEQ_PATTERN, modulation));

  /* The streams themselves need no conversion, but InitStream() will follow
   * these, so a bad offset has to be caught here. */
  if ((pitchBend != 0 && pitchBend >= w->size) || (modulation != 0 && modulation >= w->size)) {
    MUSY_REPORT("musyx: pattern at 0x%X has a stream offset outside the file.\n", (unsigned)offset);
    w->ok = false;
    return;
  }

  /* seqStartPlay's reader takes the note data to begin where the 3 word header
   * ends, so headerLen is informational. */
  SwapNoteData(w, offset + offsetof(SEQ_PATTERN, noteData));
}

static void SwapTrack(ARR_WALK* w, u32 offset, u8* seen) {
  for (;;) {
    u16 pattern;
    u8* p = ArrAt(w, offset, sizeof(TENTRY));

    if (p == NULL) {
      return;
    }

    pattern = Read16At(p + offsetof(TENTRY, pattern));
    Swap32At(p + offsetof(TENTRY, time));
    Swap16At(p + offsetof(TENTRY, pattern));

    if (pattern == 0xFFFF) {
      return; /* end of track */
    }

    if (pattern == 0xFFFE) {
      /* Loop entry: the bytes a normal record spends on transpose and
       * velocityAdd are a u16 index back into the track. It ends the track as
       * surely as 0xFFFF does -- the sequencer jumps back to that index and
       * never reads past here, so the next track begins immediately after. */
      Swap16At(p + offsetof(TENTRY, transpose));
      return;
    }

    seen[pattern >> 3] |= (u8)(1 << (pattern & 7));
    offset += sizeof(TENTRY);
  }
}

/* The ARR struct in seq.h is the 2.x layout: 16 loop points and a
 * track/section table. A 1.x file has a single section, so its header stops
 * after the first loop point -- and the track table starts immediately there,
 * which is why the difference matters. Sweeping sizeof(ARR) over 1.x data
 * swaps the first 16 track offsets a second time, undoing them. */
#if MUSY_VERSION >= MUSY_VERSION_CHECK(2, 0, 1)
#define ARR_HEADER_WORDS (sizeof(ARR) / sizeof(u32))
#else
#define ARR_HEADER_WORDS ((offsetof(ARR, loopPoint) / sizeof(u32)) + 1)
#endif

#define ARR_PATTERN_IDS 0x10000
#define ARR_SEEN_BYTES (ARR_PATTERN_IDS / 8)

bool sndSwapSongData(void* arrfile, u32 size) {
  ARR_WALK w;
  ARR* arr;
  u8* seen;
  u8* tTab;
  u32 i;

  if (arrfile == NULL || size < sizeof(ARR)) {
    MUSY_REPORT("musyx: arrangement is too small to be an arrangement (%u bytes).\n", (unsigned)size);
    return false;
  }

  /* One bit per pattern id -- a track may name the same pattern many times, and
   * converting one twice would put it back into big-endian order. */
  seen = salMalloc(ARR_SEEN_BYTES);
  if (seen == NULL) {
    MUSY_REPORT("musyx: could not allocate the arrangement pattern set.\n");
    return false;
  }
  memset(seen, 0, ARR_SEEN_BYTES);

  w.base = arrfile;
  w.size = size;
  w.ok = true;

  for (i = 0; i < ARR_HEADER_WORDS * sizeof(u32); i += sizeof(u32)) {
    Swap32At(w.base + i);
  }
  arr = arrfile;

  if (arr->mTrack != 0) {
    u32 ofs = arr->mTrack;
    for (;;) {
      /* The list ends with a bare time of -1 -- only that word is ever read to
       * spot the end, so the last entry has no bpm behind it. Check for it
       * before insisting on a whole entry's worth of room. */
      u8* p = ArrAt(&w, ofs, sizeof(u32));
      if (p == NULL) {
        break;
      }
      /* -1 reads the same in either order. */
      if (Read32At(p) == 0xFFFFFFFF) {
        break;
      }
      if (ArrAt(&w, ofs, sizeof(MTRACK_DATA)) == NULL) {
        break;
      }
      Swap32At(p + offsetof(MTRACK_DATA, time));
      Swap32At(p + offsetof(MTRACK_DATA, bpm));
      ofs += sizeof(MTRACK_DATA);
    }
  }

  /* Byte tables the sequencer follows but this walk does not, so their offsets
   * would otherwise go unchecked. tsTab is only read when the top bit of `info`
   * is set (see seqStartPlay). */
  if (arr->tmTab == 0 || arr->tmTab >= size ||
      (ARR_HEADER_WORDS * sizeof(u32) == sizeof(ARR) && (arr->info & 0x80000000) != 0 &&
       (arr->tsTab == 0 || arr->tsTab >= size))) {
    MUSY_REPORT("musyx: arrangement has a track/section table outside the file.\n");
    salFree(seen);
    return false;
  }

  tTab = ArrAt(&w, arr->tTab, 64 * sizeof(u32));
  if (tTab != NULL) {
    for (i = 0; i < 64; ++i) {
      u32 trackOfs = Read32At(tTab + i * sizeof(u32));
      Swap32At(tTab + i * sizeof(u32));
      if (trackOfs != 0) {
        SwapTrack(&w, trackOfs, seen);
      }
    }
  }

  for (i = 0; w.ok && i < ARR_PATTERN_IDS; ++i) {
    u8* entry;
    u32 patternOfs;

    if ((seen[i >> 3] & (1 << (i & 7))) == 0) {
      continue;
    }

    entry = ArrAt(&w, arr->pTab + i * sizeof(u32), sizeof(u32));
    if (entry == NULL) {
      break;
    }
    patternOfs = Read32At(entry);
    Swap32At(entry);
    SwapPattern(&w, patternOfs);
  }

  salFree(seen);
  return w.ok;
}

/* Per-sample ADPCM setup data. It does not live in the directory entry: the
 * entry stores an offset from the start of the file to a block sitting behind
 * the entry table, and hw_dspctrl.c reads the coefficients out of it when a
 * voice starts. The block's shape follows the sample's compression type,
 * which is the top byte of header.length (see dataGetSample).
 *
 *   0, 4, 5  SNDADPCMinfo         a fixed 0x28 byte header
 *   1        DSPADPCMplusInfo     the same header, then one 6 byte record per
 *                                 14 sample block, indexed by play position
 *   3, ...   none                 PCM8/PCM16 carry no extra data
 */
static u32 SdirExtraSize(const SAMPLE_HEADER* header) {
  u32 compType = header->length >> 24;
  u32 length = header->length & 0x00FFFFFF;

  switch (compType) {
  case 0:
  case 4:
  case 5:
    return sizeof(SNDADPCMinfo);

  case 1:
    /* hw_dspctrl.c indexes blk[] with (offset + 0xD) / 14, and an offset may
     * be the sample's full length, so the last block is one past that. */
    return offsetof(DSPADPCMplusInfo, blk) + (((length + 0xD) / 14) + 1) * sizeof(DSPADPCMblock);

  default:
    return 0;
  }
}

/* Both extra-data layouts open with the same header, and every multi-byte
 * field in it is 16 bits: numCoef, the two loop history samples, and the
 * coefficient table. initialPS and loopPS are single bytes. */
static void SwapAdpcmInfo(u8* p, const SAMPLE_HEADER* header) {
  u32 i;
  u32 blocks;

  Swap16At(p + offsetof(SNDADPCMinfo, numCoef));
  Swap16At(p + offsetof(SNDADPCMinfo, loopY0));
  Swap16At(p + offsetof(SNDADPCMinfo, loopY1));
  for (i = 0; i < 8; ++i) {
    Swap16At(p + offsetof(SNDADPCMinfo, coefTab) + (i * 2 + 0) * sizeof(s16));
    Swap16At(p + offsetof(SNDADPCMinfo, coefTab) + (i * 2 + 1) * sizeof(s16));
  }

  if ((header->length >> 24) != 1) {
    return;
  }

  blocks = (((header->length & 0x00FFFFFF) + 0xD) / 14) + 1;
  for (i = 0; i < blocks; ++i) {
    u8* blk = p + offsetof(DSPADPCMplusInfo, blk) + i * sizeof(DSPADPCMblock);
    Swap16At(blk + offsetof(DSPADPCMblock, Y0));
    Swap16At(blk + offsetof(DSPADPCMblock, Y1));
    /* PS and reserved are single bytes. */
  }
}

/* The host entry table is wider than the on-disc one -- 0x28 bytes against
 * 0x20, because `addr` becomes a real pointer -- so an extraData offset taken
 * from the file no longer lands where dataGetSample() looks for it, which
 * resolves it against the table it is handed. Rather than teach that arithmetic
 * about two bases, the extra-data region is copied in behind the host table and
 * the offsets are rewritten to match. That also gives somewhere to convert it:
 * it is authored big-endian like everything else here, and the coefficients
 * reach the decoder unswapped otherwise. */
SDIR_DATA* salSdirToHost(void* sdir) {
  SDIR_DATA_INTER* in = sdir;
  SDIR_DATA* out;
  u8* extraOut;
  u32 tableBytes;
  u32 discTableEnd;
  u32 extraStart;
  u32 extraEnd;
  u32 n;
  u32 i;
  u32 j;

  if (in == NULL) {
    return NULL;
  }

  /* 0xFFFF reads the same either way, so the list can be measured before any
   * swapping happens. */
  for (n = 0; in[n].id != 0xFFFF; ++n) {
    ;
  }

  tableBytes = (n + 1) * sizeof(SDIR_DATA);
  discTableEnd = (n + 1) * sizeof(SDIR_DATA_INTER);

  /* The region runs from the first extra-data block to the end of the last.
   * It normally begins where the entry table ends, but nothing in the format
   * promises that, so the bounds are taken from the entries themselves. */
  extraStart = discTableEnd;
  extraEnd = discTableEnd;
  for (i = 0; i < n; ++i) {
    SAMPLE_HEADER header;
    u32 offset = Swap32(in[i].extraData);
    u32 size;

    if (offset == 0) {
      continue;
    }

    header.info = Swap32(in[i].header.info);
    header.length = Swap32(in[i].header.length);
    header.loopOffset = Swap32(in[i].header.loopOffset);
    header.loopLength = Swap32(in[i].header.loopLength);

    size = SdirExtraSize(&header);
    if (size == 0) {
      continue;
    }

    if (offset < extraStart) {
      extraStart = offset;
    }
    if (offset + size > extraEnd) {
      extraEnd = offset + size;
    }
  }

  out = salMalloc(tableBytes + (extraEnd - extraStart));
  MUSY_ASSERT_MSG(out != NULL, "Could not allocate host-order sample directory");
  if (out == NULL) {
    return NULL;
  }

  extraOut = (u8*)out + tableBytes;
  memcpy(extraOut, (const u8*)sdir + extraStart, extraEnd - extraStart);

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
    if (out[i].extraData == 0 || SdirExtraSize(&out[i].header) == 0) {
      out[i].extraData = 0;
      continue;
    }

    /* Samples cut from the same source share one block, and swapping it once
     * per entry that names it would put it back into big-endian order. */
    for (j = 0; j < i; ++j) {
      if (out[j].extraData != 0 && Swap32(in[j].extraData) == out[i].extraData) {
        break;
      }
    }
    if (j == i) {
      SwapAdpcmInfo(extraOut + (out[i].extraData - extraStart), &out[i].header);
    }

    /* dataGetSample() adds this to the base of the table it is holding, so it
     * has to be an offset into the block allocated here. */
    out[i].extraData = tableBytes + (out[i].extraData - extraStart);
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
