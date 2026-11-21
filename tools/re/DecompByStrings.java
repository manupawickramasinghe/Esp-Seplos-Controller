// Decompile every function that (via an l32r literal-pool entry) references one of the given strings.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;

public class DecompByStrings extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String outPath = args[0];
        Memory mem = currentProgram.getMemory();
        FunctionManager fm = currentProgram.getFunctionManager();
        ReferenceManager rm = currentProgram.getReferenceManager();
        Map<Address, Set<String>> funcs = new TreeMap<>();
        PrintWriter log = new PrintWriter(new FileWriter(outPath + ".log"));
        for (int i = 1; i < args.length; i++) {
            byte[] needle = args[i].getBytes("UTF-8");
            Address a = mem.getMinAddress();
            while (a != null) {
                a = mem.findBytes(a, needle, null, true, monitor);
                if (a == null) break;
                // literal-pool words that hold this string address
                int v = (int) a.getOffset();
                byte[] le = new byte[]{(byte)v, (byte)(v>>8), (byte)(v>>16), (byte)(v>>24)};
                Address p = mem.getMinAddress();
                while (p != null) {
                    p = mem.findBytes(p, le, null, true, monitor);
                    if (p == null) break;
                    for (Reference r : rm.getReferencesTo(p)) {
                        Function f = fm.getFunctionContaining(r.getFromAddress());
                        if (f != null) funcs.computeIfAbsent(f.getEntryPoint(), k -> new TreeSet<>()).add(args[i]);
                        else log.println("ref w/o function " + r.getFromAddress() + " for " + args[i]);
                    }
                    Function f2 = fm.getFunctionContaining(p);
                    if (f2 != null) funcs.computeIfAbsent(f2.getEntryPoint(), k -> new TreeSet<>()).add(args[i]);
                    p = p.add(1);
                }
                log.println("string '" + args[i] + "' at " + a);
                a = a.add(1);
            }
        }
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        PrintWriter out = new PrintWriter(new FileWriter(outPath));
        for (Map.Entry<Address, Set<String>> e : funcs.entrySet()) {
            Function f = fm.getFunctionAt(e.getKey());
            out.println("/* ===== " + f.getName() + " @ " + e.getKey() + " size " + f.getBody().getNumAddresses() + " refs " + e.getValue() + " */");
            DecompileResults res = di.decompileFunction(f, 120, monitor);
            out.println(res.decompileCompleted() ? res.getDecompiledFunction().getC() : "/* decompile failed: " + res.getErrorMessage() + " */");
        }
        out.close(); log.close();
        println("functions: " + funcs.size() + " total in program: " + fm.getFunctionCount());
    }
}
