// Xenos microcode translator. See shader_translator.h.
#include "shader_translator.h"

#include "xenia/gpu/ucode.h"
#include "xenia/gpu/xenos.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

using namespace xe::gpu;

namespace xlat
{
    namespace
    {
        class Writer
        {
        public:
            void Line(const char* fmt, ...)
            {
                char buf[1024];
                va_list ap;
                va_start(ap, fmt);
                vsnprintf(buf, sizeof(buf), fmt, ap);
                va_end(ap);
                out.append(size_t(indent) * 2, ' ');
                out += buf;
                out += '\n';
            }
            std::string out;
            int indent = 1;
        };

        const char kComp[4] = { 'x', 'y', 'z', 'w' };

        std::string Float(float f)
        {
            if (f == 0.0f) return "0.0";
            char buf[48];
            // Exact round trip through the bit pattern.
            uint32_t bits;
            memcpy(&bits, &f, 4);
            snprintf(buf, sizeof(buf), "asfloat(0x%08Xu)", bits);
            return buf;
        }

        class Translator
        {
        public:
            Translator(ShaderType type, const uint32_t* ucode, uint32_t dwords) : m_type(type), m_ucode(ucode), m_dwords(dwords) {}

            TranslatedShader Run()
            {
                TranslatedShader result;
                if (!m_ucode || m_dwords < 3)
                {
                    result.error = "empty shader";
                    return result;
                }

                // Control flow instructions sit at the start of the program,
                // before the first exec clause's instructions.
                uint32_t cfLimit = (m_dwords / 3) * 2;
                std::vector<ucode::ControlFlowInstruction> cf;
                for (uint32_t i = 0; i < cfLimit; i++)
                {
                    ucode::ControlFlowInstruction ins = ReadCf(i);
                    if (ucode::IsControlFlowOpcodeExec(ins.opcode()))
                    {
                        uint32_t addr = ins.exec.address();
                        if (ins.exec.count())
                            cfLimit = std::min(cfLimit, addr * 2);
                    }
                    cf.push_back(ins);
                }

                // fxc fails ("l-value expected") if the switch selector is
                // assigned inside the switch, so the next address goes to xe_pn.
                w.Line("uint xe_pc = 0u;");
                w.Line("uint xe_pn = 0u;");
                w.Line("bool xe_end = false;");
                w.Line("XE_LOOP while (!xe_end)");
                w.Line("{");
                w.indent++;
                w.Line("switch (xe_pc)");
                w.Line("{");
                for (uint32_t i = 0; i < cf.size(); i++)
                {
                    w.Line("case %uu:", i);
                    w.Line("{");
                    w.indent++;
                    EmitCf(cf[i], i);
                    w.indent--;
                    w.Line("}");
                    w.Line("break;");
                }
                w.Line("default:");
                w.Line("  xe_end = true;");
                w.Line("  break;");
                w.Line("}");
                w.Line("xe_pc = xe_pn;");
                w.indent--;
                w.Line("}");

                result.body = std::move(w.out);
                result.ok = m_error.empty();
                result.error = m_error;
                result.textureFetchMask = m_textureMask;
                memcpy(result.textureDimension, m_textureDim, sizeof(m_textureDim));
                memcpy(result.vertexFetchMask, m_vertexMask, sizeof(m_vertexMask));
                result.exportMask = m_exportMask;
                result.writesPointSize = m_writesPointSize;
                result.writesDepth = m_writesDepth;
                result.usesKill = m_usesKill;
                result.floatConstCount = m_floatConsts;
                result.tempCount = m_usesRelativeTemps ? 64 : std::max<uint32_t>(m_maxTemp + 1, 1);
                return result;
            }

        private:
            ucode::ControlFlowInstruction ReadCf(uint32_t index) const
            {
                const uint32_t* pair = &m_ucode[3 * (index >> 1)];
                ucode::ControlFlowInstruction ins;
                if (index & 1)
                {
                    ins.dword_0 = (pair[1] >> 16) | (pair[2] << 16);
                    ins.dword_1 = pair[2] >> 16;
                }
                else
                {
                    ins.dword_0 = pair[0];
                    ins.dword_1 = pair[1] & 0xFFFF;
                }
                return ins;
            }

            static std::string BoolCond(uint32_t boolAddress, bool condition)
            {
                char buf[64];
                snprintf(buf, sizeof(buf), "(xe_B(%uu) == %s)", boolAddress, condition ? "true" : "false");
                return buf;
            }

            void EmitCf(const ucode::ControlFlowInstruction& ins, uint32_t index)
            {
                using Op = ucode::ControlFlowOpcode;
                Op op = ins.opcode();
                switch (op)
                {
                case Op::kExec:
                case Op::kExecEnd:
                case Op::kCondExec:
                case Op::kCondExecEnd:
                case Op::kCondExecPred:
                case Op::kCondExecPredEnd:
                case Op::kCondExecPredClean:
                case Op::kCondExecPredCleanEnd:
                {
                    std::string cond;
                    if (op == Op::kCondExec || op == Op::kCondExecEnd || op == Op::kCondExecPredClean || op == Op::kCondExecPredCleanEnd)
                        cond = BoolCond(ins.cond_exec.bool_address(), ins.cond_exec.condition());
                    else if (op == Op::kCondExecPred || op == Op::kCondExecPredEnd)
                        cond = ins.cond_exec_pred.condition() ? "(xe_p)" : "(!xe_p)";
                    bool ends = ucode::DoesControlFlowOpcodeEndShader(op);
                    w.Line("xe_pn = %uu;", index + 1);
                    if (!cond.empty())
                    {
                        w.Line("if %s", cond.c_str());
                        w.Line("{");
                        w.indent++;
                    }
                    EmitExecBody(ins.exec);
                    if (ends)
                        w.Line("xe_end = true;");
                    if (!cond.empty())
                    {
                        w.indent--;
                        w.Line("}");
                    }
                    break;
                }
                case Op::kLoopStart:
                {
                    const auto& ls = ins.loop_start;
                    w.Line("xe_pn = %uu;", index + 1);
                    w.Line("if (xe_ld >= 4) { xe_pn = %uu; }", ls.address());
                    w.Line("else");
                    w.Line("{");
                    w.indent++;
                    w.Line("uint xe_lv = xe_L(%uu);", ls.loop_id());
                    w.Line("xe_lc[xe_ld] = xe_lv & 0xFFu;");
                    w.Line("xe_lstart[xe_ld] = int((xe_lv >> 8) & 0xFFu);");
                    w.Line("xe_lstep[xe_ld] = int(xe_lv << 8) >> 24;");
                    if (!ls.is_repeat())
                        w.Line("xe_li[xe_ld] = 0u;");
                    w.Line("if (xe_li[xe_ld] >= xe_lc[xe_ld]) { xe_pn = %uu; }", ls.address());
                    w.Line("else { xe_ld = xe_ld + 1; }");
                    w.indent--;
                    w.Line("}");
                    break;
                }
                case Op::kLoopEnd:
                {
                    const auto& le = ins.loop_end;
                    w.Line("xe_pn = %uu;", index + 1);
                    w.Line("if (xe_ld > 0)");
                    w.Line("{");
                    w.indent++;
                    w.Line("xe_li[xe_ld - 1] = xe_li[xe_ld - 1] + 1u;");
                    if (le.is_predicated_break())
                        w.Line("if (xe_li[xe_ld - 1] < xe_lc[xe_ld - 1] && xe_p != %s) { xe_pn = %uu; }", le.condition() ? "true" : "false", le.address());
                    else
                        w.Line("if (xe_li[xe_ld - 1] < xe_lc[xe_ld - 1]) { xe_pn = %uu; }", le.address());
                    w.Line("else { xe_ld = xe_ld - 1; }");
                    w.indent--;
                    w.Line("}");
                    break;
                }
                case Op::kCondCall:
                {
                    const auto& cc = ins.cond_call;
                    w.Line("xe_pn = %uu;", index + 1);
                    std::string cond = "(xe_cd < 4)";
                    if (!cc.is_unconditional())
                        cond += cc.is_predicated() ? (cc.condition() ? " && (xe_p)" : " && (!xe_p)")
                                                   : " && " + BoolCond(cc.bool_address(), cc.condition());
                    w.Line("if (%s) { xe_cr[xe_cd] = %uu; xe_cd = xe_cd + 1; xe_pn = %uu; }", cond.c_str(), index + 1, cc.address());
                    break;
                }
                case Op::kReturn:
                    w.Line("xe_pn = %uu;", index + 1);
                    w.Line("if (xe_cd > 0) { xe_cd = xe_cd - 1; xe_pn = xe_cr[xe_cd]; }");
                    break;
                case Op::kCondJmp:
                {
                    const auto& cj = ins.cond_jmp;
                    w.Line("xe_pn = %uu;", index + 1);
                    if (cj.is_unconditional())
                        w.Line("xe_pn = %uu;", cj.address());
                    else
                    {
                        std::string cond = cj.is_predicated() ? (cj.condition() ? "(xe_p)" : "(!xe_p)") : BoolCond(cj.bool_address(), cj.condition());
                        w.Line("if %s { xe_pn = %uu; }", cond.c_str(), cj.address());
                    }
                    break;
                }
                default:
                    // nop, alloc, mark_vs_fetch_done
                    w.Line("xe_pn = %uu;", index + 1);
                    break;
                }
            }

            void EmitExecBody(const ucode::ControlFlowExecInstruction& exec)
            {
                for (uint32_t i = 0; i < exec.count(); i++)
                {
                    uint32_t slot = exec.address() + i;
                    if (3 * slot + 2 >= m_dwords)
                    {
                        m_error = "instruction outside the shader";
                        return;
                    }
                    const uint32_t* words = &m_ucode[3 * slot];
                    if ((exec.sequence() >> (i * 2)) & 1)
                    {
                        const auto& fetch = *reinterpret_cast<const ucode::FetchInstruction*>(words);
                        bool pred = fetch.is_predicated();
                        if (pred)
                        {
                            w.Line("if (%sxe_p)", fetch.predicate_condition() ? "" : "!");
                            w.Line("{");
                            w.indent++;
                        }
                        if (fetch.opcode() == ucode::FetchOpcode::kVertexFetch)
                            EmitVertexFetch(*reinterpret_cast<const ucode::VertexFetchInstruction*>(words));
                        else if (fetch.opcode() == ucode::FetchOpcode::kTextureFetch)
                            EmitTextureFetch(*reinterpret_cast<const ucode::TextureFetchInstruction*>(words));
                        else
                            StoreFetch(fetch.dest(), fetch.is_dest_relative(), fetch.dest_swizzle(), "float4(0.0, 0.0, 0.0, 0.0)");
                        if (pred)
                        {
                            w.indent--;
                            w.Line("}");
                        }
                    }
                    else
                    {
                        const auto& alu = *reinterpret_cast<const ucode::AluInstruction*>(words);
                        bool pred = alu.is_predicated();
                        if (pred)
                        {
                            w.Line("if (%sxe_p)", alu.predicate_condition() ? "" : "!");
                            w.Line("{");
                            w.indent++;
                        }
                        EmitAlu(alu);
                        if (pred)
                        {
                            w.indent--;
                            w.Line("}");
                        }
                    }
                }
            }

            // --- Registers ---------------------------------------------------
            std::string Temp(uint32_t reg, bool relative)
            {
                char buf[64];
                if (relative)
                {
                    m_usesRelativeTemps = true;
                    snprintf(buf, sizeof(buf), "r[(%u + xe_aL) & 63]", reg);
                }
                else
                {
                    reg &= 63;
                    m_maxTemp = std::max(m_maxTemp, reg);
                    snprintf(buf, sizeof(buf), "r[%u]", reg);
                }
                return buf;
            }

            std::string Const(uint32_t reg, bool addressed, bool a0)
            {
                char buf[64];
                m_floatConsts = std::max<uint32_t>(m_floatConsts, addressed ? 256u : std::min<uint32_t>(reg + 1, 256u));
                if (addressed)
                    snprintf(buf, sizeof(buf), "xe_C(%d + %s)", int(reg), a0 ? "xe_a0" : "xe_aL");
                else
                    snprintf(buf, sizeof(buf), "xe_C(%d)", int(reg));
                return buf;
            }

            // Loads a vector ALU operand (1-based index) with swizzle/abs/negate.
            std::string VectorOperand(const ucode::AluInstruction& alu, uint32_t i)
            {
                uint32_t reg = alu.src_reg(i);
                std::string base;
                bool absolute = false;
                if (alu.src_is_temp(i))
                {
                    base = Temp(ucode::AluInstruction::src_temp_reg(reg), ucode::AluInstruction::is_src_temp_relative(reg));
                    absolute = ucode::AluInstruction::is_src_temp_value_absolute(reg);
                }
                else
                    base = Const(reg, alu.src_const_is_addressed(i), alu.is_const_address_register_relative());
                uint32_t sw = alu.src_swizzle(i);
                char buf[160];
                snprintf(buf, sizeof(buf), "xe_swz(%s, %u, %u, %u, %u)", base.c_str(),
                         ucode::AluInstruction::GetSwizzledComponentIndex(sw, 0), ucode::AluInstruction::GetSwizzledComponentIndex(sw, 1),
                         ucode::AluInstruction::GetSwizzledComponentIndex(sw, 2), ucode::AluInstruction::GetSwizzledComponentIndex(sw, 3));
                std::string v = buf;
                if (absolute) v = "abs(" + v + ")";
                if (alu.src_negate(i)) v = "-(" + v + ")";
                return v;
            }

            void EmitAlu(const ucode::AluInstruction& alu)
            {
                using V = ucode::AluVectorOpcode;
                using S = ucode::AluScalarOpcode;
                w.Line("{");
                w.indent++;

                V vop = alu.vector_opcode();
                const auto& vinfo = ucode::GetAluVectorOpcodeInfo(vop);
                uint32_t vmask = alu.GetVectorOpResultWriteMask();
                w.Line("float4 xe_vr = float4(0.0, 0.0, 0.0, 0.0);");
                if (vmask || vinfo.changed_state)
                {
                    for (uint32_t i = 0; i < 3; i++)
                        if (vinfo.operand_components_used[i])
                            w.Line("float4 xe_v%u = %s;", i, VectorOperand(alu, i + 1).c_str());
                    const char* expr = nullptr;
                    switch (vop)
                    {
                    case V::kAdd: expr = "xe_v0 + xe_v1"; break;
                    case V::kMul: expr = "xe_mul(xe_v0, xe_v1)"; break;
                    case V::kMax: expr = "xe_max(xe_v0, xe_v1)"; break;
                    case V::kMin: expr = "xe_min(xe_v0, xe_v1)"; break;
                    case V::kSeq: expr = "xe_seq(xe_v0, xe_v1)"; break;
                    case V::kSgt: expr = "xe_sgt(xe_v0, xe_v1)"; break;
                    case V::kSge: expr = "xe_sge(xe_v0, xe_v1)"; break;
                    case V::kSne: expr = "xe_sne(xe_v0, xe_v1)"; break;
                    case V::kFrc: expr = "xe_v0 - floor(xe_v0)"; break;
                    case V::kTrunc: expr = "trunc(xe_v0)"; break;
                    case V::kFloor: expr = "floor(xe_v0)"; break;
                    case V::kMad: expr = "xe_mul(xe_v0, xe_v1) + xe_v2"; break;
                    case V::kCndEq: expr = "xe_cndeq(xe_v0, xe_v1, xe_v2)"; break;
                    case V::kCndGe: expr = "xe_cndge(xe_v0, xe_v1, xe_v2)"; break;
                    case V::kCndGt: expr = "xe_cndgt(xe_v0, xe_v1, xe_v2)"; break;
                    case V::kDp4: expr = "xe_rep(xe_dp4(xe_v0, xe_v1))"; break;
                    case V::kDp3: expr = "xe_rep(xe_dp3(xe_v0, xe_v1))"; break;
                    case V::kDp2Add: expr = "xe_rep(xe_dp2(xe_v0, xe_v1) + xe_v2.x)"; break;
                    case V::kCube: expr = "xe_cube(xe_v0)"; break;
                    case V::kMax4: expr = "xe_rep(xe_max4(xe_v0))"; break;
                    case V::kSetpEqPush:
                        w.Line("xe_p = xe_v0.w == 0.0 && xe_v1.w == 0.0;");
                        expr = "xe_rep((xe_v0.x == 0.0 && xe_v1.x == 0.0) ? 0.0 : xe_v0.x + 1.0)";
                        break;
                    case V::kSetpNePush:
                        w.Line("xe_p = xe_v0.w == 0.0 && xe_v1.w != 0.0;");
                        expr = "xe_rep((xe_v0.x == 0.0 && xe_v1.x != 0.0) ? 0.0 : xe_v0.x + 1.0)";
                        break;
                    case V::kSetpGtPush:
                        w.Line("xe_p = xe_v0.w == 0.0 && xe_v1.w > 0.0;");
                        expr = "xe_rep((xe_v0.x == 0.0 && xe_v1.x > 0.0) ? 0.0 : xe_v0.x + 1.0)";
                        break;
                    case V::kSetpGePush:
                        w.Line("xe_p = xe_v0.w == 0.0 && xe_v1.w >= 0.0;");
                        expr = "xe_rep((xe_v0.x == 0.0 && xe_v1.x >= 0.0) ? 0.0 : xe_v0.x + 1.0)";
                        break;
                    case V::kKillEq:
                        m_usesKill = true;
                        w.Line("float xe_k = float(xe_v0.x == xe_v1.x || xe_v0.y == xe_v1.y || xe_v0.z == xe_v1.z || xe_v0.w == xe_v1.w);");
                        w.Line("xe_kill = xe_kill || xe_k != 0.0;");
                        expr = "xe_rep(xe_k)";
                        break;
                    case V::kKillGt:
                        m_usesKill = true;
                        w.Line("float xe_k = float(xe_v0.x > xe_v1.x || xe_v0.y > xe_v1.y || xe_v0.z > xe_v1.z || xe_v0.w > xe_v1.w);");
                        w.Line("xe_kill = xe_kill || xe_k != 0.0;");
                        expr = "xe_rep(xe_k)";
                        break;
                    case V::kKillGe:
                        m_usesKill = true;
                        w.Line("float xe_k = float(xe_v0.x >= xe_v1.x || xe_v0.y >= xe_v1.y || xe_v0.z >= xe_v1.z || xe_v0.w >= xe_v1.w);");
                        w.Line("xe_kill = xe_kill || xe_k != 0.0;");
                        expr = "xe_rep(xe_k)";
                        break;
                    case V::kKillNe:
                        m_usesKill = true;
                        w.Line("float xe_k = float(xe_v0.x != xe_v1.x || xe_v0.y != xe_v1.y || xe_v0.z != xe_v1.z || xe_v0.w != xe_v1.w);");
                        w.Line("xe_kill = xe_kill || xe_k != 0.0;");
                        expr = "xe_rep(xe_k)";
                        break;
                    case V::kDst: expr = "float4(1.0, xe_mul1(xe_v0.y, xe_v1.y), xe_v0.z, xe_v1.w)"; break;
                    case V::kMaxA:
                        w.Line("xe_a0 = int(floor(clamp(xe_v0.w, -256.0, 255.0) + 0.5));");
                        expr = "xe_max(xe_v0, xe_v1)";
                        break;
                    default:
                        m_error = "unknown vector opcode";
                        expr = "float4(0.0, 0.0, 0.0, 0.0)";
                        break;
                    }
                    w.Line("xe_vr = %s;", expr);
                }

                // Scalar operation (operands are read after the vector op, which
                // may have changed a0).
                S sop = alu.scalar_opcode();
                const auto& sinfo = ucode::GetAluScalarOpcodeInfo(sop);
                uint32_t sw = alu.src_swizzle(3);
                bool negate = alu.src_negate(3);
                if (sinfo.operand_count == 1)
                {
                    uint32_t reg = alu.src_reg(3);
                    std::string base;
                    bool absolute = false;
                    if (alu.src_is_temp(3))
                    {
                        base = Temp(ucode::AluInstruction::src_temp_reg(reg), ucode::AluInstruction::is_src_temp_relative(reg));
                        absolute = ucode::AluInstruction::is_src_temp_value_absolute(reg);
                    }
                    else
                        base = Const(reg, alu.src_const_is_addressed(3), alu.is_const_address_register_relative());
                    w.Line("float4 xe_sv = %s;", base.c_str());
                    uint32_t count = sinfo.single_operand_is_two_component ? 2 : 1;
                    for (uint32_t i = 0; i < count; i++)
                    {
                        uint32_t c = ucode::AluInstruction::GetSwizzledComponentIndex(sw, (3 + i) & 3);
                        std::string v = "xe_flush(xe_sv." + std::string(1, kComp[c]) + ")";
                        if (absolute) v = "abs(" + v + ")";
                        if (negate) v = "-(" + v + ")";
                        w.Line("float xe_s%u = %s;", i, v.c_str());
                    }
                }
                else if (sinfo.operand_count == 2)
                {
                    std::string c = Const(alu.src_reg(3), alu.src_const_is_addressed(3), alu.is_const_address_register_relative());
                    std::string t = Temp(alu.scalar_const_reg_op_src_temp_reg(), false);
                    std::string v0 = "xe_flush(" + c + "." + kComp[ucode::AluInstruction::GetSwizzledComponentIndex(sw, 3)] + ")";
                    std::string v1 = "xe_flush(" + t + "." + kComp[ucode::AluInstruction::GetSwizzledComponentIndex(sw, 0)] + ")";
                    if (negate) { v0 = "-(" + v0 + ")"; v1 = "-(" + v1 + ")"; }
                    w.Line("float xe_s0 = %s;", v0.c_str());
                    w.Line("float xe_s1 = %s;", v1.c_str());
                }

                switch (sop)
                {
                case S::kAdds: case S::kAddsc0: case S::kAddsc1: w.Line("xe_ps = xe_s0 + xe_s1;"); break;
                case S::kAddsPrev: w.Line("xe_ps = xe_s0 + xe_ps;"); break;
                case S::kMuls: case S::kMulsc0: case S::kMulsc1: w.Line("xe_ps = xe_mul1(xe_s0, xe_s1);"); break;
                case S::kMulsPrev: w.Line("xe_ps = xe_mul1(xe_s0, xe_ps);"); break;
                case S::kMulsPrev2:
                    w.Line("if (xe_ps == -XE_FLT_MAX || !xe_isfinite(xe_ps) || !xe_isfinite(xe_s1) || xe_s1 <= 0.0) { xe_ps = -XE_FLT_MAX; }");
                    w.Line("else { xe_ps = xe_mul1(xe_s0, xe_ps); }");
                    break;
                case S::kMaxs: w.Line("xe_ps = xe_s0 >= xe_s1 ? xe_s0 : xe_s1;"); break;
                case S::kMins: w.Line("xe_ps = xe_s0 < xe_s1 ? xe_s0 : xe_s1;"); break;
                case S::kSeqs: w.Line("xe_ps = float(xe_s0 == 0.0);"); break;
                case S::kSgts: w.Line("xe_ps = float(xe_s0 > 0.0);"); break;
                case S::kSges: w.Line("xe_ps = float(xe_s0 >= 0.0);"); break;
                case S::kSnes: w.Line("xe_ps = float(xe_s0 != 0.0);"); break;
                case S::kFrcs: w.Line("xe_ps = xe_s0 - floor(xe_s0);"); break;
                case S::kTruncs: w.Line("xe_ps = trunc(xe_s0);"); break;
                case S::kFloors: w.Line("xe_ps = floor(xe_s0);"); break;
                case S::kExp: w.Line("xe_ps = exp2(xe_s0);"); break;
                case S::kLogc: w.Line("xe_ps = xe_logc(xe_s0);"); break;
                case S::kLog: w.Line("xe_ps = log2(xe_s0);"); break;
                case S::kRcpc: w.Line("xe_ps = xe_clampinf(1.0 / xe_s0);"); break;
                case S::kRcpf: w.Line("xe_ps = xe_zeroinf(1.0 / xe_s0);"); break;
                case S::kRcp: w.Line("xe_ps = 1.0 / xe_s0;"); break;
                case S::kRsqc: w.Line("xe_ps = xe_clampinf(1.0 / sqrt(xe_s0));"); break;
                case S::kRsqf: w.Line("xe_ps = xe_zeroinf(1.0 / sqrt(xe_s0));"); break;
                case S::kRsq: w.Line("xe_ps = 1.0 / sqrt(xe_s0);"); break;
                case S::kMaxAs:
                    w.Line("xe_a0 = int(floor(clamp(xe_s0, -256.0, 255.0) + 0.5));");
                    w.Line("xe_ps = xe_s0 >= xe_s1 ? xe_s0 : xe_s1;");
                    break;
                case S::kMaxAsf:
                    w.Line("xe_a0 = int(floor(clamp(xe_s0, -256.0, 255.0)));");
                    w.Line("xe_ps = xe_s0 >= xe_s1 ? xe_s0 : xe_s1;");
                    break;
                case S::kSubs: case S::kSubsc0: case S::kSubsc1: w.Line("xe_ps = xe_s0 - xe_s1;"); break;
                case S::kSubsPrev: w.Line("xe_ps = xe_s0 - xe_ps;"); break;
                case S::kSetpEq: w.Line("xe_p = xe_s0 == 0.0; xe_ps = float(!xe_p);"); break;
                case S::kSetpNe: w.Line("xe_p = xe_s0 != 0.0; xe_ps = float(!xe_p);"); break;
                case S::kSetpGt: w.Line("xe_p = xe_s0 > 0.0; xe_ps = float(!xe_p);"); break;
                case S::kSetpGe: w.Line("xe_p = xe_s0 >= 0.0; xe_ps = float(!xe_p);"); break;
                case S::kSetpInv: w.Line("xe_p = xe_s0 == 1.0; xe_ps = xe_p ? 0.0 : (xe_s0 == 0.0 ? 1.0 : xe_s0);"); break;
                case S::kSetpPop: w.Line("{ float xe_nc = xe_s0 - 1.0; xe_p = xe_nc <= 0.0; xe_ps = xe_p ? 0.0 : xe_nc; }"); break;
                case S::kSetpClr: w.Line("xe_p = false; xe_ps = XE_FLT_MAX;"); break;
                case S::kSetpRstr: w.Line("xe_p = xe_s0 == 0.0; xe_ps = xe_p ? 0.0 : xe_s0;"); break;
                case S::kKillsEq: m_usesKill = true; w.Line("xe_ps = float(xe_s0 == 0.0); xe_kill = xe_kill || xe_ps != 0.0;"); break;
                case S::kKillsGt: m_usesKill = true; w.Line("xe_ps = float(xe_s0 > 0.0); xe_kill = xe_kill || xe_ps != 0.0;"); break;
                case S::kKillsGe: m_usesKill = true; w.Line("xe_ps = float(xe_s0 >= 0.0); xe_kill = xe_kill || xe_ps != 0.0;"); break;
                case S::kKillsNe: m_usesKill = true; w.Line("xe_ps = float(xe_s0 != 0.0); xe_kill = xe_kill || xe_ps != 0.0;"); break;
                case S::kKillsOne: m_usesKill = true; w.Line("xe_ps = float(xe_s0 == 1.0); xe_kill = xe_kill || xe_ps != 0.0;"); break;
                case S::kSqrt: w.Line("xe_ps = sqrt(xe_s0);"); break;
                case S::kSin: w.Line("xe_ps = sin(xe_s0);"); break;
                case S::kCos: w.Line("xe_ps = cos(xe_s0);"); break;
                case S::kRetainPrev: break;
                default: m_error = "unknown scalar opcode"; break;
                }

                if (alu.vector_clamp())
                    w.Line("xe_vr = saturate(xe_vr);");
                w.Line("float xe_sr = %s;", alu.scalar_clamp() ? "saturate(xe_ps)" : "xe_ps");

                uint32_t smask = alu.GetScalarOpResultWriteMask();
                if (alu.is_export())
                {
                    uint32_t c1 = alu.GetConstant1WriteMask();
                    uint32_t mask = vmask | smask | alu.GetConstant0WriteMask() | c1;
                    std::string target = ExportTarget(alu.vector_dest(), mask);
                    if (!target.empty())
                        for (uint32_t i = 0; i < 4; i++)
                        {
                            uint32_t bit = 1u << i;
                            if (!(mask & bit)) continue;
                            const char* value = (vmask & bit) ? nullptr : (smask & bit) ? "xe_sr" : (c1 & bit) ? "1.0" : "0.0";
                            if (value)
                                w.Line("%s.%c = %s;", target.c_str(), kComp[i], value);
                            else
                                w.Line("%s.%c = xe_vr.%c;", target.c_str(), kComp[i], kComp[i]);
                        }
                }
                else
                {
                    if (vmask)
                    {
                        std::string d = Temp(alu.vector_dest(), alu.is_vector_dest_relative());
                        for (uint32_t i = 0; i < 4; i++)
                            if (vmask & (1u << i))
                                w.Line("%s.%c = xe_vr.%c;", d.c_str(), kComp[i], kComp[i]);
                    }
                    if (smask)
                    {
                        std::string d = Temp(alu.scalar_dest(), alu.is_scalar_dest_relative());
                        for (uint32_t i = 0; i < 4; i++)
                            if (smask & (1u << i))
                                w.Line("%s.%c = xe_sr;", d.c_str(), kComp[i]);
                    }
                }
                w.indent--;
                w.Line("}");
            }

            std::string ExportTarget(uint32_t reg, uint32_t mask)
            {
                char buf[32];
                if (m_type == ShaderType::kVertex)
                {
                    if (reg < 16)
                    {
                        m_exportMask |= 1u << reg;
                        snprintf(buf, sizeof(buf), "xe_o_t[%u]", reg);
                        return buf;
                    }
                    if (reg == uint32_t(ucode::ExportRegister::kVSPosition)) return "xe_o_pos";
                    if (reg == uint32_t(ucode::ExportRegister::kVSPointSizeEdgeFlagKillVertex))
                    {
                        if (mask & 1) m_writesPointSize = true;
                        return "xe_o_psize";
                    }
                    return ""; // memory export: not supported
                }
                if (reg < 16)
                {
                    m_exportMask |= 1u << (reg & 3);
                    snprintf(buf, sizeof(buf), "xe_o_c[%u]", reg & 3);
                    return buf;
                }
                if (reg == uint32_t(ucode::ExportRegister::kPSDepth))
                {
                    if (mask & 1) m_writesDepth = true;
                    return "xe_o_depth";
                }
                return "";
            }

            // --- Fetches -----------------------------------------------------
            void StoreFetch(uint32_t dest, bool relative, uint32_t swizzle, const std::string& value)
            {
                std::string d = Temp(dest, relative);
                w.Line("{");
                w.indent++;
                w.Line("float4 xe_fr = %s;", value.c_str());
                for (uint32_t i = 0; i < 4; i++)
                {
                    auto s = ucode::GetFetchDestinationComponentSwizzle(swizzle, i);
                    switch (s)
                    {
                    case ucode::FetchDestinationSwizzle::kX: w.Line("%s.%c = xe_fr.x;", d.c_str(), kComp[i]); break;
                    case ucode::FetchDestinationSwizzle::kY: w.Line("%s.%c = xe_fr.y;", d.c_str(), kComp[i]); break;
                    case ucode::FetchDestinationSwizzle::kZ: w.Line("%s.%c = xe_fr.z;", d.c_str(), kComp[i]); break;
                    case ucode::FetchDestinationSwizzle::kW: w.Line("%s.%c = xe_fr.w;", d.c_str(), kComp[i]); break;
                    case ucode::FetchDestinationSwizzle::k1: w.Line("%s.%c = 1.0;", d.c_str(), kComp[i]); break;
                    case ucode::FetchDestinationSwizzle::kKeep: break;
                    default: w.Line("%s.%c = 0.0;", d.c_str(), kComp[i]); break;
                    }
                }
                w.indent--;
                w.Line("}");
            }

            void EmitVertexFetch(const ucode::VertexFetchInstruction& vf)
            {
                if (!vf.is_mini_fetch())
                    m_lastFullFetch = vf;
                uint32_t fc = m_lastFullFetch.fetch_constant_index();
                if (fc < 96)
                    m_vertexMask[fc >> 5] |= 1u << (fc & 31);

                w.Line("{");
                w.indent++;
                if (!vf.is_mini_fetch())
                {
                    std::string src = Temp(vf.src(), vf.is_src_relative());
                    w.Line("xe_vf_addr = %uu * uint(floor(%s.%c%s)) + xe_vfetch_base(%uu);", vf.stride(), src.c_str(),
                           kComp[vf.src_swizzle() & 3], vf.is_index_rounded() ? " + 0.5" : "", fc);
                }

                uint32_t used = 0;
                for (uint32_t i = 0; i < 4; i++)
                {
                    uint32_t s = (vf.dest_swizzle() >> (3 * i)) & 7;
                    if (s <= 3) used |= 1u << s;
                }
                uint32_t needed = xenos::GetVertexFormatNeededWords(vf.data_format(), used);
                w.Line("float4 xe_f = float4(0.0, 0.0, 0.0, 0.0);");
                if (needed)
                {
                    for (uint32_t i = 0; i < 4; i++)
                        w.Line("uint xe_d%u = %s;", i,
                               (needed & (1u << i)) ? ("xe_vfetch_dword(" + std::to_string(fc) + "u, uint(int(xe_vf_addr) + " +
                                                      std::to_string(vf.offset() + int32_t(i)) + "))").c_str()
                                                    : "0u");
                    EmitVertexDecode(vf, used);
                }
                if (vf.exp_adjust())
                    w.Line("xe_f = xe_f * %s;", Float(std::ldexp(1.0f, vf.exp_adjust())).c_str());
                w.indent--;
                // Store inside the same scope.
                w.indent++;
                StoreFetch(vf.dest(), vf.is_dest_relative(), vf.dest_swizzle(), "xe_f");
                w.indent--;
                w.Line("}");
            }

            void EmitVertexDecode(const ucode::VertexFetchInstruction& vf, uint32_t used)
            {
                using VF = xenos::VertexFormat;
                uint32_t packed = 0, widths[4] = {}, offsets[4] = {};
                const char* dwordFor[4] = { "xe_d0", "xe_d0", "xe_d0", "xe_d0" };
                switch (vf.data_format())
                {
                case VF::k_8_8_8_8: packed = 15; widths[0] = widths[1] = widths[2] = widths[3] = 8; offsets[1] = 8; offsets[2] = 16; offsets[3] = 24; break;
                case VF::k_2_10_10_10: packed = 15; widths[0] = widths[1] = widths[2] = 10; widths[3] = 2; offsets[1] = 10; offsets[2] = 20; offsets[3] = 30; break;
                case VF::k_10_11_11: packed = 7; widths[0] = widths[1] = 11; widths[2] = 10; offsets[1] = 11; offsets[2] = 22; break;
                case VF::k_11_11_10: packed = 7; widths[0] = 10; widths[1] = widths[2] = 11; offsets[1] = 10; offsets[2] = 21; break;
                case VF::k_16_16: packed = 3; widths[0] = widths[1] = 16; offsets[1] = 16; break;
                case VF::k_16_16_16_16:
                    packed = 15; widths[0] = widths[1] = widths[2] = widths[3] = 16; offsets[1] = offsets[3] = 16;
                    dwordFor[2] = dwordFor[3] = "xe_d1";
                    break;
                case VF::k_16_16_16_16_FLOAT:
                    if (used & 8) w.Line("xe_f.w = xe_half(xe_d1 >> 16);");
                    if (used & 4) w.Line("xe_f.z = xe_half(xe_d1 & 0xFFFFu);");
                    [[fallthrough]];
                case VF::k_16_16_FLOAT:
                    if (used & 2) w.Line("xe_f.y = xe_half(xe_d0 >> 16);");
                    if (used & 1) w.Line("xe_f.x = xe_half(xe_d0 & 0xFFFFu);");
                    break;
                case VF::k_32: case VF::k_32_32: case VF::k_32_32_32_32:
                    for (int i = 0; i < 4; i++)
                    {
                        if (vf.is_signed())
                        {
                            if (!vf.is_normalized()) w.Line("xe_f.%c = float(int(xe_d%d));", kComp[i], i);
                            else if (vf.signed_rf_mode() == xenos::SignedRepeatingFractionMode::kNoZero)
                                w.Line("xe_f.%c = (float(int(xe_d%d)) + 0.5) / 2147483647.5;", kComp[i], i);
                            else
                                w.Line("xe_f.%c = float(int(xe_d%d)) / 2147483647.0;", kComp[i], i);
                        }
                        else
                            w.Line("xe_f.%c = float(xe_d%d)%s;", kComp[i], i, vf.is_normalized() ? " / 4294967295.0" : "");
                    }
                    break;
                case VF::k_32_FLOAT: case VF::k_32_32_FLOAT: case VF::k_32_32_32_32_FLOAT: case VF::k_32_32_32_FLOAT:
                    w.Line("xe_f = float4(asfloat(xe_d0), asfloat(xe_d1), asfloat(xe_d2), asfloat(xe_d3));");
                    break;
                default:
                    break;
                }
                packed &= used;
                for (uint32_t i = 0; i < 4; i++)
                {
                    if (!(packed & (1u << i))) continue;
                    uint32_t wdt = widths[i], off = offsets[i];
                    if (vf.is_signed())
                    {
                        w.Line("xe_f.%c = float(int(%s << %uu) >> %u);", kComp[i], dwordFor[i], 32 - (wdt + off), 32 - wdt);
                        if (vf.is_normalized())
                        {
                            if (vf.signed_rf_mode() == xenos::SignedRepeatingFractionMode::kNoZero)
                                w.Line("xe_f.%c = (xe_f.%c + 0.5) * %s;", kComp[i], kComp[i], Float(2.0f / float((1u << wdt) - 1)).c_str());
                            else
                                w.Line("xe_f.%c = max(-1.0, xe_f.%c / %s);", kComp[i], kComp[i], Float(float((1u << (wdt - 1)) - 1)).c_str());
                        }
                    }
                    else
                    {
                        w.Line("xe_f.%c = float((%s >> %uu) & %uu);", kComp[i], dwordFor[i], off, (1u << wdt) - 1);
                        if (vf.is_normalized())
                            w.Line("xe_f.%c = xe_f.%c / %s;", kComp[i], kComp[i], Float(float((1u << wdt) - 1)).c_str());
                    }
                }
            }

            void EmitTextureFetch(const ucode::TextureFetchInstruction& tf)
            {
                uint32_t fc = tf.fetch_constant_index() & 31;
                m_textureMask |= 1u << fc;
                m_textureDim[fc] = uint32_t(tf.dimension());
                std::string src = Temp(tf.src(), tf.is_src_relative());
                uint32_t sw = tf.src_swizzle();
                char buf[512];
                snprintf(buf, sizeof(buf), "xe_tfetch(%uu, %uu, float4(%s.%c, %s.%c, %s.%c, 0.0), float4(%s, %s, %s, 0.0), %s)",
                         fc, uint32_t(tf.dimension()),
                         src.c_str(), kComp[sw & 3], src.c_str(), kComp[(sw >> 2) & 3], src.c_str(), kComp[(sw >> 4) & 3],
                         Float(tf.offset_x()).c_str(), Float(tf.offset_y()).c_str(), Float(tf.offset_z()).c_str(),
                         tf.unnormalized_coordinates() ? "true" : "false");
                StoreFetch(tf.dest(), tf.is_dest_relative(), tf.dest_swizzle(), buf);
            }

            ShaderType m_type;
            const uint32_t* m_ucode;
            uint32_t m_dwords;
            Writer w;
            std::string m_error;
            uint32_t m_maxTemp = 0;
            bool m_usesRelativeTemps = false;
            uint32_t m_textureMask = 0, m_exportMask = 0;
            uint32_t m_vertexMask[3] = {};
            uint32_t m_textureDim[32] = {};
            bool m_writesPointSize = false, m_writesDepth = false, m_usesKill = false;
            uint32_t m_floatConsts = 0;
            ucode::VertexFetchInstruction m_lastFullFetch{};
        };
    }

    TranslatedShader Translate(ShaderType type, const uint32_t* ucode, uint32_t dwordCount)
    {
        Translator t(type, ucode, dwordCount);
        return t.Run();
    }
}
