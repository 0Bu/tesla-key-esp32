#pragma once

// Included by the real PSA/native harness after vcsec_native helpers, before main/RNG shutdown.
// No test calls process_rx_frame_ directly. Production producer/pop/clear/whole-consumer bodies
// are extracted without edits; only OS, clock, storage and external effect sinks are passive.
namespace deferred_native {
using Message = UniversalMessage_RoutableMessage;
using Kind = tk::BleDeferredEventKind;
using Event = VehicleController::BleHostEvent;
constexpr uint32_t kPaddingTag = 1000;

static void initialize_mailbox(VehicleController& controller) {
    // Passive startup seam. A source contract separately proves publication precedes registration.
    native_mailbox_enter(&controller.ble_events_.mux);
    controller.ble_events_.queue.clear();
    controller.ble_events_.ready = true;
    native_mailbox_exit(&controller.ble_events_.mux);
}
static size_t cells(VehicleController& controller) {
    native_mailbox_enter(&controller.ble_events_.mux);
    const auto result = controller.ble_events_.queue.cells_used();
    native_mailbox_exit(&controller.ble_events_.mux);
    return result;
}
static void varint(std::vector<uint8_t>& output, size_t value) {
    do {
        const auto byte = static_cast<uint8_t>(value & 0x7f);
        value >>= 7;
        output.push_back(static_cast<uint8_t>(byte | (value ? 0x80 : 0)));
    } while (value);
}
static bool padding_is_unknown() {
    Message message = UniversalMessage_RoutableMessage_init_default;
    pb_field_iter_t field;
    if (!pb_field_iter_begin(&field, UniversalMessage_RoutableMessage_fields, &message)) return false;
    do { if (field.tag == kPaddingTag) return false; } while (pb_field_iter_next(&field));
    return true;
}
static std::vector<uint8_t> encode_wire(const Message& message, size_t target_wire_size) {
    uint8_t encoded[2048]{};
    auto stream = pb_ostream_from_buffer(encoded, sizeof encoded);
    if (!pb_encode(&stream, UniversalMessage_RoutableMessage_fields, &message)) return {};
    std::vector<uint8_t> payload(encoded, encoded + stream.bytes_written);
    if (target_wire_size != 0) {
        if (target_wire_size < 2 || target_wire_size - 2 > 2048 || !padding_is_unknown()) return {};
        const size_t target = target_wire_size - 2;
        std::vector<uint8_t> tag;
        varint(tag, (kPaddingTag << 3) | 2u);
        bool found = false;
        for (size_t length = 0; length <= target; ++length) {
            std::vector<uint8_t> encoded_length;
            varint(encoded_length, length);
            if (payload.size() + tag.size() + encoded_length.size() + length == target) {
                payload.insert(payload.end(), tag.begin(), tag.end());
                payload.insert(payload.end(), encoded_length.begin(), encoded_length.end());
                payload.insert(payload.end(), length, 0x5a);
                found = true;
                break;
            }
        }
        if (!found) return {};
    }
    std::vector<uint8_t> wire{static_cast<uint8_t>(payload.size() >> 8),
                               static_cast<uint8_t>(payload.size() & 0xff)};
    wire.insert(wire.end(), payload.begin(), payload.end());
    return wire;
}
static bool publish(VehicleController& controller, const std::vector<uint8_t>& wire,
                    unsigned partition, uint32_t generation, unsigned* notifications = nullptr) {
    constexpr size_t mixed[] = {1, 20, 243, 2, 244, 7, 19, 3};
    size_t offset = 0, iteration = 0;
    bool accepted = true;
    while (offset < wire.size()) {
        const size_t requested = partition == 3 ? mixed[iteration % 8]
                               : partition == 0 ? 20 : partition == 1 ? 244 : 1;
        const size_t size = std::min(requested, wire.size() - offset);
        std::array<uint8_t, 244> caller{};
        std::memcpy(caller.data(), wire.data() + offset, size);
        accepted = VehicleController::ble_rx_event_cb_(&controller, caller.data(), size, generation) && accepted;
        std::fill(caller.begin(), caller.end(), 0xa5); // callback-local input is now dead/reused.
        offset += size;
        ++iteration;
    }
    if (notifications) *notifications = static_cast<unsigned>(iteration);
    return accepted;
}
static void shift_head(VehicleController& controller, unsigned count) {
    for (unsigned i = 0; i < count; ++i) {
        const bool queued = VehicleController::ble_link_event_cb_(&controller, true, 0x22, 8);
        Event discarded;
        const bool popped = controller.pop_ble_host_event_(discarded);
        expect(queued && popped && discarded.kind == Kind::LinkUp,
               "actual producer/pop advances the private ring head", "ring head setup failed");
    }
}
static void arrival_timing() {
    for (unsigned delayed = 0; delayed != 2; ++delayed) {
        vcsec_native::Fixture fixture;
        auto& c = fixture.controller;
        initialize_mailbox(c);
        fixture.enqueue(); c.drive_command_runner_();
        const auto wire = encode_wire(fixture.seal(vcsec_native::command_status(
            vcsec_native::envelope(fixture.request(), true), VCSEC_OperationStatus_E_OPERATIONSTATUS_OK), 11), 743);
        const std::vector<uint8_t> first(wire.begin(), wire.begin() + 20);
        const bool first_queued = publish(c, first, 0, 8);
        if (delayed == 0) {
            // Timely RX, late consumer: the old drain-time substitution discarded this reply.
            c.process_ble_host_events_();
            native_ticks += 10;
            const std::vector<uint8_t> rest(wire.begin() + 20, wire.end());
            const bool rest_queued = publish(c, rest, 0, 8);
            native_ticks += 4000;
            c.process_ble_host_events_();
            expect(first_queued && rest_queued && fixture.completions == 1 && fixture.success &&
                   c.command_runner_.rx_framer().stats().timeout_drops == 0,
                   "actual consumer preserves timely queued continuation despite delayed drain",
                   "processing latency became a false RX gap");
        } else {
            // Actual >3s gap, one delayed drain: old coalescing hid it and mixed two replies.
            native_ticks += 4001;
            const bool complete_queued = publish(c, wire, 0, 8);
            native_ticks += 1000;
            c.process_ble_host_events_();
            expect(first_queued && complete_queued && fixture.completions == 1 && fixture.success &&
                   c.command_runner_.rx_framer().stats().timeout_drops == 1 &&
                   c.command_runner_.rx_framer().stats().corrupt_length_drops == 0,
                   "actual callback/queue/consumer retains a real gap before an authenticated reply",
                   "coalescing hid the gap or lost the complete response");
        }
    }
}
static void assert_unlocked() {
    expect(native_mailbox_depth == 0 && native_mailbox_lock_violations == 0 &&
           native_external_lock_violations == 0,
           "actual mailbox unlocks before generation/Vehicle/Client/log/storage/link sinks",
           "mailbox remained locked or an external sink ran under its critical section");
}

static void max_frame_delivery() {
    expect(padding_is_unknown(), "stress padding tag is absent from the real pinned nanopb descriptor",
           "stress padding collided with a real protocol field");
    for (const size_t wire_size : {size_t{743}, size_t{2050}}) {
        for (unsigned partition = 0; partition != 4; ++partition) {
            for (const unsigned shifted : {0u, 15u}) {
                vcsec_native::Fixture fixture;
                auto& controller = fixture.controller;
                initialize_mailbox(controller);
                shift_head(controller, shifted);
                fixture.enqueue();
                controller.drive_command_runner_();
                const auto request = fixture.request();
                const auto response = fixture.seal(vcsec_native::command_status(
                    vcsec_native::envelope(request, true), VCSEC_OperationStatus_E_OPERATIONSTATUS_OK), 11);
                const auto wire = encode_wire(response, wire_size);
                Message decoded = UniversalMessage_RoutableMessage_init_default;
                const bool encoded = wire.size() == wire_size && controller.client_->parse_universal_message(
                    const_cast<uint8_t*>(wire.data() + 2), wire.size() - 2, &decoded) == 0;
                expect(encoded && decoded.request_uuid.size == request.uuid.size() &&
                       std::memcmp(decoded.request_uuid.bytes, request.uuid.data(), request.uuid.size()) == 0 &&
                       decoded.sub_sigData.signature_data.sig_type.AES_GCM_Response_data.counter == 11,
                       "exact743/2050-byte decoder-valid stress response preserves UUID/counter",
                       "maximal response fixture is not valid nanopb/current-request data");
                if (!encoded) continue;

                // Independent byte oracle: pause the consumer, then use actual pop wrappers.
                std::vector<uint8_t> drained(wire.size());
                size_t copied = 0;
                unsigned notifications = 0;
                fail_next_cpp_allocation = true;
                const bool all_queued = publish(controller, wire, partition, 8, &notifications);
                const auto used = cells(controller);
                bool metadata = true;
                Event event;
                while (controller.pop_ble_host_event_(event)) {
                    metadata = metadata && event.kind == Kind::Rx && event.generation == 8 &&
                               event.size > 0 && event.size <= 20 && copied + event.size <= drained.size();
                    if (copied + event.size > drained.size()) break;
                    std::memcpy(drained.data() + copied, event.data.data(), event.size);
                    copied += event.size;
                }
                const bool no_cpp_allocation = fail_next_cpp_allocation;
                fail_next_cpp_allocation = false;
                expect(all_queued && metadata && copied == wire.size() && drained == wire &&
                       used == (wire.size() + 243) / 244 && no_cpp_allocation,
                       "paused actual callback/pop path owns every byte without C++ allocation",
                       "mailbox dropped/corrupted bytes, metadata, capacity or allocated");
                expect(partition != 0 || notifications == (wire.size() + 19) / 20,
                       "default-MTU stress uses38/103 actual callback invocations",
                       "notification count bypassed the original capacity boundary");

                // Separate ingress of the same authenticated response, now through WHOLE consumer.
                const auto writes = controller.ble.writes;
                const bool link = VehicleController::ble_link_event_cb_(&controller, true, 0x22, 8);
                const bool rx = publish(controller, wire, partition, 8);
                expect(fixture.completions == 0, "consumer is paused until the entire response is queued",
                       "test bypassed the deferred consumer before drain");
                controller.process_ble_host_events_();
                const bool counter_used = !fixture.client.get_peer(
                    UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY)->validate_response_counter(11);
                expect(link && rx && fixture.completions == 1 && fixture.success && fixture.last_reason.empty() &&
                       counter_used && controller.command_runner_.rx_framer().stats().frames_completed == 1 &&
                       controller.command_runner_.rx_framer().buffered_bytes() == 0 &&
                       controller.ble.ready_calls == 1 && controller.deferred_mac_persists == 1 &&
                       !controller.ble_events_.overflow.load() && !controller.ble_fault_.load() &&
                       controller.ble.disconnects == 0 && controller.ble.writes == writes && cells(controller) == 0,
                       "whole real consumer/framer/Client/authenticated native dispatch completes maximal response once",
                       "maximal response failed actual deferred authenticated delivery or caused an extra effect");
                assert_unlocked();
            }
        }
    }
}

static void queue_edges() {
    Client client;
    VehicleController controller(client);
    uint8_t byte = 0x73;
    expect(!VehicleController::ble_rx_event_cb_(nullptr, &byte, 1, 8) &&
           !VehicleController::ble_link_event_cb_(nullptr, true, 0x22, 8) &&
           !VehicleController::ble_rx_event_cb_(&controller, &byte, 1, 8),
           "actual adapters refuse null context and unpublished mailbox", "preinit callback was admitted");
    initialize_mailbox(controller);
    controller.ble_events_.overflow.store(false); // private fixture reset, not firmware recovery.
    std::array<uint8_t, 245> excessive{};
    expect(!controller.enqueue_ble_rx_event_(nullptr, 1, 8) &&
           !controller.enqueue_ble_rx_event_(&byte, 0, 8) &&
           !controller.enqueue_ble_rx_event_(excessive.data(), excessive.size(), 8) && cells(controller) == 0,
           "actual enqueue refuses null/zero/245-byte input without retained prefix",
           "invalid PDU mutated the fixed queue");
    controller.ble_events_.overflow.store(false);
    expect(controller.enqueue_ble_rx_event_(&byte, 1, 8) &&
           controller.enqueue_ble_rx_event_(&byte, 1, 10),
           "different generation RX is separately queued", "generation queue setup failed");
    Event first, second, untouched;
    untouched.generation = 0xabc;
    expect(controller.pop_ble_host_event_(first) && controller.pop_ble_host_event_(second) &&
           first.generation == 8 && second.generation == 10 && first.size == 1 && second.size == 1 &&
           !controller.pop_ble_host_event_(untouched) && untouched.generation == 0xabc,
           "actual pop preserves generation barriers and failed-pop output", "RX generations merged or empty pop mutated output");
    for (const size_t space : {size_t{1}, size_t{20}, size_t{243}, size_t{244}}) {
        controller.clear_ble_host_events_();
        controller.ble_events_.overflow.store(false);
        bool fill = true;
        for (unsigned i = 0; i != 15; ++i) fill = controller.enqueue_ble_link_event_(true, 0x22, 8) && fill;
        std::array<uint8_t, 244> original, next;
        original.fill(0x31); next.fill(0x72);
        // 244 is a fresh last cell; an occupied RX cell can have only0..243 bytes free.
        const auto prefix = space == 244 ? size_t{244} : 244 - space;
        fill = controller.enqueue_ble_rx_event_(original.data(), prefix, 8) && fill;
        const size_t refused_size = space == 244 ? 1 : space + 1;
        expect(fill && cells(controller) == 16 &&
               !controller.enqueue_ble_rx_event_(next.data(), refused_size, 8) &&
               controller.ble_events_.overflow.load(),
               "full callback refuses before changing any old tail bytes", "full-push preflight accepted or lost overflow");
        std::vector<uint8_t> drained;
        Event event;
        unsigned barriers = 0;
        while (controller.pop_ble_host_event_(event)) {
            if (event.kind == Kind::Rx) drained.insert(drained.end(), event.data.begin(), event.data.begin() + event.size);
            else ++barriers;
        }
        expect(barriers == 15 && drained == std::vector<uint8_t>(prefix, 0x31),
               "rejected split append leaves the entire old prefix intact", "full rejection partially appended or overwrote an old cell");
        if (space != 244) {
            // A full-but-extendable tail must accept an exactly fitting callback.
            controller.clear_ble_host_events_(); controller.ble_events_.overflow.store(false);
            for (unsigned i = 0; i != 15; ++i) fill = controller.enqueue_ble_link_event_(true, 0x22, 8) && fill;
            fill = controller.enqueue_ble_rx_event_(original.data(), prefix, 8) && fill;
            expect(fill && controller.enqueue_ble_rx_event_(next.data(), space, 8) && cells(controller) == 16,
                   "full queue accepts callback wholly fitting existing same-generation tail",
                   "full queue unnecessarily refused an extendable tail");
            std::vector<uint8_t> fitted;
            unsigned fitted_barriers = 0;
            while (controller.pop_ble_host_event_(event)) {
                if (event.kind == Kind::Rx) fitted.insert(fitted.end(), event.data.begin(), event.data.begin() + event.size);
                else ++fitted_barriers;
            }
            std::vector<uint8_t> expected_fit(prefix, 0x31);
            expected_fit.insert(expected_fit.end(), space, 0x72);
            expect(fitted_barriers == 15 && fitted == expected_fit,
                   "full-but-extendable tail retains old prefix and every appended byte",
                   "full-tail success skipped, corrupted or reordered a copied byte");
        }
    }
    controller.clear_ble_host_events_(); controller.ble_events_.overflow.store(false);
    std::array<uint8_t, 244> full;
    full.fill(0x61);
    const bool initial_full = controller.enqueue_ble_rx_event_(full.data(), full.size(), 8) &&
                              controller.pop_ble_host_event_(first) && first.size == 20;
    full.fill(0x72);
    expect(initial_full && controller.enqueue_ble_rx_event_(full.data(), full.size(), 8),
           "actual producer can append after a partially consumed head", "partial-head setup failed");
    size_t remaining = 0;
    bool intact = true;
    while (controller.pop_ble_host_event_(first)) {
        for (size_t i = 0; i < first.size; ++i)
            intact = intact && first.data[i] == (remaining + i < 224 ? 0x61 : 0x72);
        remaining += first.size;
    }
    expect(remaining == 468 && intact, "partial-head refill preserves remaining224 plus next244 bytes",
           "partial-head append reused live prefix or lost bytes");
    controller.enqueue_ble_rx_event_(full.data(), full.size(), 8);
    controller.pop_ble_host_event_(first);
    controller.clear_ble_host_events_();
    expect(controller.enqueue_ble_rx_event_(&byte, 1, 8) && controller.pop_ble_host_event_(first) &&
           first.size == 1 && first.data[0] == byte && cells(controller) == 0,
           "actual clear retires old read offset and permits reuse", "clear reused an old partial offset");
    assert_unlocked();
}

static void lifecycle_and_faults() {
    // A barrier is always consumed, even when its generation is old. It must reset real peers.
    {
        vcsec_native::Fixture fixture;
        auto& c = fixture.controller; initialize_mailbox(c);
        c.auth_fail_streak_.store(3); c.ble.generation = 10;
        auto status = vcsec_native::status(vcsec_native::envelope({}));
        status.has_to_destination = false;
        const auto wire = encode_wire(status, 0);
        const uint8_t partial[] = {0, 4, 0x31};
        bool ready_saw_reset = false;
        c.ble.during_ready = [&] {
            ready_saw_reset = !fixture.client.get_peer(UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY)->is_valid() &&
                             c.vcsec_sleep_state_.load() == static_cast<int>(tk::SleepState::Unknown) &&
                             c.command_runner_.rx_framer().buffered_bytes() == 0;
        };
        const bool queued = c.enqueue_ble_rx_event_(partial, sizeof partial, 8) &&
            VehicleController::ble_link_event_cb_(&c, false, 0x22, 8) &&
            VehicleController::ble_link_event_cb_(&c, true, 0x22, 10) && publish(c, wire, 2, 10);
        c.process_ble_host_events_();
        expect(queued && ready_saw_reset && c.auth_fail_streak_.load() == 0 &&
               c.ble.lifecycle_trace.equals({'D', 'U', 'P'}) &&
               c.sleep_deliveries == 1 && fixture.completions == 0 && !c.ble_fault_.load(),
               "old LinkDown precedes new readiness and real passive status without stale bytes",
               "link barrier was lost, reordered or admitted stale framing/session state");
    }
    for (const uint32_t event_generation : {6u, 9u}) {
        vcsec_native::Fixture fixture;
        auto& c = fixture.controller; initialize_mailbox(c);
        fixture.enqueue(); c.drive_command_runner_();
        const auto response = fixture.seal(vcsec_native::command_status(vcsec_native::envelope(fixture.request(), true),
            VCSEC_OperationStatus_E_OPERATIONSTATUS_OK), 11);
        const auto wire = encode_wire(response, 743);
        expect(publish(c, wire, 0, event_generation), "stale/odd bytes physically enter deferred storage",
               "generation fixture could not enqueue bytes");
        c.process_ble_host_events_();
        expect(fixture.completions == 0 && c.command_runner_.rx_framer().stats().frames_completed == 0,
               "whole consumer discards old and unstable generations before framing",
               "stale/odd response reached dispatch");
    }
    // Invalid GCM must fail at the real Client/Peer, then the unchanged valid fixture must deliver.
    {
        vcsec_native::Fixture fixture;
        auto& c = fixture.controller; initialize_mailbox(c); fixture.enqueue(); c.drive_command_runner_();
        auto response = fixture.seal(vcsec_native::command_status(vcsec_native::envelope(fixture.request(), true),
            VCSEC_OperationStatus_E_OPERATIONSTATUS_OK), 11);
        auto corrupt = response; corrupt.payload.protobuf_message_as_bytes.bytes[0] ^= 1;
        const auto bad = encode_wire(corrupt, 2050), good = encode_wire(response, 2050);
        const bool queued = publish(c, bad, 0, 8); c.process_ble_host_events_();
        expect(queued && fixture.completions == 0 && !fixture.success,
               "full deferred path refuses a corrupted authenticated response", "bad GCM completed the command");
        const bool restored = publish(c, good, 0, 8); c.process_ble_host_events_();
        expect(restored && fixture.completions == 1 && fixture.success,
               "restored authenticated response still completes after bad-tag refusal",
               "bad tag consumed replay/dispatcher state or valid restoration failed");
    }
    // Reset during actual dispatch must not make the framer erase through its retired vector.
    {
        vcsec_native::Fixture fixture;
        auto& c = fixture.controller; initialize_mailbox(c); fixture.enqueue(); c.drive_command_runner_();
        const auto response = fixture.seal(vcsec_native::command_status(vcsec_native::envelope(fixture.request(), true),
            VCSEC_OperationStatus_E_OPERATIONSTATUS_OK), 11);
        const auto wire = encode_wire(response, 743);
        unsigned completed = 0;
        fixture.command()->on_complete = [&](bool ok, const std::string&, tk::CommandFailureInfo) {
            native_require_unlocked(); if (ok) ++completed; c.command_runner_.rx_framer().reset();
        };
        const bool queued = publish(c, wire, 3, 8); c.process_ble_host_events_();
        expect(queued && completed == 1 && c.command_runner_.rx_framer().buffered_bytes() == 0 && !c.ble_fault_.load(),
               "actual completion can reset framing inside the whole drain without stale-vector use",
               "dispatch reset corrupted the real framer or repeated completion");
    }
    {
        vcsec_native::Fixture fixture;
        auto& c = fixture.controller; initialize_mailbox(c); fixture.enqueue(); c.drive_command_runner_();
        const auto wire = encode_wire(fixture.seal(vcsec_native::command_status(vcsec_native::envelope(fixture.request(), true),
            VCSEC_OperationStatus_E_OPERATIONSTATUS_OK), 11), 743);
        const auto writes = c.ble.writes;
        const bool queued = publish(c, wire, 3, 8) && publish(c, wire, 2, 8);
        c.process_ble_host_events_();
        expect(queued && fixture.completions == 1 && fixture.success &&
               c.command_runner_.rx_framer().stats().frames_completed == 2 &&
               c.command_runner_.rx_framer().buffered_bytes() == 0 && c.ble.writes == writes,
               "back-to-back real frames are both framed but duplicate response completes only once",
               "back-to-back queue/framer handling lost a frame or duplicated an effect");
    }
    {
        vcsec_native::Fixture fixture;
        auto& c = fixture.controller; initialize_mailbox(c); c.ble.ready_ok = false;
        const bool queued = VehicleController::ble_link_event_cb_(&c, true, 0x22, 8);
        c.process_ble_host_events_();
        expect(queued && c.ble.ready_calls == 1 && c.deferred_mac_persists == 0 &&
               c.ble.disconnects == 1 && c.ble.lifecycle_trace.equals({'U', 'D', 'X'}) &&
               !fixture.client.get_peer(UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY)->is_valid(),
               "failed readiness acknowledgement unwinds actual native link state before disconnect",
               "failed ready CAS left a usable old peer or persisted the unready link");
    }
    for (unsigned fault = 0; fault != 2; ++fault) {
        vcsec_native::Fixture fixture;
        auto& c = fixture.controller; initialize_mailbox(c); fixture.enqueue(); c.drive_command_runner_();
        const auto wire = encode_wire(fixture.seal(vcsec_native::command_status(vcsec_native::envelope(fixture.request(), true),
            VCSEC_OperationStatus_E_OPERATIONSTATUS_OK), 11), 743);
        const bool queued = publish(c, wire, 0, 8);
        if (fault == 0) fail_next_cpp_allocation = true;
        else c.vehicle_mutex_->fail = true;
        c.process_ble_host_events_();
        fail_next_cpp_allocation = false; c.vehicle_mutex_->fail = false;
        expect(queued && fixture.completions == 0 && c.ble_fault_.load() &&
               c.command_runner_.rx_framer().buffered_bytes() == 0,
               "actual framer OOM/Vehicle-lock refusal is contained with no completion",
               "drain failed to contain allocation/lock failure");
    }
    // Receive half, run the entire consumer twice with no further bytes and advance its real clock seam.
    {
        vcsec_native::Fixture fixture;
        auto& c = fixture.controller; initialize_mailbox(c); fixture.enqueue(); c.drive_command_runner_();
        const auto wire = encode_wire(fixture.seal(vcsec_native::command_status(vcsec_native::envelope(fixture.request(), true),
            VCSEC_OperationStatus_E_OPERATIONSTATUS_OK), 11), 743);
        const std::vector<uint8_t> partial(wire.begin(), wire.begin() + 20);
        const bool queued = publish(c, partial, 0, 8); c.process_ble_host_events_();
        const auto before = c.command_runner_.rx_framer().buffered_bytes();
        native_ticks += c.command_runner_.rx_framer().timeout_ms() + 1;
        c.process_ble_host_events_();
        expect(queued && before == 20 && fixture.completions == 0 &&
               c.command_runner_.rx_framer().buffered_bytes() == 0 &&
               c.command_runner_.rx_framer().stats().timeout_drops == 1,
               "whole consumer's periodic check retires an interrupted frame after its existing timeout",
               "partial drain bypassed or renewed the framing deadline");
    }
    // RX refusal closes the SDK's readiness/disconnecting view before the later GAP event
    // changes the generation. The sink below is a passive projection of those immediate effects,
    // not a full BleClient/FreeRTOS simulation. The complete response was queued before loss;
    // native command admission must still refuse it once disconnecting is already visible.
    {
        vcsec_native::Fixture fixture;
        auto& c = fixture.controller; initialize_mailbox(c); fixture.enqueue(); c.drive_command_runner_();
        const auto wire = encode_wire(fixture.seal(vcsec_native::command_status(vcsec_native::envelope(fixture.request(), true),
            VCSEC_OperationStatus_E_OPERATIONSTATUS_OK), 11), 2050);
        bool injected = false, refused = false;
        c.ble.during_generation = [&] {
            if (injected) return;
            injected = true;
            const std::array<uint8_t, 244> pressure{};
            for (unsigned i = 0; i < 16 && cells(c) < 16; ++i)
                (void)VehicleController::ble_rx_event_cb_(&c, pressure.data(), pressure.size(), 8);
            refused = !VehicleController::ble_rx_event_cb_(&c, pressure.data(), pressure.size(), 8);
            // Real RX-refusal owner clears ready_generation_, closes intent and sets disconnecting_
            // before terminate; lifecycle_generation() remains 8 until GAP disconnect publishes 9.
            c.ble.connected = false;
            c.ble.ready_ok = false;
        };
        const bool queued = publish(c, wire, 0, 8); c.process_ble_host_events_();
        c.ble.during_generation = {};
        const bool pending = c.ble_events_.overflow.load();
        const bool refused_before_gap = c.ble.generation == 8 && !c.ble.connected && !c.ble.ready_ok &&
                                        fixture.completions == 0 && !fixture.success;
        c.process_ble_host_events_();
        const auto disconnects = c.ble.disconnects;
        c.process_ble_host_events_();
        expect(queued && injected && refused && pending && refused_before_gap &&
               disconnects == 1 && c.ble.disconnects == disconnects && cells(c) == 0 &&
               !c.ble_events_.overflow.load() && c.command_runner_.rx_framer().buffered_bytes() == 0,
               "RX refusal before GAP generation change blocks native completion and retires overflow once",
               "pre-GAP RX refusal admitted a completed response or repeated/lost overflow retirement");
    }
    // Separately cover the later GAP generation invalidation. This is deliberately not a claim
    // that overflow flag + queue + drain form one atomic transaction.
    {
        vcsec_native::Fixture fixture;
        auto& c = fixture.controller; initialize_mailbox(c); fixture.enqueue(); c.drive_command_runner_();
        const auto wire = encode_wire(fixture.seal(vcsec_native::command_status(vcsec_native::envelope(fixture.request(), true),
            VCSEC_OperationStatus_E_OPERATIONSTATUS_OK), 11), 2050);
        bool injected = false, refused = false;
        c.ble.during_generation = [&] {
            if (injected) return;
            injected = true;
            for (unsigned i = 0; i < 16 && cells(c) < 16; ++i)
                (void)c.enqueue_ble_link_event_(true, 0x22, 8);
            refused = !c.enqueue_ble_link_event_(true, 0x22, 8);
            c.ble.generation = 9; // passive later GAP-disconnect generation publication.
        };
        const bool queued = publish(c, wire, 0, 8); c.process_ble_host_events_();
        c.ble.during_generation = {};
        const bool pending = c.ble_events_.overflow.load();
        c.process_ble_host_events_();
        expect(queued && injected && refused && pending && !fixture.success &&
               c.ble.disconnects == 1 && cells(c) == 0 && !c.ble_events_.overflow.load() &&
               c.command_runner_.rx_framer().buffered_bytes() == 0,
               "interrupted drain observes producer overflow and resets/disconnects at its next observation",
               "overflow was lost or a stale response completed during generation invalidation");
    }
    assert_unlocked();
}
} // namespace deferred_native

static void check_native_ble_deferred_delivery() {
    std::printf("\n--- F116 actual callback/mailbox/whole-consumer delivery ---\n");
    deferred_native::queue_edges();
    deferred_native::arrival_timing();
    deferred_native::max_frame_delivery();
    deferred_native::lifecycle_and_faults();
    deferred_native::assert_unlocked();
}
