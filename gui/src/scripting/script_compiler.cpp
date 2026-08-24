#include "script_compiler.h"

#include <QVector>

#include <cmath>
#include <cstring>

#include "../model/configuration.h"
#include "../model/device_mapper.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "script_vm.h"
}

namespace ct {

// ---------------------------------------------------------------- symbols

// The mapper's signal table IS the device's: index i in this list is signal slot
// i on the unit, so a script's sig("name") and the running firmware agree by
// construction rather than by a second lookup table.
static ScriptSymbols symbolsFromTables(const DeviceTables &tables)
{
    ScriptSymbols out;
    for (int i = 0; i < tables.signalConfigs.size(); ++i) {
        const auto &sig = tables.signalConfigs[i];
        const QString name =
            QString::fromUtf8(sig.label, qstrnlen(sig.label, SIGNAL_LABEL_LEN));
        if (!name.isEmpty()) {
            out.signalIndex.insert(name, quint16(i));
        }
    }
    return out;
}

bool ScriptSymbols::fromConfiguration(const Configuration &config, ScriptSymbols *out,
                                      QString *error)
{
    const MappingResult mapped = mapToDevice(config);
    if (!mapped.ok()) {
        if (error) {
            *error = mapped.errors.join(QStringLiteral("\n"));
        }
        return false;
    }
    *out = symbolsFromTables(mapped.tables);
    return true;
}

namespace {

// An image -> the chunk table a Send writes. Shared by the two ways a document
// can have a script (compiled from source, or retained from a device) so the
// two cannot chunk it differently — which for the retained path is the whole
// feature: re-chunking a retained image has to reproduce the bytes the device
// gave back, not merely something equivalent.
//
// The image is padded to a whole number of chunks with zeros. Padding is
// harmless because the header's code_bytes — not the chunk count — says where
// the code ends; the count only bounds it. Zero happens to decode as an
// arithmetic instruction rather than something structural, but it is never
// reached, and script_verify() checks the bound either way.
bool chunkImage(const QByteArray &image, QVector<ScriptChunk> *chunks, QString *error)
{
    const int chunkCount =
        (image.size() + int(sizeof(ScriptChunk)) - 1) / int(sizeof(ScriptChunk));
    if (chunkCount > MAX_SCRIPT_CHUNKS) {
        if (error) {
            *error = QObject::tr("The device script is %1 bytes, more than the "
                                 "%2 bytes a device can store.")
                         .arg(image.size())
                         .arg(MAX_SCRIPT_CHUNKS * int(sizeof(ScriptChunk)));
        }
        return false;
    }
    chunks->resize(chunkCount);
    std::memset(chunks->data(), 0, size_t(chunkCount) * int(sizeof(ScriptChunk)));
    std::memcpy(chunks->data(), image.constData(), size_t(image.size()));
    return true;
}

} // namespace

bool ScriptCompiler::attachTo(const Configuration &config, const ScriptSymbols &symbols,
                              QVector<ScriptChunk> *chunks, QString *error)
{
    chunks->clear();

    // The precedence rule, at the one place that turns a document's script into
    // bytes for a device. Configuration::setScript() has already made "a source
    // AND a stale retained image" unrepresentable, so this is a two-way branch
    // and not a priority contest: whichever half the document holds is the
    // script, and there is never a second candidate to weigh it against.
    if (!config.hasScriptSource()) {
        const QByteArray &retained = config.scriptBytecode();
        if (retained.isEmpty()) {
            return true; // no script at all: zero chunks, which is what clears one
        }
        // Validated AGAIN, here, and the second check is the one that matters
        // most. The image may have reached this document through a .ct3 written
        // months ago, hand-edited, or truncated by whatever copied it — and the
        // Send that would install it runs AFTER CLEAR_CONFIG has erased the
        // unit. A corrupt image written back leaves the device running neither
        // the old script nor a new one, so the user loses both; refusing before
        // the transfer starts costs them nothing.
        QString reason;
        if (!validateScriptImage(retained, &reason)) {
            if (error) {
                *error = QObject::tr(
                             "The compiled script kept from a device is not a valid script "
                             "image (%1), so it cannot be sent. Nothing has been written to "
                             "the device. Read the device again, or write a script in the "
                             "Script Editor to replace it.")
                             .arg(reason);
            }
            return false;
        }
        return chunkImage(retained, chunks, error);
    }

    const Result r = compile(config.scriptSource(), symbols);
    if (!r.ok) {
        if (error) {
            *error = r.errorLine > 0
                         ? QObject::tr("Device script, line %1: %2")
                               .arg(r.errorLine).arg(r.error)
                         : QObject::tr("Device script: %1").arg(r.error);
        }
        return false;
    }
    return chunkImage(r.image, chunks, error);
}

MappingResult mapWithScript(const Configuration &config)
{
    MappingResult mapped = mapToDevice(config);
    if (!mapped.ok()) {
        // A configuration that will not map cannot resolve channel names either,
        // so there is nothing useful to say about the script yet — the mapping
        // errors are the ones to fix first.
        return mapped;
    }
    // attachTo runs even when the document has no SOURCE, which it did not used
    // to. It has to: a document whose script is a retained device image has no
    // source at all, and the old early return would have sent it nowhere — the
    // exact silent strip this feature removes, reintroduced one level up.

    // Reuse the map just built rather than calling fromConfiguration(), which
    // would map the whole configuration a second time.
    const ScriptSymbols symbols = symbolsFromTables(mapped.tables);

    QString error;
    if (!ScriptCompiler::attachTo(config, symbols, &mapped.tables.scriptChunks, &error)) {
        mapped.errors.append(error);
    }
    return mapped;
}

namespace {

// ---------------------------------------------------------------- lexer

enum class T {
    End, Name, Number, String,
    // keywords
    Local, Function, If, Then, ElseIf, Else, While, Do, For, End_, Return, Break,
    And, Or, Not, True, False, Nil,
    // symbols
    Assign, Eq, Ne, Lt, Le, Gt, Ge,
    Plus, Minus, Star, Slash, Percent, Caret, Hash, Concat,
    LParen, RParen, Comma, Semi,
    // anything the subset does not use but Lua does (tables, etc.)
    Unsupported,
};

struct Token {
    T type = T::End;
    QString text;
    double number = 0;
    int line = 1;
};

class Lexer
{
public:
    explicit Lexer(const QString &src) : m_src(src) {}

    // Throws QString on a lex error (caught by the compiler's try).
    Token next()
    {
        skipSpaceAndComments();
        Token t;
        t.line = m_line;
        if (m_pos >= m_src.size()) {
            t.type = T::End;
            return t;
        }
        const QChar c = m_src[m_pos];

        if (c.isLetter() || c == QLatin1Char('_')) {
            const int start = m_pos;
            while (m_pos < m_src.size()
                   && (m_src[m_pos].isLetterOrNumber() || m_src[m_pos] == QLatin1Char('_'))) {
                ++m_pos;
            }
            t.text = m_src.mid(start, m_pos - start);
            t.type = keyword(t.text);
            return t;
        }
        if (c.isDigit() || (c == QLatin1Char('.') && m_pos + 1 < m_src.size()
                            && m_src[m_pos + 1].isDigit())) {
            const int start = m_pos;
            bool hex = false;
            if (c == QLatin1Char('0') && m_pos + 1 < m_src.size()
                && (m_src[m_pos + 1] == QLatin1Char('x') || m_src[m_pos + 1] == QLatin1Char('X'))) {
                hex = true;
                m_pos += 2;
                while (m_pos < m_src.size() && isHex(m_src[m_pos])) {
                    ++m_pos;
                }
            } else {
                while (m_pos < m_src.size()
                       && (m_src[m_pos].isDigit() || m_src[m_pos] == QLatin1Char('.')
                           || m_src[m_pos] == QLatin1Char('e') || m_src[m_pos] == QLatin1Char('E')
                           || ((m_src[m_pos] == QLatin1Char('+') || m_src[m_pos] == QLatin1Char('-'))
                               && (m_src[m_pos - 1] == QLatin1Char('e')
                                   || m_src[m_pos - 1] == QLatin1Char('E'))))) {
                    ++m_pos;
                }
            }
            const QString text = m_src.mid(start, m_pos - start);
            bool okNum = false;
            t.number = hex ? double(text.mid(2).toULongLong(&okNum, 16)) : text.toDouble(&okNum);
            if (!okNum) {
                fail(QStringLiteral("'%1' is not a number I can read").arg(text));
            }
            t.type = T::Number;
            t.text = text;
            return t;
        }
        if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
            const QChar quote = c;
            ++m_pos;
            QString s;
            while (m_pos < m_src.size() && m_src[m_pos] != quote) {
                if (m_src[m_pos] == QLatin1Char('\\') && m_pos + 1 < m_src.size()) {
                    ++m_pos; // the syntax pre-check already validated escapes
                }
                s += m_src[m_pos];
                ++m_pos;
            }
            if (m_pos >= m_src.size()) {
                fail(QStringLiteral("unterminated string"));
            }
            ++m_pos;
            t.type = T::String;
            t.text = s;
            return t;
        }

        ++m_pos;
        switch (c.unicode()) {
        case '+': t.type = T::Plus; break;
        case '-': t.type = T::Minus; break;
        case '*': t.type = T::Star; break;
        case '/': t.type = T::Slash; break;
        case '%': t.type = T::Percent; break;
        case '^': t.type = T::Caret; break;
        case '#': t.type = T::Hash; break;
        case '(': t.type = T::LParen; break;
        case ')': t.type = T::RParen; break;
        case ',': t.type = T::Comma; break;
        case ';': t.type = T::Semi; break;
        case '=':
            if (peek('=')) { ++m_pos; t.type = T::Eq; } else { t.type = T::Assign; }
            break;
        case '~':
            if (peek('=')) { ++m_pos; t.type = T::Ne; }
            else { fail(QStringLiteral("'~' is only valid as '~=' here")); }
            break;
        case '<':
            if (peek('=')) { ++m_pos; t.type = T::Le; } else { t.type = T::Lt; }
            break;
        case '>':
            if (peek('=')) { ++m_pos; t.type = T::Ge; } else { t.type = T::Gt; }
            break;
        case '.':
            if (peek('.')) { ++m_pos; t.type = T::Concat; }
            else { t.type = T::Unsupported; t.text = QStringLiteral("."); }
            break;
        default:
            t.type = T::Unsupported;
            t.text = QString(c);
            break;
        }
        return t;
    }

    int line() const { return m_line; }

private:
    static bool isHex(QChar c)
    {
        return c.isDigit() || (c.toLower() >= QLatin1Char('a') && c.toLower() <= QLatin1Char('f'));
    }
    bool peek(char c) const
    {
        return m_pos < m_src.size() && m_src[m_pos] == QLatin1Char(c);
    }
    void skipSpaceAndComments()
    {
        for (;;) {
            while (m_pos < m_src.size() && m_src[m_pos].isSpace()) {
                if (m_src[m_pos] == QLatin1Char('\n')) {
                    ++m_line;
                }
                ++m_pos;
            }
            if (m_pos + 1 < m_src.size() && m_src[m_pos] == QLatin1Char('-')
                && m_src[m_pos + 1] == QLatin1Char('-')) {
                while (m_pos < m_src.size() && m_src[m_pos] != QLatin1Char('\n')) {
                    ++m_pos;
                }
                continue;
            }
            return;
        }
    }
    static T keyword(const QString &s)
    {
        if (s == QLatin1String("local")) return T::Local;
        if (s == QLatin1String("function")) return T::Function;
        if (s == QLatin1String("if")) return T::If;
        if (s == QLatin1String("then")) return T::Then;
        if (s == QLatin1String("elseif")) return T::ElseIf;
        if (s == QLatin1String("else")) return T::Else;
        if (s == QLatin1String("while")) return T::While;
        if (s == QLatin1String("do")) return T::Do;
        if (s == QLatin1String("for")) return T::For;
        if (s == QLatin1String("end")) return T::End_;
        if (s == QLatin1String("return")) return T::Return;
        if (s == QLatin1String("break")) return T::Break;
        if (s == QLatin1String("and")) return T::And;
        if (s == QLatin1String("or")) return T::Or;
        if (s == QLatin1String("not")) return T::Not;
        if (s == QLatin1String("true")) return T::True;
        if (s == QLatin1String("false")) return T::False;
        if (s == QLatin1String("nil")) return T::Nil;
        return T::Name;
    }
    [[noreturn]] void fail(const QString &msg) const { throw QStringLiteral("%1").arg(msg); }

    QString m_src;
    int m_pos = 0;
    int m_line = 1;
};

// ---------------------------------------------------------------- compiler

// Thrown for every compile error; carries the line so the editor can point.
struct CompileError {
    QString message;
    int line;
};

// An intrinsic that lowers to a single arithmetic opcode.
struct Intrinsic {
    const char *name;
    uint8_t op;
    int arity;
};

const Intrinsic kIntrinsics[] = {
    { "abs", MATH_OP_ABS, 1 },      { "neg", MATH_OP_NEG, 1 },
    { "sqrt", MATH_OP_SQRT, 1 },    { "floor", MATH_OP_FLOOR, 1 },
    { "ceil", MATH_OP_CEIL, 1 },    { "round", MATH_OP_ROUND, 1 },
    { "min", MATH_OP_MIN, 2 },      { "max", MATH_OP_MAX, 2 },
    { "clamp", MATH_OP_CLAMP, 3 },  { "lerp", MATH_OP_LERP, 3 },
    { "select", MATH_OP_SELECT, 3 },{ "wrap", MATH_OP_WRAP, 3 },
};

// Rejected by name, each with the reason. Being explicit beats "unknown
// function 'sin'" — the user needs to know it is a deliberate exclusion and
// why, or they will assume a bug and try harder.
struct Banned {
    const char *name;
    const char *reason;
};

const Banned kBanned[] = {
    { "sin", "trigonometric and exponential functions are not available: IEEE-754 "
             "does not require them to be correctly rounded, so the device and the "
             "desktop simulator could disagree by a rounding step and branch "
             "differently" },
    { "cos", "see sin" }, { "tan", "see sin" }, { "asin", "see sin" },
    { "acos", "see sin" }, { "atan", "see sin" },
    { "exp", "see sin" }, { "log", "see sin" }, { "pow", "see sin" },
    { "print", "scripts have no output; write to a channel with setSig instead" },
    { "require", "scripts cannot load modules" },
    { "pcall", "scripts cannot catch errors" },
};

class Compiler
{
public:
    Compiler(const QString &source, const ScriptSymbols &symbols)
        : m_lex(source), m_symbols(symbols)
    {
    }

    ScriptCompiler::Result run()
    {
        ScriptCompiler::Result r;
        try {
            advance();
            parseChunk();
            r.image = buildImage();
            r.ok = true;
            r.instructionCount = m_code.size();
            r.stateUsed = m_stateCount;
            r.stateDeclared = m_stateDeclared;
            r.registersUsed = m_maxReg;
            r.straightLineCost = m_straightLineCost;
            r.loopsPresent = m_loopsPresent;
            r.warnings = m_warnings;
        } catch (const CompileError &e) {
            r.ok = false;
            r.error = e.message;
            r.errorLine = e.line;
        } catch (const QString &e) {
            r.ok = false;
            r.error = e;
            r.errorLine = m_lex.line();
        }
        return r;
    }

private:
    // ------------------------------------------------------------ errors

    [[noreturn]] void fail(const QString &msg) const { throw CompileError{ msg, m_tok.line }; }
    [[noreturn]] void failAt(const QString &msg, int line) const
    {
        throw CompileError{ msg, line };
    }

    // ------------------------------------------------------------ tokens

    void advance() { m_prev = m_tok; m_tok = m_lex.next(); }
    bool check(T t) const { return m_tok.type == t; }
    bool accept(T t)
    {
        if (check(t)) { advance(); return true; }
        return false;
    }
    void expect(T t, const char *what)
    {
        if (!accept(t)) {
            fail(QStringLiteral("expected %1").arg(QLatin1String(what)));
        }
    }

    // ------------------------------------------------------------ emit

    int emitInstr(uint8_t op, uint8_t dst, uint8_t a, uint8_t b, uint32_t imm)
    {
        if (m_code.size() >= int(SCRIPT_MAX_INSTRUCTIONS)) {
            fail(QStringLiteral("script is too large: over %1 instructions")
                     .arg(SCRIPT_MAX_INSTRUCTIONS));
        }
        ScriptInstr in{};
        in.op = op; in.dst = dst; in.a = a; in.b = b; in.imm = imm;
        m_code.append(in);
        // Straight-line cost: what one pass costs with no loop iterating. A
        // floor, and labelled as one — see Result::straightLineCost.
        m_straightLineCost += script_op_cost(op);
        return m_code.size() - 1;
    }
    void patchJump(int at, int target) { m_code[at].imm = uint32_t(target); }
    int here() const { return m_code.size(); }

    // ------------------------------------------------------------ registers

    // Named locals occupy [0, m_localTop); temporaries are allocated above and
    // released at the end of every statement. Nothing spills — there is no
    // stack to spill to — so running out is a compile error that names the
    // limit rather than silently reusing a live register.
    int allocTemp()
    {
        if (m_tempTop >= int(SCRIPT_NUM_REGS)) {
            fail(QStringLiteral(
                     "expression needs more than the %1 available registers - "
                     "split it into steps using local variables")
                     .arg(SCRIPT_NUM_REGS));
        }
        const int r = m_tempTop++;
        if (m_tempTop > m_maxReg) {
            m_maxReg = m_tempTop;
        }
        return r;
    }
    void releaseTemps() { m_tempTop = m_localTop; }

    int declareLocal(const QString &name, bool isState, int stateSlot)
    {
        for (int i = m_locals.size() - 1; i >= m_scopeBase; --i) {
            if (m_locals[i].name == name) {
                fail(QStringLiteral("'%1' is already declared in this scope").arg(name));
            }
        }
        Local l;
        l.name = name;
        l.isState = isState;
        if (isState) {
            l.slot = stateSlot;
        } else {
            if (m_localTop >= int(SCRIPT_NUM_REGS)) {
                fail(QStringLiteral("too many local variables (limit %1)")
                         .arg(SCRIPT_NUM_REGS));
            }
            l.slot = m_localTop++;
            if (m_localTop > m_maxReg) {
                m_maxReg = m_localTop;
            }
        }
        m_locals.append(l);
        m_tempTop = m_localTop;
        return l.slot;
    }

    struct Local {
        QString name;
        int slot = 0;
        bool isState = false;
    };
    const Local *findLocal(const QString &name) const
    {
        for (int i = m_locals.size() - 1; i >= 0; --i) {
            if (m_locals[i].name == name) {
                return &m_locals[i];
            }
        }
        return nullptr;
    }

    // ------------------------------------------------------------ chunk

    void parseChunk()
    {
        // File scope carries state declarations and the hook definitions. Order
        // is free, but state must be declared before a hook that uses it, which
        // falls out of single-pass compilation and is worth saying plainly in
        // the error when it bites.
        while (!check(T::End)) {
            if (accept(T::Semi)) {
                continue;
            }
            if (check(T::Local)) {
                parseFileScopeLocal();
            } else if (check(T::Function)) {
                parseFunction();
            } else {
                fail(QStringLiteral(
                    "only 'local <name> = state(<initial>)' declarations and "
                    "'function on_tick() ... end' are allowed at the top level"));
            }
        }
        if (m_entryTick < 0) {
            failAt(QStringLiteral("no 'function on_tick() ... end' was defined - a "
                                  "script needs a hook or it would never run"),
                   1);
        }
    }

    void parseFileScopeLocal()
    {
        expect(T::Local, "'local'");
        if (!check(T::Name)) {
            fail(QStringLiteral("expected a name after 'local'"));
        }
        const QString name = m_tok.text;
        const int line = m_tok.line;
        advance();
        expect(T::Assign, "'=' - a top-level local must be 'local x = state(0)'");
        if (!(check(T::Name) && m_tok.text == QLatin1String("state"))) {
            failAt(QStringLiteral(
                       "top-level locals must be persistent state: write "
                       "'local %1 = state(0)'. Ordinary locals belong inside a hook.")
                       .arg(name),
                   line);
        }
        advance();
        expect(T::LParen, "'(' after state");
        if (!check(T::Number)) {
            fail(QStringLiteral("state() needs a constant initial value"));
        }
        const double initial = m_tok.number;
        advance();
        expect(T::RParen, "')'");

        if (m_stateCount >= int(SCRIPT_NUM_STATE)) {
            fail(QStringLiteral("too many state variables (limit %1)").arg(SCRIPT_NUM_STATE));
        }
        const int slot = m_stateCount++;
        ++m_stateDeclared;
        declareLocal(name, /*isState=*/true, slot);
        m_stateInit.append(qMakePair(slot, float(initial)));
    }

    void parseFunction()
    {
        expect(T::Function, "'function'");
        if (!check(T::Name)) {
            fail(QStringLiteral("expected a function name"));
        }
        const QString name = m_tok.text;
        const int line = m_tok.line;
        advance();
        expect(T::LParen, "'('");
        if (!check(T::RParen)) {
            fail(QStringLiteral("hooks take no parameters"));
        }
        expect(T::RParen, "')'");

        if (name == QLatin1String("on_rx") || name == QLatin1String("on_tx")) {
            failAt(QStringLiteral(
                       "'%1' is a phase-3 hook this firmware does not run yet - only "
                       "on_tick is available")
                       .arg(name),
                   line);
        }
        if (name != QLatin1String("on_tick")) {
            failAt(QStringLiteral("'%1' is not a hook - the only hook is on_tick").arg(name),
                   line);
        }
        if (m_entryTick >= 0) {
            failAt(QStringLiteral("on_tick is defined more than once"), line);
        }

        // The hook's entry is the first instruction of its prologue.
        m_entryTick = emitStateInitPrologue();

        const int scopeBase = m_locals.size();
        const int savedScopeBase = m_scopeBase;
        m_scopeBase = scopeBase;
        parseBlock();
        expect(T::End_, "'end' to close on_tick");
        closeScope(scopeBase);
        m_scopeBase = savedScopeBase;

        // Patch every `return` to land on the trailing HALT.
        const int haltAt = emitInstr(SCRIPT_OP_HALT, 0, 0, 0, 0);
        for (int at : m_returnPatches) {
            patchJump(at, haltAt);
        }
        m_returnPatches.clear();
    }

    // State variables are initialised on the FIRST tick only. There is no
    // separate init hook, and zeroed state (which is what a fresh load gives)
    // cannot be told from "the user chose 0" — so a sentinel slot records
    // whether the prologue has run. It costs one state slot and four
    // instructions once, which is cheaper than an init-hook opcode in the ABI.
    int emitStateInitPrologue()
    {
        if (m_stateInit.isEmpty()) {
            return here();
        }
        if (m_stateCount >= int(SCRIPT_NUM_STATE)) {
            fail(QStringLiteral("no state slot left for the initialiser flag "
                                "(limit %1)")
                     .arg(SCRIPT_NUM_STATE));
        }
        const int flagSlot = m_stateCount++;
        const int entry = here();

        const int r = allocTemp();
        emitInstr(SCRIPT_OP_LOADST, uint8_t(r), 0, 0, uint32_t(flagSlot));
        const int skip = emitInstr(SCRIPT_OP_JNZ, 0, uint8_t(r), 0, 0); // patched below
        for (const auto &init : m_stateInit) {
            emitInstr(SCRIPT_OP_LOADK, uint8_t(r), 0, 0, floatBits(init.second));
            emitInstr(SCRIPT_OP_STOREST, 0, uint8_t(r), 0, uint32_t(init.first));
        }
        emitInstr(SCRIPT_OP_LOADK, uint8_t(r), 0, 0, floatBits(1.0f));
        emitInstr(SCRIPT_OP_STOREST, 0, uint8_t(r), 0, uint32_t(flagSlot));
        patchJump(skip, here());
        releaseTemps();
        return entry;
    }

    void closeScope(int base)
    {
        while (m_locals.size() > base) {
            if (!m_locals.last().isState) {
                --m_localTop;
            }
            m_locals.removeLast();
        }
        m_tempTop = m_localTop;
    }

    // ------------------------------------------------------------ statements

    void parseBlock()
    {
        for (;;) {
            if (check(T::End_) || check(T::Else) || check(T::ElseIf) || check(T::End)) {
                return;
            }
            parseStatement();
        }
    }

    void parseStatement()
    {
        if (accept(T::Semi)) {
            return;
        }
        if (check(T::Local)) { parseLocalStatement(); return; }
        if (check(T::If))    { parseIf();  return; }
        if (check(T::While)) { parseWhile(); return; }
        if (check(T::For))   { parseFor(); return; }
        if (check(T::Do))    { parseDoBlock(); return; }
        if (check(T::Return)) {
            advance();
            if (!check(T::End_) && !check(T::Else) && !check(T::ElseIf) && !check(T::Semi)) {
                fail(QStringLiteral("a hook cannot return a value"));
            }
            m_returnPatches.append(emitInstr(SCRIPT_OP_JMP, 0, 0, 0, 0));
            return;
        }
        if (check(T::Break)) {
            advance();
            if (m_breakPatches.isEmpty()) {
                fail(QStringLiteral("'break' outside a loop"));
            }
            m_breakPatches.last().append(emitInstr(SCRIPT_OP_JMP, 0, 0, 0, 0));
            return;
        }
        if (check(T::Function)) {
            fail(QStringLiteral("functions can only be defined at the top level, and "
                                "only on_tick"));
        }
        parseAssignmentOrCall();
        releaseTemps();
    }

    void parseDoBlock()
    {
        expect(T::Do, "'do'");
        const int base = m_locals.size();
        parseBlock();
        expect(T::End_, "'end'");
        closeScope(base);
    }

    void parseLocalStatement()
    {
        expect(T::Local, "'local'");
        if (!check(T::Name)) {
            fail(QStringLiteral("expected a name after 'local'"));
        }
        const QString name = m_tok.text;
        advance();
        expect(T::Assign, "'=' - locals must be initialised");
        if (check(T::Name) && m_tok.text == QLatin1String("state")) {
            fail(QStringLiteral("state() variables must be declared at the top level, "
                                "outside the hook, so they persist across ticks"));
        }
        // The local count is checked BEFORE the expression so exhausting it
        // reports "too many local variables" rather than allocTemp's "this
        // expression needs more registers" — which would send someone off to
        // simplify an expression when the real problem is the eightieth local.
        if (m_localTop >= int(SCRIPT_NUM_REGS)) {
            fail(QStringLiteral("too many local variables (limit %1) - a script this "
                                "large needs splitting up")
                     .arg(SCRIPT_NUM_REGS));
        }
        // The initialiser is compiled BEFORE the name is declared, so
        // `local x = x` reads the OUTER x, as Lua does. Declaring first would
        // silently read the new, uninitialised one.
        const int src = parseExpr(0);
        const int slot = declareLocal(name, false, 0);
        if (src != slot) {
            emitInstr(SCRIPT_OP_MOV, uint8_t(slot), uint8_t(src), 0, 0);
        }
        releaseTemps();
    }

    void parseAssignmentOrCall()
    {
        if (!check(T::Name)) {
            fail(QStringLiteral("expected a statement"));
        }
        const QString name = m_tok.text;
        const int line = m_tok.line;

        // setSig(...) is the one call that is a statement.
        if (name == QLatin1String("setSig")) {
            advance();
            expect(T::LParen, "'(' after setSig");
            const quint16 idx = parseChannelArgument();
            expect(T::Comma, "',' - setSig(channel, value)");
            const int val = parseExpr(0);
            expect(T::RParen, "')'");
            emitInstr(SCRIPT_OP_STORESIG, 0, uint8_t(val), 0, idx);
            return;
        }

        const Local *local = findLocal(name);
        if (!local) {
            // A call to something that is not setSig, or an assignment to an
            // undeclared name — both worth distinct messages.
            advance();
            if (check(T::LParen)) {
                failAt(bannedOrUnknown(name), line);
            }
            failAt(QStringLiteral("'%1' is not declared - locals need 'local %1 = ...'")
                       .arg(name),
                   line);
        }
        advance();
        expect(T::Assign, "'=' - a bare name is not a statement");
        const int src = parseExpr(0);
        if (local->isState) {
            emitInstr(SCRIPT_OP_STOREST, 0, uint8_t(src), 0, uint32_t(local->slot));
        } else if (src != local->slot) {
            emitInstr(SCRIPT_OP_MOV, uint8_t(local->slot), uint8_t(src), 0, 0);
        }
    }

    void parseIf()
    {
        expect(T::If, "'if'");
        QVector<int> endJumps;
        for (;;) {
            const int cond = parseExpr(0);
            expect(T::Then, "'then'");
            const int falseJump = emitInstr(SCRIPT_OP_JZ, 0, uint8_t(cond), 0, 0);
            releaseTemps();

            const int base = m_locals.size();
            parseBlock();
            closeScope(base);

            if (check(T::ElseIf)) {
                endJumps.append(emitInstr(SCRIPT_OP_JMP, 0, 0, 0, 0));
                patchJump(falseJump, here());
                advance();
                continue;
            }
            if (accept(T::Else)) {
                endJumps.append(emitInstr(SCRIPT_OP_JMP, 0, 0, 0, 0));
                patchJump(falseJump, here());
                const int elseBase = m_locals.size();
                parseBlock();
                closeScope(elseBase);
                expect(T::End_, "'end' to close if");
                break;
            }
            expect(T::End_, "'end' to close if");
            patchJump(falseJump, here());
            break;
        }
        for (int at : endJumps) {
            patchJump(at, here());
        }
    }

    void parseWhile()
    {
        expect(T::While, "'while'");
        m_loopsPresent = true;
        const int top = here();
        const int cond = parseExpr(0);
        expect(T::Do, "'do'");
        const int exit = emitInstr(SCRIPT_OP_JZ, 0, uint8_t(cond), 0, 0);
        releaseTemps();

        m_breakPatches.append(QVector<int>());
        const int base = m_locals.size();
        parseBlock();
        closeScope(base);
        expect(T::End_, "'end' to close while");

        emitInstr(SCRIPT_OP_JMP, 0, 0, 0, uint32_t(top));
        patchJump(exit, here());
        for (int at : m_breakPatches.last()) {
            patchJump(at, here());
        }
        m_breakPatches.removeLast();
    }

    void parseFor()
    {
        expect(T::For, "'for'");
        if (!check(T::Name)) {
            fail(QStringLiteral("expected a loop variable"));
        }
        const QString var = m_tok.text;
        advance();
        expect(T::Assign, "'=' - only numeric for is supported: for i = a, b do");
        m_loopsPresent = true;

        // The control variable is a local for the loop's scope.
        const int base = m_locals.size();
        const int startReg = parseExpr(0);
        const int slot = declareLocal(var, false, 0);
        if (startReg != slot) {
            emitInstr(SCRIPT_OP_MOV, uint8_t(slot), uint8_t(startReg), 0, 0);
        }
        expect(T::Comma, "',' - for i = a, b do");

        // The limit is evaluated ONCE into a dedicated register, matching Lua,
        // so a loop whose limit expression reads a channel does not re-read it
        // (and cannot be made to run forever by a changing input).
        const int limitReg = allocTemp();
        const int limitSrc = parseExpr(0);
        if (limitSrc != limitReg) {
            emitInstr(SCRIPT_OP_MOV, uint8_t(limitReg), uint8_t(limitSrc), 0, 0);
        }
        // Reserve the limit register for the loop's lifetime.
        const int savedLocalTop = m_localTop;
        m_localTop = qMax(m_localTop, limitReg + 1);
        m_tempTop = m_localTop;

        // The step must be a compile-time constant so the comparison's DIRECTION
        // is known now. A runtime step would need a sign test every iteration,
        // or an opcode that does not exist; requiring a literal costs nothing
        // real and keeps the loop two instructions shorter.
        double step = 1.0;
        if (accept(T::Comma)) {
            bool negate = false;
            if (accept(T::Minus)) {
                negate = true;
            }
            if (!check(T::Number)) {
                fail(QStringLiteral("the 'for' step must be a constant number"));
            }
            step = m_tok.number * (negate ? -1.0 : 1.0);
            advance();
            if (step == 0.0) {
                fail(QStringLiteral("a 'for' step of 0 would never finish"));
            }
        }
        expect(T::Do, "'do'");

        const int top = here();
        const int condReg = allocTemp();
        emitInstr(step > 0 ? MATH_OP_LE : MATH_OP_GE, uint8_t(condReg), uint8_t(slot),
             uint8_t(limitReg), 0);
        const int exit = emitInstr(SCRIPT_OP_JZ, 0, uint8_t(condReg), 0, 0);
        m_tempTop = m_localTop;

        m_breakPatches.append(QVector<int>());
        parseBlock();
        expect(T::End_, "'end' to close for");

        const int stepReg = allocTemp();
        emitInstr(SCRIPT_OP_LOADK, uint8_t(stepReg), 0, 0, floatBits(float(step)));
        emitInstr(MATH_OP_ADD, uint8_t(slot), uint8_t(slot), uint8_t(stepReg), 0);
        m_tempTop = m_localTop;
        emitInstr(SCRIPT_OP_JMP, 0, 0, 0, uint32_t(top));

        patchJump(exit, here());
        for (int at : m_breakPatches.last()) {
            patchJump(at, here());
        }
        m_breakPatches.removeLast();

        m_localTop = savedLocalTop;
        closeScope(base);
    }

    // ------------------------------------------------------------ expressions

    struct OpInfo { uint8_t op; int prec; };

    // Precedence, lowest binds loosest. Matches Lua's table for the operators
    // the subset has.
    bool binaryOp(T t, OpInfo *out) const
    {
        switch (t) {
        case T::Or:      *out = { MATH_OP_LOR, 1 }; return true;
        case T::And:     *out = { MATH_OP_LAND, 2 }; return true;
        case T::Lt:      *out = { MATH_OP_LT, 3 }; return true;
        case T::Gt:      *out = { MATH_OP_GT, 3 }; return true;
        case T::Le:      *out = { MATH_OP_LE, 3 }; return true;
        case T::Ge:      *out = { MATH_OP_GE, 3 }; return true;
        case T::Ne:      *out = { MATH_OP_NE, 3 }; return true;
        case T::Eq:      *out = { MATH_OP_EQ, 3 }; return true;
        case T::Plus:    *out = { MATH_OP_ADD, 4 }; return true;
        case T::Minus:   *out = { MATH_OP_SUB, 4 }; return true;
        case T::Star:    *out = { MATH_OP_MUL, 5 }; return true;
        case T::Slash:   *out = { MATH_OP_DIV, 5 }; return true;
        case T::Percent: *out = { MATH_OP_MOD, 5 }; return true;
        default: return false;
        }
    }

    // Precedence climbing. Returns the register holding the value.
    int parseExpr(int minPrec)
    {
        int lhs = parseUnary();
        for (;;) {
            OpInfo info;
            if (!binaryOp(m_tok.type, &info) || info.prec < minPrec) {
                if (m_tok.type == T::Caret) {
                    fail(QStringLiteral(
                        "'^' (power) is not available: it is not required to be "
                        "correctly rounded, so the device and the desktop simulator "
                        "could disagree. Use repeated multiplication."));
                }
                if (m_tok.type == T::Concat) {
                    fail(QStringLiteral("string concatenation is not available - "
                                        "scripts work with numbers only"));
                }
                return lhs;
            }
            advance();
            const int rhs = parseExpr(info.prec + 1);
            const int dst = allocTemp();
            emitInstr(info.op, uint8_t(dst), uint8_t(lhs), uint8_t(rhs), 0);
            lhs = dst;
        }
    }

    int parseUnary()
    {
        if (accept(T::Minus)) {
            const int v = parseUnary();
            const int dst = allocTemp();
            emitInstr(MATH_OP_NEG, uint8_t(dst), uint8_t(v), 0, 0);
            return dst;
        }
        if (accept(T::Not)) {
            const int v = parseUnary();
            const int dst = allocTemp();
            emitInstr(MATH_OP_LNOT, uint8_t(dst), uint8_t(v), 0, 0);
            return dst;
        }
        if (check(T::Hash)) {
            fail(QStringLiteral("'#' (length) is not available - there are no tables "
                                "or strings"));
        }
        return parsePrimary();
    }

    int parsePrimary()
    {
        if (check(T::Number)) {
            const double v = m_tok.number;
            advance();
            return loadConstant(float(v));
        }
        if (check(T::True)) { advance(); return loadConstant(1.0f); }
        if (check(T::False) || check(T::Nil)) { advance(); return loadConstant(0.0f); }
        if (accept(T::LParen)) {
            const int v = parseExpr(0);
            expect(T::RParen, "')'");
            return v;
        }
        if (check(T::String)) {
            fail(QStringLiteral("strings are only allowed as a channel name inside "
                                "sig() or setSig()"));
        }
        if (check(T::Name)) {
            const QString name = m_tok.text;
            const int line = m_tok.line;
            advance();

            if (check(T::LParen)) {
                return parseCall(name, line);
            }
            const Local *local = findLocal(name);
            if (!local) {
                failAt(QStringLiteral("'%1' is not declared").arg(name), line);
            }
            if (local->isState) {
                const int dst = allocTemp();
                emitInstr(SCRIPT_OP_LOADST, uint8_t(dst), 0, 0, uint32_t(local->slot));
                return dst;
            }
            return local->slot;
        }
        fail(QStringLiteral("expected a value"));
    }

    int parseCall(const QString &name, int line)
    {
        expect(T::LParen, "'('");

        if (name == QLatin1String("sig")) {
            const quint16 idx = parseChannelArgument();
            expect(T::RParen, "')'");
            const int dst = allocTemp();
            emitInstr(SCRIPT_OP_LOADSIG, uint8_t(dst), 0, 0, idx);
            return dst;
        }
        if (name == QLatin1String("setSig")) {
            failAt(QStringLiteral("setSig() is a statement and has no value - call it "
                                  "on its own line"),
                   line);
        }
        if (name == QLatin1String("state")) {
            failAt(QStringLiteral("state() may only appear in a top-level declaration: "
                                  "local x = state(0)"),
                   line);
        }

        for (const Intrinsic &fn : kIntrinsics) {
            if (name != QLatin1String(fn.name)) {
                continue;
            }
            int args[3] = { 0, 0, 0 };
            for (int i = 0; i < fn.arity; ++i) {
                if (i > 0) {
                    expect(T::Comma, "',' between arguments");
                }
                args[i] = parseExpr(0);
            }
            if (check(T::Comma)) {
                failAt(QStringLiteral("%1() takes %2 argument(s)")
                           .arg(name)
                           .arg(fn.arity),
                       line);
            }
            expect(T::RParen, "')'");
            const int dst = allocTemp();
            emitInstr(fn.op, uint8_t(dst), uint8_t(args[0]), uint8_t(args[1]),
                 uint32_t(args[2]));
            return dst;
        }
        failAt(bannedOrUnknown(name), line);
    }

    // A channel name argument: a string literal resolved against the config.
    quint16 parseChannelArgument()
    {
        if (!check(T::String)) {
            fail(QStringLiteral("expected a channel name in quotes, e.g. "
                                "sig(\"Engine RPM\")"));
        }
        const QString channel = m_tok.text;
        const int line = m_tok.line;
        advance();
        const auto it = m_symbols.signalIndex.constFind(channel);
        if (it == m_symbols.signalIndex.constEnd()) {
            failAt(QStringLiteral("no channel named '%1' in this configuration")
                       .arg(channel),
                   line);
        }
        return it.value();
    }

    QString bannedOrUnknown(const QString &name) const
    {
        for (const Banned &b : kBanned) {
            if (name == QLatin1String(b.name)) {
                if (std::strcmp(b.reason, "see sin") == 0) {
                    return QStringLiteral(
                               "'%1' is not available: trigonometric and exponential "
                               "functions are not required by IEEE-754 to be correctly "
                               "rounded, so the device and the desktop simulator could "
                               "disagree by a rounding step and branch differently")
                        .arg(name);
                }
                return QStringLiteral("'%1' is not available: %2")
                    .arg(name, QLatin1String(b.reason));
            }
        }
        return QStringLiteral("'%1' is not a function a script can call").arg(name);
    }

    int loadConstant(float v)
    {
        // A literal past FLT_MAX narrows to +/-Inf on the cast in parsePrimary,
        // which the device verifier then rejects as non-finite - reported to
        // the user as an "internal compiler error" it is not. Catch it here,
        // the single chokepoint every constant flows through, and say what is
        // actually wrong.
        if (!std::isfinite(v))
            fail(QStringLiteral("a numeric constant is outside the range a 32-bit "
                                "float can represent (scripts run in float32 on "
                                "the device)"));
        const int dst = allocTemp();
        emitInstr(SCRIPT_OP_LOADK, uint8_t(dst), 0, 0, floatBits(v));
        return dst;
    }

    static uint32_t floatBits(float f)
    {
        uint32_t b;
        std::memcpy(&b, &f, sizeof(b));
        return b;
    }

    // ------------------------------------------------------------ image

    QByteArray buildImage()
    {
        if (m_code.isEmpty()) {
            fail(QStringLiteral("the script produced no code"));
        }
        // Every hook path ends in the HALT parseFunction emitted, but the image
        // as a whole must also END in one — the verifier checks the final
        // instruction, so a trailing state-init or patch could otherwise leave
        // something else last.
        if (m_code.last().op != SCRIPT_OP_HALT) {
            emitInstr(SCRIPT_OP_HALT, 0, 0, 0, 0);
        }

        const uint32_t codeBytes = uint32_t(m_code.size()) * SCRIPT_INSTR_SIZE;
        QByteArray image;
        image.resize(int(sizeof(ScriptHeader) + codeBytes));

        ScriptHeader h{};
        h.magic = SCRIPT_MAGIC;
        h.version = SCRIPT_BYTECODE_VERSION;
        h.num_state = uint16_t(m_stateCount);
        h.code_bytes = codeBytes;
        h.entry_tick = uint32_t(m_entryTick);
        h.entry_rx = SCRIPT_NO_ENTRY;
        h.entry_tx = SCRIPT_NO_ENTRY;
        h.reserved = 0;

        std::memcpy(image.data() + sizeof(ScriptHeader), m_code.constData(), codeBytes);
        h.code_crc32 =
            script_crc32(image.constData() + sizeof(ScriptHeader), codeBytes);
        std::memcpy(image.data(), &h, sizeof(h));

        // Run the DEVICE's verifier on our own output. A compiler that emitted
        // an image the device would refuse is a compiler bug; catching it here
        // means it never reaches a vehicle, and the failure names the exact
        // check rather than arriving as "the script did not load".
        const uint8_t verdict =
            script_verify(image.constData(), uint32_t(image.size()), MAX_SIGNALS);
        if (verdict != SCRIPT_OK) {
            fail(QStringLiteral(
                     "internal compiler error: the generated bytecode failed the "
                     "device verifier (code %1). Please report this script.")
                     .arg(verdict));
        }
        return image;
    }

    // ------------------------------------------------------------ state

    Lexer m_lex;
    const ScriptSymbols &m_symbols;
    Token m_tok;
    Token m_prev;

    QVector<ScriptInstr> m_code;
    QVector<Local> m_locals;
    QVector<QPair<int, float>> m_stateInit;
    QVector<int> m_returnPatches;
    QVector<QVector<int>> m_breakPatches;
    QStringList m_warnings;

    int m_localTop = 0;
    int m_tempTop = 0;
    int m_maxReg = 0;
    int m_scopeBase = 0;
    int m_stateCount = 0;
    // The user's state() variables, without the once-only initialiser flag the
    // compiler allocates alongside them. Reported separately because the two
    // answer different questions: m_stateCount is what the DEVICE allocates and
    // what the limit applies to, m_stateDeclared is what the person WROTE and
    // what a state view should show.
    int m_stateDeclared = 0;
    int m_entryTick = -1;
    uint32_t m_straightLineCost = 0;
    bool m_loopsPresent = false;
};

// Lua is the syntax authority: if it will not load, report ITS message, which
// carries the line and is better than anything worth hand-writing for
// unbalanced blocks and the like. Only if Lua is satisfied does the subset
// compiler get to complain about semantics.
bool luaSyntaxCheck(const QString &source, QString *error, int *line)
{
    lua_State *L = luaL_newstate();
    if (!L) {
        return true; // cannot check; let the subset compiler try
    }
    const QByteArray src = source.toUtf8();
    const bool ok = luaL_loadbufferx(L, src.constData(), size_t(src.size()),
                                     "=script", "t") == LUA_OK;
    if (!ok && error) {
        QString msg = QString::fromUtf8(lua_tostring(L, -1));
        // Lua prefixes "script:12: "; lift the line number out for the editor.
        const int colon = msg.indexOf(QLatin1Char(':'));
        if (colon >= 0) {
            const int colon2 = msg.indexOf(QLatin1Char(':'), colon + 1);
            if (colon2 > colon) {
                bool okNum = false;
                const int n = msg.mid(colon + 1, colon2 - colon - 1).toInt(&okNum);
                if (okNum && line) {
                    *line = n;
                }
                msg = msg.mid(colon2 + 1).trimmed();
            }
        }
        *error = msg;
    }
    lua_close(L);
    return ok;
}

} // namespace

ScriptCompiler::Result ScriptCompiler::compile(const QString &source,
                                               const ScriptSymbols &symbols)
{
    Result r;
    QString syntaxError;
    int syntaxLine = 0;
    if (!luaSyntaxCheck(source, &syntaxError, &syntaxLine)) {
        r.ok = false;
        r.error = syntaxError;
        r.errorLine = syntaxLine;
        return r;
    }
    Compiler c(source, symbols);
    return c.run();
}

} // namespace ct
