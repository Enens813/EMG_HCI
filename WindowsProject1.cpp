#include <iostream>
#include <iomanip>
#include <sstream>
#include <thread>
#include <vector>
#include <deque>
#include <chrono>
#include <mutex>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <windows.h>

#include <onnxruntime_cxx_api.h> // #include <onnxruntime/core/session/onnxruntime_cxx_api.h>

using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Devices::Bluetooth;
using namespace Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace Windows::Storage::Streams;
using namespace Windows::Devices::Bluetooth::Advertisement;

bool running = false;
const GUID characteristic_uuid = { 0xbeb5483e, 0x36e1, 0x4688, { 0xb7, 0xf5, 0xea, 0x07, 0x36, 0x1b, 0x26, 0xa8 } };
const int window = 30;
std::mutex device_mutex;
BluetoothLEDevice global_device = nullptr;
std::chrono::steady_clock::time_point lastClickTime = std::chrono::steady_clock::now() - std::chrono::seconds(1);

std::vector<float> emgDataQueue;
std::mutex queue_mutex;
std::condition_variable data_condition;


std::wstring BluetoothAddressToString(uint64_t address) {
    std::wstringstream ss;
    ss << std::hex << std::setw(2) << std::setfill(L'0')
        << ((address >> 40) & 0xFF) << L":"
        << ((address >> 32) & 0xFF) << L":"
        << ((address >> 24) & 0xFF) << L":"
        << ((address >> 16) & 0xFF) << L":"
        << ((address >> 8) & 0xFF) << L":"
        << (address & 0xFF);
    return ss.str();
}

uint64_t StringToBluetoothAddress(const std::wstring& address) {
    uint64_t btAddr = 0;
    int shift = 40;
    for (size_t i = 0; i < address.length(); ++i) {
        if (address[i] != L':') {
            btAddr += static_cast<uint64_t>(std::stoul(address.substr(i, 2), nullptr, 16)) << shift;
            shift -= 8;
            i++; // 한 번에 두 자리를 처리하므로 인덱스를 추가로 증가시킴
        }
    }
    return btAddr;
}

std::deque<std::pair<int, int>> kalHistory; // To store the last 10 kal[0] and kal[1] values

void HandleNotification(GattCharacteristic characteristic, GattValueChangedEventArgs args) {
    try {
        auto reader = DataReader::FromBuffer(args.CharacteristicValue());
        std::vector<uint8_t> raw_data;
        while (reader.UnconsumedBufferLength() > 0) {
            raw_data.push_back(reader.ReadByte());
        }

        if (raw_data.size() == 24) {
            int timestamp;
            int emgValues[3];
            int kal[2];
            memcpy(&timestamp, raw_data.data(), 4);
            memcpy(emgValues, raw_data.data() + 4, 12);
            memcpy(kal, raw_data.data() + 16, 8);
            std::wcout << timestamp << "\t" << emgValues[0] << "\t" << emgValues[1] << "\t" << emgValues[2] << "\t" << kal[0] << "\t" << kal[1] << "\t" << std::endl;


            // EMG 데이터를 큐에 추가
            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                emgDataQueue.push_back(static_cast<float>(emgValues[0]));
                emgDataQueue.push_back(static_cast<float>(emgValues[1]));
                emgDataQueue.push_back(static_cast<float>(emgValues[2]));
            }
            // 30 데이터가 들어왔음을 알림
            if (emgDataQueue.size() == 3 * window) data_condition.notify_one();


            // for stabilize
            // Store the current kal values in the history deque
            if (kalHistory.size() >= 5) {
                kalHistory.pop_front(); // Remove the oldest entry if we already have 10 values
            }
            kalHistory.push_back(std::make_pair(kal[0], kal[1]));

            // Calculate weighted average for kal[0] and kal[1] based on the last 10 values
            const std::vector<float> weights = { 0.1f, 0.15f, 0.2f, 0.25f, 0.3f }; // Weights for averaging
            float weightedKal0 = 0.0f, weightedKal1 = 0.0f;
            float totalWeight = 0.0f;

            // Apply weights
            int size = kalHistory.size();
            for (int i = 0; i < size; ++i) {
                float weight = weights[i % weights.size()];
                weightedKal0 += kalHistory[i].first * weight;
                weightedKal1 += kalHistory[i].second * weight;
                totalWeight += weight;
            }

            // Normalize the result
            weightedKal0 /= totalWeight;
            weightedKal1 /= totalWeight;

            // Move the mouse based on kal values
            INPUT input = { 0 };
            input.type = INPUT_MOUSE;
            input.mi.dx = static_cast<LONG>(weightedKal0 * 20);  // Adjust sensitivity
            input.mi.dy = static_cast<LONG>(-weightedKal1 * 40); // Adjust sensitivity
            input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
            SendInput(1, &input, sizeof(INPUT));

            // Check if any emgValue is greater than 2000 to trigger a mouse click
            auto now = std::chrono::steady_clock::now();
            if ((emgValues[0] > 1900 || emgValues[2] > 1900) &&
                (now - lastClickTime >= std::chrono::seconds(1))) {
                INPUT clickInput = { 0 };
                clickInput.type = INPUT_MOUSE;
                clickInput.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
                SendInput(1, &clickInput, sizeof(INPUT));

                clickInput.mi.dwFlags = MOUSEEVENTF_LEFTUP;
                SendInput(1, &clickInput, sizeof(INPUT));

                // Second click (for double click)
                clickInput.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
                SendInput(1, &clickInput, sizeof(INPUT));

                clickInput.mi.dwFlags = MOUSEEVENTF_LEFTUP;
                SendInput(1, &clickInput, sizeof(INPUT));

                lastClickTime = now; // Update the last click time
            }
        }
        else {
            std::wcerr << L"Received data of incorrect size." << std::endl;
        }
    }
    catch (const std::exception& e) {
        std::wcerr << L"Exception: " << e.what() << std::endl;
    }
    catch (...) {
        std::wcerr << L"Unknown exception occurred." << std::endl;
    }
}

void ReadBLEData(BluetoothLEDevice device) {
    try {
        std::wcout << "reading" << std::endl;
        running = true;
        auto services_result = device.GetGattServicesAsync().get();
        if (services_result.Status() != GattCommunicationStatus::Success) {
            std::wcerr << L"Failed to get GATT services." << std::endl;
            return;
        }

        for (auto service : services_result.Services()) {
            auto characteristics_result = service.GetCharacteristicsAsync().get();
            if (characteristics_result.Status() != GattCommunicationStatus::Success)
                continue;

            for (auto characteristic : characteristics_result.Characteristics()) {
                if (characteristic.Uuid() == winrt::guid(characteristic_uuid)) {
                    characteristic.ValueChanged({ &HandleNotification });
                    std::wcout << "valuechanged" << std::endl;

                    auto status = characteristic.WriteClientCharacteristicConfigurationDescriptorAsync(
                        GattClientCharacteristicConfigurationDescriptorValue::Notify
                    ).get();

                    if (status != GattCommunicationStatus::Success) {
                        std::wcerr << L"Failed to enable notifications." << std::endl;
                        return;
                    }

                    std::wcout << L"Started receiving notifications." << std::endl;

                    while (running) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }

                    std::wcout << "aftersleep" << std::endl;
                    characteristic.WriteClientCharacteristicConfigurationDescriptorAsync(
                        GattClientCharacteristicConfigurationDescriptorValue::None
                    ).get();
                    std::wcout << "writeasync2" << std::endl;
                    break;
                }
            }
        }
    }
    catch (const std::exception& e) {
        std::wcerr << L"Exception: " << e.what() << std::endl;
    }
    catch (...) {
        std::wcerr << L"Unknown exception occurred." << std::endl;
    }
}

void OnAdvertisementReceived(BluetoothLEAdvertisementWatcher watcher, BluetoothLEAdvertisementReceivedEventArgs args) {
    try {
        const std::wstring targetAddress = L"08:d1:f9:fd:40:7e";
        std::wcout << L"Device found: " << BluetoothAddressToString(args.BluetoothAddress()) << std::endl;

        if (BluetoothAddressToString(args.BluetoothAddress()) == targetAddress) {
            std::wcout << L"Target device found: " << BluetoothAddressToString(args.BluetoothAddress()) << std::endl;
            watcher.Stop();

            auto deviceTask = BluetoothLEDevice::FromBluetoothAddressAsync(args.BluetoothAddress());
            deviceTask.Completed([](IAsyncOperation<BluetoothLEDevice> const& asyncOp, AsyncStatus const status) {
                if (status == AsyncStatus::Completed) {
                    BluetoothLEDevice device = asyncOp.GetResults();
                    std::wcout << L"Connected to device: " << BluetoothAddressToString(device.BluetoothAddress()) << std::endl;
                    std::lock_guard<std::mutex> lock(device_mutex);
                    global_device = device;
                }
                else {
                    std::wcerr << L"Failed to connect to the device." << std::endl;
                }
                });
        }
    }
    catch (const std::exception& e) {
        std::wcerr << L"Exception: " << e.what() << std::endl;
    }
    catch (...) {
        std::wcerr << L"Unknown exception occurred." << std::endl;
    }
}

/*
// Function to process and print output tensors
void ProcessOutputTensors(std::vector<Ort::Value>& output_tensors) {
    if (output_tensors.empty()) {
        std::cerr << "No output tensors returned from model inference." << std::endl;
        return;
    }

    std::cout << "Number of output tensors: " << output_tensors.size() << std::endl;
    for (size_t i = 0; i < output_tensors.size(); ++i) {
        Ort::Value& output_tensor = output_tensors[i];

        // Get the type and shape info of the output tensor
        Ort::TensorTypeAndShapeInfo info = output_tensor.GetTensorTypeAndShapeInfo();
        size_t num_elements = info.GetElementCount();
        std::vector<int64_t> shape = info.GetShape();
        std::cout << "Number of elements: " << num_elements << std::endl;

        // Handle the output type as per model specification
        ONNXTensorElementDataType type = info.GetElementType();
        size_t element_size = 0;
        switch (type) {
        case ONNXTensorElementDataType::ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:
            element_size = sizeof(float);
            break;
        case ONNXTensorElementDataType::ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32:
            element_size = sizeof(int32_t);
            break;
        case ONNXTensorElementDataType::ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64:
            element_size = sizeof(int64_t);
            break;
        default:
            std::cerr << "Unsupported data type." << std::endl;
            return;
        }

        // Print the output tensor name and shape
        std::cout << "Output tensor " << i << " shape: ";
        for (const auto& dim : shape) {
            std::cout << dim << " ";
        }
        std::cout << std::endl;

        // Get the data from the tensor
        void* output_data = output_tensor.GetTensorMutableData<void>();

        // Calculate total size in bytes
        size_t total_size_bytes = num_elements * element_size;
        std::cout << "Size of each element: " << element_size << " bytes" << std::endl;
        std::cout << "Total size of tensor data: " << total_size_bytes << " bytes" << std::endl;

        // Print the output data
        std::cout << "Output tensor " << i << " values: ";
        for (size_t j = 0; j < num_elements; ++j) {
            if (type == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
                std::cout << "FLOAT" << std::endl;
                std::cout << static_cast<float*>(output_data)[j] << " ";
            }
            else if (type == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32) {
                std::cout << "32" << std::endl;
                std::cout << static_cast<int32_t*>(output_data)[j] << " ";
            }
            else if (type == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64) {
                std::cout << "64" << std::endl;
                std::cout << static_cast<int64_t*>(output_data)[j] << " ";
            }
        }
        std::cout << std::endl;
    }
}
*/




// ONNX 모델을 로드하고 예측을 수행하는 함수
void predictThread(Ort::Session& session, const Ort::MemoryInfo& memory_info, const std::vector<const char*>& input_names, const std::vector<const char*>& output_names) {
    while (running) {
        //std::cout << "Fcn call" << std::endl;
        std::vector<float> input_data;

        // 데이터를 대기하고 가져오기
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            data_condition.wait(lock);

            if (!running && emgDataQueue.empty())
                break;

            /*std::cout << "good" << std::endl;
            for (int i = 1; i < 92; i++) {
                emgDataQueue.push_back(i);
            }
            std::cout << emgDataQueue.size() << std::endl;
            */
            if (emgDataQueue.size() >= 3 * window) {
                input_data.assign(emgDataQueue.end() - 3 * window, emgDataQueue.end());
                emgDataQueue.clear();
            }
            else {
                std::cerr << "Not enough data in the queue" << std::endl;
                continue;  // 데이터를 기다리기 위해 반복문으로 돌아감
            }
        }

        // 예측 수행
        //std::cout << input_data.size() << std::endl;
        std::vector<int64_t> input_node_dims = { 1, static_cast<int64_t>(input_data.size()) };
        Ort::Value input_tensor = Ort::Value::CreateTensor(
            memory_info,                                   // 메모리 정보
            const_cast<float*>(input_data.data()),         // 실제 데이터 (const 제거 필요)
            input_data.size() * sizeof(float),             // 데이터 크기 (바이트 단위로 전달)
            input_node_dims.data(),                        // 데이터 차원 배열
            input_node_dims.size()                         // 차원 배열의 길이
        );

        // 추론 실행
        //std::cout << "Touched2" << std::endl;
        auto output_tensors = session.Run(Ort::RunOptions{ nullptr }, input_names.data(), &input_tensor, 1, output_names.data(), 1);

        /*
        try {
            auto output_tensors = session.Run(Ort::RunOptions{ nullptr }, input_names.data(), &input_tensor, 1, output_names.data(), 1);
            Ort::Value& output_tensor = output_tensors[0];
            std::cout << "Touched4" << std::endl;
            float* result_data = output_tensor.GetTensorMutableData<float>();
            size_t result_size = output_tensor.GetTensorTypeAndShapeInfo().GetElementCount();
            std::vector<float> result(result_data, result_data + result_size);
        }
        catch (const Ort::Exception& e) {
            std::cerr << "ONNX Runtime Exception: " << e.what() << std::endl;
            return; // Exit or handle error accordingly
        }
        */

        Ort::Value& output_tensor = output_tensors[0];
        void* output_data = output_tensor.GetTensorMutableData<void>();
        std::cout << static_cast<int64_t*>(output_data)[0] << std::endl;

        // ProcessOutputTensors(output_tensors);

        /*
        // Output tensor
        std::cout << "Touched3" << std::endl;
        std::cout << output_tensors.size() << std::endl;
        Ort::Value& output_tensor = output_tensors[0];  // Get the first output tensor

        // Get data pointer and size
        std::cout << "Touched4" << std::endl;
        float* result_data = output_tensor.GetTensorMutableData<float>();
        size_t result_size = output_tensor.GetTensorTypeAndShapeInfo().GetElementCount();

        // Convert to std::vector<float>
        std::vector<float> result(result_data, result_data + result_size);

        // 결과 출력
        std::wcout << L"Predicted Label: " << result[0] << std::endl;
        */
    }
}

int main() {
    try {
        const std::wstring targetDeviceAddress = L"08:d1:f9:fd:40:7e";

        init_apartment();
        std::wcout << L"Waiting for connection..." << std::endl;
        BluetoothLEAdvertisementWatcher watcher;
        watcher.Received(OnAdvertisementReceived);
        watcher.ScanningMode(BluetoothLEScanningMode::Active);

        std::wstring input;
        do {
            std::wcout << L"Starting BLE scan..." << std::endl;
            watcher.Start();
            std::this_thread::sleep_for(std::chrono::seconds(5));
            watcher.Stop();

            if (global_device == nullptr)   std::wcout << L"Couldn't find target device. Want to scan again? [y/n] ";
            else                            std::wcout << L"Type 'start' to begin reading BLE data: ";
            std::wcin >> input;

        } while (input == L"y" | input == L"yes");

        /*
        if (input == L"debug") {
            std::wcout << "debug started" << std::endl;
            // ONNX Runtime 환경 설정
            Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "test");
            Ort::SessionOptions session_options;
            // ONNX 모델 로드, Allocator 및 메모리 정보 설정
            Ort::Session session(env, L"random_forest_model.onnx", session_options);
            Ort::AllocatorWithDefaultOptions allocator;
            Ort::MemoryInfo memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

            // Start the prediction thread
            std::thread prediction_thread(predictThread, std::ref(session), std::ref(memory_info));
            prediction_thread.join();
            std::wcout << "debug ended" << std::endl;
        }
        */

        if (input == L"start") {
            std::wcout << L"Starting BLE data read" << std::endl;

            Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "test");
            Ort::SessionOptions session_options;
            Ort::Session session(env, L"random_forest_model.onnx", session_options);
            Ort::AllocatorWithDefaultOptions allocator;
            Ort::MemoryInfo memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

            std::vector<const char*> input_names = { "float_input" };
            std::vector<const char*> output_names = { "label", "probabilities" };

            // std::cout << "thread start" << std::endl;
            // Start the prediction thread
            std::thread prediction_thread(predictThread, std::ref(session), std::ref(memory_info), std::ref(input_names), std::ref(output_names));

            std::lock_guard<std::mutex> lock(device_mutex);
            if (global_device != nullptr) {
                std::thread ble_thread(ReadBLEData, global_device);
                ble_thread.join();
                prediction_thread.join();
                std::wcout << L"Finished BLE data read" << std::endl;
            }
            else {
                std::wcerr << L"Failed to connect to the BLE device." << std::endl;
            }

            // Stop the prediction thread and join
            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                running = false;
                data_condition.notify_one();  // Wake up prediction thread to exit
            }
            prediction_thread.join();
        }
        else {
            std::wcout << L"Invalid input, exiting." << std::endl;
        }
    }
    catch (const std::exception& e) {
        std::wcerr << L"Exception: " << e.what() << std::endl;
    }
    catch (...) {
        std::wcerr << L"Unknown exception occurred." << std::endl;
    }

    return 0;
}
