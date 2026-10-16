// solrun: run a Sol script outside the game.
//
//   solrun script.sol|script.sx [-I dir] [--frames n] [--call Name] [--quiet]
//
// The VM's own built-ins (numbers, strings, lists, __if / __while / __for /
// __switch, print, Rand, StartThread, …) are all there.  The game's root
// functions that only need time are supplied: Wait(seconds) / Wait(frames)
// at 30 frames a second, WaitFor(def), Rapid(), Require(name) and
// StartScript(name) (name.sol compiled, else name.sx, looked up in the -I
// directories).  Anything else a script asks for — Map, Character, UI … — is
// a stub that answers 0 and is reported once.
//
// The script runs to its end (or for --frames game frames, default 18000);
// --call Name then calls one of the functions it defined, as the game calls
// entry points such as StartupEvent.
#include "sol/compiler.h"
#include "sol/vm.h"

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using sol::Frame;
using sol::Ref;

static std::vector<fs::path> sDirs;
static bool sQuiet = false;

// The game's archive ignores case ("FlagID" is flagID.sol): so do these lookups.
static fs::path nocase(const fs::path& dir, const fs::path& rel) {
    fs::path at = dir;
    for (const auto& part : rel) {
        fs::path exact = at / part;
        if (fs::exists(exact)) { at = exact; continue; }
        std::string want = part.string();
        for (auto& c : want) c = (char)tolower((unsigned char)c);
        bool found = false;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(at, ec)) {
            std::string n = e.path().filename().string();
            for (auto& c : n) c = (char)tolower((unsigned char)c);
            if (n == want) { at = e.path(); found = true; break; }
        }
        if (!found) return dir / rel;
    }
    return at;
}

static bool read_file(const fs::path& p, std::vector<uint8_t>* out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out->assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

static bool find(const std::string& name, const char* ext, fs::path* out) {
    fs::path p(name);
    if (p.extension() == ext && fs::exists(p)) { *out = p; return true; }
    for (const auto& d : sDirs) {
        fs::path q = nocase(d, name + ext);
        if (fs::exists(q)) { *out = q; return true; }
    }
    return false;
}

// name, a path or a script name → its code (compiled from .sol when there is one).
static Ref load(sol::VM& vm, const std::string& name) {
    fs::path p;
    std::vector<uint8_t> bytes;
    if (find(name, ".sol", &p) || (fs::path(name).extension() == ".sol" && fs::exists(p = name))) {
        read_file(p, &bytes);
        auto inc = [dir = p.parent_path()](const std::string& file, std::vector<uint8_t>* out) {
            if (read_file(nocase(dir, file), out)) return true;
            for (const auto& d : sDirs)
                if (read_file(nocase(d, file), out)) return true;
            return false;
        };
        const auto r = sol::compile(bytes, p.filename().string(), inc);
        if (!r.ok) { fprintf(stderr, "solrun: %s\n", r.error.c_str()); return nullptr; }
        return vm.load(sol::make_sx(r.code), name);
    }
    if (find(name, ".sx", &p) || (fs::path(name).extension() == ".sx" && fs::exists(p = name))) {
        read_file(p, &bytes);
        return vm.load(bytes, name);
    }
    fprintf(stderr, "solrun: no script %s\n", name.c_str());
    return nullptr;
}

static int sBudget = 10;

static void builtins(sol::VM& vm) {
    const Ref& root = vm.root;
    vm.def(root, "Wait", [](Frame& f) {
        sBudget = 10;
        sol::s32 frames;
        float seconds;
        if (sol::argInt(f, 0, &frames)) f.phase = frames;
        else if (sol::argNumber(f, 0, &seconds)) f.phase = (int)(seconds * 30.0f);
        else return 0;
        return 1;
    }, [](Frame& f) { return f.phase-- < 1 ? sol::kEnd : sol::kYield; });
    vm.def(root, "WaitFor", [](Frame& f) {
        if (sol::argCount(f) == 0) return 0;
        f.phase = 0;
        return 1;
    }, [&vm](Frame& f) {
        if (f.phase == 10) { f.phase = 0; return (int)sol::kGoOn; }
        if (f.phase == 1) {
            const Ref r = f.thread->result;
            if (!r || r->type() != sol::Type::Int) return (int)sol::kError;
            if (static_cast<sol::Int*>(r.get())->value != 0) return (int)sol::kEnd;
            f.phase++;
            return (int)sol::kGoOn;
        }
        if (f.phase != 0) { f.phase++; return (int)sol::kYield; }
        f.phase = 1;
        sol::call(*f.thread, f.receiver, sol::arg(f, 0), vm.newList(), f.scope);
        return (int)sol::kGoOn;
    });
    vm.def(root, "Rapid", [](Frame& f) {
        sol::s32 n = 100;
        sol::argInt(f, 0, &n);
        sBudget = n;
        return 0;
    });
    vm.def(root, "Require", [&vm](Frame& f) {
        const std::u16string* name = sol::argString(f, 0);
        const Ref code = name ? load(vm, sol::utf8(*name)) : nullptr;
        if (!code) return 0;
        sol::call(*f.thread, vm.root, code, vm.newList(), vm.newObject());
        return 1;
    }, [](Frame&) { return sol::kEnd; });
    vm.def(root, "StartScript", [&vm](Frame& f) {
        const std::u16string* name = sol::argString(f, 0);
        const Ref code = name ? load(vm, sol::utf8(*name)) : nullptr;
        if (code) vm.start(vm.root, code, vm.newList(), vm.newObject());
        return 0;
    });
    static std::set<std::string> reported;
    vm.missing = [&vm](const Ref& object, const std::string& name, bool call) -> Ref {
        if (!sQuiet && reported.insert(name).second)
            fprintf(stderr, "[solrun] not supplied: %s%s\n", name.c_str(), call ? "()" : "");
        if (!call) {
            Ref made = vm.newObject();
            if (object) object->attrs[name] = made;
            return made;
        }
        auto stub = std::make_shared<sol::Native>();
        stub->onBegin = [&vm](Frame& f) { f.thread->result = vm.newInt(0); return 0; };
        return stub;
    };
}

int main(int argc, char** argv) {
    std::string script, entry;
    long frames = 18000;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "-I" && i + 1 < argc) sDirs.push_back(argv[++i]);
        else if (a == "--frames" && i + 1 < argc) frames = atol(argv[++i]);
        else if (a == "--call" && i + 1 < argc) entry = argv[++i];
        else if (a == "--quiet") sQuiet = true;
        else script = a;
    }
    if (script.empty()) {
        fprintf(stderr, "usage: solrun script.sol|script.sx [-I dir] [--frames n] [--call Name] [--quiet]\n");
        return 2;
    }
    sDirs.insert(sDirs.begin(), fs::path(script).parent_path().empty() ? fs::path(".") : fs::path(script).parent_path());
    sol::VM vm;
    builtins(vm);
    const Ref code = load(vm, script);
    if (!code) return 1;
    vm.start(vm.root, code, vm.newList(), vm.root);
    long frame = 0;
    auto run = [&] {
        while (frame < frames && !vm.update(sBudget)) frame++;
    };
    run();
    if (!entry.empty()) {
        Ref object, method;
        if (!vm.resolve(entry, &object, &method) || !method) {
            fprintf(stderr, "solrun: the script defines no %s\n", entry.c_str());
            return 1;
        }
        vm.start(object, method, vm.newList(), vm.newObject());
        run();
    }
    fflush(stdout);
    if (!vm.lastError.empty()) fprintf(stderr, "solrun: %s\n", vm.lastError.c_str());
    if (!sQuiet)
        fprintf(stderr, "[solrun] %s after %ld frames (%.1f s)\n", vm.threads.empty() ? "finished" : "still running",
                frame, frame / 30.0);
    return vm.threads.empty() ? 0 : 3;
}
