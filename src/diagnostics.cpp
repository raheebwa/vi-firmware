#include "diagnostics.h"
#include "signals.h"
#include "can/canwrite.h"
#include "can/canread.h"
#include "util/log.h"
#include "util/timer.h"
#include "obd2.h"
#include <bitfield/bitfield.h>
#include <limits.h>

#define MAX_RECURRING_DIAGNOSTIC_FREQUENCY_HZ 10
#define DIAGNOSTIC_RESPONSE_ARBITRATION_ID_OFFSET 0x8

using openxc::diagnostics::ActiveDiagnosticRequest;
using openxc::diagnostics::DiagnosticRequestListEntry;
using openxc::diagnostics::DiagnosticsManager;
using openxc::util::log::debug;
using openxc::can::lookupBus;
using openxc::can::addAcceptanceFilter;
using openxc::can::removeAcceptanceFilter;
using openxc::can::read::sendNumericalMessage;
using openxc::pipeline::Pipeline;
using openxc::signals::getCanBuses;
using openxc::signals::getCanBusCount;

namespace time = openxc::util::time;
namespace pipeline = openxc::pipeline;

static bool timedOut(ActiveDiagnosticRequest* request) {
    // don't use staggered start with the timeout clock
    return time::elapsed(&request->timeoutClock, false);
}

/* Private: Returns true if a sufficient response has been received for a diagnostic request.
 *
 * This is true when at least one response has been received and the request is
 * configured to not wait for multiple responses. Functional broadcast requests
 * may often wish to wait the full 100ms for modules to respond.
 */
static bool responseReceived(ActiveDiagnosticRequest* request) {
    return !request->waitForMultipleResponses &&
                request->handle.completed;
}

/* Private: Returns true if the request has timed out waiting for a response,
 *      or a sufficient number of responses has been received.
 */
static bool requestCompleted(ActiveDiagnosticRequest* request) {
    return responseReceived(request) || (
            timedOut(request) && diagnostic_request_sent(&request->handle));
}

/* Private: Move the entry to the free list and decrement the lock count for any
 * CAN filters it used.
 */
static void cancelRequest(DiagnosticsManager* manager,
        DiagnosticRequestListEntry* entry) {
    LIST_INSERT_HEAD(&manager->freeRequestEntries, entry, listEntries);
    if(entry->request.arbitration_id == OBD2_FUNCTIONAL_BROADCAST_ID) {
        for(uint32_t filter = OBD2_FUNCTIONAL_RESPONSE_START;
                filter < OBD2_FUNCTIONAL_RESPONSE_START +
                    OBD2_FUNCTIONAL_RESPONSE_COUNT;
                filter++) {
            removeAcceptanceFilter(entry->request.bus, filter, getCanBuses(),
                    getCanBusCount());
        }
    } else {
        removeAcceptanceFilter(entry->request.bus,
                entry->request.arbitration_id +
                    DIAGNOSTIC_RESPONSE_ARBITRATION_ID_OFFSET,
                getCanBuses(), getCanBusCount());
    }
}

static void cleanupRequest(DiagnosticsManager* manager,
        DiagnosticRequestListEntry* entry) {
    ActiveDiagnosticRequest* request = &entry->request;
    if(entry->request.inFlight && requestCompleted(&entry->request)) {
        entry->request.inFlight = false;

        char request_string[128] = {0};
        diagnostic_request_to_string(&request->handle.request,
                request_string, sizeof(request_string));
        if(request->recurring) {
            debug("Moving completed recurring request to the back of the queue: %s",
                    request_string);
            TAILQ_REMOVE(&manager->recurringRequests, entry, queueEntries);
            TAILQ_INSERT_TAIL(&manager->recurringRequests, entry, queueEntries);
        } else {
            debug("Cancelling completed, non-recurring request: %s",
                    request_string);
            LIST_REMOVE(entry, listEntries);
            cancelRequest(manager, entry);
        }
    }
}

// clean up the request list, move as many to the free list as possible
static void cleanupActiveRequests(DiagnosticsManager* manager) {
    DiagnosticRequestListEntry* entry, *tmp;
    LIST_FOREACH_SAFE(entry, &manager->nonrecurringRequests, listEntries, tmp) {
        cleanupRequest(manager, entry);
    }

    TAILQ_FOREACH_SAFE(entry, &manager->recurringRequests, queueEntries, tmp) {
        cleanupRequest(manager, entry);
    }
}

static bool sendDiagnosticCanMessage(CanBus* bus,
        const uint32_t arbitrationId, const uint8_t* data,
        const uint8_t size) {
    CanMessage message = {
        id: arbitrationId,
        data: get_bitfield(data, size, 0, size * CHAR_BIT)
            << (64 - CHAR_BIT * size),
        length: size
    };
    openxc::can::write::enqueueMessage(bus, &message);
    return true;
}

static bool sendDiagnosticCanMessageBus1(
        const uint32_t arbitrationId, const uint8_t* data,
        const uint8_t size) {
    return sendDiagnosticCanMessage(&getCanBuses()[0], arbitrationId, data,
            size);
}

static bool sendDiagnosticCanMessageBus2(
        const uint32_t arbitrationId, const uint8_t* data,
        const uint8_t size) {
    return sendDiagnosticCanMessage(&getCanBuses()[1], arbitrationId, data,
            size);
}

void openxc::diagnostics::reset(DiagnosticsManager* manager) {
    TAILQ_INIT(&manager->recurringRequests);
    LIST_INIT(&manager->nonrecurringRequests);
    LIST_INIT(&manager->freeRequestEntries);

    for(int i = 0; i < MAX_SIMULTANEOUS_DIAG_REQUESTS; i++) {
        LIST_INSERT_HEAD(&manager->freeRequestEntries,
                &manager->requestListEntries[i], listEntries);
    }
}

void openxc::diagnostics::initialize(DiagnosticsManager* manager, CanBus* buses,
        int busCount, CanBus* obd2Bus) {
    if(busCount > 0) {
        manager->shims[0] = diagnostic_init_shims(openxc::util::log::debug,
                sendDiagnosticCanMessageBus1, NULL);
        manager->obd2Bus = obd2Bus;
        obd2::initialize(manager);
        if(busCount > 1) {
            manager->shims[1] = diagnostic_init_shims(openxc::util::log::debug,
                    sendDiagnosticCanMessageBus2, NULL);
        }
    }

    reset(manager);

}

static inline bool conflicting(ActiveDiagnosticRequest* request,
        ActiveDiagnosticRequest* candidate) {
    return (candidate->inFlight && candidate != request &&
            candidate->bus == request->bus &&
            candidate->arbitration_id == request->arbitration_id);
}


/* Private: Returns true if there are no other active requests to the same arb
 * ID.
 */
static inline bool clearToSend(DiagnosticsManager* manager,
        ActiveDiagnosticRequest* request) {
    DiagnosticRequestListEntry* entry;
    LIST_FOREACH(entry, &manager->nonrecurringRequests, listEntries) {
        if(conflicting(request, &entry->request)) {
            return false;
        }
    }

    TAILQ_FOREACH(entry, &manager->recurringRequests, queueEntries) {
        if(conflicting(request, &entry->request)) {
            return false;
        }
    }
    return true;
}

static inline bool shouldSend(ActiveDiagnosticRequest* request) {
    return !request->inFlight && (
            (!request->recurring && !requestCompleted(request)) ||
            (request->recurring && time::elapsed(&request->frequencyClock, true)));
}

static void sendRequest(DiagnosticsManager* manager, CanBus* bus,
        DiagnosticRequestListEntry* entry) {
    ActiveDiagnosticRequest* request = &entry->request;
    if(request->bus == bus && shouldSend(request) &&
            clearToSend(manager, request)) {
        time::tick(&request->frequencyClock);
        start_diagnostic_request(&manager->shims[bus->address - 1],
                &request->handle);
        request->timeoutClock = {0};
        request->timeoutClock.frequency = 10;
        time::tick(&request->timeoutClock);
        entry->request.inFlight = true;
    }
}

void openxc::diagnostics::sendRequests(DiagnosticsManager* manager,
        CanBus* bus) {
    cleanupActiveRequests(manager);

    DiagnosticRequestListEntry* entry;
    LIST_FOREACH(entry, &manager->nonrecurringRequests, listEntries) {
        sendRequest(manager, bus, entry);
    }

    TAILQ_FOREACH(entry, &manager->recurringRequests, queueEntries) {
        sendRequest(manager, bus, entry);
    }
}

static openxc_VehicleMessage wrapDiagnosticResponseWithSabot(CanBus* bus,
        const ActiveDiagnosticRequest* request,
        const DiagnosticResponse* response, float parsedValue) {
    openxc_VehicleMessage message = {0};
    message.has_type = true;
    message.type = openxc_VehicleMessage_Type_DIAGNOSTIC;
    message.has_diagnostic_response = true;
    message.diagnostic_response = {0};
    message.diagnostic_response.has_bus = true;
    message.diagnostic_response.bus = bus->address;
    message.diagnostic_response.has_message_id = true;

    if(request->arbitration_id != OBD2_FUNCTIONAL_BROADCAST_ID) {
        message.diagnostic_response.message_id = response->arbitration_id
            - DIAGNOSTIC_RESPONSE_ARBITRATION_ID_OFFSET;
    } else {
        // must preserve responding arb ID for responses to functional broadcast
        // requests, as they are the actual module address and not just arb ID +
        // 8.
        message.diagnostic_response.message_id = response->arbitration_id;
    }

    message.diagnostic_response.has_mode = true;
    message.diagnostic_response.mode = response->mode;
    message.diagnostic_response.has_pid = response->has_pid;
    if(message.diagnostic_response.has_pid) {
        message.diagnostic_response.pid = response->pid;
    }
    message.diagnostic_response.has_success = true;
    message.diagnostic_response.success = response->success;
    message.diagnostic_response.has_negative_response_code = !response->success;
    message.diagnostic_response.negative_response_code =
            response->negative_response_code;
    message.diagnostic_response.has_payload = response->payload_length > 0;
    memcpy(message.diagnostic_response.payload.bytes, response->payload,
            response->payload_length);
    message.diagnostic_response.payload.size = response->payload_length;

    if(message.diagnostic_response.has_payload && request->parsePayload) {
        message.diagnostic_response.has_value = true;
        message.diagnostic_response.value = parsedValue;
    }

    return message;
}

static void relayDiagnosticResponse(DiagnosticsManager* manager,
        ActiveDiagnosticRequest* request,
        const DiagnosticResponse* response, Pipeline* pipeline) {
    float value = diagnostic_payload_to_integer(response) *
            request->factor + request->offset;
    if(request->decoder != NULL) {
        value = request->decoder(response, value);
    }

    if(response->success && strnlen(
                request->genericName, sizeof(request->genericName)) > 0) {
        sendNumericalMessage(request->genericName, value, pipeline);
    } else {
        openxc_VehicleMessage message = wrapDiagnosticResponseWithSabot(
                request->bus, request, response, value);
        pipeline::sendVehicleMessage(&message, pipeline);
    }

    if(request->callback != NULL) {
        request->callback(manager, request, response, value);
    }
}

static void receiveCanMessage(DiagnosticsManager* manager,
        CanBus* bus,
        DiagnosticRequestListEntry* entry,
        CanMessage* message, Pipeline* pipeline) {
    if(bus == entry->request.bus && entry->request.inFlight) {
        ArrayOrBytes combined;
        combined.whole = message->data;
        DiagnosticResponse response = diagnostic_receive_can_frame(
                // TODO eek, is bus address and array index this tightly
                // coupled?
                &manager->shims[bus->address - 1],
                &entry->request.handle, message->id, combined.bytes,
                sizeof(combined.bytes));
        if(response.completed && entry->request.handle.completed) {
            if(entry->request.handle.success) {
                relayDiagnosticResponse(manager, &entry->request, &response,
                        pipeline);
            } else {
                debug("Fatal error sending or receiving diagnostic request");
            }
        }
    }
}

void openxc::diagnostics::receiveCanMessage(DiagnosticsManager* manager,
        CanBus* bus, CanMessage* message, Pipeline* pipeline) {
    DiagnosticRequestListEntry* entry;
    TAILQ_FOREACH(entry, &manager->recurringRequests, queueEntries) {
        receiveCanMessage(manager, bus, entry, message, pipeline);
    }

    LIST_FOREACH(entry, &manager->nonrecurringRequests, listEntries) {
        receiveCanMessage(manager, bus, entry, message, pipeline);
    }
    cleanupActiveRequests(manager);
}

/* Note that this pops it off of whichver list it was on and returns it, so make
 * sure to add it to some other list or it'll be lost.
 */
static DiagnosticRequestListEntry* lookupRecurringRequest(
        DiagnosticsManager* manager, const CanBus* bus,
        const DiagnosticRequest* request) {
    DiagnosticRequestListEntry* existingEntry = NULL, *entry, *tmp;
    TAILQ_FOREACH_SAFE(entry, &manager->recurringRequests, queueEntries, tmp) {
        ActiveDiagnosticRequest* candidate = &entry->request;
        if(candidate->bus == bus && diagnostic_request_equals(
                    &candidate->handle.request, request)) {
            TAILQ_REMOVE(&manager->recurringRequests, entry, queueEntries);
            existingEntry = entry;
            break;
        }
    }
    return existingEntry;
}

bool openxc::diagnostics::cancelRecurringRequest(
        DiagnosticsManager* manager, CanBus* bus, DiagnosticRequest* request) {
    DiagnosticRequestListEntry* entry = lookupRecurringRequest(manager, bus,
            request);
    if(entry != NULL) {
        cancelRequest(manager, entry);
    }
    return entry != NULL;
}

bool openxc::diagnostics::addRecurringRequest(DiagnosticsManager* manager,
        CanBus* bus, DiagnosticRequest* request, const char* genericName,
        bool parsePayload, float factor, float offset,
        const openxc::diagnostics::DiagnosticResponseDecoder decoder,
        const openxc::diagnostics::DiagnosticResponseCallback callback,
        float frequencyHz, bool waitForMultipleResponses) {
    if(frequencyHz > MAX_RECURRING_DIAGNOSTIC_FREQUENCY_HZ) {
        debug("Requested recurring diagnostic frequency %d is higher than maximum of %d",
                frequencyHz, MAX_RECURRING_DIAGNOSTIC_FREQUENCY_HZ);
        return false;
    }

    cleanupActiveRequests(manager);

    DiagnosticRequestListEntry* entry = NULL;
    if(frequencyHz != 0.0) {
        entry = lookupRecurringRequest(manager, bus, request);
    }

    bool usedFreeEntry = false;
    if(entry == NULL) {
        usedFreeEntry = true;
        entry = LIST_FIRST(&manager->freeRequestEntries);
        // Don't remove it from the free list yet, because there's still an
        // opportunity to fail before we add it to another other list.
        if(entry == NULL) {
            debug("Unable to allocate space for a new diagnostic request");
            return false;
        }

        bool filterStatus = true;
        if(request->arbitration_id == OBD2_FUNCTIONAL_BROADCAST_ID) {
            for(uint32_t filter = OBD2_FUNCTIONAL_RESPONSE_START;
                    filter < OBD2_FUNCTIONAL_RESPONSE_START +
                        OBD2_FUNCTIONAL_RESPONSE_COUNT;
                    filter++) {
                filterStatus = filterStatus && addAcceptanceFilter(bus, filter,
                        getCanBuses(), getCanBusCount());
            }
        } else {
            filterStatus = addAcceptanceFilter(bus,
                    request->arbitration_id +
                            DIAGNOSTIC_RESPONSE_ARBITRATION_ID_OFFSET,
                    getCanBuses(), getCanBusCount());
        }

        if(!filterStatus) {
            debug("Couldn't add filter 0x%x to bus %d", request->arbitration_id,
                    bus->address);
            return false;
        }
    }

    entry->request.bus = bus;
    entry->request.arbitration_id = request->arbitration_id;
    entry->request.handle = generate_diagnostic_request(
            &manager->shims[bus->address - 1], request, NULL);
    if(genericName != NULL) {
        strncpy(entry->request.genericName, genericName,
                MAX_GENERIC_NAME_LENGTH);
    } else {
        entry->request.genericName[0] = '\0';
    }
    entry->request.parsePayload = parsePayload;
    entry->request.waitForMultipleResponses = waitForMultipleResponses;
    entry->request.factor = factor;
    entry->request.offset = offset;
    entry->request.decoder = decoder;
    entry->request.callback = callback;
    entry->request.recurring = frequencyHz != 0;
    entry->request.frequencyClock = {0};
    entry->request.frequencyClock.frequency =
            entry->request.recurring ? frequencyHz : 0;
    // time out after 100ms
    entry->request.timeoutClock = {0};
    entry->request.timeoutClock.frequency = 10;
    entry->request.inFlight = false;

    char request_string[128] = {0};
    diagnostic_request_to_string(&entry->request.handle.request,
            request_string, sizeof(request_string));
    if(usedFreeEntry) {
        LIST_REMOVE(entry, listEntries);
        debug("Added new diagnostic request (freq: %f) on bus %d: %s",
                frequencyHz, bus->address, request_string);
    } else {
        // lookupRecurringRequest already popped it off of the queue
        debug("Updated existing diagnostic request (freq: %f): %s", frequencyHz,
                request_string);
    }

    if(entry->request.recurring) {
        TAILQ_INSERT_HEAD(&manager->recurringRequests, entry, queueEntries);
    } else {
        LIST_INSERT_HEAD(&manager->nonrecurringRequests, entry, listEntries);
    }

    return true;
}

bool openxc::diagnostics::addRecurringRequest(DiagnosticsManager* manager,
        CanBus* bus, DiagnosticRequest* request, const char* genericName,
        bool parsePayload, float factor, float offset,
        float frequencyHz, bool waitForMultipleResponses) {
    return addRecurringRequest(manager, bus, request, genericName,
            parsePayload, factor, offset, NULL, NULL, frequencyHz,
            waitForMultipleResponses);
}

bool openxc::diagnostics::addRecurringRequest(DiagnosticsManager* manager,
        CanBus* bus, DiagnosticRequest* request, float frequencyHz) {
    return addRecurringRequest(manager, bus, request, NULL, false, 1.0, 0,
            NULL, NULL, frequencyHz, false);
}

bool openxc::diagnostics::handleDiagnosticCommand(
        DiagnosticsManager* manager, openxc_ControlCommand* command) {
    bool status = true;
    if(command->has_diagnostic_request) {
        openxc_DiagnosticRequest* commandRequest = &command->diagnostic_request;
        if(commandRequest->has_message_id && commandRequest->has_mode) {
            CanBus* bus = NULL;
            if(commandRequest->has_bus) {
                bus = lookupBus(commandRequest->bus, getCanBuses(), getCanBusCount());
            } else if(getCanBusCount() > 0) {
                bus = &getCanBuses()[0];
                debug("No bus specified for diagnostic request missing bus, using first active: %d", bus->address);
            }

            if(bus == NULL) {
                debug("No active bus to send diagnostic request");
                status = false;
            } else if(bus->rawWritable) {
                DiagnosticRequest request = {
                    arbitration_id: commandRequest->message_id,
                    mode: uint8_t(commandRequest->mode),
                };

                if(commandRequest->has_payload) {
                    request.payload_length = commandRequest->payload.size;
                    memcpy(request.payload, commandRequest->payload.bytes,
                            request.payload_length);
                }

                if(commandRequest->has_pid) {
                    request.has_pid = true;
                    request.pid = commandRequest->pid;
                }

                bool multipleResponses = commandRequest->message_id ==
                        OBD2_FUNCTIONAL_BROADCAST_ID;
                if(commandRequest->has_multiple_responses) {
                    multipleResponses = commandRequest->multiple_responses;
                }

                addRecurringRequest(manager, bus, &request,
                        commandRequest->has_name ?
                                commandRequest->name : NULL,
                        commandRequest->has_parse_payload ?
                                commandRequest->parse_payload : false,
                        commandRequest->has_factor ?
                                commandRequest->factor : 1.0,
                        commandRequest->has_offset ?
                                commandRequest->offset : 0,
                        commandRequest->has_frequency ?
                                commandRequest->frequency : 0,
                        multipleResponses);
            } else {
                debug("Raw CAN writes not allowed for bus %d", bus->address);
                status = false;
            }

        } else {
            debug("Diagnostic requests need at least a bus, arb. ID and mode");
            status = false;
        }
    } else {
        debug("Command was not a diagnostic request");
        status = false;
    }
    return status;
}
