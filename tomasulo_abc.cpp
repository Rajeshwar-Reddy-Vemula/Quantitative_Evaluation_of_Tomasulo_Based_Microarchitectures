// ============================================================================
// Tomasulo Simulator with three models:
//
//   Model A: Classic Tomasulo (no ROB, no speculation)
//   Model B: Tomasulo + 32-entry ROB, precise exceptions, no speculation
//   Model C: Tomasulo + 32-entry ROB + simple branch prediction + speculation
//
// - RS sizes and latencies taken from trace file
// - ASCII tables with '-' for "none"
// - Interactive stepping: press Enter to advance, 'q'+Enter to quit
// - Compatible with g++ 4.8.5 using -std=c++11
// ============================================================================

#include <iostream>
#include <iomanip>
#include <vector>
#include <map>
#include <string>
#include <sstream>
#include <fstream>
#include <cstdlib>
#include <algorithm>

static std::string toUpper(const std::string &s) {
    std::string t = s;
    std::transform(t.begin(), t.end(), t.begin(),
                   [](unsigned char c){ return std::toupper(c); });
    return t;
}

static int regIndex(const std::string &r) {
    if (r.size() < 2) return 0;
    return std::atoi(r.substr(1).c_str());
}

static std::string fmtInt(int v) {
    if (v < 0) return "-";
    return std::to_string(v);
}

static void line(const std::string &s) {
    std::cout << s << "\n";
}

// ------------------------------------------------------------------
// ISA + config
// ------------------------------------------------------------------

enum class OpKind {
    Add,
    Sub,
    Mul,
    Div,
    Load,
    Store,
    Beq,
    Bneq
};

struct InstrStatus {
    int issue        = -1;
    int execStart    = -1;
    int execComplete = -1;
    int commit       = -1;
};

struct Instruction {
    OpKind kind;
    std::string opStr;
    int rd = -1;
    int rs = -1;
    int rt = -1;
    int offset   = 0;
    int targetPc = -1;
    InstrStatus status;
};

// 3-letter display mnemonic for tables only
static std::string dispOp3(const Instruction &ins) {
    switch (ins.kind) {
    case OpKind::Add:   return "ADD";
    case OpKind::Sub:   return "SUB";
    case OpKind::Mul:   return "MUL";
    case OpKind::Div:   return "DIV";
    case OpKind::Load:  return "LDR";
    case OpKind::Store: return "STR";
    case OpKind::Beq:   return "BEQ";
    case OpKind::Bneq:  return "BNQ";
    default:            return "???";
    }
}

struct Config {
    int numAddRS    = 0;
    int numMulRS    = 0;
    int numLoadBuf  = 0;
    int numStoreBuf = 0;

    int addSubCycles    = 1;
    int mulCycles       = 1;
    int divCycles       = 1;
    int loadStoreCycles = 1;

    int numRegs = 0;
};

struct MachineState {
    std::vector<double> regs;
    std::map<int,double> memory;
};

struct Program {
    Config cfg;
    MachineState init;
    std::vector<Instruction> instrs;
};

// ------------------------------------------------------------------
// Trace parser
// ------------------------------------------------------------------
// Expected format:
//
// LOAD  Fd  baseReg  imm
// STORE Fs  baseReg  imm
// ADD   Fd  Fs Ft
// SUB   Fd  Fs Ft
// MUL   Fd  Fs Ft
// DIV   Fd  Fs Ft
// BEQ   Rs  Rt offset
// BNEQ  Rs  Rt offset
// ------------------------------------------------------------------

Program parseProgram(const std::string &filename) {
    Program P;
    std::ifstream fin(filename.c_str());
    if (!fin) {
        std::cerr << "ERROR: cannot open trace file: " << filename << "\n";
        std::exit(1);
    }

    std::string lineStr;
    while (std::getline(fin, lineStr)) {
        if (lineStr.empty()) continue;
        if (lineStr[0] == '#') continue;

        std::stringstream ss(lineStr);
        std::string token;
        ss >> token;
        if (token.empty()) continue;
        std::string up = toUpper(token);

        if (up == "ADD_SUB_RESERVATION_STATIONS") {
            ss >> P.cfg.numAddRS;
        } else if (up == "MUL_DIV_RESERVATION_STATIONS") {
            ss >> P.cfg.numMulRS;
        } else if (up == "LOAD_BUFFERS") {
            ss >> P.cfg.numLoadBuf;
        } else if (up == "STORE_BUFFERS") {
            ss >> P.cfg.numStoreBuf;
        } else if (up == "ADD_SUB_CYCLES") {
            ss >> P.cfg.addSubCycles;
        } else if (up == "MUL_CYCLES") {
            ss >> P.cfg.mulCycles;
        } else if (up == "DIV_CYCLES") {
            ss >> P.cfg.divCycles;
        } else if (up == "LOAD_STORE_CYCLES") {
            ss >> P.cfg.loadStoreCycles;
        } else if (up == "REG") {
            ss >> P.cfg.numRegs;
            P.init.regs.assign(P.cfg.numRegs, 0.0);
        } else if (up == "INIT_REG") {
            std::string r; double v;
            ss >> r >> v;
            int idx = regIndex(toUpper(r));
            if (idx >= 0 && idx < (int)P.init.regs.size())
                P.init.regs[idx] = v;
        } else if (up == "INIT_MEM") {
            int addr; double v;
            ss >> addr >> v;
            P.init.memory[addr] = v;
        } else if (up == "LOAD") {
            // LOAD Fd baseReg imm
            Instruction I;
            I.kind  = OpKind::Load;
            I.opStr = "LOAD";
            std::string rd, base;
            int imm;
            ss >> rd >> base >> imm;
            I.rd     = regIndex(toUpper(rd));
            I.rs     = regIndex(toUpper(base));
            I.offset = imm;
            P.instrs.push_back(I);
        } else if (up == "STORE") {
            // STORE Fs baseReg imm
            Instruction I;
            I.kind  = OpKind::Store;
            I.opStr = "STORE";
            std::string rs, base;
            int imm;
            ss >> rs >> base >> imm;
            I.rs     = regIndex(toUpper(rs));   // value
            I.rt     = regIndex(toUpper(base)); // base
            I.offset = imm;
            P.instrs.push_back(I);
        } else if (up == "ADD" || up == "SUB" ||
                   up == "MUL" || up == "DIV") {
            Instruction I;
            I.opStr = up;
            if (up == "ADD")      I.kind = OpKind::Add;
            else if (up == "SUB") I.kind = OpKind::Sub;
            else if (up == "MUL") I.kind = OpKind::Mul;
            else                  I.kind = OpKind::Div;

            std::string rd, rs, rt;
            ss >> rd >> rs >> rt;
            I.rd = regIndex(toUpper(rd));
            I.rs = regIndex(toUpper(rs));
            I.rt = regIndex(toUpper(rt));
            P.instrs.push_back(I);
        } else if (up == "BEQ" || up == "BNEQ") {
            Instruction I;
            I.opStr = up;
            I.kind  = (up == "BEQ" ? OpKind::Beq : OpKind::Bneq);
            std::string rs, rt; int off;
            ss >> rs >> rt >> off;
            I.rs     = regIndex(toUpper(rs));
            I.rt     = regIndex(toUpper(rt));
            I.offset = off;
            P.instrs.push_back(I);
        }
    }
    fin.close();

    // Compute branch targets
    for (int i = 0; i < (int)P.instrs.size(); ++i) {
        Instruction &ins = P.instrs[i];
        if (ins.kind == OpKind::Beq || ins.kind == OpKind::Bneq) {
            int tgt = i + 1 + ins.offset;
            if (tgt < 0) tgt = 0;
            if (tgt > (int)P.instrs.size())
                tgt = P.instrs.size();
            ins.targetPc = tgt;
        }
    }

    return P;
}

// ============================================================================
// MODEL A: classic Tomasulo, no ROB, no speculation
// ============================================================================

namespace ModelA {

struct RegStatus {
    int writer; // -1 = architectural
};

struct RS {
    bool busy;
    OpKind op;
    int rd, rs, rt;
    double Vj, Vk;
    bool VjReady, VkReady;
    int Qj, Qk;
    Instruction* inst;
    RS() : busy(false), op(OpKind::Add),
           rd(-1), rs(-1), rt(-1),
           Vj(0), Vk(0), VjReady(false), VkReady(false),
           Qj(-1), Qk(-1), inst(NULL) {}
};

struct LSBuf {
    bool busy;
    bool isLoad;
    int rd;   // for load
    int rs;   // base or value
    int rt;   // base for store
    int offset;
    double addrVal;
    bool addrReady;
    int addrQ;
    double val;
    bool valReady;
    int valQ;
    Instruction* inst;
    LSBuf() : busy(false), isLoad(true), rd(-1), rs(-1), rt(-1),
              offset(0), addrVal(0), addrReady(false), addrQ(-1),
              val(0), valReady(false), valQ(-1), inst(NULL) {}
};

class Sim {
public:
    Sim(const Program &p)
        : prog(p),
          regs(p.init.regs),
          memory(p.init.memory),
          cycle(0), pc(0),
          branchPending(false), branchInstrIdx(-1)
    {
        const Config &cfg = prog.cfg;
        regStatus.assign(cfg.numRegs, RegStatus{-1});
        addRS.resize(cfg.numAddRS);
        mulRS.resize(cfg.numMulRS);
        loadBuf.resize(cfg.numLoadBuf);
        storeBuf.resize(cfg.numStoreBuf);
    }

    void run() {
        int committed = 0;
        const int maxCycles = 10000;

        while (true) {
            if (pc >= (int)prog.instrs.size() && !anyBusy()) {
                printState();
                break;
            }

            if (cycle >= maxCycles) {
                std::cout << "Model A: stopping after " << maxCycles
                          << " cycles (safety limit)\n";
                printState();
                break;
            }

            printState();
            if (!prompt()) break;

            if (!branchPending && pc < (int)prog.instrs.size()) {
                tryIssue(pc);
            }

            stepExecute();
            stepWriteBack(committed);
            cycle++;
        }

        double cpi = committed > 0 ? (double)cycle / (double)committed : 0.0;
        printSummary("Model A (no ROB, no speculation)", committed, cpi);
    }

private:
    const Program &prog;
    std::vector<double> regs;
    std::map<int,double> memory;

    std::vector<RegStatus> regStatus;
    std::vector<RS> addRS, mulRS;
    std::vector<LSBuf> loadBuf, storeBuf;

    int cycle;
    int pc;

    bool branchPending;
    int  branchInstrIdx;

    bool anyBusy() const {
        for (size_t i=0;i<addRS.size();++i) if (addRS[i].busy) return true;
        for (size_t i=0;i<mulRS.size();++i) if (mulRS[i].busy) return true;
        for (size_t i=0;i<loadBuf.size();++i) if (loadBuf[i].busy) return true;
        for (size_t i=0;i<storeBuf.size();++i) if (storeBuf[i].busy) return true;
        return false;
    }

    void attachSrc(int r, double &V, bool &ready, int &Q) {
        if (r < 0) { ready=false; Q=-1; return; }
        RegStatus &rs = regStatus[r];
        if (rs.writer < 0) {
            V = regs[r];
            ready = true;
            Q = -1;
        } else {
            ready = false;
            Q = rs.writer;
        }
    }

    int findFreeAdd() const {
        for (size_t i=0;i<addRS.size();++i)
            if (!addRS[i].busy) return (int)i;
        return -1;
    }
    int findFreeMul() const {
        for (size_t i=0;i<mulRS.size();++i)
            if (!mulRS[i].busy) return (int)i;
        return -1;
    }
    int findFreeLoad() const {
        for (size_t i=0;i<loadBuf.size();++i)
            if (!loadBuf[i].busy) return (int)i;
        return -1;
    }
    int findFreeStore() const {
        for (size_t i=0;i<storeBuf.size();++i)
            if (!storeBuf[i].busy) return (int)i;
        return -1;
    }

    void tryIssue(int idx) {
        if (idx < 0 || idx >= (int)prog.instrs.size()) return;
        Instruction &ins = const_cast<Instruction&>(prog.instrs[idx]);
        if (ins.status.issue >= 0) return;

        const Config &cfg = prog.cfg;
        (void)cfg;

        switch (ins.kind) {
        case OpKind::Add:
        case OpKind::Sub: {
            int s = findFreeAdd();
            if (s < 0) return;
            RS &r = addRS[s];
            r = RS();
            r.busy = true;
            r.op   = ins.kind;
            r.rd   = ins.rd;
            r.rs   = ins.rs;
            r.rt   = ins.rt;
            r.inst = &ins;
            attachSrc(ins.rs, r.Vj, r.VjReady, r.Qj);
            attachSrc(ins.rt, r.Vk, r.VkReady, r.Qk);
            regStatus[ins.rd].writer = s;
            ins.status.issue = cycle;
            pc++;
            break;
        }
        case OpKind::Mul:
        case OpKind::Div: {
            int s = findFreeMul();
            if (s < 0) return;
            RS &r = mulRS[s];
            r = RS();
            r.busy = true;
            r.op   = ins.kind;
            r.rd   = ins.rd;
            r.rs   = ins.rs;
            r.rt   = ins.rt;
            r.inst = &ins;
            attachSrc(ins.rs, r.Vj, r.VjReady, r.Qj);
            attachSrc(ins.rt, r.Vk, r.VkReady, r.Qk);
            // store writer tag as RS index + addRS.size() for uniqueness
            regStatus[ins.rd].writer = (int)addRS.size() + s;
            ins.status.issue = cycle;
            pc++;
            break;
        }
        case OpKind::Load: {
            int s = findFreeLoad();
            if (s < 0) return;
            LSBuf &b = loadBuf[s];
            b = LSBuf();
            b.busy   = true;
            b.isLoad = true;
            b.rd     = ins.rd;
            b.rs     = ins.rs;
            b.offset = ins.offset;
            b.inst   = &ins;
            {
                RegStatus &rsb = regStatus[ins.rs];
                if (rsb.writer < 0) {
                    b.addrVal   = (int)regs[ins.rs] + ins.offset;
                    b.addrReady = true;
                    b.addrQ     = -1;
                } else {
                    b.addrReady = false;
                    b.addrQ     = rsb.writer;
                }
            }
            regStatus[ins.rd].writer =
                (int)addRS.size() + (int)mulRS.size() + s;
            ins.status.issue = cycle;
            pc++;
            break;
        }
        case OpKind::Store: {
            int s = findFreeStore();
            if (s < 0) return;
            LSBuf &b = storeBuf[s];
            b = LSBuf();
            b.busy   = true;
            b.isLoad = false;
            b.rs     = ins.rs; // value
            b.rt     = ins.rt; // base
            b.offset = ins.offset;
            b.inst   = &ins;
            {
                RegStatus &rbase = regStatus[ins.rt];
                if (rbase.writer < 0) {
                    b.addrVal   = (int)regs[ins.rt] + ins.offset;
                    b.addrReady = true;
                    b.addrQ     = -1;
                } else {
                    b.addrReady = false;
                    b.addrQ     = rbase.writer;
                }
            }
            {
                RegStatus &rval = regStatus[ins.rs];
                if (rval.writer < 0) {
                    b.val      = regs[ins.rs];
                    b.valReady = true;
                    b.valQ     = -1;
                } else {
                    b.valReady = false;
                    b.valQ     = rval.writer;
                }
            }
            ins.status.issue = cycle;
            pc++;
            break;
        }
        case OpKind::Beq:
        case OpKind::Bneq: {
            int s = findFreeAdd();
            if (s < 0) return;
            RS &r = addRS[s];
            r = RS();
            r.busy = true;
            r.op   = ins.kind;
            r.rs   = ins.rs;
            r.rt   = ins.rt;
            r.rd   = -1;
            r.inst = &ins;
            attachSrc(ins.rs, r.Vj, r.VjReady, r.Qj);
            attachSrc(ins.rt, r.Vk, r.VkReady, r.Qk);
            ins.status.issue = cycle;
            branchPending   = true;
            branchInstrIdx  = idx;
            break;
        }
        default:
            break;
        }
    }

    void stepExecute() {
        const Config &cfg = prog.cfg;
        for (size_t i=0;i<addRS.size();++i) {
            RS &r = addRS[i];
            if (!r.busy || !r.inst) continue;
            Instruction &ins = *r.inst;
            if (ins.status.execStart >= 0) continue;
            if (!r.VjReady || !r.VkReady) continue;
            if (ins.status.issue == cycle) continue;
            ins.status.execStart    = cycle;
            ins.status.execComplete = cycle + cfg.addSubCycles;
        }
        for (size_t i=0;i<mulRS.size();++i) {
            RS &r = mulRS[i];
            if (!r.busy || !r.inst) continue;
            Instruction &ins = *r.inst;
            if (ins.status.execStart >= 0) continue;
            if (!r.VjReady || !r.VkReady) continue;
            if (ins.status.issue == cycle) continue;
            ins.status.execStart = cycle;
            int lat = (ins.kind == OpKind::Mul) ? cfg.mulCycles : cfg.divCycles;
            ins.status.execComplete = cycle + lat;
        }
        for (size_t i=0;i<loadBuf.size();++i) {
            LSBuf &b = loadBuf[i];
            if (!b.busy || !b.inst) continue;
            Instruction &ins = *b.inst;
            if (ins.status.execStart >= 0) continue;
            if (!b.addrReady) continue;
            if (ins.status.issue == cycle) continue;
            ins.status.execStart    = cycle;
            ins.status.execComplete = cycle + cfg.loadStoreCycles;
        }
        for (size_t i=0;i<storeBuf.size();++i) {
            LSBuf &b = storeBuf[i];
            if (!b.busy || !b.inst) continue;
            Instruction &ins = *b.inst;
            if (ins.status.execStart >= 0) continue;
            if (!b.addrReady || !b.valReady) continue;
            if (ins.status.issue == cycle) continue;
            ins.status.execStart    = cycle;
            ins.status.execComplete = cycle + cfg.loadStoreCycles;
        }
    }

    void broadcast(int tag, double val) {
        for (size_t i=0;i<addRS.size();++i) {
            RS &r = addRS[i];
            if (!r.busy) continue;
            if (r.Qj == tag) { r.Qj=-1; r.Vj=val; r.VjReady=true; }
            if (r.Qk == tag) { r.Qk=-1; r.Vk=val; r.VkReady=true; }
        }
        for (size_t i=0;i<mulRS.size();++i) {
            RS &r = mulRS[i];
            if (!r.busy) continue;
            if (r.Qj == tag) { r.Qj=-1; r.Vj=val; r.VjReady=true; }
            if (r.Qk == tag) { r.Qk=-1; r.Vk=val; r.VkReady=true; }
        }
        for (size_t i=0;i<loadBuf.size();++i) {
            LSBuf &b = loadBuf[i];
            if (!b.busy) continue;
            if (b.addrQ == tag) {
                b.addrQ   = -1;
                b.addrVal = val;
                b.addrReady = true;
            }
        }
        for (size_t i=0;i<storeBuf.size();++i) {
            LSBuf &b = storeBuf[i];
            if (!b.busy) continue;
            if (b.addrQ == tag) {
                b.addrQ   = -1;
                b.addrVal = val;
                b.addrReady = true;
            }
            if (b.valQ == tag) {
                b.valQ    = -1;
                b.val     = val;
                b.valReady= true;
            }
        }
    }

    void stepWriteBack(int &committed) {
        for (size_t i=0;i<loadBuf.size();++i) {
            LSBuf &b = loadBuf[i];
            if (!b.busy || !b.inst) continue;
            Instruction &ins = *b.inst;
            if (ins.status.execComplete >= 0 &&
                ins.status.execComplete <= cycle &&
                ins.status.commit < 0)
            {
                int addr = (int)b.addrVal;
                double val = 0.0;
                if (memory.count(addr)) val = memory[addr];
                regs[ins.rd] = val;
                int tag = (int)addRS.size() + (int)mulRS.size() + (int)i;
                if (regStatus[ins.rd].writer == tag)
                    regStatus[ins.rd].writer = -1;
                broadcast(tag,val);
                ins.status.commit = cycle;
                b.busy = false;
                b.inst = NULL;
                committed++;
            }
        }
        for (size_t i=0;i<storeBuf.size();++i) {
            LSBuf &b = storeBuf[i];
            if (!b.busy || !b.inst) continue;
            Instruction &ins = *b.inst;
            if (ins.status.execComplete >= 0 &&
                ins.status.execComplete <= cycle &&
                ins.status.commit < 0)
            {
                int addr = (int)b.addrVal;
                memory[addr] = b.val;
                ins.status.commit = cycle;
                b.busy = false;
                b.inst = NULL;
                committed++;
            }
        }
        for (size_t i=0;i<addRS.size();++i) {
            RS &r = addRS[i];
            if (!r.busy || !r.inst) continue;
            Instruction &ins = *r.inst;
            if (ins.status.execComplete >= 0 &&
                ins.status.execComplete <= cycle &&
                ins.status.commit < 0)
            {
                if (ins.kind == OpKind::Add || ins.kind == OpKind::Sub) {
                    double val = (ins.kind == OpKind::Add)
                               ? (r.Vj + r.Vk)
                               : (r.Vj - r.Vk);
                    regs[ins.rd] = val;
                    if (regStatus[ins.rd].writer == (int)i)
                        regStatus[ins.rd].writer = -1;
                    broadcast((int)i, val);
                } else if (ins.kind == OpKind::Beq || ins.kind == OpKind::Bneq) {
                    bool equal = (r.Vj == r.Vk);
                    bool taken = (ins.kind == OpKind::Beq) ? equal : !equal;
                    if (branchPending && branchInstrIdx >= 0) {
                        if (taken) pc = ins.targetPc;
                        else       pc = branchInstrIdx + 1;
                        branchPending   = false;
                        branchInstrIdx  = -1;
                    }
                }
                ins.status.commit = cycle;
                r.busy = false;
                r.inst = NULL;
                committed++;
            }
        }
        for (size_t i=0;i<mulRS.size();++i) {
            RS &r = mulRS[i];
            if (!r.busy || !r.inst) continue;
            Instruction &ins = *r.inst;
            if (ins.status.execComplete >= 0 &&
                ins.status.execComplete <= cycle &&
                ins.status.commit < 0)
            {
                double val = (ins.kind == OpKind::Mul)
                           ? (r.Vj*r.Vk)
                           : (r.Vj/r.Vk);
                regs[ins.rd] = val;
                int tag = (int)addRS.size() + (int)i;
                if (regStatus[ins.rd].writer == tag)
                    regStatus[ins.rd].writer = -1;
                broadcast(tag,val);
                ins.status.commit = cycle;
                r.busy = false;
                r.inst = NULL;
                committed++;
            }
        }
    }

    bool prompt() {
        std::cout << "Press Enter to advance, or 'q' + Enter to quit: ";
        std::cout.flush();
        std::string s;
        std::getline(std::cin, s);
        if (!s.empty() && (s[0]=='q' || s[0]=='Q')) {
            std::cout << "Stopping at cycle " << cycle << "\n";
            return false;
        }
        return true;
    }

    void printState() {
        std::cout << "\n============================================================\n";
        std::cout << "                     Model A - Cycle " << cycle << "\n";
        std::cout << "============================================================\n\n";

        line("+---------------- REGISTER FILE ----------------+");
        std::cout << "| ";
        for (size_t i=0;i<regs.size();++i) {
            std::cout << "F" << i << "=" << regs[i] << " ";
        }
        std::cout << "|\n";
        line("+------------------------------------------------+\n");

        line("+---------------- INSTRUCTION STATUS ----------------+");
        std::cout << "Idx  Op   Rd  Rs  Rt | Iss ExSt ExCm Cmt\n";
        std::cout << "------------------------------------------------\n";
        for (size_t i=0;i<prog.instrs.size();++i) {
            const Instruction &ins = prog.instrs[i];
            std::string opDisp = dispOp3(ins);
            std::cout << std::setw(3) << i << "  "
                      << std::setw(3) << opDisp << " "
                      << std::setw(3) << fmtInt(ins.rd) << " "
                      << std::setw(3) << fmtInt(ins.rs) << " "
                      << std::setw(3) << fmtInt(ins.rt) << " | "
                      << std::setw(3) << fmtInt(ins.status.issue) << " "
                      << std::setw(4) << fmtInt(ins.status.execStart) << " "
                      << std::setw(4) << fmtInt(ins.status.execComplete) << " "
                      << std::setw(3) << fmtInt(ins.status.commit) << "\n";
        }
        line("+------------------------------------------------+\n");

        line("+------------------- RAT (tags) -------------------+");
        std::cout << "Reg: ";
        for (size_t i=0;i<regStatus.size();++i) {
            std::cout << "F" << i << ":" << fmtInt(regStatus[i].writer) << " ";
        }
        std::cout << "\n";
        line("+--------------------------------------------------+\n");

        line("+------------------- ADD/SUB RS -------------------+");
        std::cout << "Idx Busy Op  Rd  Rs  Rt | VjR Qj  VkR Qk\n";
        std::cout << "----------------------------------------------\n";
        for (size_t i=0;i<addRS.size();++i) {
            const RS &r = addRS[i];
            std::cout << std::setw(3) << i << "  "
                      << (r.busy?'Y':'N') << "   "
                      << std::setw(3) << (int)r.op << " "
                      << std::setw(3) << fmtInt(r.rd) << " "
                      << std::setw(3) << fmtInt(r.rs) << " "
                      << std::setw(3) << fmtInt(r.rt) << " | "
                      << (r.VjReady?'1':'0') << "  "
                      << std::setw(2) << fmtInt(r.Qj) << "   "
                      << (r.VkReady?'1':'0') << "  "
                      << std::setw(2) << fmtInt(r.Qk) << "\n";
        }
        line("+--------------------------------------------------+\n");

        line("+------------------- MUL/DIV RS -------------------+");
        std::cout << "Idx Busy Op  Rd  Rs  Rt | VjR Qj  VkR Qk\n";
        std::cout << "----------------------------------------------\n";
        for (size_t i=0;i<mulRS.size();++i) {
            const RS &r = mulRS[i];
            std::cout << std::setw(3) << i << "  "
                      << (r.busy?'Y':'N') << "   "
                      << std::setw(3) << (int)r.op << " "
                      << std::setw(3) << fmtInt(r.rd) << " "
                      << std::setw(3) << fmtInt(r.rs) << " "
                      << std::setw(3) << fmtInt(r.rt) << " | "
                      << (r.VjReady?'1':'0') << "  "
                      << std::setw(2) << fmtInt(r.Qj) << "   "
                      << (r.VkReady?'1':'0') << "  "
                      << std::setw(2) << fmtInt(r.Qk) << "\n";
        }
        line("+--------------------------------------------------+\n");

        line("+-------------------- LOAD BUFFERS --------------------+");
        std::cout << "Idx Busy AddrR AddrQ\n";
        std::cout << "--------------------------\n";
        for (size_t i=0;i<loadBuf.size();++i) {
            const LSBuf &b = loadBuf[i];
            std::cout << std::setw(3) << i << "  "
                      << (b.busy?'Y':'N') << "    "
                      << (b.addrReady?'1':'0') << "    "
                      << std::setw(3) << fmtInt(b.addrQ) << "\n";
        }
        line("+------------------------------------------------------+\n");

        line("+-------------------- STORE BUFFERS -------------------+");
        std::cout << "Idx Busy AddrR AddrQ ValR ValQ\n";
        std::cout << "-------------------------------------\n";
        for (size_t i=0;i<storeBuf.size();++i) {
            const LSBuf &b = storeBuf[i];
            std::cout << std::setw(3) << i << "  "
                      << (b.busy?'Y':'N') << "    "
                      << (b.addrReady?'1':'0') << "    "
                      << std::setw(3) << fmtInt(b.addrQ) << "   "
                      << (b.valReady?'1':'0') << "   "
                      << std::setw(3) << fmtInt(b.valQ) << "\n";
        }
        line("+------------------------------------------------------+\n");
    }

    void printSummary(const std::string &label, int committed, double cpi) {
        std::cout << "\n================ " << label << " ================\n";
        std::cout << "Final registers:\n";
        for (size_t i=0;i<regs.size();++i)
            std::cout << "F" << i << " = " << regs[i] << "\n";

        std::cout << "\nFinal memory (touched):\n";
        for (std::map<int,double>::const_iterator it = memory.begin();
             it != memory.end(); ++it)
        {
            std::cout << "[" << it->first << "] = " << it->second << "\n";
        }

        std::cout << "\nTotal cycles     : " << cycle << "\n";
        std::cout << "Committed instrs : " << committed << "\n";
        std::cout << "CPI              : " << cpi << "\n\n";
    }
};

} // namespace ModelA
// ============================================================================
// MODEL B/C: ROB-based Tomasulo
// B: no speculation, C: speculation with simple predictor
// ============================================================================

namespace ModelBC {

struct RATEntry {
    int robIdx;
};

struct ROBEntry {
    bool valid;
    int  instrIdx;
    bool ready;
    bool hasDest;
    int  destReg;
    bool isStore;
    bool isBranch;
    bool predictedTaken;
    double value;
    int  addr;

    ROBEntry() :
        valid(false), instrIdx(-1), ready(false),
        hasDest(false), destReg(-1),
        isStore(false), isBranch(false),
        predictedTaken(false), value(0.0), addr(0) {}
};

struct RS {
    bool busy;
    OpKind op;
    int rd, rs, rt;
    double Vj, Vk;
    bool VjReady, VkReady;
    int Qj, Qk;
    int robIdx;
    Instruction *inst;
    RS() : busy(false), op(OpKind::Add),
           rd(-1), rs(-1), rt(-1),
           Vj(0), Vk(0), VjReady(false), VkReady(false),
           Qj(-1), Qk(-1), robIdx(-1), inst(NULL) {}
};

struct LSBuf {
    bool busy;
    bool isLoad;
    int rd;
    int rs;
    int rt;
    int offset;
    double addrVal;
    bool addrReady;
    int addrQ;
    double val;
    bool valReady;
    int valQ;
    int robIdx;
    Instruction *inst;

    LSBuf() : busy(false), isLoad(true),
              rd(-1), rs(-1), rt(-1),
              offset(0), addrVal(0), addrReady(false), addrQ(-1),
              val(0), valReady(false), valQ(-1),
              robIdx(-1), inst(NULL) {}
};

struct BPredEntry {
    bool valid;
    bool predictTaken;
    BPredEntry() : valid(false), predictTaken(false) {}
};

class Sim {
public:
    Sim(const Program &p, bool speculative_)
        : prog(p),
          regs(p.init.regs),
          memory(p.init.memory),
          speculative(speculative_),
          cycle(0), pc(0),
          robHead(0), robTail(0), robCount(0),
          branchCount(0), mispredCount(0), committed(0),
          nsBranchInFlight(false), nsBranchRobIdx(-1)
    {
        const Config &cfg = prog.cfg;
        rat.assign(cfg.numRegs, RATEntry{-1});
        addRS.resize(cfg.numAddRS);
        mulRS.resize(cfg.numMulRS);
        loadBuf.resize(cfg.numLoadBuf);
        storeBuf.resize(cfg.numStoreBuf);
        rob.resize(32);            // fixed ROB size
        bpred.resize(prog.instrs.size());
    }

    void run() {
        const int maxCycles = 100000;
        while (true) {
            if (!hasWork()) {
                printState();
                break;
            }

            if (cycle >= maxCycles) {
                std::cout << (speculative ? "Model C" : "Model B")
                          << ": safety stop at " << maxCycles << " cycles\n";
                printState();
                break;
            }

            printState();
            if (!prompt()) break;

            stepCommit();
            stepWriteBack();
            stepExecute();
            stepIssue();
            cycle++;
        }

        double cpi = committed > 0 ? (double)cycle / (double)committed : 0.0;
        std::string label = speculative
            ? "Model C (ROB + speculation)"
            : "Model B (ROB, no speculation)";
        printSummary(label, committed, cpi);
    }

private:
    const Program &prog;
    std::vector<double> regs;
    std::map<int,double> memory;

    std::vector<RATEntry> rat;
    std::vector<RS> addRS, mulRS;
    std::vector<LSBuf> loadBuf, storeBuf;
    std::vector<ROBEntry> rob;
    std::vector<BPredEntry> bpred;

    bool speculative;
    int cycle;
    int pc;

    int robHead, robTail, robCount;

    int branchCount;
    int mispredCount;
    int committed;

    // non-speculative branch stall for Model B
    bool nsBranchInFlight;
    int  nsBranchRobIdx;

    bool hasWork() {
        if (robCount > 0) return true;
        for (size_t i=0;i<addRS.size();++i) if (addRS[i].busy) return true;
        for (size_t i=0;i<mulRS.size();++i) if (mulRS[i].busy) return true;
        for (size_t i=0;i<loadBuf.size();++i) if (loadBuf[i].busy) return true;
        for (size_t i=0;i<storeBuf.size();++i) if (storeBuf[i].busy) return true;
        if (pc < (int)prog.instrs.size()) return true;
        return false;
    }

    int allocROB(int instrIdx, bool hasDest, int destReg, bool isStore) {
        if (robCount == (int)rob.size()) return -1;
        int idx = robTail;
        robTail = (robTail + 1) % (int)rob.size();
        robCount++;
        ROBEntry &e = rob[idx];
        e = ROBEntry();
        e.valid   = true;
        e.instrIdx= instrIdx;
        e.hasDest = hasDest;
        e.destReg = destReg;
        e.isStore = isStore;
        return idx;
    }

    ROBEntry* getROB(int idx) {
        if (idx < 0 || idx >= (int)rob.size()) return NULL;
        if (!rob[idx].valid) return NULL;
        return &rob[idx];
    }

    void attachSrc(int r, double &V, bool &ready, int &Q) {
        if (r < 0) { ready=false; Q=-1; return; }
        int ri = rat[r].robIdx;
        if (ri < 0) {
            V = regs[r];
            ready = true;
            Q = -1;
        } else {
            ROBEntry &e = rob[ri];
            if (e.ready) {
                V = e.value;
                ready = true;
                Q = -1;
            } else {
                ready = false;
                Q = ri;
            }
        }
    }

    int freeAdd() const {
        for (size_t i=0;i<addRS.size();++i)
            if (!addRS[i].busy) return (int)i;
        return -1;
    }
    int freeMul() const {
        for (size_t i=0;i<mulRS.size();++i)
            if (!mulRS[i].busy) return (int)i;
        return -1;
    }
    int freeLoad() const {
        for (size_t i=0;i<loadBuf.size();++i)
            if (!loadBuf[i].busy) return (int)i;
        return -1;
    }
    int freeStore() const {
        for (size_t i=0;i<storeBuf.size();++i)
            if (!storeBuf[i].busy) return (int)i;
        return -1;
    }

    // ----------------------------------------------------------
    // ISSUE
    // ----------------------------------------------------------
    void stepIssue() {
        // Model B: do not issue any instruction while a branch is in-flight
        if (!speculative && nsBranchInFlight)
            return;

        if (pc >= (int)prog.instrs.size()) return;
        Instruction &ins = const_cast<Instruction&>(prog.instrs[pc]);

        switch (ins.kind) {
        case OpKind::Add:
        case OpKind::Sub: {
            int slot = freeAdd();
            if (slot < 0) return;
            int rIdx = allocROB(pc,true,ins.rd,false);
            if (rIdx < 0) return;
            RS &r = addRS[slot];
            r = RS();
            r.busy = true;
            r.op   = ins.kind;
            r.rd   = ins.rd;
            r.rs   = ins.rs;
            r.rt   = ins.rt;
            r.inst = &ins;
            r.robIdx = rIdx;
            attachSrc(ins.rs, r.Vj, r.VjReady, r.Qj);
            attachSrc(ins.rt, r.Vk, r.VkReady, r.Qk);
            rat[ins.rd].robIdx = rIdx;
            if (ins.status.issue < 0) ins.status.issue = cycle;
            pc++;
            break;
        }
        case OpKind::Mul:
        case OpKind::Div: {
            int slot = freeMul();
            if (slot < 0) return;
            int rIdx = allocROB(pc,true,ins.rd,false);
            if (rIdx < 0) return;
            RS &r = mulRS[slot];
            r = RS();
            r.busy = true;
            r.op   = ins.kind;
            r.rd   = ins.rd;
            r.rs   = ins.rs;
            r.rt   = ins.rt;
            r.inst = &ins;
            r.robIdx = rIdx;
            attachSrc(ins.rs, r.Vj, r.VjReady, r.Qj);
            attachSrc(ins.rt, r.Vk, r.VkReady, r.Qk);
            rat[ins.rd].robIdx = rIdx;
            if (ins.status.issue < 0) ins.status.issue = cycle;
            pc++;
            break;
        }
        case OpKind::Load: {
            int slot = freeLoad();
            if (slot < 0) return;
            int rIdx = allocROB(pc,true,ins.rd,false);
            if (rIdx < 0) return;
            LSBuf &b = loadBuf[slot];
            b = LSBuf();
            b.busy   = true;
            b.isLoad = true;
            b.rd     = ins.rd;
            b.rs     = ins.rs;
            b.offset = ins.offset;
            b.inst   = &ins;
            b.robIdx = rIdx;
            {
                int baseROB = rat[ins.rs].robIdx;
                if (baseROB < 0) {
                    b.addrVal   = (int)regs[ins.rs] + ins.offset;
                    b.addrReady = true;
                    b.addrQ     = -1;
                } else {
                    ROBEntry &e = rob[baseROB];
                    if (e.ready) {
                        b.addrVal   = (int)e.value + ins.offset;
                        b.addrReady = true;
                        b.addrQ     = -1;
                    } else {
                        b.addrReady = false;
                        b.addrQ     = baseROB;
                    }
                }
            }
            rat[ins.rd].robIdx = rIdx;
            if (ins.status.issue < 0) ins.status.issue = cycle;
            pc++;
            break;
        }
        case OpKind::Store: {
            int slot = freeStore();
            if (slot < 0) return;
            int rIdx = allocROB(pc,false,-1,true);
            if (rIdx < 0) return;
            LSBuf &b = storeBuf[slot];
            b = LSBuf();
            b.busy   = true;
            b.isLoad = false;
            b.rs     = ins.rs;
            b.rt     = ins.rt;
            b.offset = ins.offset;
            b.inst   = &ins;
            b.robIdx = rIdx;
            {
                int baseROB = rat[ins.rt].robIdx;
                if (baseROB < 0) {
                    b.addrVal   = (int)regs[ins.rt] + ins.offset;
                    b.addrReady = true;
                    b.addrQ     = -1;
                } else {
                    ROBEntry &e = rob[baseROB];
                    if (e.ready) {
                        b.addrVal   = (int)e.value + ins.offset;
                        b.addrReady = true;
                        b.addrQ     = -1;
                    } else {
                        b.addrReady = false;
                        b.addrQ     = baseROB;
                    }
                }
            }
            {
                int valROB = rat[ins.rs].robIdx;
                if (valROB < 0) {
                    b.val      = regs[ins.rs];
                    b.valReady = true;
                    b.valQ     = -1;
                } else {
                    ROBEntry &e = rob[valROB];
                    if (e.ready) {
                        b.val      = e.value;
                        b.valReady = true;
                        b.valQ     = -1;
                    } else {
                        b.valReady = false;
                        b.valQ     = valROB;
                    }
                }
            }
            if (ins.status.issue < 0) ins.status.issue = cycle;
            pc++;
            break;
        }
        case OpKind::Beq:
        case OpKind::Bneq: {
            int slot = freeAdd();
            if (slot < 0) return;
            int rIdx = allocROB(pc,false,-1,false);
            if (rIdx < 0) return;
            ROBEntry &e = rob[rIdx];
            e.isBranch = true;

            RS &r = addRS[slot];
            r = RS();
            r.busy = true;
            r.op   = ins.kind;
            r.rs   = ins.rs;
            r.rt   = ins.rt;
            r.rd   = -1;
            r.inst = &ins;
            r.robIdx = rIdx;
            attachSrc(ins.rs, r.Vj, r.VjReady, r.Qj);
            attachSrc(ins.rt, r.Vk, r.VkReady, r.Qk);

            if (ins.status.issue < 0) ins.status.issue = cycle;

            if (!speculative) {
                // Model B: stall fetch until this branch commits
                nsBranchInFlight = true;
                nsBranchRobIdx   = rIdx;
                // pc will be updated at commit
            } else {
                BPredEntry &bp = bpred[pc];
                if (!bp.valid) { bp.valid = true; bp.predictTaken = false; }
                bool pred = bp.predictTaken;
                e.predictedTaken = pred;
                branchCount++;
                if (pred) pc = ins.targetPc;
                else      pc = pc + 1;
            }
            break;
        }
        default:
            break;
        }
    }

    // ----------------------------------------------------------
    // EXECUTE
    // ----------------------------------------------------------
    void stepExecute() {
        const Config &cfg = prog.cfg;
        for (size_t i=0;i<addRS.size();++i) {
            RS &r = addRS[i];
            if (!r.busy || !r.inst) continue;
            Instruction &ins = *r.inst;
            if (ins.status.execStart >= 0) continue;
            if (!r.VjReady || !r.VkReady) continue;
            if (ins.status.issue == cycle) continue;
            ins.status.execStart = cycle;
            ins.status.execComplete = cycle + cfg.addSubCycles;
        }
        for (size_t i=0;i<mulRS.size();++i) {
            RS &r = mulRS[i];
            if (!r.busy || !r.inst) continue;
            Instruction &ins = *r.inst;
            if (ins.status.execStart >= 0) continue;
            if (!r.VjReady || !r.VkReady) continue;
            if (ins.status.issue == cycle) continue;
            ins.status.execStart = cycle;
            int lat = (ins.kind == OpKind::Mul) ? cfg.mulCycles : cfg.divCycles;
            ins.status.execComplete = cycle + lat;
        }
        for (size_t i=0;i<loadBuf.size();++i) {
            LSBuf &b = loadBuf[i];
            if (!b.busy || !b.inst) continue;
            Instruction &ins = *b.inst;
            if (ins.status.execStart >= 0) continue;
            if (!b.addrReady) continue;
            if (ins.status.issue == cycle) continue;
            ins.status.execStart    = cycle;
            ins.status.execComplete = cycle + cfg.loadStoreCycles;
        }
        for (size_t i=0;i<storeBuf.size();++i) {
            LSBuf &b = storeBuf[i];
            if (!b.busy || !b.inst) continue;
            Instruction &ins = *b.inst;
            if (ins.status.execStart >= 0) continue;
            if (!b.addrReady || !b.valReady) continue;
            if (ins.status.issue == cycle) continue;
            ins.status.execStart    = cycle;
            ins.status.execComplete = cycle + cfg.loadStoreCycles;
        }
    }

    // ----------------------------------------------------------
    // BROADCAST + WRITEBACK
    // ----------------------------------------------------------
    void broadcast(int ridx, double val) {
        for (size_t i=0;i<addRS.size();++i) {
            RS &r = addRS[i];
            if (!r.busy) continue;
            if (r.Qj == ridx) { r.Qj=-1; r.Vj=val; r.VjReady=true; }
            if (r.Qk == ridx) { r.Qk=-1; r.Vk=val; r.VkReady=true; }
        }
        for (size_t i=0;i<mulRS.size();++i) {
            RS &r = mulRS[i];
            if (!r.busy) continue;
            if (r.Qj == ridx) { r.Qj=-1; r.Vj=val; r.VjReady=true; }
            if (r.Qk == ridx) { r.Qk=-1; r.Vk=val; r.VkReady=true; }
        }
        for (size_t i=0;i<loadBuf.size();++i) {
            LSBuf &b = loadBuf[i];
            if (!b.busy) continue;
            if (b.addrQ == ridx) {
                b.addrQ   = -1;
                b.addrVal = val;
                b.addrReady = true;
            }
        }
        for (size_t i=0;i<storeBuf.size();++i) {
            LSBuf &b = storeBuf[i];
            if (!b.busy) continue;
            if (b.addrQ == ridx) {
                b.addrQ   = -1;
                b.addrVal = val;
                b.addrReady = true;
            }
            if (b.valQ == ridx) {
                b.valQ    = -1;
                b.val     = val;
                b.valReady= true;
            }
        }
    }

    void stepWriteBack() {
        for (size_t i=0;i<loadBuf.size();++i) {
            LSBuf &b = loadBuf[i];
            if (!b.busy || !b.inst) continue;
            Instruction &ins = *b.inst;
            if (ins.status.execComplete >= 0 &&
                ins.status.execComplete <= cycle)
            {
                ROBEntry *e = getROB(b.robIdx);
                if (!e) { b.busy=false; b.inst=NULL; continue; }
                int addr = (int)b.addrVal;
                double val = 0.0;
                if (memory.count(addr)) val = memory[addr];
                e->value = val;
                e->ready = true;
                broadcast(b.robIdx,val);
                b.busy = false;
                b.inst = NULL;
            }
        }
        for (size_t i=0;i<storeBuf.size();++i) {
            LSBuf &b = storeBuf[i];
            if (!b.busy || !b.inst) continue;
            Instruction &ins = *b.inst;
            if (ins.status.execComplete >= 0 &&
                ins.status.execComplete <= cycle)
            {
                ROBEntry *e = getROB(b.robIdx);
                if (!e) { b.busy=false; b.inst=NULL; continue; }
                e->value = b.val;
                e->addr  = (int)b.addrVal;
                e->ready = true;
                b.busy = false;
                b.inst = NULL;
            }
        }
        for (size_t i=0;i<addRS.size();++i) {
            RS &r = addRS[i];
            if (!r.busy || !r.inst) continue;
            Instruction &ins = *r.inst;
            if (ins.status.execComplete >= 0 &&
                ins.status.execComplete <= cycle)
            {
                ROBEntry *e = getROB(r.robIdx);
                if (!e) { r.busy=false; r.inst=NULL; continue; }
                if (ins.kind == OpKind::Add) {
                    e->value = r.Vj + r.Vk;
                    e->ready = true;
                    broadcast(r.robIdx,e->value);
                } else if (ins.kind == OpKind::Sub) {
                    e->value = r.Vj - r.Vk;
                    e->ready = true;
                    broadcast(r.robIdx,e->value);
                } else if (ins.kind == OpKind::Beq ||
                           ins.kind == OpKind::Bneq) {
                    bool equal = (r.Vj == r.Vk);
                    bool taken = (ins.kind == OpKind::Beq) ? equal : !equal;
                    e->value = taken ? 1.0 : 0.0;
                    e->ready = true;
                }
                r.busy = false;
                r.inst = NULL;
            }
        }
        for (size_t i=0;i<mulRS.size();++i) {
            RS &r = mulRS[i];
            if (!r.busy || !r.inst) continue;
            Instruction &ins = *r.inst;
            if (ins.status.execComplete >= 0 &&
                ins.status.execComplete <= cycle)
            {
                ROBEntry *e = getROB(r.robIdx);
                if (!e) { r.busy=false; r.inst=NULL; continue; }
                if (ins.kind == OpKind::Mul) e->value = r.Vj*r.Vk;
                else                          e->value = r.Vj/r.Vk;
                e->ready = true;
                broadcast(r.robIdx,e->value);
                r.busy = false;
                r.inst = NULL;
            }
        }
    }
    // ----------------------------------------------------------
    // Flush younger speculative state on misprediction (Model C)
    // ----------------------------------------------------------
    void flushAfterMispredict(int branchRobIdx) {
        int idx = (branchRobIdx + 1) % (int)rob.size();
        while (idx != robTail) {
            rob[idx].valid = false;
            idx = (idx + 1) % (int)rob.size();
        }
        robTail  = (branchRobIdx + 1) % (int)rob.size();
        robCount = 1;   // keep only the branch

        for (size_t r = 0; r < rat.size(); ++r) {
            if (rat[r].robIdx != branchRobIdx)
                rat[r].robIdx = -1;
        }

        for (size_t i=0;i<addRS.size();++i) {
            RS &rs = addRS[i];
            if (rs.busy && rs.robIdx != branchRobIdx) {
                rs.busy = false;
                rs.inst = NULL;
                rs.robIdx = -1;
                rs.Qj = rs.Qk = -1;
            }
        }
        for (size_t i=0;i<mulRS.size();++i) {
            RS &rs = mulRS[i];
            if (rs.busy && rs.robIdx != branchRobIdx) {
                rs.busy = false;
                rs.inst = NULL;
                rs.robIdx = -1;
                rs.Qj = rs.Qk = -1;
            }
        }
        for (size_t i=0;i<loadBuf.size();++i) {
            LSBuf &b = loadBuf[i];
            if (b.busy && b.robIdx != branchRobIdx) {
                b.busy = false;
                b.inst = NULL;
                b.robIdx = -1;
                b.addrQ = b.valQ = -1;
            }
        }
        for (size_t i=0;i<storeBuf.size();++i) {
            LSBuf &b = storeBuf[i];
            if (b.busy && b.robIdx != branchRobIdx) {
                b.busy = false;
                b.inst = NULL;
                b.robIdx = -1;
                b.addrQ = b.valQ = -1;
            }
        }
    }

   // ----------------------------------------------------------
// COMMIT: up to 4 in-order commits per cycle
// ----------------------------------------------------------
void stepCommit() {
    const int MAX_COMMITS = 4;
    int commitsThisCycle = 0;

    while (commitsThisCycle < MAX_COMMITS && robCount > 0) {
        ROBEntry &e = rob[robHead];

        // Stop if the head is not a valid, ready entry.
        if (!e.valid || !e.ready)
            break;

        Instruction &ins =
            const_cast<Instruction&>(prog.instrs[e.instrIdx]);

        bool isBranch          = e.isBranch;
        bool mispredictHappened = false;

        // -------- Branch handling (same semantics as before) --------
        if (isBranch) {
            bool actualTaken = (e.value != 0.0);

            if (speculative) {
                // Model C: branch prediction + speculation
                bool pred = e.predictedTaken;
                if (pred != actualTaken) {
                    // Misprediction: fix predictor, redirect PC, flush younger
                    mispredCount++;
                    bpred[e.instrIdx].valid         = true;
                    bpred[e.instrIdx].predictTaken  = actualTaken;

                    if (actualTaken)
                        pc = ins.targetPc;
                    else
                        pc = e.instrIdx + 1;

                    flushAfterMispredict(robHead);
                    mispredictHappened = true;
                } else {
                    // Correct prediction: just update predictor state
                    bpred[e.instrIdx].valid        = true;
                    bpred[e.instrIdx].predictTaken = actualTaken;
                }
            } else {
                // Model B: no speculation, branch resolves PC at commit
                if (actualTaken)
                    pc = ins.targetPc;
                else
                    pc = e.instrIdx + 1;

                nsBranchInFlight = false;
                nsBranchRobIdx   = -1;
            }
        }

        // -------- Architectural updates (same semantics as before) --------
        if (e.hasDest && e.destReg >= 0) {
            // Write back register result
            regs[e.destReg] = e.value;
            // Clear RAT mapping if this ROB entry was still the live mapping
            if (rat[e.destReg].robIdx == robHead)
                rat[e.destReg].robIdx = -1;
        }

        if (e.isStore) {
            // Perform the store
            memory[e.addr] = e.value;
        }

        // Record commit cycle in instruction status
        if (ins.status.commit < 0)
            ins.status.commit = cycle;
        committed++;

        // Retire this ROB entry
        e.valid = false;
        robHead = (robHead + 1) % (int)rob.size();
        robCount--;
        commitsThisCycle++;

        // After a misprediction flush in Model C:
        //  - younger entries have been invalidated
        //  - this branch is now committed
        // It is simplest and safe to stop committing further this cycle.
        if (isBranch && speculative) {
            break;
        }
    }
}


    // ----------------------------------------------------------
    // UI helpers
    // ----------------------------------------------------------
    bool prompt() {
        std::cout << "Press Enter to advance, or 'q' + Enter to quit: ";
        std::cout.flush();
        std::string s;
        std::getline(std::cin, s);
        if (!s.empty() && (s[0]=='q' || s[0]=='Q')) {
            std::cout << "Stopping at cycle " << cycle << "\n";
            return false;
        }
        return true;
    }

    void printState() {
        std::cout << "\n============================================================\n";
        std::cout << "       " << (speculative ? "Model C" : "Model B")
                  << " - Cycle " << cycle << "\n";
        std::cout << "============================================================\n\n";

        line("+---------------- REGISTER FILE ----------------+");
        std::cout << "| ";
        for (size_t i=0;i<regs.size();++i)
            std::cout << "F" << i << "=" << regs[i] << " ";
        std::cout << "|\n";
        line("+------------------------------------------------+\n");

        line("+---------------- INSTRUCTION STATUS ----------------+");
        std::cout << "Idx  Op   Rd  Rs  Rt | Iss ExSt ExCm Cmt\n";
        std::cout << "------------------------------------------------\n";
        for (size_t i=0;i<prog.instrs.size();++i) {
            const Instruction &ins = prog.instrs[i];
            std::string opDisp = dispOp3(ins);
            std::cout << std::setw(3) << i << "  "
                      << std::setw(3) << opDisp << " "
                      << std::setw(3) << fmtInt(ins.rd) << " "
                      << std::setw(3) << fmtInt(ins.rs) << " "
                      << std::setw(3) << fmtInt(ins.rt) << " | "
                      << std::setw(3) << fmtInt(ins.status.issue) << " "
                      << std::setw(4) << fmtInt(ins.status.execStart) << " "
                      << std::setw(4) << fmtInt(ins.status.execComplete) << " "
                      << std::setw(3) << fmtInt(ins.status.commit) << "\n";
        }
        line("+------------------------------------------------+\n");

        line("+-------------------- RAT (reg->ROB) --------------------+");
        std::cout << "Reg: ";
        for (size_t i=0;i<rat.size();++i)
            std::cout << "F" << i << ":" << fmtInt(rat[i].robIdx) << " ";
        std::cout << "\n";
        line("+--------------------------------------------------------+\n");

        line("+----------------------- ROB (valid only) -----------------------+");
        std::cout << "Idx Vld IIdx Rdy DestR St Br Pred  Val      Addr\n";
        std::cout << "-----------------------------------------------------------\n";
        for (size_t i=0;i<rob.size();++i) {
            const ROBEntry &re = rob[i];
            if (!re.valid) continue;
            std::cout << std::setw(3) << i << "  "
                      << (re.valid?1:0) << "   "
                      << std::setw(3) << fmtInt(re.instrIdx) << "  "
                      << (re.ready?1:0) << "   "
                      << std::setw(3) << fmtInt(re.destReg) << "   "
                      << (re.isStore?1:0) << "  "
                      << (re.isBranch?1:0) << "   "
                      << (re.predictedTaken?1:0) << "   "
                      << std::setw(8) << re.value << " "
                      << re.addr << "\n";
        }
        line("+-----------------------------------------------------------+\n");

        line("+------------------- ADD/SUB RS -------------------+");
        std::cout << "Idx Busy Op  Rd  Rs  Rt | VjR Qj  VkR Qk ROB\n";
        std::cout << "------------------------------------------------------\n";
        for (size_t i=0;i<addRS.size();++i) {
            const RS &r = addRS[i];
            std::cout << std::setw(3) << i << "  "
                      << (r.busy?'Y':'N') << "   "
                      << std::setw(3) << (int)r.op << " "
                      << std::setw(3) << fmtInt(r.rd) << " "
                      << std::setw(3) << fmtInt(r.rs) << " "
                      << std::setw(3) << fmtInt(r.rt) << " | "
                      << (r.VjReady?1:0) << "  "
                      << std::setw(3) << fmtInt(r.Qj) << "  "
                      << (r.VkReady?1:0) << "  "
                      << std::setw(3) << fmtInt(r.Qk) << " "
                      << std::setw(3) << fmtInt(r.robIdx) << "\n";
        }
        line("+--------------------------------------------------+\n");

        line("+------------------- MUL/DIV RS -------------------+");
        std::cout << "Idx Busy Op  Rd  Rs  Rt | VjR Qj  VkR Qk ROB\n";
        std::cout << "------------------------------------------------------\n";
        for (size_t i=0;i<mulRS.size();++i) {
            const RS &r = mulRS[i];
            std::cout << std::setw(3) << i << "  "
                      << (r.busy?'Y':'N') << "   "
                      << std::setw(3) << (int)r.op << " "
                      << std::setw(3) << fmtInt(r.rd) << " "
                      << std::setw(3) << fmtInt(r.rs) << " "
                      << std::setw(3) << fmtInt(r.rt) << " | "
                      << (r.VjReady?1:0) << "  "
                      << std::setw(3) << fmtInt(r.Qj) << "  "
                      << (r.VkReady?1:0) << "  "
                      << std::setw(3) << fmtInt(r.Qk) << " "
                      << std::setw(3) << fmtInt(r.robIdx) << "\n";
        }
        line("+--------------------------------------------------+\n");

        line("+-------------------- LOAD BUFFERS --------------------+");
        std::cout << "Idx Busy AddrR AddrQ ROB\n";
        std::cout << "-------------------------------\n";
        for (size_t i=0;i<loadBuf.size();++i) {
            const LSBuf &b = loadBuf[i];
            std::cout << std::setw(3) << i << "  "
                      << (b.busy?'Y':'N') << "    "
                      << (b.addrReady?1:0) << "    "
                      << std::setw(3) << fmtInt(b.addrQ) << "  "
                      << std::setw(3) << fmtInt(b.robIdx) << "\n";
        }
        line("+------------------------------------------------------+\n");

        line("+-------------------- STORE BUFFERS -------------------+");
        std::cout << "Idx Busy AddrR AddrQ ValR ValQ ROB\n";
        std::cout << "-----------------------------------------\n";
        for (size_t i=0;i<storeBuf.size();++i) {
            const LSBuf &b = storeBuf[i];
            std::cout << std::setw(3) << i << "  "
                      << (b.busy?'Y':'N') << "    "
                      << (b.addrReady?1:0) << "    "
                      << std::setw(3) << fmtInt(b.addrQ) << "   "
                      << (b.valReady?1:0) << "   "
                      << std::setw(3) << fmtInt(b.valQ) << "  "
                      << std::setw(3) << fmtInt(b.robIdx) << "\n";
        }
        line("+------------------------------------------------------+\n");
    }

    void printSummary(const std::string &label, int committed, double cpi) {
        std::cout << "\n================ " << label << " ================\n";
        std::cout << "Final registers:\n";
        for (size_t i=0;i<regs.size();++i)
            std::cout << "F" << i << " = " << regs[i] << "\n";

        std::cout << "\nFinal memory (touched):\n";
        for (std::map<int,double>::const_iterator it = memory.begin();
             it != memory.end(); ++it)
        {
            std::cout << "[" << it->first << "] = " << it->second << "\n";
        }

        std::cout << "\nTotal cycles     : " << cycle << "\n";
        std::cout << "Committed instrs : " << committed << "\n";
        std::cout << "CPI              : " << cpi << "\n";
        if (speculative) {
            double rate = branchCount>0 ? (double)mispredCount/(double)branchCount : 0.0;
            std::cout << "Branches         : " << branchCount << "\n";
            std::cout << "Mispredictions   : " << mispredCount << "\n";
            std::cout << "Mispred rate     : " << rate << "\n";
        }
        std::cout << "\n";
    }
};

} // namespace ModelBC

// ============================================================================
// main
// ============================================================================

enum class Mode { A, B, C };

int main(int argc, char *argv[]) {
    Mode mode = Mode::A;
    std::string file = "source.txt";

    if (argc >= 2) {
        std::string m = argv[1];
        if (m=="A" || m=="a") mode = Mode::A;
        else if (m=="B" || m=="b") mode = Mode::B;
        else if (m=="C" || m=="c") mode = Mode::C;
        else {
            std::cerr << "Unknown mode '" << m << "', using Model A.\n";
        }
    }
    if (argc >= 3) {
        file = argv[2];
    }

    Program prog = parseProgram(file);

    switch (mode) {
    case Mode::A: {
        ModelA::Sim sim(prog);
        sim.run();
        break;
    }
    case Mode::B: {
        ModelBC::Sim sim(prog,false);
        sim.run();
        break;
    }
    case Mode::C: {
        ModelBC::Sim sim(prog,true);
        sim.run();
        break;
    }
    }

    return 0;
}

