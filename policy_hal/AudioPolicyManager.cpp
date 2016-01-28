/*
 * Copyright (c) 2013-2015, The Linux Foundation. All rights reserved.
 * Not a contribution.
 *
 * Copyright (C) 2009 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "AudioPolicyManagerCustom"
//#define LOG_NDEBUG 0

//#define VERY_VERBOSE_LOGGING
#ifdef VERY_VERBOSE_LOGGING
#define ALOGVV ALOGV
#else
#define ALOGVV(a...) do { } while(0)
#endif

#include <inttypes.h>
#include <math.h>

#include <cutils/properties.h>
#include <utils/Log.h>
#include <hardware/audio.h>
#include <hardware/audio_effect.h>
#include <media/AudioParameter.h>
#include <media/AudioPolicyHelper.h>
#include <soundtrigger/SoundTrigger.h>
#include <policy.h>
#include "AudioPolicyManager.h"

#ifdef DRIVER_SIDE_PLAYBACK_ENABLED
#define AUDIO_OUTPUT_FLAG_DRIVER_SIDE 0x10000
#endif

namespace android {

// ----------------------------------------------------------------------------
// AudioPolicyInterface implementation
// ----------------------------------------------------------------------------

AudioPolicyManagerCustom::AudioPolicyManagerCustom(AudioPolicyClientInterface *clientInterface)
    :AudioPolicyManager(clientInterface)
{
#ifdef DRIVER_SIDE_PLAYBACK_ENABLED
    audio_module_handle_t moduleHandle;
    audio_config_t config;
    audio_io_handle_t handle = AUDIO_IO_HANDLE_NONE;
    status_t status;

    if (mPrimaryOutput != 0) {
        mDriverSideProfile = new IOProfile(String8("driver-side"), AUDIO_PORT_ROLE_SOURCE);
        mDriverSideProfile->attach(mPrimaryOutput->mPort->mModule);
        mDriverSideProfile->mSamplingRates.add(48000);
        mDriverSideProfile->mFormats.add(AUDIO_FORMAT_PCM_16_BIT);
        mDriverSideProfile->mChannelMasks.add(AUDIO_CHANNEL_OUT_STEREO);
        mDriverSideProfile->mSupportedDevices.add(mDefaultOutputDevice);
        mDriverSideProfile->mFlags = (audio_output_flags_t)(AUDIO_OUTPUT_FLAG_FAST|AUDIO_OUTPUT_FLAG_DRIVER_SIDE);
        mPrimaryOutput->mPort->mModule->mOutputProfiles.add(mDriverSideProfile);

        moduleHandle = mPrimaryOutput->getModuleHandle();

        mDriverSideOutput = new SwAudioOutputDescriptor(mDriverSideProfile,
                                                        mpClientInterface);
        mDriverSideOutput->mDevice = AUDIO_DEVICE_OUT_SPEAKER;

        config = AUDIO_CONFIG_INITIALIZER;
        config.sample_rate = 48000;
        config.channel_mask = AUDIO_CHANNEL_OUT_STEREO;
        config.format = AUDIO_FORMAT_PCM_16_BIT;

        status = mpClientInterface->openOutput(moduleHandle,
                                               &handle,
                                               &config,
                                               &mDriverSideOutput->mDevice,
                                               String8(""),
                                               &mDriverSideOutput->mLatency,
                                               mDriverSideOutput->mFlags);

        if (status != NO_ERROR) {
            ALOGE("Cannot open driver side output stream.");
        } else {
            mDriverSideOutput->mSamplingRate = config.sample_rate;
            mDriverSideOutput->mChannelMask = config.channel_mask;
            mDriverSideOutput->mFormat = config.format;

            addOutput(handle, mDriverSideOutput);
            setOutputDevice(mDriverSideOutput,
                            mDriverSideOutput->mDevice,
                            true);
        }
    }
#endif
    // set the volume index for music playback
    setStreamVolumeIndex(AUDIO_STREAM_MUSIC, 1, AUDIO_DEVICE_OUT_SPEAKER);
}

status_t AudioPolicyManagerCustom::getOutputForAttr(const audio_attributes_t *attr,
                                              audio_io_handle_t *output,
                                              audio_session_t session,
                                              audio_stream_type_t *stream,
                                              uid_t uid,
                                              uint32_t samplingRate,
                                              audio_format_t format,
                                              audio_channel_mask_t channelMask,
                                              audio_output_flags_t flags,
                                              audio_port_handle_t selectedDeviceId,
                                              const audio_offload_info_t *offloadInfo)
{
    audio_attributes_t attributes;
    if (attr != NULL) {
        if (!isValidAttributes(attr)) {
            ALOGE("getOutputForAttr() invalid attributes: usage=%d content=%d flags=0x%x tags=[%s]",
                  attr->usage, attr->content_type, attr->flags,
                  attr->tags);
            return BAD_VALUE;
        }
        attributes = *attr;
    } else {
        if (*stream < AUDIO_STREAM_MIN || *stream >= AUDIO_STREAM_PUBLIC_CNT) {
            ALOGE("getOutputForAttr():  invalid stream type");
            return BAD_VALUE;
        }
        stream_type_to_audio_attributes(*stream, &attributes);
    }
    sp<SwAudioOutputDescriptor> desc;
    if (mPolicyMixes.getOutputForAttr(attributes, desc) == NO_ERROR) {
        ALOG_ASSERT(desc != 0, "Invalid desc returned by getOutputForAttr");
        if (!audio_is_linear_pcm(format)) {
            return BAD_VALUE;
        }
        *stream = streamTypefromAttributesInt(&attributes);
        *output = desc->mIoHandle;
        ALOGV("getOutputForAttr() returns output %d", *output);
        return NO_ERROR;
    }
    if (attributes.usage == AUDIO_USAGE_VIRTUAL_SOURCE) {
        ALOGW("getOutputForAttr() no policy mix found for usage AUDIO_USAGE_VIRTUAL_SOURCE");
        return BAD_VALUE;
    }

    ALOGV("getOutputForAttr() usage=%d, content=%d, tag=%s flags=%08x"
            " session %d selectedDeviceId %d",
            attributes.usage, attributes.content_type, attributes.tags, attributes.flags,
            session, selectedDeviceId);

    *stream = streamTypefromAttributesInt(&attributes);

    // Explicit routing?
    sp<DeviceDescriptor> deviceDesc;
    for (size_t i = 0; i < mAvailableOutputDevices.size(); i++) {
        if (mAvailableOutputDevices[i]->getId() == selectedDeviceId) {
            deviceDesc = mAvailableOutputDevices[i];
            break;
        }
    }
    mOutputRoutes.addRoute(session, *stream, SessionRoute::SOURCE_TYPE_NA, deviceDesc, uid);

    routing_strategy strategy = (routing_strategy) getStrategyForAttr(&attributes);
    audio_devices_t device = getDeviceForStrategy(strategy, false /*fromCache*/);

    if ((attributes.flags & AUDIO_FLAG_HW_AV_SYNC) != 0) {
        flags = (audio_output_flags_t)(flags | AUDIO_OUTPUT_FLAG_HW_AV_SYNC);
    }

#ifdef DRIVER_SIDE_PLAYBACK_ENABLED
    if (attributes.usage == AUDIO_USAGE_ASSISTANCE_NAVIGATION_GUIDANCE) {
        flags = (audio_output_flags_t)(AUDIO_OUTPUT_FLAG_FAST | AUDIO_OUTPUT_FLAG_DRIVER_SIDE);
    }
#endif

    ALOGV("getOutputForAttr() device 0x%x, samplingRate %d, format %x, channelMask %x, flags %x",
          device, samplingRate, format, channelMask, flags);

    *output = getOutputForDevice(device, session, *stream,
                                 samplingRate, format, channelMask,
                                 flags, offloadInfo);
    if (*output == AUDIO_IO_HANDLE_NONE) {
        mOutputRoutes.removeRoute(session);
        return INVALID_OPERATION;
    }

    return NO_ERROR;
}

audio_io_handle_t AudioPolicyManagerCustom::getOutputForDevice(
        audio_devices_t device,
        audio_session_t session __unused,
        audio_stream_type_t stream,
        uint32_t samplingRate,
        audio_format_t format,
        audio_channel_mask_t channelMask,
        audio_output_flags_t flags,
        const audio_offload_info_t *offloadInfo)
{
    audio_io_handle_t output = AUDIO_IO_HANDLE_NONE;
    uint32_t latency = 0;
    status_t status;

#ifdef AUDIO_POLICY_TEST
    if (mCurOutput != 0) {
        ALOGV("getOutput() test output mCurOutput %d, samplingRate %d, format %d, channelMask %x, mDirectOutput %d",
                mCurOutput, mTestSamplingRate, mTestFormat, mTestChannels, mDirectOutput);

        if (mTestOutputs[mCurOutput] == 0) {
            ALOGV("getOutput() opening test output");
            sp<AudioOutputDescriptor> outputDesc = new SwAudioOutputDescriptor(NULL,
                                                                               mpClientInterface);
            outputDesc->mDevice = mTestDevice;
            outputDesc->mLatency = mTestLatencyMs;
            outputDesc->mFlags =
                    (audio_output_flags_t)(mDirectOutput ? AUDIO_OUTPUT_FLAG_DIRECT : 0);
            outputDesc->mRefCount[stream] = 0;
            audio_config_t config = AUDIO_CONFIG_INITIALIZER;
            config.sample_rate = mTestSamplingRate;
            config.channel_mask = mTestChannels;
            config.format = mTestFormat;
            if (offloadInfo != NULL) {
                config.offload_info = *offloadInfo;
            }
            status = mpClientInterface->openOutput(0,
                                                  &mTestOutputs[mCurOutput],
                                                  &config,
                                                  &outputDesc->mDevice,
                                                  String8(""),
                                                  &outputDesc->mLatency,
                                                  outputDesc->mFlags);
            if (status == NO_ERROR) {
                outputDesc->mSamplingRate = config.sample_rate;
                outputDesc->mFormat = config.format;
                outputDesc->mChannelMask = config.channel_mask;
                AudioParameter outputCmd = AudioParameter();
                outputCmd.addInt(String8("set_id"),mCurOutput);
                mpClientInterface->setParameters(mTestOutputs[mCurOutput],outputCmd.toString());
                addOutput(mTestOutputs[mCurOutput], outputDesc);
            }
        }
        return mTestOutputs[mCurOutput];
    }
#endif //AUDIO_POLICY_TEST

    // open a direct output if required by specified parameters
    //force direct flag if offload flag is set: offloading implies a direct output stream
    // and all common behaviors are driven by checking only the direct flag
    // this should normally be set appropriately in the policy configuration file
    if ((flags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) != 0) {
        flags = (audio_output_flags_t)(flags | AUDIO_OUTPUT_FLAG_DIRECT);
    }
    if ((flags & AUDIO_OUTPUT_FLAG_HW_AV_SYNC) != 0) {
        flags = (audio_output_flags_t)(flags | AUDIO_OUTPUT_FLAG_DIRECT);
    }
    // only allow deep buffering for music stream type
    if (stream != AUDIO_STREAM_MUSIC) {
        flags = (audio_output_flags_t)(flags &~AUDIO_OUTPUT_FLAG_DEEP_BUFFER);
    } else if (/* stream == AUDIO_STREAM_MUSIC && */
            flags == AUDIO_OUTPUT_FLAG_NONE &&
            property_get_bool("audio.deep_buffer.media", false /* default_value */)) {
        // use DEEP_BUFFER as default output for music stream type
        flags = (audio_output_flags_t)AUDIO_OUTPUT_FLAG_DEEP_BUFFER;
    }
    if (stream == AUDIO_STREAM_TTS) {
        flags = AUDIO_OUTPUT_FLAG_TTS;
    }

    sp<IOProfile> profile;

    // skip direct output selection if the request can obviously be attached to a mixed output
    // and not explicitly requested
    if (((flags & AUDIO_OUTPUT_FLAG_DIRECT) == 0) &&
            audio_is_linear_pcm(format) && samplingRate <= MAX_MIXER_SAMPLING_RATE &&
            audio_channel_count_from_out_mask(channelMask) <= 2) {
        goto non_direct_output;
    }

    // Do not allow offloading if one non offloadable effect is enabled. This prevents from
    // creating an offloaded track and tearing it down immediately after start when audioflinger
    // detects there is an active non offloadable effect.
    // FIXME: We should check the audio session here but we do not have it in this context.
    // This may prevent offloading in rare situations where effects are left active by apps
    // in the background.

    if (((flags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) == 0) ||
            !mEffects.isNonOffloadableEffectEnabled()) {
        profile = getProfileForDirectOutput(device,
                                           samplingRate,
                                           format,
                                           channelMask,
                                           (audio_output_flags_t)flags);
    }

    if (profile != 0) {
        sp<SwAudioOutputDescriptor> outputDesc = NULL;

        for (size_t i = 0; i < mOutputs.size(); i++) {
            sp<SwAudioOutputDescriptor> desc = mOutputs.valueAt(i);
            if (!desc->isDuplicated() && (profile == desc->mProfile)) {
                outputDesc = desc;
                // reuse direct output if currently open and configured with same parameters
                if ((samplingRate == outputDesc->mSamplingRate) &&
                        (format == outputDesc->mFormat) &&
                        (channelMask == outputDesc->mChannelMask)) {
                    outputDesc->mDirectOpenCount++;
                    ALOGV("getOutput() reusing direct output %d", mOutputs.keyAt(i));
                    return mOutputs.keyAt(i);
                }
            }
        }
        // close direct output if currently open and configured with different parameters
        if (outputDesc != NULL) {
            closeOutput(outputDesc->mIoHandle);
        }

        // if the selected profile is offloaded and no offload info was specified,
        // create a default one
        audio_offload_info_t defaultOffloadInfo = AUDIO_INFO_INITIALIZER;
        if ((profile->mFlags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD) && !offloadInfo) {
            flags = (audio_output_flags_t)(flags | AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD);
            defaultOffloadInfo.sample_rate = samplingRate;
            defaultOffloadInfo.channel_mask = channelMask;
            defaultOffloadInfo.format = format;
            defaultOffloadInfo.stream_type = stream;
            defaultOffloadInfo.bit_rate = 0;
            defaultOffloadInfo.duration_us = -1;
            defaultOffloadInfo.has_video = true; // conservative
            defaultOffloadInfo.is_streaming = true; // likely
            offloadInfo = &defaultOffloadInfo;
        }

        outputDesc = new SwAudioOutputDescriptor(profile, mpClientInterface);
        outputDesc->mDevice = device;
        outputDesc->mLatency = 0;
        outputDesc->mFlags = (audio_output_flags_t)(outputDesc->mFlags | flags);
        audio_config_t config = AUDIO_CONFIG_INITIALIZER;
        config.sample_rate = samplingRate;
        config.channel_mask = channelMask;
        config.format = format;
        if (offloadInfo != NULL) {
            config.offload_info = *offloadInfo;
        }
        status = mpClientInterface->openOutput(profile->getModuleHandle(),
                                               &output,
                                               &config,
                                               &outputDesc->mDevice,
                                               String8(""),
                                               &outputDesc->mLatency,
                                               outputDesc->mFlags);

        // only accept an output with the requested parameters
        if (status != NO_ERROR ||
            (samplingRate != 0 && samplingRate != config.sample_rate) ||
            (format != AUDIO_FORMAT_DEFAULT && format != config.format) ||
            (channelMask != 0 && channelMask != config.channel_mask)) {
            ALOGV("getOutput() failed opening direct output: output %d samplingRate %d %d,"
                    "format %d %d, channelMask %04x %04x", output, samplingRate,
                    outputDesc->mSamplingRate, format, outputDesc->mFormat, channelMask,
                    outputDesc->mChannelMask);
            if (output != AUDIO_IO_HANDLE_NONE) {
                mpClientInterface->closeOutput(output);
            }
            // fall back to mixer output if possible when the direct output could not be open
            if (audio_is_linear_pcm(format) && samplingRate <= MAX_MIXER_SAMPLING_RATE) {
                goto non_direct_output;
            }
            return AUDIO_IO_HANDLE_NONE;
        }
        outputDesc->mSamplingRate = config.sample_rate;
        outputDesc->mChannelMask = config.channel_mask;
        outputDesc->mFormat = config.format;
        outputDesc->mRefCount[stream] = 0;
        outputDesc->mStopTime[stream] = 0;
        outputDesc->mDirectOpenCount = 1;

        audio_io_handle_t srcOutput = getOutputForEffect();
        addOutput(output, outputDesc);
        audio_io_handle_t dstOutput = getOutputForEffect();
        if (dstOutput == output) {
            mpClientInterface->moveEffects(AUDIO_SESSION_OUTPUT_MIX, srcOutput, dstOutput);
        }
        mPreviousOutputs = mOutputs;
        ALOGV("getOutput() returns new direct output %d", output);
        mpClientInterface->onAudioPortListUpdate();
        return output;
    }

non_direct_output:
    // ignoring channel mask due to downmix capability in mixer

    // open a non direct output

    // for non direct outputs, only PCM is supported
    if (audio_is_linear_pcm(format)) {
        // get which output is suitable for the specified stream. The actual
        // routing change will happen when startOutput() will be called
        SortedVector<audio_io_handle_t> outputs = getOutputsForDevice(device, mOutputs);

        // at this stage we should ignore the DIRECT flag as no direct output could be found earlier
        flags = (audio_output_flags_t)(flags & ~AUDIO_OUTPUT_FLAG_DIRECT);
        output = selectOutput(outputs, flags, format);
    }
    ALOGW_IF((output == 0), "getOutput() could not find output for stream %d, samplingRate %d,"
            "format %d, channels %x, flags %x", stream, samplingRate, format, channelMask, flags);

    ALOGV("  getOutputForDevice() returns output %d", output);

    return output;
}

audio_stream_type_t AudioPolicyManagerCustom::streamTypefromAttributesInt(const audio_attributes_t *attr)
{
    // flags to stream type mapping
    if ((attr->flags & AUDIO_FLAG_AUDIBILITY_ENFORCED) == AUDIO_FLAG_AUDIBILITY_ENFORCED) {
        return AUDIO_STREAM_ENFORCED_AUDIBLE;
    }
    if ((attr->flags & AUDIO_FLAG_SCO) == AUDIO_FLAG_SCO) {
        return AUDIO_STREAM_BLUETOOTH_SCO;
    }
    if ((attr->flags & AUDIO_FLAG_BEACON) == AUDIO_FLAG_BEACON) {
        return AUDIO_STREAM_TTS;
    }

    // usage to stream type mapping
    switch (attr->usage) {
    case AUDIO_USAGE_MEDIA:
    case AUDIO_USAGE_GAME:
    case AUDIO_USAGE_ASSISTANCE_NAVIGATION_GUIDANCE:
        return AUDIO_STREAM_MUSIC;
    case AUDIO_USAGE_ASSISTANCE_ACCESSIBILITY:
        if (isStreamActive(AUDIO_STREAM_ALARM)) {
            return AUDIO_STREAM_ALARM;
        }
        if (isStreamActive(AUDIO_STREAM_RING)) {
            return AUDIO_STREAM_RING;
        }
        if (isInCall()) {
            return AUDIO_STREAM_VOICE_CALL;
        }
        return AUDIO_STREAM_ACCESSIBILITY;
    case AUDIO_USAGE_ASSISTANCE_SONIFICATION:
        return AUDIO_STREAM_SYSTEM;
    case AUDIO_USAGE_VOICE_COMMUNICATION:
        return AUDIO_STREAM_VOICE_CALL;

    case AUDIO_USAGE_VOICE_COMMUNICATION_SIGNALLING:
        return AUDIO_STREAM_DTMF;

    case AUDIO_USAGE_ALARM:
        return AUDIO_STREAM_ALARM;
    case AUDIO_USAGE_NOTIFICATION_TELEPHONY_RINGTONE:
        return AUDIO_STREAM_RING;

    case AUDIO_USAGE_NOTIFICATION:
    case AUDIO_USAGE_NOTIFICATION_COMMUNICATION_REQUEST:
    case AUDIO_USAGE_NOTIFICATION_COMMUNICATION_INSTANT:
    case AUDIO_USAGE_NOTIFICATION_COMMUNICATION_DELAYED:
    case AUDIO_USAGE_NOTIFICATION_EVENT:
        return AUDIO_STREAM_NOTIFICATION;

    case AUDIO_USAGE_UNKNOWN:
    default:
        return AUDIO_STREAM_MUSIC;
    }
}

bool AudioPolicyManagerCustom::isValidAttributes(const audio_attributes_t *paa)
{
    // has flags that map to a strategy?
    if ((paa->flags & (AUDIO_FLAG_AUDIBILITY_ENFORCED | AUDIO_FLAG_SCO | AUDIO_FLAG_BEACON)) != 0) {
        return true;
    }

    // has known usage?
    switch (paa->usage) {
    case AUDIO_USAGE_UNKNOWN:
    case AUDIO_USAGE_MEDIA:
    case AUDIO_USAGE_VOICE_COMMUNICATION:
    case AUDIO_USAGE_VOICE_COMMUNICATION_SIGNALLING:
    case AUDIO_USAGE_ALARM:
    case AUDIO_USAGE_NOTIFICATION:
    case AUDIO_USAGE_NOTIFICATION_TELEPHONY_RINGTONE:
    case AUDIO_USAGE_NOTIFICATION_COMMUNICATION_REQUEST:
    case AUDIO_USAGE_NOTIFICATION_COMMUNICATION_INSTANT:
    case AUDIO_USAGE_NOTIFICATION_COMMUNICATION_DELAYED:
    case AUDIO_USAGE_NOTIFICATION_EVENT:
    case AUDIO_USAGE_ASSISTANCE_ACCESSIBILITY:
    case AUDIO_USAGE_ASSISTANCE_NAVIGATION_GUIDANCE:
    case AUDIO_USAGE_ASSISTANCE_SONIFICATION:
    case AUDIO_USAGE_GAME:
    case AUDIO_USAGE_VIRTUAL_SOURCE:
        break;
    default:
        return false;
    }
    return true;
}

void AudioPolicyManagerCustom::setPhoneState(audio_mode_t state)
{
    ALOGV("setPhoneState() state %d", state);
    // store previous phone state for management of sonification strategy below
    int oldState = mEngine->getPhoneState();

    if (mEngine->setPhoneState(state) != NO_ERROR) {
        ALOGW("setPhoneState() invalid or same state %d", state);
        return;
    }
    /// Opens: can these line be executed after the switch of volume curves???
    // if leaving call state, handle special case of active streams
    // pertaining to sonification strategy see handleIncallSonification()
    if (isStateInCall(oldState)) {
        ALOGV("setPhoneState() in call state management: new state is %d", state);
        for (int stream = 0; stream < AUDIO_STREAM_CNT; stream++) {
            if (stream == AUDIO_STREAM_PATCH) {
                continue;
            }
            handleIncallSonification((audio_stream_type_t)stream, false, true);
        }

        // force reevaluating accessibility routing when call stops
        mpClientInterface->invalidateStream(AUDIO_STREAM_ACCESSIBILITY);
    }

    /**
     * Switching to or from incall state or switching between telephony and VoIP lead to force
     * routing command.
     */
    bool force = ((is_state_in_call(oldState) != is_state_in_call(state))
                  || (is_state_in_call(state) && (state != oldState)));

    // check for device and output changes triggered by new phone state
    checkA2dpSuspend();
    checkOutputForAllStrategies();
    updateDevicesAndOutputs();

    int delayMs = 0;
    if (isStateInCall(state)) {
        nsecs_t sysTime = systemTime();
        for (size_t i = 0; i < mOutputs.size(); i++) {
            sp<SwAudioOutputDescriptor> desc = mOutputs.valueAt(i);
            // mute media and sonification strategies and delay device switch by the largest
            // latency of any output where either strategy is active.
            // This avoid sending the ring tone or music tail into the earpiece or headset.
            if ((isStrategyActive(desc, STRATEGY_MEDIA,
                                  SONIFICATION_HEADSET_MUSIC_DELAY,
                                  sysTime) ||
                 isStrategyActive(desc, STRATEGY_SONIFICATION,
                                  SONIFICATION_HEADSET_MUSIC_DELAY,
                                  sysTime)) &&
                    (delayMs < (int)desc->latency()*2)) {
                delayMs = desc->latency()*2;
            }
            setStrategyMute(STRATEGY_MEDIA, true, desc);
            setStrategyMute(STRATEGY_MEDIA, false, desc, MUTE_TIME_MS,
                getDeviceForStrategy(STRATEGY_MEDIA, true /*fromCache*/));
            setStrategyMute(STRATEGY_SONIFICATION, true, desc);
            setStrategyMute(STRATEGY_SONIFICATION, false, desc, MUTE_TIME_MS,
                getDeviceForStrategy(STRATEGY_SONIFICATION, true /*fromCache*/));
        }
    }

    if (hasPrimaryOutput()) {
        // Note that despite the fact that getNewOutputDevice() is called on the primary output,
        // the device returned is not necessarily reachable via this output
        audio_devices_t rxDevice = getNewOutputDevice(mPrimaryOutput, false /*fromCache*/);
        // force routing command to audio hardware when ending call
        // even if no device change is needed
        if (isStateInCall(oldState) && rxDevice == AUDIO_DEVICE_NONE) {
            rxDevice = mPrimaryOutput->device();
        }

        if (state == AUDIO_MODE_IN_CALL) {
            updateCallRouting(rxDevice, delayMs);
        } else if (oldState == AUDIO_MODE_IN_CALL) {
            if (mCallRxPatch != 0) {
                mpClientInterface->releaseAudioPatch(mCallRxPatch->mAfPatchHandle, 0);
                mCallRxPatch.clear();
            }
            if (mCallTxPatch != 0) {
                mpClientInterface->releaseAudioPatch(mCallTxPatch->mAfPatchHandle, 0);
                mCallTxPatch.clear();
            }
            setOutputDevice(mPrimaryOutput, rxDevice, force, 0);
        } else {
            setOutputDevice(mPrimaryOutput, rxDevice, force, 0);
        }
    }
#if 0
    // if entering in call state, handle special case of active streams
    // pertaining to sonification strategy see handleIncallSonification()
    if (isStateInCall(state)) {
        ALOGV("setPhoneState() in call state management: new state is %d", state);
        for (int stream = 0; stream < AUDIO_STREAM_CNT; stream++) {
            if (stream == AUDIO_STREAM_PATCH) {
                continue;
            }
            handleIncallSonification((audio_stream_type_t)stream, true, true);
        }

        // force reevaluating accessibility routing when call starts
        mpClientInterface->invalidateStream(AUDIO_STREAM_ACCESSIBILITY);
    }
#endif
    // Flag that ringtone volume must be limited to music volume until we exit MODE_RINGTONE
    if (state == AUDIO_MODE_RINGTONE &&
        isStreamActive(AUDIO_STREAM_MUSIC, SONIFICATION_HEADSET_MUSIC_DELAY)) {
        mLimitRingtoneVolume = true;
    } else {
        mLimitRingtoneVolume = false;
    }
}

// This function checks for the parameters which can be offloaded.
// This can be enhanced depending on the capability of the DSP and policy
// of the system.
bool AudioPolicyManagerCustom::isOffloadSupported(const audio_offload_info_t& offloadInfo)
{
    ALOGV("isOffloadSupported: SR=%u, CM=0x%x, Format=0x%x, StreamType=%d,"
     " BitRate=%u, duration=%" PRId64 " us, has_video=%d",
     offloadInfo.sample_rate, offloadInfo.channel_mask,
     offloadInfo.format,
     offloadInfo.stream_type, offloadInfo.bit_rate, offloadInfo.duration_us,
     offloadInfo.has_video);
#ifdef VOICE_CONCURRENCY
    char concpropValue[PROPERTY_VALUE_MAX];
    if (property_get("voice.playback.conc.disabled", concpropValue, NULL)) {
         bool propenabled = atoi(concpropValue) || !strncmp("true", concpropValue, 4);
         if (propenabled) {
            if (isInCall())
            {
                ALOGD("\n copl: blocking  compress offload on call mode\n");
                return false;
            }
         }
    }
#endif
#ifdef RECORD_PLAY_CONCURRENCY
    char recConcPropValue[PROPERTY_VALUE_MAX];
    bool prop_rec_play_enabled = false;

    if (property_get("rec.playback.conc.disabled", recConcPropValue, NULL)) {
        prop_rec_play_enabled = atoi(recConcPropValue) || !strncmp("true", recConcPropValue, 4);
    }

    if ((prop_rec_play_enabled) &&
         ((true == mIsInputRequestOnProgress) || (mInputs.activeInputsCount() > 0))) {
        ALOGD("copl: blocking  compress offload for record concurrency");
        return false;
    }
#endif
    // Check if stream type is music, then only allow offload as of now.
    if (offloadInfo.stream_type != AUDIO_STREAM_MUSIC)
    {
        ALOGV("isOffloadSupported: stream_type != MUSIC, returning false");
        return false;
    }

    char propValue[PROPERTY_VALUE_MAX];
    bool pcmOffload = false;
#ifdef PCM_OFFLOAD_ENABLED
    if ((offloadInfo.format & AUDIO_FORMAT_MAIN_MASK) == AUDIO_FORMAT_PCM_OFFLOAD) {
        bool prop_enabled = false;
        if ((AUDIO_FORMAT_PCM_16_BIT_OFFLOAD == offloadInfo.format) &&
               property_get("audio.offload.pcm.16bit.enable", propValue, NULL)) {
            prop_enabled = atoi(propValue) || !strncmp("true", propValue, 4);
        }

#ifdef PCM_OFFLOAD_ENABLED_24
        if ((AUDIO_FORMAT_PCM_24_BIT_OFFLOAD == offloadInfo.format) &&
               property_get("audio.offload.pcm.24bit.enable", propValue, NULL)) {
            prop_enabled = atoi(propValue) || !strncmp("true", propValue, 4);
        }
#endif

        if (prop_enabled) {
            ALOGI("PCM offload property is enabled");
            pcmOffload = true;
        }

        if (!pcmOffload) {
            ALOGD("system property not enabled for PCM offload format[%x]",offloadInfo.format);
            return false;
        }
    }
#endif
    if (!pcmOffload) {
        // Check if offload has been disabled
        if (property_get("audio.offload.disable", propValue, "0")) {
            if (atoi(propValue) != 0) {
                ALOGV("offload disabled by audio.offload.disable=%s", propValue );
                return false;
            }
        }
        //check if it's multi-channel AAC (includes sub formats) and FLAC format
        if ((popcount(offloadInfo.channel_mask) > 2) &&
           (((offloadInfo.format & AUDIO_FORMAT_MAIN_MASK) == AUDIO_FORMAT_AAC) ||
            ((offloadInfo.format & AUDIO_FORMAT_MAIN_MASK) == AUDIO_FORMAT_VORBIS))) {
               ALOGD("offload disabled for multi-channel AAC,FLAC and VORBIS format");
               return false;
        }

#ifdef AUDIO_EXTN_FORMATS_ENABLED
            //check if it's multi-channel FLAC/ALAC/WMA format with sample rate > 48k
        if ((popcount(offloadInfo.channel_mask) > 2) &&
            (((offloadInfo.format & AUDIO_FORMAT_MAIN_MASK) == AUDIO_FORMAT_FLAC) ||
            (((offloadInfo.format & AUDIO_FORMAT_MAIN_MASK) == AUDIO_FORMAT_ALAC) && (offloadInfo.sample_rate > 48000)) ||
            (((offloadInfo.format & AUDIO_FORMAT_MAIN_MASK) == AUDIO_FORMAT_WMA) && (offloadInfo.sample_rate > 48000)) ||
            (((offloadInfo.format & AUDIO_FORMAT_MAIN_MASK) == AUDIO_FORMAT_WMA_PRO) && (offloadInfo.sample_rate > 48000)) ||
            ((offloadInfo.format & AUDIO_FORMAT_MAIN_MASK) == AUDIO_FORMAT_AAC_ADTS))) {
                ALOGD("offload disabled for multi-channel FLAC/ALAC/WMA/AAC_ADTS clips with sample rate > 48kHz");
            return false;
        }
#endif
        //TODO: enable audio offloading with video when ready
        const bool allowOffloadWithVideo =
                property_get_bool("audio.offload.video", false /* default_value */);
        if (offloadInfo.has_video && !allowOffloadWithVideo) {
            ALOGV("isOffloadSupported: has_video == true, returning false");
            return false;
        }

        const bool allowOffloadStreamingWithVideo = property_get_bool("av.streaming.offload.enable",
                                                                   false /*default value*/);
        if(offloadInfo.has_video && offloadInfo.is_streaming && !allowOffloadStreamingWithVideo) {
            ALOGW("offload disabled by av.streaming.offload.enable = %s ", propValue );
            return false;
        }

    }

    //If duration is less than minimum value defined in property, return false
    if (property_get("audio.offload.min.duration.secs", propValue, NULL)) {
        if (offloadInfo.duration_us < (atoi(propValue) * 1000000 )) {
            ALOGV("Offload denied by duration < audio.offload.min.duration.secs(=%s)", propValue);
            return false;
        }
    } else if (offloadInfo.duration_us < OFFLOAD_DEFAULT_MIN_DURATION_SECS * 1000000) {
        //duration checks only valid for MP3/AAC/ formats,
        //do not check duration for other audio formats, e.g. dolby AAC/AC3 and amrwb+ formats
        if ((offloadInfo.format == AUDIO_FORMAT_MP3) ||
            ((offloadInfo.format & AUDIO_FORMAT_MAIN_MASK) == AUDIO_FORMAT_AAC) ||
            ((offloadInfo.format & AUDIO_FORMAT_MAIN_MASK) == AUDIO_FORMAT_VORBIS) ||
#ifdef AUDIO_EXTN_FORMATS_ENABLED
            ((offloadInfo.format & AUDIO_FORMAT_MAIN_MASK) == AUDIO_FORMAT_FLAC) ||
            ((offloadInfo.format & AUDIO_FORMAT_MAIN_MASK) == AUDIO_FORMAT_ALAC) ||
            ((offloadInfo.format & AUDIO_FORMAT_MAIN_MASK) == AUDIO_FORMAT_APE) ||
            ((offloadInfo.format & AUDIO_FORMAT_MAIN_MASK) == AUDIO_FORMAT_AAC_ADTS) ||
#endif
            pcmOffload)
			{
				ALOGV("Offload denied by duration < default min(=%u)", OFFLOAD_DEFAULT_MIN_DURATION_SECS);
				return false;
			}

    }

    // Do not allow offloading if one non offloadable effect is enabled. This prevents from
    // creating an offloaded track and tearing it down immediately after start when audioflinger
    // detects there is an active non offloadable effect.
    // FIXME: We should check the audio session here but we do not have it in this context.
    // This may prevent offloading in rare situations where effects are left active by apps
    // in the background.
    if (mEffects.isNonOffloadableEffectEnabled()) {
        return false;
    }
    // Check for soundcard status
    String8 valueStr = mpClientInterface->getParameters((audio_io_handle_t)0,
    String8("SND_CARD_STATUS"));
    AudioParameter result = AudioParameter(valueStr);
    int isonline = 0;
    if ((result.getInt(String8("SND_CARD_STATUS"), isonline) == NO_ERROR)
           && !isonline) {
         ALOGD("copl: soundcard is offline rejecting offload request");
         return false;
    }
    // See if there is a profile to support this.
    // AUDIO_DEVICE_NONE
    sp<IOProfile> profile = getProfileForDirectOutput(AUDIO_DEVICE_NONE /*ignore device */,
                                            offloadInfo.sample_rate,
                                            offloadInfo.format,
                                            offloadInfo.channel_mask,
                                            AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD);
    ALOGV("isOffloadSupported() profile %sfound", profile != 0 ? "" : "NOT ");
    return (profile != 0);
}

status_t AudioPolicyManagerCustom::setStreamVolumeIndex(audio_stream_type_t stream,
                                                  int index,
                                                  audio_devices_t device)
{
    if ((index < mStreams.valueFor(stream).getVolumeIndexMin()) ||
            (index > mStreams.valueFor(stream).getVolumeIndexMax())) {
        return BAD_VALUE;
    }
    if (!audio_is_output_device(device)) {
        return BAD_VALUE;
    }

    // Force max volume if stream cannot be muted
    if (!mStreams.canBeMuted(stream)) index = mStreams.valueFor(stream).getVolumeIndexMax();

    ALOGV("setStreamVolumeIndex() stream %d, device %04x, index %d",
          stream, device, index);

    // if device is AUDIO_DEVICE_OUT_DEFAULT set default value and
    // clear all device specific values
    // FIXME: no need to clear volume index if default device
    //        at least not needed for AUDIO_STREAM_MUSIC
    //        AUDIO_STREAM_ACCESSIBILITY follows
    if (device == AUDIO_DEVICE_OUT_DEFAULT &&
            stream != AUDIO_STREAM_MUSIC &&
            stream != AUDIO_STREAM_ACCESSIBILITY) {
        mStreams.clearCurrentVolumeIndex(stream);
    }
    mStreams.addCurrentVolumeIndex(stream, device, index);

    // update volume on all outputs whose current device is also selected by the same
    // strategy as the device specified by the caller
    audio_devices_t strategyDevice = getDeviceForStrategy(getStrategy(stream), true /*fromCache*/);

    //FIXME: AUDIO_STREAM_ACCESSIBILITY volume follows AUDIO_STREAM_MUSIC for now
    audio_devices_t accessibilityDevice = AUDIO_DEVICE_NONE;
    if (stream == AUDIO_STREAM_MUSIC) {
        mStreams.addCurrentVolumeIndex(AUDIO_STREAM_ACCESSIBILITY, device, index);
        accessibilityDevice = getDeviceForStrategy(STRATEGY_ACCESSIBILITY, true /*fromCache*/);
    }
    if ((device != AUDIO_DEVICE_OUT_DEFAULT) &&
            (device & (strategyDevice | accessibilityDevice)) == 0) {
        return NO_ERROR;
    }
    status_t status = NO_ERROR;
    for (size_t i = 0; i < mOutputs.size(); i++) {
        sp<SwAudioOutputDescriptor> desc = mOutputs.valueAt(i);
        audio_devices_t curDevice = Volume::getDeviceForVolume(desc->device());
        if ((device == AUDIO_DEVICE_OUT_DEFAULT) || ((curDevice & strategyDevice) != 0)) {
            status_t volStatus = checkAndSetVolume(stream,
                                                   mStreams.valueFor(stream).getVolumeIndex(curDevice),
                                                   desc,
                                                   curDevice);
            if (volStatus != NO_ERROR) {
                status = volStatus;
            }
        }
        if ((accessibilityDevice != AUDIO_DEVICE_NONE) &&
                ((device == AUDIO_DEVICE_OUT_DEFAULT) || ((curDevice & accessibilityDevice) != 0)))
        {
            status_t volStatus = checkAndSetVolume(AUDIO_STREAM_ACCESSIBILITY,
                                                   mStreams.valueFor(stream).getVolumeIndex(curDevice),
                                                   desc,
                                                   curDevice);
        }
    }
    return status;
}

extern "C" AudioPolicyInterface* createAudioPolicyManager(
        AudioPolicyClientInterface *clientInterface)
{
    return new AudioPolicyManagerCustom(clientInterface);
}

extern "C" void destroyAudioPolicyManager(AudioPolicyInterface *interface)
{
    delete interface;
}

}; // namespace android
