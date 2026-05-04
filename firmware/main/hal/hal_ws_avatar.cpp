/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include <stackchan/stackchan.h>
#include "board/hal_bridge.h"
#include <mooncake.h>
#include <mooncake_log.h>
#include <board.h>
#include <web_socket.h>
#include <esp_log.h>
#include <arpa/inet.h>
#include <jpg/image_to_jpeg.h>
#include <wifi_station.h>
#include <ArduinoJson.hpp>
#include <settings.h>
#include <mutex>
#include <queue>
#include <vector>
#include <esp_heap_caps.h>
#include <display.h>
#include <lvgl_image.h>
#include <wifi_manager.h>
#include "utils/jpeg_to_image/jpeg_decoder.h"
#include "utils/secret_logic/secret_logic.h"
#include <audio_codec.h>
#include "esp_opus_enc.h"
#include "esp_opus_dec.h"
#include "esp_ae_rate_cvt.h"
#include "esp_audio_types.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <atomic>

static std::string _tag = "WS-Avatar";

static const std::string _setting_ns              = "stackchan";
static const std::string _setting_device_name_key = "device_name";

class WebSocketAvatar {
public:
    ~WebSocketAvatar()
    {
        destroyAudio();
    }

    enum class DataType : uint8_t {
        Opus              = 0x01,
        Jpeg              = 0x02,
        ControlAvatar     = 0x03,
        ControlMotion     = 0x04,
        StartCameraStream = 0x05,
        StopCameraStream  = 0x06,
        TextMessage       = 0x07,
        RequestCall       = 0x09,
        DeclineCall       = 0x0A,
        AcceptCall        = 0x0B,
        EndCall           = 0x0C,
        SetDeviceName     = 0x0D,
        GetDeviceName     = 0x0E,
        HeartbeatPing     = 0x10,
        HeartbeatPong     = 0x11,
        VideoModeOn       = 0x12,
        VideoModeOff      = 0x13,
        DanceSequence     = 0x14,
        StartAudioStream  = 0x18,
        StopAudioStream   = 0x19,
    };

    struct ReceivedMessage {
        bool binary;
        std::vector<uint8_t> data;
    };

    void init()
    {
        _url = fmt::format("{}/stackChan/ws?deviceType=StackChan&mac={}",
                           secret_logic::get_server_url(),
                           GetHAL().getFactoryMacString(""));

        connect();

        GetHAL().onWsCallResponse.connect([this](bool accepted) {
            if (!isConnected()) {
                return;
            }

            if (accepted) {
                ESP_LOGI(_tag.c_str(), "Sending AcceptCall");
                sendPacket(DataType::AcceptCall, nullptr, 0);
            } else {
                ESP_LOGI(_tag.c_str(), "Sending DeclineCall");
                sendPacket(DataType::DeclineCall, nullptr, 0);
            }
        });

        GetHAL().onWsCallEnd.connect([this](WsSignalSource source) {
            if (!isConnected()) {
                return;
            }

            if (source != WsSignalSource::Local) {
                return;
            }

            ESP_LOGI(_tag.c_str(), "Sending EndCall");
            sendPacket(DataType::EndCall, nullptr, 0);
        });
    }

    void connect()
    {
        auto token = secret_logic::generate_auth_token();

        // 销毁旧实例，确保状态复位
        _websocket.reset();

        auto& board  = Board::GetInstance();
        auto network = board.GetNetwork();

        // 创建 WebSocket 实例
        _websocket = network->CreateWebSocket(1);

        if (!_websocket) {
            ESP_LOGE(_tag.c_str(), "Failed to create websocket");
            return;
        }

        // 设置认证头
        _websocket->SetHeader("Authorization", token.c_str());

        // 设置回调
        _websocket->OnConnected([this]() {
            ESP_LOGI(_tag.c_str(), "Connected to server!");
            // GetHAL().onWsLog.emit(CommonLogLevel::Info, "Server connected");
            _last_heartbeat_time = GetHAL().millis();
            _websocket->Send("{\"type\":\"hello\", \"msg\":\"Hello from StackChan!\"}");
        });

        _websocket->OnDisconnected([this]() {
            ESP_LOGI(_tag.c_str(), "Disconnected!");
            GetHAL().onWsLog.emit(CommonLogLevel::Error, "Server disconnected");
        });

        _websocket->OnData([this](const char* data, size_t len, bool binary) {
            std::lock_guard<std::mutex> lock(_mutex);
            _msg_queue.push({binary, std::vector<uint8_t>(data, data + len)});
        });

        // ESP_LOGI(_tag.c_str(), "Connecting to %s...", _url.c_str());
        // GetHAL().onWsLog.emit(CommonLogLevel::Info, "Connecting to server...");
        if (!_websocket->Connect(_url.c_str())) {
            ESP_LOGE(_tag.c_str(), "Failed to connect");
            GetHAL().onWsLog.emit(CommonLogLevel::Error, "Connect to server Failed");
        }
        _last_reconnect_attempt = GetHAL().millis();
    }

    void update()
    {
        if (!_websocket) {
            return;
        }

        if (!_websocket->IsConnected()) {
            if (GetHAL().millis() - _last_reconnect_attempt > 5000) {
                connect();
            }
        } else {
            processMessages();

            // Check heartbeat timeout
            if (GetHAL().millis() - _last_heartbeat_time > 10000) {
                ESP_LOGE(_tag.c_str(), "Heartbeat timeout!");
                GetHAL().onWsLog.emit(CommonLogLevel::Error, "Heartbeat Timeout");
                _last_heartbeat_time = GetHAL().millis();
                return;
            }
        }

        if (_is_streaming) {
            if (GetHAL().millis() - _last_capture_time >= (_is_video_mode ? 700 : 350)) {
                captureAndSendFrame();
                _last_capture_time = GetHAL().millis();
            }
        }
    }

    void processMessages()
    {
        std::vector<ReceivedMessage> messages;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            while (!_msg_queue.empty()) {
                messages.push_back(std::move(_msg_queue.front()));
                _msg_queue.pop();
            }
        }

        for (const auto& msg : messages) {
            handleMessage(msg);
        }
    }

    void handleMessage(const ReceivedMessage& msg)
    {
        if (msg.binary) {
            if (msg.data.size() < 1) return;
            DataType type = static_cast<DataType>(msg.data[0]);
            ESP_LOGI(_tag.c_str(), "Received binary type: %d, len: %d", (int)type, (int)msg.data.size());

            switch (type) {
                case DataType::Opus: {
                    if (msg.data.size() > 5) {
                        playOpusPacket(msg.data.data() + 5, msg.data.size() - 5);
                    }
                    break;
                }
                case DataType::StartCameraStream: {
                    ESP_LOGI(_tag.c_str(), "Start Camera Stream");
                    setStreamingEnabled(true);
                    _websocket->Send("camera stream started");
                    break;
                }
                case DataType::StopCameraStream: {
                    ESP_LOGI(_tag.c_str(), "Stop Camera Stream");
                    setStreamingEnabled(false);
                    _websocket->Send("camera stream stopped");
                    break;
                }
                case DataType::ControlAvatar: {
                    // Protocol: [Type(1)] [Length(4)] [Payload]
                    if (msg.data.size() >= 5) {
                        std::string payload(msg.data.begin() + 5, msg.data.end());
                        // ESP_LOGI(_tag.c_str(), "Control Avatar Payload: %s", payload.c_str());
                        GetHAL().onWsAvatarData.emit(payload);
                    }
                    break;
                }
                case DataType::ControlMotion: {
                    // Protocol: [Type(1)] [Length(4)] [Payload]
                    if (msg.data.size() >= 5) {
                        std::string payload(msg.data.begin() + 5, msg.data.end());
                        // ESP_LOGI(_tag.c_str(), "Control Motion Payload: %s", payload.c_str());
                        GetHAL().onWsMotionData.emit(payload);
                    }
                    break;
                }
                case DataType::RequestCall: {
                    // Protocol: [Type(1)] [Length(4)] [Payload]
                    if (msg.data.size() >= 5) {
                        std::string payload(msg.data.begin() + 5, msg.data.end());
                        ESP_LOGI(_tag.c_str(), "RequestCall Payload: %s", payload.c_str());
                        GetHAL().onWsCallRequest.emit(payload);
                    }
                    break;
                }
                case DataType::EndCall: {
                    ESP_LOGI(_tag.c_str(), "EndCall");
                    GetHAL().onWsCallEnd.emit(WsSignalSource::Remote);
                    break;
                }
                case DataType::SetDeviceName: {
                    // Protocol: [Type(1)] [Length(4)] [Payload]
                    if (msg.data.size() >= 5) {
                        std::string payload(msg.data.begin() + 5, msg.data.end());
                        ESP_LOGI(_tag.c_str(), "SetDeviceName Payload: %s", payload.c_str());

                        Settings settings(_setting_ns, true);
                        settings.SetString(_setting_device_name_key, payload);
                    }
                    break;
                }
                case DataType::GetDeviceName: {
                    ESP_LOGI(_tag.c_str(), "GetDeviceName");

                    Settings settings(_setting_ns, false);
                    auto device_name = settings.GetString(_setting_device_name_key, "StackChan");

                    sendPacket(DataType::GetDeviceName, (const uint8_t*)device_name.c_str(), device_name.size());
                    break;
                }
                case DataType::HeartbeatPing: {
                    ESP_LOGI(_tag.c_str(), "HeartbeatPing");
                    _last_heartbeat_time = GetHAL().millis();
                    sendPacket(DataType::HeartbeatPong, nullptr, 0);
                    break;
                }
                case DataType::TextMessage: {
                    // Protocol: [Type(1)] [Length(4)] [Payload]
                    if (msg.data.size() >= 5) {
                        std::string payload(msg.data.begin() + 5, msg.data.end());
                        ESP_LOGI(_tag.c_str(), "TextMessage Payload: %s", payload.c_str());

                        ArduinoJson::JsonDocument doc;
                        auto error = ArduinoJson::deserializeJson(doc, payload);
                        if (error) {
                            ESP_LOGE(_tag.c_str(), "DeserializeJson failed: %s", error.c_str());
                            return;
                        }

                        WsTextMessage_t text_msg;

                        if (doc["name"].is<std::string>()) {
                            text_msg.name = doc["name"].as<std::string>();
                        }
                        if (doc["content"].is<std::string>()) {
                            text_msg.content = doc["content"].as<std::string>();
                        }

                        GetHAL().onWsTextMessage.emit(text_msg);
                    }
                    break;
                }
                case DataType::VideoModeOn: {
                    ESP_LOGI(_tag.c_str(), "VideoModeOn");
                    GetHAL().onWsVideoModeChange.emit(true);
                    _is_video_mode = true;
                    break;
                }
                case DataType::VideoModeOff: {
                    ESP_LOGI(_tag.c_str(), "VideoModeOff");
                    GetHAL().onWsVideoModeChange.emit(false);
                    _is_video_mode = false;
                    break;
                }
                case DataType::Jpeg: {
                    // Protocol: [Type(1)] [Length(4)] [Payload]
                    if (msg.data.size() >= 5) {
                        ESP_LOGI(_tag.c_str(), "Jpeg Frame Received, size: %d", (int)(msg.data.size() - 5));

                        static int64_t _time_count = 0;
                        static int64_t _interval   = 0;
                        _time_count                = esp_timer_get_time();

                        size_t jpeg_len    = msg.data.size() - 5;
                        uint8_t* jpeg_data = (uint8_t*)heap_caps_malloc(jpeg_len, MALLOC_CAP_8BIT);
                        if (jpeg_data) {
                            memcpy(jpeg_data, msg.data.data() + 5, jpeg_len);

                            auto image = jpeg_dec::decode_to_lvgl(jpeg_data, jpeg_len);
                            if (image) {
                                // ESP_LOGI(_tag.c_str(), "Done");

                                _interval = esp_timer_get_time() - _time_count;
                                mclog::info("jpeg decode time: {} ms", _interval / 1000);

                                GetHAL().onWsVideoFrame.emit(image);
                            } else {
                                ESP_LOGE(_tag.c_str(), "Failed to decode JPEG");
                            }
                            heap_caps_free(jpeg_data);
                        } else {
                            ESP_LOGE(_tag.c_str(), "Failed to allocate memory for JPEG");
                        }
                    }
                    break;
                }
                case DataType::DanceSequence: {
                    // Protocol: [Type(1)] [Length(4)] [Payload]
                    if (msg.data.size() >= 5) {
                        std::string payload(msg.data.begin() + 5, msg.data.end());
                        // ESP_LOGI(_tag.c_str(), "Dance Payload:\n%s", payload.c_str());
                        ESP_LOGI(_tag.c_str(), "DanceSequence size: %d", (int)payload.size());
                        GetHAL().onWsDanceData.emit(payload);
                    }
                    break;
                }
                case DataType::StartAudioStream: {
                    ESP_LOGI(_tag.c_str(), "Start Audio Stream");
                    startAudioCapture();
                    break;
                }
                case DataType::StopAudioStream: {
                    ESP_LOGI(_tag.c_str(), "Stop Audio Stream");
                    stopAudioCapture();
                    break;
                }
                default:
                    break;
            }
        } else {
            ESP_LOGI(_tag.c_str(), "Received text: %.*s", (int)msg.data.size(), (char*)msg.data.data());
        }
    }

    bool isConnected()
    {
        return _websocket && _websocket->IsConnected();
    }

    void captureAndSendFrame()
    {
        if (!isConnected()) {
            return;
        }

        static int64_t _time_count = 0;
        static int64_t _interval   = 0;

        auto camera = hal_bridge::board_get_camera();
        if (!camera) {
            return;
        }

        _time_count = esp_timer_get_time();
        if (camera->StreamCaptures()) {
            _interval = esp_timer_get_time() - _time_count;
            mclog::info("camera capture time: {} ms", _interval / 1000);

            const uint8_t* frameData = camera->GetFrameData();
            size_t frameSize         = camera->GetFrameSize();
            int width                = camera->GetFrameWidth();
            int height               = camera->GetFrameHeight();
            int format               = camera->GetFrameFormat();

            uint8_t* jpeg_data = nullptr;
            size_t jpeg_len    = 0;

            // 压缩为 JPEG
            _time_count = esp_timer_get_time();
            if (image_to_jpeg((uint8_t*)frameData, frameSize, width, height, (v4l2_pix_fmt_t)format, 20, &jpeg_data,
                              &jpeg_len)) {
                _interval = esp_timer_get_time() - _time_count;
                // mclog::info("jpeg encode time: {} ms, size: {}", _interval / 1000, jpeg_len);
                mclog::info("jpeg encode time: {} ms", _interval / 1000);

                if (jpeg_data) {
                    sendPacket(DataType::Jpeg, jpeg_data, jpeg_len);  // Type 2 for JPEG
                    free(jpeg_data);
                }
            }
        }
    }

    void setStreamingEnabled(bool enabled)
    {
        _is_streaming = enabled;
    }

    /* ----------------------------- Audio Streaming ----------------------------- */

    static constexpr int kOpusFrameDurationMs = 60;
    static constexpr int kOpusSampleRate      = 16000;

    void initAudio()
    {
        auto codec = Board::GetInstance().GetAudioCodec();
        if (!codec) {
            ESP_LOGE(_tag.c_str(), "No audio codec available");
            return;
        }

        // Opus encoder: 16kHz mono, 60ms frames
        esp_opus_enc_config_t enc_cfg = {
            .sample_rate      = ESP_AUDIO_SAMPLE_RATE_16K,
            .channel          = ESP_AUDIO_MONO,
            .bits_per_sample  = ESP_AUDIO_BIT16,
            .bitrate          = ESP_OPUS_BITRATE_AUTO,
            .frame_duration   = ESP_OPUS_ENC_FRAME_DURATION_60_MS,
            .application_mode = ESP_OPUS_ENC_APPLICATION_VOIP,
            .complexity       = 0,
            .enable_fec       = false,
            .enable_dtx       = true,
            .enable_vbr       = true,
        };
        auto ret = esp_opus_enc_open(&enc_cfg, sizeof(enc_cfg), &_opus_encoder);
        if (!_opus_encoder) {
            ESP_LOGE(_tag.c_str(), "Failed to create Opus encoder: %d", ret);
            return;
        }
        esp_opus_enc_get_frame_size(_opus_encoder, &_enc_frame_size, &_enc_outbuf_size);
        _enc_frame_samples = _enc_frame_size / sizeof(int16_t);

        // Opus decoder: 16kHz mono, 60ms frames
        esp_opus_dec_cfg_t dec_cfg = {
            .sample_rate    = (uint32_t)kOpusSampleRate,
            .channel        = ESP_AUDIO_MONO,
            .frame_duration = ESP_OPUS_DEC_FRAME_DURATION_60_MS,
            .self_delimited = false,
        };
        ret = esp_opus_dec_open(&dec_cfg, sizeof(dec_cfg), &_opus_decoder);
        if (!_opus_decoder) {
            ESP_LOGE(_tag.c_str(), "Failed to create Opus decoder: %d", ret);
        }

        // Input resampler: 24kHz -> 16kHz (hardware to Opus)
        int hw_rate = codec->input_sample_rate();
        if (hw_rate != kOpusSampleRate) {
            esp_ae_rate_cvt_cfg_t in_cfg = {
                .src_rate        = (uint32_t)hw_rate,
                .dest_rate       = (uint32_t)kOpusSampleRate,
                .channel         = 1,
                .bits_per_sample = ESP_AUDIO_BIT16,
                .complexity      = 2,
                .perf_type       = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,
            };
            esp_ae_rate_cvt_open(&in_cfg, &_input_resampler);
        }

        // Output resampler: 16kHz -> 24kHz (Opus to hardware)
        int out_rate = codec->output_sample_rate();
        if (out_rate != kOpusSampleRate) {
            esp_ae_rate_cvt_cfg_t out_cfg = {
                .src_rate        = (uint32_t)kOpusSampleRate,
                .dest_rate       = (uint32_t)out_rate,
                .channel         = 1,
                .bits_per_sample = ESP_AUDIO_BIT16,
                .complexity      = 2,
                .perf_type       = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,
            };
            esp_ae_rate_cvt_open(&out_cfg, &_output_resampler);
        }

        _audio_initialized = true;
        ESP_LOGI(_tag.c_str(), "Audio initialized (hw: %d Hz, opus: %d Hz, frame: %d samples)",
                 hw_rate, kOpusSampleRate, _enc_frame_samples);
    }

    void destroyAudio()
    {
        stopAudioCapture();
        if (_opus_encoder)     { esp_opus_enc_close(_opus_encoder);       _opus_encoder = nullptr; }
        if (_opus_decoder)     { esp_opus_dec_close(_opus_decoder);       _opus_decoder = nullptr; }
        if (_input_resampler)  { esp_ae_rate_cvt_close(_input_resampler); _input_resampler = nullptr; }
        if (_output_resampler) { esp_ae_rate_cvt_close(_output_resampler);_output_resampler = nullptr; }
        _audio_initialized = false;
    }

    void startAudioCapture()
    {
        if (_is_audio_streaming) {
            return;
        }
        if (!_audio_initialized) {
            initAudio();
        }
        if (!_audio_initialized || !_opus_encoder) {
            ESP_LOGE(_tag.c_str(), "Cannot start audio: not initialized");
            return;
        }

        auto codec = Board::GetInstance().GetAudioCodec();
        codec->EnableInput(true);

        _is_audio_streaming = true;
        _audio_task_done = xSemaphoreCreateBinary();

        // Dedicated task for mic capture + encode + send
        xTaskCreatePinnedToCore([](void* arg) {
            auto* self = static_cast<WebSocketAvatar*>(arg);
            self->audioCaptureTask();
            if (self->_audio_task_done) {
                xSemaphoreGive(self->_audio_task_done);
            }
            vTaskDelete(NULL);
        }, "audio_capture", 4096 * 2, this, 6, &_audio_capture_task, 0);

        ESP_LOGI(_tag.c_str(), "Audio capture started");
        GetHAL().showRgbColor(0, 80, 0);  // Green LED = recording
    }

    void stopAudioCapture()
    {
        if (!_is_audio_streaming) {
            return;
        }
        _is_audio_streaming = false;

        // Wait for the task to exit before closing the codec
        if (_audio_capture_task && _audio_task_done) {
            xSemaphoreTake(_audio_task_done, pdMS_TO_TICKS(500));
            _audio_capture_task = nullptr;
        }
        if (_audio_task_done) {
            vSemaphoreDelete(_audio_task_done);
            _audio_task_done = nullptr;
        }

        auto codec = Board::GetInstance().GetAudioCodec();
        if (codec) {
            codec->EnableInput(false);
        }

        ESP_LOGI(_tag.c_str(), "Audio capture stopped");
        GetHAL().showRgbColor(0, 0, 0);  // LED off
    }

    void audioCaptureTask()
    {
        auto codec = Board::GetInstance().GetAudioCodec();
        if (!codec) return;

        const int hw_rate       = codec->input_sample_rate();
        const int hw_channels   = codec->input_channels();
        const int hw_samples    = (hw_rate * kOpusFrameDurationMs) / 1000;  // samples per channel
        const int read_samples  = hw_samples * hw_channels;

        std::vector<int16_t> hw_buf(read_samples);
        std::vector<int16_t> mono_buf(hw_samples);
        std::vector<uint8_t> opus_buf(_enc_outbuf_size);
        std::vector<int16_t> resampled;
        resampled.reserve(hw_samples);  // pre-allocate, reuse across iterations

        while (_is_audio_streaming) {
            // Read from mic via public InputData API
            codec->InputData(hw_buf);

            // Extract mono: channel 0 is mic, channel 1 is echo-reference (discarded)
            for (int i = 0; i < hw_samples; i++) {
                mono_buf[i] = hw_buf[i * hw_channels];
            }

            // Resample 24kHz -> 16kHz if needed
            std::vector<int16_t>* pcm_for_encoder = &mono_buf;
            if (_input_resampler) {
                uint32_t in_samples = mono_buf.size();
                uint32_t out_samples = 0;
                esp_ae_rate_cvt_get_max_out_sample_num(_input_resampler, in_samples, &out_samples);
                resampled.resize(out_samples);
                uint32_t actual = out_samples;
                esp_ae_rate_cvt_process(_input_resampler,
                                        (esp_ae_sample_t)mono_buf.data(), in_samples,
                                        (esp_ae_sample_t)resampled.data(), &actual);
                resampled.resize(actual);
                pcm_for_encoder = &resampled;
            }

            // Encode to Opus
            esp_audio_enc_in_frame_t enc_in = {
                .buffer = (uint8_t*)pcm_for_encoder->data(),
                .len    = (uint32_t)(pcm_for_encoder->size() * sizeof(int16_t))
            };
            esp_audio_enc_out_frame_t enc_out = {
                .buffer = opus_buf.data(),
                .len    = (uint32_t)opus_buf.size()
            };
            auto enc_ret = esp_opus_enc_process(_opus_encoder, &enc_in, &enc_out);
            if (enc_ret == ESP_AUDIO_ERR_OK && enc_out.encoded_bytes > 0) {
                sendPacket(DataType::Opus, opus_buf.data(), enc_out.encoded_bytes);
            }
        }
    }

    void playOpusPacket(const uint8_t* data, size_t len)
    {
        if (!_audio_initialized) {
            initAudio();
        }
        if (!_opus_decoder) {
            return;
        }

        auto codec = Board::GetInstance().GetAudioCodec();
        if (!codec) return;

        if (!codec->output_enabled()) {
            codec->EnableOutput(true);
        }

        // Decode Opus -> PCM (16kHz mono)
        const int decoded_samples = kOpusSampleRate * kOpusFrameDurationMs / 1000;
        std::vector<int16_t> pcm(decoded_samples);
        int in_size  = (int)len;
        int out_size = decoded_samples * sizeof(int16_t);

        esp_audio_dec_in_raw_t dec_in = {
            .buffer = (uint8_t*)data,
            .len    = (uint32_t)in_size
        };
        esp_audio_dec_out_frame_t dec_out = {
            .buffer = (uint8_t*)pcm.data(),
            .len    = (uint32_t)out_size
        };
        auto dec_ret = esp_opus_dec_decode(_opus_decoder, &dec_in, &dec_out, nullptr);
        if (dec_ret != ESP_AUDIO_ERR_OK || dec_out.decoded_size <= 0) {
            return;
        }

        int pcm_samples = dec_out.decoded_size / sizeof(int16_t);

        // Resample 16kHz -> 24kHz if needed, then write to speaker
        if (_output_resampler) {
            uint32_t out_max = 0;
            esp_ae_rate_cvt_get_max_out_sample_num(_output_resampler, pcm_samples, &out_max);
            std::vector<int16_t> upsampled(out_max);
            uint32_t actual = out_max;
            esp_ae_rate_cvt_process(_output_resampler,
                                    (esp_ae_sample_t)pcm.data(), (uint32_t)pcm_samples,
                                    (esp_ae_sample_t)upsampled.data(), &actual);
            codec->OutputData(upsampled);
        } else {
            codec->OutputData(pcm);
        }
    }

private:
    std::unique_ptr<WebSocket> _websocket;
    std::string _url;
    uint32_t _last_reconnect_attempt = 0;
    uint32_t _last_capture_time      = 0;
    uint32_t _last_heartbeat_time    = 0;
    bool _is_streaming               = false;
    bool _is_video_mode              = false;
    std::mutex _mutex;
    std::queue<ReceivedMessage> _msg_queue;

    std::mutex _send_mutex;

    // Audio streaming state
    bool _audio_initialized                    = false;
    std::atomic<bool> _is_audio_streaming      {false};
    void* _opus_encoder                        = nullptr;
    void* _opus_decoder                        = nullptr;
    int _enc_frame_size                        = 0;
    int _enc_frame_samples                     = 0;
    int _enc_outbuf_size                       = 0;
    esp_ae_rate_cvt_handle_t _input_resampler  = nullptr;
    esp_ae_rate_cvt_handle_t _output_resampler = nullptr;
    TaskHandle_t _audio_capture_task           = nullptr;
    SemaphoreHandle_t _audio_task_done         = nullptr;

    void sendPacket(DataType type, const uint8_t* data, size_t len)
    {
        std::lock_guard<std::mutex> lock(_send_mutex);
        if (!_websocket || !_websocket->IsConnected()) {
            return;
        }

        // mclog::info("sending packet type: {}, len: {}", (int)type, (int)len);

        // static int64_t _time_count = 0;
        // static int64_t _interval   = 0;
        // _time_count                = esp_timer_get_time();

        std::vector<uint8_t> packet;
        packet.reserve(1 + 4 + len);

        // [1 byte type]
        packet.push_back(static_cast<uint8_t>(type));

        // [4 bytes length] (Big Endian)
        uint32_t net_len       = htonl((uint32_t)len);
        const uint8_t* len_ptr = (const uint8_t*)&net_len;
        packet.push_back(len_ptr[0]);
        packet.push_back(len_ptr[1]);
        packet.push_back(len_ptr[2]);
        packet.push_back(len_ptr[3]);

        // [payload]
        if (len > 0) {
            packet.insert(packet.end(), data, data + len);
        }

        // _interval = esp_timer_get_time() - _time_count;
        // mclog::info("pack time: {} ms, size: {}", _interval / 1000, packet.size());

        // _time_count = esp_timer_get_time();
        _websocket->Send(packet.data(), packet.size(), true);
        // _interval = esp_timer_get_time() - _time_count;
        // mclog::info("send time: {} ms, size: {}", _interval / 1000, packet.size());
    }
};

class WebsocketAvatarWorker : public mooncake::BasicAbility {
public:
    WebsocketAvatarWorker()
    {
        _service = std::make_unique<WebSocketAvatar>();
        _service->init();
    }

    void onCreate() override
    {
    }

    void onRunning() override
    {
        if (GetHAL().millis() - _last_tick < 20) {
            return;
        }
        _last_tick = GetHAL().millis();
        _service->update();
    }

    void onDestroy() override
    {
        _service.reset();
    }

private:
    std::unique_ptr<WebSocketAvatar> _service;
    uint32_t _last_tick = 0;
};

void Hal::startWebSocketAvatarService(std::function<void(std::string_view)> onStartLog)
{
    mclog::tagInfo(_tag, "start websocket avatar service");

    startNetwork(onStartLog);

    onStartLog("Connecting to\nserver...");
    mooncake::GetMooncake().extensionManager()->createAbility(std::make_unique<WebsocketAvatarWorker>());
}
