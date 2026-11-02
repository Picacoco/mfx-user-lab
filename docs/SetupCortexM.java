// Seed Cortex-M firmware: memory blocks + vector table functions
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.data.*;
public class SetupCortexM extends GhidraScript {
  void blk(String n,long a,long len,boolean x) throws Exception{
    Memory m=currentProgram.getMemory();
    Address s=toAddr(a);
    if(m.getBlock(s)!=null) return;
    MemoryBlock b=m.createUninitializedBlock(n,s,len,false);
    b.setRead(true);b.setWrite(true);b.setExecute(x);
    if(a>=0x40000000L && a<0x60000000L) b.setVolatile(true);
  }
  public void run() throws Exception{
    blk("SRAM",0x20000000L,0x20000,false);
    blk("CCM",0x10000000L,0x10000,false);
    blk("PERIPH",0x40000000L,0x10000000L,false);
    blk("FSMC",0x60000000L,0x10000000L,false);
    blk("SCS",0xE0000000L,0x100000L,false);
    long base=0x08010000L;
    for(int i=0;i<98;i++){
      Address a=toAddr(base+4*i);
      createDWord(a);
      long v=getInt(a)&0xffffffffL;
      if(i==0||v==0) continue;
      if((v&1)==1){
        Address f=toAddr(v&~1L);
        currentProgram.getProgramContext().setValue(currentProgram.getProgramContext().getRegister("TMode"),f,f,java.math.BigInteger.ONE);
        disassemble(f);
        if(getFunctionAt(f)==null) createFunction(f,i==1?"Reset_Handler":"vec_"+i);
        createMemoryReference(getDataAt(a),f,RefType.DATA);
      }
    }
    currentProgram.getSymbolTable().createLabel(toAddr(base),"VectorTable",SourceType.USER_DEFINED);
  }
}
