// Print decompiler output for the functions at the given addresses.
// Usage (via tools/ghidra_decomp.sh): Decompile.java <outfile> 0x1107f0 [0x...]
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import java.io.PrintWriter;

public class Decompile extends GhidraScript {
    @Override
    public void run() throws Exception {
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        String[] args = getScriptArgs();
        PrintWriter out = new PrintWriter(args[0]);
        for (int i = 1; i < args.length; i++) {
            String arg = args[i];
            Address addr = toAddr(Long.decode(arg));
            Function fn = getFunctionContaining(addr);
            if (fn == null) {
                disassemble(addr);
                fn = createFunction(addr, null);
            }
            if (fn == null) {
                out.println("==== " + arg + ": no function");
                continue;
            }
            DecompileResults res = decomp.decompileFunction(fn, 120, monitor);
            out.println("==== " + fn.getName() + " @ " + fn.getEntryPoint());
            if (res.decompileCompleted()) {
                out.println(res.getDecompiledFunction().getC());
            } else {
                out.println("decompile failed: " + res.getErrorMessage());
            }
        }
        out.close();
        decomp.dispose();
    }
}
