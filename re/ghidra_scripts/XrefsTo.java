// Prints the references to the strings matching a pattern (read-only).
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;

public class XrefsTo extends GhidraScript {
	@Override
	public void run() throws Exception {
		String pat = getScriptArgs()[0];
		DataIterator it = currentProgram.getListing().getDefinedData(true);
		while (it.hasNext()) {
			Data d = it.next();
			Object v = d.getValue();
			if (!(v instanceof String) || !((String) v).matches(pat)) continue;
			for (Reference r : getReferencesTo(d.getAddress())) {
				Function f = getFunctionContaining(r.getFromAddress());
				println("XREF " + d.getAddress() + " " + v + " <- " + r.getFromAddress() + " " + (f == null ? "?" : f.getName()));
			}
		}
	}
}
