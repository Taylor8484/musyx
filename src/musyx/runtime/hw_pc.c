#include "musyx/platform.h"

// TODO: Finish implementation, rename platform specific functions
// TODO: Use macros to alias platform specific calls, or ifdef for each?

#if MUSY_TARGET == MUSY_TARGET_PC
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif
#include "musyx/assert.h"
#include "musyx/hardware.h"
#include "musyx/sal.h"
#include <string.h>

static volatile u32 oldState = 0;
static volatile u16 hwIrqLevel = 0;
static volatile u32 salDspInitIsDone = 0;
static volatile u64 salLastTick = 0;
static volatile u32 salLogicActive = 0;
static volatile u32 salLogicIsWaiting = 0;
static volatile u32 salDspIsDone = 0;
void* salAIBufferBase = NULL;
static u8 salAIBufferIndex = 0;
static SND_SOME_CALLBACK userCallback = NULL;

#define DMA_BUFFER_LEN 0x280

#ifdef _WIN32
HANDLE globalMutex;
HANDLE globalInterrupt;
#else
pthread_mutex_t globalMutex;
pthread_mutex_t globalInterrupt;
#endif

/* The host supplies the AI (Audio Interface) side, exactly as the Dolphin
 * backend expects it from the SDK -- middleware layered on top of musyx may
 * chain its own handler in front of salCallback via AIRegisterDMACallback, so
 * the registration has to go through AI rather than being driven directly.
 * Declared here rather than via <dolphin/ai.h> to keep musyx free of an SDK
 * include path.
 *
 * AIInitDMA() is deliberately not used on PC: its address argument is a 32-bit
 * physical address and cannot carry a host pointer. The host reads the block to
 * play from salAiGetPlayBuffer() below instead. */
typedef void (*AI_DMA_CALLBACK)(void);
extern AI_DMA_CALLBACK AIRegisterDMACallback(AI_DMA_CALLBACK callback);
extern void AIStartDMA(void);
extern void AIStopDMA(void);

u32 salGetStartDelay();
static void callUserCallback() {
  if (salLogicActive) {
    return;
  }
  salLogicActive = 1;
  // OSEnableInterrupts();
  userCallback();
  // OSDisableInterrupts();
  salLogicActive = 0;
}

void salCallback() {
  salAIBufferIndex = (salAIBufferIndex + 1) % 4;
  salLastTick = 0; // OSGetTick();
  if (salDspIsDone) {
    callUserCallback();
  } else {
    salLogicIsWaiting = 1;
  }
}

void dspInitCallback() {
  salDspIsDone = TRUE;
  salDspInitIsDone = TRUE;
}

void dspResumeCallback() {
  salDspIsDone = TRUE;
  if (salLogicIsWaiting) {
    salLogicIsWaiting = FALSE;
    callUserCallback();
  }
}

bool salInitAi(SND_SOME_CALLBACK callback, u32 unk, u32* outFreq) {
  if ((salAIBufferBase = salMalloc(DMA_BUFFER_LEN * 4)) != NULL) {
    memset(salAIBufferBase, 0, DMA_BUFFER_LEN * 4);
    // DCFlushRange(salAIBufferBase, DMA_BUFFER_LEN * 4);
    salAIBufferIndex = TRUE;
    salLogicIsWaiting = FALSE;
    salDspIsDone = TRUE;
    salLogicActive = FALSE;
    userCallback = callback;
    AIRegisterDMACallback(salCallback);
    synthInfo.numSamples = 0x20;
    *outFreq = 32000;
    MUSY_DEBUG("MusyX AI interface initialized.\n");
    return TRUE;
  }

  return FALSE;
}

bool salStartAi() {
  AIStartDMA();
  return TRUE;
}

bool salExitAi() {
  AIRegisterDMACallback(NULL);
  AIStopDMA();
  salFree(salAIBufferBase);
  return TRUE;
}

/* The block the "DMA" is currently playing, for the host to hand to its audio
 * device. salAiGetDest() below is its counterpart: where the next block is
 * mixed. Both are slots in the same four-deep ring. */
void* salAiGetPlayBuffer(u32* length) {
  if (length != NULL) {
    *length = DMA_BUFFER_LEN;
  }
  if (salAIBufferBase == NULL) {
    return NULL;
  }
  return (void*)((u8*)salAIBufferBase + salAIBufferIndex * DMA_BUFFER_LEN);
}

void* salAiGetDest() {
  u8 index; // r31
  index = (salAIBufferIndex + 2) % 4;
  return (void*)((u8*)salAIBufferBase + index * DMA_BUFFER_LEN);
}

bool salInitDsp(u32 arg0) { return TRUE; }

bool salExitDsp() { return false; }

void salStartDsp(u16* cmdList) {}

void salCtrlDsp(s16* dest) {
  salBuildCommandList(dest, salGetStartDelay());
  salStartDsp(dspCmdList);
}

u32 salGetStartDelay() { return 0; }

void hwInitIrq() {
  // oldState = OSDisableInterrupts();
  hwIrqLevel = 1;
#ifdef _WIN32
  globalMutex = CreateMutex(NULL, FALSE, NULL);
#elif defined(__linux__) && !defined(__ANDROID__)
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ROBUST);
  pthread_mutex_init(&globalMutex, &attr);
#else
  // TODO
#endif
}

void hwExitIrq() {}

void hwEnableIrq() {
  if (--hwIrqLevel == 0) {
    // OSRestoreInterrupts(oldState);
  }
}

void hwDisableIrq() {
  if ((hwIrqLevel++) == 0) {
    // oldState = OSDisableInterrupts();
  }
}

void hwIRQEnterCritical() {
#ifdef _WIN32
  DWORD waitResult = WaitForSingleObject(globalMutex, INFINITE);
#else
  pthread_mutex_lock(&globalMutex);
#endif
}

void hwIRQLeaveCritical() {
#ifdef _WIN32
  ReleaseMutex(globalMutex);
#else
  pthread_mutex_unlock(&globalMutex);
#endif
}
#endif
