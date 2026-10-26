#!/usr/bin/env python3
"""Proposed reviewed actual-maintenance host/source closure, not a review certificate."""
from pathlib import Path
import hashlib
ROOT=Path(__file__).resolve().parents[1]
PROGRAMS={'main/config_repair.cpp': '6f4696daf2b02353d4ff45a24ac0cee8dd9254880afbaccc44fa46c865c206f7', 'main/config_repair_console.cpp': '17447049158c527c88c1a075a095302089c0589e51a5a5309b103863902d9b0e', 'main/config_repair_store.hpp': '287144d0fb35b6f1ed449b248fc6cccf7c76f528c6cf976731aa8c977b10dba9', 'main/logic/config_repair.hpp': 'eb1db2ab7aec9763db4f76272bc174d67df53a7f845371f01f25e7912e5ec0e8', 'main/logic/nvs_contract.hpp': 'af21cca072f70db29d2718e4cacb1a4dfbdff2566e850be0112e1110efb74560'}
def check():
 for name,digest in PROGRAMS.items():
  if hashlib.sha256((ROOT/name).read_bytes()).hexdigest()!=digest:
   raise AssertionError('maintenance complete source needs independent review: '+name)
 fixture=(ROOT/'test/test_config_repair_backend.cpp').read_text()
 for token in ('#include "config_repair_store.hpp"','#include "config_repair_backend.generated.hpp"','CHECK(fixture::owner_held&&fixture::owner_sealed&&fixture::seal_calls==1);'):
  if token not in fixture:raise AssertionError('actual backend fixture missing '+token)
 cmake=(ROOT/'test/config_repair_backend.cmake').read_text()
 for token in ('foreach(repair_target IN ITEMS esp32 esp32s3 esp32c3 esp32c6)','test_config_repair_backend.cpp ${repair_generated}','CONFIG_IDF_TARGET_${repair_upper}=1'):
  if token not in cmake:raise AssertionError('actual backend source graph missing '+token)
 shell=(ROOT/'test/run-config-repair-backend.sh').read_text()
 for token in ('test/generate_config_repair_backend.py','for target in esp32 esp32s3 esp32c3 esp32c6; do','CONFIG_REPAIR_SANITIZE','"$work/esp32s3" "$mode"'):
  if token not in shell:raise AssertionError('actual backend fallback graph missing '+token)
if __name__=='__main__':check();print('actual maintenance backend source/graph contract: PASS')
