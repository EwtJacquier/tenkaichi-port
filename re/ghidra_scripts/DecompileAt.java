// Prints the decompiled C of the functions containing the given addresses.
// Usage (headless, read-only): -postScript DecompileAt.java 08a9fb88 0887dfc4 ...
//@category PSP port

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;

public class DecompileAt extends GhidraScript {

	@Override
	public void run() throws Exception {
		DecompInterface di = new DecompInterface();
		di.setOptions(new DecompileOptions());
		di.toggleCCode(true);
		di.openProgram(currentProgram);
		for (String a : getScriptArgs()) {
			Function f = getFunctionContaining(toAddr(a));
			if (f == null) {
				println("== " + a + ": no function");
				continue;
			}
			DecompileResults res = di.decompileFunction(f, 120, monitor);
			println("== " + a + " in " + f.getName() + " @ " + f.getEntryPoint() + "\n" +
				(res.decompileCompleted() ? res.getDecompiledFunction().getC() : res.getErrorMessage()));
		}
		di.dispose();
	}
}
