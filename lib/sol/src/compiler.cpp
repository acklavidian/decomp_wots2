// The Sol compiler, decompiled from the game's (see compiler.h).
//
// Three stages, as in the original:
//  1. comments are taken out (0x21A330: a comment becomes a space, or as many
//     line feeds as it spanned; string literals are copied as they are);
//  2. lines are preprocessed (0x218B40): trailing blanks trimmed, #define
//     names replaced (0x219DF0), #define / #include carried out (0x2191C0),
//     and every non-empty line prefixed with "\x1B<file>,<line>;" so that
//     errors can name where they are;
//  3. the result is compiled in one pass, straight to byte code (0x1AB100
//     statements, 0x1ABE40 expressions, 0x1AE9A0 what follows an operand).
//
// The language has no operator precedence: an operator's right-hand side is
// a whole expression, so `a - b - c` is a - (b - c) and `-1 + 2` is -(1 + 2).
// Arithmetic copies its left operand (`new`, then the method changes it in
// place); `+=` and the like leave the copy out.  `a = b` on anything but an
// attribute is the method Set.

#include "sol/compiler.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>

namespace sol {

namespace {

// ── Op codes (the VM's) ──────────────────────────────────────────────────
enum : uint8_t {
    OpEnd = 0x00, OpObj = ' ', OpInt = '!', OpWString = '"', OpDef = '#', OpFloat = '$', OpList = '%',
    OpGetRoot = '0', OpGetParent = '1', OpGetLocal = '2', OpGetArgv = '3', OpGetArgs = '4',
    OpGetResult = '5', OpPopResult = '6', OpPopStack = '7', OpGetAttr = '8', OpSetAttr = '9',
    OpSendMsg = 'P', OpOperate = 'Q', OpNew = 'R', OpExit = 'S', OpNop = 'T',
};

// Keywords (table at 0x2D8450).
enum Keyword {
    KwObject = 0, KwLocal = 1, KwRoot = 2, KwThis = 3, KwDef = 4, KwNew = 5, KwArgv = 6, KwArgs = 7,
    KwOperate = 8, KwBreak = 9, KwReturn = 10, KwIf = 11, KwElse = 12, KwWhile = 13, KwFor = 14,
    KwSwitch = 15, KwCase = 16, KwVar = 17,
};
const struct { const char* word; int id; } kKeywords[] = {
    {"local", 1}, {"lc", 1}, {"root", 2}, {"this", 3}, {"def", 4}, {"new", 5}, {"argv", 6},
    {"args", 7}, {"argument", 7}, {"object", 0}, {"operate", 8}, {"break", 9}, {"return", 10},
    {"if", 11}, {"else", 12}, {"while", 13}, {"for", 14}, {"switch", 15}, {"case", 16}, {"var", 17},
};

const char16_t kBlanks[] = u" \t\r\n";                                  // 0x2E3528
const char16_t kSeparators[] = u" \t\n\r:;()[]{}<>,.+-*/%=@!#$&~\\|";   // 0x2E3530
const char16_t kQuotes[] = u"\"'";                                      // 0x2E3550
const struct { const char16_t* open; const char16_t* close; } kComments[] = {
    {u"//", u"\n"}, {u"/*", u"*/"},
};

bool in(const char16_t* set, char16_t c) {
    for (; *set; set++)
        if (*set == c) return true;
    return false;
}
bool is_separator(char16_t c) { return c == 0 || in(kSeparators, c); }

bool starts_with(const std::u16string& s, size_t at, const char16_t* what) {
    for (size_t i = 0; what[i]; i++)
        if (at + i >= s.size() || s[at + i] != what[i]) return false;
    return true;
}
size_t length(const char16_t* s) { size_t n = 0; while (s[n]) n++; return n; }

// 16-bit text → 8-bit, anything from 0xFF up as '?' (0x1AAC40).
std::string narrow(const std::u16string& s, size_t from, size_t to) {
    std::string out;
    for (size_t i = from; i < to; i++)
        out += s[i] < 0xFF ? (char)s[i] : '?';
    return out;
}
std::u16string widen(const std::string& s) {
    std::u16string out;
    for (unsigned char c : s) out += (char16_t)c;
    return out;
}

// A comment at `at` (0x1BAD90): → where it ends (`at` if none) and the line
// feeds it spanned (its own closing one included).
size_t skip_comment(const std::u16string& s, size_t at, size_t end, int* lines) {
    *lines = 0;
    for (const auto& c : kComments) {
        if (!starts_with(s, at, c.open))
            continue;
        size_t p = at + length(c.open);
        const size_t closeLen = length(c.close);
        while (true) {
            if (p >= end) return end;
            if (starts_with(s, p, c.close)) break;
            if (s[p] == u'\n') (*lines)++;
            p++;
        }
        if (c.close[0] == u'\n' && c.close[1] == 0) (*lines)++;
        return p + closeLen;
    }
    return at;
}

// A string literal at `at` (0x1BA910): → just past its closing quote.
size_t skip_string(const std::u16string& s, size_t at, size_t end) {
    if (at >= end || !in(kQuotes, s[at]))
        return at;
    const char16_t quote = s[at];
    char16_t prev = 0;
    for (size_t p = at + 1; p < end; p++) {
        if (s[p] == quote && prev != u'\\')
            return p + 1;
        prev = s[p];
    }
    return end;
}

// ── The lexer (0x1BA9C0 blanks, 0x1BB1B0 a token) ─────────────────────────
struct Token {
    int kind = 0;               // 0 a separator (value = the character; 0 at the end), 1 a keyword, 2 a word, 3 a string
    int value = 0;
    size_t start = 0, end = 0;
    int line = 0;
    size_t size() const { return end - start; }
};

struct Lexer {
    const std::u16string* text = nullptr;
    size_t pos = 0, end = 0;
    int line = 1;
    std::string file;

    char16_t at(size_t p) const { return p < end ? (*text)[p] : 0; }
    char16_t peek() const { return at(pos); }

    void blanks() {
        while (pos < end) {
            while (pos < end && in(kBlanks, (*text)[pos])) {
                if ((*text)[pos] == u'\n') line++;
                pos++;
            }
            if (pos >= end) return;
            if ((*text)[pos] == 0x1B) {             // "\x1B<file>,<line>;" (0x1BAB20)
                size_t semi = pos + 1;
                while (semi < end && (*text)[semi] != 0 && (*text)[semi] != u';') semi++;
                if (semi >= end || (*text)[semi] != u';') { pos++; continue; }
                size_t comma = pos + 1;
                while (comma < semi && (*text)[comma] != u',') comma++;
                if (comma < semi) {
                    file = narrow(*text, pos + 1, comma);
                    line = atoi(narrow(*text, comma + 1, semi).c_str());
                }
                pos = semi + 1;
                continue;
            }
            int lines = 0;
            const size_t after = skip_comment(*text, pos, end, &lines);
            if (after == pos) return;
            pos = after;
            line += lines;
        }
    }

    Token next() {
        blanks();
        Token t;
        t.line = line;
        if (pos >= end) {
            t.start = t.end = end;
            return t;
        }
        const size_t start = pos;
        const size_t afterString = skip_string(*text, pos, end);
        if (afterString != start) {
            pos = afterString;
            t.kind = 3;
        } else if (is_separator((*text)[pos])) {
            t.value = (*text)[pos];
            pos++;
        } else {
            while (pos < end && !is_separator((*text)[pos])) pos++;
            const std::string word = narrow(*text, start, pos);
            t.kind = 2;
            t.value = -1;
            for (const auto& k : kKeywords)
                if (word == k.word) { t.kind = 1; t.value = k.id; break; }
        }
        t.start = start;
        t.end = pos;
        return t;
    }
    Token look() {                  // 0x1BB040: the next token, not taken
        const int l = line;
        Token t = next();
        pos = t.start;
        line = l;
        return t;
    }
    void back(const Token& t) { pos = t.start; line = t.line; }
    void take(const Token& t) { pos = t.end; }

    std::string str(const Token& t) const { return narrow(*text, t.start, t.end); }
    bool is(const Token& t, char c) const { return t.size() == 1 && (*text)[t.start] == (char16_t)(unsigned char)c; }
    bool is(const Token& t, const char* s) const { return s != nullptr && str(t) == s; }
    bool keyword(const Token& t, int id) const { return t.kind == 1 && t.value == id; }
};

using Locals = std::shared_ptr<std::set<std::string>>;

// ── Code generation ───────────────────────────────────────────────────────
struct Compiler {
    Lexer lx;
    std::vector<uint8_t> out;
    std::string error;

    bool fail(const char* what) {
        if (error.empty()) {
            char buf[256];
            snprintf(buf, sizeof buf, "%s:%d: %s", lx.file.c_str(), lx.line, what);
            error = buf;
        }
        return false;
    }

    void op(uint8_t b) { out.push_back(b); }
    void put(int32_t v, int bytes) {
        for (int i = 0; i < bytes; i++) { out.push_back((uint8_t)v); v >>= 8; }
    }
    void patch_size(size_t start) {                 // a def's size, written before its code
        const int32_t n = (int32_t)(out.size() - start);
        for (int i = 0; i < 4; i++) out[start - 4 + i] = (uint8_t)(n >> (8 * i));
    }

    // A name operand (0x1B3930): s16 length + bytes, or (0x1B3A10) the
    // distance back to the same bytes earlier in the code — any earlier
    // bytes, the first match within 0x7F00 wins.
    void name(const std::string& s) {
        const size_t n = s.size() > 1000 ? 1000 : s.size();
        const size_t at = out.size();
        put((int32_t)n, 2);
        out.insert(out.end(), s.begin(), s.begin() + (long)n);
        const size_t limit = 0x7Fu << 8;
        size_t from = at > limit ? at - limit : 0;
        const long tries = (long)(at - from) - (long)(out.size() - at) + 1;
        for (long k = 0; k < tries; k++, from++) {
            if (memcmp(&out[from], &out[at], out.size() - at) == 0) {
                out.resize(at);
                put(-(int32_t)(at - from), 2);
                return;
            }
        }
    }

    // ── statements (0x1AB100) ─────────────────────────────────────────────
    bool statements(const Locals& L, const char* until) {
        while (true) {
            Token t = lx.next();
            if (until != nullptr && lx.is(t, until)) { op(OpEnd); return true; }
            if (t.size() == 0) {
                if (until != nullptr) return fail("unexpected end of the script");
                op(OpEnd);
                return true;
            }
            if (lx.is(t, ";")) { op(OpNop); continue; }
            switch (t.value) {
            case KwBreak:
                op(OpExit); put(4, 2);
                break;
            case KwReturn:
                if (!return_statement(L)) return false;
                break;
            case KwIf:
                if (!if_statement(L)) return false;
                op(OpPopStack);
                break;
            case KwWhile:
                if (!loop_statement(L, "__while", 1)) return false;
                op(OpPopStack);
                break;
            case KwFor:
                if (!loop_statement(L, "__for", 3)) return false;
                op(OpPopStack);
                break;
            case KwSwitch:
                if (!switch_statement(L)) return false;
                op(OpPopStack);
                break;
            case KwVar:
                if (!var_statement(L)) return false;
                op(OpPopStack);
                break;
            default:
                lx.back(t);
                if (!expression(L)) return fail("bad expression");
                op(OpPopResult);
                break;
            }
        }
    }

    bool return_statement(const Locals& L) {                   // 0x1B0C30
        lx.blanks();
        if (lx.peek() == u';') {
            lx.pos++;
        } else {
            if (!expression(L)) return false;
            op(OpPopResult);
            lx.blanks();
            if (lx.peek() != u';') return fail("';' expected after return");
            lx.pos++;
        }
        op(OpExit); put(3, 2);
        return true;
    }

    // A block: a def whose code is statements up to `until` (0x1AE6B0).
    bool block(const Locals& L, const char* open, const char* until) {
        if (open != nullptr) {
            Token t = lx.next();
            if (!lx.is(t, open)) return fail(open[0] == '(' ? "'(' expected" : "'{' expected");
        }
        op(OpDef); put(0, 4);
        const size_t start = out.size();
        if (!statements(L, until)) return false;
        patch_size(start);
        return true;
    }

    // getlocal, getparent, the construct, getargs: what operate takes, with the blocks after.
    void construct(const char* which) {
        op(OpGetLocal); op(OpGetParent); op(OpGetRoot); op(OpGetAttr); name(which); op(OpGetArgs);
    }
    void construct_end(int count) {
        op(OpList); put(count, 4); op(OpOperate); op(OpGetResult);
    }

    bool if_statement(const Locals& L) {                       // 0x1B25E0
        construct("__if");
        int count = 1;
        while (true) {
            if (!block(L, "(", ")") || !block(L, "{", "}")) return false;
            Token t = lx.look();
            if (!lx.keyword(t, KwElse)) { count += 2; break; }
            lx.take(t);
            Token t2 = lx.look();
            if (lx.keyword(t2, KwIf)) { lx.take(t2); count += 2; continue; }
            if (!block(L, "{", "}")) return false;
            count += 3;
            break;
        }
        construct_end(count);
        return true;
    }

    bool loop_statement(const Locals& L, const char* which, int parts) {   // 0x1B2330 / 0x1B1FB0
        construct(which);
        if (parts == 1) {
            if (!block(L, "(", ")")) return false;
        } else {
            if (!block(L, "(", ";") || !block(L, nullptr, ";") || !block(L, nullptr, ")")) return false;
        }
        if (!block(L, "{", "}")) return false;
        construct_end(parts + 2);
        return true;
    }

    bool switch_statement(const Locals& L) {                   // 0x1B16E0
        construct("__switch");
        Token t = lx.next();
        if (!lx.is(t, '(')) return fail("'(' expected after switch");
        if (!expression(L)) return false;
        t = lx.next();
        if (!lx.is(t, ')')) return fail("')' expected");
        int count = 2;
        while (true) {
            Token c = lx.look();
            if (lx.keyword(c, KwCase)) {
                lx.take(c);
                if (!expression(L) || !block(L, "{", "}")) return false;
                count += 2;
                continue;
            }
            if (lx.keyword(c, KwElse)) {
                lx.take(c);
                if (!block(L, "{", "}")) return false;
                count += 1;
            }
            construct_end(count);
            return true;
        }
    }

    bool var_statement(const Locals& L) {                      // 0x1B0DD0
        while (true) {
            Token t = lx.next();
            if (t.kind != 2) return fail("a name expected after var");
            const std::string n = lx.str(t);
            if (L->count(n)) return fail("variable declared twice");
            L->insert(n);
            Token t2 = lx.next();
            if (lx.is(t2, '=')) {
                op(OpGetLocal);
                if (!expression(L)) return false;
                op(OpSetAttr); name(n); op(OpPopStack);
                t2 = lx.next();
            }
            if (lx.is(t2, ',')) continue;
            lx.back(t2);
            op(OpGetRoot);
            return true;
        }
    }

    // ── expressions (0x1ABE40) ────────────────────────────────────────────
    bool expression(const Locals& L) {
        lx.blanks();
        const char16_t c = lx.peek();
        if (c >= u'0' && c <= u'9') {
            if (!number()) return false;
        } else if (c == u'"') {
            Token t = lx.next();
            std::u16string s = unescape(t);
            op(OpWString); put((int32_t)s.size(), 2);
            for (char16_t ch : s) put(ch, 2);
        } else if (c == u'(') {
            lx.pos++;
            if (!expression(L)) return false;
            lx.blanks();
            if (lx.peek() != u')') return fail("')' expected");
            lx.pos++;
        } else if (c == u'[') {
            if (!list(L, "[", "]")) return false;
        } else if (c == u'!' || c == u'-') {
            lx.pos++;
            if (!expression(L)) return false;
            unary(c == u'!' ? "Not" : "Inv");               // 0x1B0AF0
            return true;
        } else {
            Token t = lx.next();
            switch (t.value) {
            case KwObject: op(OpObj); break;
            case KwLocal: op(OpGetLocal); break;
            case KwRoot: op(OpGetRoot); break;
            case KwThis: op(OpGetParent); break;
            case KwDef: return def();
            case KwNew:
                if (!expression(L)) return false;
                op(OpNew);
                break;
            case KwArgs: op(OpGetArgs); break;
            case KwOperate:
                if (!operate(L)) return false;
                break;
            default:
                if (t.kind == 0) return fail("an operand expected");
                op(L->count(lx.str(t)) ? OpGetLocal : OpGetRoot);
                lx.back(t);
                break;
            }
        }
        lx.blanks();
        if (lx.peek() == u'.') lx.pos++;
        return postfix(L);
    }

    bool number() {
        std::string s = lx.str(lx.next());
        bool isFloat = false;
        if (lx.peek() == u'.') {
            lx.pos++;
            s += '.';
            s += lx.str(lx.next());
            isFloat = true;
        }
        char* end = nullptr;
        if (isFloat) {
            const float f = (float)strtod(s.c_str(), &end);
            int32_t bits;
            memcpy(&bits, &f, 4);
            op(OpFloat); put(bits, 4);
        } else {
            long v = strtol(s.c_str(), &end, 0);
            if (v > 0x7FFFFFFFL) v = 0x7FFFFFFFL;
            if (v < -0x7FFFFFFFL - 1) v = -0x7FFFFFFFL - 1;
            op(OpInt); put((int32_t)v, 4);
        }
        if (end != s.c_str() + s.size()) return fail("bad number");
        return true;
    }

    std::u16string unescape(const Token& t) {                  // 0x1BB760
        std::u16string s;
        if (t.size() < 2) return s;
        const std::u16string& x = *lx.text;
        size_t p = t.start;
        while (p + 1 != t.end - 1) {
            if (x[p + 1] == u'\\') {
                p += 2;
                if (p == t.end - 1) return s;
                s += x[p] == u't' ? u'\t' : x[p] == u'n' ? u'\n' : x[p];
            } else {
                s += x[p + 1];
                p++;
            }
        }
        return s;
    }

    void unary(const char* method) {
        op(OpNew); op(OpObj); op(OpList); put(0, 4); op(OpSendMsg); name(method); op(OpGetResult);
    }
    // `receiver method rhs` (0x1B0960): the receiver is on the stack already.
    bool binary(const Locals& L, const char* method) {
        op(OpObj);
        if (!expression(L)) return false;
        op(OpList); put(1, 4); op(OpSendMsg); name(method); op(OpGetResult);
        return true;
    }
    bool arithmetic(const Locals& L, const char* method) {     // 0x1B0820: `+` copies, `+=` does not
        if (lx.peek() == u'=') lx.pos++;
        else op(OpNew);
        return binary(L, method);
    }
    bool comparison(const Locals& L, const char* single, const char* withEquals) {   // 0x1B06F0
        if (lx.peek() == u'=') {
            lx.pos++;
            return binary(L, withEquals);
        }
        if (single == nullptr) return fail("'=' expected");
        return binary(L, single);
    }

    // What follows an operand (0x1AE9A0).
    bool postfix(const Locals& L) {
        while (true) {
            Token t = lx.next();
            if (t.kind == 0) {
                bool ok = true;
                switch (t.value) {
                case '[':
                    ok = binary(L, "at");
                    if (ok && !lx.is(lx.next(), ']')) return fail("']' expected");
                    break;
                case '!': ok = comparison(L, nullptr, "ne"); break;
                case '=': ok = comparison(L, "Set", "eq"); break;
                case '>': ok = comparison(L, "gt", "ge"); break;
                case '<': ok = comparison(L, "lt", "le"); break;
                case '|': ok = arithmetic(L, "Or"); break;
                case '&': ok = arithmetic(L, "And"); break;
                case '%': ok = arithmetic(L, "Mod"); break;
                case '/': ok = arithmetic(L, "Div"); break;
                case '*': ok = arithmetic(L, "Mul"); break;
                case '-': ok = arithmetic(L, "Sub"); break;
                case '+': ok = arithmetic(L, "Add"); break;
                case '.': break;
                default:
                    lx.back(t);
                    return true;
                }
                if (!ok) return false;
                continue;
            }
            if (t.kind != 2) return fail("a name expected");
            const std::string n = lx.str(t);
            Token after = lx.look();
            if (!lx.is(after, '(') && !lx.is(after, '@')) {
                if (lx.is(after, '=') && !starts_with(*lx.text, lx.pos, u"==")) {
                    lx.take(after);
                    if (!expression(L)) return false;
                    op(OpSetAttr); name(n);
                    return true;
                }
                op(OpGetAttr); name(n);
            } else {
                if (lx.is(after, '@')) { lx.take(after); op(OpGetLocal); }
                else op(OpObj);
                if (!list(L, "(", ")")) return false;
                op(OpSendMsg); name(n); op(OpGetResult);
            }
        }
    }

    bool list(const Locals& L, const char* open, const char* close) {     // 0x1B3420
        Token t = lx.next();
        if (!lx.is(t, open)) return fail("a list expected");
        int count = 0;
        Token p = lx.look();
        if (!lx.is(p, close)) {
            while (true) {
                if (!expression(L)) return false;
                count++;
                p = lx.look();
                if (!lx.is(p, ',')) break;
                lx.take(p);
            }
            if (!lx.is(p, close)) return fail(close[0] == ')' ? "')' expected" : "']' expected");
        }
        lx.take(p);
        op(OpList); put(count, 4);
        return true;
    }

    // `def (a, b) { … }` (0x1ADB00): the arguments become locals.
    bool def() {
        Locals L = std::make_shared<std::set<std::string>>();
        op(OpDef); put(0, 4);
        const size_t start = out.size();
        int index = 0;
        Token p = lx.look();
        if (lx.is(p, '(')) {
            lx.take(p);
            while (true) {
                Token t = lx.next();
                if (lx.is(t, ')')) break;
                if (t.size() == 0) return fail("')' expected");
                if (lx.is(t, ',')) continue;
                const std::string n = lx.str(t);
                L->insert(n);
                op(OpGetLocal); op(OpGetArgv); put(index, 2); op(OpSetAttr); name(n); op(OpPopStack);
                index++;
            }
        }
        if (!lx.is(lx.next(), '{')) return fail("'{' expected after def");
        if (!statements(L, "}")) return false;
        patch_size(start);
        return true;
    }

    // `operate [@] (receiver; method; arguments)` (0x1B2B60).
    bool operate(const Locals& L) {
        Token t = lx.next();
        Locals scope;
        if (!lx.is(t, '@')) {
            op(OpObj);
            scope = std::make_shared<std::set<std::string>>();
        } else {
            op(OpGetLocal);
            scope = L;
            t = lx.next();
        }
        if (!lx.is(t, '(')) return fail("'(' expected after operate");
        if (!expression(scope)) return false;
        if (!lx.is(lx.next(), ';')) return fail("';' expected");
        if (!expression(scope)) return false;
        if (!lx.is(lx.next(), ';')) return fail("';' expected");
        Token p = lx.look();
        if (!lx.is(p, ')')) {
            if (!expression(L)) return false;
            if (!lx.is(lx.next(), ')')) return fail("')' expected");
        } else {
            op(OpList); put(0, 4);
            lx.take(p);
        }
        op(OpOperate); op(OpGetResult);
        return true;
    }
};

// ── Preprocessing ─────────────────────────────────────────────────────────
struct Preprocessor {
    std::map<std::string, std::u16string> defines;
    const IncludeLoader* include = nullptr;
    std::string error;
    int depth = 0;

    // 0x21A330
    static std::u16string strip_comments(const std::u16string& s) {
        std::u16string out;
        size_t p = 0;
        const size_t end = s.size();
        while (p < end && s[p] != 0) {
            const size_t q = skip_string(s, p, end);
            if (q != p) { out.append(s, p, q - p); p = q; continue; }
            int lines = 0;
            const size_t c = skip_comment(s, p, end, &lines);
            if (c != p) {
                if (lines == 0) out += u' ';
                else out.append((size_t)lines, u'\n');
                p = c;
                continue;
            }
            if (s[p] != 0xFEFF) out += s[p];
            p++;
        }
        return out;
    }

    // 0x219DF0: #define names replaced, the line rebuilt from its tokens.
    void substitute(std::u16string& line) {
        if (defines.empty()) return;
        Lexer lx;
        lx.text = &line;
        lx.end = line.size();
        std::u16string rebuilt;
        int replaced = 0;
        while (lx.pos < lx.end) {
            Token t = lx.next();
            const auto d = defines.find(lx.str(t));
            rebuilt += u' ';
            if (d != defines.end()) {
                rebuilt += d->second;
                replaced++;
            } else {
                rebuilt.append(line, t.start, t.size());
            }
        }
        if (replaced > 0) line = rebuilt;
    }

    // 0x2191C0
    void directive(std::u16string& line) {
        Lexer lx;
        lx.text = &line;
        lx.end = line.size();
        Token t = lx.next();
        if (!lx.is(t, '#')) return;
        t = lx.next();
        if (lx.is(t, "define")) {
            const std::string n = lx.str(lx.next());
            lx.blanks();
            defines[n] = line.substr(lx.pos);
            line.clear();
            return;
        }
        if (lx.is(t, "include")) {
            t = lx.next();
            if (t.kind == 3) {
                const std::string file = narrow(line, t.start + 1, t.end - 1);
                std::vector<uint8_t> bytes;
                if (include == nullptr || !*include || !(*include)(file, &bytes) || depth > 16) {
                    if (error.empty()) error = file + ": cannot include";
                    line.clear();
                    return;
                }
                depth++;
                line = run(decode_text(bytes), file);
                depth--;
            }
            return;
        }
        line.clear();
    }

    // 0x218760 / 0x218B40
    std::u16string run(const std::u16string& source, const std::string& file) {
        const std::u16string text = strip_comments(source);
        std::u16string out;
        size_t p = 0;
        int lineNo = 1;
        while (p < text.size()) {
            size_t q = p;
            while (q < text.size() && text[q] != u'\n') q++;
            std::u16string line = text.substr(p, q - p);
            while (!line.empty() && (line.back() == u' ' || line.back() == u'\t' || line.back() == u'\r'))
                line.pop_back();
            substitute(line);
            directive(line);
            if (!line.empty()) {
                const std::u16string mark = u"\x1B" + widen(file) + u"," + widen(std::to_string(lineNo)) + u";";
                line = mark + line;
            }
            lineNo++;
            out += line;
            out += u'\n';
            p = q < text.size() ? q + 1 : q;
        }
        return out;
    }
};

}  // namespace

std::u16string decode_text(const std::vector<uint8_t>& b) {
    std::u16string s;
    if (b.size() >= 2 && b[0] == 0xFF && b[1] == 0xFE) {
        for (size_t i = 2; i + 1 < b.size(); i += 2) s += (char16_t)(b[i] | b[i + 1] << 8);
        return s;
    }
    if (b.size() >= 2 && b[0] == 0xFE && b[1] == 0xFF) {
        for (size_t i = 2; i + 1 < b.size(); i += 2) s += (char16_t)(b[i] << 8 | b[i + 1]);
        return s;
    }
    size_t i = (b.size() >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) ? 3 : 0;
    while (i < b.size()) {                      // UTF-8 (characters past U+FFFF become '?')
        uint32_t c = b[i++];
        int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (more) c &= 0x3F >> more;
        while (more-- > 0 && i < b.size()) c = c << 6 | (b[i++] & 0x3F);
        s += c > 0xFFFF ? u'?' : (char16_t)c;
    }
    return s;
}

CompileResult compile(const std::u16string& source, const std::string& name, const IncludeLoader& include) {
    CompileResult r;
    Preprocessor pp;
    pp.include = &include;
    std::u16string src = source;
    if (!src.empty() && src[0] == 0xFEFF) src.erase(0, 1);
    const std::u16string text = pp.run(src, name);
    if (!pp.error.empty()) {
        r.error = pp.error;
        return r;
    }
    Compiler c;
    c.lx.text = &text;
    c.lx.end = text.size();
    c.lx.file = name;
    Locals top = std::make_shared<std::set<std::string>>();
    r.ok = c.statements(top, nullptr);
    if (r.ok) r.code = std::move(c.out);
    else r.error = c.error.empty() ? name + ": syntax error" : c.error;
    return r;
}

CompileResult compile(const std::vector<uint8_t>& source, const std::string& name, const IncludeLoader& include) {
    return compile(decode_text(source), name, include);
}

std::vector<uint8_t> make_sx(const std::vector<uint8_t>& code) {
    // The size counts one more zero byte than the code ends with (every file on the disc does).
    const uint32_t size = (uint32_t)code.size() + 1, padded = (size + 3) & ~3u;
    std::vector<uint8_t> f(0x14 + padded, 0);
    memcpy(f.data(), "TH00", 4);
    auto w = [&](size_t at, uint32_t v) { for (int i = 0; i < 4; i++) f[at + i] = (uint8_t)(v >> (8 * i)); };
    w(4, 0);
    w(8, size);
    w(12, padded);
    w(16, 0);
    memcpy(f.data() + 0x14, code.data(), code.size());
    return f;
}

}  // namespace sol
