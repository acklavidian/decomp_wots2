// Print all references to the given addresses.
// Usage (via tools/ghidra_decomp.sh --xrefs): Xrefs.java <outfile> 0x118728 [0x...]
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.Symbol;
import java.io.PrintWriter;

public class Xrefs extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        PrintWriter out = new PrintWriter(args[0]);
        for (int i = 1; i < args.length; i++) {
            Address addr = toAddr(Long.decode(args[i]));
            Symbol sym = getSymbolAt(addr);
            out.println("==== " + (sym != null ? sym.getName() : "?") + " @ " + addr);
            for (Reference ref : getReferencesTo(addr)) {
                Function fn = getFunctionContaining(ref.getFromAddress());
                out.println("  " + ref.getFromAddress() + "  " + ref.getReferenceType() + "  "
                            + (fn != null ? fn.getName() : "(no function)"));
            }
        }
        out.close();
    }
}
