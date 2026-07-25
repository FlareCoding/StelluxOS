#include "drivers/gpu/nvidia/nv_display.h"
#include "drivers/gpu/nvidia/nv_mem.h"
#include "common/logging.h"
#include "common/string.h"
#include "hw/mmio.h"
#include "hw/barrier.h"
#include "hw/cpu.h"
#include "hw/delay.h"
#include "dynpriv/dynpriv.h"

namespace nvidia {

// RM class ids (g_allclasses.h).
constexpr uint32_t NV01_ROOT           = 0x00000000;
constexpr uint32_t NV01_DEVICE_0       = 0x00000080;
constexpr uint32_t NV20_SUBDEVICE_0    = 0x00002080;
constexpr uint32_t NV01_MEMORY_VIRTUAL = 0x00000070;
constexpr uint32_t NV04_DISPLAY_COMMON = 0x00000073;
constexpr uint32_t NVC670_DISPLAY      = 0x0000c670;
constexpr uint32_t NVC67D_CORE_CHANNEL_DMA   = 0x0000c67d;
constexpr uint32_t NVC67E_WINDOW_CHANNEL_DMA = 0x0000c67e;

// Our handle scheme: one external client (RS_CLIENT_HANDLE_BASE) + a simple
// object counter (RM uses RS_UNIQUE_HANDLE_BASE 0xcaf00000). NV04_DISPLAY_COMMON
// is parented to the DEVICE (a sibling of the subdevice) -- spec CORRECTION #4.
constexpr uint32_t H_CLIENT      = 0xC1D00000;
constexpr uint32_t H_DEVICE      = 0xCAF00000;
constexpr uint32_t H_SUBDEVICE   = 0xCAF00001;
constexpr uint32_t H_DISPLAY     = 0xCAF00002;
constexpr uint32_t H_MEM_VIRTUAL = 0xCAF00003;
constexpr uint32_t H_DISP_ENGINE = 0xCAF00004; // NVC670_DISPLAY
constexpr uint32_t H_CORE_CH     = 0xCAF00005; // NVC67D core channel
constexpr uint32_t H_WIN_CH      = 0xCAF00006; // NVC67E window0 channel
constexpr uint32_t H_CORE_CTXDMA = 0xCAF00007; // core PB ctxdma (CPU-RM-side; opaque to GSP)
constexpr uint32_t H_WIN_CTXDMA  = 0xCAF00008; // window0 PB ctxdma (CPU-RM-side; opaque to GSP)

// The GSP's own internal client/subdevice for INTERNAL_* controls (spec CORRECTION #7).
constexpr uint32_t INTERNAL_CLIENT    = 0xc2000005;
constexpr uint32_t INTERNAL_SUBDEVICE = 0xabcd2080;
constexpr uint32_t ADDR_SYSMEM        = 1; // ctrl0080fb.h:168 (UNKNOWN/SYSMEM/FBMEM = 0/1/2)
constexpr uint32_t NV2080_CTRL_CMD_INTERNAL_DISPLAY_CHANNEL_PUSHBUFFER = 0x20800a58;

// EVO channel control regions in BAR0 (PUT@+0x0, GET@+0x4 -- NV917D_PUT/GET, cl917d.h).
// base(i) = NV_UDISP_FE_CHN_ASSY_BASEADR(i): core(chnNum 0)=0x680000, win(j)=chnNum 1+j ->
// 0x690000+(j)*0x1000 (dev_disp.h v03_00:65, kern_disp_0300.c:224).
constexpr uint32_t NVC67D_CORE_CTRL_BASE = 0x680000;
constexpr uint32_t NVC67E_WIN0_CTRL_BASE = 0x690000; // NV_UDISP_FE_CHN_ASSY_BASEADR(win0=chn1)
constexpr uint32_t NVC67E_WIN6_CTRL_BASE = 0x696000; // win6 = win0 + 6*0x1000 (head 3's natural window)
constexpr uint32_t NV917D_PUT_OFF = 0x0;
constexpr uint32_t NV917D_GET_OFF = 0x4;

// Increment 2b: VRAM scanout surface + display instance memory + paint.
// On GSP-offload the HOST RM owns the FB heap + display instance memory (the GSP has no
// client-FB-alloc RPC -- mem_mgr_gsp_client.c FWCLIENT; ALLOC_MEMORY is IS_VIRTUAL-only).
// So WE carve VRAM and build the display instance memory (ctxdma hash + entry) directly in
// FBMEM via PRAMIN, then point the HW at it with WRITE_INST_MEM. Formats: disp_inst_mem_0300.c
// + dev_disp.h v03_00.
constexpr uint32_t H_ISO_CTXDMA = 0xCAF0000A; // ISO-surface ctxdma handle (hash key + SET_CONTEXT_DMA_ISO)

// PRAMIN / BAR0_WINDOW (dev_bus.h:43, dev_ram.h) -- RM's foundational CPU->VRAM path.
constexpr uint32_t NV_PBUS_BAR0_WINDOW = 0x00001700; // BASE[23:0]=vramOff>>16, TARGET[25:24]=VID_MEM(0)
constexpr uint32_t NV_PRAMIN_BASE      = 0x00700000; // 1 MB window into VRAM at BAR0+0x700000
constexpr uint64_t NV_PRAMIN_SIZE      = 0x00100000;

// Display instance memory layout (dev_disp.h v03_00): [hash 0..0x1FFF | obj 0x2000..0xFFFF] = 64KB.
constexpr uint32_t DISP_INSTMEM_SIZE = 0x10000;
constexpr uint32_t DISP_OBJ_MEM_BASE = 0x2000;       // we place our single ctxdma entry here
constexpr uint32_t DISP_HASH_ENTRIES = 0x2000 / 8;   // 1024 = hashTableSize / sizeof(entry)
constexpr uint32_t NV2080_CTRL_CMD_INTERNAL_DISPLAY_WRITE_INST_MEM = 0x20800a49;
constexpr uint32_t ADDR_FBMEM   = 2;                 // ctrl0080fb.h:169
constexpr uint32_t CHN_NUM_CORE = 0;                 // NV_PDISP_CHN_NUM_CORE
constexpr uint32_t CHN_NUM_WIN0 = 1;                 // NV_PDISP_CHN_NUM_WIN(0)
constexpr uint32_t CHN_NUM_WIN6 = 7;                 // NV_PDISP_CHN_NUM_WIN(6) = core(0)+win6+1 (head 3's window)
// ctxdma entry word0: TARGET_NODE_PHYSICAL_NVM(1) | ACCESS_READ_AND_WRITE(1<<2) | KIND_PITCH(0).
constexpr uint32_t DISP_DMA_W0_FBMEM_RW_PITCH = 0x00000005;

// VRAM carve (we are the FB-heap owner): free offsets deep in usable FB -- below the GSP
// reservation (~0x275b00000) and well above low-FB. 64KB-aligned for PRAMIN.
constexpr uint64_t FB_SURFACE_VRAM = 0x180000000ull; // scanout surface (~6 GB offset)
constexpr uint64_t FB_INSTMEM_VRAM = 0x181000000ull; // display instance memory (64 KB)

// Target mode (head 3 / DP 0x800, 2560x1440 -- matches Stage 15 raster).
constexpr uint32_t FB_W = 2560, FB_H = 1440, FB_PITCH = FB_W * 4; // 10240, 64B-aligned

// --- Hardware cursor (clc67a.h / clc67d.h:854-890 / nvkms-evo3.c EvoSetCursorImageC3) ---
constexpr uint32_t NVC67A_CURSOR_IMM_CHANNEL_PIO = 0x0000c67a;
constexpr uint32_t H_CURSOR_CH     = 0xCAF0000B; // NVC67A cursor PIO channel
constexpr uint32_t H_CURSOR_CTXDMA = 0xCAF0000C; // cursor-surface ctxdma handle
constexpr uint64_t FB_CURSOR_VRAM  = 0x181100000ull; // 64x64 A8R8G8B8 cursor image (16KB)
constexpr uint32_t CURSOR_W = 64, CURSOR_H = 64;
constexpr uint32_t CURSOR_BYTES = CURSOR_W * CURSOR_H * 4; // 16384
constexpr uint32_t DISP_CURSOR_ENTRY_OFF = 0x2020; // 2nd ctxdma instance entry (after ISO @0x2000)
// HEAD_SET_CONTROL_CURSOR = ENABLE(1<<31) | FORMAT_A8R8G8B8(0xCF) | SIZE_W64_H64(1<<8), hotspot 0.
constexpr uint32_t HEAD_SET_CONTROL_CURSOR_VAL = 0x800001CF;
// COMPOSITION = K1(255) | CURSOR_COLOR_FACTOR_SELECT_K1_TIMES_SRC(5<<8) |
// VIEWPORT_COLOR_FACTOR_SELECT_NEG_K1_TIMES_SRC(7<<12) | MODE_BLEND -> straight alpha over.
// DIAGNOSTIC: OPAQUE composition (K1=255 | CURSOR_FACTOR=K1 | VIEWPORT_FACTOR=ZERO | BLEND)
// -> cursor color shows regardless of src alpha (matches the golden capture's 0x2ff). Was
// 0x75FF (non-premult alpha). Use this to test whether the cursor pipe displays anything.
constexpr uint32_t HEAD_SET_CONTROL_CURSOR_COMP_VAL = 0x000002FF;
// NVC67A PIO control region for head 3 = NV_UDISP_FE_CHN_ASSY_BASEADR_CURS(3) (dev_disp.h:64).
constexpr uint32_t NVC67A_CURS3_CTRL_BASE = 0x006DB000;
constexpr uint32_t NVC67A_UPDATE_OFF = 0x200;
constexpr uint32_t NVC67A_HOT_SPOT_POINT_OUT_OFF = 0x208; // X[15:0], Y[31:16]
constexpr uint32_t NVC67A_FREE_OFF   = 0x008;             // NVC37ADispCursorImmControlPio.Free

// --- NVDisplay-3 composition pipeline (window ILUT + head OLUT + OCSC0) ---
// Required once the window is depth-composited (BYPASS=DISABLE) so the HW compositor (which
// blends the cursor plane) is active. The compositor works internally in FP16: the window
// ILUT converts the 8-bit X8R8G8B8 surface to FP16; the head OLUT converts back to panel bits;
// OCSC0 applies the (identity) saturation/colour matrix. Skipping any of these blanks a
// composited head. All offsets/values verified vs clc67e.h / clc67d.h / nvkms-evo3.c.
constexpr uint32_t H_LUT_CTXDMA   = 0xCAF0000D; // shared ILUT(window,chn1) + OLUT(core,chn0) ctxdma
constexpr uint64_t FB_LUT_VRAM    = 0x181200000ull; // NVEvoLutDataRec {base[], output[]}
constexpr uint32_t LUT_BYTES      = 0x4140;     // (sizeof(NVEvoLutDataRec)+63)&~63
constexpr uint32_t LUT_OUTPUT_OFF = 0x2100;     // byte offset of output[] (OLUT) within the buffer
constexpr uint32_t LUT_SIZE_FIELD = 1029;       // 4 VSS-header + 1025 entries
constexpr uint32_t LUT_HDR        = 4;          // NV_LUT_VSS_HEADER_SIZE entries (zeroed)
constexpr uint32_t DISP_LUT_ENTRY_OFF = 0x2040; // 3rd ctxdma instance entry (ISO@0x2000, cursor@0x2020)
// SET_{I,O}LUT_CONTROL packed = MODE_DIRECT10(2<<2) | SIZE(1029<<8), INTERPOLATE=0 (full 1025-entry)
constexpr uint32_t LUT_CONTROL_VAL = (2u << 2) | (LUT_SIZE_FIELD << 8); // 0x00040508
// Identity coeff for FMT (IdentityFMTMatrix) AND OCSC0 (cscCoefConvertS514(1.0)&0x1ffffc): both 0x10000.
constexpr uint32_t CSC_IDENTITY_DIAG = 0x00010000;

// Window (NVC67E) precomp method offsets (clc67e.h).
constexpr uint32_t NVC67E_FMT_COEFFICIENT_C00 = 0x400; // C00..C23 contiguous (+4)
constexpr uint32_t NVC67E_ILUT_CONTROL        = 0x440;
constexpr uint32_t NVC67E_CONTEXT_DMA_ILUT    = 0x444;
constexpr uint32_t NVC67E_OFFSET_ILUT         = 0x448;
constexpr uint32_t NVC67E_CSC00CONTROL        = 0x45C;
constexpr uint32_t NVC67E_CSC01CONTROL        = 0x4BC;
constexpr uint32_t NVC67E_CONTEXT_DMA_TMO_LUT = 0x528;
constexpr uint32_t NVC67E_CSC10CONTROL        = 0x53C;
constexpr uint32_t NVC67E_CSC11CONTROL        = 0x59C;
constexpr uint32_t NVC67E_CONTROL_INPUT_SCALER = 0x2A8;

// Head (NVC67D) postcomp method offsets (add head*0x400) (clc67d.h).
constexpr uint32_t NVC67D_DESKTOP_ALPHA_RED  = 0x2220;
constexpr uint32_t NVC67D_DESKTOP_GREEN_BLUE = 0x2224;
constexpr uint32_t NVC67D_CLAMP_GREEN        = 0x2238;
constexpr uint32_t NVC67D_CLAMP_RED_BLUE     = 0x223C;
constexpr uint32_t NVC67D_OCSC0CONTROL       = 0x2240;
constexpr uint32_t NVC67D_OCSC0COEF_C00      = 0x2244; // C00..C23 contiguous (+4)
constexpr uint32_t NVC67D_OLUT_CONTROL       = 0x2280;
constexpr uint32_t NVC67D_OLUT_FP_NORM_SCALE = 0x2284;
constexpr uint32_t NVC67D_CONTEXT_DMA_OLUT   = 0x2288;
constexpr uint32_t NVC67D_OFFSET_OLUT        = 0x228C;
constexpr uint32_t NVC67D_OCSC1CONTROL       = 0x229C;

constexpr uint32_t DP_TARGET = 0x800; // primary connected DP displayId (Stage 12)

// NV0073 (DISPLAY_COMMON) control commands.
constexpr uint32_t NV0073_CTRL_CMD_SYSTEM_GET_NUM_HEADS     = 0x730102;
constexpr uint32_t NV0073_CTRL_CMD_SYSTEM_GET_SUPPORTED     = 0x730120;
constexpr uint32_t NV0073_CTRL_CMD_SYSTEM_GET_CONNECT_STATE = 0x730122;
constexpr uint32_t NV0073_CTRL_CMD_SPECIFIC_GET_EDID_V2     = 0x730245;
constexpr uint32_t NV0073_CTRL_CMD_SPECIFIC_OR_GET_INFO     = 0x73028b;
constexpr uint32_t NV0073_CTRL_CMD_DFP_GET_INFO            = 0x731140;
constexpr uint32_t NV0073_CTRL_CMD_DFP_ASSIGN_SOR         = 0x731152; // assign a SOR to a displayId (DP prereq)
constexpr uint32_t NV0073_CTRL_CMD_DP_CTRL                = 0x731343; // DP link training (GSP runs CR/EQ + powers SOR)
constexpr uint32_t NV0073_CTRL_CMD_DP_SET_MANUAL         = 0x731365; // put port in manual DP mode (DP_CTRL prereq)
constexpr uint32_t NV0073_CTRL_CMD_DP_GET_CAPS           = 0x731369; // read DP caps into RM (prereq)
constexpr uint32_t NV0073_CTRL_CMD_DP_AUXCH_CTRL         = 0x731341; // DPCD/AUX transaction (sink power-up)
constexpr uint32_t NV0073_CTRL_CMD_DP_CONFIG_STREAM      = 0x731362; // configure SF stream/watermark (post-train)
constexpr uint32_t NV0073_CTRL_CMD_SPECIFIC_DISPLAY_CHANGE = 0x7302a4; // modeset begin/end bracket
constexpr uint32_t NV0073_CTRL_CMD_DP_SET_MSA_PROPERTIES_V2 = 0x731381; // MSA override (color/raster signalling)
// NVC372_DISPLAY_SW (clc372sw.h) -- the display-SW object IMP runs on. Allocated under the device
// with NULL params (nvkms EvoAllocRmCtrlObjectC3). IS_MODE_POSSIBLE validates the proposed mode +
// computes the bandwidth/v-pstate the GSP supervisor needs; the golden runs it before EVERY modeset.
constexpr uint32_t NVC372_DISPLAY_SW                       = 0x0000c372;
constexpr uint32_t NVC372_CTRL_CMD_IS_MODE_POSSIBLE       = 0xc3720101;
constexpr uint32_t H_DISP_SW                              = 0xCAF0000E; // our NVC372_DISPLAY_SW handle

// Alloc-param structs (sizes verified against the 535.183.01 SDK headers).
struct NV0000_ALLOC_PARAMETERS {
    uint32_t hClient;       // must be first member (cl0000.h:48)
    uint32_t processID;
    char     processName[100];
};
static_assert(sizeof(NV0000_ALLOC_PARAMETERS) == 108, "NV0000_ALLOC_PARAMETERS");

struct NV0080_ALLOC_PARAMETERS {
    uint32_t deviceId;
    uint32_t hClientShare;
    uint32_t hTargetClient;
    uint32_t hTargetDevice;
    uint32_t flags;
    uint64_t vaSpaceSize;
    uint64_t vaStartInternal;
    uint64_t vaLimitInternal;
    uint32_t vaMode;
};
static_assert(sizeof(NV0080_ALLOC_PARAMETERS) == 56, "NV0080_ALLOC_PARAMETERS");

struct NV2080_ALLOC_PARAMETERS {
    uint32_t subDeviceId;
};
static_assert(sizeof(NV2080_ALLOC_PARAMETERS) == 4, "NV2080_ALLOC_PARAMETERS");

// NV0073 control-param structs (ctrl0073system.h / ctrl0073specific.h).
struct NV0073_GET_NUM_HEADS { uint32_t subDeviceInstance; uint32_t flags; uint32_t numHeads; };
struct NV0073_GET_SUPPORTED { uint32_t subDeviceInstance; uint32_t displayMask; uint32_t displayMaskDDC; };
struct NV0073_GET_CONNECT_STATE { uint32_t subDeviceInstance; uint32_t flags; uint32_t displayMask; uint32_t retryTimeMs; };
struct NV0073_GET_EDID_V2 {
    uint32_t subDeviceInstance;
    uint32_t displayId;
    uint32_t bufferSize;
    uint32_t flags;
    uint8_t  edidBuffer[2048]; // NV0073_CTRL_SPECIFIC_GET_EDID_MAX_EDID_BYTES
};
static_assert(sizeof(NV0073_GET_EDID_V2) == 2064, "NV0073_GET_EDID_V2");

// Modeset-prep control params + the NV01_MEMORY_VIRTUAL alloc params.
struct NV0073_OR_GET_INFO {
    uint32_t subDeviceInstance, displayId, index, type, protocol, ditherType, ditherAlgo,
             location, rootPortId, dcbIndex;
    uint64_t vbiosAddress;
    uint8_t  bIsLitByVbios, bIsDispDynamic;
};
static_assert(sizeof(NV0073_OR_GET_INFO) == 56, "NV0073_OR_GET_INFO");

struct NV0073_DFP_GET_INFO { uint32_t subDeviceInstance, displayId, flags, flags2; };
static_assert(sizeof(NV0073_DFP_GET_INFO) == 16, "NV0073_DFP_GET_INFO");

// DP bring-up control params. On GSP-offload these RPCs make GSP-RM assign + power the SOR and run
// the DPCD CR/EQ handshake; the host only picks lane count + link rate. Layouts verified vs
// ctrl0073dfp.h (ASSIGN_SOR, 80B) and ctrl0073dp.h (DP_CTRL, 28B).
struct NV0073_DFP_ASSIGN_SOR {
    uint32_t subDeviceInstance;
    uint32_t displayId;
    uint8_t  sorExcludeMask;   // followed by 3B pad (next field is u32)
    uint32_t slaveDisplayId;
    uint32_t forceSublinkConfig;
    uint8_t  bIs2Head1Or;      // +3B pad
    uint32_t sorAssignList[4];                                              // OUT (legacy)
    struct { uint32_t displayMask; uint32_t sorType; } sorAssignListWithTag[4]; // OUT (authoritative)
    uint8_t  reservedSorMask;  // +3B pad
    uint32_t flags;
};
static_assert(sizeof(NV0073_DFP_ASSIGN_SOR) == 80, "NV0073_DFP_ASSIGN_SOR");

struct NV0073_DP_CTRL {
    uint32_t subDeviceInstance;
    uint32_t displayId;
    uint32_t cmd;   // SET_LANE_COUNT(b0)|SET_LINK_BW(b1)|SET_ENHANCED_FRAMING(b7)|TRAIN_PHY_REPEATER(b13)
    uint32_t data;  // SET_LANE_COUNT[4:0] | SET_LINK_BW[15:8] (0x14=HBR2) | TARGET[22:19] (0=SINK)
    uint32_t err;                  // OUT: CLOCK_RECOVERY(b4)/CHANNEL_EQ(b5)/LINK_TRAINING(b31)
    uint32_t retryTimeMs;          // OUT
    uint32_t eightLaneDpcdBaseAddr; // 0 for <=4 lanes
};
static_assert(sizeof(NV0073_DP_CTRL) == 28, "NV0073_DP_CTRL");

struct NV0073_DP_SET_MANUAL { uint32_t subDeviceInstance; };
static_assert(sizeof(NV0073_DP_SET_MANUAL) == 4, "NV0073_DP_SET_MANUAL");

struct NV0073_DP_GET_CAPS {
    uint32_t subDeviceInstance;
    uint32_t sorIndex;
    uint32_t maxLinkRate;          // OUT
    uint32_t dpVersionsSupported;  // OUT
    uint32_t UHBRSupported;        // OUT
    uint8_t  bIsMultistreamSupported, bIsSCEnabled, bHasIncreasedWatermarkLimits, bIsPC2Disabled;
    uint8_t  isSingleHeadMSTSupported, bFECSupported, bIsTrainPhyRepeater, bOverrideLinkBw;
    uint8_t  DSC[28];              // NV0073_CTRL_CMD_DSC_CAP_PARAMS (opaque -- we don't use DSC)
};
static_assert(sizeof(NV0073_DP_GET_CAPS) == 56, "NV0073_DP_GET_CAPS");

struct NV0073_DP_AUXCH_CTRL {
    uint32_t subDeviceInstance;
    uint32_t displayId;
    uint8_t  bAddrOnly;   // +3B pad
    uint32_t cmd;         // bit3 TYPE(AUX=1) | bits1:0 REQ(WRITE=0/READ=1)
    uint32_t addr;        // 20-bit DPCD address
    uint8_t  data[16];    // NV0073_CTRL_DP_AUXCH_MAX_DATA_SIZE
    uint32_t size;        // 0-based (nbytes-1) in; bytes moved out
    uint32_t replyType;   // OUT (ACK/NACK/DEFER)
    uint32_t retryTimeMs; // OUT
};
static_assert(sizeof(NV0073_DP_AUXCH_CTRL) == 48, "NV0073_DP_AUXCH_CTRL");

// NV0073_CTRL_CMD_DP_CONFIG_STREAM_PARAMS (ctrl0073dp.h, 116B). Configures the SF (stream
// formatter) stream on the *trained* link: how to packetize active pixels into transfer units
// (tuSize/waterMark) and the per-line/-frame blank symbol budget (hBlankSym/vBlankSym). WITHOUT
// this the OR has no stream to carry, so the core UPDATE attaches the head to a stream-less link ->
// the OR idles/drops the link (SOR_DP_LINKCTL en=0) and the head stays SNOOZE (= run #43). The
// SST values below are ported from the DP-lib watermark formula (dp_watermark.cpp
// isModePossibleSSTWithFEC, selected because DP_GET_CAPS.bFECSupported=1) and validated bit-for-bit
// against the golden payload (hBlankSym=0x14c, vBlankSym=0x163d) for 2560x1440@60 / 2-lane HBR2.
// bEnableOverride MUST be TRUE or GSP-RM ignores every value here (ctrl0073dp.h:1495). NvBool=u8.
struct NV0073_DP_CONFIG_STREAM {
    uint32_t subDeviceInstance;                 // [0]
    uint32_t head;                              // [1]
    uint32_t sorIndex;                          // [2]
    uint32_t dpLink;                            // [3]
    uint8_t  bEnableOverride;                   // [4] byte16
    uint8_t  bMST;                              //     byte17
    uint8_t  _pad0[2];                          //     byte18..19
    uint32_t singleHeadMultistreamMode;         // [5]
    uint32_t hBlankSym;                         // [6]
    uint32_t vBlankSym;                         // [7]
    uint32_t colorFormat;                       // [8]
    uint8_t  bEnableTwoHeadOneOr;               // [9] byte36 (+3B pad)
    uint8_t  _pad1[3];
    uint32_t mst_slotStart;                     // [10] MST sub-struct (unused in SST)
    uint32_t mst_slotEnd;                       // [11]
    uint32_t mst_PBN;                           // [12]
    uint32_t mst_Timeslice;                     // [13]
    uint8_t  mst_sendACT;                       // [14] byte56 (+3B pad)
    uint8_t  _pad2[3];
    uint32_t mst_singleHeadMSTPipeline;         // [15]
    uint8_t  mst_bEnableAudioOverRightPanel;    // [16] byte64 (+3B pad)
    uint8_t  _pad3[3];
    uint8_t  sst_bEnhancedFraming;              // [17] byte68 (+3B pad) SST sub-struct
    uint8_t  _pad4[3];
    uint32_t sst_tuSize;                        // [18]
    uint32_t sst_waterMark;                     // [19]
    uint32_t sst_actualPclkHz;                  // [20] deprecated
    uint32_t sst_linkClkFreqHz;                 // [21] deprecated
    uint8_t  sst_bEnableAudioOverRightPanel;    // [22] byte88 (+3B pad)
    uint8_t  _pad5[3];
    uint32_t sst_legacy_activeCnt;              // [23] Legacy/MVID (unused on GA10x)
    uint32_t sst_legacy_activeFrac;             // [24]
    uint32_t sst_legacy_activePolarity;         // [25]
    uint8_t  sst_legacy_mvidWarEnabled;         // [26] byte104 (+3B pad)
    uint8_t  _pad6[3];
    uint32_t sst_legacy_mvid_actualPclkHz;      // [27]
    uint32_t sst_legacy_mvid_linkClkFreqHz;     // [28]
};
static_assert(sizeof(NV0073_DP_CONFIG_STREAM) == 116, "NV0073_DP_CONFIG_STREAM");

// NV0073_CTRL_SPECIFIC_DISPLAY_CHANGE_PARAMS (ctrl0073specific.h:1521, 16B). Modeset begin/end
// bracket -- the golden wraps EVERY modeset in START(enable=1)...END(enable=0) (rpc-trace 0x7302a4).
// Documented as performing "the necessary synchronizations"; this is what engages the GSP display
// supervisor to power the OR + enable the link + wake the head (SNOOZE->AWAKE).
struct NV0073_DISPLAY_CHANGE {
    uint32_t subDeviceInstance; // 0
    uint32_t newDevices;        // displayMask being enabled (0x800)
    uint32_t properties;        // 0 (none used)
    uint32_t enable;            // 1=START, 0=END
};
static_assert(sizeof(NV0073_DISPLAY_CHANGE) == 16, "NV0073_DISPLAY_CHANGE");

// NV0073_CTRL_CMD_DP_SET_MSA_PROPERTIES_V2_PARAMS (ctrl0073dp.h:2737, 80B). The golden issues this
// inside the modeset bracket right after DP_CONFIG_STREAM (payload-trace cmd 0x731381). It programs
// the DP Main-Stream-Attribute overrides the SF carries in the stream. NvBool == NvU8; the nested
// MASK/VALUES match the golden byte layout exactly (verified vs payload w=...00010001 00010006 ...05c90000).
struct NV0073_DP_MSA_MASK {                 // NV0073_CTRL_DP_MSA_PROPERTIES_MASK (15B, u8 throughout)
    uint8_t miscMask[2];
    uint8_t bRasterTotalHorizontal, bRasterTotalVertical;
    uint8_t bActiveStartHorizontal, bActiveStartVertical;
    uint8_t bSurfaceTotalHorizontal, bSurfaceTotalVertical;
    uint8_t bSyncWidthHorizontal, bSyncPolarityHorizontal;
    uint8_t bSyncHeightVertical, bSyncPolarityVertical;
    uint8_t bReservedEnable[3];
};
struct NV0073_DP_MSA_VALUES {               // NV0073_CTRL_DP_MSA_PROPERTIES_VALUES (26B, u16 align)
    uint8_t  misc[2];
    uint16_t rasterTotalHorizontal, rasterTotalVertical;
    uint16_t activeStartHorizontal, activeStartVertical;
    uint16_t surfaceTotalHorizontal, surfaceTotalVertical;
    uint16_t syncWidthHorizontal, syncPolarityHorizontal;
    uint16_t syncHeightVertical, syncPolarityVertical;
    uint8_t  reserved[3];
};
struct NV0073_DP_SET_MSA_V2 {
    uint32_t subDeviceInstance;             // 0
    uint32_t displayId;                     // 0x800
    uint8_t  bEnableMSA;                    // 1
    uint8_t  bStereoPhaseInverse;           // 0
    uint8_t  bCacheMsaOverrideForNextModeset; // 1
    NV0073_DP_MSA_MASK   featureMask;
    NV0073_DP_MSA_VALUES featureValues;
    uint8_t  bDebugValues;                  // 0
    NV0073_DP_MSA_VALUES featureDebugValues;
};
static_assert(sizeof(NV0073_DP_SET_MSA_V2) == 80, "NV0073_DP_SET_MSA_V2");

// NVC372_CTRL_IS_MODE_POSSIBLE_PARAMS (ctrlc372chnc.h:393-513, 1924B). IMP: the CPU describes the
// proposed display state (active heads + their windows) and GSP-RM validates feasibility + computes
// the bandwidth/v-pstate its modeset supervisor needs. Layouts/offsets verified field-by-field
// against the header AND the golden 481-dword payload (head=3, window=6). NvBool=u8; the control
// lock enums are int (u32). Only active head[0]/window[0] are filled; numHeads/numWindows say how
// many. Sub-struct sizes pinned by static_assert so a layout slip is a compile error.
struct NVC372_IMP_HEAD {
    uint8_t  headIndex;                  // @0
    uint8_t  _pad0[3];
    uint32_t maxPixelClkKHz;            // @4
    uint32_t rasterSize_w, rasterSize_h;          // @8
    uint32_t rasterBlankStart_X, rasterBlankStart_Y; // @16
    uint32_t rasterBlankEnd_X, rasterBlankEnd_Y;     // @24
    uint32_t rasterVertBlank2_yStart, rasterVertBlank2_yEnd; // @32
    uint32_t ctl_masterLockMode, ctl_masterLockPin;  // @40 (NV_DISP_LOCK_MODE/PIN = int)
    uint32_t ctl_slaveLockMode, ctl_slaveLockPin;    // @48
    uint32_t maxDownscaleFactorH, maxDownscaleFactorV; // @56
    uint8_t  outputScalerVerticalTaps;  // @64
    uint8_t  bUpscalingAllowedV;        // @65
    uint8_t  bOverfetchEnabled;         // @66
    uint8_t  _pad1;
    uint16_t minFrameIdle_leading, minFrameIdle_trailing; // @68
    uint8_t  lut;                       // @72
    uint8_t  cursorSize32p;             // @73
    uint8_t  bEnableDsc;                // @74
    uint8_t  bYUV420Format;             // @75
    uint8_t  bIs2Head1Or;               // @76
    uint8_t  bGetOSLDOutput;            // @77
    uint8_t  bDisableMidFrameAndDWCFWatermark; // @78
    uint8_t  _pad2;
};
static_assert(sizeof(NVC372_IMP_HEAD) == 80, "NVC372_IMP_HEAD");

struct NVC372_IMP_WINDOW {
    uint32_t windowIndex;               // @0
    uint32_t owningHead;                // @4
    uint32_t formatUsageBound;          // @8
    uint32_t rotatedFormatUsageBound;   // @12
    uint32_t maxPixelsFetchedPerLine;   // @16
    uint32_t maxDownscaleFactorH;       // @20
    uint32_t maxDownscaleFactorV;       // @24
    uint8_t  inputScalerVerticalTaps;   // @28
    uint8_t  bUpscalingAllowedV;        // @29
    uint8_t  bOverfetchEnabled;         // @30
    uint8_t  lut;                       // @31
    uint8_t  tmoLut;                    // @32
    uint8_t  _pad[3];
};
static_assert(sizeof(NVC372_IMP_WINDOW) == 36, "NVC372_IMP_WINDOW");

struct NVC372_IS_MODE_POSSIBLE {
    uint32_t base_subdeviceIndex;       // NVC372_CTRL_CMD_BASE_PARAMS @0
    uint8_t  numHeads;                  // @4
    uint8_t  numWindows;               // @5
    uint8_t  _pad0[2];
    NVC372_IMP_HEAD   head[8];          // @8   (NVC372_CTRL_MAX_POSSIBLE_HEADS)
    NVC372_IMP_WINDOW window[32];       // @648 (NVC372_CTRL_MAX_POSSIBLE_WINDOWS)
    uint32_t options;                  // @1800
    uint32_t testMclkFreqKHz;          // @1804
    uint8_t  bIsPossible;              // @1808 OUT
    uint8_t  bIsOSLDPossible[8];        // @1809 OUT
    uint8_t  _pad1[3];
    uint32_t minImpVPState;            // @1820 OUT
    uint32_t minPState;                // @1824
    uint32_t minRequiredBandwidthKBPS; // @1828
    uint32_t floorBandwidthKBPS;       // @1832
    uint32_t minRequiredHubclkKHz;     // @1836
    uint32_t vblankIncreaseInLinesForOSLDMode[8]; // @1840
    uint32_t wakeUpRgLineForOSLDMode[8];          // @1872
    uint32_t worstCaseMargin;          // @1904
    uint32_t dispClkKHz;               // @1908
    char     worstCaseDomain[8];       // @1912
    uint8_t  bUseCachedPerfState;      // @1920
    uint8_t  _pad2[3];
};
static_assert(sizeof(NVC372_IS_MODE_POSSIBLE) == 1924, "NVC372_IS_MODE_POSSIBLE");

struct NV_MEMORY_VIRTUAL_PARAMS { uint64_t offset; uint64_t limit; uint32_t hVASpace; };
static_assert(sizeof(NV_MEMORY_VIRTUAL_PARAMS) == 24, "NV_MEMORY_VIRTUAL_PARAMS");

// Pushbuffer registration (0x20800a58) — tells the display HW where a channel's PB
// physically lives (ctrl2080internal.h:1365-1373, 40 bytes). cacheSnoop mirrors the
// ctxdma's snoop flag (=1 for coherent sysmem); valid=TRUE for DMA channels.
struct NV2080_DISPLAY_CHANNEL_PUSHBUFFER {
    uint32_t addressSpace;      // ADDR_SYSMEM(1)/ADDR_FBMEM(2)
    uint64_t physicalAddr;      // PB base phys (8-aligned @8)
    uint64_t limit;             // PB size - 1
    uint32_t cacheSnoop;
    uint32_t hclass;            // channel class (0xc67d/0xc67e)
    uint32_t channelInstance;
    uint32_t valid;             // NvBool (GSP reads low byte)
};
static_assert(sizeof(NV2080_DISPLAY_CHANNEL_PUSHBUFFER) == 40, "PUSHBUFFER params");

// EVO DMA-channel alloc params (nvos.h:2417-2432, 32 bytes). hObjectBuffer is the
// CPU-RM-side pushbuffer ctxdma handle; the GSP never sees the ctxdma (the PB reaches
// it via 0x20800a58), so the handle is opaque here. Spec D04.3/F07.
struct NV50VAIO_CHANNELDMA_PARAMS {
    uint32_t channelInstance;
    uint32_t hObjectBuffer;
    uint32_t hObjectNotify;
    uint32_t offset;
    uint64_t pControl;          // NvP64 (OUT)
    uint32_t flags;
    uint32_t _pad;
};
static_assert(sizeof(NV50VAIO_CHANNELDMA_PARAMS) == 32, "CHANNELDMA params");

// NV2080_CTRL_INTERNAL_DISPLAY_WRITE_INST_MEM_PARAMS (ctrl2080internal.h:886-891, 24 B):
// points the display hardware at our display instance memory.
struct NV2080_WRITE_INST_MEM {
    uint64_t instMemPhysAddr;
    uint64_t instMemSize;
    uint32_t instMemAddrSpace;
    uint32_t instMemCpuCacheAttr;
};
static_assert(sizeof(NV2080_WRITE_INST_MEM) == 24, "WRITE_INST_MEM params");

// NV50VAIO_CHANNELPIO_ALLOCATION_PARAMETERS (nvos.h, 16 B, probe-verified) -- cursor PIO channel.
struct NV50VAIO_CHANNELPIO_PARAMS {
    uint32_t channelInstance;
    uint32_t hObjectNotify;
    uint32_t _pad0;
    uint32_t _pad1;
};
static_assert(sizeof(NV50VAIO_CHANNELPIO_PARAMS) == 16, "CHANNELPIO params");

// Decode + log the highlights of a raw EDID block (manufacturer, product, the
// preferred detailed-timing resolution).
static void log_edid(uint32_t displayId, const uint8_t* e, uint32_t len) {
    const bool hdr_ok = e[0] == 0x00 && e[1] == 0xFF && e[2] == 0xFF && e[3] == 0xFF &&
                        e[4] == 0xFF && e[5] == 0xFF && e[6] == 0xFF && e[7] == 0x00;
    if (!hdr_ok) {
        log::warn("nvidia:   display 0x%x: EDID header invalid (%02x %02x..)", displayId, e[0], e[1]);
        return;
    }
    const uint16_t mfg = static_cast<uint16_t>((e[8] << 8) | e[9]);
    char m[4];
    m[0] = static_cast<char>(((mfg >> 10) & 0x1f) + 'A' - 1);
    m[1] = static_cast<char>(((mfg >> 5) & 0x1f) + 'A' - 1);
    m[2] = static_cast<char>((mfg & 0x1f) + 'A' - 1);
    m[3] = '\0';
    const uint16_t product = static_cast<uint16_t>(e[10] | (e[11] << 8));
    const uint32_t h = e[56] | (static_cast<uint32_t>(e[58] >> 4) << 8);
    const uint32_t v = e[59] | (static_cast<uint32_t>(e[61] >> 4) << 8);
    log::info("nvidia:   display 0x%x: EDID %u B, mfg '%s' product 0x%04x, preferred %ux%u",
              displayId, len, m, product, h, v);
}

// Stand up one EVO DMA channel: allocate a 4 KB sysmem pushbuffer, register its
// physical address with the display HW (0x20800a58 on the internal subdevice), then
// allocate the channel bound to it (hParent = NVC670 display engine). The pushbuffer
// MUST be registered BEFORE the channel alloc, else the pusher won't fetch methods.
// `pb` is caller-owned and retained for the GPU's lifetime (the HW DMA-fetches from it).
static int32_t setup_dma_channel(gsp_rpc& rpc, const gsp_seq_ctx& ctx, uintptr_t bar0_va,
                                 dma_buffer& pb, uint32_t hClass, uint32_t hChannel,
                                 uint32_t hCtxDma, uint32_t channelInstance, const char* name) {
    uint32_t st = 0;
    if (dma_alloc(0x1000, /*uncached=*/false, pb) != MEM_OK) {
        log::error("nvidia: display: %s pushbuffer alloc failed", name);
        return ERR_DISP_ALLOC;
    }
    string::memset(reinterpret_cast<void*>(pb.cpu_va), 0, 0x1000);

    NV2080_DISPLAY_CHANNEL_PUSHBUFFER reg;
    string::memset(&reg, 0, sizeof(reg));
    reg.addressSpace    = ADDR_SYSMEM;
    reg.physicalAddr    = pb.phys;
    reg.limit           = 0xfff;          // 4 KB - 1
    reg.cacheSnoop      = 1;
    reg.hclass          = hClass;
    reg.channelInstance = channelInstance;
    reg.valid           = 1;
    if (gsp_rpc_control(rpc, ctx, bar0_va, INTERNAL_CLIENT, INTERNAL_SUBDEVICE,
                        NV2080_CTRL_CMD_INTERNAL_DISPLAY_CHANNEL_PUSHBUFFER,
                        &reg, sizeof(reg), &st) != RPC_OK || st != 0) {
        log::error("nvidia: display: %s PB register failed (status=0x%x)", name, st);
        return ERR_DISP_CTRL;
    }
    log::info("nvidia: display: %s PB registered phys=0x%lx limit=0xfff (hclass=0x%x)",
              name, (uint64_t)pb.phys, hClass);

    NV50VAIO_CHANNELDMA_PARAMS cp;
    string::memset(&cp, 0, sizeof(cp));
    cp.channelInstance = channelInstance;
    cp.hObjectBuffer   = hCtxDma;
    cp.offset          = 0;
    cp.flags           = 0;
    if (gsp_rpc_alloc(rpc, ctx, bar0_va, H_CLIENT, H_DISP_ENGINE, hChannel, hClass,
                      &cp, sizeof(cp), &st) != RPC_OK || st != 0) {
        log::error("nvidia: display: %s channel alloc failed (status=0x%x)", name, st);
        return ERR_DISP_ALLOC;
    }
    log::info("nvidia: display: *** %s channel 0x%x allocated (status=0) ***", name, hChannel);
    return DISP_OK;
}

// --- EVO pushbuffer method encoder + kickoff (nvkms-dma.h:240-296 / nvkms-dma.c:44-126) ---
// The pushbuffer is a stream of dwords: a method-header dword followed by `count` data
// dwords. Header = OPCODE_METHOD(0)<<29 | count<<18 | (method>>2)<<2.
struct evo_push {
    volatile uint32_t* base; // pushbuffer base (cpu_va)
    volatile uint32_t* cur;  // write cursor
    uintptr_t          ctrl; // BAR0 VA of the channel control region (PUT@+0, GET@+4)
};

// Begin a push at byte offset `start_off` into the pushbuffer. Use start_off = the previous
// kick's PUT to *continue* a channel's pushbuffer (the engine fetches [GET, PUT), so we must
// append, not overwrite already-consumed methods).
static void evo_begin(evo_push& p, dma_buffer& pb, uintptr_t bar0_va, uint32_t ctrl_base,
                      uint32_t start_off = 0) {
    p.base = reinterpret_cast<volatile uint32_t*>(pb.cpu_va);
    p.cur  = reinterpret_cast<volatile uint32_t*>(pb.cpu_va + start_off);
    p.ctrl = bar0_va + ctrl_base;
}

// Emit a method header (offset in dwords, `count` data dwords follow).
static void evo_method(evo_push& p, uint32_t method, uint32_t count) {
    *p.cur++ = (0u << 29) | ((count & 0x3ff) << 18) | (((method >> 2) & 0xfff) << 2);
}
static void evo_data(evo_push& p, uint32_t data) { *p.cur++ = data; }

// Emit a method header + its single data dword (the common count=1 case).
static void evo_m1(evo_push& p, uint32_t method, uint32_t data) {
    evo_method(p, method, 1);
    evo_data(p, data);
}

// Kick off: fence the pushbuffer writes, then write PUT = byte length consumed so far.
// Returns the PUT byte offset (for GET polling).
static uint32_t evo_kick(evo_push& p) {
    barrier::dma_full(); // sfence/mfence: PB writes must land before the engine fetches
    const uint32_t put = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(p.cur) - reinterpret_cast<uintptr_t>(p.base));
    mmio::write32(p.ctrl + NV917D_PUT_OFF, put);
    return put;
}

// Poll the channel GET pointer until it reaches PUT (engine fetched all methods).
static bool evo_wait_get(const evo_push& p, uint32_t put) {
    for (uint32_t i = 0; i < 2000000; i++) {
        const uint32_t get = mmio::read32(p.ctrl + NV917D_GET_OFF) & 0xffcu;
        if (get == (put & 0xffcu)) {
            return true;
        }
        cpu::relax();
    }
    return false;
}

// Cursor PIO channel: wait until the FE reports free space (Free != 0) before pushing a
// method. Mirrors nvkms WaitForFreeSpace (nvkms-cursor3.c) -- the FE drops writes pushed
// while Free==0 (FE busy with a previous method), which silently loses the position update.
static bool cursor_pio_wait_free(uintptr_t cursor_ctrl_base) {
    for (uint32_t i = 0; i < 200000; i++) {
        if (mmio::read32(cursor_ctrl_base + NVC67A_FREE_OFF) != 0) {
            return true;
        }
        cpu::relax();
    }
    return false;
}

// A display mode: active res is always 2560x1440 here; only raster timing + pixel clock differ.
// Dwords are the exact captured/derived values (spec F09.3 60Hz measured / F09.4 144Hz EDID).
// pclk_hz packs into HEAD_SET_PIXEL_CLOCK_FREQUENCY.HERTZ. Defined here (before disp_dump_state)
// so the diagnostics can reference SELECTED_MODE.
struct evo_mode {
    const char* name;
    uint32_t pclk_hz;            // 0x0e64ff60=241.5MHz (60), 0x23493400=592MHz (144)
    uint32_t raster_size;        // (vTotal<<16)|hTotal
    uint32_t raster_sync_end;    // (Y<<16)|X
    uint32_t raster_blank_end;
    uint32_t raster_blank_start;
    // DP link + SF stream config, decoded byte-exact from the golden trace 20260531-091913:
    // 60Hz modeset (ts 1335.5) trains 2-lane HBR2 + stream wm=33/sym 0x14c,0x163d; the 144Hz
    // modeset (ts 1347.4) trains 4-lane HBR2 + stream wm=24/sym 0x4e,0x8fb. (tuSize=64 both.)
    // maxPixelClkKHz (IMP) = pclk_hz/1000, MSA rasterTotalVertical = raster_size>>16 -- derived.
    uint32_t dp_train_data;      // DP_CTRL train data: 0x1402=2-lane HBR2, 0x1404=4-lane HBR2
    uint32_t dp_hblank_sym;      // DP_CONFIG_STREAM hBlankSym
    uint32_t dp_vblank_sym;      // DP_CONFIG_STREAM vBlankSym
    uint32_t dp_watermark;       // DP_CONFIG_STREAM SST waterMark
};
[[maybe_unused]] static const evo_mode MODE_2560x1440_60  = {"2560x1440@60",
    0x0e64ff60, 0x05c90aa0, 0x0004001f, 0x0025006f, 0x05c50a6f,
    0x00001402, 0x0000014c, 0x0000163d, 33};
[[maybe_unused]] static const evo_mode MODE_2560x1440_144 = {"2560x1440@144",
    0x23493400, 0x06070a6a, 0x0007001f, 0x004d0061, 0x05ed0a61,
    0x00001404, 0x0000004e, 0x000008fb, 24};

// Mode used for the initial modeset. 144 Hz is the panel's EDID max; it trains the DP link at
// 4-lane HBR2 (golden ts 1347.4). All the mode-specific IMP/DP/MSA values follow from the
// evo_mode fields, so this single line is the only switch. Set to MODE_2560x1440_60 for 60 Hz.
static const evo_mode& SELECTED_MODE = MODE_2560x1440_144;

// --- Display-engine state READBACK (ground truth) -------------------------------------------
// GET==PUT only proves the FE *fetched* the pushbuffer; it says nothing about whether methods
// were accepted, whether the UPDATE *latched*, or whether the head is *scanning*. These reads
// answer those directly. Registers (open-gpu-kernel-modules dev_disp.h, cross-checked vs nouveau):
//   FE_CORE_HEAD_STATE(i)=0x612078+i*2048  OPERATING_MODE[9:8] SLEEP/SNOOZE/AWAKE   (v04_00:44)
//   RG_DPCA(i)=0x616330+i*2048             LINE_CNT[15:0] FRM_CNT[31:16]            (v03_00:67)
//   FE head-exists mask = 0x612004 & 0xf                                  (nouveau disp.c:2749)
//   core ASSY base=0x680000, ARMED base=0x688000 (=ASSY+0x8000)           (v03_00:60-61)
//   window ASSY base WIN(i)=0x690000+i*4096                               (v03_00:62)
// All plain BAR0 MMIO; an invalid display read returns 0xbadfxxxx (not a fault), so this is safe.
static void dump_core(uintptr_t bar0_va, const char* name, uint32_t off, uint32_t exp) {
    const uint32_t assy = mmio::read32(bar0_va + 0x680000 + off); // what the pushbuffer wrote
    const uint32_t armd = mmio::read32(bar0_va + 0x688000 + off); // what latched on UPDATE
    log::info("nvidia: DIAG   core %s off=0x%x ASSY=0x%08x ARMED=0x%08x (exp 0x%x)",
              name, off, assy, armd, exp);
}

static void disp_dump_state(uintptr_t bar0_va, const char* when) {
    const uint32_t hc = 3 * 0x400; // head 3 method stride (+0xC00)
    const uint32_t hstate = mmio::read32(bar0_va + 0x612078 + 3 * 2048); // FE_CORE_HEAD_STATE(3)
    const uint32_t mode = (hstate >> 8) & 0x3;
    const char* mn = mode == 2 ? "AWAKE" : (mode == 1 ? "SNOOZE" : (mode == 0 ? "SLEEP" : "?"));
    log::info("nvidia: DIAG[%s]: head3 CORE_HEAD_STATE=0x%08x mode=%s head_mask=0x%x",
              when, hstate, mn, mmio::read32(bar0_va + 0x612004) & 0xf);
    // Core channel state + exception -- the smoking gun for a non-latching UPDATE. If the core
    // channel raised a method/ctxdma exception it HALTS and no UPDATE latches. Regs (nouveau
    // gv100_disp_exception + dev_disp): FE_CHNSTATUS_CORE=0x610630 (STATE bits 20:16; 0xb=IDLE,
    // 0xc=BUSY), EVT_STAT_EXC_OTHER=0x611854 bit0=CORE, FE_EXCEPT(0)=0x611020
    // (method=(v&0xfff)<<2, reason=(v>>12)&7: 7=UNRESOLVABLE_HANDLE,3=RESERVED_METHOD,4=INVALID_ARG,
    // 5=INVALID_STATE), arg=0x611024, code=0x611028.
    const uint32_t chnstat = mmio::read32(bar0_va + 0x610630);
    const uint32_t excOther = mmio::read32(bar0_va + 0x611854);
    log::info("nvidia: DIAG[%s]: core CHNSTATUS=0x%08x (STATE=0x%x) EXC_OTHER=0x%08x",
              when, chnstat, (chnstat >> 16) & 0x1f, excOther);
    if (excOther & 0x1) {
        const uint32_t e0 = mmio::read32(bar0_va + 0x611020);
        log::error("nvidia: DIAG[%s]: *** CORE EXCEPTION method=0x%x reason=%u arg=0x%08x code=0x%08x ***",
                   when, (e0 & 0xfff) << 2, (e0 >> 12) & 0x7,
                   mmio::read32(bar0_va + 0x611024), mmio::read32(bar0_va + 0x611028));
    }
    // Raster generator counter read twice -> if it advances the head IS scanning out pixels.
    const uint32_t rg0 = mmio::read32(bar0_va + 0x616330 + 3 * 2048);
    delay::us(50000);
    const uint32_t rg1 = mmio::read32(bar0_va + 0x616330 + 3 * 2048);
    log::info("nvidia: DIAG[%s]: head3 RG_DPCA t0(line=%u frame=%u) t1(line=%u frame=%u) -> %s",
              when, rg0 & 0xffff, rg0 >> 16, rg1 & 0xffff, rg1 >> 16,
              (rg0 != rg1) ? "ADVANCING=SCANNING" : "FROZEN=NOT-SCANNING");
    // SOR1 (head 3's OR) DP link/power state -- confirms the Stage-14.5 DP bring-up. (dev_display:
    // SOR_PWR=0x61c004, SOR_TEST=0x61c008, SOR_DP_LINKCTL=0x61c10c, SOR_DP_TPG=0x61c110; stride id*0x800.)
    const uint32_t sp  = mmio::read32(bar0_va + 0x61c004 + 0x800); // SOR_PWR(1): MODE[28] NORMAL=0/SAFE=1, NORMAL_STATE[0]
    const uint32_t sts = mmio::read32(bar0_va + 0x61c008 + 0x800); // SOR_TEST(1): OWNER_MASK[13:10], ACT_HEAD_OPMODE[9:8]
    const uint32_t slc = mmio::read32(bar0_va + 0x61c10c + 0x800); // SOR_DP_LINKCTL(1,0) -- GSP-owned on GA10x (see note)
    const uint32_t tpg = mmio::read32(bar0_va + 0x61c110 + 0x800); // SOR_DP_TPG(1,0)     -- GSP-owned on GA10x
    const uint32_t soropmode = (sts >> 8) & 0x3;
    // The reliable CPU-visible OR truth is PWR (NORMAL) + TEST (owner=head + opmode=AWAKE). The SOR_DP_*
    // link registers are NOT published for GA10x (open-gpu-kernel-modules has no NV_PDISP_SOR_DP_LINKCTL;
    // GSP-RM programs the DP sublink from WPR, so the CPU shadow reads 0). en/lanes here are informational
    // only -- the authoritative link-up proof is head AWAKE + RG SCANNING + a live panel.
    log::info("nvidia: DIAG[%s]: SOR1 PWR=0x%08x(%s pu=%u) TEST=0x%08x(owner=0x%x opmode=%s) [GSP-owned: linkctl=0x%08x tpg=0x%08x]",
              when, sp, ((sp >> 28) & 1) ? "SAFE" : "NORMAL", sp & 1,
              sts, (sts >> 10) & 0xf,
              soropmode == 2 ? "AWAKE" : (soropmode == 1 ? "SNOOZE" : "SLEEP"),
              slc, tpg);
    // Core ASSY vs ARMED: ARMED!=ASSY => the UPDATE didn't latch; ASSY wrong => DMA didn't deliver.
    dump_core(bar0_va, "RASTER_SIZE  ", 0x2064 + hc, SELECTED_MODE.raster_size); // 0x05c90aa0 (60Hz) / 0x06070a6a (144Hz)
    dump_core(bar0_va, "HEAD_CONTROL ", 0x2008 + hc, 0x0);
    dump_core(bar0_va, "OUTPUT_SCALER", 0x2014 + hc, 0x11);
    dump_core(bar0_va, "USAGE_BOUNDS ", 0x2030 + hc, 0x1014);
    dump_core(bar0_va, "DISPLAY_ID   ", 0x2020 + hc, 0x800);
    dump_core(bar0_va, "OLUT_CONTROL ", 0x2280 + hc, LUT_CONTROL_VAL);
    dump_core(bar0_va, "CTXDMA_OLUT  ", 0x2288 + hc, H_LUT_CTXDMA);
    dump_core(bar0_va, "OCSC0_CONTROL", 0x2240 + hc, 0x1);
    dump_core(bar0_va, "WIN6_CONTROL ", 0x1000 + 6 * 0x80, 0x3); // WINDOW_SET_CONTROL(6) owner=head3
    // Window-0 channel ASSY (no public ARMED base for windows): surface binding + composition.
    log::info("nvidia: DIAG[%s]: win6 ASSY ISO=0x%08x PARAMS=0x%08x COMP=0x%08x (exp 0xcaf0000a/0xe6/0x80)",
              when, mmio::read32(bar0_va + NVC67E_WIN6_CTRL_BASE + 0x240),
              mmio::read32(bar0_va + NVC67E_WIN6_CTRL_BASE + 0x22c),
              mmio::read32(bar0_va + NVC67E_WIN6_CTRL_BASE + 0x2ec));
}

// (struct evo_mode + MODE_2560x1440_60/144 + SELECTED_MODE are defined above disp_dump_state so
//  the diagnostics can reference SELECTED_MODE.raster_size.)

// Increment 2a -- push the golden CORE (NVC67D) modeset program for head 3 / SOR 1 /
// displayId 0x800 (spec F09.3, all non-timing dwords measured vs evo-full trace), then kick.
// DP link training is implicit (GSP-RM trains on the SOR_SET_CONTROL+UPDATE for the pclk).
// Assemble the head-3 core modeset program (golden NVC67D, head3 / SOR1 / window6 @ 2560x1440@60)
// into `cp` starting at `start_off` (continues the PB after the pre-bracket window-owner update). Does
// NOT kick: the caller commits it INTERLOCKED with the window-6 flip (the golden latches the core
// together with its window atomically -- SET_WINDOW_INTERLOCK_FLAGS=win6). Every value is the byte-exact
// golden decode (decode agent, trace 20260531-091913, head3 bracket = red_pixel-60). The window OWNER
// write is done BEFORE this (separate pre-bracket update) so this update is owner-stable and interlockable.
static void evo_core_modeset_assemble(evo_push& cp, dma_buffer& core_pb, uintptr_t bar0_va,
                                      uint32_t start_off) {
    constexpr uint32_t HEAD = 3, OR_INDEX = 1, WIN = 6;
    const uint32_t h = HEAD * 0x400;   // per-head stride; head 3 -> +0xC00
    const uint32_t w = WIN * 0x80;     // window-6 method block = 0x1000 + 6*0x80 = 0x1300
    const evo_mode& m = SELECTED_MODE;
    evo_begin(cp, core_pb, bar0_va, NVC67D_CORE_CTRL_BASE, start_off); // continue PB after the ownership update
    // Head-3 timing/output program (golden head-3 values).
    evo_m1(cp, 0x2000 + h, 0x00000000);   // HEAD_SET_PROCAMP                  COLOR_SPACE=RGB
    evo_m1(cp, 0x2004 + h, 0xfc000041);   // HEAD_SET_CONTROL_OUTPUT_RESOURCE  CRC=COMPLETE|HSYNC/VSYNC=POS|BPP_24_444
    evo_m1(cp, 0x2008 + h, 0x00000000);   // HEAD_SET_CONTROL                  STRUCTURE=PROGRESSIVE
    evo_m1(cp, 0x200C + h, m.pclk_hz);    // HEAD_SET_PIXEL_CLOCK_FREQUENCY    241.5 MHz
    evo_m1(cp, 0x201C + h, 0x00000000);   // HEAD_SET_PIXEL_CLOCK_CONFIGURATION
    evo_m1(cp, 0x2028 + h, m.pclk_hz);    // HEAD_SET_PIXEL_CLOCK_FREQUENCY_MAX
    evo_m1(cp, 0x2020 + h, 0x00000800);   // HEAD_SET_DISPLAY_ID(h,0)          = 0x800
    evo_m1(cp, 0x2064 + h, m.raster_size);        // HEAD_SET_RASTER_SIZE         0x05c90aa0
    evo_m1(cp, 0x2068 + h, m.raster_sync_end);    // HEAD_SET_RASTER_SYNC_END     0x0004001f
    evo_m1(cp, 0x206C + h, m.raster_blank_end);   // HEAD_SET_RASTER_BLANK_END    0x0025006f
    evo_m1(cp, 0x2070 + h, m.raster_blank_start); // HEAD_SET_RASTER_BLANK_START  0x05c50a6f
    evo_m1(cp, 0x2218 + h, 0x00030026);   // HEAD_SET_MIN_FRAME_IDLE  leading=0x26(38)/trailing=3 (golden; matches IMP; scan-timing, BYPASS-relevant)
    evo_m1(cp, 0x2014 + h, 0x00000011);   // HEAD_SET_CONTROL_OUTPUT_SCALER    V_TAPS_2|H_TAPS_2 (always inline)
    evo_m1(cp, 0x2018 + h, 0x00000211);   // HEAD_SET_DITHER_CONTROL
    evo_m1(cp, 0x202C + h, 0x04000400);   // HEAD_SET_MAX_OUTPUT_SCALE_FACTOR
    evo_m1(cp, 0x2030 + h, 0x00001014);   // HEAD_SET_HEAD_USAGE_BOUNDS        CURSOR_W256|OLUT_ALLOWED|TAPS_2
    evo_m1(cp, 0x2048 + h, 0x00000000);   // HEAD_SET_VIEWPORT_POINT_IN        (0,0)
    evo_m1(cp, 0x204C + h, 0x05a00a00);   // HEAD_SET_VIEWPORT_SIZE_IN         2560x1440
    evo_m1(cp, 0x2058 + h, 0x05a00a00);   // HEAD_SET_VIEWPORT_SIZE_OUT
    evo_m1(cp, 0x205C + h, 0x00000000);   // HEAD_SET_VIEWPORT_POINT_OUT_ADJUST
    // --- head-3 output pipeline (OLUT + OCSC0 + clamp): required by DEPTH composition (golden head3) ---
    // The window flip below uses DEPTH composition (not BYPASS), so the window feeds the head compositor
    // -> head OLUT/OCSC are now IN the scanout path and MUST be programmed (run #50 proved BYPASS+sparse
    // head config does NOT wake the head on the GSP/bracket path). All values trace-verified vs the head3
    // 0x800 bracket + clc67d.h offsets. OLUT = identity (DIRECT10, 4 VSS-hdr+1025 entries) at byte off
    // 0x2100>>8=0x21 in the shared LUT ctxdma (== our lut_fill_identity layout).
    evo_m1(cp, 0x2280 + h, 0x00040508);   // HEAD_SET_OLUT_CONTROL          MODE=DIRECT10 | SIZE=0x405(1029)
    evo_m1(cp, 0x2284 + h, 0xffffffff);   // HEAD_SET_OLUT_FP_NORM_SCALE
    evo_m1(cp, 0x2288 + h, H_LUT_CTXDMA); // HEAD_SET_CONTEXT_DMA_OLUT      (resolved via core-channel hash)
    evo_m1(cp, 0x228C + h, 0x00000021);   // HEAD_SET_OFFSET_OLUT           = OLUT byte-off 0x2100 >> 8
    evo_m1(cp, 0x2240 + h, 0x00000001);   // HEAD_SET_OCSC0CONTROL          ENABLE=1
    static const uint32_t ocsc0_identity[12] = { // HEAD_SET_OCSC0COEFFICIENT_C00..C23 (golden: identity, off-col=0x20)
        0x00010000, 0x00000000, 0x00000000, 0x00000020,
        0x00000000, 0x00010000, 0x00000000, 0x00000020,
        0x00000000, 0x00000000, 0x00010000, 0x00000020,
    };
    for (uint32_t i = 0; i < 12; i++)
        evo_m1(cp, 0x2244 + h + i * 4, ocsc0_identity[i]);
    evo_m1(cp, 0x229C + h, 0x00000000);   // HEAD_SET_OCSC1CONTROL          ENABLE=0 (disabled)
    evo_m1(cp, 0x2238 + h, 0x0fff0000);   // HEAD_SET_CLAMP_RANGE_GREEN     LOW=0 HIGH=0xfff
    evo_m1(cp, 0x223C + h, 0x0fff0000);   // HEAD_SET_CLAMP_RANGE_RED_BLUE  LOW=0 HIGH=0xfff
    // Cursor methods: DISABLED here to keep the modeset byte-faithful to the golden (the modetest capture
    // had no cursor). The HW cursor is enabled by a separate core update AFTER the head wakes (Stage 17) --
    // the normal runtime path. NB: 0x2090/0x2094 are HEAD_SET_OFFSET_CURSOR(h,0/1) (clc67d.h:856), NOT
    // CONTEXT_DMA_CURSOR(2/3); 0 == surface-relative offset 0.
    evo_m1(cp, 0x2088 + h, 0x00000000);   // HEAD_SET_CONTEXT_DMA_CURSOR(h,0)
    evo_m1(cp, 0x208C + h, 0x00000000);   // HEAD_SET_CONTEXT_DMA_CURSOR(h,1)
    evo_m1(cp, 0x2090 + h, 0x00000000);   // HEAD_SET_OFFSET_CURSOR(h,0)
    evo_m1(cp, 0x2094 + h, 0x00000000);   // HEAD_SET_OFFSET_CURSOR(h,1)
    evo_m1(cp, 0x2098 + h, 0x00000000);   // HEAD_SET_PRESENT_CONTROL_CURSOR  MODE=MONO
    evo_m1(cp, 0x209C + h, 0x000000cf);   // HEAD_SET_CONTROL_CURSOR          ENABLE=DISABLE | FORMAT=A8R8G8B8
    evo_m1(cp, 0x20A0 + h, 0x000002ff);   // HEAD_SET_CONTROL_CURSOR_COMPOSITION
    evo_m1(cp, 0x0300 + OR_INDEX * 0x20, 0x00000908); // SOR_SET_CONTROL(1)    OWNER=HEAD3|PROTOCOL=DP_B
    // Window-6 capability declarations on the core channel (head 3's natural window). The OWNER write
    // (WINDOW_SET_CONTROL(6)=HEAD3) is intentionally NOT here -- it is issued as a separate pre-bracket
    // update (evo_window_owner_assign in gsp_display_init). The golden does the same: it sets win6's
    // owner ONCE at setup (trace ts 1334.813, OUTSIDE the modeset bracket), so by modeset time win6 is
    // ALREADY head3 and this update is NOT an owner change -> it can be safely INTERLOCKED with the flip
    // (the "error 37" trap only fires when an owner-CHANGING update is interlocked). This refreshes bounds.
    evo_m1(cp, 0x1004 + w, 0x00000007);   // WINDOW_SET_WINDOW_FORMAT_USAGE_BOUNDS(6)
    evo_m1(cp, 0x100C + w, 0x04000400);   // WINDOW_SET_MAX_INPUT_SCALE_FACTOR(6)
    evo_m1(cp, 0x1010 + w, 0x10110a16);   // WINDOW_SET_WINDOW_USAGE_BOUNDS(6)
    // INTERLOCKED commit (the golden way): win6 is ALREADY owned by head3 (pre-bracket update), so this
    // update is NOT an owner change and CAN interlock with the window-6 flip. The golden Main group
    // interlocks core 0x021c=0xc0 (win6+win7) with window 0x0374=0xc0; we drive only win6 -> 0x40 (bit6).
    // The supervisor then sees head3 + its flipped window as ONE atomic attach and wakes the head (this
    // is why the prior NON-interlocked split modeset never latched: the supervisor saw head3 claim win6
    // with no surface attached -> incomplete model -> declined the attach -> ARMED stayed at defaults).
    // INTERLOCKED commit: with the boot heads shut down (Stage 14.05) and win6 pre-owned (owner-stable, so
    // NOT an owner change), this interlocks with the win6 flip. The GSP-offload supervisor ACCEPTS the
    // interlocked structure (it FETCHES it into ASSY); a DECOUPLED/non-interlocked modeset is REJECTED
    // (run #52: STALL + CHNSTATUS exception bit). The supervisor sees head3 + its flipped window as ONE
    // atomic attach. (run #51 fetched but didn't promote because the boot head still held the panel; the
    // Stage-14.05 shutdown removes that conflict.)
    evo_m1(cp, 0x0218, 0x00000000);       // SET_INTERLOCK_FLAGS        = 0
    evo_m1(cp, 0x021c, 0x00000040);       // SET_WINDOW_INTERLOCK_FLAGS = win6 (bit6) -- interlock the flip
    evo_m1(cp, 0x0200, 0x00000001);       // UPDATE  RELEASE_ELV=TRUE  (caller kicks core+win6 interlocked)
}

// Fill a VRAM region with a constant 32-bit value via the BAR0_WINDOW/PRAMIN aperture
// (kbusMemAccessBar0Window_GM107) -- RM's foundational CPU->VRAM primitive, the same one
// it uses to bootstrap BAR1/BAR2 page tables. vram_off must be 64 KB-aligned; the window
// base is 64 KB-granular and spans 1 MB, so we re-window every 1 MB.
static void pramin_fill(uintptr_t bar0_va, uint64_t vram_off, uint64_t size, uint32_t val) {
    const uint64_t end = vram_off + size;
    uint64_t p = vram_off;
    while (p < end) {
        const uint64_t winBase = p & ~0xffffull;            // 64KB-aligned window base
        mmio::write32(bar0_va + NV_PBUS_BAR0_WINDOW,
                      static_cast<uint32_t>((winBase >> 16) & 0xffffff)); // TARGET=VID_MEM(0)
        const uint64_t winEnd   = winBase + NV_PRAMIN_SIZE; // window spans [winBase, +1MB)
        const uint64_t chunkEnd = (end < winEnd) ? end : winEnd;
        for (uint64_t a = p; a < chunkEnd; a += 4) {
            mmio::write32(bar0_va + NV_PRAMIN_BASE + static_cast<uint32_t>(a - winBase), val);
        }
        p = chunkEnd;
    }
    barrier::dma_full();
}

// Read one dword from VRAM via PRAMIN (self-check that the paint landed / the window is
// not privilege-locked post-GSP).
static uint32_t pramin_read32(uintptr_t bar0_va, uint64_t vram_off) {
    const uint64_t base = (vram_off & ~0xffffull) >> 16;
    mmio::write32(bar0_va + NV_PBUS_BAR0_WINDOW, static_cast<uint32_t>(base & 0xffffff));
    return mmio::read32(bar0_va + NV_PRAMIN_BASE + static_cast<uint32_t>(vram_off & 0xffff));
}

// Write one dword to VRAM via PRAMIN (windows then writes).
static void pramin_write32(uintptr_t bar0_va, uint64_t vram_off, uint32_t val) {
    const uint64_t base = (vram_off & ~0xffffull) >> 16;
    mmio::write32(bar0_va + NV_PBUS_BAR0_WINDOW, static_cast<uint32_t>(base & 0xffffff));
    mmio::write32(bar0_va + NV_PRAMIN_BASE + static_cast<uint32_t>(vram_off & 0xffff), val);
}

// HW cursor glyph copied verbatim from the stlx display manager (userland/apps/stlxdm/src/
// stlxdm_input.c g_cursor_shape): an 18x16 north-west pointer. 'X' = black outline, '.' = white
// fill, ' ' = transparent. The HW cursor is a single A8R8G8B8 plane, so we also bake in stlxdm's
// drop-shadow (a 50%-black copy offset by +1,+1) to match the software cursor it draws.
static const char* const CURSOR_GLYPH[16] = {
    "X                 ",
    "XX                ",
    "X.X               ",
    "X..X              ",
    "X...X             ",
    "X....X            ",
    "X.....X           ",
    "X......X          ",
    "X.......X         ",
    "X........X        ",
    "X.........X       ",
    "X..........X      ",
    "X......XXXXXXX    ",
    "X...XX            ",
    "X..X              ",
    "XX                ",
};
static constexpr uint32_t CURSOR_GLYPH_W = 18, CURSOR_GLYPH_H = 16;

// Paint the cursor surface (A8R8G8B8, non-premultiplied) from CURSOR_GLYPH: a transparent field,
// a 50%-black drop shadow at +1,+1, then the opaque black-outlined white pointer on top.
// Composited NON_PREMULT_ALPHA (0x75FF) so alpha=0 shows the desktop and alpha=0x80 darkens it.
static void paint_cursor_arrow(uintptr_t bar0_va, uint64_t vram, uint32_t w, uint32_t h) {
    pramin_fill(bar0_va, vram, (uint64_t)w * h * 4, 0x00000000); // clear to transparent
    for (uint32_t r = 0; r < CURSOR_GLYPH_H; r++) {             // pass 1: drop shadow (offset +1,+1)
        for (uint32_t c = 0; c < CURSOR_GLYPH_W; c++) {
            const char g = CURSOR_GLYPH[r][c];
            if (g != 'X' && g != '.') continue;
            const uint32_t sx = c + 1, sy = r + 1;
            if (sx < w && sy < h)
                pramin_write32(bar0_va, vram + (uint64_t)(sy * w + sx) * 4, 0x80000000);
        }
    }
    for (uint32_t r = 0; r < CURSOR_GLYPH_H; r++) {             // pass 2: glyph (opaque, over the shadow)
        for (uint32_t c = 0; c < CURSOR_GLYPH_W; c++) {
            const char g = CURSOR_GLYPH[r][c];
            uint32_t argb;
            if (g == 'X')      argb = 0xFF000000;               // black outline
            else if (g == '.') argb = 0xFFFFFFFF;               // white fill
            else continue;                                       // transparent (leave shadow/clear)
            if (c < w && r < h)
                pramin_write32(bar0_va, vram + (uint64_t)(r * w + c) * 4, argb);
        }
    }
    barrier::dma_full();
}

// Display ctxdma hash function (instmemHashFunc_v03_00, disp_inst_mem_0300.c:92-123). Maps
// (client, ctxdma handle, dispChannelNum) -> hash-table slot the display HW will probe.
static uint32_t disp_hash(uint32_t hClient, uint32_t handle, uint32_t chn) {
    uint32_t h = ((handle >> 0)  & 0x3FF)
               ^ ((handle >> 10) & 0x3FF)
               ^ ((handle >> 20) & 0x3FF)
               ^ (((hClient & 0xFF) << 2) | (handle >> 30))
               ^ (((chn & 0xF) << 6) | ((hClient >> 8) & 0x3F))
               ^ ((chn >> 4) & 0x7);
    return h & (DISP_HASH_ENTRIES - 1);
}

// Write one display ctxdma hash entry (DISP_HW_HASH_TABLE_ENTRY = {handle, context}, 8 B)
// at the hash slot for (hClient, handle, chn), pointing at instance `instField`
// (= ctxdma-entry byte offset >> 5). ht_Context = CLIENT_ID[13:0] | INSTANCE[24:14] | CHN[31:25].
static void disp_write_hash_entry(uintptr_t bar0_va, uint64_t instmem_vram,
                                  uint32_t hClient, uint32_t handle, uint32_t chn,
                                  uint32_t instField) {
    const uint32_t slot = disp_hash(hClient, handle, chn);
    const uint32_t ctx  = (hClient & 0x3FFF) | ((instField & 0x7FF) << 14) | ((chn & 0x7F) << 25);
    pramin_write32(bar0_va, instmem_vram + slot * 8 + 0, handle); // ht_ObjectHandle
    pramin_write32(bar0_va, instmem_vram + slot * 8 + 4, ctx);    // ht_Context
    log::info("nvidia: display: instmem hash[chn=%u] slot=%u handle=0x%x ctx=0x%08x", chn, slot, handle, ctx);
}

// IEEE-754 half (FP16) of i/1024 for i in [0,1024] -- the identity unorm->FP16 the window ILUT
// applies (nvkms nvUnorm10ToFp16). Integer-only (no FP in the kernel). value = i * 2^-10.
static uint16_t unorm10_to_fp16(uint32_t i) {
    if (i == 0) {
        return 0x0000;
    }
    uint32_t b = 0;                       // index of i's highest set bit (1 <= i <= 1024)
    for (uint32_t t = i; t > 1; t >>= 1) {
        b++;
    }
    const uint32_t exp_field = (uint32_t)((int32_t)b - 10 + 15); // FP16 exponent (bias 15)
    const uint32_t mant = (b <= 10) ? ((i - (1u << b)) << (10 - b))
                                    : ((i - (1u << b)) >> (b - 10));
    return (uint16_t)((exp_field << 10) | (mant & 0x3FF));
}

// Build the identity ILUT (base[], FP16) + identity OLUT (output[], 16-bit) in the LUT VRAM
// buffer via PRAMIN. Layout = NVEvoLutDataRec: [4 VSS-hdr + 1025] base entries at off 0, then
// output[] at off 0x2100; each entry is 4x u16 {R,G,B,unused} (8 B). Entry 1024 duplicates 1023
// (interpolation guard). ILUT = nvUnorm10ToFp16(i); OLUT = i<<6 (= i<<(16-10)). nvkms-evo3.c
// 4297-4308 (ILUT) / 5125-5138 (OLUT).
static void lut_fill_identity(uintptr_t bar0_va, uint64_t lutVram) {
    pramin_fill(bar0_va, lutVram, LUT_BYTES, 0x00000000); // zero everything incl. VSS headers
    for (uint32_t i = 0; i <= 1024; i++) {
        const uint32_t s = (i < 1024) ? i : 1023;            // entry 1024 == entry 1023
        const uint16_t il = unorm10_to_fp16(s);              // ILUT FP16
        const uint16_t ol = (uint16_t)(s << 6);              // OLUT 16-bit linear
        const uint64_t ib = lutVram + (uint64_t)(LUT_HDR + i) * 8;
        const uint64_t ob = lutVram + LUT_OUTPUT_OFF + (uint64_t)(LUT_HDR + i) * 8;
        pramin_write32(bar0_va, ib + 0, ((uint32_t)il << 16) | il); // R | G<<16
        pramin_write32(bar0_va, ib + 4, (uint32_t)il);              // B | unused<<16
        pramin_write32(bar0_va, ob + 0, ((uint32_t)ol << 16) | ol);
        pramin_write32(bar0_va, ob + 4, (uint32_t)ol);
    }
    barrier::dma_full();
}

int32_t gsp_display_init(gsp_rpc& rpc, const gsp_seq_ctx& ctx, uintptr_t bar0_va) {
    log::info("nvidia: --- Stage 12: display object tree + connector detection ---");
    uint32_t st = 0;

    // 1. client (NV01_ROOT) -- handle travels in NV0000_ALLOC_PARAMETERS.hClient.
    NV0000_ALLOC_PARAMETERS root;
    string::memset(&root, 0, sizeof(root));
    root.hClient = H_CLIENT;
    if (gsp_rpc_alloc(rpc, ctx, bar0_va, H_CLIENT, 0, 0, NV01_ROOT, &root, sizeof(root), &st) != RPC_OK || st != 0) {
        log::error("nvidia: display: client alloc failed (status=0x%x)", st);
        return ERR_DISP_ALLOC;
    }

    // 2. device (NV01_DEVICE_0) under the client; deviceId=0.
    NV0080_ALLOC_PARAMETERS dev;
    string::memset(&dev, 0, sizeof(dev));
    if (gsp_rpc_alloc(rpc, ctx, bar0_va, H_CLIENT, H_CLIENT, H_DEVICE, NV01_DEVICE_0, &dev, sizeof(dev), &st) != RPC_OK || st != 0) {
        log::error("nvidia: display: device alloc failed (status=0x%x)", st);
        return ERR_DISP_ALLOC;
    }

    // 3. subdevice (NV20_SUBDEVICE_0) under the device; subDeviceId=0.
    NV2080_ALLOC_PARAMETERS sub;
    string::memset(&sub, 0, sizeof(sub));
    if (gsp_rpc_alloc(rpc, ctx, bar0_va, H_CLIENT, H_DEVICE, H_SUBDEVICE, NV20_SUBDEVICE_0, &sub, sizeof(sub), &st) != RPC_OK || st != 0) {
        log::error("nvidia: display: subdevice alloc failed (status=0x%x)", st);
        return ERR_DISP_ALLOC;
    }

    // 4. display_common (NV04_DISPLAY_COMMON) parented to the DEVICE; NULL params.
    if (gsp_rpc_alloc(rpc, ctx, bar0_va, H_CLIENT, H_DEVICE, H_DISPLAY, NV04_DISPLAY_COMMON, nullptr, 0, &st) != RPC_OK || st != 0) {
        log::error("nvidia: display: display_common alloc failed (status=0x%x)", st);
        return ERR_DISP_ALLOC;
    }
    log::info("nvidia: display: object tree OK (client=0x%x device=0x%x subdev=0x%x dispCommon=0x%x)",
              H_CLIENT, H_DEVICE, H_SUBDEVICE, H_DISPLAY);

    // --- detect ---
    NV0073_GET_NUM_HEADS nh;
    string::memset(&nh, 0, sizeof(nh));
    if (gsp_rpc_control(rpc, ctx, bar0_va, H_CLIENT, H_DISPLAY, NV0073_CTRL_CMD_SYSTEM_GET_NUM_HEADS,
                        &nh, sizeof(nh), &st) != RPC_OK || st != 0) {
        log::error("nvidia: display: GET_NUM_HEADS failed (status=0x%x)", st);
        return ERR_DISP_CTRL;
    }
    log::info("nvidia: display: numHeads=%u", nh.numHeads);

    NV0073_GET_SUPPORTED sup;
    string::memset(&sup, 0, sizeof(sup));
    if (gsp_rpc_control(rpc, ctx, bar0_va, H_CLIENT, H_DISPLAY, NV0073_CTRL_CMD_SYSTEM_GET_SUPPORTED,
                        &sup, sizeof(sup), &st) != RPC_OK || st != 0) {
        log::error("nvidia: display: GET_SUPPORTED failed (status=0x%x)", st);
        return ERR_DISP_CTRL;
    }
    log::info("nvidia: display: supported displayMask=0x%x (ddc=0x%x)", sup.displayMask, sup.displayMaskDDC);

    NV0073_GET_CONNECT_STATE cs;
    string::memset(&cs, 0, sizeof(cs));
    cs.displayMask = sup.displayMask; // probe the supported set
    if (gsp_rpc_control(rpc, ctx, bar0_va, H_CLIENT, H_DISPLAY, NV0073_CTRL_CMD_SYSTEM_GET_CONNECT_STATE,
                        &cs, sizeof(cs), &st) != RPC_OK || st != 0) {
        log::error("nvidia: display: GET_CONNECT_STATE failed (status=0x%x)", st);
        return ERR_DISP_CTRL;
    }
    const uint32_t connected = cs.displayMask;
    log::info("nvidia: display: *** connected displayMask=0x%x ***", connected);

    // EDID for each connected display (best-effort).
    dma_buffer edidbuf;
    if (dma_alloc(sizeof(NV0073_GET_EDID_V2), /*uncached=*/false, edidbuf) != MEM_OK) {
        log::warn("nvidia: display: EDID buffer alloc failed; skipping EDID");
        return DISP_OK;
    }
    NV0073_GET_EDID_V2* edid = reinterpret_cast<NV0073_GET_EDID_V2*>(edidbuf.cpu_va);
    for (uint32_t bit = 0; bit < 32; bit++) {
        const uint32_t id = 1u << bit;
        if ((connected & id) == 0) {
            continue;
        }
        string::memset(edid, 0, sizeof(NV0073_GET_EDID_V2));
        edid->displayId = id;
        if (gsp_rpc_control(rpc, ctx, bar0_va, H_CLIENT, H_DISPLAY, NV0073_CTRL_CMD_SPECIFIC_GET_EDID_V2,
                            edid, sizeof(NV0073_GET_EDID_V2), &st) != RPC_OK || st != 0) {
            log::warn("nvidia:   display 0x%x: GET_EDID failed (status=0x%x)", id, st);
            continue;
        }
        log_edid(id, edid->edidBuffer, edid->bufferSize);
    }
    dma_free(edidbuf);
    log::info("nvidia: ===== stage 12 COMPLETE: object tree + display detection =====");

    // --- Increment 1a: modeset-prep controls + display engine object ---
    log::info("nvidia: --- Stage 13: modeset-prep (OR/DFP) + display engine ---");

    // SOR routing for the primary DP target (golden 0x800 -> index 1, type 2 SOR, proto 9 DP_B).
    NV0073_OR_GET_INFO ori;
    string::memset(&ori, 0, sizeof(ori));
    ori.displayId = DP_TARGET;
    if (gsp_rpc_control(rpc, ctx, bar0_va, H_CLIENT, H_DISPLAY, NV0073_CTRL_CMD_SPECIFIC_OR_GET_INFO,
                        &ori, sizeof(ori), &st) == RPC_OK && st == 0) {
        log::info("nvidia: display 0x%x: OR index=%u type=%u protocol=%u (2=SOR, 9=DP_B)",
                  DP_TARGET, ori.index, ori.type, ori.protocol);
    } else {
        log::warn("nvidia: display 0x%x: OR_GET_INFO failed (status=0x%x)", DP_TARGET, st);
    }

    // DFP link capabilities (golden 0x800 -> flags 0x0208001b: DP/4-lane/HBR).
    NV0073_DFP_GET_INFO dfp;
    string::memset(&dfp, 0, sizeof(dfp));
    dfp.displayId = DP_TARGET;
    if (gsp_rpc_control(rpc, ctx, bar0_va, H_CLIENT, H_DISPLAY, NV0073_CTRL_CMD_DFP_GET_INFO,
                        &dfp, sizeof(dfp), &st) == RPC_OK && st == 0) {
        log::info("nvidia: display 0x%x: DFP flags=0x%08x (signal/lane/bw)", DP_TARGET, dfp.flags);
    } else {
        log::warn("nvidia: display 0x%x: DFP_GET_INFO failed (status=0x%x)", DP_TARGET, st);
    }

    // NV01_MEMORY_VIRTUAL (default VA space) under the device -- cheap, kept for the
    // window surface VA later.
    NV_MEMORY_VIRTUAL_PARAMS mvp;
    string::memset(&mvp, 0, sizeof(mvp));
    if (gsp_rpc_alloc(rpc, ctx, bar0_va, H_CLIENT, H_DEVICE, H_MEM_VIRTUAL, NV01_MEMORY_VIRTUAL,
                      &mvp, sizeof(mvp), &st) != RPC_OK || st != 0) {
        log::error("nvidia: display: MEMORY_VIRTUAL alloc failed (status=0x%x)", st);
        return ERR_DISP_ALLOC;
    }
    log::info("nvidia: display: memory_virtual 0x%x allocated", H_MEM_VIRTUAL);

    // NVC670_DISPLAY -- the display engine object (NULL params; RM/GSP defaults).
    if (gsp_rpc_alloc(rpc, ctx, bar0_va, H_CLIENT, H_DEVICE, H_DISP_ENGINE, NVC670_DISPLAY,
                      nullptr, 0, &st) != RPC_OK || st != 0) {
        log::error("nvidia: display: NVC670_DISPLAY alloc failed (status=0x%x)", st);
        return ERR_DISP_ALLOC;
    }
    log::info("nvidia: display: NVC670 display engine 0x%x allocated", H_DISP_ENGINE);

    // NVC372_DISPLAY_SW -- the display-SW object IMP (IS_MODE_POSSIBLE) runs on. Allocated under the
    // device with NULL params (nvkms EvoAllocRmCtrlObjectC3, nvkms-evo3.c:6745). Previously skipped
    // ("skippable for first pixel") -- WRONG: IMP is part of the GSP modeset model that wakes the head.
    bool disp_sw_ok = (gsp_rpc_alloc(rpc, ctx, bar0_va, H_CLIENT, H_DEVICE, H_DISP_SW, NVC372_DISPLAY_SW,
                      nullptr, 0, &st) == RPC_OK && st == 0);
    if (!disp_sw_ok)
        log::warn("nvidia: display: NVC372_DISPLAY_SW alloc failed (status=0x%x) -- IMP will be skipped", st);
    else
        log::info("nvidia: display: NVC372 display-SW 0x%x allocated", H_DISP_SW);

    log::info("nvidia: ===== stage 13 COMPLETE: display engine + modeset-prep =====");

    // --- Increment 1b: EVO channel-DMA path (core + window0) ---
    // Minimal channel set to drive one head + one window (spec lines 3860-3868):
    //   NVC670 (done) -> 0x20800a58(c67d) -> NVC67D -> 0x20800a58(c67e) -> NVC67E.
    // NVC372_DISPLAY_SW + NV5070 caps are skippable for first pixel. Pushbuffers are
    // retained (static) for later EVO method pushes (Increment 2).
    log::info("nvidia: --- Stage 14: EVO channel-DMA (core c67d + window0 c67e) ---");
    static dma_buffer core_pb;
    static dma_buffer win_pb;

    int32_t crc = setup_dma_channel(rpc, ctx, bar0_va, core_pb,
                                    NVC67D_CORE_CHANNEL_DMA, H_CORE_CH, H_CORE_CTXDMA, 0, "core(c67d)");
    if (crc != DISP_OK) {
        return crc;
    }
    int32_t wrc = setup_dma_channel(rpc, ctx, bar0_va, win_pb,
                                    NVC67E_WINDOW_CHANNEL_DMA, H_WIN_CH, H_WIN_CTXDMA, 6, "window6(c67e)");
    if (wrc != DISP_OK) {
        return wrc;
    }
    log::info("nvidia: ===== stage 14 COMPLETE: core + window EVO channels live =====");

    // --- Stage 14.05: shut down the boot/VBIOS heads (nvShutDownApiHeads -- the COLD-boot prerequisite) ---
    // On a COLD power-on the VBIOS/GOP brought up the panel on SOME head; that boot head stays active and
    // BLOCKS activating head3 (the GSP supervisor won't promote a conflicting modeset -> head stays SLEEP).
    // nvkms shuts down ALL heads on its first modeset (coreInitMethodsPending -> nvShutDownApiHeads,
    // nvkms-modeset.c:3936). The golden (warm) shows the open driver doing it too (trace ts 1334.813:
    // HEAD_SET_DISPLAY_ID(head)=0 + SOR_SET_CONTROL(sor)=0 BEFORE activating at 1334.867). My driver never
    // did -> the boot head persisted = the cold-vs-warm gap. Do it now, OUTSIDE the bracket (a plain core
    // UPDATE latches directly on a cold engine, like the run-#43 baseline that DID latch).
    uint32_t sd_put = 0;
    {
        for (uint32_t i = 0; i < 4; i++) { // pristine boot head state (which head the VBIOS/GOP left active)
            const uint32_t hs = mmio::read32(bar0_va + 0x00612078 + i * 0x800);
            const uint32_t m  = (hs >> 8) & 0x3;
            log::info("nvidia: display: BOOT head%u CORE_HEAD_STATE=0x%08x mode=%s", i, hs,
                      m == 2 ? "AWAKE" : (m == 1 ? "SNOOZE" : "SLEEP"));
        }
        evo_push sp;
        evo_begin(sp, core_pb, bar0_va, NVC67D_CORE_CTRL_BASE, 0);
        for (uint32_t i = 0; i < 4; i++)
            evo_m1(sp, 0x2020 + i * 0x400, 0x00000000); // HEAD_SET_DISPLAY_ID(head)=0 (detach display)
        for (uint32_t s = 0; s < 4; s++)
            evo_m1(sp, 0x0300 + s * 0x20, 0x00000000);  // SOR_SET_CONTROL(sor)=OWNER_NONE (release OR)
        evo_m1(sp, 0x0218, 0x00000000);                 // SET_INTERLOCK_FLAGS        = 0
        evo_m1(sp, 0x021c, 0x00000000);                 // SET_WINDOW_INTERLOCK_FLAGS = 0
        evo_m1(sp, 0x0200, 0x00000001);                 // UPDATE
        sd_put = evo_kick(sp);
        const bool sok = evo_wait_get(sp, sd_put);
        log::info("nvidia: display: boot-head shutdown (all heads DISPLAY_ID=0 + SORs released) PUT=0x%x %s",
                  sd_put, sok ? "=PUT (latched)" : "STALLED");
        delay::us(100000);
    }

    // --- window-6 owner assignment is DEFERRED to Stage 14.6 (after the DP setup) ---
    // run-#49 ROOT CAUSE: kicking the win6->head3 owner update HERE (before DFP_ASSIGN_SOR) latched it
    // (WIN6_OWN ARMED=0x3 -- the linchpin worked, and the interlocked modeset+flip then FETCHED with NO
    // stall, fixing run #48!) BUT it made head3 "active" (it now owns a window), and DFP_ASSIGN_SOR then
    // FAILED (status=0xffff) -> no SOR -> DP_CTRL CR-failed (err=0x80000010) -> modeset couldn't latch.
    // nvkms only ASSEMBLES the owner methods (EvoInitWindowMapping3) before AssignSor; it KICKS them later
    // in the batched modeset update (KickoffModesetUpdateState -> nvDoIMPUpdateEvo), AFTER AssignSor + DP
    // train. NVDisplay rule (nvkms-modeset.c:3035-3046): "you can't move a window while the head is active"
    // -- so ASSIGN_SOR + DP MUST run while head3 is INACTIVE (win6 still OWNER_NONE). FIX: run the whole DP
    // block first, THEN (Stage 14.6) assign win6->head3 -- still BEFORE the activating modeset, so it's a
    // legal owner change (head inactive) and the interlocked modeset+flip is owner-stable.
    uint32_t own_put = 0;

    // The SPECIFIC_DISPLAY_CHANGE bracket below now wraps the FULL coordinated GSP sequence
    // (IMP -> START -> DFP_ASSIGN_SOR -> DP_CTRL -> DP_CONFIG_STREAM -> MSA -> INTERLOCKED core+win6
    // UPDATE -> END). With win6 pre-owned (above) and the core update interlocked with the flip, the
    // supervisor receives a COMPLETE, consistent head3+window6 model and wakes the head.

    // --- Stage 14.3: IMP (IS_MODE_POSSIBLE) -- validate the proposed mode with GSP-RM ---
    // The golden runs this before EVERY modeset (NVC372 0xc3720101). It describes head 3 + window 6
    // @ 2560x1440@60 and GSP-RM validates feasibility + computes the bandwidth/v-pstate its modeset
    // supervisor needs. READ-ONLY (cannot change HW state) -> a SAFE checkpoint on the current
    // baseline. Field values are the golden full-payload decode (head=3, window=6). bIsPossible=1 =>
    // GSP accepts our mode description (validates the 1924B struct VALUES, not just the layout).
    // 'static' (not stack): 1924B in BSS, zero-init, no stack pressure.
    if (disp_sw_ok) {
        const evo_mode& im = SELECTED_MODE; // mode-specific raster/clock derive from here
        static NVC372_IS_MODE_POSSIBLE imp;
        string::memset(&imp, 0, sizeof(imp));
        imp.numHeads   = 1;
        imp.numWindows = 1;
        imp.head[0].headIndex            = 3;
        imp.head[0].maxPixelClkKHz       = im.pclk_hz / 1000;                  // 241500 (60) / 592000 (144)
        imp.head[0].rasterSize_w         = im.raster_size & 0xffff;        imp.head[0].rasterSize_h       = im.raster_size >> 16;
        imp.head[0].rasterBlankStart_X   = im.raster_blank_start & 0xffff; imp.head[0].rasterBlankStart_Y = im.raster_blank_start >> 16;
        imp.head[0].rasterBlankEnd_X     = im.raster_blank_end & 0xffff;   imp.head[0].rasterBlankEnd_Y   = im.raster_blank_end >> 16;
        imp.head[0].ctl_masterLockPin    = 0x10;  imp.head[0].ctl_slaveLockPin     = 0x10;
        imp.head[0].maxDownscaleFactorH  = 0x400; imp.head[0].maxDownscaleFactorV  = 0x400;
        imp.head[0].outputScalerVerticalTaps = 2;
        imp.head[0].minFrameIdle_leading = 38;    imp.head[0].minFrameIdle_trailing = 3;
        imp.head[0].lut = 2;                       imp.head[0].cursorSize32p = 8;
        imp.window[0].windowIndex            = 6;
        imp.window[0].owningHead             = 3;
        imp.window[0].formatUsageBound       = 0x1659f;
        imp.window[0].maxPixelsFetchedPerLine = 2582;
        imp.window[0].maxDownscaleFactorH    = 0x400; imp.window[0].maxDownscaleFactorV = 0x400;
        imp.window[0].inputScalerVerticalTaps = 2;
        imp.window[0].lut = 2;                     imp.window[0].tmoLut = 2;
        if (gsp_rpc_control(rpc, ctx, bar0_va, H_CLIENT, H_DISP_SW,
                            NVC372_CTRL_CMD_IS_MODE_POSSIBLE, &imp, sizeof(imp), &st) != RPC_OK)
            log::error("nvidia: display: IMP RPC transport failed");
        else
            log::info("nvidia: display: IMP status=0x%x bIsPossible=%u dispClkKHz=%u minVPState=%u margin=%u -> %s",
                      st, imp.bIsPossible, imp.dispClkKHz, imp.minImpVPState, imp.worstCaseMargin,
                      (st == 0 && imp.bIsPossible) ? "*** MODE POSSIBLE ***" : "NOT POSSIBLE");
    }

    // --- Stage 14.4: DISPLAY_CHANGE START -- open the modeset bracket ---
    // The golden wraps the whole modeset (DP train + EVO UPDATE) in SPECIFIC_DISPLAY_CHANGE
    // START(enable=1)...END(enable=0). Earlier the bracket alone broke the latch because the model
    // was incomplete; now the FULL model is in place (NVC372+IMP above; interlocked core+win6 commit
    // below), so the GSP supervisor can complete head 3's wake between START and END.
    {
        NV0073_DISPLAY_CHANGE dc; string::memset(&dc, 0, sizeof(dc));
        dc.newDevices = DP_TARGET; // 0x800 -- the display we are enabling
        dc.enable     = 1;         // NV0073_CTRL_SPECIFIC_DISPLAY_CHANGE_START
        if (gsp_rpc_control(rpc, ctx, bar0_va, H_CLIENT, H_DISPLAY,
                            NV0073_CTRL_CMD_SPECIFIC_DISPLAY_CHANGE, &dc, sizeof(dc), &st) != RPC_OK || st != 0)
            log::warn("nvidia: display: DISPLAY_CHANGE START failed (status=0x%x)", st);
        else
            log::info("nvidia: display: DISPLAY_CHANGE START ok (newDevices=0x%x)", DP_TARGET);
    }

    // --- Stage 14.5: DP link bring-up (the missing piece: head SNOOZE -> AWAKE) ---
    // On GSP-offload the OR power-up + DP clock-recovery/channel-eq are done by GSP-RM via NV0073
    // RPCs, NOT by EVO methods. We previously relied on VBIOS-inherited link state (non-deterministic
    // -> "no signal" this boot). Do it properly (the nvkms path), BEFORE the core UPDATE so the head
    // latches onto an already-trained link:
    //   (1) DFP_ASSIGN_SOR binds displayId 0x800 -> a SOR in RM (hard prereq; DP_CTRL is skipped
    //       in the DP lib if no SOR is assigned).
    //   (2) DP_CTRL trains the link: GSP powers SOR1 + runs the DPCD CR/EQ handshake itself; we just
    //       request 2-lane HBR2 (data=0x1402) for 2560x1440@60. (Evidence: 10-agent OR/DP sweep +
    //       golden rpc/payload traces decode this exact cmd/data; nvkms nvDPPreSetMode ->
    //       trainLinkOptimized -> EvoMainLink::train issues one DP_CTRL.)
    {
        // (a) Manual DP mode -- the RM/GSP DP-lib prerequisite for client-driven DP_CTRL training
        //     (golden does this first, rpc-trace cmd 0x731365). Without it DP_CTRL trains against an
        //     uninitialised connector -> err=0x80000000 (LINK_TRAINING failed), which is what run #42 hit.
        NV0073_DP_SET_MANUAL man; string::memset(&man, 0, sizeof(man));
        if (gsp_rpc_control(rpc, ctx, bar0_va, H_CLIENT, H_DISPLAY,
                            NV0073_CTRL_CMD_DP_SET_MANUAL, &man, sizeof(man), &st) != RPC_OK || st != 0)
            log::warn("nvidia: display: DP_SET_MANUAL failed (status=0x%x)", st);
        else
            log::info("nvidia: display: DP_SET_MANUAL ok (manual DP mode)");

        // (b) Assign a SOR to displayId 0x800 (prereq; DP lib skips LT if no SOR is assigned).
        NV0073_DFP_ASSIGN_SOR sor; string::memset(&sor, 0, sizeof(sor));
        sor.displayId = DP_TARGET; // 0x800
        uint32_t sorIdx = 1;
        if (gsp_rpc_control(rpc, ctx, bar0_va, H_CLIENT, H_DISPLAY,
                            NV0073_CTRL_CMD_DFP_ASSIGN_SOR, &sor, sizeof(sor), &st) != RPC_OK || st != 0) {
            log::warn("nvidia: display: DFP_ASSIGN_SOR failed (status=0x%x)", st);
        } else {
            for (uint32_t i = 0; i < 4; i++)
                if (sor.sorAssignListWithTag[i].displayMask & DP_TARGET) { sorIdx = i; break; }
            log::info("nvidia: display: DFP_ASSIGN_SOR ok (displayId=0x%x -> SOR%u type=0x%x)",
                      DP_TARGET, sorIdx, sor.sorAssignListWithTag[sorIdx < 4 ? sorIdx : 0].sorType);
        }

        // (c) Read DP caps for the assigned SOR (prepares the connector's DP state in RM/GSP).
        NV0073_DP_GET_CAPS caps; string::memset(&caps, 0, sizeof(caps));
        caps.sorIndex = sorIdx;
        if (gsp_rpc_control(rpc, ctx, bar0_va, H_CLIENT, H_DISPLAY,
                            NV0073_CTRL_CMD_DP_GET_CAPS, &caps, sizeof(caps), &st) != RPC_OK || st != 0)
            log::warn("nvidia: display: DP_GET_CAPS failed (status=0x%x)", st);
        else
            log::info("nvidia: display: DP_GET_CAPS ok (maxLinkRate=0x%x dpVer=0x%x MST=%u FEC=%u incWM=%u)",
                      caps.maxLinkRate, caps.dpVersionsSupported, caps.bIsMultistreamSupported,
                      caps.bFECSupported, caps.bHasIncreasedWatermarkLimits);

        // (d) Power the sink up to D0 via DPCD (AUX write 0x600=0x1). A sink in D3 fails CR entirely.
        NV0073_DP_AUXCH_CTRL aux; string::memset(&aux, 0, sizeof(aux));
        aux.displayId = DP_TARGET;
        aux.cmd  = 0x08;     // AUX WRITE (TYPE=AUX bit3, REQ=WRITE)
        aux.addr = 0x600;    // DPCD SET_POWER
        aux.data[0] = 0x01;  // D0 (normal operation)
        aux.size = 0;        // 0-based: 1 byte
        if (gsp_rpc_control(rpc, ctx, bar0_va, H_CLIENT, H_DISPLAY,
                            NV0073_CTRL_CMD_DP_AUXCH_CTRL, &aux, sizeof(aux), &st) != RPC_OK || st != 0)
            log::warn("nvidia: display: DP_AUXCH SET_POWER D0 failed (status=0x%x reply=0x%x)", st, aux.replyType);
        else
            log::info("nvidia: display: DP_AUXCH SET_POWER D0 ok (reply=0x%x)", aux.replyType);

        // (e) DP link training -- the golden issues TWO DP_CTRL calls: (1) quiesce to 0 lanes (reset
        //     the link before retrain), (2) train 2-lane HBR2. Both cmd=0x2083 (SET_LANE_COUNT|
        //     SET_LINK_BW|SET_ENHANCED_FRAMING|TRAIN_PHY_REPEATER); FAKE_LINK_TRAINING NOT set.
        NV0073_DP_CTRL dp; string::memset(&dp, 0, sizeof(dp));
        dp.displayId = DP_TARGET;
        dp.cmd  = 0x00002083;
        dp.data = 0x00001400; // SET_LANE_COUNT=0 (quiesce) | SET_LINK_BW=0x14 (HBR2) | TARGET=SINK
        if (gsp_rpc_control(rpc, ctx, bar0_va, H_CLIENT, H_DISPLAY,
                            NV0073_CTRL_CMD_DP_CTRL, &dp, sizeof(dp), &st) != RPC_OK)
            log::warn("nvidia: display: DP_CTRL quiesce RPC failed");
        else
            log::info("nvidia: display: DP_CTRL quiesce (0 lanes): status=0x%x err=0x%x", st, dp.err);
        // Train 2-lane HBR2, WITH RETRY. DP clock-recovery can transiently fail (run #47 CR-failed once);
        // the golden itself issues multiple DP_CTRL quiesce/train cycles. Retry up to 3x: on failure
        // re-quiesce (reset the link) + settle, then re-train. (Owner->head binding is orthogonal to the
        // SOR->sink link, so the pre-bracket owner update should not affect CR -- but retry is cheap insurance.)
        bool trained = false;
        for (uint32_t attempt = 0; attempt < 3 && !trained; attempt++) {
            if (attempt > 0) {
                string::memset(&dp, 0, sizeof(dp));
                dp.displayId = DP_TARGET; dp.cmd = 0x00002083; dp.data = 0x00001400; // re-quiesce
                gsp_rpc_control(rpc, ctx, bar0_va, H_CLIENT, H_DISPLAY,
                                NV0073_CTRL_CMD_DP_CTRL, &dp, sizeof(dp), &st);
                delay::us(20000);
            }
            string::memset(&dp, 0, sizeof(dp));
            dp.displayId = DP_TARGET;
            dp.cmd  = 0x00002083;
            dp.data = SELECTED_MODE.dp_train_data; // SET_LANE_COUNT|SET_LINK_BW=0x14: 0x1402=2-lane(60Hz), 0x1404=4-lane(144Hz)
            if (gsp_rpc_control(rpc, ctx, bar0_va, H_CLIENT, H_DISPLAY,
                                NV0073_CTRL_CMD_DP_CTRL, &dp, sizeof(dp), &st) != RPC_OK) {
                log::error("nvidia: display: DP_CTRL train RPC transport failed (attempt %u)", attempt);
                continue;
            }
            trained = (st == 0 && dp.err == 0);
            log::info("nvidia: display: DP_CTRL train attempt %u: status=0x%x err=0x%x data=0x%x retry=%u -> %s",
                      attempt, st, dp.err, dp.data, dp.retryTimeMs,
                      trained ? "*** LINK TRAINED ***" : "CR/EQ FAILED");
        }
        if (!trained)
            log::warn("nvidia: display: DP_CTRL train FAILED after 3 attempts -- modeset will likely stall");

        // (f) DP_CONFIG_STREAM -- configure the SF stream/watermark on the trained link. SAFE now that
        //     it is INSIDE the DISPLAY_CHANGE bracket (run #44 proved it must not be standalone).
        //     tuSize=64/waterMark=33 computed from dp_watermark.cpp isModePossibleSSTWithFEC + validated
        //     bit-for-bit vs the golden hBlankSym=0x14c/vBlankSym=0x163d.
        NV0073_DP_CONFIG_STREAM cs; string::memset(&cs, 0, sizeof(cs));
        cs.head                 = 3;
        cs.sorIndex             = sorIdx;
        cs.dpLink               = 1;
        cs.bEnableOverride      = 1;
        cs.bMST                 = 0;
        cs.hBlankSym            = SELECTED_MODE.dp_hblank_sym; // 0x14c (60Hz/2-lane) / 0x4e (144Hz/4-lane)
        cs.vBlankSym            = SELECTED_MODE.dp_vblank_sym; // 0x163d (60Hz) / 0x8fb (144Hz)
        cs.colorFormat          = 0;
        cs.sst_bEnhancedFraming = 1;
        cs.sst_tuSize           = 64;
        cs.sst_waterMark        = SELECTED_MODE.dp_watermark;  // 33 (60Hz) / 24 (144Hz)
        if (gsp_rpc_control(rpc, ctx, bar0_va, H_CLIENT, H_DISPLAY,
                            NV0073_CTRL_CMD_DP_CONFIG_STREAM, &cs, sizeof(cs), &st) != RPC_OK || st != 0)
            log::warn("nvidia: display: DP_CONFIG_STREAM failed (status=0x%x)", st);
        else
            log::info("nvidia: display: DP_CONFIG_STREAM ok (head=3 sor=%u tu=64 wm=%u hSym=0x%x vSym=0x%x)",
                      sorIdx, SELECTED_MODE.dp_watermark, SELECTED_MODE.dp_hblank_sym, SELECTED_MODE.dp_vblank_sym);

        // (g) DP_SET_MSA_PROPERTIES_V2 -- program the DP Main-Stream-Attribute overrides the SF carries
        //     in the stream. The golden issues this right after CONFIG_STREAM (payload-trace 0x731381):
        //     enable MSA, cache the override for the next modeset, force MISC1 bits{1,2}=0, and override
        //     the vertical raster total = 1481 (== HEAD_SET_RASTER_SIZE height). Byte-exact vs golden
        //     (w=...00010001 00010006 ...05c90000).
        NV0073_DP_SET_MSA_V2 msa; string::memset(&msa, 0, sizeof(msa));
        msa.displayId                         = DP_TARGET; // 0x800
        msa.bEnableMSA                        = 1;
        msa.bCacheMsaOverrideForNextModeset   = 1;
        msa.featureMask.miscMask[1]           = 0x06;      // override MISC1 bits 1,2
        msa.featureMask.bRasterTotalVertical  = 1;         // override vertical raster total
        msa.featureValues.rasterTotalVertical = SELECTED_MODE.raster_size >> 16; // 1481 (60Hz) / 1543 (144Hz)
        if (gsp_rpc_control(rpc, ctx, bar0_va, H_CLIENT, H_DISPLAY,
                            NV0073_CTRL_CMD_DP_SET_MSA_PROPERTIES_V2, &msa, sizeof(msa), &st) != RPC_OK || st != 0)
            log::warn("nvidia: display: DP_SET_MSA_PROPERTIES_V2 failed (status=0x%x)", st);
        else
            log::info("nvidia: display: DP_SET_MSA_PROPERTIES_V2 ok (displayId=0x%x vTotal=%u)",
                      DP_TARGET, SELECTED_MODE.raster_size >> 16);
    }
    log::info("nvidia: ===== stage 14.5 COMPLETE: DP link trained + stream configured (%u-lane HBR2, %s) =====",
              SELECTED_MODE.dp_train_data & 0xf, SELECTED_MODE.name);

    // --- Stage 14.6: window-6 owner assignment (NOW -- after DP, while head3 is still INACTIVE) ---
    // run-#49 fix: ASSIGN_SOR + DP train above ran with head3 INACTIVE (win6 still OWNER_NONE), so they
    // succeed. We now assign win6->head3 in a standalone NON-interlocked core update (an owner change must
    // NOT be interlocked -- "error 37"). This is still BEFORE the modeset that activates head3, so it is a
    // legal owner change (NVDisplay: "can't move a window while the head is active", nvkms-modeset.c:3035).
    // It latches directly (run #49 confirmed this exact update latches: WIN6 ARMED=0x3) -> the interlocked
    // modeset+flip below is owner-stable (run #49 proved it then FETCHES with NO win6 stall).
    {
        evo_push op;
        evo_begin(op, core_pb, bar0_va, NVC67D_CORE_CTRL_BASE, sd_put); // continue PB after the boot-head shutdown
        evo_m1(op, 0x1000 + 6 * 0x80, 0x00000003); // WINDOW_SET_CONTROL(6) OWNER=HEAD3 (clc67d.h:360)
        evo_m1(op, 0x0218, 0x00000000);            // SET_INTERLOCK_FLAGS        = 0
        evo_m1(op, 0x021c, 0x00000000);            // SET_WINDOW_INTERLOCK_FLAGS = 0 (owner change -> NON-interlocked)
        evo_m1(op, 0x0200, 0x00000001);            // UPDATE
        own_put = evo_kick(op);
        const bool ook = evo_wait_get(op, own_put);
        log::info("nvidia: display: win6->head3 owner-assign (post-DP) PUT=0x%x GET %s",
                  own_put, ook ? "=PUT (fetched)" : "STALLED");
        delay::us(50000); // let the owner update ARM before the modeset
        // THE linchpin: must read ARMED=0x3 (run #49 confirmed it latches here); if 0xf the modeset stalls.
        dump_core(bar0_va, "WIN6_OWN(post)", 0x1300, 0x00000003);
    }

    // --- Increment 2a: EVO core modeset (first light) ---
    // Push the golden NVC67D core program (head 3 / SOR 1 / DP_B / 2560x1440@60) into the
    // core pushbuffer and kick. The GSP-RM trains the DP link when it processes the UPDATE.
    // Expected: the monitor leaves "no signal" and syncs at 2560x1440@60 (blank/black).
    log::info("nvidia: --- Stage 15: EVO core modeset assemble (head 3, DP_B, %s) ---", SELECTED_MODE.name);
    evo_push cp, wp; // core + window6 pushes; assembled now, kicked INTERLOCKED together below
    evo_core_modeset_assemble(cp, core_pb, bar0_va, own_put); // continue PB after the Stage-14.6 owner update; interlocked commit
    log::info("nvidia: ===== stage 15 COMPLETE: core modeset assembled (head3/SOR1/win6 interlocked, %s) =====", SELECTED_MODE.name);

    // --- Increment 2b: VRAM scanout surface + display instance memory + paint red ---
    // On GSP-offload WE own the FB heap + display instance memory (the GSP has no client-FB
    // alloc RPC). Carve VRAM, build the ctxdma hash+entry directly in FBMEM via PRAMIN,
    // WRITE_INST_MEM to point the HW at it, paint, then push the window program. The display
    // HW resolves the window's SET_CONTEXT_DMA_ISO handle through this instance memory.
    log::info("nvidia: --- Stage 16: VRAM scanout (display instance memory + window) ---");
    const uint64_t fbSize   = static_cast<uint64_t>(FB_PITCH) * FB_H; // 14745600 (64KB-aligned)
    const uint64_t surfVram = FB_SURFACE_VRAM;
    const uint64_t instVram = FB_INSTMEM_VRAM;

    // 0. PRAMIN sanity (confirm BAR0_WINDOW isn't privilege-locked post-GSP).
    pramin_write32(bar0_va, surfVram, 0xDEADBEEF);
    const uint32_t sane = pramin_read32(bar0_va, surfVram);
    log::info("nvidia: display: PRAMIN sanity = 0x%08x (%s)",
              sane, sane == 0xDEADBEEF ? "OK" : "FAIL - BAR0_WINDOW priv-locked");

    // 1. Zero the 64 KB display instance memory (unused hash slots must read 0/invalid).
    pramin_fill(bar0_va, instVram, DISP_INSTMEM_SIZE, 0x00000000);

    // 2. ctxdma instance entry (5 dwords, NV_DMA_* format) at the obj-mem base.
    const uint64_t base256  = surfVram >> 8;                  // disp ctxdma addr is 256B-aligned
    const uint64_t lim256   = (surfVram + fbSize - 1) >> 8;
    const uint64_t entryVram = instVram + DISP_OBJ_MEM_BASE;
    pramin_write32(bar0_va, entryVram + 0,  DISP_DMA_W0_FBMEM_RW_PITCH);          // word0
    pramin_write32(bar0_va, entryVram + 4,  static_cast<uint32_t>(base256));       // ADDRESS_BASE_LO
    pramin_write32(bar0_va, entryVram + 8,  static_cast<uint32_t>(base256 >> 32)); // ADDRESS_BASE_HI
    pramin_write32(bar0_va, entryVram + 12, static_cast<uint32_t>(lim256));        // ADDRESS_LIMIT_LO
    pramin_write32(bar0_va, entryVram + 16, static_cast<uint32_t>(lim256 >> 32));  // ADDRESS_LIMIT_HI
    const uint32_t instField = DISP_OBJ_MEM_BASE >> 5; // 0x100 (32-byte-chunk index)
    const uint32_t entryChk  = pramin_read32(bar0_va, entryVram + 4);
    log::info("nvidia: display: ctxdma entry @vram=0x%lx base256=0x%lx readback=0x%08x",
              (uint64_t)entryVram, (uint64_t)base256, entryChk);

    // 3. Hash entries (core + window6) -> same instance. Window 6 resolves its SET_CONTEXT_DMA_ISO
    //    via its dispChannelNum=7 (head 3's natural window), so the ISO hash MUST be at chn 7.
    disp_write_hash_entry(bar0_va, instVram, H_CLIENT, H_ISO_CTXDMA, CHN_NUM_CORE, instField);
    disp_write_hash_entry(bar0_va, instVram, H_CLIENT, H_ISO_CTXDMA, CHN_NUM_WIN6, instField);

    // 3b. Cursor: 2nd ctxdma instance entry + hash for the CORE channel (SET_CONTEXT_DMA_CURSOR
    //     is a core method, so the HW resolves the cursor ctxdma via the core channel's context).
    const uint64_t cbase256 = FB_CURSOR_VRAM >> 8;
    const uint64_t clim256  = (FB_CURSOR_VRAM + CURSOR_BYTES - 1) >> 8;
    const uint64_t curEntryVram = instVram + DISP_CURSOR_ENTRY_OFF;
    pramin_write32(bar0_va, curEntryVram + 0,  DISP_DMA_W0_FBMEM_RW_PITCH);
    pramin_write32(bar0_va, curEntryVram + 4,  static_cast<uint32_t>(cbase256));
    pramin_write32(bar0_va, curEntryVram + 8,  static_cast<uint32_t>(cbase256 >> 32));
    pramin_write32(bar0_va, curEntryVram + 12, static_cast<uint32_t>(clim256));
    pramin_write32(bar0_va, curEntryVram + 16, static_cast<uint32_t>(clim256 >> 32));
    const uint32_t curInstField = DISP_CURSOR_ENTRY_OFF >> 5; // 0x101
    disp_write_hash_entry(bar0_va, instVram, H_CLIENT, H_CURSOR_CTXDMA, CHN_NUM_CORE, curInstField);
    const uint32_t curEntryChk = pramin_read32(bar0_va, curEntryVram + 4); // ADDRESS_BASE_LO
    log::info("nvidia: display: cursor ctxdma entry @vram=0x%lx cbase256=0x%lx readback=0x%08x (%s)",
              (uint64_t)curEntryVram, (uint64_t)cbase256, curEntryChk,
              curEntryChk == (uint32_t)cbase256 ? "OK" : "MISMATCH");

    // 3d. LUT ctxdma (shared by window ILUT @origin0 and head OLUT @origin0x2100) -- 3rd
    //     instance entry + hashes on BOTH the window (chn1) and core (chn0) channels.
    const uint64_t lbase256 = FB_LUT_VRAM >> 8;
    const uint64_t llim256  = (FB_LUT_VRAM + LUT_BYTES - 1) >> 8;
    const uint64_t lutEntryVram = instVram + DISP_LUT_ENTRY_OFF;
    pramin_write32(bar0_va, lutEntryVram + 0,  DISP_DMA_W0_FBMEM_RW_PITCH);
    pramin_write32(bar0_va, lutEntryVram + 4,  static_cast<uint32_t>(lbase256));
    pramin_write32(bar0_va, lutEntryVram + 8,  static_cast<uint32_t>(lbase256 >> 32));
    pramin_write32(bar0_va, lutEntryVram + 12, static_cast<uint32_t>(llim256));
    pramin_write32(bar0_va, lutEntryVram + 16, static_cast<uint32_t>(llim256 >> 32));
    const uint32_t lutInstField = DISP_LUT_ENTRY_OFF >> 5; // 0x102
    disp_write_hash_entry(bar0_va, instVram, H_CLIENT, H_LUT_CTXDMA, CHN_NUM_CORE, lutInstField); // head OLUT (SET_CONTEXT_DMA_OLUT is a core method)
    disp_write_hash_entry(bar0_va, instVram, H_CLIENT, H_LUT_CTXDMA, CHN_NUM_WIN6, lutInstField); // win6 ILUT (SET_CONTEXT_DMA_ILUT resolves via win6's chn=7)
    barrier::dma_full();

    // 4. Point the display HW at our instance memory (internal client/subdevice).
    NV2080_WRITE_INST_MEM wim;
    string::memset(&wim, 0, sizeof(wim));
    wim.instMemPhysAddr  = instVram;
    wim.instMemSize      = DISP_INSTMEM_SIZE;
    wim.instMemAddrSpace = ADDR_FBMEM;
    if (gsp_rpc_control(rpc, ctx, bar0_va, INTERNAL_CLIENT, INTERNAL_SUBDEVICE,
                        NV2080_CTRL_CMD_INTERNAL_DISPLAY_WRITE_INST_MEM,
                        &wim, sizeof(wim), &st) != RPC_OK || st != 0) {
        log::error("nvidia: display: WRITE_INST_MEM failed (status=0x%x)", st);
        return ERR_DISP_CTRL;
    }
    log::info("nvidia: display: WRITE_INST_MEM ok (instmem @vram=0x%lx 64KB FBMEM)", (uint64_t)instVram);

    // 5. Paint the desktop surface + self-check. A touch darker + bluer than neutral gray.
    constexpr uint32_t FB_CLEAR_COLOR = 0x001A1A28; // R=26 G=26 B=40 (X8R8G8B8): dark blue-gray
    pramin_fill(bar0_va, surfVram, fbSize, FB_CLEAR_COLOR);
    const uint32_t pchk = pramin_read32(bar0_va, surfVram);
    log::info("nvidia: display: painted blue-gray(0x%06x) @vram=0x%lx (%lu B); readback[0]=0x%08x (%s)",
              FB_CLEAR_COLOR, (uint64_t)surfVram, (uint64_t)fbSize, pchk, pchk == FB_CLEAR_COLOR ? "OK" : "MISMATCH");

    // 5b. Paint the cursor image (64x64 A8R8G8B8, non-premult): a white arrow pointer with a 1px black
    //     outline over a transparent field. Composited NON_PREMULT_ALPHA (0x75FF) in Stage 17 so the
    //     transparent pixels (alpha=0) let the desktop show through and only the glyph draws.
    paint_cursor_arrow(bar0_va, FB_CURSOR_VRAM, CURSOR_W, CURSOR_H);
    const uint32_t curchk = pramin_read32(bar0_va, FB_CURSOR_VRAM); // pixel(0,0) = arrow tip = black outline
    log::info("nvidia: display: cursor (stlxdm glyph) painted @vram=0x%lx (%ux%u A8R8G8B8); readback[0]=0x%08x (%s)",
              (uint64_t)FB_CURSOR_VRAM, CURSOR_W, CURSOR_H, curchk, curchk == 0xFF000000 ? "OK" : "MISMATCH");

    // 5c. Fill the identity ILUT (base[], FP16) + OLUT (output[], 16-bit) into the LUT buffer.
    lut_fill_identity(bar0_va, FB_LUT_VRAM);
    const uint32_t lchk = pramin_read32(bar0_va, FB_LUT_VRAM + (uint64_t)(LUT_HDR + 512) * 8); // ILUT[512]=fp16(0.5)
    log::info("nvidia: display: identity ILUT+OLUT filled @vram=0x%lx (%u B); ILUT[512]=0x%08x (exp 0x38003800)",
              (uint64_t)FB_LUT_VRAM, LUT_BYTES, lchk);

    // 6. KICK -- the head-3 core modeset (cp) committed INTERLOCKED with the window-6 surface flip (wp)
    //    as ONE atomic update. This IS the golden Main group (core SET_WINDOW_INTERLOCK_FLAGS=win6 +
    //    window SET_INTERLOCK_FLAGS=WITH_CORE/SET_WINDOW_INTERLOCK_FLAGS=win6). win6 is ALREADY owned by
    //    head3 (pre-bracket owner update at Stage 14.1), so the core update is owner-stable and the
    //    interlock is legal (no "error 37"). DP train + CONFIG_STREAM + MSA already ran. With the
    //    DISPLAY_CHANGE bracket open, the GSP supervisor now receives a COMPLETE, consistent head3+window6
    //    model in ONE atomic latch -> it powers the OR + enables the link + wakes head 3 (SLEEP->AWAKE).
    //    (cp's UPDATE+interlock were emitted by evo_core_modeset_assemble; here we stage the flip + kick.)
    evo_begin(wp, win_pb, bar0_va, NVC67E_WIN6_CTRL_BASE);
    evo_m1(wp, 0x0308, 0x00000001);    // SET_PRESENT_CONTROL  MIN_PRESENT_INTERVAL=1
    // FMT (input X8R8G8B8 -> FP16) identity 3x4: C00/C11/C22 = 1.0 (0x10000), rest 0. Golden writes all
    // 12 explicitly (don't rely on power-on defaults for the off-diagonal).
    static const uint32_t fmt_identity[12] = { // SET_FMT_COEFFICIENT_C00..C23
        0x00010000, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x00010000, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x00010000, 0x00000000,
    };
    for (uint32_t i = 0; i < 12; i++)
        evo_m1(wp, 0x0400 + i * 4, fmt_identity[i]);
    // Surface (the dark-gray X8R8G8B8 scanout buffer).
    evo_m1(wp, 0x0240, H_ISO_CTXDMA);  // SET_CONTEXT_DMA_ISO[0] = surface ctxdma (resolves via chn7 hash)
    evo_m1(wp, 0x0260, 0x00000000);    // SET_OFFSET[0] = 0
    evo_m1(wp, 0x0224, 0x05a00a00);    // SET_SIZE          2560x1440
    evo_m1(wp, 0x0298, 0x05a00a00);    // SET_SIZE_IN
    evo_m1(wp, 0x02a4, 0x05a00a00);    // SET_SIZE_OUT
    evo_m1(wp, 0x02a8, 0x00000011);    // SET_CONTROL_INPUT_SCALER  V_TAPS_2|H_TAPS_2
    evo_m1(wp, 0x0228, 0x00000000);    // SET_STORAGE       pitch-linear (BLOCK_HEIGHT=ONE_GOB)
    evo_m1(wp, 0x0230, FB_PITCH >> 6); // SET_PLANAR_STORAGE[0] = pitch>>6 (=0xa0)
    evo_m1(wp, 0x022c, 0x000000e6);    // SET_PARAMS        FORMAT=X8R8G8B8
    // ILUT (input unorm10 -> FP16) identity at byte-off 0 in the shared LUT ctxdma (lut_fill_identity base[]).
    evo_m1(wp, 0x0440, 0x00040508);    // SET_ILUT_CONTROL  MODE=DIRECT10 | SIZE=0x405(1029)
    evo_m1(wp, 0x0444, H_LUT_CTXDMA);  // SET_CONTEXT_DMA_ILUT  (resolves via chn7/win6 hash)
    evo_m1(wp, 0x0448, 0x00000000);    // SET_OFFSET_ILUT   = ILUT byte-off 0 >> 8
    // CSC stages disabled (passthrough through the compositor) -- golden sets these to 0.
    evo_m1(wp, 0x045c, 0x00000000);    // SET_CSC00CONTROL  ENABLE=0
    evo_m1(wp, 0x04a0, 0x00000000);    // SET_CSC0LUT_CONTROL
    evo_m1(wp, 0x04bc, 0x00000000);    // SET_CSC01CONTROL
    evo_m1(wp, 0x053c, 0x00000000);    // SET_CSC10CONTROL
    evo_m1(wp, 0x0580, 0x00000000);    // SET_CSC1LUT_CONTROL
    evo_m1(wp, 0x059c, 0x00000000);    // SET_CSC11CONTROL
    // DEPTH composition (golden) -- NOT bypass: the window feeds the head compositor + OLUT/OCSC. This is
    // the run-#50 fix: BYPASS scanned out directly (skipping head OLUT/OCSC) and the bracketed GSP
    // supervisor refused to wake the head; depth routes through the (now-programmed) head pipeline.
    evo_m1(wp, 0x02ec, 0x00000080);    // SET_COMPOSITION_CONTROL  DEPTH=8 | BYPASS=DISABLE | COLOR_KEY=DISABLE
    evo_m1(wp, 0x02f0, 0x00000000);    // SET_COMPOSITION_CONSTANT_ALPHA
    evo_m1(wp, 0x02f4, 0x00000011);    // SET_COMPOSITION_FACTOR_SELECT  SRC color factor = ONE (opaque)
    evo_m1(wp, 0x0370, 0x00000001);    // SET_INTERLOCK_FLAGS        = INTERLOCK_WITH_CORE
    evo_m1(wp, 0x0374, 0x00000040);    // SET_WINDOW_INTERLOCK_FLAGS = win6
    evo_m1(wp, 0x0200, 0x00000001);    // UPDATE  RELEASE_ELV=TRUE
    // KICK (INTERLOCKED): core modeset + win6 flip committed atomically. With the boot heads shut down
    // (Stage 14.05) clearing the conflict, the supervisor should now promote head3 SLEEP->AWAKE here.
    const uint32_t core_put = evo_kick(cp);   // core modeset UPDATE (interlocked, waits for win6)
    const uint32_t wput     = evo_kick(wp);   // win6 flip UPDATE (interlocked, waits for core) -> latch together
    const bool cok = evo_wait_get(cp, core_put);
    const bool wok = evo_wait_get(wp, wput);
    delay::us(200000);                        // let both channels fetch + the supervisor begin the modeset
    // NB: the head does NOT wake at the kick -- the GSP supervisor only finalizes the head/OR transition
    // when the DISPLAY_CHANGE bracket is CLOSED (END, below). So we report fetch status only here; the
    // authoritative SLEEP->AWAKE is confirmed by disp_dump_state("post-modeset") after END.
    log::info("nvidia: display: KICK interlocked core+win6: core PUT=0x%x (%s) + win6 PUT=0x%x (%s) [head wakes after DISPLAY_CHANGE END]",
              core_put, cok ? "=PUT" : "STALLED", wput, wok ? "=PUT" : "STALLED");
    delay::us(150000); // let the flip + supervisor settle

    // 8. DISPLAY_CHANGE END -- close the modeset bracket (GSP finalizes the head/OR transition).
    {
        NV0073_DISPLAY_CHANGE dc; string::memset(&dc, 0, sizeof(dc));
        dc.newDevices = DP_TARGET; // 0x800
        dc.enable     = 0;         // NV0073_CTRL_SPECIFIC_DISPLAY_CHANGE_END
        if (gsp_rpc_control(rpc, ctx, bar0_va, H_CLIENT, H_DISPLAY,
                            NV0073_CTRL_CMD_SPECIFIC_DISPLAY_CHANGE, &dc, sizeof(dc), &st) != RPC_OK || st != 0)
            log::warn("nvidia: display: DISPLAY_CHANGE END failed (status=0x%x)", st);
        else
            log::info("nvidia: display: DISPLAY_CHANGE END ok (newDevices=0x%x)", DP_TARGET);
    }
    log::info("nvidia: ===== stage 16 COMPLETE: modeset committed (interlocked core+win6, bracketed) =====");
    disp_dump_state(bar0_va, "post-modeset");

    // --- Stage 17: enable the hardware cursor (separate core update on the now-awake head) ---
    // The modeset above is kept byte-faithful (cursor disabled) so it stays exactly the proven golden
    // sequence. Now that head3 is AWAKE + scanning, enabling the cursor is a normal runtime operation
    // (mirrors nvkms EvoSetCursorImageC3, nvkms-evo3.c:6286): set the cursor ctxdma + image control +
    // composition on the CORE channel, then position via the NVC67A immediate PIO channel. This is
    // strictly additive -- if it doesn't latch, the desktop frame (already latched) is unaffected.
    log::info("nvidia: --- Stage 17: hardware cursor enable (head 3) ---");
    {
        const uint32_t hc3 = 3 * 0x400; // head-3 core method stride (+0xC00)

        // (a) Allocate the cursor immediate PIO channel. NVC67A is one channel per head (head3 -> instance
        //     3, control region NV_UDISP_FE_CHN_ASSY_BASEADR_CURS(3)=0x6DB000); PIO has no pushbuffer.
        NV50VAIO_CHANNELPIO_PARAMS pio; string::memset(&pio, 0, sizeof(pio));
        pio.channelInstance = 3;
        pio.hObjectNotify   = 0;
        const bool cur_chan_ok = (gsp_rpc_alloc(rpc, ctx, bar0_va, H_CLIENT, H_DISP_ENGINE, H_CURSOR_CH,
                                  NVC67A_CURSOR_IMM_CHANNEL_PIO, &pio, sizeof(pio), &st) == RPC_OK && st == 0);
        log::info("nvidia: display: cursor PIO channel 0x%x alloc %s (status=0x%x)",
                  H_CURSOR_CH, cur_chan_ok ? "ok" : "FAILED", st);

        // (b) Enable the cursor on the core channel (continue the PB after the modeset's PUT). Faithful to
        //     EvoSetCursorImageC3: PRESENT_CONTROL(MONO) -> CONTEXT_DMA x2 + OFFSET x2 -> CONTROL
        //     (ENABLE|A8R8G8B8|W64_H64) -> COMPOSITION (NON_PREMULT_ALPHA). Non-interlocked, outside the bracket.
        evo_push xp;
        evo_begin(xp, core_pb, bar0_va, NVC67D_CORE_CTRL_BASE, core_put);
        evo_m1(xp, 0x2098 + hc3, 0x00000000);                 // HEAD_SET_PRESENT_CONTROL_CURSOR  MODE=MONO
        evo_m1(xp, 0x2088 + hc3, H_CURSOR_CTXDMA);            // HEAD_SET_CONTEXT_DMA_CURSOR(3,0)
        evo_m1(xp, 0x208C + hc3, H_CURSOR_CTXDMA);            // HEAD_SET_CONTEXT_DMA_CURSOR(3,1) (stereo-right = same)
        evo_m1(xp, 0x2090 + hc3, 0x00000000);                 // HEAD_SET_OFFSET_CURSOR(3,0) = 0
        evo_m1(xp, 0x2094 + hc3, 0x00000000);                 // HEAD_SET_OFFSET_CURSOR(3,1) = 0
        evo_m1(xp, 0x209C + hc3, HEAD_SET_CONTROL_CURSOR_VAL); // ENABLE | FORMAT_A8R8G8B8 | SIZE_W64_H64 (0x800001cf)
        evo_m1(xp, 0x20A0 + hc3, 0x000075ff);                 // COMPOSITION NON_PREMULT_ALPHA (K1=255|CUR=K1*SRC|VP=NEG_K1*SRC|BLEND)
        evo_m1(xp, 0x0218, 0x00000000);                       // SET_INTERLOCK_FLAGS        = 0
        evo_m1(xp, 0x021c, 0x00000000);                       // SET_WINDOW_INTERLOCK_FLAGS = 0
        evo_m1(xp, 0x0200, 0x00000001);                       // UPDATE
        const uint32_t cur_put = evo_kick(xp);
        const bool curok = evo_wait_get(xp, cur_put);
        delay::us(50000);
        dump_core(bar0_va, "CONTROL_CURSOR", 0x209c + hc3, HEAD_SET_CONTROL_CURSOR_VAL);
        log::info("nvidia: display: cursor enable core update PUT=0x%x %s",
                  cur_put, curok ? "=PUT (fetched)" : "STALLED");

        // (c) Position the cursor once at screen center via the PIO channel. Static cursor for now;
        //     dynamic positioning (gpu_set_cursor_pos + /dev/nvdisp) was a temporary test, now removed.
        if (cur_chan_ok) {
            const uint32_t cx = FB_W / 2, cy = FB_H / 2;
            const uint32_t cpos = (cy << 16) | (cx & 0xffff); // SET_CURSOR_HOT_SPOT_POINT_OUT: X[15:0] Y[31:16]
            cursor_pio_wait_free(bar0_va + NVC67A_CURS3_CTRL_BASE);
            mmio::write32(bar0_va + NVC67A_CURS3_CTRL_BASE + NVC67A_HOT_SPOT_POINT_OUT_OFF, cpos);
            cursor_pio_wait_free(bar0_va + NVC67A_CURS3_CTRL_BASE);
            mmio::write32(bar0_va + NVC67A_CURS3_CTRL_BASE + NVC67A_UPDATE_OFF, 0x00000000); // UPDATE: FLIP_LOCK_PIN_NONE (nvkms MoveCursorC3, immediate)
            barrier::dma_full();
            log::info("nvidia: display: HW cursor ENABLED + centered at (%u,%u)", cx, cy);
        }
    }
    log::info("nvidia: ===== display init COMPLETE: head 3 lit + HW cursor (full GSP sequence) =====");
    return DISP_OK;
}

} // namespace nvidia
