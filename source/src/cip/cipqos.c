/*******************************************************************************
 * Copyright (c) 2009/, Rockwell Automation, Inc.
 * All rights reserved.
 *
 ******************************************************************************/

#include "cipqos.h"

#include "opener_user_conf.h"
#include "cipcommon.h"
#include "cipmessagerouter.h"
#include "ciperror.h"
#include "endianconv.h"
#include "cipethernetlink.h"
#include "opener_api.h"
#include "trace.h"

#define DEFAULT_DSCP_EVENT 59u
#define DEFAULT_DSCP_GENERAL 47u
#define DEFAULT_DSCP_URGENT 55u
#define DEFAULT_DSCP_SCHEDULED 47u
#define DEFAULT_DSCP_HIGH 43u
#define DEFAULT_DSCP_LOW 31u
#define DEFAULT_DSCP_EXPLICIT 27u

typedef struct cip_qos_dscp_values {
  CipUsint dscp_event; /**< DSCP value for event messages*/
  CipUsint dscp_general; /**< DSCP value for general messages*/
  CipUsint dscp_urgent; /**< DSCP value for CIP transport class 0/1 Urgent priority messages */
  CipUsint dscp_scheduled; /**< DSCP value for CIP transport class 0/1 Scheduled priority messages*/
  CipUsint dscp_high; /**< DSCP value for CIP transport class 0/1 High priority messages */
  CipUsint dscp_low; /**< DSCP value for CIP transport class 0/1 low priority messages */
  CipUsint dscp_explicit; /**< DSCP value for CIP explicit messages (transport class 2/3 and UCMM)
                                        and all other EtherNet/IP encapsulation messages */
} CipQosDscpValues;

typedef struct {
  CipUsint q_frames_enable; /**< Enables or disable sending 802.1Q frames on CIP and IEEE 1588 messages */
  CipQosDscpValues dscp_values; /**< Attribute set of DSCP values - beware! must not be the used set */
} CipQosInstanceAttributes;

static CipQosInstanceAttributes s_instance_1_attributes = {
  .q_frames_enable = false,
  .dscp_values.dscp_event = DEFAULT_DSCP_EVENT,
  .dscp_values.dscp_general = DEFAULT_DSCP_GENERAL,
  .dscp_values.dscp_urgent = DEFAULT_DSCP_URGENT,
  .dscp_values.dscp_scheduled = DEFAULT_DSCP_SCHEDULED,
  .dscp_values.dscp_high = DEFAULT_DSCP_HIGH,
  .dscp_values.dscp_low = DEFAULT_DSCP_LOW,
  .dscp_values.dscp_explicit = DEFAULT_DSCP_EXPLICIT
};

/** @brief Hidden copy of data that need to be frozen on boot-up
 *
 *  The QoS DSCP values can be changed from the EIP network but the changes should come
 *  into effect only after a restart. Values are initialized with the default values.
 *  Changes are activated via the Identity Reset function
 */
static CipQosDscpValues s_currently_used_dscp_values = {
  .dscp_event = DEFAULT_DSCP_EVENT,
  .dscp_general = DEFAULT_DSCP_GENERAL,
  .dscp_urgent = DEFAULT_DSCP_URGENT,
  .dscp_scheduled = DEFAULT_DSCP_SCHEDULED,
  .dscp_high = DEFAULT_DSCP_HIGH,
  .dscp_low = DEFAULT_DSCP_LOW,
  .dscp_explicit = DEFAULT_DSCP_EXPLICIT
};

/************** Functions ****************************************/
EipStatus GetAttributeSingleQoS(
  CipInstance *const RESTRICT instance,
  CipMessageRouterRequest *RESTRICT const message_router_request,
  CipMessageRouterResponse *RESTRICT const message_router_response,
  const struct sockaddr *originator_address,
  const int encapsulation_session) {

  return GetAttributeSingle(instance, message_router_request,
                            message_router_response, originator_address,
                            encapsulation_session);
}

EipStatus SetAttributeSingleQoS(
  CipInstance *instance,
  CipMessageRouterRequest *message_router_request,
  CipMessageRouterResponse *message_router_response,
  const struct sockaddr *originator_address,
  const int encapsulation_session) {

  CipAttributeStruct *attribute = GetCipAttribute(
    instance, message_router_request->request_path.attribute_number);
  (void) instance;   /*Suppress compiler warning */
  EipUint16 attribute_number =
    message_router_request->request_path.attribute_number;
  uint8_t set_bit_mask = (instance->cip_class->set_bit_mask[CalculateIndex(
                                                              attribute_number)
                          ]);

  if( NULL != attribute &&
      ( set_bit_mask & ( 1 << ( (attribute_number) % 8 ) ) ) ) {
    CipUint attribute_value_recieved = GetDintFromMessage(
      &(message_router_request->data) );

    if( !( (attribute_value_recieved <= 0) ||
           (attribute_value_recieved >= 63) ) ) {
      OPENER_TRACE_INFO(" setAttribute %d\n", attribute_number);

      if(NULL != attribute->data) {
        CipUsint *data = (CipUsint *) attribute->data;
        *(data) = attribute_value_recieved;

        message_router_response->general_status = kCipErrorSuccess;
      } else {
        message_router_response->general_status = kCipErrorNotEnoughData;
        OPENER_TRACE_INFO("CIP QoS not enough data\n");
      }
    } else {
      message_router_response->general_status = kCipErrorInvalidAttributeValue;
    }
  } else {
    /* we don't have this attribute */
    message_router_response->general_status = kCipErrorAttributeNotSupported;
  }

  message_router_response->size_of_additional_status = 0;
  message_router_response->data_length = 0;
  message_router_response->reply_service = (0x80
                                            | message_router_request->service);

  return kEipStatusOkSend;
}

CipUsint CipQosGetDscpPriority(ConnectionObjectPriority priority) {

  CipUsint priority_value = s_currently_used_dscp_values.dscp_explicit;
  switch (priority) {
    case kConnectionObjectPriorityLow:
      priority_value = s_currently_used_dscp_values.dscp_low;
      break;
    case kConnectionObjectPriorityHigh:
      priority_value = s_currently_used_dscp_values.dscp_high;
      break;
    case kConnectionObjectPriorityScheduled:
      priority_value = s_currently_used_dscp_values.dscp_scheduled;
      break;
    case kConnectionObjectPriorityUrgent:
      priority_value = s_currently_used_dscp_values.dscp_urgent;
      break;
    case kConnectionObjectPriorityExplicit: /* Fall-through wanted here */
    default:
      priority_value = s_currently_used_dscp_values.dscp_explicit;
      break;
  }
  return priority_value;
}

void InitializeCipQos(CipClass *class) {
}

EipStatus CipQoSInit() {

  CipClass *qos_class = NULL;

  if( ( qos_class = CreateCipClass(kCipQoSClassCode,
                                   0, /* # class attributes */
                                   7, /* # highest class attribute number */
                                   0, /* # class services */
                                   8, /* # instance attributes */
                                   8, /* # highest instance attribute number */
                                   2, /* # instance services */
                                   1, /* # instances */
                                   "Quality of Service",
                                   1, /* # class revision */
                                   &InitializeCipQos /* # function pointer for initialization */
                                   ) ) == 0 ) {

    return kEipStatusError;
  }

  CipInstance *instance = GetCipInstance(qos_class, 1); /* bind attributes to the instance #1 that was created above */

  InsertAttribute(instance,
                  1,
                  kCipUsint,
                  (void *) &s_instance_1_attributes.q_frames_enable,
                  kNotSetOrGetable);
  InsertAttribute(instance,
                  2,
                  kCipUsint,
                  (void *) &s_instance_1_attributes.dscp_values.dscp_event,
                  kNotSetOrGetable);
  InsertAttribute(instance,
                  3,
                  kCipUsint,
                  (void *) &s_instance_1_attributes.dscp_values.dscp_general,
                  kNotSetOrGetable);
  InsertAttribute(instance,
                  4,
                  kCipUsint,
                  (void *) &s_instance_1_attributes.dscp_values.dscp_urgent,
                  kGetableSingle | kSetable);
  InsertAttribute(instance,
                  5,
                  kCipUsint,
                  (void *) &s_instance_1_attributes.dscp_values.dscp_scheduled,
                  kGetableSingle | kSetable);
  InsertAttribute(instance,
                  6,
                  kCipUsint,
                  (void *) &s_instance_1_attributes.dscp_values.dscp_high,
                  kGetableSingle | kSetable);
  InsertAttribute(instance,
                  7,
                  kCipUsint,
                  (void *) &s_instance_1_attributes.dscp_values.dscp_low,
                  kGetableSingle | kSetable);
  InsertAttribute(instance,
                  8,
                  kCipUsint,
                  (void *) &s_instance_1_attributes.dscp_values.dscp_explicit,
                  kGetableSingle | kSetable);

  InsertService(qos_class, kGetAttributeSingle, &GetAttributeSingleQoS,
                "GetAttributeSingleQoS");
  InsertService(qos_class, kSetAttributeSingle, &SetAttributeSingleQoS,
                "SetAttributeSingleQoS");

  return kEipStatusOk;
}

void CipQosUpdateUsedSetQosValues() {
  s_currently_used_dscp_values = s_instance_1_attributes.dscp_values;
}

void CipQosResetAttributesToDefaultValues() {
  const CipQosDscpValues kDefaultValues = {
    .dscp_event = DEFAULT_DSCP_EVENT,
    .dscp_general = DEFAULT_DSCP_GENERAL,
    .dscp_urgent = DEFAULT_DSCP_URGENT,
    .dscp_scheduled = DEFAULT_DSCP_SCHEDULED,
    .dscp_high = DEFAULT_DSCP_HIGH,
    .dscp_low = DEFAULT_DSCP_LOW,
    .dscp_explicit = DEFAULT_DSCP_EXPLICIT
  };
  s_instance_1_attributes.dscp_values = kDefaultValues;
}
