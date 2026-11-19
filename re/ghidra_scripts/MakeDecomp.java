// Creates functions at the given addresses (in memory only: the project is opened read-only) and prints their pseudo-C.
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class MakeDecomp extends GhidraScript {
	@Override
	public void run() throws Exception {
		DecompInterface d = new DecompInterface();
		d.openProgram(currentProgram);
		for (String s : getScriptArgs()) {
			Address a = toAddr(s);
			Function f = getFunctionAt(a);
			if (f == null) {
				Function old = getFunctionContaining(a);
				if (old != null) removeFunction(old);
				disassemble(a);
				f = createFunction(a, "F_" + s);
			}
			if (f == null) { println("== " + s + ": cannot create"); continue; }
			DecompileResults r = d.decompileFunction(f, 120, monitor);
			println("== " + s + "\n" + (r.getDecompiledFunction() != null ? r.getDecompiledFunction().getC() : r.getErrorMessage()));
		}
	}
}
