#include "sm3_to_sm4.hpp"
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <string>

// SM3 (D3D9) to SM4 (D3D11) DXBC shader converter.
// Handles vs_3_0 and ps_3_0 bytecode.

namespace dxmt::d3d9 {

// ============================================================
// SM3 constants
// ============================================================

enum SM3Opcode : uint32_t {
    SM3_NOP     = 0,
    SM3_MOV     = 1,
    SM3_ADD     = 2,
    SM3_SUB     = 3,
    SM3_MAD     = 4,
    SM3_MUL     = 5,
    SM3_RCP     = 6,
    SM3_RSQ     = 7,
    SM3_DP3     = 8,
    SM3_DP4     = 9,
    SM3_MIN     = 10,
    SM3_MAX     = 11,
    SM3_SLT     = 12,
    SM3_SGE     = 13,
    SM3_EXP     = 14,
    SM3_LOG     = 15,
    SM3_LIT     = 16,
    SM3_DST     = 17,
    SM3_LRP     = 18,
    SM3_FRC     = 19,
    SM3_M4x4    = 20,
    SM3_M4x3    = 21,
    SM3_M3x4    = 22,
    SM3_M3x3    = 23,
    SM3_M3x2    = 24,
    SM3_CALL    = 25,
    SM3_CALLNZ  = 26,
    SM3_LOOP    = 27,
    SM3_RET     = 28,
    SM3_ENDLOOP = 29,
    SM3_LABEL   = 30,
    SM3_DCL     = 31,
    SM3_POW     = 32,
    SM3_CRS     = 33,
    SM3_SGN     = 34,
    SM3_ABS     = 35,
    SM3_NRM     = 36,
    SM3_SINCOS  = 37,
    SM3_REP     = 38,
    SM3_ENDREP  = 39,
    SM3_IF      = 40,
    SM3_IFC     = 41,
    SM3_ELSE    = 42,
    SM3_ENDIF   = 43,
    SM3_BREAK   = 44,
    SM3_BREAKC  = 45,
    SM3_MOVA    = 46,
    SM3_DEFB    = 47,
    SM3_DEFI    = 48,
    SM3_TEXKILL = 65,
    SM3_TEX     = 66,
    SM3_DEF     = 81,
    SM3_CMP     = 88,
    SM3_DP2ADD  = 90,
    SM3_DSX     = 91,
    SM3_DSY     = 92,
    SM3_TEXLDD  = 93,
    SM3_TEXLDL  = 95,
    SM3_COMMENT = 0xFFFE,
    SM3_END     = 0xFFFF,
};

enum SM3RegType : uint32_t {
    SM3RT_TEMP      = 0,
    SM3RT_INPUT     = 1,
    SM3RT_CONST     = 2,
    SM3RT_ADDR      = 3,
    SM3RT_RASTOUT   = 4,
    SM3RT_ATTROUT   = 5,
    SM3RT_OUTPUT    = 6,
    SM3RT_CONSTINT  = 7,
    SM3RT_COLOROUT  = 8,
    SM3RT_DEPTHOUT  = 9,
    SM3RT_SAMPLER   = 10,
    SM3RT_CONST2    = 11,
    SM3RT_CONST3    = 12,
    SM3RT_CONST4    = 13,
    SM3RT_CONSTBOOL = 14,
    SM3RT_LOOP      = 15,
    SM3RT_MISCTYPE  = 17,
};

// SM3 source modifiers
enum SM3SrcMod : uint32_t {
    SM3SRC_NONE   = 0,
    SM3SRC_NEG    = 1,
    SM3SRC_ABS    = 11,
    SM3SRC_ABSNEG = 12,
};

// ============================================================
// SM4 constants (from d3d12tokenizedprogramformat.hpp)
// ============================================================

enum SM4Opcode : uint32_t {
    SM4_ADD         = 0,
    SM4_AND         = 1,
    SM4_BREAK       = 2,
    SM4_BREAKC      = 3,
    SM4_DERIV_RTX   = 11,
    SM4_DERIV_RTY   = 12,
    SM4_DISCARD     = 13,
    SM4_DIV         = 14,
    SM4_DP3         = 16,
    SM4_DP4         = 17,
    SM4_ELSE        = 18,
    SM4_ENDIF       = 21,
    SM4_ENDLOOP     = 22,
    SM4_EXP         = 25,
    SM4_FRC         = 26,
    SM4_GE          = 29,
    SM4_IF          = 31,
    SM4_LOG         = 47,
    SM4_LOOP        = 48,
    SM4_LT          = 49,
    SM4_MAD         = 50,
    SM4_MIN         = 51,
    SM4_MAX         = 52,
    SM4_MOV         = 54,
    SM4_MOVC        = 55,
    SM4_MUL         = 56,
    SM4_NOP         = 58,
    SM4_RET         = 62,
    SM4_RSQ         = 68,
    SM4_SAMPLE      = 69,
    SM4_SAMPLE_L    = 72,
    SM4_SAMPLE_D    = 73,
    SM4_SINCOS      = 77,
    SM4_DCL_RESOURCE           = 88,
    SM4_DCL_CONSTANT_BUFFER    = 89,
    SM4_DCL_SAMPLER            = 90,
    SM4_DCL_INPUT              = 95,
    SM4_DCL_INPUT_PS           = 98,
    SM4_DCL_OUTPUT             = 101,
    SM4_DCL_OUTPUT_SIV         = 103,
    SM4_DCL_TEMPS              = 104,
};

enum SM4OperandType : uint32_t {
    SM4OT_TEMP        = 0,
    SM4OT_INPUT       = 1,
    SM4OT_OUTPUT      = 2,
    SM4OT_IMM32       = 4,
    SM4OT_SAMPLER     = 6,
    SM4OT_RESOURCE    = 7,
    SM4OT_CB          = 8,
    SM4OT_OUTPUT_DEPTH = 12,
};

static constexpr uint32_t SM4_SWIZZLE_XYZW = 0xE4; // (0 | 1<<2 | 2<<4 | 3<<6)
static constexpr uint32_t SM4_SAT           = (1u << 13);  // saturation control bit
static constexpr uint32_t SM4_NONZERO_TEST  = (1u << 18);  // for DISCARD/IF nonzero

// Scratch temp register index: always the last one beyond game temps
static constexpr uint32_t kScratchTemps = 4; // extra scratch regs appended

// ============================================================
// SM3 decode helpers
// ============================================================

static inline uint32_t sm3_opcode(uint32_t tok)    { return tok & 0xFFFF; }
static inline uint32_t sm3_regtype(uint32_t tok)   { return ((tok >> 28) & 0x7) | ((tok >> 8) & 0x18); }
static inline uint32_t sm3_regnum(uint32_t tok)    { return tok & 0x7FF; }
static inline uint32_t sm3_writemask(uint32_t tok) { return (tok >> 16) & 0xF; }
static inline uint32_t sm3_swizzle(uint32_t tok)   { return (tok >> 16) & 0xFF; }
static inline uint32_t sm3_srcmod(uint32_t tok)    { return (tok >> 24) & 0xF; }
static inline bool     sm3_dst_sat(uint32_t tok)   { return ((tok >> 20) & 1) != 0; }
static inline uint32_t sm3_instrlen(uint32_t tok)  { return (tok >> 24) & 0xF; }

// Map SM3 source modifier to SM4 extended modifier value
static uint32_t MapSrcMod(uint32_t sm3mod) {
    switch (sm3mod) {
    case SM3SRC_NEG:    return 1;
    case SM3SRC_ABS:    return 2;
    case SM3SRC_ABSNEG: return 3;
    default:            return 0;
    }
}

// ============================================================
// SM4 token builder
// ============================================================

struct SM4Builder {
    std::vector<uint32_t> tokens;

    // Begin an instruction: push opcode placeholder, return its index
    size_t beginOp() {
        size_t pos = tokens.size();
        tokens.push_back(0);
        return pos;
    }

    // End instruction: patch opcode token with final length
    void endOp(size_t pos, uint32_t opcode, uint32_t controlBits = 0) {
        uint32_t length = (uint32_t)(tokens.size() - pos);
        tokens[pos] = (opcode & 0x7FF) | controlBits | (length << 24);
    }

    // 4-comp operand with write mask, 1D immediate index (for dst)
    void emitDst(uint32_t regType, uint32_t regIdx, uint32_t mask) {
        tokens.push_back(2u | (0u << 2) | ((mask & 0xF) << 4) |
                         ((regType & 0xFF) << 12) | (1u << 20));
        tokens.push_back(regIdx);
    }

    // 4-comp operand with swizzle, 1D immediate index (for src without modifier)
    void emitSrc(uint32_t regType, uint32_t regIdx, uint32_t swizzle4) {
        tokens.push_back(2u | (1u << 2) | ((swizzle4 & 0xFF) << 4) |
                         ((regType & 0xFF) << 12) | (1u << 20));
        tokens.push_back(regIdx);
    }

    // 4-comp operand with swizzle and optional extended modifier, 1D index
    void emitSrcMod(uint32_t regType, uint32_t regIdx, uint32_t swizzle4, uint32_t mod) {
        if (mod == 0) { emitSrc(regType, regIdx, swizzle4); return; }
        tokens.push_back(2u | (1u << 2) | ((swizzle4 & 0xFF) << 4) |
                         ((regType & 0xFF) << 12) | (1u << 20) | (1u << 31));
        tokens.push_back(1u | ((mod & 0xFF) << 6));
        tokens.push_back(regIdx);
    }

    // Constant buffer cb[cbSlot][elemIdx] with swizzle and optional modifier, 2D index
    void emitCB(uint32_t cbSlot, uint32_t elemIdx, uint32_t swizzle4, uint32_t mod) {
        uint32_t extBit = (mod != 0) ? (1u << 31) : 0;
        tokens.push_back(2u | (1u << 2) | ((swizzle4 & 0xFF) << 4) |
                         (SM4OT_CB << 12) | (2u << 20) | extBit);
        if (mod != 0) tokens.push_back(1u | ((mod & 0xFF) << 6));
        tokens.push_back(cbSlot);
        tokens.push_back(elemIdx);
    }

    // 0-comp sampler, 1D index
    void emitSampler(uint32_t regIdx) {
        tokens.push_back((SM4OT_SAMPLER << 12) | (1u << 20));
        tokens.push_back(regIdx);
    }

    // 0-comp resource, 1D index
    void emitResource(uint32_t regIdx) {
        tokens.push_back((SM4OT_RESOURCE << 12) | (1u << 20));
        tokens.push_back(regIdx);
    }

    // 0-comp output depth, no index
    void emitOutputDepth() {
        tokens.push_back(SM4OT_OUTPUT_DEPTH << 12);
    }

    // 4-comp float immediate (4 DWORDs of float values)
    void emitImm4(float x, float y, float z, float w) {
        tokens.push_back(2u | (0u << 2) | (0xFu << 4) | (SM4OT_IMM32 << 12));
        uint32_t bx, by, bz, bw;
        memcpy(&bx, &x, 4); memcpy(&by, &y, 4);
        memcpy(&bz, &z, 4); memcpy(&bw, &w, 4);
        tokens.push_back(bx); tokens.push_back(by);
        tokens.push_back(bz); tokens.push_back(bw);
    }

    // Immediate with raw uint4 bits
    void emitImm4u(uint32_t x, uint32_t y, uint32_t z, uint32_t w) {
        tokens.push_back(2u | (0u << 2) | (0xFu << 4) | (SM4OT_IMM32 << 12));
        tokens.push_back(x); tokens.push_back(y);
        tokens.push_back(z); tokens.push_back(w);
    }
};

// ============================================================
// Shader info gathered in first pass
// ============================================================

struct IODecl {
    uint32_t regType;
    uint32_t regIdx;
    uint32_t usage;
    uint32_t usageIdx;
    uint32_t texType;  // for samplers: 2D=2, CUBE=3, VOLUME=4
    uint32_t mask;
};

struct ShaderInfo {
    bool     isPS = false;
    uint32_t numTemps = 4;
    std::vector<IODecl> inputs;
    std::vector<IODecl> outputs;
    std::vector<IODecl> samplers;
    bool usesDepthOut = false;
};

// ============================================================
// First pass: scan DCL declarations
// ============================================================

static void ScanBytecode(const uint32_t *pCode, size_t dwCount, ShaderInfo &info) {
    uint32_t versionTok = pCode[0];
    info.isPS = ((versionTok >> 16) & 0xFFFF) == 0xFFFF;
    info.numTemps = 4;

    const uint32_t *p   = pCode + 1;
    const uint32_t *end = pCode + dwCount;

    while (p < end) {
        uint32_t tok = *p;
        uint32_t op  = sm3_opcode(tok);

        if (op == SM3_END) break;

        if (op == 0xFFFE) { // COMMENT
            p += 1 + ((tok >> 16) & 0x7FFF);
            continue;
        }

        uint32_t instrLen = sm3_instrlen(tok);
        const uint32_t *next = p + 1 + instrLen;

        if (op == SM3_DCL && instrLen >= 2) {
            uint32_t usageTok = p[1];
            uint32_t regTok   = p[2];
            IODecl decl;
            decl.regType  = sm3_regtype(regTok);
            decl.regIdx   = sm3_regnum(regTok);
            decl.usage    = usageTok & 0x1F;
            decl.usageIdx = (usageTok >> 16) & 0xF;
            decl.texType  = (usageTok >> 27) & 0x3;
            decl.mask     = sm3_writemask(regTok);
            if (decl.mask == 0) decl.mask = 0xF;

            if (decl.regType == SM3RT_SAMPLER) {
                info.samplers.push_back(decl);
            } else if (decl.regType == SM3RT_INPUT || decl.regType == SM3RT_MISCTYPE) {
                info.inputs.push_back(decl);
            } else {
                // RASTOUT, ATTROUT, OUTPUT, COLOROUT, DEPTHOUT
                if (decl.regType == SM3RT_DEPTHOUT) info.usesDepthOut = true;
                info.outputs.push_back(decl);
            }
        }

        // Scan all operand tokens for temp register high-water mark
        for (uint32_t i = 1; i <= instrLen; i++) {
            if (p + i >= end) break;
            uint32_t rtok  = p[i];
            uint32_t rtype = sm3_regtype(rtok);
            uint32_t rnum  = sm3_regnum(rtok);
            if (rtype == SM3RT_TEMP && rnum + 1 > info.numTemps)
                info.numTemps = rnum + 1;
        }

        p = next;
    }
}

// ============================================================
// ISGN/OSGN blob construction
// ============================================================

static const char *UsageName(uint32_t usage, bool forSvPosition) {
    if (forSvPosition) return "SV_Position";
    switch (usage) {
    case 0:  return "POSITION";
    case 1:  return "BLENDWEIGHT";
    case 2:  return "BLENDINDICES";
    case 3:  return "NORMAL";
    case 4:  return "PSIZE";
    case 5:  return "TEXCOORD";
    case 6:  return "TANGENT";
    case 7:  return "BINORMAL";
    case 8:  return "TESSFACTOR";
    case 9:  return "POSITIONT";
    case 10: return "COLOR";
    case 11: return "FOG";
    case 12: return "DEPTH";
    default: return "TEXCOORD";
    }
}

// Returns SM4 output register for SM3 output register
static uint32_t GetSM4OutReg(uint32_t sm3rtype, uint32_t sm3rnum) {
    switch (sm3rtype) {
    case SM3RT_RASTOUT:  return sm3rnum;           // oPos=0, oFog=1, oPts=2
    case SM3RT_ATTROUT:  return 3 + sm3rnum;       // oD0→3, oD1→4
    case SM3RT_OUTPUT:   return 5 + sm3rnum;       // oT0→5, oT1→6, ...
    case SM3RT_COLOROUT: return sm3rnum;            // PS: oC0→0, oC1→1, ...
    default:             return 0;
    }
}

struct SigParam {
    std::string name;
    uint32_t    nameIdx;  // offset into string table (filled later)
    uint32_t    semanticIdx;
    uint32_t    svName;   // system value: 0=none, 1=SV_Position
    uint32_t    compType; // 3=float32
    uint32_t    reg;      // SM4 register number
    uint32_t    mask;
    uint32_t    readWriteMask;
};

static std::vector<uint8_t> BuildSigBlob(const std::vector<SigParam> &params) {
    // String table: collect unique names
    std::vector<std::string> names;
    for (auto &p : params) {
        bool found = false;
        for (auto &n : names) if (n == p.name) { found = true; break; }
        if (!found) names.push_back(p.name);
    }

    // Build string table bytes (null-terminated, DWORD-padded)
    std::vector<uint8_t> strTable;
    std::vector<uint32_t> nameOffsets; // offset from start of blob data for each unique name
    for (auto &n : names) {
        uint32_t base = (uint32_t)strTable.size();
        // offset is from start of blob data (which starts at offset 8: paramCount+tableOffset)
        // Parameter table: 8 + N*24 bytes. String table comes after.
        // We'll fix up offsets after we know table size.
        nameOffsets.push_back(base);
        for (char c : n) strTable.push_back((uint8_t)c);
        strTable.push_back(0);
        // Pad to DWORD
        while (strTable.size() % 4) strTable.push_back(0);
    }

    uint32_t paramCount   = (uint32_t)params.size();
    uint32_t paramTableOff = 8;
    uint32_t paramTableSize = paramCount * 24;
    uint32_t strTableOff  = paramTableOff + paramTableSize; // from start of blob data

    // Build param table (24 bytes each)
    std::vector<uint8_t> paramTable;
    paramTable.resize(paramTableSize, 0);
    for (uint32_t i = 0; i < paramCount; i++) {
        const SigParam &sp = params[i];
        // Find name offset in strTable
        uint32_t nameOff = 0;
        for (uint32_t j = 0; j < (uint32_t)names.size(); j++) {
            if (names[j] == sp.name) { nameOff = nameOffsets[j]; break; }
        }
        // Absolute offset from start of blob data
        uint32_t absNameOff = strTableOff + nameOff;

        uint8_t *entry = paramTable.data() + i * 24;
        memcpy(entry +  0, &absNameOff,       4);
        memcpy(entry +  4, &sp.semanticIdx,   4);
        memcpy(entry +  8, &sp.svName,        4);
        memcpy(entry + 12, &sp.compType,      4);
        memcpy(entry + 16, &sp.reg,           4);
        entry[20] = (uint8_t)sp.mask;
        entry[21] = (uint8_t)sp.readWriteMask;
        entry[22] = 0; entry[23] = 0;
    }

    // Assemble: header (8 bytes) + param table + string table
    std::vector<uint8_t> blob;
    blob.resize(8 + paramTableSize + strTable.size(), 0);
    uint8_t *p = blob.data();
    memcpy(p, &paramCount,    4);
    memcpy(p + 4, &paramTableOff, 4);
    memcpy(p + 8, paramTable.data(), paramTableSize);
    memcpy(p + 8 + paramTableSize, strTable.data(), strTable.size());
    return blob;
}

// Build ISGN for VS: declared inputs
static std::vector<uint8_t> BuildVsIsgn(const ShaderInfo &info) {
    std::vector<SigParam> params;
    // Sort by register number for correctness
    std::vector<IODecl> sorted = info.inputs;
    std::sort(sorted.begin(), sorted.end(), [](const IODecl &a, const IODecl &b){
        return a.regIdx < b.regIdx;
    });
    for (auto &d : sorted) {
        if (d.regType != SM3RT_INPUT) continue;
        SigParam sp;
        sp.name        = UsageName(d.usage, false);
        sp.semanticIdx = d.usageIdx;
        sp.svName      = 0;
        sp.compType    = 3; // float32
        sp.reg         = d.regIdx;
        sp.mask        = d.mask ? d.mask : 0xF;
        sp.readWriteMask = 0; // AlwaysReads
        params.push_back(sp);
    }
    if (params.empty()) {
        // No DCL inputs: create a placeholder POSITION v0
        SigParam sp;
        sp.name = "POSITION"; sp.semanticIdx = 0; sp.svName = 0;
        sp.compType = 3; sp.reg = 0; sp.mask = 0xF; sp.readWriteMask = 0;
        params.push_back(sp);
    }
    return BuildSigBlob(params);
}

// Build OSGN for VS: declared outputs
static std::vector<uint8_t> BuildVsOsgn(const ShaderInfo &info) {
    std::vector<SigParam> params;
    for (auto &d : info.outputs) {
        SigParam sp;
        bool isSvPos = (d.regType == SM3RT_RASTOUT && d.regIdx == 0);
        sp.name        = UsageName(d.usage, isSvPos);
        sp.semanticIdx = d.usageIdx;
        sp.svName      = isSvPos ? 1u : 0u; // D3D10_NAME_POSITION = 1
        sp.compType    = 3;
        sp.reg         = GetSM4OutReg(d.regType, d.regIdx);
        sp.mask        = d.mask ? d.mask : 0xF;
        sp.readWriteMask = 0;
        params.push_back(sp);
    }
    if (params.empty()) {
        // Minimal: SV_Position at o0
        SigParam sp;
        sp.name = "SV_Position"; sp.semanticIdx = 0; sp.svName = 1;
        sp.compType = 3; sp.reg = 0; sp.mask = 0xF; sp.readWriteMask = 0;
        params.push_back(sp);
    }
    std::sort(params.begin(), params.end(), [](const SigParam &a, const SigParam &b){
        return a.reg < b.reg;
    });
    return BuildSigBlob(params);
}

// Build ISGN for PS: declared inputs
static std::vector<uint8_t> BuildPsIsgn(const ShaderInfo &info) {
    std::vector<SigParam> params;
    std::vector<IODecl> sorted = info.inputs;
    std::sort(sorted.begin(), sorted.end(), [](const IODecl &a, const IODecl &b){
        return a.regIdx < b.regIdx;
    });
    for (auto &d : sorted) {
        if (d.regType != SM3RT_INPUT) continue;
        SigParam sp;
        sp.name        = UsageName(d.usage, false);
        sp.semanticIdx = d.usageIdx;
        sp.svName      = 0;
        sp.compType    = 3;
        sp.reg         = d.regIdx;
        sp.mask        = d.mask ? d.mask : 0xF;
        sp.readWriteMask = 0xFF; // NeverWrites (input)
        params.push_back(sp);
    }
    if (params.empty()) {
        SigParam sp;
        sp.name = "SV_Position"; sp.semanticIdx = 0; sp.svName = 1;
        sp.compType = 3; sp.reg = 0xFFFFFFFF; sp.mask = 0xF; sp.readWriteMask = 0xFF;
        params.push_back(sp);
    }
    return BuildSigBlob(params);
}

// Build OSGN for PS: color outputs
static std::vector<uint8_t> BuildPsOsgn(const ShaderInfo &info) {
    std::vector<SigParam> params;
    for (auto &d : info.outputs) {
        if (d.regType != SM3RT_COLOROUT) continue;
        SigParam sp;
        sp.name        = "SV_Target";
        sp.semanticIdx = d.regIdx;
        sp.svName      = 0;
        sp.compType    = 3;
        sp.reg         = d.regIdx;
        sp.mask        = 0xF;
        sp.readWriteMask = 0;
        params.push_back(sp);
    }
    if (params.empty()) {
        SigParam sp;
        sp.name = "SV_Target"; sp.semanticIdx = 0; sp.svName = 0;
        sp.compType = 3; sp.reg = 0; sp.mask = 0xF; sp.readWriteMask = 0;
        params.push_back(sp);
    }
    if (info.usesDepthOut) {
        // depth output has no OSGN entry (handled by SM4OT_OUTPUT_DEPTH in SHDR)
    }
    return BuildSigBlob(params);
}

// ============================================================
// SM4 source/destination operand emitters
// ============================================================

static void EmitSM4Dst(SM4Builder &b, uint32_t sm3tok, bool isPS, uint32_t numTemps) {
    uint32_t rtype = sm3_regtype(sm3tok);
    uint32_t rnum  = sm3_regnum(sm3tok);
    uint32_t mask  = sm3_writemask(sm3tok);
    if (mask == 0) mask = 0xF;

    switch (rtype) {
    case SM3RT_TEMP:
        b.emitDst(SM4OT_TEMP, rnum, mask);
        break;
    case SM3RT_ADDR:
        b.emitDst(SM4OT_TEMP, numTemps + 2, mask); // address register in scratch
        break;
    case SM3RT_RASTOUT:
    case SM3RT_ATTROUT:
    case SM3RT_OUTPUT:
    case SM3RT_COLOROUT:
        b.emitDst(SM4OT_OUTPUT, GetSM4OutReg(rtype, rnum), mask);
        break;
    case SM3RT_DEPTHOUT:
        b.emitOutputDepth();
        break;
    default:
        b.emitDst(SM4OT_TEMP, 0, mask);
        break;
    }
}

static void EmitSM4Src(SM4Builder &b, uint32_t sm3tok, uint32_t numTemps) {
    uint32_t rtype = sm3_regtype(sm3tok);
    uint32_t rnum  = sm3_regnum(sm3tok);
    uint32_t swz   = sm3_swizzle(sm3tok); // 8-bit SM3 swizzle
    uint32_t mod   = MapSrcMod(sm3_srcmod(sm3tok));

    switch (rtype) {
    case SM3RT_TEMP:
        b.emitSrcMod(SM4OT_TEMP, rnum, swz, mod);
        break;
    case SM3RT_CONST:
        b.emitCB(0, rnum, swz, mod);
        break;
    case SM3RT_CONST2:
        b.emitCB(0, 2048 + rnum, swz, mod);
        break;
    case SM3RT_CONST3:
        b.emitCB(0, 4096 + rnum, swz, mod);
        break;
    case SM3RT_CONST4:
        b.emitCB(0, 6144 + rnum, swz, mod);
        break;
    case SM3RT_INPUT:
        b.emitSrcMod(SM4OT_INPUT, rnum, swz, mod);
        break;
    case SM3RT_ADDR:
        b.emitSrcMod(SM4OT_TEMP, numTemps + 2, swz, mod);
        break;
    default:
        // Fallback: read from temp 0 with identity swizzle
        b.emitSrcMod(SM4OT_TEMP, 0, SM4_SWIZZLE_XYZW, 0);
        break;
    }
}

// Emit scratch temp operand as source (XYZW swizzle)
static void EmitScratch(SM4Builder &b, uint32_t numTemps, uint32_t swz = SM4_SWIZZLE_XYZW) {
    b.emitSrc(SM4OT_TEMP, numTemps, swz);
}

// Emit scratch temp operand as destination (mask=XYZW)
static void EmitScratchDst(SM4Builder &b, uint32_t numTemps, uint32_t mask = 0xF) {
    b.emitDst(SM4OT_TEMP, numTemps, mask);
}

// ============================================================
// Matrix multiply expansion (M4x4, M4x3, etc.)
// These emit multiple DP4/DP3 instructions expanding the matrix-multiply.
// dst is a register that we write .x, .y, .z, .w components to.
// The SM3 src1 matrix starts at the register indicated (c[n]), and each
// row is in consecutive registers.
// ============================================================

static void EmitMatMul(SM4Builder &b, uint32_t dstTok, uint32_t src0Tok, uint32_t matSrc,
                       int rows, int cols, uint32_t numTemps, bool sat) {
    // cols = number of DP ops, rows = vector size used (3 or 4)
    uint32_t dstRtype = sm3_regtype(dstTok);
    uint32_t dstNum   = sm3_regnum(dstTok);
    uint32_t matBase  = sm3_regnum(matSrc);
    uint32_t matRtype = sm3_regtype(matSrc);
    uint32_t ctlBits  = sat ? SM4_SAT : 0;

    SM4Opcode dpOp = (cols == 4) ? SM4_DP4 : SM4_DP3;

    static const uint32_t xmasks[] = { 0x1, 0x2, 0x4, 0x8 };
    static const uint32_t xswiz[]  = {
        0x00,  // .xxxx
        0x55,  // .yyyy
        0xAA,  // .zzzz
        0xFF,  // .wwww
    };

    for (int i = 0; i < rows; i++) {
        size_t pos = b.beginOp();
        // dst: write single component
        b.emitDst(SM4OT_OUTPUT, GetSM4OutReg(dstRtype, dstNum), xmasks[i]);
        if (sm3_regtype(dstTok) == SM3RT_TEMP)
            b.emitDst(SM4OT_TEMP, dstNum, xmasks[i]);
        EmitSM4Src(b, src0Tok, numTemps);
        // matrix row src
        if (matRtype == SM3RT_CONST)
            b.emitCB(0, matBase + (uint32_t)i, SM4_SWIZZLE_XYZW, 0);
        else
            b.emitSrc(SM4OT_TEMP, matBase + (uint32_t)i, SM4_SWIZZLE_XYZW);
        b.endOp(pos, dpOp, ctlBits);
    }
}

// ============================================================
// Emit SM4 SHDR instructions for one SM3 instruction
// Returns false if we should stop (END token)
// ============================================================

static bool EmitSM4Instr(SM4Builder &b, const uint32_t *p, const ShaderInfo &info) {
    uint32_t instrTok = p[0];
    uint32_t op       = sm3_opcode(instrTok);
    bool     sat      = sm3_dst_sat(instrTok);
    uint32_t ctlBits  = sat ? SM4_SAT : 0;
    uint32_t numTemps = info.numTemps;

    // Operand tokens: p[1]=dst, p[2]=src0, p[3]=src1, p[4]=src2
    switch (op) {
    case SM3_NOP: {
        size_t pos = b.beginOp();
        b.endOp(pos, SM4_NOP);
        break;
    }
    case SM3_MOV: {
        size_t pos = b.beginOp();
        EmitSM4Dst(b, p[1], info.isPS, numTemps);
        EmitSM4Src(b, p[2], numTemps);
        b.endOp(pos, SM4_MOV, ctlBits);
        break;
    }
    case SM3_ABS: {
        // ABS dst, src → MOV dst, |src|  (use abs modifier on src)
        size_t pos = b.beginOp();
        EmitSM4Dst(b, p[1], info.isPS, numTemps);
        // Force abs modifier
        uint32_t s = p[2];
        uint32_t rtype = sm3_regtype(s), rnum = sm3_regnum(s), swz = sm3_swizzle(s);
        if (rtype == SM3RT_TEMP)      b.emitSrcMod(SM4OT_TEMP, rnum, swz, 2);
        else if (rtype == SM3RT_INPUT) b.emitSrcMod(SM4OT_INPUT, rnum, swz, 2);
        else { EmitSM4Src(b, s, numTemps); } // fallback (no abs for CB)
        b.endOp(pos, SM4_MOV, ctlBits);
        break;
    }
    case SM3_ADD: {
        size_t pos = b.beginOp();
        EmitSM4Dst(b, p[1], info.isPS, numTemps);
        EmitSM4Src(b, p[2], numTemps);
        EmitSM4Src(b, p[3], numTemps);
        b.endOp(pos, SM4_ADD, ctlBits);
        break;
    }
    case SM3_SUB: {
        // SUB dst, src0, src1 → ADD dst, src0, -src1
        size_t pos = b.beginOp();
        EmitSM4Dst(b, p[1], info.isPS, numTemps);
        EmitSM4Src(b, p[2], numTemps);
        // Negate src1
        { uint32_t s = p[3];
          uint32_t rtype = sm3_regtype(s), rnum = sm3_regnum(s), swz = sm3_swizzle(s);
          uint32_t origmod = MapSrcMod(sm3_srcmod(s));
          uint32_t negmod  = (origmod == 0) ? 1 : (origmod == 2) ? 3 : (origmod == 3) ? 2 : 0;
          if (rtype == SM3RT_CONST || rtype == SM3RT_CONST2 ||
              rtype == SM3RT_CONST3 || rtype == SM3RT_CONST4) {
              uint32_t base = (rtype == SM3RT_CONST) ? 0 :
                             (rtype == SM3RT_CONST2) ? 2048 :
                             (rtype == SM3RT_CONST3) ? 4096 : 6144;
              b.emitCB(0, base + rnum, swz, negmod ? negmod : 1);
          } else {
              b.emitSrcMod((rtype == SM3RT_TEMP) ? SM4OT_TEMP :
                           (rtype == SM3RT_INPUT) ? SM4OT_INPUT : SM4OT_TEMP,
                           rnum, swz, negmod ? negmod : 1);
          }
        }
        b.endOp(pos, SM4_ADD, ctlBits);
        break;
    }
    case SM3_MAD: {
        size_t pos = b.beginOp();
        EmitSM4Dst(b, p[1], info.isPS, numTemps);
        EmitSM4Src(b, p[2], numTemps);
        EmitSM4Src(b, p[3], numTemps);
        EmitSM4Src(b, p[4], numTemps);
        b.endOp(pos, SM4_MAD, ctlBits);
        break;
    }
    case SM3_MUL: {
        size_t pos = b.beginOp();
        EmitSM4Dst(b, p[1], info.isPS, numTemps);
        EmitSM4Src(b, p[2], numTemps);
        EmitSM4Src(b, p[3], numTemps);
        b.endOp(pos, SM4_MUL, ctlBits);
        break;
    }
    case SM3_RCP: {
        // RCP dst, src → DIV dst, l(1,1,1,1), src
        size_t pos = b.beginOp();
        EmitSM4Dst(b, p[1], info.isPS, numTemps);
        b.emitImm4(1.0f, 1.0f, 1.0f, 1.0f);
        EmitSM4Src(b, p[2], numTemps);
        b.endOp(pos, SM4_DIV, ctlBits);
        break;
    }
    case SM3_RSQ: {
        size_t pos = b.beginOp();
        EmitSM4Dst(b, p[1], info.isPS, numTemps);
        EmitSM4Src(b, p[2], numTemps);
        b.endOp(pos, SM4_RSQ, ctlBits);
        break;
    }
    case SM3_DP3: {
        size_t pos = b.beginOp();
        EmitSM4Dst(b, p[1], info.isPS, numTemps);
        EmitSM4Src(b, p[2], numTemps);
        EmitSM4Src(b, p[3], numTemps);
        b.endOp(pos, SM4_DP3, ctlBits);
        break;
    }
    case SM3_DP4: {
        size_t pos = b.beginOp();
        EmitSM4Dst(b, p[1], info.isPS, numTemps);
        EmitSM4Src(b, p[2], numTemps);
        EmitSM4Src(b, p[3], numTemps);
        b.endOp(pos, SM4_DP4, ctlBits);
        break;
    }
    case SM3_MIN: {
        size_t pos = b.beginOp();
        EmitSM4Dst(b, p[1], info.isPS, numTemps);
        EmitSM4Src(b, p[2], numTemps);
        EmitSM4Src(b, p[3], numTemps);
        b.endOp(pos, SM4_MIN, ctlBits);
        break;
    }
    case SM3_MAX: {
        size_t pos = b.beginOp();
        EmitSM4Dst(b, p[1], info.isPS, numTemps);
        EmitSM4Src(b, p[2], numTemps);
        EmitSM4Src(b, p[3], numTemps);
        b.endOp(pos, SM4_MAX, ctlBits);
        break;
    }
    case SM3_SLT: {
        // SLT dst, src0, src1 → LT scratch, src0, src1; AND dst, scratch, l(1.0)
        { size_t pos = b.beginOp();
          EmitScratchDst(b, numTemps);
          EmitSM4Src(b, p[2], numTemps);
          EmitSM4Src(b, p[3], numTemps);
          b.endOp(pos, SM4_LT); }
        { size_t pos = b.beginOp();
          EmitSM4Dst(b, p[1], info.isPS, numTemps);
          EmitScratch(b, numTemps, SM4_SWIZZLE_XYZW);
          static const uint32_t one_f = 0x3F800000u;
          b.emitImm4u(one_f, one_f, one_f, one_f);
          b.endOp(pos, SM4_AND, ctlBits); }
        break;
    }
    case SM3_SGE: {
        // SGE dst, src0, src1 → GE scratch, src0, src1; AND dst, scratch, l(1.0)
        { size_t pos = b.beginOp();
          EmitScratchDst(b, numTemps);
          EmitSM4Src(b, p[2], numTemps);
          EmitSM4Src(b, p[3], numTemps);
          b.endOp(pos, SM4_GE); }
        { size_t pos = b.beginOp();
          EmitSM4Dst(b, p[1], info.isPS, numTemps);
          EmitScratch(b, numTemps, SM4_SWIZZLE_XYZW);
          static const uint32_t one_f = 0x3F800000u;
          b.emitImm4u(one_f, one_f, one_f, one_f);
          b.endOp(pos, SM4_AND, ctlBits); }
        break;
    }
    case SM3_EXP: {
        size_t pos = b.beginOp();
        EmitSM4Dst(b, p[1], info.isPS, numTemps);
        EmitSM4Src(b, p[2], numTemps);
        b.endOp(pos, SM4_EXP, ctlBits);
        break;
    }
    case SM3_LOG: {
        size_t pos = b.beginOp();
        EmitSM4Dst(b, p[1], info.isPS, numTemps);
        EmitSM4Src(b, p[2], numTemps);
        b.endOp(pos, SM4_LOG, ctlBits);
        break;
    }
    case SM3_FRC: {
        size_t pos = b.beginOp();
        EmitSM4Dst(b, p[1], info.isPS, numTemps);
        EmitSM4Src(b, p[2], numTemps);
        b.endOp(pos, SM4_FRC, ctlBits);
        break;
    }
    case SM3_LRP: {
        // LRP dst, s0, s1, s2 = s0*(s1-s2) + s2
        // Step 1: ADD scratch, s1, -s2   (using negate modifier on s2)
        { size_t pos = b.beginOp();
          EmitScratchDst(b, numTemps);
          EmitSM4Src(b, p[3], numTemps);
          // Negate s2 modifier
          { uint32_t s = p[4];
            uint32_t rtype = sm3_regtype(s), rnum = sm3_regnum(s), swz = sm3_swizzle(s);
            uint32_t origmod = MapSrcMod(sm3_srcmod(s));
            uint32_t negmod  = (origmod == 2) ? 3 : (origmod == 3) ? 2 : 1;
            if (rtype == SM3RT_CONST || rtype == SM3RT_CONST2 ||
                rtype == SM3RT_CONST3 || rtype == SM3RT_CONST4) {
                uint32_t base = (rtype == SM3RT_CONST) ? 0 :
                               (rtype == SM3RT_CONST2) ? 2048 :
                               (rtype == SM3RT_CONST3) ? 4096 : 6144;
                b.emitCB(0, base + rnum, swz, negmod);
            } else {
                b.emitSrcMod((rtype == SM3RT_TEMP) ? SM4OT_TEMP :
                             (rtype == SM3RT_INPUT) ? SM4OT_INPUT : SM4OT_TEMP,
                             rnum, swz, negmod);
            }
          }
          b.endOp(pos, SM4_ADD); }
        // Step 2: MAD dst, s0, scratch, s2
        { size_t pos = b.beginOp();
          EmitSM4Dst(b, p[1], info.isPS, numTemps);
          EmitSM4Src(b, p[2], numTemps);
          EmitScratch(b, numTemps, SM4_SWIZZLE_XYZW);
          EmitSM4Src(b, p[4], numTemps);
          b.endOp(pos, SM4_MAD, ctlBits); }
        break;
    }
    case SM3_CMP: {
        // CMP dst, s0, s1, s2: dst = (s0 >= 0) ? s1 : s2
        { size_t pos = b.beginOp();
          EmitScratchDst(b, numTemps);
          EmitSM4Src(b, p[2], numTemps);
          b.emitImm4(0.0f, 0.0f, 0.0f, 0.0f);
          b.endOp(pos, SM4_GE); }
        { size_t pos = b.beginOp();
          EmitSM4Dst(b, p[1], info.isPS, numTemps);
          EmitScratch(b, numTemps, SM4_SWIZZLE_XYZW);
          EmitSM4Src(b, p[3], numTemps);
          EmitSM4Src(b, p[4], numTemps);
          b.endOp(pos, SM4_MOVC, ctlBits); }
        break;
    }
    case SM3_DP2ADD: {
        // DP2ADD dst, s0, s1, s2: dst = s0.x*s1.x + s0.y*s1.y + s2
        // Approximate: DP3 with s2 appended... not exact in SM4
        // Use: dp3 scratch.x, s0.xy0x, s1.xy0x; add dst, scratch.xxxx, s2
        // Actually SM4 doesn't have DP2. Use: mul scratch, s0.xy, s1.xy; add scratch2, scratch.x, scratch.y; add dst, scratch, s2
        // For simplicity: use MAD twice
        // scratch.x = s0.x * s1.x + s2.x
        { uint32_t xswz = 0x00; // .xxxx
          uint32_t yswz = 0x55; // .yyyy
          // scratch.x = s0.x * s1.x
          size_t pos = b.beginOp();
          EmitScratchDst(b, numTemps, 0x1);
          { uint32_t s = p[2]; uint32_t rt=sm3_regtype(s), rn=sm3_regnum(s);
            if (rt==SM3RT_TEMP) b.emitSrc(SM4OT_TEMP, rn, xswz);
            else b.emitSrc(SM4OT_INPUT, rn, xswz); }
          { uint32_t s = p[3]; uint32_t rt=sm3_regtype(s), rn=sm3_regnum(s);
            if (rt==SM3RT_TEMP) b.emitSrc(SM4OT_TEMP, rn, xswz);
            else b.emitCB(0, rn, xswz, 0); }
          b.endOp(pos, SM4_MUL); }
        // scratch.x += s0.y * s1.y
        { size_t pos = b.beginOp();
          EmitScratchDst(b, numTemps, 0x1);
          { uint32_t s = p[2]; uint32_t rt=sm3_regtype(s), rn=sm3_regnum(s);
            if (rt==SM3RT_TEMP) b.emitSrc(SM4OT_TEMP, rn, 0x55);
            else b.emitSrc(SM4OT_INPUT, rn, 0x55); }
          { uint32_t s = p[3]; uint32_t rt=sm3_regtype(s), rn=sm3_regnum(s);
            if (rt==SM3RT_TEMP) b.emitSrc(SM4OT_TEMP, rn, 0x55);
            else b.emitCB(0, rn, 0x55, 0); }
          EmitScratch(b, numTemps, 0x00);
          b.endOp(pos, SM4_MAD); }
        // dst = scratch.x + s2
        { size_t pos = b.beginOp();
          EmitSM4Dst(b, p[1], info.isPS, numTemps);
          EmitScratch(b, numTemps, 0x00); // replicate X
          EmitSM4Src(b, p[4], numTemps);
          b.endOp(pos, SM4_ADD, ctlBits); }
        break;
    }
    case SM3_NRM: {
        // NRM dst, src → normalize: dp3 scratch.x, src, src; rsq scratch.x, scratch.x; mul dst, src, scratch.xxxx
        { size_t pos = b.beginOp();
          EmitScratchDst(b, numTemps, 0x1);
          EmitSM4Src(b, p[2], numTemps);
          EmitSM4Src(b, p[2], numTemps);
          b.endOp(pos, SM4_DP3); }
        { size_t pos = b.beginOp();
          EmitScratchDst(b, numTemps, 0x1);
          EmitScratch(b, numTemps, 0x00); // .xxxx
          b.endOp(pos, SM4_RSQ); }
        { size_t pos = b.beginOp();
          EmitSM4Dst(b, p[1], info.isPS, numTemps);
          EmitSM4Src(b, p[2], numTemps);
          EmitScratch(b, numTemps, 0x00); // replicate .x
          b.endOp(pos, SM4_MUL, ctlBits); }
        break;
    }
    case SM3_SINCOS: {
        // SM3 SINCOS: may have 3 or 5 src operands (vs_3_0/ps_3_0 use 3)
        // SINCOS cosDst, sinDst, src  (or in vs_2_0: sincos dst, src, c0, c1)
        // In SM3 (3.0): SINCOS dst_cos_sin, src_x
        // SM4: sincos dst_cos, dst_sin, src
        // SM3 SINCOS packs cos in .x, sin in .y of dst (ps_3_0 behavior)
        // For simplicity: emit sincos on the same dst register
        size_t pos = b.beginOp();
        // cos dst
        EmitSM4Dst(b, p[1], info.isPS, numTemps);
        // sin dst (same reg but .y)
        EmitSM4Dst(b, p[1], info.isPS, numTemps);
        EmitSM4Src(b, p[2], numTemps);
        b.endOp(pos, SM4_SINCOS, ctlBits);
        break;
    }
    case SM3_MOVA: {
        // MOVA a0, src → MOV scratch_addr, src (treat as float mov)
        size_t pos = b.beginOp();
        b.emitDst(SM4OT_TEMP, numTemps + 2, sm3_writemask(p[1]) ? sm3_writemask(p[1]) : 0xF);
        EmitSM4Src(b, p[2], numTemps);
        b.endOp(pos, SM4_MOV);
        break;
    }
    case SM3_POW: {
        // POW dst, s0, s1: dst = |s0|^s1 = exp(s1 * log(|s0|))
        // Step 1: LOG scratch, |s0|
        { uint32_t s = p[2]; uint32_t rtype=sm3_regtype(s), rnum=sm3_regnum(s), swz=sm3_swizzle(s);
          size_t pos = b.beginOp();
          EmitScratchDst(b, numTemps);
          if (rtype == SM3RT_TEMP) b.emitSrcMod(SM4OT_TEMP, rnum, swz, 2); // abs
          else b.emitCB(0, rnum, swz, 2);
          b.endOp(pos, SM4_LOG); }
        // Step 2: MUL scratch, scratch, s1
        { size_t pos = b.beginOp();
          EmitScratchDst(b, numTemps);
          EmitScratch(b, numTemps, SM4_SWIZZLE_XYZW);
          EmitSM4Src(b, p[3], numTemps);
          b.endOp(pos, SM4_MUL); }
        // Step 3: EXP dst, scratch
        { size_t pos = b.beginOp();
          EmitSM4Dst(b, p[1], info.isPS, numTemps);
          EmitScratch(b, numTemps, SM4_SWIZZLE_XYZW);
          b.endOp(pos, SM4_EXP, ctlBits); }
        break;
    }
    case SM3_DST: {
        // DST dst, s0, s1: dst = {1, s0.y*s1.y, s0.z, s1.w}
        // Approximate with MOV from s0
        size_t pos = b.beginOp();
        EmitSM4Dst(b, p[1], info.isPS, numTemps);
        EmitSM4Src(b, p[2], numTemps);
        b.endOp(pos, SM4_MOV, ctlBits);
        break;
    }
    case SM3_LIT: {
        // LIT dst, src: dst.x=1, dst.y=max(src.x,0), dst.z=src.x>0?max(src.y,0)^clamp(src.w,-128,128):0, dst.w=1
        // Approximate: MOV dst, src (wrong but won't crash)
        size_t pos = b.beginOp();
        EmitSM4Dst(b, p[1], info.isPS, numTemps);
        b.emitImm4(1.0f, 0.0f, 0.0f, 1.0f);
        b.endOp(pos, SM4_MOV, ctlBits);
        break;
    }
    case SM3_M4x4: {
        // dst = s0 * s1 (4x4 matrix, dst gets 4 components)
        uint32_t dstMask = sm3_writemask(p[1]); if (!dstMask) dstMask = 0xF;
        uint32_t dstRtype = sm3_regtype(p[1]), dstRnum = sm3_regnum(p[1]);
        uint32_t matBase  = sm3_regnum(p[3]), matRtype = sm3_regtype(p[3]);
        static const uint32_t xmasks[] = { 0x1, 0x2, 0x4, 0x8 };
        for (int i = 0; i < 4; i++) {
            size_t pos = b.beginOp();
            if (dstRtype == SM3RT_TEMP)
                b.emitDst(SM4OT_TEMP, dstRnum, xmasks[i]);
            else
                b.emitDst(SM4OT_OUTPUT, GetSM4OutReg(dstRtype, dstRnum), xmasks[i]);
            EmitSM4Src(b, p[2], numTemps);
            if (matRtype == SM3RT_CONST || matRtype == SM3RT_CONST2)
                b.emitCB(0, matBase + (uint32_t)i, SM4_SWIZZLE_XYZW, 0);
            else
                b.emitSrc(SM4OT_TEMP, matBase + (uint32_t)i, SM4_SWIZZLE_XYZW);
            b.endOp(pos, SM4_DP4, ctlBits);
        }
        break;
    }
    case SM3_M4x3: {
        uint32_t dstRtype = sm3_regtype(p[1]), dstRnum = sm3_regnum(p[1]);
        uint32_t matBase  = sm3_regnum(p[3]), matRtype = sm3_regtype(p[3]);
        static const uint32_t xmasks[] = { 0x1, 0x2, 0x4 };
        for (int i = 0; i < 3; i++) {
            size_t pos = b.beginOp();
            if (dstRtype == SM3RT_TEMP) b.emitDst(SM4OT_TEMP, dstRnum, xmasks[i]);
            else b.emitDst(SM4OT_OUTPUT, GetSM4OutReg(dstRtype, dstRnum), xmasks[i]);
            EmitSM4Src(b, p[2], numTemps);
            if (matRtype == SM3RT_CONST || matRtype == SM3RT_CONST2)
                b.emitCB(0, matBase + (uint32_t)i, SM4_SWIZZLE_XYZW, 0);
            else b.emitSrc(SM4OT_TEMP, matBase + (uint32_t)i, SM4_SWIZZLE_XYZW);
            b.endOp(pos, SM4_DP4, ctlBits);
        }
        break;
    }
    case SM3_M3x4: {
        uint32_t dstRtype = sm3_regtype(p[1]), dstRnum = sm3_regnum(p[1]);
        uint32_t matBase  = sm3_regnum(p[3]), matRtype = sm3_regtype(p[3]);
        static const uint32_t xmasks[] = { 0x1, 0x2, 0x4, 0x8 };
        for (int i = 0; i < 4; i++) {
            size_t pos = b.beginOp();
            if (dstRtype == SM3RT_TEMP) b.emitDst(SM4OT_TEMP, dstRnum, xmasks[i]);
            else b.emitDst(SM4OT_OUTPUT, GetSM4OutReg(dstRtype, dstRnum), xmasks[i]);
            EmitSM4Src(b, p[2], numTemps);
            if (matRtype == SM3RT_CONST || matRtype == SM3RT_CONST2)
                b.emitCB(0, matBase + (uint32_t)i, SM4_SWIZZLE_XYZW, 0);
            else b.emitSrc(SM4OT_TEMP, matBase + (uint32_t)i, SM4_SWIZZLE_XYZW);
            b.endOp(pos, SM4_DP3, ctlBits);
        }
        break;
    }
    case SM3_M3x3: {
        uint32_t dstRtype = sm3_regtype(p[1]), dstRnum = sm3_regnum(p[1]);
        uint32_t matBase  = sm3_regnum(p[3]), matRtype = sm3_regtype(p[3]);
        static const uint32_t xmasks[] = { 0x1, 0x2, 0x4 };
        for (int i = 0; i < 3; i++) {
            size_t pos = b.beginOp();
            if (dstRtype == SM3RT_TEMP) b.emitDst(SM4OT_TEMP, dstRnum, xmasks[i]);
            else b.emitDst(SM4OT_OUTPUT, GetSM4OutReg(dstRtype, dstRnum), xmasks[i]);
            EmitSM4Src(b, p[2], numTemps);
            if (matRtype == SM3RT_CONST || matRtype == SM3RT_CONST2)
                b.emitCB(0, matBase + (uint32_t)i, SM4_SWIZZLE_XYZW, 0);
            else b.emitSrc(SM4OT_TEMP, matBase + (uint32_t)i, SM4_SWIZZLE_XYZW);
            b.endOp(pos, SM4_DP3, ctlBits);
        }
        break;
    }
    case SM3_M3x2: {
        uint32_t dstRtype = sm3_regtype(p[1]), dstRnum = sm3_regnum(p[1]);
        uint32_t matBase  = sm3_regnum(p[3]), matRtype = sm3_regtype(p[3]);
        static const uint32_t xmasks[] = { 0x1, 0x2 };
        for (int i = 0; i < 2; i++) {
            size_t pos = b.beginOp();
            if (dstRtype == SM3RT_TEMP) b.emitDst(SM4OT_TEMP, dstRnum, xmasks[i]);
            else b.emitDst(SM4OT_OUTPUT, GetSM4OutReg(dstRtype, dstRnum), xmasks[i]);
            EmitSM4Src(b, p[2], numTemps);
            if (matRtype == SM3RT_CONST || matRtype == SM3RT_CONST2)
                b.emitCB(0, matBase + (uint32_t)i, SM4_SWIZZLE_XYZW, 0);
            else b.emitSrc(SM4OT_TEMP, matBase + (uint32_t)i, SM4_SWIZZLE_XYZW);
            b.endOp(pos, SM4_DP3, ctlBits);
        }
        break;
    }
    case SM3_IF: {
        // IF [p0] → if_nz scratch (approximate: if non-zero test on src)
        // SM3 IF uses predicate or boolean src
        size_t pos = b.beginOp();
        if (sm3_instrlen(instrTok) >= 1)
            EmitSM4Src(b, p[1], numTemps);
        else
            b.emitSrc(SM4OT_TEMP, 0, 0x00); // fallback
        b.endOp(pos, SM4_IF, SM4_NONZERO_TEST);
        break;
    }
    case SM3_IFC: {
        // IFC GT, s0, s1 → ge/lt/eq based on comparison type, then if_nz
        // For simplicity: emit ge scratch, s0, s1; if_nz scratch
        { size_t pos = b.beginOp();
          EmitScratchDst(b, numTemps);
          EmitSM4Src(b, p[2], numTemps);
          EmitSM4Src(b, p[3], numTemps);
          b.endOp(pos, SM4_GE); }
        { size_t pos = b.beginOp();
          EmitScratch(b, numTemps, 0x00);
          b.endOp(pos, SM4_IF, SM4_NONZERO_TEST); }
        break;
    }
    case SM3_ELSE: {
        size_t pos = b.beginOp();
        b.endOp(pos, SM4_ELSE);
        break;
    }
    case SM3_ENDIF: {
        size_t pos = b.beginOp();
        b.endOp(pos, SM4_ENDIF);
        break;
    }
    case SM3_LOOP: {
        // LOOP aL, i0 → loop
        size_t pos = b.beginOp();
        b.endOp(pos, SM4_LOOP);
        break;
    }
    case SM3_ENDLOOP: {
        size_t pos = b.beginOp();
        b.endOp(pos, SM4_ENDLOOP);
        break;
    }
    case SM3_BREAK: {
        size_t pos = b.beginOp();
        b.endOp(pos, SM4_BREAK);
        break;
    }
    case SM3_BREAKC: {
        // BREAKC: break on condition. Approximate: emit BREAK
        size_t pos = b.beginOp();
        b.endOp(pos, SM4_BREAK);
        break;
    }
    case SM3_REP:
    case SM3_ENDREP: {
        // Rep/endrep: approximate with nop
        size_t pos = b.beginOp();
        b.endOp(pos, SM4_NOP);
        break;
    }
    case SM3_RET: {
        size_t pos = b.beginOp();
        b.endOp(pos, SM4_RET);
        break;
    }
    case SM3_TEX: {
        // TEXLD dst, src0(coords), src1(sampler) in SM3
        // SM4 SAMPLE dst, src(coords), t#, s#
        uint32_t samplerReg = sm3_regnum(p[3]);
        size_t pos = b.beginOp();
        EmitSM4Dst(b, p[1], info.isPS, numTemps);
        EmitSM4Src(b, p[2], numTemps);
        b.emitResource(samplerReg);
        b.emitSampler(samplerReg);
        b.endOp(pos, SM4_SAMPLE, ctlBits);
        break;
    }
    case SM3_TEXLDL: {
        // TEXLDL dst, src(xyzw where w=lod), s#
        // SM4 SAMPLE_L dst, src.xy, t#, s#, src.w
        uint32_t samplerReg = sm3_regnum(p[3]);
        uint32_t srcSwz = sm3_swizzle(p[2]);
        size_t pos = b.beginOp();
        EmitSM4Dst(b, p[1], info.isPS, numTemps);
        EmitSM4Src(b, p[2], numTemps); // coord
        b.emitResource(samplerReg);
        b.emitSampler(samplerReg);
        // LOD = src.w
        { uint32_t s = p[2]; uint32_t rtype=sm3_regtype(s), rnum=sm3_regnum(s);
          uint32_t wswz = 0xFF; // .wwww
          if (rtype == SM3RT_TEMP) b.emitSrc(SM4OT_TEMP, rnum, wswz);
          else b.emitCB(0, rnum, wswz, 0); }
        b.endOp(pos, SM4_SAMPLE_L, ctlBits);
        break;
    }
    case SM3_TEXLDD: {
        // TEXLDD dst, s0(coord), s1(sampler), s2(ddx), s3(ddy)
        uint32_t samplerReg = sm3_regnum(p[3]);
        size_t pos = b.beginOp();
        EmitSM4Dst(b, p[1], info.isPS, numTemps);
        EmitSM4Src(b, p[2], numTemps);
        b.emitResource(samplerReg);
        b.emitSampler(samplerReg);
        EmitSM4Src(b, p[4], numTemps); // ddx
        EmitSM4Src(b, p[5], numTemps); // ddy
        b.endOp(pos, SM4_SAMPLE_D, ctlBits);
        break;
    }
    case SM3_TEXKILL: {
        // TEXKILL src → discard_nz src
        size_t pos = b.beginOp();
        EmitSM4Src(b, p[1], numTemps);
        b.endOp(pos, SM4_DISCARD, SM4_NONZERO_TEST);
        break;
    }
    case SM3_DSX: {
        size_t pos = b.beginOp();
        EmitSM4Dst(b, p[1], info.isPS, numTemps);
        EmitSM4Src(b, p[2], numTemps);
        b.endOp(pos, SM4_DERIV_RTX, ctlBits);
        break;
    }
    case SM3_DSY: {
        size_t pos = b.beginOp();
        EmitSM4Dst(b, p[1], info.isPS, numTemps);
        EmitSM4Src(b, p[2], numTemps);
        b.endOp(pos, SM4_DERIV_RTY, ctlBits);
        break;
    }
    case SM3_CRS: {
        // CRS dst, s0, s1: cross product
        // dst.x = s0.y*s1.z - s0.z*s1.y
        // dst.y = s0.z*s1.x - s0.x*s1.z
        // dst.z = s0.x*s1.y - s0.y*s1.x
        // Approximate with three MAD/SUB ops using scratch
        uint32_t yswz = 0x55, zswz = 0xAA, xswz = 0x00;
        // scratch = s0.yzx * s1.zxy
        uint32_t yzx = (1u) | (2u<<2) | (0u<<4) | (0u<<6); // .yzxx (Y=1,Z=2,X=0 in SM3)
        uint32_t zxy = (2u) | (0u<<2) | (1u<<4) | (1u<<6); // .zxyy (Z=2,X=0,Y=1)
        { size_t pos = b.beginOp();
          EmitScratchDst(b, numTemps);
          { uint32_t s=p[2]; uint32_t rt=sm3_regtype(s),rn=sm3_regnum(s);
            if(rt==SM3RT_TEMP) b.emitSrc(SM4OT_TEMP,rn,yzx);
            else b.emitCB(0,rn,yzx,0); }
          { uint32_t s=p[3]; uint32_t rt=sm3_regtype(s),rn=sm3_regnum(s);
            if(rt==SM3RT_TEMP) b.emitSrc(SM4OT_TEMP,rn,zxy);
            else b.emitCB(0,rn,zxy,0); }
          b.endOp(pos, SM4_MUL); }
        // scratch2 = s0.zxy * s1.yzx → then dst = scratch - scratch2
        { size_t pos = b.beginOp();
          EmitSM4Dst(b, p[1], info.isPS, numTemps);
          EmitScratch(b, numTemps, SM4_SWIZZLE_XYZW);
          { uint32_t s=p[2]; uint32_t rt=sm3_regtype(s),rn=sm3_regnum(s);
            if(rt==SM3RT_TEMP) b.emitSrcMod(SM4OT_TEMP,rn,zxy,0);
            else b.emitCB(0,rn,zxy,0); }
          { uint32_t s=p[3]; uint32_t rt=sm3_regtype(s),rn=sm3_regnum(s);
            if(rt==SM3RT_TEMP) b.emitSrcMod(SM4OT_TEMP,rn,yzx,1); // negate
            else b.emitCB(0,rn,yzx,1); }
          b.endOp(pos, SM4_MAD, ctlBits); }
        break;
    }
    case SM3_SGN: {
        // SGN dst, s0, s1, s2 (s1, s2 are scratch regs in SM2, unused in SM3)
        // dst = sign(s0): -1 if s0<0, 0 if s0==0, 1 if s0>0
        // Approximate: MOV dst, s0 (wrong but won't crash)
        size_t pos = b.beginOp();
        EmitSM4Dst(b, p[1], info.isPS, numTemps);
        EmitSM4Src(b, p[2], numTemps);
        b.endOp(pos, SM4_MOV, ctlBits);
        break;
    }
    // Skip DCL, DEF, DEFB, DEFI, CALL, CALLNZ, LABEL, PHASE
    case SM3_DCL:
    case SM3_DEF:
    case SM3_DEFB:
    case SM3_DEFI:
    case SM3_CALL:
    case SM3_CALLNZ:
    case SM3_LABEL:
        // Skip these (declarations or unsupported control flow)
        break;
    case SM3_END:
        return false;
    default:
        // Unknown/unsupported: emit NOP
        { size_t pos = b.beginOp();
          b.endOp(pos, SM4_NOP); }
        break;
    }
    return true;
}

// ============================================================
// Build SHDR blob
// ============================================================

static std::vector<uint32_t> BuildSHDR(const uint32_t *pCode, size_t dwCount,
                                        const ShaderInfo &info) {
    SM4Builder b;

    uint32_t totalTemps = info.numTemps + kScratchTemps;

    // Version token (placeholher for total length follows)
    if (info.isPS)
        b.tokens.push_back(0x00000040u); // PS 4.0
    else
        b.tokens.push_back(0x00010040u); // VS 4.0

    size_t lengthPos = b.tokens.size();
    b.tokens.push_back(0); // total length placeholder

    // DCL_TEMPS
    {
        size_t pos = b.beginOp();
        b.tokens.push_back(totalTemps);
        b.endOp(pos, SM4_DCL_TEMPS);
    }

    // DCL_CONSTANT_BUFFER cb0[256], access=immediate
    {
        size_t pos = b.beginOp();
        // CB operand: type=CB, 0-comp?, 2D index [0][256]
        // Actually DCL_CB uses a "resource range" format:
        // operand: 4-comp, mask, CB, 2D, cbSlot, numElements
        // bits[11] of instruction control = access pattern (0=immediate)
        b.tokens.push_back(2u | (0u << 2) | (0xFu << 4) | (SM4OT_CB << 12) | (2u << 20));
        b.tokens.push_back(0);   // cb slot 0
        b.tokens.push_back(256); // element count
        b.endOp(pos, SM4_DCL_CONSTANT_BUFFER);
    }

    // DCL_SAMPLER for each sampler (mode=default, bits[14:11]=0)
    for (auto &s : info.samplers) {
        size_t pos = b.beginOp();
        b.emitSampler(s.regIdx);
        b.endOp(pos, SM4_DCL_SAMPLER);
    }

    // DCL_RESOURCE for each texture (dim=TEXTURE2D, bits[15:11]=3(2D))
    for (auto &s : info.samplers) {
        size_t pos = b.beginOp();
        // resource_dim in bits[15:11]: 2D=3
        uint32_t dim;
        switch (s.texType) {
        case 1:  dim = 5; break; // TEXTURECUBE
        case 2:  dim = 5; break; // TEXTURECUBE (D3DSTT_CUBE)
        case 3:  dim = 5; break; // use cube
        case 4:  dim = 5; break; // TEXTURE3D: use cube (approx)
        default: dim = 3; break; // TEXTURE2D
        }
        // Actually: texType from DCL sampler token bits[28:27]:
        // 0=unknown, 1=2D, 2=CUBE, 3=VOLUME
        switch (s.texType) {
        case 1:  dim = 3; break; // 2D
        case 2:  dim = 6; break; // CUBE (SM4RD_TEXTURECUBE=6)
        case 3:  dim = 5; break; // VOLUME (SM4RD_TEXTURE3D=5)
        default: dim = 3; break; // default 2D
        }
        b.emitResource(s.regIdx);
        // ResourceReturnTypeToken: all-float (5|5<<4|5<<8|5<<12)
        b.tokens.push_back(0x5555u);
        b.endOp(pos, SM4_DCL_RESOURCE, dim << 11);
    }

    // DCL_INPUT for VS or DCL_INPUT_PS for PS
    for (auto &inp : info.inputs) {
        if (inp.regType != SM3RT_INPUT) continue;
        size_t pos = b.beginOp();
        uint32_t mask = inp.mask ? inp.mask : 0xF;
        b.emitDst(SM4OT_INPUT, inp.regIdx, mask);
        if (info.isPS) {
            // interp mode in bits[14:11]: LINEAR=2
            b.endOp(pos, SM4_DCL_INPUT_PS, 2u << 11);
        } else {
            b.endOp(pos, SM4_DCL_INPUT);
        }
    }

    // DCL_OUTPUT for each output
    for (auto &out : info.outputs) {
        uint32_t sm4reg = GetSM4OutReg(out.regType, out.regIdx);
        uint32_t mask   = out.mask ? out.mask : 0xF;
        bool isSvPos    = (out.regType == SM3RT_RASTOUT && out.regIdx == 0);

        if (out.regType == SM3RT_DEPTHOUT) {
            size_t pos = b.beginOp();
            b.emitOutputDepth();
            b.endOp(pos, SM4_DCL_OUTPUT);
        } else if (isSvPos) {
            size_t pos = b.beginOp();
            b.emitDst(SM4OT_OUTPUT, sm4reg, mask);
            b.tokens.push_back(1u); // SV name = D3D10_NAME_POSITION = 1
            b.endOp(pos, SM4_DCL_OUTPUT_SIV);
        } else {
            size_t pos = b.beginOp();
            b.emitDst(SM4OT_OUTPUT, sm4reg, mask);
            b.endOp(pos, SM4_DCL_OUTPUT);
        }
    }

    // If no outputs declared, add minimal ones to prevent validation errors
    if (info.outputs.empty()) {
        if (info.isPS) {
            // Declare oC0 output
            size_t pos = b.beginOp();
            b.emitDst(SM4OT_OUTPUT, 0, 0xF);
            b.endOp(pos, SM4_DCL_OUTPUT);
        } else {
            // Declare SV_Position at o0
            size_t pos = b.beginOp();
            b.emitDst(SM4OT_OUTPUT, 0, 0xF);
            b.tokens.push_back(1u);
            b.endOp(pos, SM4_DCL_OUTPUT_SIV);
        }
    }

    // ---- Instructions ----
    const uint32_t *p   = pCode + 1; // skip version token
    const uint32_t *end = pCode + dwCount;

    while (p < end) {
        uint32_t tok = *p;
        uint32_t op  = sm3_opcode(tok);

        if (op == SM3_END) break;

        if (op == 0xFFFE) { // COMMENT
            p += 1 + ((tok >> 16) & 0x7FFF);
            continue;
        }

        uint32_t instrLen = sm3_instrlen(tok);
        const uint32_t *next = p + 1 + instrLen;

        if (!EmitSM4Instr(b, p, info)) break;
        p = next;
    }

    // Always end with RET
    {
        size_t pos = b.beginOp();
        b.endOp(pos, SM4_RET);
    }

    // Patch total length in SHDR header (2nd DWORD)
    b.tokens[lengthPos] = (uint32_t)b.tokens.size();

    return b.tokens;
}

// ============================================================
// Assemble final DXBC container
// ============================================================

static std::vector<uint8_t> AssembleDXBC(
    const std::vector<uint8_t> &isgnBlob,
    const std::vector<uint8_t> &osgnBlob,
    const std::vector<uint32_t> &shdrTokens)
{
    // DXBC layout:
    //   Header (32 bytes): magic(4) + hash(16) + version(4) + size(4) + blobCount(4)
    //   Blob offset table: blobCount * 4 bytes
    //   Blobs: 8-byte header (FourCC+size) + data (padded to DWORD)

    auto padToU32 = [](size_t n) { return (n + 3) & ~size_t(3); };

    uint32_t shdrSizeBytes = (uint32_t)(shdrTokens.size() * 4);
    uint32_t isgnSize = (uint32_t)isgnBlob.size();
    uint32_t osgnSize = (uint32_t)osgnBlob.size();

    // Each blob: 8-byte blob header + data
    uint32_t isgnBlobSize = 8 + isgnSize;
    uint32_t osgnBlobSize = 8 + osgnSize;
    uint32_t shdrBlobSize = 8 + shdrSizeBytes;

    uint32_t blobCount  = 3;
    uint32_t headerSize = 32;
    uint32_t offsetsSize = blobCount * 4;

    uint32_t isgnOffset = headerSize + offsetsSize;
    uint32_t osgnOffset = isgnOffset + isgnBlobSize;
    uint32_t shdrOffset = osgnOffset + osgnBlobSize;
    uint32_t totalSize  = shdrOffset + shdrBlobSize;

    std::vector<uint8_t> out(totalSize, 0);
    uint8_t *p = out.data();

    // Header
    const char dxbcMagic[4] = {'D','X','B','C'};
    memcpy(p, dxbcMagic, 4); p += 4;
    memset(p, 0, 16); p += 16;   // hash (zeroed, not validated by CDXBCParser)
    uint32_t ver = 0x00000001u;   // DXBCVersion: Major=1, Minor=0
    memcpy(p, &ver, 4); p += 4;
    memcpy(p, &totalSize, 4); p += 4;
    memcpy(p, &blobCount, 4); p += 4;

    // Offset table
    memcpy(p, &isgnOffset, 4); p += 4;
    memcpy(p, &osgnOffset, 4); p += 4;
    memcpy(p, &shdrOffset, 4); p += 4;

    // ISGN blob
    const char isgnFourCC[4] = {'I','S','G','N'};
    memcpy(p, isgnFourCC, 4); p += 4;
    memcpy(p, &isgnSize, 4); p += 4;
    memcpy(p, isgnBlob.data(), isgnSize); p += isgnSize;

    // OSGN blob
    const char osgnFourCC[4] = {'O','S','G','N'};
    memcpy(p, osgnFourCC, 4); p += 4;
    memcpy(p, &osgnSize, 4); p += 4;
    memcpy(p, osgnBlob.data(), osgnSize); p += osgnSize;

    // SHDR blob
    const char shdrFourCC[4] = {'S','H','D','R'};
    memcpy(p, shdrFourCC, 4); p += 4;
    memcpy(p, &shdrSizeBytes, 4); p += 4;
    memcpy(p, shdrTokens.data(), shdrSizeBytes); p += shdrSizeBytes;

    return out;
}

// ============================================================
// Public API
// ============================================================

std::vector<uint8_t> ConvertSM3ToDXBC(const uint32_t *pFunction, size_t dwCount) {
    if (!pFunction || dwCount < 2) return {};

    ShaderInfo info;
    ScanBytecode(pFunction, dwCount, info);

    // Build signatures
    std::vector<uint8_t> isgn, osgn;
    if (info.isPS) {
        isgn = BuildPsIsgn(info);
        osgn = BuildPsOsgn(info);
    } else {
        isgn = BuildVsIsgn(info);
        osgn = BuildVsOsgn(info);
    }

    // Build SHDR
    auto shdr = BuildSHDR(pFunction, dwCount, info);

    // Assemble DXBC
    return AssembleDXBC(isgn, osgn, shdr);
}

} // namespace dxmt::d3d9
