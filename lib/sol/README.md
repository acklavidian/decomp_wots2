# Sol: the event-script language of Way of the Samurai 2

*Way of the Samurai 2* (Acquire / Spike, PS2, 2003) runs its story, its shops
and its opening from scripts in a small language of the developers' own. The
game carries the whole language with it: a compiler from source (`.sol`) to
byte code (`.sx`), and a VM that runs the byte code with cooperative threads.
This directory is both, decompiled from the game's executable (SLUS_208.93)
into plain C++17 with no other dependencies, so other projects can use them.

- **The compiler** (`sol/compiler.h`) is decompiled from 0x1A9D90 and what it
  calls. Its output is byte-for-byte identical to the game's compiled files:
  `solc --check` compiles all 164 scripts on the disc that come with their
  source, including the developers' 400 KB test script, and compares each
  result with the disc's `.sx`. Only the padding after the code differs, and
  that padding is whatever was in the developers' memory when they wrote the
  file.
- **The VM** (`sol/vm.h`) is decompiled from 0x191B10–0x1BC3C0: values and
  their methods, the 22 op codes, frames, threads, the `__if` / `__while` /
  `__for` / `__switch` constructs. Its objects live on the host heap, and
  nothing outside the VM depends on their layout.

The game's own built-ins (`Character`, `Camera`, `UI` and the rest, about 230
of them) are not here. They belong to the game and live in
`src/game/event_player.cpp` of the port. A host registers whatever built-ins
it needs with `VM::def`.

## Building

```bash
cmake -S lib/sol -B build-sol && cmake --build build-sol
```

or `add_subdirectory(sol)` and link the `sol` target. Two tools come with it:

```bash
solc script.sol [-o script.sx] [-I dir]   # compile
solc --check dir                          # compile dir/**/*.sol, compare with the .sx beside each
solrun script.sol [-I dir] [--call Name]  # run a script: the VM's built-ins, plus
                                          # Wait / WaitFor / Rapid / Require / StartScript;
                                          # anything else is a stub that answers 0
```

`solrun examples/tour.sol` runs the example below.

## Using it from C++

```cpp
#include "sol/compiler.h"
#include "sol/vm.h"

sol::VM vm;
vm.def(vm.root, "Beep", [](sol::Frame& f) {            // a built-in that answers at once
    sol::s32 n = 0;
    sol::argInt(f, 0, &n);
    f.thread->result = f.thread->vm->newInt(n * 2);
    return 0;                                          // 0 = done
});
vm.def(vm.root, "Sleep", [](sol::Frame& f) {           // one that waits: begin answers 1,
    f.phase = 30;                                      // then step runs once a frame
    return 1;
}, [](sol::Frame& f) { return f.phase-- > 0 ? sol::kYield : sol::kEnd; });

auto r = sol::compile(sourceBytes, "main.sol");       // UTF-16 with a BOM, or UTF-8
if (!r.ok) { puts(r.error.c_str()); return; }
sol::Ref code = vm.load(sol::make_sx(r.code), "main");
vm.start(vm.root, code, vm.newList(), vm.root);        // run it on a thread of its own
while (!vm.update(10)) { /* one game frame: each thread gets 10 ops */ }
```

`VM::random` (the root's `Rand`) defaults to newlib's `rand`, the game's.
`VM::output` takes what `print` prints. `VM::missing` is asked for any
attribute that is not there, which lets a host stub what it lacks. `VM::resolve("A.B.Name")`
finds a function a script defined, as the game finds entry points such as
`StartupEvent`.

## The language

```
#include "BGMList.h"            // preprocessor: #include "file", #define NAME text
Counter = 0;                    // a name not declared with var is an attribute of the root
Add = def(a, b) {               // a function; its arguments are locals
    var sum = a;                // locals: var
    sum += b;
    return sum;
};
Point = new object;             // objects are bags of attributes; new copies one
Point.x = 1.0;
Point.Show = def { print("x = ", this.x); };    // this = whatever the call was made on
if (Counter == 0) { Point.Show(); } else if (Counter > 3) { … } else { … }
while (Counter < 3) { Counter += 1; }
for (var i = 0; i < 3; i += 1) { … }
switch (Counter) case 1 { … } case 3 { … } else { … }
List = [1, 2.5, "three"];  List.push_back(4);  print(List[2], List.size());
StartThread(root, def { Wait(1.0); print("later"); }, []);     // a cooperative thread
```

Things that will surprise you. All of them are what the original does.

- **No operator precedence.** An operator's right-hand side is the whole rest
  of the expression: `2 * 3 + 4` is 14, `10 - 2 - 3` is 11, and `-1 + 2` is
  −3. Use parentheses.
- **Operators are method calls.** `a + b` is `(copy of a).Add(b)`, and
  `a += b` is `a.Add(b)`, which changes `a` in place. `a = b` on a plain value
  is `a.Set(b)`. `x.y = b` sets the attribute.
- **Strict types.** `1.5 * 2` is an error (float times int). Write `1.5 * 2.0`.
  An error leaves a "no data" value behind, and the thread goes on.
- **Numbers.** `0.5` is a float, `5` is an int, and integers are read as C
  reads them, so `0x10` is 16 and `010` is 8.
- **Names.** A name that is not a `var` or an argument of the function it is
  in refers to the root object. Blocks (`if`, `while`, …) share their
  function's locals, but a nested `def` does not: it has its own.
- **`#define` respaces the line.** On a line where a macro is replaced, every
  token is put back with a space between, so `x += LIMIT;` becomes
  `x + = 5;`, and that is a syntax error. Keep such lines simple.
- **Text is UTF-16.** The game's files are UTF-16 with a byte-order mark. The
  compiler also takes UTF-8. Names (identifiers) are 8-bit.
- Statements need `;` after expressions; `def { … };` is an expression too.

## The byte code (`.sx`)

```
"TH00"  u32 entry (0)  u32 code size + 1  u32 that rounded up to 4  u32 0
code…   zero padding
```

| Op | Name | Operand | Does |
|----|------|---------|------|
| `0x00` | end | | end of a code block |
| `T` | nop | | a statement boundary (`;`) |
| ` ` | obj | | push a new empty object |
| `!` | int | s32 | push an int |
| `$` | float | f32 | push a float |
| `"` | wstring | u16 n, n × u16 | push a string |
| `#` | def | s32 n | push a function whose code is the next n bytes |
| `%` | list | s32 n | pop n values, push them as a list |
| `0` `1` `2` | getroot / getparent / getlocal | | push the root / the receiver (`this`) / the locals |
| `3` `4` | getargv / getargs | s16 i / – | push argument i / the argument list |
| `5` `6` `7` | getresult / popresult / popstack | | |
| `8` `9` | getattr / setattr | name | |
| `P` | sendmsg | name | receiver, scope, arguments → call the receiver's method |
| `Q` | operate | | scope, receiver, method, arguments → call |
| `R` | new | | replace the top with a copy |
| `S` | exit | s16 (3 return, 4 break) | |

A **name** operand is `s16 length` + bytes. A negative length is instead the
distance back to the same bytes earlier in the code: from the end of this
operand to the start of the earlier name's characters. The compiler takes the
first match within 0x7F00 bytes, and the match can lie inside any earlier
bytes, not only an earlier name.

How things compile: a call `A.B(x)` is `0 8"A" ␠ <x> %1 P"B" 5`; an
expression statement ends with `6 T`; `if (c) {…} else {…}` is
`2 1 0 8"__if" 4 #{c 6 end} #{… end} #{… end} %4 Q 5 7`. The loop
constructs are the same with `__while` / `__for` / `__switch`.
