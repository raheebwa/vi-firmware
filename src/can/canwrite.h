#ifndef _CANWRITE_H_
#define _CANWRITE_H_

#include "can/canutil.h"

using openxc::can::CanCommand;

namespace openxc {
namespace can {
namespace write {

uint64_t encodeSignal(CanSignal* signal, float value);

/* Public: Write the given number to the correct bitfield for the given signal.
 *
 * signal - The signal associated with the value.
 * signals - An array of all CAN signals.
 * signalCount - The size of the CAN signals array.
 * value - The value to write.
 * send - An output argument that will be set to false if the value should
 *     not be sent for any reason.
 *
 * TODO update docs for all of these functions.
 * Returns a 64-bit data block with the bit field for the signal set to the
 * encoded value.
 */
float numberEncoder(CanSignal* signal, CanSignal* signals,
        int signalCount, float value, bool* send);

/* Public: Interpret the value as a float, then do the same as
 * numberEncoder(CanSignal*, CanSignal*, int, float, bool*).
 */
float numberEncoder(CanSignal* signal, CanSignal* signals,
        int signalCount, openxc_DynamicField* value, bool* send);

/* Public: Convert the string value to the correct integer value for the given
 * CAN signal and write it to the signal's bitfield.
 *
 * Be aware that the behavior is undefined if there are multiple values assigned
 * to a single state. See https://github.com/openxc/vi-firmware/issues/185.
 *
 * signal - The signal associated with the value.
 * signals - An array of all CAN signals.
 * signalCount - The size of the CAN signals array.
 * value - The string object to write. The value should correspond to a signal
 *         state integer value.
 * send - An output argument that will be set to false if the value should
 *     not be sent for any reason.
 *
 * Returns a 64-bit data block with the bit field for the signal set to the
 * encoded value.
 */
float stateEncoder(CanSignal* signal, CanSignal* signals,
        int signalCount, const char* value, bool* send);

/* Public: Interpret the JSON value as a string, then do the same as
 * stateEncoder(CanSignal*, CanSignal*, int, const char*, bool*).
 */
float stateEncoder(CanSignal* signal, CanSignal* signals,
        int signalCount, openxc_DynamicField* value, bool* send);

/* Public: Write a CAN signal with the given value to the bus.
 *
 * Using the provided CanSignal and writer function, convert the value
 * into a numerical value appropriate for the CAN signal. This may include
 * converting a string state value to its numerical equivalent, for example. The
 * writer function must know how to do this conversion (and return a fully
 * filled out uint64_t).
 *
 * signal - The CanSignal to send.
 * value - The value to send in the signal. This could be a boolean, number or
 *         string (i.e. a state value).
 * writer - A function to convert from the value to an encoded uint64_t.
 * signals - An array of all CAN signals.
 * signalCount - The size of the signals array.
 * force - true if the signals should be sent regardless of the writable status
 *         in the CAN message structure.
 *
 * Returns true if the message was sent successfully.
 */
bool sendSignal(CanSignal* signal, openxc_DynamicField* value,
        SignalEncoder writer, CanSignal* signals, int signalCount, bool force);

/* Public: Write a CAN signal with the given value to the bus.
 *
 * Just like the above function sendSignal(), but the value of force defaults
 * to false.
 */
bool sendSignal(CanSignal* signal, openxc_DynamicField* value,
        SignalEncoder writer, CanSignal* signals, int signalCount);

/* Public: Write a CAN signal with the given value to the bus.
 *
 * Just like the above function sendSignal() that accepts a writer function,
 * but uses the CanSignal's value for "writeHandler" instead.
 *
 * See above for argument descriptions.
 */
bool sendSignal(CanSignal* signal, openxc_DynamicField* value,
        CanSignal* signals, int signalCount, bool force);

/* Public: Write a CAN signal with the given value to the bus.
 *
 * Just like the above function sendSignal(), but the value of force defaults
 * to false.
 */
bool sendSignal(CanSignal* signal, openxc_DynamicField* value,
        CanSignal* signals, int signalCount);

bool sendSignal(CanSignal* signal, float value, CanSignal* signals,
                int signalCount, bool force);

bool sendSignal(CanSignal* signal, float value, CanSignal* signals,
                int signalCount);

/* Public: The lowest-level API available to send a CAN message. The byte order
 * of the data is swapped, but otherwise this function queues the data to write
 * out to CAN without any additional processing.
 *
 * If the 'length' field of the CanMessage struct is 0, the message size is
 * assumed to be 8 (i.e. it will use the entire contents of the 'data' field, so
 * make sure it's all valid or zereod out!).
 *
 * bus - the bus to send the message.
 * message - the CAN message this data should be sent in. The byte order of the
 *      data will be reversed.
 */
void enqueueMessage(CanBus* bus, CanMessage* message);

/* Public: Write any queued outgoing messages to the CAN bus.
 *
 * bus - The CanBus instance that has a queued to be flushed out to CAN.
 */
void processWriteQueue(CanBus* bus);

/* Public: Write a CAN message with the given data and node ID to the bus
 * immeidately.
 *
 * You should usually use enqueueMessage, unless you absolutely need the message
 * written to the bus right now.
 *
 * bus - The CAN bus to send the message on.
 * request - the CanMessage message to send.
 *
 * Returns true if the message was sent successfully.
 */
bool sendCanMessage(const CanBus* bus, const CanMessage* request);

/* Private: Actually, finally write a CAN message with the given data and node
 * ID to the bus.
 *
 * Defined per-platform. Users should use enqueueMessage instead.
 *
 * bus - The CAN bus to send the message on.
 * request - the CanMessage message to send.
 *
 * Returns true if the message was sent successfully.
 */
bool sendMessage(const CanBus* bus, const CanMessage* request);

} // namespace write
} // namespace can
} // namespace openxc

#endif // _CANWRITE_H_
