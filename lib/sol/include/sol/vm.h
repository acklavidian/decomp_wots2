#pragma once
#include <cstdint>

#include <functional>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <vector>

// The Sol virtual machine: Way of the Samurai 2's event-script language (the
// story, shops, the opening).  Derived from the game's runtime at 0x191B10–
// 0x1BC3C0, op by op and built-in by built-in, but its objects live on the
// host heap: nothing outside the VM reads their layout.  The compiler that
// makes its byte code is sol/compiler.h.  No dependency on the game: a host
// registers its own built-ins with VM::def.

namespace sol {

using s16 = int16_t;
using s32 = int32_t;
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;

struct Obj;
struct Frame;
struct Thread;
struct VM;
using Ref = std::shared_ptr<Obj>;

// What an object's type id tells apart (vtable +0x0C in the original).
enum class Type { Object, Int, Float, String, List, Code, Native, NoData, Other };

// Frame status / what a step answers.
enum : int { kGoOn = 0, kYield = 1, kEnd = 2, kReturn = 3, kBreak = 4, kError = 5 };

struct Obj {
    std::map<std::string, Ref> attrs;       // (a hash table in the original)
    virtual ~Obj() = default;
    virtual Type type() const { return Type::Object; }
    // vtable +0x14: a copy, attributes cloned one by one (0x1BA130).
    virtual Ref clone() const;
    // vtable +0x1C / +0x20: start a call (0 = it is already over) / go on with it.
    virtual int begin(Frame&) { return 0; }
    virtual int step(Frame&) { return kEnd; }
    void cloneAttrs(const Obj& from);
};

struct Int : Obj {
    s32 value = 0;
    Type type() const override { return Type::Int; }
    Ref clone() const override;
};
struct Float : Obj {
    float value = 0;
    Type type() const override { return Type::Float; }
    Ref clone() const override;
};
struct String : Obj {
    std::u16string value;
    Type type() const override { return Type::String; }
    Ref clone() const override;
};
struct List : Obj {
    std::vector<Ref> items;
    Type type() const override { return Type::List; }
    Ref clone() const override;
};
// What a failed lookup or a failed built-in leaves behind.
struct NoData : Obj {
    std::string message;
    Type type() const override { return Type::NoData; }
    Ref clone() const override;
};

// A built-in: begin does all the work unless it has something to wait for,
// in which case it answers non-zero and `step` is called until it ends.
struct Native : Obj {
    std::function<int(Frame&)> onBegin;
    std::function<int(Frame&)> onStep;
    Type type() const override { return Type::Native; }
    Ref clone() const override;
    int begin(Frame& f) override { return onBegin ? onBegin(f) : 0; }
    int step(Frame& f) override { return onStep ? onStep(f) : kEnd; }
};

struct Frame {
    Thread* thread = nullptr;
    Ref receiver;           // "parent" to script code
    Ref callee;
    size_t base = 0;        // the thread's stack size when the call began
    bool flagged = false;   // a loop: where `break` stops
    int status = kGoOn;
    Ref args;               // a List
    Ref scope;              // "local"
    // Free for a built-in that runs over several steps.
    int phase = 0, value = 0;
    float timer = 0;
    Ref keep;
};

struct Thread {
    VM* vm = nullptr;
    Ref result;
    std::list<Frame> frames;        // front = the running one
    std::vector<Ref> stack;
    std::shared_ptr<Int> state;     // 0 running, 1 stopped, 2 suspended; scripts hold it as the handle
    bool runnable();                // 0x1B7420
    void stop();                    // 0x1B7020
    bool step(int budget);          // 0x1B71E0  → true when it has finished
};

struct VM {
    Ref root;
    std::shared_ptr<Int> protoInt;
    std::shared_ptr<Float> protoFloat;
    std::shared_ptr<String> protoString;
    std::shared_ptr<List> protoList;
    Ref protoObject;
    std::list<Thread> threads;
    std::string lastError;
    // Development aid, not in the original: asked for an attribute an object
    // does not have (`call`: it is about to be called) before the script
    // fails — lets scripts run while built-ins are still missing.
    std::function<Ref(const Ref& object, const std::string& name, bool call)> missing;
    // The root's Rand(n) draws from this (the game: its own rand, 0x124B10).
    // By default newlib's rand, which is what the game's is.
    std::function<int()> random;
    // Where the root's print(…) goes (one line, UTF-8).  By default stdout.
    std::function<void(const std::string& line)> output;

    VM();
    Ref newInt(s32 v);
    Ref newFloat(float v);
    Ref newString(const std::u16string& v);
    Ref newList();
    Ref newObject();
    Ref noData(const std::string& message);
    void error(const std::string& message);                     // 0x1B40D0

    // Registers a built-in on an object.
    void def(const Ref& on, const char* name, std::function<int(Frame&)> begin,
             std::function<int(Frame&)> step = nullptr);

    // A compiled script (the contents of a .sx file) as a callable; null if it is not one.
    Ref load(const std::vector<u8>& file, const std::string& name);
    // Starts `method` on a new thread (0x1B54C0); → its handle.
    Ref start(const Ref& receiver, const Ref& method, const Ref& args, const Ref& scope);
    // Runs `method` to the end at once on a thread of its own (0x1B4E50); → its result.
    Ref callNow(const Ref& receiver, const Ref& method, const Ref& args, const Ref& scope);
    // "A.B.name" from the root (0x1B50D0).
    bool resolve(const std::string& path, Ref* object, Ref* method);
    // One game frame (0x1B4F90): every thread gets `budget` steps.  → true when none is left.
    bool update(int budget);
};

Ref getAttr(VM& vm, const Ref& object, const std::string& name);       // 0x1B93A0
bool setAttr(const Ref& object, const std::string& name, const Ref& value);    // 0x1B9950
// 0x1B74F0: non-zero if the call is still going on (its frame stays).
int call(Thread& thread, const Ref& receiver, const Ref& method, const Ref& args, const Ref& scope);
// 0x1B7860: a built-in failed.
void fail(Frame& frame, const std::string& message);

// Argument helpers for built-ins.
size_t argCount(const Frame& f);
Ref arg(const Frame& f, size_t i);
bool argInt(const Frame& f, size_t i, s32* out);
bool argNumber(const Frame& f, size_t i, float* out);      // an int or a float
const std::u16string* argString(const Frame& f, size_t i);
std::string narrow(const std::u16string& s);
std::u16string widen(const std::string& s);
std::string utf8(const std::u16string& s);
std::u16string from_utf8(const std::string& s);

}  // namespace sol
