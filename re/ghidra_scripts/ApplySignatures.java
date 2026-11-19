// Names functions and (optionally) sets their signatures from a JSONL file.
// Each line: {"addr": "08b152f0", "name": "strcmp", "ret": "i", "args": "ss"}
// ret/args use PPSSPP's type letters: v void, i int, x uint, s char*, p void*, f float,
// I int64, X uint64; anything else becomes uint. Missing ret/args = rename only.
// Usage (headless): -postScript ApplySignatures.java /path/to/known.jsonl
//@category PSP port

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

import com.google.gson.JsonObject;
import com.google.gson.JsonParser;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.CharDataType;
import ghidra.program.model.data.DataType;
import ghidra.program.model.data.FloatDataType;
import ghidra.program.model.data.IntegerDataType;
import ghidra.program.model.data.LongLongDataType;
import ghidra.program.model.data.PointerDataType;
import ghidra.program.model.data.UnsignedIntegerDataType;
import ghidra.program.model.data.UnsignedLongLongDataType;
import ghidra.program.model.data.VoidDataType;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Function.FunctionUpdateType;
import ghidra.program.model.listing.Parameter;
import ghidra.program.model.listing.ParameterImpl;
import ghidra.program.model.listing.ReturnParameterImpl;
import ghidra.program.model.symbol.SourceType;

public class ApplySignatures extends GhidraScript {

	private DataType type(char c) {
		switch (c) {
			case 'v': return VoidDataType.dataType;
			case 'i': return IntegerDataType.dataType;
			case 's': return new PointerDataType(CharDataType.dataType);
			case 'p': return PointerDataType.dataType;
			case 'f': return FloatDataType.dataType;
			case 'I': return LongLongDataType.dataType;
			case 'X': return UnsignedLongLongDataType.dataType;
			default: return UnsignedIntegerDataType.dataType;
		}
	}

	@Override
	public void run() throws Exception {
		String[] args = getScriptArgs();
		File in = args.length > 0 ? new File(args[0]) : askFile("Known functions JSONL", "Apply");
		int named = 0, typed = 0, missing = 0, errors = 0;

		try (BufferedReader r = new BufferedReader(
			new InputStreamReader(new FileInputStream(in), StandardCharsets.UTF_8))) {
			String line;
			while ((line = r.readLine()) != null) {
				if (line.isBlank()) {
					continue;
				}
				JsonObject o = JsonParser.parseString(line).getAsJsonObject();
				Function f = getFunctionAt(toAddr(o.get("addr").getAsString()));
				if (f == null) {
					f = createFunction(toAddr(o.get("addr").getAsString()), null);
				}
				if (f == null) {
					missing++;
					continue;
				}
				try {
					f.setName(o.get("name").getAsString(), SourceType.IMPORTED);
					named++;
					if (o.has("ret") && !o.get("ret").isJsonNull() && o.has("args") &&
						!o.get("args").isJsonNull()) {
						String ret = o.get("ret").getAsString();
						String argSpec = o.get("args").getAsString();
						List<Parameter> params = new ArrayList<>();
						for (int i = 0; i < argSpec.length(); i++) {
							params.add(new ParameterImpl("a" + i, type(argSpec.charAt(i)), currentProgram));
						}
						f.updateFunction(null,
							new ReturnParameterImpl(type(ret.isEmpty() ? 'x' : ret.charAt(0)), currentProgram),
							params, FunctionUpdateType.DYNAMIC_STORAGE_ALL_PARAMS, true, SourceType.IMPORTED);
						typed++;
					}
				}
				catch (Exception e) {
					errors++;
					printerr(f.getEntryPoint() + " " + o.get("name") + ": " + e.getMessage());
				}
			}
		}
		println("Named " + named + ", typed " + typed + ", missing " + missing + ", errors " + errors);
	}
}
