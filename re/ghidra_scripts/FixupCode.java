// Applies code fixups found outside Ghidra:
//  - "functions": addresses that must be functions (e.g. virtual methods only reachable via vtables)
//  - "jumptables": computed jumps whose case targets were recovered from switch tables
// Usage (headless): -postScript FixupCode.java /path/to/fixups.json
//@category PSP port

import java.io.FileReader;

import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;

import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.RefType;
import ghidra.program.model.symbol.SourceType;

public class FixupCode extends GhidraScript {

	@Override
	public void run() throws Exception {
		JsonObject root;
		try (FileReader r = new FileReader(getScriptArgs()[0])) {
			root = JsonParser.parseReader(r).getAsJsonObject();
		}
		int created = 0, existing = 0, failed = 0;
		for (JsonElement e : root.getAsJsonArray("functions")) {
			Address a = toAddr(e.getAsString());
			if (getFunctionAt(a) != null) {
				existing++;
				continue;
			}
			// The address may sit inside another function's body; carve it out as its own function.
			Function containing = getFunctionContaining(a);
			disassemble(a);
			if (createFunction(a, null) != null) {
				created++;
				if (containing != null) {
					CreateFunctionCmd.fixupFunctionBody(currentProgram, containing, monitor);
				}
			}
			else {
				failed++;
				printerr("could not create function at " + a);
			}
		}

		int refs = 0;
		for (JsonElement e : root.getAsJsonArray("jumptables")) {
			JsonObject t = e.getAsJsonObject();
			Address at = toAddr(t.get("at").getAsString());
			for (JsonElement tgt : t.getAsJsonArray("targets")) {
				Address to = toAddr(tgt.getAsString());
				disassemble(to);
				currentProgram.getReferenceManager()
						.addMemoryReference(at, to, RefType.COMPUTED_JUMP, SourceType.USER_DEFINED, 0);
				refs++;
			}
			Function f = getFunctionContaining(at);
			if (f != null) {
				CreateFunctionCmd.fixupFunctionBody(currentProgram, f, monitor);
			}
		}
		println("functions created=" + created + " existing=" + existing + " failed=" + failed +
			", jump refs added=" + refs);
	}
}
