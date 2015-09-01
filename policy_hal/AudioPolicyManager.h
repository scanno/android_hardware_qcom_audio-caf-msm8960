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

#include <audiopolicy/managerdefault/AudioPolicyManager.h>
#include <audio_policy_conf.h>

namespace android {

// ----------------------------------------------------------------------------

class AudioPolicyManagerCustom: public AudioPolicyManager
{

public:
        AudioPolicyManagerCustom(AudioPolicyClientInterface *clientInterface);

        virtual ~AudioPolicyManagerCustom() {}

        virtual status_t getOutputForAttr(const audio_attributes_t *attr,
                                          audio_io_handle_t *output,
                                          audio_session_t session,
                                          audio_stream_type_t *stream,
                                          uid_t uid,
                                          uint32_t samplingRate,
                                          audio_format_t format,
                                          audio_channel_mask_t channelMask,
                                          audio_output_flags_t flags,
                                          audio_port_handle_t selectedDeviceId,
                                          const audio_offload_info_t *offloadInfo);

        virtual void setPhoneState(audio_mode_t state);
protected:
        virtual audio_io_handle_t getOutputForDevice(
                audio_devices_t device,
                audio_session_t session,
                audio_stream_type_t stream,
                uint32_t samplingRate,
                audio_format_t format,
                audio_channel_mask_t channelMask,
                audio_output_flags_t flags,
                const audio_offload_info_t *offloadInfo);

        // internal function to derive a stream type value from audio attributes
        audio_stream_type_t streamTypefromAttributesInt(const audio_attributes_t *attr);
        bool isValidAttributes(const audio_attributes_t *paa);
        sp<SwAudioOutputDescriptor> mDriverSideOutput;
        sp<IOProfile> mDriverSideProfile;
};

};
