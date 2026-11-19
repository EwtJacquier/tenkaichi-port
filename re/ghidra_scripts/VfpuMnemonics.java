// Counts the VFPU instructions in the program by mnemonic (and one address of each), so the runtime's
// VFPU (runtime/vfpu.c) can be checked for what this game needs. Usage: -postScript VfpuMnemonics.java out.txt
//@category PSP port
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import java.io.*;
import java.util.*;

public class VfpuMnemonics extends GhidraScript {
	@Override
	public void run() throws Exception {
		TreeMap<String, Integer> count = new TreeMap<>();
		TreeMap<String, String> where = new TreeMap<>();
		InstructionIterator it = currentProgram.getListing().getInstructions(true);
		while (it.hasNext()) {
			Instruction in = it.next();
			int op = in.getInt(0);
			int major = (op >>> 26) & 63;
			boolean vfpu = major == 0x18 || major == 0x19 || major == 0x1B || major == 0x34 || major == 0x37 || major == 0x3C ||
			               major == 0x3F || major == 0x32 || major == 0x36 || major == 0x3A || major == 0x3E || major == 0x35 ||
			               major == 0x3D || (major == 0x12 && ((op >>> 21) & 31) != 0x08 && ((op >>> 21) & 31) != 0x09);
			if (!vfpu) continue;
			String m = in.getMnemonicString();
			count.merge(m, 1, Integer::sum);
			where.putIfAbsent(m, in.getAddress().toString() + " " + Integer.toHexString(op));
		}
		try (PrintWriter w = new PrintWriter(new FileWriter(getScriptArgs()[0]))) {
			for (String m : count.keySet()) w.println(m + " " + count.get(m) + " " + where.get(m));
		}
		println("VfpuMnemonics: " + count.size() + " mnemonics");
	}
}
