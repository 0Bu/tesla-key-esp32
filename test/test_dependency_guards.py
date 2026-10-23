#!/usr/bin/env python3
"""Native guards for the actual patched decrypt body and upstream logging macros.

Synthetic PSA failure drivers prove output cleanup, not AES or ESP driver behavior.
The real-library V1 harness separately owns cryptographic known answers and key compatibility.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent

PSA_FACADE = r'''
#include <array>
#include <cstdint>
#include <cstring>
#include <cstdlib>
using pb_byte_t=unsigned char;
using psa_status_t=int; using mbedtls_svc_key_id_t=int; struct psa_aead_operation_t{};
constexpr int PSA_SUCCESS=0, PSA_ALG_GCM=1, PSA_KEY_USAGE_DECRYPT=1;
constexpr int TeslaBLE_Status_E_ERROR_INVALID_PARAMS=-1, TeslaBLE_Status_E_ERROR_PRIVATE_KEY_NOT_INITIALIZED=-2, TeslaBLE_Status_E_ERROR_DECRYPT=-3, TeslaBLE_Status_E_OK=0;
#define MBEDTLS_SVC_KEY_ID_INIT 0
#define PSA_AEAD_OPERATION_INIT {}
#define LOG_ERROR(...) ((void)0)
static int mode, aborted, destroyed;
static psa_status_t hash_ad_buffer(const pb_byte_t*,size_t,unsigned char* out){std::memset(out,0,32);return 0;}
static psa_status_t import_session_key(const unsigned char*,int,int* key){*key=1;return 0;}
static int psa_aead_decrypt_setup(psa_aead_operation_t*,int,int){return mode==3?-9:0;}
static int psa_aead_set_nonce(psa_aead_operation_t*,const unsigned char*,size_t){return mode==4?-10:0;}
static int psa_aead_update_ad(psa_aead_operation_t*,const unsigned char*,size_t){return mode==5?-11:0;}
static int psa_aead_update(psa_aead_operation_t*,const unsigned char*,size_t,unsigned char* out,size_t,size_t* size){std::memset(out,0x42,4);*size=mode==1?0:4;return mode==1?-7:0;}
static int psa_aead_verify(psa_aead_operation_t*,unsigned char* out,size_t,size_t* size,const unsigned char*,size_t){std::memset(out,0x43,2);*size=mode==2?0:2;return mode==2?-8:0;}
static int psa_aead_abort(psa_aead_operation_t*){++aborted;return 0;}
static int psa_destroy_key(int){++destroyed;return 0;}
[[maybe_unused]] static void mbedtls_platform_zeroize(void* out,size_t size){std::memset(out,0,size);}
struct Peer {
 std::array<unsigned char,16> shared_secret_sha1_{};
 bool is_private_key_initialized() const{return true;}
 int construct_response_ad_buffer(uint32_t,pb_byte_t* out,size_t* len,uint32_t,const pb_byte_t*,size_t,uint32_t) const{out[0]=0;*len=1;return 0;}
 int decrypt_response(const pb_byte_t*,size_t,const pb_byte_t*,const pb_byte_t*,const pb_byte_t*,size_t,uint32_t,uint32_t,uint32_t,pb_byte_t*,size_t,size_t*) const;
};
'''

DECRYPT_MAIN = r'''
int main(int argc,char** argv) {
 if(argc!=2)return 2;
 mode=std::atoi(argv[1]); Peer peer;
 unsigned char in[8]{},nonce[12]{},tag[16]{};
 std::array<unsigned char,18> guarded; guarded.fill(0xa5);
 auto* out=guarded.data()+1; size_t len=999;
 int result=peer.decrypt_response(in,sizeof in,nonce,tag,nullptr,0,0,0,1,out,16,&len);
 if(guarded.front()!=0xa5||guarded.back()!=0xa5)return 9;
 if(aborted!=1||destroyed!=1)return 3;
 if(mode) {
  if(result!=TeslaBLE_Status_E_ERROR_DECRYPT||len!=999)return 4;
  for(unsigned i=0;i<16;++i)if(out[i]!=0)return 1;
  return 0;
 }
 if(result!=0||len!=6)return 5;
 for(unsigned i=0;i<4;++i)if(out[i]!=0x42)return 6;
 for(unsigned i=4;i<6;++i)if(out[i]!=0x43)return 7;
 for(unsigned i=6;i<16;++i)if(out[i]!=0xa5)return 8;
 return 0;
}
'''


LOG_MAIN = r"""
#include <cstdio>
#include <cstdarg>
static unsigned formatting_calls,argument_evaluations;
static int counted_vsnprintf(char* out,size_t size,const char* fmt,va_list args) {
 ++formatting_calls; return std::vsnprintf(out,size,fmt,args);
}
#define vsnprintf counted_vsnprintf
#include "logging_bridge.cpp"
#undef vsnprintf
static int argument(){++argument_evaluations;return 7;}
int main() {
 LOG_DEBUG("synthetic session canary %d",argument());
 LOG_VERBOSE("synthetic session canary %d",argument());
 if(formatting_calls||argument_evaluations)return 1;
 LOG_INFO("ordinary info canary");
 LOG_WARNING("ordinary warning canary");
 LOG_ERROR("ordinary error canary");
 return formatting_calls==3?0:2;
}
"""


def run_probe(work: Path, label: str, code: str, flags: list[str], compiler: str,
              cases: list[tuple[list[str], int]]) -> None:
    source, binary = work / (label + ".cpp"), work / label
    source.write_text(code)
    subprocess.run([compiler, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror",
                    *flags, str(source), "-o", str(binary)], check=True, timeout=30)
    for arguments, expected in cases:
        result = subprocess.run([str(binary), *arguments], capture_output=True,
                                text=True, timeout=10)
        # A sanitizer crash never counts as an expected negative-control result.
        if result.returncode != expected:
            raise AssertionError((label, arguments, result.returncode, expected,
                                  result.stdout, result.stderr))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--component", type=Path, required=True)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    component = args.component.resolve(strict=True)
    manifest = json.loads((ROOT / "scripts/tesla-ble-patch-state.json").read_text())
    for row in manifest["files"]:
        assert hashlib.sha256((component / row["path"]).read_bytes()).hexdigest() == row["afterSha256"], row["path"]
    # Unmodified v6.2.0 inputs used by the formatting probe, outside the patch touched-file set.
    for relative, digest in {
        "include/defs.h": "cdfd3b7f8cde260251dc0b4002827e60d668cc6671d1d27451797963a4d47361",
        "src/logging_bridge.cpp": "1b1a50903727c016f2a427caab5c6a935ef12dfedc4b3e9f7c7d70efc1bab012",
    }.items():
        assert hashlib.sha256((component / relative).read_bytes()).hexdigest() == digest, relative
    source = (component / "src/peer.cpp").read_text()
    start, end = "int Peer::decrypt_response(", "\nint Peer::encrypt("
    assert source.count(start) == source.count(end) == 1
    function = start + source.split(start, 1)[1].split(end, 1)[0]
    wipe = "mbedtls_platform_zeroize(output_buffer, output_buffer_length);"
    assert function.count(wipe) == 1
    compiler = os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++")
    assert compiler, "native dependency guards require a C++20 compiler"
    flags = (["-g", "-fno-omit-frame-pointer", "-fno-sanitize-recover=all",
              "-fsanitize=address,undefined,leak"] if args.sanitize else [])
    with tempfile.TemporaryDirectory(prefix="tesla-dependency-guards-") as directory:
        work = Path(directory)
        run_probe(work, "decrypt", PSA_FACADE + function + DECRYPT_MAIN, flags, compiler,
                  [([str(mode)], 0) for mode in range(6)])
        for label, replacement in (
            ("reported-length-wipe", "mbedtls_platform_zeroize(output_buffer, update_length);"),
            ("removed-wipe", "(void)output_buffer;"),
        ):
            run_probe(work, label, PSA_FACADE + function.replace(wipe, replacement) + DECRYPT_MAIN,
                      flags, compiler, [(["0"], 0), (["1"], 1), (["2"], 1)])
        includes = ["-I", str(component / "include"), "-I", str(component / "src")]
        for label, defines, expected in (
            ("info", ["-DTESLA_BLE_LOG_LEVEL=2"], 0),
            ("upstream-default", [], 1),
            ("debug-enabled", ["-DTESLA_BLE_LOG_LEVEL=3"], 1),
        ):
            run_probe(work, label, LOG_MAIN, flags + includes + defines,
                      compiler, [([], expected)])
    print("dependency guards: PASS (12 actual-decrypt runs, 3 logging controls; synthetic drivers)")


if __name__ == "__main__":
    main()
