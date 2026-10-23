#include "coredump_hash.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

namespace {
int checks = 0;
#define HASH_CHECK(condition) do { ++checks; if (!(condition)) { \
    std::fprintf(stderr, "FAIL coredump hash:%d: %s\n", __LINE__, #condition); \
    std::exit(1); } } while (0)

enum class Phase { None, Init, Setup, Update, Verify, Abort };
struct FakeHash {
    Phase fail{Phase::None}, throwing{Phase::None};
    unsigned init{}, setup{}, update{}, verify{}, abort{}, active{};
    std::vector<uint8_t> body, received, trailer;
    std::vector<char> events;
} fake;

bool fail(Phase phase) {
    if (fake.throwing == phase) throw std::bad_alloc();
    return fake.fail == phase;
}
}  // namespace

extern "C" psa_status_t psa_crypto_init(void) {
    ++fake.init; fake.events.push_back('i');
    return fail(Phase::Init) ? -1 : PSA_SUCCESS;
}
extern "C" psa_status_t psa_hash_setup(psa_hash_operation_t* operation, psa_algorithm_t algorithm) {
    ++fake.setup; fake.events.push_back('s');
    HASH_CHECK(fake.init == 1 && algorithm == PSA_ALG_SHA_256);
    operation->active = 1; ++fake.active;   // setup failure may already own a resource
    return fail(Phase::Setup) ? -1 : PSA_SUCCESS;
}
extern "C" psa_status_t psa_hash_update(psa_hash_operation_t* operation, const uint8_t* bytes, size_t length) {
    ++fake.update; fake.events.push_back('u');
    HASH_CHECK(operation->active == 1 && length > 0 && length <= 256);
    if (fail(Phase::Update)) return -1;
    fake.received.insert(fake.received.end(), bytes, bytes + length);
    return PSA_SUCCESS;
}
extern "C" psa_status_t psa_hash_verify(psa_hash_operation_t* operation, const uint8_t* trailer, size_t length) {
    ++fake.verify; fake.events.push_back('v');
    HASH_CHECK(operation->active == 1 && length == 32);
    if (fail(Phase::Verify)) return -1;
    return fake.received == fake.body && std::equal(fake.trailer.begin(), fake.trailer.end(), trailer)
        ? PSA_SUCCESS : -1;
}
extern "C" psa_status_t psa_hash_abort(psa_hash_operation_t* operation) {
    ++fake.abort; fake.events.push_back('a');
    HASH_CHECK(fake.init == 1);
    if (fail(Phase::Abort)) return -1;
    if (operation->active) { HASH_CHECK(fake.active > 0); --fake.active; }
    operation->active = 0;
    return PSA_SUCCESS;
}

int test_coredump_hash() {
    const int before = checks;
    std::vector<uint8_t> image(768, 0x5a);
    for (unsigned i = 0; i < 4; ++i) image[i] = static_cast<uint8_t>(image.size() >> (i * 8));
    std::fill(image.end() - 32, image.end(), 0xa5);  // synthetic trailer, not a real SHA vector
    unsigned last_reads = 0;
    auto run = [&](Phase error = Phase::None, Phase throwing = Phase::None,
                   unsigned fail_read = 0, unsigned throw_read = 0) {
        fake = FakeHash{}; fake.fail = error; fake.throwing = throwing;
        fake.body.assign(image.begin(), image.end() - 32);
        fake.trailer.assign(image.end() - 32, image.end());
        unsigned reads = 0;
        auto read = [&](uint32_t offset, void* destination, uint32_t length) {
            ++reads;
            HASH_CHECK(offset <= image.size() && length <= image.size() - offset && length <= 256);
            if (reads == throw_read) throw std::bad_alloc();
            if (reads == fail_read) return false;
            std::memcpy(destination, image.data() + offset, length);
            return true;
        };
        tk::CoredumpInput<decltype(read)> input(read, static_cast<uint32_t>(image.size()));
        const auto outer = input.admit();
        bool result = false, caught = false;
        try { result = tk::verify_coredump_checksum(input, outer); }
        catch (const std::bad_alloc&) { caught = true; }
        HASH_CHECK(caught == (throwing != Phase::None || throw_read != 0));
        if (fail_read && reads >= fail_read) {
            uint8_t byte;
            HASH_CHECK(input.failed() && !input.read(0, &byte, 1) && reads == fail_read);
        }
        last_reads = reads;
        return result;
    };
    HASH_CHECK(run());
    HASH_CHECK(fake.init == 1 && fake.setup == 1 && fake.update == 3 && fake.verify == 1);
    HASH_CHECK(fake.abort == 1 && fake.active == 0 && fake.events.back() == 'a');
    const unsigned normal_reads = last_reads;
    for (Phase phase : {Phase::Init, Phase::Setup, Phase::Update, Phase::Verify, Phase::Abort}) {
        HASH_CHECK(!run(phase));
        HASH_CHECK(fake.abort == (phase == Phase::Init ? 0U : 1U));
        HASH_CHECK(fake.active == (phase == Phase::Abort ? 1U : 0U));
        if (phase == Phase::Init) HASH_CHECK(fake.setup == 0 && fake.update == 0 && fake.verify == 0);
        if (phase == Phase::Setup) HASH_CHECK(fake.update == 0 && fake.verify == 0);
        if (phase == Phase::Update) HASH_CHECK(fake.verify == 0);
        HASH_CHECK(!run(Phase::None, phase));
        HASH_CHECK(fake.abort == (phase == Phase::Init ? 0U : 1U));
    }
    for (unsigned at = 1; at <= normal_reads; ++at) {
        HASH_CHECK(!run(Phase::None, Phase::None, at));
        HASH_CHECK(fake.abort == (at == 1 ? 0U : 1U));
    }
    for (unsigned at = 2; at <= normal_reads; ++at) {
        HASH_CHECK(!run(Phase::None, Phase::None, 0, at));
        HASH_CHECK(fake.abort == 1 && fake.active == 0);
    }
    // Explicit close is consumed before the API call; neither error nor destructor retries it.
    fake = FakeHash{}; HASH_CHECK(psa_crypto_init() == PSA_SUCCESS);
    {
        tk::coredump_detail::HashOperation operation;
        HASH_CHECK(operation.close());
        HASH_CHECK(!operation.close() && fake.abort == 1);
    }
    HASH_CHECK(fake.abort == 1);
    // Inspect still-live unsigned-byte storage after ending the placed object's lifetime.
    using Buffers = tk::coredump_detail::HashBuffers;
    static_assert(sizeof(Buffers) == 288, "test observes exactly the two byte buffers");
    alignas(Buffers) unsigned char storage[sizeof(Buffers)];
    auto* buffers = new (storage) Buffers;
    std::memset(buffers->chunk, 0x5a, sizeof(buffers->chunk));
    std::memset(buffers->trailer, 0xa5, sizeof(buffers->trailer));
    buffers->~Buffers();
    for (unsigned char byte : storage) HASH_CHECK(byte == 0);
    std::printf("OK %d coredump PSA adapter checks (synthetic API)\n", checks - before);
    return checks - before;
}
