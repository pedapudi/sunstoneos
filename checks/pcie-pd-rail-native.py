"""Changed native rail reservation/snapshot request instructions only."""
import importlib.util,json,struct
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn.arm64_const import UC_ARM64_REG_X0

def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m

def setup(image,out,source):
 old=module('pcie_pd_native_base',source/'checks/pcie-application-native.py');elf=old.Image(image)
 assert 'apbs0' in elf.layouts['a733_pmic']
 for name in ('read_register','write_register','admitted'):elf.address(name,'/apps/pcie/native.c')
 for name in ('a733_rail_acquire','a733_rail_status','a733_pmic_live','a733_pmic_submit','pcie_construct','pcie_session_valid'):elf.address(name)
 save={'layouts':elf.layouts,'sizes':elf.sizes,'current_compilation_unit':'/drivers/pci/a733_pcie_native.c','actual_image':str(image)}
 (out/'pd-native-preflight.json').write_text(json.dumps(save,indent=2)+'\n');return old,elf

def run(image,out,source,session):
 old,elf=setup(image,out,source)
 class Machine(old.Machine):
  def __init__(self):
   self.requests=[];super().__init__(elf,session,'pd-rail')
   self.hooks.pop(elf.address('a733_pcie_poll'))
  def code(self,cpu,address,size,unused):
   args=lambda n:cpu.reg_read(UC_ARM64_REG_X0+n)
   if address==elf.address('a733_rail_acquire'):
    assert args(0)==self.objects['rails'] and args(1)==23 and args(2)==((1<<1)|(1<<15)) and args(3)==0
    self.requests.append({'operation':'rail-acquire','shared':args(2),'exclusive':args(3)});self.returned(1);return
   if address==elf.address('a733_rail_status'):
    assert args(0)==self.objects['rails'];self.returned(0);return
   if address==elf.address('a733_pmic_live'):
    assert args(0)==self.objects['pmic'];self.returned(1);return
   if address==elf.address('a733_pmic_submit'):
    assert args(0)==self.objects['pmic'] and args(1)==23 and args(3)==15
    self.requests.append({'operation':'pmic-submit','rail':args(3)});self.returned(0);return
   super().code(cpu,address,size,unused)
 m=Machine();assert m.call('pcie_session_valid',0x41f00000)==1
 assert m.call('pcie_construct')==1
 assert m.call('admitted',m.base,m.port+elf.layouts['a733_pcie']['config'],unit='/apps/pcie/native.c')==1
 phase=m.port+elf.layouts['a733_pcie']['phase'];index=m.port+elf.layouts['a733_pcie']['supply_index']
 m.cpu.mem_write(phase,struct.pack('<I',1));assert m.call('a733_pcie_poll',m.port)==0
 m.cpu.mem_write(phase,struct.pack('<I',5));m.cpu.mem_write(index,struct.pack('<I',1));assert m.call('a733_pcie_poll',m.port)==0
 assert len(m.requests)==2
 (out/'pd-native-result.json').write_text(json.dumps({'passed':True,'requests':m.requests,'scope':'Actual final native session/config/init/admission and changed rail/PMIC request instructions; ready metadata phases/lower returns modeled, no hardware startup replay.'},indent=2)+'\n')
