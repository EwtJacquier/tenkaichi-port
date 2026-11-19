// Applies names/comments from a JSONL file ({"addr","name","summary","confidence"} per line).
// Names like "CFxBox::LoadSequence" are placed in the CFxBox namespace.
// Only functions that still carry a default name (FUN_xxxxxxxx) are renamed, unless "force" is passed.
// Usage (headless): -postScript ApplyNames.java /path/to/names.jsonl [minConfidence] [force]
//@category PSP port

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;

import com.google.gson.JsonObject;
import com.google.gson.JsonParser;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Namespace;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.SymbolTable;

public class ApplyNames extends GhidraScript {

	@Override
	public void run() throws Exception {
		String[] args = getScriptArgs();
		File in = args.length > 0 ? new File(args[0]) : askFile("Names JSONL", "Apply");
		double minConf = args.length > 1 ? Double.parseDouble(args[1]) : 0.5;
		boolean force = args.length > 2 && args[2].equals("force");

		SymbolTable st = currentProgram.getSymbolTable();
		Namespace global = currentProgram.getGlobalNamespace();
		int applied = 0, skipped = 0, errors = 0;

		try (BufferedReader r = new BufferedReader(
			new InputStreamReader(new FileInputStream(in), StandardCharsets.UTF_8))) {
			String line;
			while ((line = r.readLine()) != null) {
				line = line.trim();
				if (line.isEmpty()) {
					continue;
				}
				JsonObject o = JsonParser.parseString(line).getAsJsonObject();
				if (!o.has("name") || o.get("name").isJsonNull()) {
					skipped++;
					continue;
				}
				double conf = o.has("confidence") && !o.get("confidence").isJsonNull()
						? o.get("confidence").getAsDouble()
						: 0;
				if (conf < minConf) {
					skipped++;
					continue;
				}
				Address addr = toAddr(o.get("addr").getAsString());
				Function f = getFunctionAt(addr);
				if (f == null || (!force && !f.getName().startsWith("FUN_"))) {
					skipped++;
					continue;
				}
				String full = o.get("name").getAsString();
				try {
					Namespace ns = global;
					String name = full;
					int sep = full.lastIndexOf("::");
					if (sep > 0) {
						ns = st.getOrCreateNameSpace(global, full.substring(0, sep), SourceType.USER_DEFINED);
						name = full.substring(sep + 2);
					}
					f.setParentNamespace(ns);
					f.setName(name, SourceType.USER_DEFINED);
					if (o.has("summary") && !o.get("summary").isJsonNull()) {
						f.setComment(String.format("[AI %.2f] %s", conf, o.get("summary").getAsString()));
					}
					applied++;
				}
				catch (Exception e) {
					errors++;
					printerr(addr + " " + full + ": " + e.getMessage());
				}
			}
		}
		println("Applied " + applied + ", skipped " + skipped + ", errors " + errors);
	}
}
