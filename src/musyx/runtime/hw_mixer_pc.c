#include "musyx/platform.h"

#if MUSY_TARGET == MUSY_TARGET_PC

#include "musyx/assert.h"
#include "musyx/dspvoice.h"
#include "musyx/hardware.h"
#include "musyx/sal.h"
#include "musyx/synth.h"
#include "musyx/synthdata.h"

#include <stdint.h>
#include <string.h>

/* Software stand-in for the GameCube DSP's mixing pass.
 *
 * salBuildCommandList() leaves a parameter block per voice describing what the
 * DSP would have been told to do for this 5 ms frame; this walks those blocks
 * instead, decodes each voice out of emulated ARAM, resamples it to the mix
 * rate, applies the ramped volumes and accumulates into the studio buses.
 *
 * The DSP also hands state back through the same block -- play position, ADPCM
 * predictor, resampler phase, and the volumes as left by their ramps -- so
 * everything advanced here is written back for the next frame to pick up.
 */

extern u8 salFrame;
extern u8 salAuxFrame;
extern void* ARGetStorageAddress(void);
extern u32 ARGetSize(void);

#define SAMPLES_PER_FRAME 160

/* pb->addr.format, as chosen in salBuildCommandList(). The address units
 * differ per format: ADPCM counts nibbles, PCM8 bytes, PCM16 16-bit words. */
#define FMT_ADPCM 0x00
#define FMT_PCM8 0x19
#define FMT_PCM16 0x0A

/* Bus offsets within a studio buffer: 160 samples of left, then right, then
 * surround (see salHandleAuxProcessing). */
#define BUS_L 0
#define BUS_R SAMPLES_PER_FRAME
#define BUS_S (SAMPLES_PER_FRAME * 2)
#define BUS_TOTAL (SAMPLES_PER_FRAME * 3)

static s32 ClampS16(s32 v) { return v > 32767 ? 32767 : (v < -32768 ? -32768 : v); }

static u32 PBAddr(u16 hi, u16 lo) { return ((u32)hi << 16) | lo; }

/* Decodes one input sample and steps the play position, looping as the DSP
 * would. Non-looping voices have loopAddress pointed at the ARAM zero buffer,
 * so they drain into silence rather than stopping here. */
static s16 FetchSample(_PB* pb, const u8* aram, u32 aramSize, u32* pAddr, u32 endAddr,
                       u32 loopAddr) {
  u32 a = *pAddr;
  s32 out = 0;

  switch (pb->addr.format) {
  case FMT_ADPCM: {
    s32 nibble;
    s32 scale;
    s32 idx;
    s32 val;
    u8 byte;

    /* Each 8-byte block opens with a predictor/scale byte occupying the first
     * two nibbles; the remaining 14 nibbles are samples. */
    if ((a & 0x0F) == 0) {
      if ((a >> 1) < aramSize) {
        pb->adpcm.pred_scale = aram[a >> 1];
      }
      a += 2;
    }

    byte = ((a >> 1) < aramSize) ? aram[a >> 1] : 0;
    nibble = (a & 1) ? (byte & 0x0F) : (byte >> 4);
    if (nibble >= 8) {
      nibble -= 16;
    }

    idx = (pb->adpcm.pred_scale >> 4) & 0x07;
    scale = 1 << (pb->adpcm.pred_scale & 0x0F);

    val = ((nibble * scale) << 11) + 1024 + (s32)(s16)pb->adpcm.a[idx][0] * (s32)(s16)pb->adpcm.yn1 +
          (s32)(s16)pb->adpcm.a[idx][1] * (s32)(s16)pb->adpcm.yn2;
    val >>= 11;
    val = ClampS16(val);

    pb->adpcm.yn2 = pb->adpcm.yn1;
    pb->adpcm.yn1 = (u16)(s16)val;
    out = val;
    a += 1;
  } break;

  case FMT_PCM8:
    out = (a < aramSize) ? ((s32)(s8)aram[a] << 8) : 0;
    a += 1;
    break;

  case FMT_PCM16: {
    /* Sample data is copied into ARAM straight off the disc, so it is still
     * big-endian here. */
    u32 off = a * 2;
    out = (off + 1 < aramSize) ? (s32)(s16)(((u16)aram[off] << 8) | aram[off + 1]) : 0;
    a += 1;
  } break;

  default:
    a += 1;
    break;
  }

  if (a > endAddr) {
    a = loopAddr;
    /* loopType 1 is a streamed voice, whose loop context is maintained by the
     * streaming code rather than carried in the block. */
    if (pb->addr.format == FMT_ADPCM && pb->loopType == 0) {
      pb->adpcm.pred_scale = pb->adpcmLoop.loop_pred_scale;
      pb->adpcm.yn1 = pb->adpcmLoop.loop_yn1;
      pb->adpcm.yn2 = pb->adpcmLoop.loop_yn2;
    }
  }

  *pAddr = a;
  return (s16)out;
}

static void MixVoice(_PB* pb, s32* mainBuf, s32* auxABuf, s32* auxBBuf) {
  const u8* aram = ARGetStorageAddress();
  u32 aramSize = ARGetSize();

  u32 addr = PBAddr(pb->addr.currentAddressHi, pb->addr.currentAddressLo);
  u32 endAddr = PBAddr(pb->addr.endAddressHi, pb->addr.endAddressLo);
  u32 loopAddr = PBAddr(pb->addr.loopAddressHi, pb->addr.loopAddressLo);

  /* 16.16 step through the source. srcSelect 2 means the voice is already at
   * the mix rate and no resampling was requested. */
  u32 ratio = (pb->srcSelect == 2) ? 0x00010000u : PBAddr(pb->src.ratioHi, pb->src.ratioLo);
  u32 acc = pb->src.currentAddressFrac;

  s32 prev = (s16)pb->src.last_samples[2];
  s32 cur = (s16)pb->src.last_samples[3];

  s32 ve = (s16)pb->ve.currentVolume;
  s32 veDelta = (s16)pb->ve.currentDelta;

  s32 vL = (s16)pb->mix.vL, dL = (s16)pb->mix.vDeltaL;
  s32 vR = (s16)pb->mix.vR, dR = (s16)pb->mix.vDeltaR;
  s32 vS = (s16)pb->mix.vS, dS = (s16)pb->mix.vDeltaS;
  s32 vAL = (s16)pb->mix.vAuxAL, dAL = (s16)pb->mix.vDeltaAuxAL;
  s32 vAR = (s16)pb->mix.vAuxAR, dAR = (s16)pb->mix.vDeltaAuxAR;
  s32 vBL = (s16)pb->mix.vAuxBL, dBL = (s16)pb->mix.vDeltaAuxBL;
  s32 vBR = (s16)pb->mix.vAuxBR, dBR = (s16)pb->mix.vDeltaAuxBR;

  u32 wantSurround = (pb->mixerCtrl & 0x04) != 0;
  u32 wantAuxA = (pb->mixerCtrl & 0x01) != 0;
  u32 wantAuxB = (pb->mixerCtrl & 0x02) != 0;

  s32 i;

  if (aram == NULL) {
    return;
  }

  for (i = 0; i < SAMPLES_PER_FRAME; ++i) {
    s32 frac = (s32)(acc & 0xFFFF);
    s32 s = prev + (((cur - prev) * frac) >> 16);

    /* Volume envelope first, then the per-bus sends, matching the DSP's order. */
    s = (s * ve) >> 15;

    mainBuf[BUS_L + i] += (s * vL) >> 15;
    mainBuf[BUS_R + i] += (s * vR) >> 15;
    if (wantSurround) {
      mainBuf[BUS_S + i] += (s * vS) >> 15;
    }
    if (wantAuxA) {
      auxABuf[BUS_L + i] += (s * vAL) >> 15;
      auxABuf[BUS_R + i] += (s * vAR) >> 15;
    }
    if (wantAuxB) {
      auxBBuf[BUS_L + i] += (s * vBL) >> 15;
      auxBBuf[BUS_R + i] += (s * vBR) >> 15;
    }

    /* Every ramp advances once per sample -- that is what makes 160 steps of
     * vDelta cover exactly one frame's worth of change. */
    vL += dL;
    vR += dR;
    vS += dS;
    vAL += dAL;
    vAR += dAR;
    vBL += dBL;
    vBR += dBR;

    ve += veDelta;
    if (ve < 0) {
      ve = 0;
    } else if (ve > 32767) {
      ve = 32767;
    }

    acc += ratio;
    while (acc >= 0x10000u) {
      prev = cur;
      cur = FetchSample(pb, aram, aramSize, &addr, endAddr, loopAddr);
      acc -= 0x10000u;
    }
  }

  /* Hand the advanced state back the way the DSP would have. */
  pb->addr.currentAddressHi = (u16)(addr >> 16);
  pb->addr.currentAddressLo = (u16)addr;
  pb->src.currentAddressFrac = (u16)acc;
  pb->src.last_samples[2] = (u16)(s16)prev;
  pb->src.last_samples[3] = (u16)(s16)cur;

  pb->ve.currentVolume = (u16)ve;
  pb->mix.vL = (u16)(s16)ClampS16(vL);
  pb->mix.vR = (u16)(s16)ClampS16(vR);
  pb->mix.vS = (u16)(s16)ClampS16(vS);
  pb->mix.vAuxAL = (u16)(s16)ClampS16(vAL);
  pb->mix.vAuxAR = (u16)(s16)ClampS16(vAR);
  pb->mix.vAuxBL = (u16)(s16)ClampS16(vBL);
  pb->mix.vAuxBR = (u16)(s16)ClampS16(vBR);
}

void salMixFrame(s16* dest) {
  DSPstudioinfo* master = NULL;
  u8 st;
  s32 i;

  for (st = 0; st < salMaxStudioNum; ++st) {
    DSPstudioinfo* stp = &dspStudio[st];
    s32* mainBuf;
    s32* auxABuf;
    s32* auxBBuf;
    DSPvoice* dv;

    if (stp->state != 1) {
      continue;
    }
    if (stp->isMaster) {
      master = stp;
    }

    mainBuf = stp->main[salFrame];
    auxABuf = stp->auxA[salAuxFrame];
    auxBBuf = stp->auxB[salAuxFrame];

    memset(mainBuf, 0, BUS_TOTAL * sizeof(s32));
    memset(auxABuf, 0, BUS_TOTAL * sizeof(s32));
    memset(auxBBuf, 0, BUS_TOTAL * sizeof(s32));

    for (dv = stp->voiceRoot; dv != NULL; dv = dv->next) {
      if (dv->pb != NULL && dv->pb->state != 0) {
        MixVoice(dv->pb, mainBuf, auxABuf, auxBBuf);
      }
    }
  }

  if (dest == NULL) {
    return;
  }

  if (master == NULL) {
    memset(dest, 0, SAMPLES_PER_FRAME * 2 * sizeof(s16));
    return;
  }

  /* The aux buses are processed after this by salHandleAuxProcessing() but are
   * not yet folded back into the master mix, so effect returns are silent for
   * now. */
  {
    const s32* mainBuf = master->main[salFrame];
    for (i = 0; i < SAMPLES_PER_FRAME; ++i) {
      dest[i * 2 + 0] = (s16)ClampS16(mainBuf[BUS_L + i]);
      dest[i * 2 + 1] = (s16)ClampS16(mainBuf[BUS_R + i]);
    }
  }
}

#endif
