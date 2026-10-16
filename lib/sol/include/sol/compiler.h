#pragma once
// The Sol compiler: event-script source (.sol) → byte code (.sx).
//
// Decompiled from Way of the Samurai 2's own compiler, which the game carries
// next to its VM (0x1A9D90 and what it calls, 0x1A9D90–0x1B4240, 0x1BA730–
// 0x1BC000, the preprocessor 0x218760–0x21A330).  Its output is checked byte
// for byte against every compiled script on the disc.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace sol {

struct CompileResult {
    bool ok = false;
    std::vector<uint8_t> code;  // the byte code alone (what follows a .sx header)
    std::string error;          // "file:line: message" when !ok
};

// Reads an #include'd file: its name as written (e.g. "BGMList.h") → the
// file's bytes (UTF-16 with a byte-order mark, or UTF-8).  False if missing.
using IncludeLoader = std::function<bool(const std::string& name, std::vector<uint8_t>* bytes)>;

// `source` is a whole .sol file as stored (UTF-16 LE with BOM, or UTF-8);
// `name` is used in error messages and line markers.
CompileResult compile(const std::vector<uint8_t>& source, const std::string& name,
                      const IncludeLoader& include = nullptr);
CompileResult compile(const std::u16string& source, const std::string& name,
                      const IncludeLoader& include = nullptr);

// A .sx file: "TH00", u32 entry (0), u32 code size + 1, that rounded up to 4,
// u32 0, then the code padded with zeros to that size.
std::vector<uint8_t> make_sx(const std::vector<uint8_t>& code);

// UTF-16 from a file's bytes (BOM: UTF-16 LE / BE; otherwise UTF-8).
std::u16string decode_text(const std::vector<uint8_t>& bytes);

}  // namespace sol
