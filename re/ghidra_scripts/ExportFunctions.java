// Exports every function as one JSON line: pseudo-C, callees, callers and referenced strings.
// Usage (headless): -postScript ExportFunctions.java /path/to/functions.jsonl
//@category PSP port

import java.io.BufferedWriter;
import java.io.File;
import java.io.FileOutputStream;
import java.io.OutputStreamWriter;
import java.io.Writer;
import java.nio.charset.StandardCharsets;
import java.util.LinkedHashSet;
import java.util.Set;

import com.google.gson.Gson;
import com.google.gson.GsonBuilder;
import com.google.gson.JsonArray;
import com.google.gson.JsonObject;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressIterator;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceManager;

public class ExportFunctions extends GhidraScript {

	private static final int MAX_STR = 300;

	@Override
	public void run() throws Exception {
		String[] args = getScriptArgs();
		File out = args.length > 0 ? new File(args[0]) : askFile("Output JSONL", "Save");

		DecompInterface di = new DecompInterface();
		di.setOptions(new DecompileOptions());
		di.toggleCCode(true);
		di.toggleSyntaxTree(false);
		di.setSimplificationStyle("decompile");
		if (!di.openProgram(currentProgram)) {
			throw new RuntimeException("Decompiler failed to open program: " + di.getLastMessage());
		}

		Gson gson = new GsonBuilder().disableHtmlEscaping().create();
		FunctionManager fm = currentProgram.getFunctionManager();
		int total = fm.getFunctionCount();
		monitor.initialize(total);
		int done = 0, failed = 0;

		try (Writer w = new BufferedWriter(
			new OutputStreamWriter(new FileOutputStream(out), StandardCharsets.UTF_8))) {
			for (Function f : fm.getFunctions(true)) {
				if (monitor.isCancelled()) {
					break;
				}
				monitor.incrementProgress(1);

				JsonObject o = new JsonObject();
				o.addProperty("addr", f.getEntryPoint().toString());
				o.addProperty("name", f.getName(true));
				o.addProperty("size", f.getBody().getNumAddresses());
				o.addProperty("thunk", f.isThunk());
				o.addProperty("signature", f.getPrototypeString(false, false));

				JsonArray callees = new JsonArray();
				for (Function c : f.getCalledFunctions(monitor)) {
					callees.add(ref(c));
				}
				o.add("callees", callees);

				JsonArray callers = new JsonArray();
				for (Function c : f.getCallingFunctions(monitor)) {
					callers.add(ref(c));
				}
				o.add("callers", callers);

				JsonArray strs = new JsonArray();
				for (String s : referencedStrings(f)) {
					strs.add(s);
				}
				o.add("strings", strs);

				String code = null;
				if (!f.isThunk()) {
					DecompileResults res = di.decompileFunction(f, 120, monitor);
					if (res != null && res.decompileCompleted() && res.getDecompiledFunction() != null) {
						code = res.getDecompiledFunction().getC();
					}
					else {
						failed++;
						o.addProperty("error", res == null ? "null result" : res.getErrorMessage());
					}
				}
				o.addProperty("c", code);

				w.write(gson.toJson(o));
				w.write('\n');
				done++;
				if (done % 500 == 0) {
					println("exported " + done + "/" + total);
				}
			}
		}
		finally {
			di.dispose();
		}
		println("Done: " + done + " functions, " + failed + " decompile failures -> " + out);
	}

	private JsonObject ref(Function c) {
		JsonObject co = new JsonObject();
		co.addProperty("addr", c.getEntryPoint().toString());
		co.addProperty("name", c.getName(true));
		return co;
	}

	private Set<String> referencedStrings(Function f) {
		Set<String> found = new LinkedHashSet<>();
		Listing listing = currentProgram.getListing();
		ReferenceManager rm = currentProgram.getReferenceManager();
		AddressIterator it = rm.getReferenceSourceIterator(f.getBody(), true);
		while (it.hasNext()) {
			Address src = it.next();
			for (Reference r : rm.getReferencesFrom(src)) {
				Address to = r.getToAddress();
				if (!to.isMemoryAddress()) {
					continue;
				}
				Data d = listing.getDataAt(to);
				String s = null;
				if (d != null && d.hasStringValue()) {
					Object v = d.getValue();
					s = v == null ? null : v.toString();
				}
				else if (d == null || !d.isDefined()) {
					s = readCString(to);
				}
				if (s != null && s.length() >= 3) {
					found.add(s.length() > MAX_STR ? s.substring(0, MAX_STR) + "..." : s);
				}
			}
		}
		return found;
	}

	// Fallback for strings the auto-analysis did not define: printable ASCII ending in NUL.
	private String readCString(Address a) {
		Memory mem = currentProgram.getMemory();
		StringBuilder sb = new StringBuilder();
		try {
			for (int i = 0; i < MAX_STR; i++) {
				byte b = mem.getByte(a.add(i));
				if (b == 0) {
					return sb.length() >= 4 ? sb.toString() : null;
				}
				int c = b & 0xff;
				if (c == '\n' || c == '\t' || c == '\r') {
					sb.append(c == '\n' ? "\\n" : c == '\t' ? "\\t" : "\\r");
				}
				else if (c >= 0x20 && c < 0x7f) {
					sb.append((char) c);
				}
				else {
					return null;
				}
			}
		}
		catch (Exception e) {
			return null;
		}
		return null;
	}
}
