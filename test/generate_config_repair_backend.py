#!/usr/bin/env python3
"""Whole production run/console extraction. No transaction/Store/transport body replacement."""
from pathlib import Path
import argparse,hashlib,json,re

def main():
 p=argparse.ArgumentParser();p.add_argument('output',type=Path);a=p.parse_args()
 root=Path(__file__).resolve().parents[1]
 allowed={
 'config_repair.cpp':{'sdkconfig.h','config_repair.hpp','config_repair_store.hpp','ota_update.hpp','esp_random.h','esp_timer.h','freertos/FreeRTOS.h','freertos/task.h'},
 'config_repair_console.cpp':{'sdkconfig.h','config_repair.hpp','algorithm','esp_timer.h','freertos/FreeRTOS.h','freertos/task.h','driver/uart.h','hal/uart_ll.h','driver/usb_serial_jtag.h'}}
 bodies=[];receipt=[]
 for name,headers in allowed.items():
  source=(root/'main'/name).read_text()
  found=re.findall(r'^#include [<"]([^>"\n]+)[>"]\n',source,re.M)
  if set(found)!=headers or len(found)!=len(headers):raise ValueError('actual include inventory changed: '+name)
  body=re.sub(r'^#include [<"][^>"\n]+[>"]\n','',source,flags=re.M)
  # Includes alone are removed. Outer role/config guards, anonymous namespaces, complete
  # run/finish/report/park, RawConsole and console_entry remain in actual lexical order.
  if body.count('namespace tk::repair {')!=1 or not body.rstrip().endswith('#endif'):
   raise ValueError('complete actual source boundary changed')
  bodies.append('// Actual '+name+'; include directives supplied by passive fixture.\n'+body)
  receipt.append({'file':'main/'+name,'sourceSha256':hashlib.sha256(source.encode()).hexdigest(),
                  'bodySha256':hashlib.sha256(body.encode()).hexdigest(),'removedIncludes':found})
 a.output.write_text('\n'.join(bodies))
 a.output.with_suffix('.json').write_text(json.dumps({'scope':'whole source minus exact includes; no SDK ABI proof','files':receipt},indent=2)+'\n')
if __name__=='__main__':main()
