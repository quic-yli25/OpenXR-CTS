// Copyright (c) 2019-2024, The Khronos Group Inc.
//
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "ConformanceHooks.h"
#include "CustomHandleState.h"
#include "HandleState.h"
#include "RuntimeFailure.h"

#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <openxr/openxr_reflection_parent_structs.h>
#include <atomic>
#include <cstdint>

namespace instance
{
    HandleState* GetInstanceState(XrInstance handle)
    {
        return GetHandleState({(IntHandle)handle, XR_OBJECT_TYPE_INSTANCE});
    }

}  // namespace instance

/////////////////
// ABI
/////////////////

XrResult ConformanceHooks::xrEnumerateViewConfigurations(XrInstance instance, XrSystemId systemId,
                                                         uint32_t viewConfigurationTypeCapacityInput,
                                                         uint32_t* viewConfigurationTypeCountOutput,
                                                         XrViewConfigurationType* viewConfigurationTypes)
{
    const XrResult result = ConformanceHooksBase::xrEnumerateViewConfigurations(instance, systemId, viewConfigurationTypeCapacityInput,
                                                                                viewConfigurationTypeCountOutput, viewConfigurationTypes);
    if (checkTwoCallIdiomFunc(__func__, result, viewConfigurationTypeCapacityInput, viewConfigurationTypeCountOutput,
                              viewConfigurationTypes)) {
        // We have some output to check
        for (uint32_t i = 0; i < *viewConfigurationTypeCountOutput; ++i) {
            VALIDATE_XRENUM(viewConfigurationTypes[i]);
        }
    }

    return result;
}

XrResult ConformanceHooks::xrEnumerateEnvironmentBlendModes(XrInstance instance, XrSystemId systemId,
                                                            XrViewConfigurationType viewConfigurationType,
                                                            uint32_t environmentBlendModeCapacityInput,
                                                            uint32_t* environmentBlendModeCountOutput,
                                                            XrEnvironmentBlendMode* environmentBlendModes)
{
    const XrResult result =
        ConformanceHooksBase::xrEnumerateEnvironmentBlendModes(instance, systemId, viewConfigurationType, environmentBlendModeCapacityInput,
                                                               environmentBlendModeCountOutput, environmentBlendModes);
    if (checkTwoCallIdiomFunc(__func__, result, environmentBlendModeCapacityInput, environmentBlendModeCountOutput,
                              environmentBlendModes)) {
        // We have some output to check
        for (uint32_t i = 0; i < *environmentBlendModeCountOutput; ++i) {
            VALIDATE_XRENUM(environmentBlendModes[i]);
        }
    }

    return result;
}

XrResult ConformanceHooks::xrPollEvent(XrInstance instance, XrEventDataBuffer* eventData)
{
    const XrResult result = ConformanceHooksBase::xrPollEvent(instance, eventData);

    if (result == XR_EVENT_UNAVAILABLE) {
        const HandleState* const instanceState = instance::GetInstanceState(instance);

        // Clear the "xrSyncActions called" flag for all known sessions
        std::unique_lock<std::recursive_mutex> lock(instanceState->childrenMutex);
        for (HandleState* childState : instanceState->children) {
            if (childState->type != XR_OBJECT_TYPE_SESSION) {
                continue;
            }
            session::CustomSessionState* const customSessionState =
                dynamic_cast<session::CustomSessionState*>(childState->GetCustomState());

            // avoid setting queue exhaust flag while xrSyncActions is ongoing
            // caveat: it is technically possible but unlikely that an entire xrSyncActions has happened
            // since this function forwarded the xrPollEvent call
            session::SyncActionsState exchangeIfState = session::SyncActionsState::CALLED_SINCE_QUEUE_EXHAUST;
            customSessionState->syncActionsState.compare_exchange_strong(exchangeIfState,
                                                                         session::SyncActionsState::NOT_CALLED_SINCE_QUEUE_EXHAUST,  //
#if __cplusplus >= 202000L
                                                                         std::memory_order_seq_cst, std::memory_order_seq_cst
#else
                                                                         std::memory_order::memory_order_seq_cst,
                                                                         std::memory_order::memory_order_seq_cst
#endif  // __cpluscplus >= 202000L
            );
        }
    }

    if (result != XR_SUCCESS) {
        // exit now if we don't have a good event
        return result;
    }

    // For each known event type, check if that's the current event type,
    // and if so, cast the event to the derived type then call checkEventPayload().
    // This will end up choosing an overload per event data type.

    // macro for a case statement when we have defined enough defines to get that type:
    // reinterpret cast then call to checkEventPayload
#define MAKE_CASE(STRUCT_TYPE, TYPE_ENUM)                                   \
    case TYPE_ENUM: {                                                       \
        const auto typed = reinterpret_cast<const STRUCT_TYPE*>(eventData); \
        checkEventPayload(typed);                                           \
        break;                                                              \
    }
    // macro for a case statement where a type is not available due to lack of defines, but the structure type enum still exists (needed by the reflection-generated macro)
#define MAKE_UNAVAIL_CASE(STRUCT_TYPE, TYPE_ENUM)                                                                            \
    case TYPE_ENUM: {                                                                                                        \
        POSSIBLE_NONCONFORMANT(                                                                                              \
            "Recognized event type: %d but support for this type not compiled in to conformance layer; could not verify it", \
            eventData->type);                                                                                                \
        break;                                                                                                               \
    }

    try {
        switch ((int)eventData->type) {  // int cast so compiler doesn't warn about other enumerants.

            // Use the reflection headers to generate all the case statements for things derived from XrEventDataBaseHeader
            // and call the appropriate checkEventPayload overload on those we can cope with (those whose needed defines are defined)
            XR_LIST_ALL_CHILD_STRUCTURE_TYPES_XrEventDataBaseHeader(MAKE_CASE, MAKE_UNAVAIL_CASE);

        default:
            POSSIBLE_NONCONFORMANT("Unsupported event type: %d", eventData->type);
            break;
        }
    }
    catch (const HandleException& ex) {  // Some event data struct has a handle value which is not tracked.
        NONCONFORMANT("Event type %s: %s", to_string(eventData->type), ex.what());
    }

    return result;
}

bool ConformanceHooks::checkTwoCallIdiomFunc(const char* function, XrResult result, uint32_t capacityInput, const uint32_t* countOutput,
                                             void* array)
{
    if (result != XR_SUCCESS) {
        return false;
    }
    if (countOutput == nullptr) {
        this->ConformanceFailure(XR_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT, function,
                                 "Must not return XR_SUCCESS for a null CountOutput");
        return false;
    }
    if (capacityInput == 0) {
        // just a capacity check
        return false;
    }
    if (array == nullptr) {
        this->ConformanceFailure(XR_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT, function,
                                 "Must not return success from a two call idiom call with non-zero capacity but null array");
        return false;
    }
    if (*countOutput > capacityInput) {
        this->ConformanceFailure(XR_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT, function,
                                 "Must not return success from a two call idiom call with non-zero capacity and larger count output.");
        // may read out of bounds if we validate all *countOutput so:
        return false;
    }
    // If we get here, it was successful and should have actually returned data.
    return true;
}

// Helpers
#define VALIDATE_EVENT_XRBOOL32(value) ValidateXrBool32(this, value, #value, "xrPollEvent")
#define VALIDATE_EVENT_FLOAT(value, min, max) ValidateFloat(this, value, min, max, #value, "xrPollEvent")
#define VALIDATE_EVENT_XRTIME(value) ValidateXrTime(this, value, #value, "xrPollEvent")
#define VALIDATE_EVENT_QUATERNION(value) ValidateXrQuaternion(this, value, #value, "xrPollEvent")
#define VALIDATE_EVENT_VECTOR3F(value) ValidateXrVector3f(this, value, #value, "xrPollEvent")
#define VALIDATE_EVENT_XRENUM(value) ValidateXrEnum(this, value, #value, "xrPollEvent")

void ConformanceHooks::checkEventPayload(const XrEventDataEventsLost* data)
{
    const auto eventsLost = reinterpret_cast<const XrEventDataEventsLost*>(data);
    NONCONFORMANT_IF(eventsLost->lostEventCount == 0, "lostEventCount must be > 0");
}

void ConformanceHooks::checkEventPayload(const XrEventDataInstanceLossPending* data)
{
    VALIDATE_EVENT_XRTIME(data->lossTime);
}

void ConformanceHooks::checkEventPayload(const XrEventDataSessionStateChanged* data)
{
    VALIDATE_EVENT_XRTIME(data->time);
    VALIDATE_EVENT_XRENUM(data->state);
    session::SessionStateChanged(this, data);
}

void ConformanceHooks::checkEventPayload(const XrEventDataReferenceSpaceChangePending* data)
{
    VALIDATE_EVENT_XRTIME(data->changeTime);
    VALIDATE_EVENT_QUATERNION(data->poseInPreviousSpace.orientation);
    VALIDATE_EVENT_XRBOOL32(data->poseValid);
    VALIDATE_EVENT_XRENUM(data->referenceSpaceType);
    (void)session::GetSessionState(data->session);  // Check handle is alive/valid.
}

void ConformanceHooks::checkEventPayload(const XrEventDataInteractionProfileChanged* data)
{
    session::InteractionProfileChanged(this, data);  // Validate session handle and timing of InteractionProfileChanged
}

void ConformanceHooks::checkEventPayload(const XrEventDataVisibilityMaskChangedKHR* data)
{
    session::VisibilityMaskChanged(this, data);  // Validate session handle and view data.
}

void ConformanceHooks::checkEventPayload(const XrEventDataPerfSettingsEXT* data)
{
    VALIDATE_EVENT_XRENUM(data->domain);
    VALIDATE_EVENT_XRENUM(data->subDomain);
    VALIDATE_EVENT_XRENUM(data->fromLevel);
    VALIDATE_EVENT_XRENUM(data->toLevel);
}

void ConformanceHooks::checkEventPayload(const XrEventDataSpatialAnchorCreateCompleteFB* data)
{
    (void)data;
    // Event data used in gen_dispatch.cpp
}

void ConformanceHooks::checkEventPayload(const XrEventDataUserPresenceChangedEXT* data)
{
    VALIDATE_EVENT_XRBOOL32(data->isUserPresent);
}

XrResult ConformanceHooks::xrGetSystemProperties(XrInstance instance, XrSystemId systemId, XrSystemProperties* properties)
{
    const XrResult result = ConformanceHooksBase::xrGetSystemProperties(instance, systemId, properties);

    if (result == XR_SUCCESS) {
        // validate some structs?
        ForEachExtension(properties->next, [&](const XrBaseInStructure* ext) {
            switch (ext->type) {
            case XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT: {
                auto testStruct = reinterpret_cast<const XrSystemHandTrackingPropertiesEXT*>(ext);
                // The runtime is only required to validate this if EXT_hand_tracking is enabled
                // but we don't supply invalid values for this in our tests so this is a
                // a reasonable thing to check here.
                VALIDATE_EVENT_XRBOOL32(testStruct->supportsHandTracking);
                break;
            }
            case XR_TYPE_SYSTEM_USER_PRESENCE_PROPERTIES_EXT: {
                // The runtime is only required to validate this if EXT_hand_tracking is enabled
                // but we don't supply invalidate values for this in our tests so this is a
                // a reasonable thing to check here.

                auto testStruct = reinterpret_cast<const XrSystemUserPresencePropertiesEXT*>(ext);
                // The runtime is only required to validate this if EXT_user_presence is enabled
                // but we don't supply invalid values for this in the tests so this is a
                // a reasonable thing to check here.
                VALIDATE_EVENT_XRBOOL32(testStruct->supportsUserPresence);
                break;
            }
            default:
                break;
            }
        });
    }

    return result;
}
