import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import java.io.*;
public class DumpAll extends GhidraScript {
  public void run() throws Exception{
    String out=getScriptArgs()[0];
    DecompInterface d=new DecompInterface(); d.openProgram(currentProgram);
    PrintWriter w=new PrintWriter(new FileWriter(out));
    int n=0;
    for(Function f: currentProgram.getFunctionManager().getFunctions(true)){
      DecompileResults r=d.decompileFunction(f,60,monitor);
      w.println("// ==== "+f.getName()+" @ "+f.getEntryPoint()+" size "+f.getBody().getNumAddresses());
      if(r!=null&&r.decompileCompleted()) w.println(r.getDecompiledFunction().getC()); else w.println("// FAILED");
      n++;
    }
    w.close(); println("dumped "+n);
  }
}
