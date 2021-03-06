/*
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 * responder.c
 * Copyright (C) 2015 Simon Newton
 */

#include "responder.h"

#include <stdlib.h>

#include "constants.h"
#include "dmx_spec.h"
#include "rdm_frame.h"
#include "rdm_handler.h"
#include "receiver_counters.h"
#include "rdm_util.h"
#include "spi_rgb.h"
#include "syslog.h"
#include "transceiver.h"
#include "utils.h"

/*
 * @brief The g_state machine for decoding RS-485 data.
 *
 * Each g_state is named after the slot we're waiting for, i.e. STATE_START_CODE
 * means we're waiting to receive the first byte.
 */
typedef enum {
  STATE_START_CODE,  //!< Waiting for the start code
  STATE_DMX_DATA,  //!< Receiving DMX512 data
  STATE_RDM_SUB_START_CODE,  //!< Waiting for the RDM sub-start-code
  STATE_RDM_MESSAGE_LENGTH,  //!< Waiting for the RDM message length
  STATE_RDM_BODY,  //!< Receiving the RDM frame data.
  STATE_RDM_CHECKSUM_LO,  //!< Waiting for the low byte of the RDM checksum
  STATE_RDM_CHECKSUM_HI,  //!< Waiting for the high byte of the RDM checksum
  STATE_RDM_POST_CHECKSUM,  //!< After the last checksum byte.

  STATE_DISCARD  //!< Discarding the remaining data.
} ResponderState;

static const uint16_t UNINITIALIZED_COUNTER = 0xffffu;

/*
 * @brief The timing information for the current frame.
 */
static TransceiverTiming g_timing;

/*
 * @brief The current g_state
 */
static ResponderState g_state = STATE_START_CODE;

/*
 * @brief The g_offset of the next byte to process
 */
static unsigned int g_offset = 0u;

/*
 * @brief Call the RDM handler when we have a complete and valid frame.
 */
static inline void DispatchRDMRequest(const uint8_t *frame) {
  RDMHeader *header = (RDMHeader*) frame;
  RDMHandler_HandleRequest(
      header,
      header->param_data_length ? frame + RDM_PARAM_DATA_OFFSET : NULL);
  SysLog_Print(
      SYSLOG_INFO,
      "RDM: break %dus, mark %dus, TN %d CC 0x%x, PID 0x%x, PDL %d",
      g_timing.request.break_time / 10u,
      g_timing.request.mark_time / 10u,
      header->transaction_number,
      header->command_class,
      ntohs(header->param_id),
      header->param_data_length);
}

/*
 * @brief Increment the bad-checksum counter if the frame was for us.
 */
static inline void PossiblyIncrementChecksumCounter(const uint8_t *frame) {
  uint8_t uid[UID_LENGTH];
  RDMHandler_GetUID(uid);
  RDMHeader *header = (RDMHeader*) frame;
  if (RDMUtil_RequiresAction(uid, header->dest_uid)) {
    SysLog_Message(SYSLOG_ERROR, "Checksum mismatch");
    g_responder_counters.rdm_checksum_invalid++;
  }
}

/*
 * @brief Increment the length-mismatch counter if the frame was for us.
 */
static inline void PossiblyIncrementLengthMismatchCounter(
    const uint8_t *frame) {
  uint8_t uid[UID_LENGTH];
  RDMHandler_GetUID(uid);
  RDMHeader *header = (RDMHeader*) frame;
  if (RDMUtil_RequiresAction(uid, header->dest_uid)) {
    g_responder_counters.rdm_length_mismatch++;
  }
}

// Public Functions
// ----------------------------------------------------------------------------
void Responder_Initialize() {}

void Responder_Receive(const TransceiverEvent *event) {
  // While this function is running, UART interrupts are disabled.
  // Try to keep things short.
  if (event->op != T_OP_RX) {
    return;
  }

  if (event->result == T_RESULT_RX_START_FRAME) {
    // Right now we can only tell a DMX frame ended when the next one starts.
    // TODO(simon): get some clarity on this. It needs to be discussed and
    // explained in E1.37-5.
    if (g_state == STATE_DMX_DATA &&
        (g_responder_counters.dmx_min_slot_count == UNINITIALIZED_COUNTER ||
         g_responder_counters.dmx_last_slot_count <
         g_responder_counters.dmx_min_slot_count)) {
      g_responder_counters.dmx_min_slot_count =
        g_responder_counters.dmx_last_slot_count;
    }
    if (g_state == STATE_RDM_SUB_START_CODE ||
        g_state == STATE_RDM_MESSAGE_LENGTH ||
        (g_state == STATE_RDM_BODY && g_offset < 9)) {
      // COMMS_STATUS, short frame
      g_responder_counters.rdm_short_frame++;
    }

    g_offset = 0u;
    g_state = STATE_START_CODE;
    if (event->timing) {
      g_timing = *event->timing;
    }
  }

  if (event->result == T_RESULT_RX_FRAME_TIMEOUT) {
    SPIRGB_CompleteUpdate();
    return;
  }

  for (; g_offset < event->length; g_offset++) {
    uint8_t b = event->data[g_offset];
    switch (g_state) {
      case STATE_START_CODE:
        if (b == NULL_START_CODE) {
          g_responder_counters.dmx_last_checksum = 0u;
          g_responder_counters.dmx_last_slot_count = 0u;
          SysLog_Message(SYSLOG_DEBUG, "DMX frame");
          g_responder_counters.dmx_frames++;
          g_state = STATE_DMX_DATA;
          SPIRGB_BeginUpdate();
        } else if (b == RDM_START_CODE) {
          g_responder_counters.rdm_frames++;
          g_state = STATE_RDM_SUB_START_CODE;
        } else {
          SysLog_Print(SYSLOG_DEBUG, "ASC frame: %d", (int) b);
          g_responder_counters.asc_frames++;
          g_state = STATE_DISCARD;
        }
        break;
      case STATE_RDM_SUB_START_CODE:
        if (b != RDM_SUB_START_CODE) {
          SysLog_Print(SYSLOG_ERROR, "RDM sub-start-code mismatch: %d",
                       (int) b);
          g_responder_counters.rdm_sub_start_code_invalid++;
          g_state = STATE_DISCARD;
        } else {
          g_state = STATE_RDM_MESSAGE_LENGTH;
        }
        break;
      case STATE_RDM_MESSAGE_LENGTH:
        if (b < sizeof(RDMHeader)) {
          SysLog_Print(SYSLOG_INFO, "RDM msg len too short: %d", (int) b);
          g_responder_counters.rdm_msg_len_invalid++;
          g_state = STATE_DISCARD;
        } else {
          g_state = STATE_RDM_BODY;
        }
        break;
      // data[2] is at least 24
      case STATE_RDM_BODY:
        if (g_offset == RDM_PARAM_DATA_LENGTH_OFFSET) {
          if (b != event->data[MESSAGE_LENGTH_OFFSET] - sizeof(RDMHeader)) {
            SysLog_Print(SYSLOG_INFO, "Invalid RDM PDL: %d, msg len: %d",
                         (int) b, event->data[MESSAGE_LENGTH_OFFSET]);
            g_state = STATE_DISCARD;
            g_responder_counters.rdm_param_data_len_invalid++;
            continue;
          }
        }
        if (g_offset + 1u == event->data[MESSAGE_LENGTH_OFFSET]) {
          g_state = STATE_RDM_CHECKSUM_LO;
        }
        break;
      case STATE_RDM_CHECKSUM_LO:
        g_state = STATE_RDM_CHECKSUM_HI;
        break;
      case STATE_RDM_CHECKSUM_HI:
        if (RDMUtil_VerifyChecksum(event->data, event->length)) {
          DispatchRDMRequest(event->data);
        } else {
          PossiblyIncrementChecksumCounter(event->data);
        }
        g_state = STATE_RDM_POST_CHECKSUM;
        break;
      case STATE_RDM_POST_CHECKSUM:
        PossiblyIncrementLengthMismatchCounter(event->data);
        g_state = STATE_DISCARD;
        break;
      case STATE_DMX_DATA:
        // TODO(simon): configure this with DMX_START_ADDRESS and footprints.
        if (g_offset - 1u < 6u) {
          SPIRGB_SetPixel((g_offset - 1u) / 3u, (g_offset - 1u) % 3u, b);
        } else if (g_offset - 1u == 6u) {
          SPIRGB_CompleteUpdate();
        }

        g_responder_counters.dmx_last_checksum += b;
        g_responder_counters.dmx_last_slot_count++;
        if (g_responder_counters.dmx_max_slot_count == UNINITIALIZED_COUNTER ||
           g_responder_counters.dmx_last_slot_count >
             g_responder_counters.dmx_max_slot_count) {
          g_responder_counters.dmx_max_slot_count =
            g_responder_counters.dmx_last_slot_count;
        }
        break;
      case STATE_DISCARD:
        break;
    }
  }
}
