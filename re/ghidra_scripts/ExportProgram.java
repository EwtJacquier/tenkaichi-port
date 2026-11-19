// Exports what the static recompiler needs, as one JSON document:
//  - functions: entry, name, body ranges (start/end inclusive)
//  - jumptables: for every computed jump (jr reg, not ra) the possible targets Ghidra resolved
//  - datarefs: code addresses referenced from data (vtables, callbacks, function pointers)
// Usage (headless): -postScript ExportProgram.java /path/to/program.json
//@category PSP port

import java.io.BufferedWriter;
import java.io.File;
import java.io.FileOutputStream;
import java.io.OutputStreamWriter;
import java.io.Writer;
import java.nio.charset.StandardCharsets;
import java.util.TreeSet;

import com.google.gson.Gson;
import com.google.gson.JsonArray;
import com.google.gson.JsonObject;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressRange;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.FlowType;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

public class ExportProgram extends GhidraScript {

	@Override
	public void run() throws Exception {
		String[] args = getScriptArgs();
		File out = args.length > 0 ? new File(args[0]) : askFile("Output JSON", "Save");
		JsonObject root = new JsonObject();

		JsonArray funcs = new JsonArray();
		for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
			JsonObject o = new JsonObject();
			o.addProperty("entry", hex(f.getEntryPoint()));
			o.addProperty("name", f.getName(true));
			JsonArray ranges = new JsonArray();
			for (AddressRange r : f.getBody()) {
				JsonArray pair = new JsonArray();
				pair.add(hex(r.getMinAddress()));
				pair.add(hex(r.getMaxAddress()));
				ranges.add(pair);
			}
			o.add("ranges", ranges);
			funcs.add(o);
		}
		root.add("functions", funcs);

		JsonArray tables = new JsonArray();
		InstructionIterator it = currentProgram.getListing().getInstructions(true);
		while (it.hasNext() && !monitor.isCancelled()) {
			Instruction ins = it.next();
			FlowType ft = ins.getFlowType();
			if (!ft.isComputed() || !ft.isJump()) {
				continue;
			}
			TreeSet<String> targets = new TreeSet<>();
			for (Reference r : ins.getReferencesFrom()) {
				if (r.getReferenceType().isFlow()) {
					targets.add(hex(r.getToAddress()));
				}
			}
			JsonObject o = new JsonObject();
			o.addProperty("at", hex(ins.getAddress()));
			o.addProperty("mnemonic", ins.toString());
			JsonArray t = new JsonArray();
			targets.forEach(t::add);
			o.add("targets", t);
			tables.add(o);
		}
		root.add("jumptables", tables);

		// Code addresses stored in data (vtables, callback tables): every reference whose source is
		// in a non-executable block and whose target is the entry of a function.
		TreeSet<String> dataRefs = new TreeSet<>();
		for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
			if (b.isExecute()) {
				continue;
			}
			ReferenceIterator ri = currentProgram.getReferenceManager()
					.getReferenceIterator(b.getStart());
			while (ri.hasNext()) {
				Reference r = ri.next();
				if (!b.contains(r.getFromAddress())) {
					break;
				}
				if (getFunctionAt(r.getToAddress()) != null) {
					dataRefs.add(hex(r.getToAddress()));
				}
			}
		}
		JsonArray dr = new JsonArray();
		dataRefs.forEach(dr::add);
		root.add("datarefs", dr);

		try (Writer w = new BufferedWriter(
			new OutputStreamWriter(new FileOutputStream(out), StandardCharsets.UTF_8))) {
			new Gson().toJson(root, w);
		}
		println("functions=" + funcs.size() + " jumptables=" + tables.size() + " datarefs=" +
			dr.size() + " -> " + out);
	}

	private static String hex(Address a) {
		return String.format("%08x", a.getOffset());
	}
}
