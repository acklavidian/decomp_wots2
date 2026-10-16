// The Sol virtual machine — see sol/vm.h.  Each part names the functions of the
// original it follows.

#include "sol/vm.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace sol {

// ═══ Values ═══════════════════════════════════════════════════════════════

void Obj::cloneAttrs(const Obj& from) {     // 0x1BA130
    // (The original clones every attribute.  Built-ins hold no state, so they
    // are shared here rather than copied: every new number carries a dozen.)
    for (const auto& a : from.attrs)
        attrs[a.first] = !a.second || a.second->type() == Type::Native ? a.second : a.second->clone();
}
template <typename T> static Ref cloneOf(const T& self) {
    auto copy = std::make_shared<T>(self);
    copy->attrs.clear();
    copy->cloneAttrs(self);
    return copy;
}
Ref Obj::clone() const { return cloneOf(*this); }
Ref Int::clone() const { return cloneOf(*this); }
Ref Float::clone() const { return cloneOf(*this); }
Ref String::clone() const { return cloneOf(*this); }
Ref List::clone() const { return cloneOf(*this); }
Ref NoData::clone() const { return cloneOf(*this); }
Ref Native::clone() const { return cloneOf(*this); }

std::string narrow(const std::u16string& s) {
    std::string out;
    for (char16_t c : s)
        out.push_back(c < 0x80 ? (char)c : '?');
    return out;
}
std::u16string widen(const std::string& s) {
    std::u16string out;
    for (unsigned char c : s)
        out.push_back(c);
    return out;
}
std::string utf8(const std::u16string& s) {
    std::string out;
    for (char16_t c : s) {
        if (c < 0x80) {
            out += (char)c;
        } else if (c < 0x800) {
            out += (char)(0xC0 | c >> 6);
            out += (char)(0x80 | (c & 0x3F));
        } else {
            out += (char)(0xE0 | c >> 12);
            out += (char)(0x80 | (c >> 6 & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        }
    }
    return out;
}
std::u16string from_utf8(const std::string& s) {
    std::u16string out;
    for (size_t i = 0; i < s.size();) {
        uint32_t c = (unsigned char)s[i++];
        int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (more) c &= 0x3Fu >> more;
        while (more-- > 0 && i < s.size()) c = c << 6 | ((unsigned char)s[i++] & 0x3F);
        out += c > 0xFFFF ? u'?' : (char16_t)c;
    }
    return out;
}

static inline Int* asInt(const Ref& r) { return r && r->type() == Type::Int ? static_cast<Int*>(r.get()) : nullptr; }
static inline Float* asFloat(const Ref& r) { return r && r->type() == Type::Float ? static_cast<Float*>(r.get()) : nullptr; }
static inline String* asString(const Ref& r) { return r && r->type() == Type::String ? static_cast<String*>(r.get()) : nullptr; }
static inline List* asList(const Ref& r) { return r && r->type() == Type::List ? static_cast<List*>(r.get()) : nullptr; }

size_t argCount(const Frame& f) { return asList(f.args) ? asList(f.args)->items.size() : 0; }
Ref arg(const Frame& f, size_t i) { return i < argCount(f) ? asList(f.args)->items[i] : nullptr; }
bool argInt(const Frame& f, size_t i, s32* out) {
    if (Int* v = asInt(arg(f, i))) {
        *out = v->value;
        return true;
    }
    return false;
}
bool argNumber(const Frame& f, size_t i, float* out) {
    const Ref a = arg(f, i);
    if (Int* v = asInt(a)) {
        *out = (float)v->value;
        return true;
    }
    if (Float* v = asFloat(a)) {
        *out = v->value;
        return true;
    }
    return false;
}
const std::u16string* argString(const Frame& f, size_t i) {
    const Ref a = arg(f, i);
    return asString(a) ? &asString(a)->value : nullptr;
}

// ── 0x1B93A0 / 0x1B9950 ───────────────────────────────────────────────────
Ref getAttr(VM& vm, const Ref& object, const std::string& name) {
    if (object) {
        auto it = object->attrs.find(name);
        if (it != object->attrs.end())
            return it->second;
    }
    return vm.noData("attribute not found: '" + name + "'");
}
// 0x1B8D60: does `value` hold `object` somewhere among its attributes?
static bool contains(const Ref& value, const Obj* object, int depth = 0) {
    if (!value || depth > 32)
        return false;
    if (value.get() == object)
        return true;
    for (const auto& a : value->attrs)
        if (contains(a.second, object, depth + 1))
            return true;
    return false;
}
bool setAttr(const Ref& object, const std::string& name, const Ref& value) {
    if (!object || contains(value, object.get()))
        return false;
    object->attrs[name] = value;
    return true;
}

void fail(Frame& frame, const std::string& message) {
    frame.thread->result = frame.thread->vm->noData(message);
}

// ═══ Calls, frames, threads ═══════════════════════════════════════════════

// ── 0x1B74F0 + 0x1B7990 ───────────────────────────────────────────────────
int call(Thread& thread, const Ref& receiver, const Ref& method, const Ref& args, const Ref& scope) {
    thread.frames.emplace_front();
    Frame& f = thread.frames.front();
    // The callee is the method itself if nothing else holds it, else a copy,
    // so that every running call has a function object of its own.
    f.callee = (!method || method.use_count() == 1) ? method : method->clone();
    f.thread = &thread;
    f.receiver = receiver;
    f.base = thread.stack.size();
    f.scope = scope;
    f.args = asList(args) ? args : thread.vm->newList();
    f.flagged = false;
    f.status = kGoOn;
    if (!f.callee || f.callee->begin(f) == 0) {
        if (!thread.frames.empty())
            thread.frames.pop_front();
        return 0;
    }
    return 1;
}

bool Thread::runnable() {
    if (frames.empty())
        return false;
    const s32 s = state ? state->value : 1;
    if (s == 1)
        stop();
    return s == 0;
}
void Thread::stop() {
    stack.clear();
    frames.clear();
    if (state)
        state->value = 1;
}

bool Thread::step(int budget) {
    while (budget > 0 && runnable()) {
        Frame& f = frames.front();
        budget--;
        switch (f.status) {
        case kError:
            stop();
            break;
        case kBreak:        // up to and including the nearest loop
            while (!frames.empty()) {
                const bool loop = frames.front().flagged;
                frames.pop_front();
                if (loop)
                    break;
            }
            break;
        case kReturn: {     // everything that runs in this scope
            const Obj* scope = f.scope.get();
            while (!frames.empty() && frames.front().scope.get() == scope)
                frames.pop_front();
            break;
        }
        case kEnd:
            frames.pop_front();
            break;
        case kGoOn:
            f.status = f.callee->step(f);
            if (f.status == kYield) {
                f.status = kGoOn;
                budget = 0;
            }
            break;
        default:
            return true;
        }
    }
    return frames.empty();
}

// ═══ Code ═════════════════════════════════════════════════════════════════

struct CodeBuffer {
    std::vector<u8> bytes;
    std::string name;
};

struct Code : Obj {         // 0x1A9860…: vtables 0x2EB620 / 0x2EB600
    std::shared_ptr<CodeBuffer> buffer;
    size_t offset = 0, pc = 0;
    Type type() const override { return Type::Code; }
    Ref clone() const override { return cloneOf(*this); }
    int begin(Frame&) override {            // 0x1A4E50
        if (!buffer)
            return 0;
        pc = offset;
        return 1;
    }
    int step(Frame& f) override;            // 0x1A4D60: one op
};

static inline s16 rd16(const std::vector<u8>& c, size_t at) { return (s16)(c[at] | c[at + 1] << 8); }
static inline s32 rd32(const std::vector<u8>& c, size_t at) {
    return (s32)((u32)c[at] | (u32)c[at + 1] << 8 | (u32)c[at + 2] << 16 | (u32)c[at + 3] << 24);
}
// 0x1A9730: a name, or a reference back to an earlier one.
static std::string readName(const std::vector<u8>& c, size_t& pc) {
    const s16 n = rd16(c, pc);
    pc += 2;
    if (n < 0) {
        const size_t at = pc + n;
        const s16 m = rd16(c, at - 2);
        return std::string(c.begin() + at, c.begin() + at + m);
    }
    std::string s(c.begin() + pc, c.begin() + pc + n);
    pc += n;
    return s;
}

int Code::step(Frame& f) {
    Thread& t = *f.thread;
    VM& vm = *t.vm;
    const std::vector<u8>& c = buffer->bytes;
    if (pc >= c.size())
        return kEnd;
    auto depth = [&] { return (long)t.stack.size() - (long)f.base; };
    auto push = [&](const Ref& v) { t.stack.push_back(v); };
    auto pop = [&] {
        Ref v = t.stack.back();
        t.stack.pop_back();
        return v;
    };
    auto error = [&](const std::string& message) {
        vm.error(message + "  [" + buffer->name + " @" + std::to_string(pc) + "]");
        return kError;
    };
    const u8 op = c[pc++];
    switch (op) {
    case 0x00:              // op_end 0x1A5250
        if ((long)t.stack.size() != (long)f.base)
            vm.error("op_end: stack error (" + std::to_string(depth()) + " stacks)");
        return kEnd;
    case 0x54:              // op_nop 0x1A5220
        return kGoOn;
    case 0x53: {            // op_exit 0x1A52C0
        const s16 status = rd16(c, pc);
        pc += 2;
        if ((long)t.stack.size() != (long)f.base)
            vm.error("op_end: stack error (" + std::to_string(depth()) + " stacks)");
        return status;
    }
    case 0x20:              // op_obj 0x1A9550
        push(vm.newObject());
        return kGoOn;
    case 0x21:              // op_int 0x1A9410
        push(vm.newInt(rd32(c, pc)));
        pc += 4;
        return kGoOn;
    case 0x24: {            // op_float 0x1A92C0
        float v;
        const s32 bits = rd32(c, pc);
        memcpy(&v, &bits, 4);
        pc += 4;
        push(vm.newFloat(v));
        return kGoOn;
    }
    case 0x22: {            // op_wstring 0x1A8DB0
        const s16 n = rd16(c, pc);
        pc += 2;
        std::u16string s;
        for (s16 i = 0; i < n; i++, pc += 2)
            s.push_back((char16_t)(u16)rd16(c, pc));
        push(vm.newString(s));
        return kGoOn;
    }
    case 0x23: {            // op_def 0x1A8950
        const s32 n = rd32(c, pc);
        pc += 4;
        auto code = std::make_shared<Code>();
        code->buffer = buffer;
        code->offset = pc;
        push(code);
        pc += n;
        return kGoOn;
    }
    case 0x25: {            // op_list 0x1A9010
        const s32 n = rd32(c, pc);
        pc += 4;
        Ref list = vm.newList();
        List* l = asList(list);
        for (s32 i = 0; i < n && !t.stack.empty(); i++)
            l->items.insert(l->items.begin(), pop());
        push(list);
        return kGoOn;
    }
    case 0x30:              // op_getroot 0x1A8860
        push(vm.root);
        return kGoOn;
    case 0x31:              // op_getparent 0x1A8770
        push(f.receiver);
        return kGoOn;
    case 0x32:              // op_getlocal 0x1A86E0
        push(f.scope);
        return kGoOn;
    case 0x33: {            // op_getargv 0x1A82A0
        const s16 i = rd16(c, pc);
        pc += 2;
        if (i < 0 || (size_t)i >= argCount(f)) {
            fail(f, "op_getargv: range error " + std::to_string(i));
            return error("op_getargv: range error " + std::to_string(i));
        }
        push(arg(f, (size_t)i));
        return kGoOn;
    }
    case 0x34:              // op_getargs 0x1A8650
        push(f.args);
        return kGoOn;
    case 0x35:              // op_getresult 0x1A81B0
        push(t.result);
        return kGoOn;
    case 0x36:              // op_popresult 0x1A7480
        if (depth() < 1) {
            vm.error("op_popresult: stack under flow");
            return kGoOn;
        }
        t.result = pop();
        return kGoOn;
    case 0x37:              // op_popstack 0x1A7360
        if (depth() < 1) {
            vm.error("op_popstack: stack under flow");
            return kGoOn;
        }
        pop();
        return kGoOn;
    case 0x38: {            // op_getattr 0x1A7BC0
        if (depth() < 1) {
            vm.error("op_getattr: stack under flow");
            return kGoOn;
        }
        const std::string name = readName(c, pc);
        const Ref object = pop();
        Ref value = getAttr(vm, object, name);
        if (value->type() == Type::NoData) {
            if (vm.missing)
                value = vm.missing(object, name, false);
            if (!value || value->type() == Type::NoData)
                return error("op_getattr: attribute not found: " + name);
        }
        push(value);
        return kGoOn;
    }
    case 0x39: {            // op_setattr 0x1A7650
        if (depth() < 2) {
            vm.error("op_setattr: stack under flow");
            return kGoOn;
        }
        const std::string name = readName(c, pc);
        const Ref value = pop();
        const Ref object = pop();
        if (!setAttr(object, name, value)) {
            fail(f, "op_setattr: recurse error");
            return error("op_setattr: recurse error");
        }
        push(value);
        return kGoOn;
    }
    case 0x50: {            // op_sendmsg 0x1A5D10
        const std::string name = readName(c, pc);
        if (depth() < 3)
            return error("op_sendmsg: stack under flow");
        if (!asList(t.stack.back()))
            return error("op_sendmsg: argument error");
        const Ref args = pop();
        const Ref scope = pop();
        const Ref receiver = pop();
        Ref method = getAttr(vm, receiver, name);
        if (method->type() == Type::NoData) {
            if (vm.missing)
                method = vm.missing(receiver, name, true);
            if (!method || method->type() == Type::NoData)
                return error("op_sendmsg: attribute not found: " + name);
        }
        call(t, receiver, method, args, scope);
        return kGoOn;
    }
    case 0x51: {            // op_operate 0x1A5620
        if (depth() < 4)
            return error("op_operate: stack under flow");
        if (!asList(t.stack.back()))
            return error("op_operate: argument error");
        const Ref args = pop();
        const Ref method = pop();
        const Ref receiver = pop();
        const Ref scope = pop();
        call(t, receiver, method, args, scope);
        return kGoOn;
    }
    case 0x52: {            // op_new 0x1A5390
        if (depth() < 1)
            return error("op_new: stack under flow");
        const Ref v = pop();
        push(v ? v->clone() : nullptr);
        return kGoOn;
    }
    default:                // 0x1A51B0
        vm.error("Illegal op-code:" + std::to_string(op));
        return kGoOn;
    }
}

Ref VM::load(const std::vector<u8>& file, const std::string& name) {      // 0x1A47C0
    if (file.size() < 0x14 || memcmp(file.data(), "TH00", 4) != 0)
        return nullptr;
    const u32 entry = (u32)rd32(file, 4), size = (u32)rd32(file, 8);
    if (0x14 + (size_t)size > file.size())
        return nullptr;
    auto buffer = std::make_shared<CodeBuffer>();
    buffer->bytes.assign(file.begin() + 0x14, file.begin() + 0x14 + size);
    buffer->name = name;
    auto code = std::make_shared<Code>();
    code->buffer = buffer;
    code->offset = entry;
    return code;
}

// ═══ The control constructs ═══════════════════════════════════════════════
// Callable objects of the root.  Their blocks are `def`s called with the
// caller's receiver and scope, so a block sees its function's variables.

static bool resultIsTrue(Thread& t) {
    Int* v = asInt(t.result);
    return v != nullptr && v->value != 0;
}
static int callBlock(Frame& f, const Ref& block, const Ref& args) {
    return call(*f.thread, f.receiver, block, args, f.scope);
}

struct If : Obj {           // 0x19FEF0 / 0x1A00B0
    size_t count = 0, index = 0;
    int phase = 0;
    Ref blockArgs;
    Type type() const override { return Type::Other; }
    Ref clone() const override { return cloneOf(*this); }
    int begin(Frame& f) override {
        count = argCount(f);
        if (count < 3)
            return 0;
        blockArgs = arg(f, 0);
        index = 1;
        phase = 0;
        return 1;
    }
    int step(Frame& f) override {
        if (index >= count)
            return kEnd;
        List* args = asList(f.args);
        if (phase == 0) {                   // the condition
            const size_t at = index++;
            phase = 1;
            callBlock(f, args->items[at], blockArgs);
            return kGoOn;
        }
        if (phase == 1) {
            if (!resultIsTrue(*f.thread)) {
                index++;                    // skip its body
                phase = 0;
                return kGoOn;
            }
            phase = 2;
            callBlock(f, args->items[index], blockArgs);
            return kGoOn;
        }
        return kEnd;
    }
};

struct While : Obj {        // 0x19EF20 / 0x19F250
    int phase = 0;
    Ref blockArgs, cond, body;
    Type type() const override { return Type::Other; }
    Ref clone() const override { return cloneOf(*this); }
    int begin(Frame& f) override {
        if (argCount(f) != 3)
            return 0;
        phase = 0;
        blockArgs = arg(f, 0);
        cond = arg(f, 1);
        body = arg(f, 2);
        f.flagged = true;
        return 1;
    }
    int step(Frame& f) override {
        if (phase == 0) {
            phase = 1;
            callBlock(f, cond, blockArgs);
            return kGoOn;
        }
        if (phase == 1 && resultIsTrue(*f.thread)) {
            phase = 0;
            callBlock(f, body, blockArgs);
            return kGoOn;
        }
        return kEnd;
    }
};

struct For : Obj {          // 0x19E600 / 0x19EAA0
    int phase = 0;
    Ref blockArgs, init, cond, next, body;
    Type type() const override { return Type::Other; }
    Ref clone() const override { return cloneOf(*this); }
    int begin(Frame& f) override {
        if (argCount(f) != 5)
            return 0;
        phase = 0;
        blockArgs = arg(f, 0);
        init = arg(f, 1);
        cond = arg(f, 2);
        next = arg(f, 3);
        body = arg(f, 4);
        f.flagged = true;
        return 1;
    }
    int step(Frame& f) override {
        switch (phase) {
        case 0: phase = 1; callBlock(f, init, blockArgs); return kGoOn;
        case 1: phase = 2; callBlock(f, cond, blockArgs); return kGoOn;
        case 2:
            if (!resultIsTrue(*f.thread))
                return kEnd;
            phase = 3;
            callBlock(f, body, blockArgs);
            return kGoOn;
        case 3: phase = 1; callBlock(f, next, blockArgs); return kGoOn;
        }
        return kEnd;
    }
};

struct Switch : Obj {       // 0x19F530 / 0x19F5C0
    Type type() const override { return Type::Other; }
    Ref clone() const override { return cloneOf(*this); }
    int begin(Frame& f) override { return argCount(f) > 2; }
    int step(Frame& f) override {
        VM& vm = *f.thread->vm;
        List* args = asList(f.args);
        const Ref blockArgs = args->items[0];
        const Ref value = args->items[1];
        size_t at = 2;
        for (size_t left = args->items.size() - 2; left > 0;) {
            if (left == 1) {                // the default
                callBlock(f, args->items[at], blockArgs);
                return kEnd;
            }
            Ref one = vm.newList();
            asList(one)->items.push_back(args->items[at]);
            const Ref same = vm.callNow(value, getAttr(vm, value, "eq"), one, vm.newObject());
            if (asInt(same) && asInt(same)->value != 0) {
                callBlock(f, args->items[at + 1], blockArgs);
                return kEnd;
            }
            at += 2;
            left -= 2;
        }
        return kEnd;
    }
};

// ═══ Built-in methods ═════════════════════════════════════════════════════

// 0x19C230 / 0x19C430: `fn(receiver, argument)` for every argument, each of
// the receiver's own type; the receiver is the result.
template <typename T, typename F> static std::function<int(Frame&)> binary(Type type, F fn) {
    return [type, fn](Frame& f) {
        if (!f.receiver || f.receiver->type() != type) {
            fail(f, "api_opr_indirect: different type operation");
            return 0;
        }
        for (size_t i = 0; i < argCount(f); i++) {
            const Ref a = arg(f, i);
            if (!a || a->type() != type) {
                fail(f, "api_opr_indirect: different type operation");
                return 0;
            }
            fn(*static_cast<T*>(f.receiver.get()), *static_cast<T*>(a.get()));
        }
        f.thread->result = f.receiver;
        return 0;
    };
}
// 0x19BC20
template <typename T, typename F> static std::function<int(Frame&)> unary(Type type, F fn) {
    return [type, fn](Frame& f) {
        if (argCount(f) != 0 || !f.receiver || f.receiver->type() != type) {
            fail(f, "unary function: argument error");
            return 0;
        }
        fn(*static_cast<T*>(f.receiver.get()));
        f.thread->result = f.receiver;
        return 0;
    };
}
// 0x19BED0
template <typename T, typename F> static std::function<int(Frame&)> compare(Type type, F fn) {
    return [type, fn](Frame& f) {
        if (argCount(f) != 1) {
            fail(f, "cmpr: no compare target");
            return 0;
        }
        const Ref a = arg(f, 0);
        if (!f.receiver || !a || f.receiver->type() != type || a->type() != type) {
            fail(f, "cmpr: compare different type");
            return 0;
        }
        f.thread->result = f.thread->vm->newInt(fn(*static_cast<T*>(f.receiver.get()), *static_cast<T*>(a.get())));
        return 0;
    };
}

void VM::def(const Ref& on, const char* name, std::function<int(Frame&)> begin, std::function<int(Frame&)> step) {
    auto n = std::make_shared<Native>();
    n->onBegin = std::move(begin);
    n->onStep = std::move(step);
    on->attrs[name] = n;
}

static std::u16string toString(VM& vm, const Ref& v) {
    if (Int* i = asInt(v)) {
        char text[32];
        snprintf(text, sizeof text, "%d", i->value);
        return widen(text);
    }
    if (Float* x = asFloat(v)) {
        char text[64];
        snprintf(text, sizeof text, "%f", x->value);
        return widen(text);
    }
    if (String* s = asString(v))
        return s->value;
    if (List* l = asList(v)) {
        std::u16string out = u"[";
        for (size_t i = 0; i < l->items.size(); i++)
            out += (i ? u", " : u"") + toString(vm, l->items[i]);
        return out + u"]";
    }
    return u"";
}

VM::VM() {
    // newlib's rand (the game's, 0x124B10): a 64-bit LCG, the top 31 bits.
    random = [state = (uint64_t)1]() mutable {
        state = state * 6364136223846793005ULL + 1;
        return (int)((state >> 32) & 0x7FFFFFFF);
    };
    // ── Every object: SetValue(name, value), at(name)  (0x197F70) ──────────
    protoObject = std::make_shared<Obj>();
    def(protoObject, "SetValue", [](Frame& f) {         // 0x198EC0
        const std::u16string* name = argString(f, 0);
        if (argCount(f) != 2) {
            fail(f, "prim_value_SetVal: argument error");
            return 0;
        }
        if (name == nullptr)
            return 0;
        if (!setAttr(f.receiver, narrow(*name), arg(f, 1)))
            fail(f, "prim_value_SetVal: set error");
        else
            f.thread->result = arg(f, 1);
        return 0;
    });
    def(protoObject, "at", [](Frame& f) {               // 0x1989C0
        const std::u16string* name = argString(f, 0);
        if (argCount(f) != 1) {
            fail(f, "prim_value_SetVal: argument error");
            return 0;
        }
        if (name != nullptr)
            f.thread->result = getAttr(*f.thread->vm, f.receiver, narrow(*name));
        return 0;
    });

    // ── int (0x1A2140, 0x1A2940, 0x19A8A0) ────────────────────────────────
    protoInt = std::make_shared<Int>();
    def(protoInt, "Add", binary<Int>(Type::Int, [](Int& a, Int& b) { a.value += b.value; }));
    def(protoInt, "Sub", binary<Int>(Type::Int, [](Int& a, Int& b) { a.value -= b.value; }));
    def(protoInt, "Mul", binary<Int>(Type::Int, [](Int& a, Int& b) { a.value *= b.value; }));
    def(protoInt, "Div", binary<Int>(Type::Int, [](Int& a, Int& b) { a.value = b.value ? a.value / b.value : 0; }));
    def(protoInt, "Mod", binary<Int>(Type::Int, [](Int& a, Int& b) { a.value = b.value ? a.value % b.value : 0; }));
    def(protoInt, "And", binary<Int>(Type::Int, [](Int& a, Int& b) { a.value = a.value && b.value; }));
    def(protoInt, "Or", binary<Int>(Type::Int, [](Int& a, Int& b) { a.value = a.value || b.value; }));
    def(protoInt, "Set", binary<Int>(Type::Int, [](Int& a, Int& b) { a.value = b.value; }));
    def(protoInt, "Inv", unary<Int>(Type::Int, [](Int& a) { a.value = -a.value; }));
    def(protoInt, "Not", unary<Int>(Type::Int, [](Int& a) { a.value = !a.value; }));
    def(protoInt, "eq", compare<Int>(Type::Int, [](Int& a, Int& b) { return a.value == b.value; }));
    def(protoInt, "ne", compare<Int>(Type::Int, [](Int& a, Int& b) { return a.value != b.value; }));
    def(protoInt, "lt", compare<Int>(Type::Int, [](Int& a, Int& b) { return a.value < b.value; }));
    def(protoInt, "le", compare<Int>(Type::Int, [](Int& a, Int& b) { return a.value <= b.value; }));
    def(protoInt, "gt", compare<Int>(Type::Int, [](Int& a, Int& b) { return a.value > b.value; }));
    def(protoInt, "ge", compare<Int>(Type::Int, [](Int& a, Int& b) { return a.value >= b.value; }));

    // ── float (0x1A2540, 0x1A2D40, 0x19A600) ──────────────────────────────
    protoFloat = std::make_shared<Float>();
    def(protoFloat, "Add", binary<Float>(Type::Float, [](Float& a, Float& b) { a.value += b.value; }));
    def(protoFloat, "Sub", binary<Float>(Type::Float, [](Float& a, Float& b) { a.value -= b.value; }));
    def(protoFloat, "Mul", binary<Float>(Type::Float, [](Float& a, Float& b) { a.value *= b.value; }));
    def(protoFloat, "Div", binary<Float>(Type::Float, [](Float& a, Float& b) { a.value /= b.value; }));
    def(protoFloat, "Set", binary<Float>(Type::Float, [](Float& a, Float& b) { a.value = b.value; }));
    def(protoFloat, "Inv", unary<Float>(Type::Float, [](Float& a) { a.value = -a.value; }));
    def(protoFloat, "eq", compare<Float>(Type::Float, [](Float& a, Float& b) { return a.value == b.value; }));
    def(protoFloat, "ne", compare<Float>(Type::Float, [](Float& a, Float& b) { return a.value != b.value; }));
    def(protoFloat, "lt", compare<Float>(Type::Float, [](Float& a, Float& b) { return a.value < b.value; }));
    def(protoFloat, "le", compare<Float>(Type::Float, [](Float& a, Float& b) { return a.value <= b.value; }));
    def(protoFloat, "gt", compare<Float>(Type::Float, [](Float& a, Float& b) { return a.value > b.value; }));
    def(protoFloat, "ge", compare<Float>(Type::Float, [](Float& a, Float& b) { return a.value >= b.value; }));
    // 0x19D3A0 / 0x19CF40: of the argument if there is one, else of the receiver.
    auto trig = [](float (*fn)(float)) {
        return [fn](Frame& f) {
            float x = 0;
            if (argCount(f) == 1) {
                if (Float* a = asFloat(arg(f, 0)))
                    x = a->value;
            } else if (Float* a = asFloat(f.receiver)) {
                x = a->value;
            }
            f.thread->result = f.thread->vm->newFloat(fn(x));
            return 0;
        };
    };
    def(protoFloat, "Sin", trig(sinf));
    def(protoFloat, "Cos", trig(cosf));

    // ── string (0x199270) ─────────────────────────────────────────────────
    protoString = std::make_shared<String>();
    def(protoString, "Add", binary<String>(Type::String, [](String& a, String& b) { a.value += b.value; }));
    def(protoString, "Set", binary<String>(Type::String, [](String& a, String& b) { a.value = b.value; }));
    def(protoString, "eq", compare<String>(Type::String, [](String& a, String& b) { return a.value == b.value; }));

    // ── list (0x199BD0) ───────────────────────────────────────────────────
    protoList = std::make_shared<List>();
    def(protoList, "size", [](Frame& f) {               // 0x1A1F50
        if (List* l = asList(f.receiver))
            f.thread->result = f.thread->vm->newInt((s32)l->items.size());
        return 0;
    });
    def(protoList, "push_back", [](Frame& f) {          // 0x1A1C30
        if (List* l = asList(f.receiver)) {
            for (size_t i = 0; i < argCount(f); i++)
                l->items.push_back(arg(f, i));
            f.thread->result = f.receiver;
        }
        return 0;
    });
    def(protoList, "push_front", [](Frame& f) {         // 0x1A3670
        if (List* l = asList(f.receiver)) {
            for (size_t i = 0; i < argCount(f); i++)
                l->items.insert(l->items.begin(), arg(f, i));
            f.thread->result = f.receiver;
        }
        return 0;
    });
    def(protoList, "resize", [](Frame& f) {             // 0x1A3160
        s32 n;
        List* l = asList(f.receiver);
        if (l == nullptr || argCount(f) != 1 || !argInt(f, 0, &n) || n < 0) {
            fail(f, "resize: argument error");
            return 0;
        }
        while ((s32)l->items.size() < n)
            l->items.push_back(f.thread->vm->newObject());
        l->items.resize((size_t)n);
        f.thread->result = f.receiver;
        return 0;
    });
    def(protoList, "at", [](Frame& f) {                 // 0x1A3990
        s32 i;
        List* l = asList(f.receiver);
        if (l == nullptr || argCount(f) != 1 || !argInt(f, 0, &i)) {
            fail(f, "at: argument error");
            return 0;
        }
        if (i < 0 || (size_t)i >= l->items.size()) {
            fail(f, "at: out of range");
            return 0;
        }
        f.thread->result = l->items[(size_t)i];
        return 0;
    });

    // ToString: 0x19DAA0 (int), 0x19D800 (float), 0x19CAB0 (string: itself), 0x1A3CD0 (list).
    for (const Ref& proto : {Ref(protoInt), Ref(protoFloat), Ref(protoList)})
        def(proto, "ToString", [](Frame& f) {
            f.thread->result = f.thread->vm->newString(toString(*f.thread->vm, f.receiver));
            return 0;
        });
    def(protoString, "ToString", [](Frame& f) {
        f.thread->result = f.receiver;
        return 0;
    });

    // ── The root (0x19ACE0, 0x1B6870) ─────────────────────────────────────
    root = newObject();
    def(root, "print", [](Frame& f) {                   // 0x19C580
        std::u16string text;
        for (size_t i = 0; i < argCount(f); i++)
            text += toString(*f.thread->vm, arg(f, i));
        VM& vm = *f.thread->vm;
        if (vm.output) vm.output(utf8(text));
        else printf("%s\n", utf8(text).c_str());
        return 0;
    });
    root->attrs["__if"] = std::make_shared<If>();
    root->attrs["__switch"] = std::make_shared<Switch>();
    root->attrs["__while"] = std::make_shared<While>();
    root->attrs["__for"] = std::make_shared<For>();
    def(root, "Rand", [](Frame& f) {                    // 0x19CBD0
        s32 n = 0x7FFF;                                 // (the default is *0x2EDB24)
        if (argCount(f) == 1)
            argInt(f, 0, &n);
        if (n < 1)
            n = 1;
        f.thread->result = f.thread->vm->newInt(f.thread->vm->random() % n);
        return 0;
    });
    def(root, "StartThread", [](Frame& f) {             // 0x19E1E0: (receiver, function, arguments)
        VM& vm = *f.thread->vm;
        if (argCount(f) < 3 || !asList(arg(f, 2))) {
            fail(f, "api_StartThread: argument error");
            return 0;
        }
        // The new thread goes in front of the running one in the VM's list;
        // `f` stays valid (std::list).
        Thread* self = f.thread;
        const Ref handle = vm.start(arg(f, 0), arg(f, 1), arg(f, 2), vm.newObject());
        self->result = handle;
        return 0;
    });
    def(root, "GetThreadHandle", [](Frame& f) {         // 0x19E080
        f.thread->result = f.thread->state;
        return 0;
    });
    root->attrs["List"] = protoList;
    // Thread handles are the threads' state ints: Stop / Suspend / Resume (0x19DF90).
    def(protoInt, "Stop", [](Frame& f) { if (Int* s = asInt(f.receiver)) s->value = 1; return 0; });
    def(protoInt, "Suspend", [](Frame& f) { if (Int* s = asInt(f.receiver)) s->value = 2; return 0; });
    def(protoInt, "Resume", [](Frame& f) { if (Int* s = asInt(f.receiver)) s->value = 0; return 0; });
}

Ref VM::newInt(s32 v) {                     // 0x1B64A0: the prototype's clone
    auto r = std::static_pointer_cast<Int>(protoInt->clone());
    r->value = v;
    return r;
}
Ref VM::newFloat(float v) {                 // 0x1B6430
    auto r = std::static_pointer_cast<Float>(protoFloat->clone());
    r->value = v;
    return r;
}
Ref VM::newString(const std::u16string& v) {    // 0x1B6200
    auto r = std::static_pointer_cast<String>(protoString->clone());
    r->value = v;
    return r;
}
Ref VM::newList() {                         // 0x1B5FE0
    auto r = std::static_pointer_cast<List>(protoList->clone());
    r->items.clear();
    return r;
}
Ref VM::newObject() { return protoObject->clone(); }    // 0x197F70
Ref VM::noData(const std::string& message) {
    auto r = std::make_shared<NoData>();
    r->message = message;
    return r;
}
void VM::error(const std::string& message) {
    lastError = message;
    fprintf(stderr, "[script] ERROR : %s\n", message.c_str());
}

Ref VM::start(const Ref& receiver, const Ref& method, const Ref& args, const Ref& scope) {    // 0x1B54C0
    threads.emplace_front();
    Thread& t = threads.front();
    t.vm = this;
    t.state = std::static_pointer_cast<Int>(newInt(0));
    call(t, receiver, method, args, scope);
    return t.state;
}

Ref VM::callNow(const Ref& receiver, const Ref& method, const Ref& args, const Ref& scope) {  // 0x1B4E50
    Thread t;
    t.vm = this;
    t.state = std::static_pointer_cast<Int>(newInt(0));
    call(t, receiver, method, args, scope);
    for (int guard = 0; guard < 1000000 && !t.step(1); guard++) {
    }
    return t.result;
}

bool VM::resolve(const std::string& path, Ref* object, Ref* method) {       // 0x1B50D0
    *object = root;
    size_t from = 0;
    for (size_t dot; (dot = path.find('.', from)) != std::string::npos; from = dot + 1)
        *object = getAttr(*this, *object, path.substr(from, dot - from));
    *method = getAttr(*this, *object, path.substr(from));
    return (*method)->type() != Type::NoData;
}

bool VM::update(int budget) {               // 0x1B4F90
    for (auto it = threads.begin(); it != threads.end();) {
        if (it->step(budget))
            it = threads.erase(it);
        else
            ++it;
    }
    return threads.empty();
}

}  // namespace sol
