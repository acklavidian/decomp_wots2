// Apply config/functions.json and config/globals.json to the Ghidra project.
// Usage (via tools/ghidra_decomp.sh --names): ApplyNames.java <functions.json> <globals.json>
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;
import java.io.FileReader;
import java.math.BigInteger;
import java.util.Map;

public class ApplyNames extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        int fns = 0, created = 0, labels = 0;
        JsonObject functions = JsonParser.parseReader(new FileReader(args[0])).getAsJsonObject();
        for (Map.Entry<String, com.google.gson.JsonElement> e : functions.entrySet()) {
            if (e.getKey().startsWith("_")) continue;
            Address addr = toAddr(Long.decode(e.getKey()));
            String name = e.getValue().getAsString();
            Function fn = getFunctionAt(addr);
            if (fn == null) {
                disassemble(addr);
                fn = createFunction(addr, name);
                if (fn == null) {
                    // Inside another function's body (e.g. a mid-function label)
                    createLabel(addr, name, true, SourceType.USER_DEFINED);
                    println("label only (not a function entry): " + e.getKey() + " " + name);
                    labels++;
                    continue;
                }
                created++;
            }
            fn.setName(name, SourceType.USER_DEFINED);
            fns++;
        }
        JsonObject globals = JsonParser.parseReader(new FileReader(args[1])).getAsJsonObject();
        for (Map.Entry<String, com.google.gson.JsonElement> e : globals.entrySet()) {
            if (e.getKey().startsWith("_")) continue;
            createLabel(toAddr(Long.decode(e.getKey())), e.getValue().getAsString(), true,
                        SourceType.USER_DEFINED);
            labels++;
        }
        // Stripped ELF has no _gp symbol: tell the decompiler what gp holds so
        // gp-relative loads resolve to real addresses.
        Register gp = currentProgram.getRegister("gp");
        currentProgram.getProgramContext().setValue(gp, toAddr(0x100000L), toAddr(0x2ee47fL),
                                                    BigInteger.valueOf(0x2f59f0L));
        println("named " + fns + " functions (" + created + " newly created), " + labels + " labels");
    }
}
