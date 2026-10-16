// Debug aid for the event-script VM (lib/sol/src/vm.cpp):
//   WOTS2_RUN_SCRIPT=<name> ./build/wots2 extracted
// loads script/<name>.sx, runs it for a while with every built-in the game
// would supply replaced by a stub that reports itself once, and quits.  It
// shows which built-ins a script needs and that the VM runs its code.

#include "sol/vm.h"
#include "native/archive.h"

#include <cstdio>
#include <set>
#include <string>

int hle_run_script(const char* name) {
    sol::VM vm;
    static std::set<std::string> reported;
    vm.missing = [&vm](const sol::Ref& object, const std::string& name, bool call) -> sol::Ref {
        if (reported.insert(name).second)
            printf("[script] not there: %s%s\n", name.c_str(), call ? "()" : "");
        if (!call) {
            // A constant or a sub-object: remember an empty object for it, so
            // that both `X.constant` and `X.Sub.Method()` get further.
            sol::Ref made = vm.newObject();
            if (object)
                object->attrs[name] = made;
            return made;
        }
        auto stub = std::make_shared<sol::Native>();
        stub->onBegin = [&vm](sol::Frame& f) {
            f.thread->result = vm.newInt(0);
            return 0;
        };
        return stub;
    };
    // Objects scripts expect in the root, so that `Map.Load(…)` gets as far
    // as asking for `Load`.
    for (const char* object : {"Map", "Character", "UI", "Effect", "Camera", "Locator", "MapID", "CharaID",
                               "CharModelID", "Sound", "System", "Event", "Flag", "Item", "Weapon",
                               "EffectCinescoID", "MotionID"})
        vm.root->attrs[object] = vm.newObject();
    std::vector<u8> file;
    const std::string path = std::string("script/") + name + ".sx";
    if (!archive::read(path.c_str(), file)) {
        fprintf(stderr, "[script] %s is not in the archive\n", path.c_str());
        return 1;
    }
    const sol::Ref code = vm.load(file, name);
    if (!code) {
        fprintf(stderr, "[script] %s is not a compiled script\n", path.c_str());
        return 1;
    }
    vm.start(vm.root, code, vm.newList(), vm.root);
    int frames = 0;
    while (frames < 600 && !vm.update(1000))
        frames++;
    printf("[script] %s: %s after %d frames\n", name, vm.threads.empty() ? "finished" : "still running", frames);
    printf("[script] root now has:");
    for (const auto& a : vm.root->attrs)
        printf(" %s", a.first.c_str());
    printf("\n");
    return 0;
}
