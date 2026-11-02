import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
public class PtrFuncs extends GhidraScript {
  public void run() throws Exception{
    long lo=0x08010188L, hi=0x08048000L; int made=0;
    for(int pass=0;pass<3;pass++){
    for(long a=0x08010000L;a<0x0809c500L;a+=4){
      long v=getInt(toAddr(a))&0xffffffffL;
      if((v&1)==0||v<lo||v>=hi) continue;
      Address f=toAddr(v&~1L);
      if(getFunctionContaining(f)!=null && getFunctionAt(f)==null){ /* inside other func: split */ }
      if(getFunctionAt(f)!=null) continue;
      Instruction ins=getInstructionAt(f);
      if(ins==null){
        if(getDataAt(f)!=null && getDataAt(f).isDefined()) continue;
        currentProgram.getProgramContext().setValue(currentProgram.getProgramContext().getRegister("TMode"),f,f,java.math.BigInteger.ONE);
        disassemble(f);
      }
      if(getInstructionAt(f)!=null){
        Function old=getFunctionContaining(f);
        if(old!=null && !old.getEntryPoint().equals(f)){
          // shrink: let analysis split
          removeFunction(old); createFunction(old.getEntryPoint(),null);
        }
        if(createFunction(f,null)!=null) made++;
      }
    }}
    println("created "+made);
  }
}
