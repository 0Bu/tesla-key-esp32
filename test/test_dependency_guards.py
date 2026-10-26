#!/usr/bin/env python3
"""Native guards for patched decrypt/SessionInfo selection and upstream logging macros.

Synthetic drivers prove output cleanup and secret selection, not AES/HMAC or ESP driver behavior.
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



# This facade observes secret selection/delegation only. It never creates a real HMAC or
# routable message; cryptographic correctness belongs to the separate unchanged V1 vectors.
SESSION_FACADE = r"""
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
using pb_byte_t=unsigned char;
enum UniversalMessage_Domain { DOMAIN_NONE=0, DOMAIN_SECURITY=2, DOMAIN_INFO=3 };
constexpr int TeslaBLE_Status_E_OK=0;
constexpr unsigned char Signatures_Tag_TAG_SIGNATURE_TYPE=0, Signatures_SignatureType_SIGNATURE_TYPE_HMAC=6,
 Signatures_Tag_TAG_PERSONALIZATION=2, Signatures_Tag_TAG_CHALLENGE=3, Signatures_Tag_TAG_END=255;
struct Signatures_SessionInfo { struct { size_t size=65; pb_byte_t bytes[65]{}; } publicKey; };
struct UniversalMessage_RoutableMessage_session_info_t { size_t size=1; pb_byte_t bytes[128]{}; };
static unsigned ecdh_calls,derive_calls,hmac_calls; static bool ecdh_fail;
static std::array<uint8_t,16> selected_key{};
template<class... Args> static void passive_log(Args&&...) {}
#define LOG_ERROR(...) passive_log(__VA_ARGS__)
struct Peer {
 static constexpr size_t SHARED_KEY_SIZE_BYTES=16;
 bool retained=false,valid=true,matching=true; std::array<uint8_t,16> secret{};
 bool has_shared_secret() const{return retained;}
 [[maybe_unused]] bool is_initialized() const{return retained&&valid;}
 bool accepts_session_key(const uint8_t*,size_t) const{return matching;}
 const uint8_t* get_shared_secret() const{return secret.data();}
};
struct CryptoContext {
 int perform_tesla_ecdh(const uint8_t*,size_t,uint8_t* out){++ecdh_calls;std::memset(out,0x22,16);return ecdh_fail?-1:0;}
};
struct CryptoUtils {
 static void clear_sensitive_memory(void* out,size_t n){std::memset(out,0,n);}
 static int derive_session_info_key(const uint8_t* key,size_t n,uint8_t* out,size_t len){
  ++derive_calls;if(n!=selected_key.size())return -1;std::memcpy(selected_key.data(),key,n);std::memset(out,0x33,len);return 0;
 }
 static int hmac_sha256(const uint8_t*,size_t,const uint8_t*,size_t,uint8_t* out,size_t len){++hmac_calls;std::memset(out,0x44,len);return 0;}
 static bool secure_memory_compare(const void* a,const void* b,size_t n){return std::memcmp(a,b,n)==0;}
};
struct Client {
 CryptoContext crypto_context_; std::string vin_{"SYNTHETICVIN00000"}; Peer security,info;
 Peer* get_peer(UniversalMessage_Domain domain){return domain==DOMAIN_SECURITY?&security:domain==DOMAIN_INFO?&info:nullptr;}
 bool verify_session_info_tag(UniversalMessage_Domain,const Signatures_SessionInfo&,const UniversalMessage_RoutableMessage_session_info_t&,const pb_byte_t*,size_t,const pb_byte_t*,size_t);
};
"""
SESSION_MAIN = r"""
int main(int argc,char** argv) {
 if(argc!=2)return 2;
 const int mode=std::atoi(argv[1]);Client client;
 client.security.secret.fill(0x11);client.info.secret.fill(0x55);
 if(mode!=0&&mode!=7)client.security.retained=true;
 if(mode==2)client.security.valid=false;
 if(mode==3)client.security.matching=false;
 if(mode==5)client.info.retained=true;
 if(mode==6||mode==7)ecdh_fail=true;
 Signatures_SessionInfo value;value.publicKey.bytes[0]=4;
 UniversalMessage_RoutableMessage_session_info_t body;
 if(mode==8)body.size=sizeof(body.bytes)+1;
 if(mode==9)body.size=0;
 const uint8_t uuid[16]{};uint8_t tag[32];std::memset(tag,0x44,sizeof(tag));
 const auto domain=mode==4?DOMAIN_NONE:mode==5?DOMAIN_INFO:DOMAIN_SECURITY;
 const bool accepted=client.verify_session_info_tag(domain,value,body,uuid,sizeof(uuid),tag,sizeof(tag));
 if(mode==3||mode==4||mode==8||mode==9)return !accepted&&!ecdh_calls&&!derive_calls&&!hmac_calls?0:1;
 if(mode==7)return !accepted&&ecdh_calls==1&&!derive_calls&&!hmac_calls?0:1;
 const uint8_t expected=mode==0?0x22:mode==5?0x55:0x11;
 if(!accepted||ecdh_calls!=(mode==0?1u:0u)||derive_calls!=1||hmac_calls!=1)return 1;
 for(auto byte:selected_key)if(byte!=expected)return 1;
 return 0;
}
"""


def check_session_secret_selection(component: Path, work: Path, flags: list[str], compiler: str) -> None:
    source = (component / "src/client.cpp").read_text()
    start, end = "bool Client::verify_session_info_tag(", "\nint Client::build_universal_message_with_payload("
    assert source.count(start) == source.count(end) == 1
    method = start + source.split(start, 1)[1].split(end, 1)[0]
    run_probe(work, "session-selection", SESSION_FACADE + method + SESSION_MAIN,
              flags, compiler, [([str(mode)], 0) for mode in range(10)])
    controls = (
        ("session-empty-payload", "session_info_length == 0 || ", "", [(9, 1)]),
        ("session-rederive", "if (peer->has_shared_secret())", "if (false)", [(1, 1), (6, 1)]),
        ("session-key-source", "peer->get_shared_secret()", "session_info.publicKey.bytes", [(1, 1)]),
        ("session-invalidity-bypass", "if (peer->has_shared_secret())", "if (peer->is_initialized())", [(2, 1)]),
        ("session-domain", "get_peer(domain)", "get_peer((static_cast<void>(domain), DOMAIN_SECURITY))", [(5, 1), (4, 1)]),
        ("session-key-match", "!peer || !peer->accepts_session_key(session_info.publicKey.bytes, session_info.publicKey.size)", "!peer", [(3, 1)]),
    )
    for label, old, new, cases in controls:
        assert method.count(old) == 1, label
        run_probe(work, label, SESSION_FACADE + method.replace(old, new) + SESSION_MAIN,
                  flags, compiler, [(["0"], 0)] + [([str(mode)], result) for mode, result in cases])



# Logging-order driver: every backing buffer stays65 bytes, even when its reported
# size is short. A removed/moved guard fails by counters, never by an invalid read.
SESSION_LOAD_FACADE = r"""
#include <array>
#include <cstdint>
#include <cstring>
#include <cstdlib>
using pb_byte_t=unsigned char;
constexpr int TeslaBLE_Status_E_OK=0,TeslaBLE_Status_E_ERROR_INVALID_PARAMS=-1;
static unsigned verbose_calls,ecdh_calls;
template<class... Args> static void passive_log(Args&&...) {}
#define LOG_ERROR(...) passive_log(__VA_ARGS__)
#define LOG_DEBUG(...) passive_log(__VA_ARGS__)
#define LOG_VERBOSE(...) (++verbose_calls,passive_log(__VA_ARGS__))
struct CryptoContext {
 int perform_tesla_ecdh(const uint8_t*,size_t,uint8_t* out){++ecdh_calls;std::memset(out,0x31,16);return 0;}
};
struct Peer {
 CryptoContext context;CryptoContext* crypto_context_=&context;
 bool has_shared_secret_=false,is_valid_=false;
 std::array<uint8_t,65> tesla_public_key_{};std::array<uint8_t,16> shared_secret_sha1_{};
 bool accepts_session_key(const uint8_t*,size_t) const;
 int load_tesla_key(const uint8_t*,size_t);
};
"""
SESSION_LOAD_MAIN = r"""
int main(int argc,char** argv){
 if(argc!=2)return 2;
 const int mode=std::atoi(argv[1]);Peer peer;
 std::array<uint8_t,65> key{};key[0]=4;
 if(mode==2)key[0]=2;
 if(mode==4){peer.has_shared_secret_=true;peer.is_valid_=true;peer.tesla_public_key_.fill(0x71);peer.shared_secret_sha1_.fill(0x72);}
 const auto old_public=peer.tesla_public_key_;const auto old_secret=peer.shared_secret_sha1_;
 const auto result=peer.load_tesla_key(mode==3?nullptr:key.data(),mode==1?1:key.size());
 if(mode==0)return result==0&&verbose_calls==1&&ecdh_calls==1&&peer.has_shared_secret_&&peer.is_valid_?0:1;
 return result==TeslaBLE_Status_E_ERROR_INVALID_PARAMS&&!verbose_calls&&!ecdh_calls&&
  peer.tesla_public_key_==old_public&&peer.shared_secret_sha1_==old_secret&&
  peer.has_shared_secret_==(mode==4)&&peer.is_valid_==(mode==4)?0:1;
}
"""


def check_session_load_order(component: Path, work: Path, flags: list[str], compiler: str) -> None:
    source = (component / "src/peer.cpp").read_text()
    start, end = "bool Peer::accepts_session_key(", "\nint Peer::update_session("
    assert source.count(start) == source.count(end) == 1
    method = start + source.split(start, 1)[1].split(end, 1)[0]
    run_probe(work, "session-pre-log", SESSION_LOAD_FACADE + method + SESSION_LOAD_MAIN,
              flags, compiler, [([str(mode)], 0) for mode in range(5)])
    guard = """  // Existing trust material may only be refreshed with the same vehicle public key.
  if (!accepts_session_key(public_key_buffer, public_key_size)) {
    LOG_ERROR("Session public key rejected");
    return TeslaBLE_Status_E_ERROR_INVALID_PARAMS;
  }

"""
    assert method.count(guard) == 1
    marker = "  // Use CryptoContext's Tesla ECDH method"
    assert method.count(marker) == 1
    late = method.replace(guard, "").replace(marker, guard + marker)
    run_probe(work, "session-late-log-guard", SESSION_LOAD_FACADE + late + SESSION_LOAD_MAIN,
              flags, compiler, [(["0"], 0), (["1"], 1), (["2"], 1), (["4"], 1)])


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
        check_session_secret_selection(component, work, flags, compiler)
        check_session_load_order(component, work, flags, compiler)
        includes = ["-I", str(component / "include"), "-I", str(component / "src")]
        for label, defines, expected in (
            ("info", ["-DTESLA_BLE_LOG_LEVEL=2"], 0),
            ("upstream-default", [], 1),
            ("debug-enabled", ["-DTESLA_BLE_LOG_LEVEL=3"], 1),
        ):
            run_probe(work, label, LOG_MAIN, flags + includes + defines,
                      compiler, [([], expected)])
    print("dependency guards: PASS (12 actual-decrypt runs, 3 logging controls, 10 SessionInfo selector/bounds cases/6 removed-guard controls and 5 pre-log cases/1 late-guard control; synthetic drivers)")


if __name__ == "__main__":
    main()
