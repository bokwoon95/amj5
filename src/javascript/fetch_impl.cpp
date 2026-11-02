#include "fetch_impl.hpp"

#include "errors/errors.hpp"
#include "scheduler.hpp"

#include <ada.h> // IWYU pragma: keep
#include <ada/implementation.h>
#include <ada/url_aggregator.h>
#include <httplib.h>
#include <quickjs.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <expected>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace amj::javascript {
namespace chrono = std::chrono;
namespace errors = amj::errors;
namespace {

// NOLINTBEGIN(bugprone-throwing-static-initialization): JS_MKVAL only constructs scalars.
const JSValue undefinedValue = JS_UNDEFINED;
const JSValue nullValue = JS_NULL;
const JSValue exceptionValue = JS_EXCEPTION;
// NOLINTEND(bugprone-throwing-static-initialization)
constexpr std::size_t headerLimit = 65536;
constexpr std::size_t bodyLimit = 33554432;
constexpr std::size_t chunkLimit = 65536;
constexpr std::size_t bufferLimit = 262144;

class JSValueOwner {
  public:
    JSContext* jsContext = nullptr;
    JSValue value = undefinedValue;
    JSValueOwner(JSContext* jsContext, JSValue value) {
        this->jsContext = jsContext;
        this->value = value;
    }
    ~JSValueOwner() {
        JS_FreeValue(jsContext, value);
    }
    JSValueOwner(const JSValueOwner&) = delete;
    JSValueOwner& operator=(const JSValueOwner&) = delete;
    JSValue release() {
        return std::exchange(value, undefinedValue);
    }
};

struct Failure {
    std::string type = "";
    std::string message = "";
};

// Only this native data is accessed by the worker. The owner retains it until
// stop() joins the worker. Scheduler notifications happen outside this mutex.
struct Transport {
    Scheduler* scheduler = nullptr;
    std::unique_ptr<httplib::Client> client = nullptr;
    std::mutex mutex = {};
    std::condition_variable condition = {};
    std::deque<std::vector<unsigned char>> chunks = {};
    std::size_t bufferedBytes = 0;
    int status = 0;
    std::vector<std::pair<std::string, std::string>> headers = {};
    std::optional<Failure> failure = {};
    std::exception_ptr workerException = nullptr;
    bool headersReceived = false;
    bool finished = false;
    bool canceled = false;
    int64_t headersID = 0; // Immutable after starting the worker.
    int64_t readID = 0;    // Protected by mutex.
    std::jthread worker = {};

    bool receiveHeaders(const httplib::Response& response) {
        {
            const std::lock_guard<std::mutex> lock(mutex);
            if (canceled || failure.has_value()) {
                return false;
            }
            if (headersReceived) {
                return true;
            }
            if (response.status == 301 || response.status == 302 || response.status == 303 || response.status == 307 || response.status == 308) {
                failure = Failure{
                    .type = "RedirectRejected",
                    .message = "HTTP redirect rejected",
                };
                return false;
            }
            std::size_t bytes = 0;
            for (const httplib::Headers::value_type& header : response.headers) {
                for (const std::string* field : {&header.first, &header.second}) {
                    if (field->size() > headerLimit - bytes) {
                        failure = Failure{
                            .type = "LimitExceeded",
                            .message = "HTTP response headers exceed 64 KiB",
                        };
                        return false;
                    }
                    bytes += field->size();
                }
                headers.push_back({header.first, header.second});
            }
            status = response.status;
            headersReceived = true;
        }
        const errors::Error error = scheduler->markReady(headersID);
        if (error.errorCode) {
            throw std::runtime_error(error.toString());
        }
        return true;
    }

    bool receiveBody(const char* data, std::size_t length) {
        std::size_t offset = 0;
        while (offset < length) {
            const std::size_t count = std::min(length - offset, chunkLimit);
            int64_t callbackID = 0;
            {
                std::unique_lock<std::mutex> lock(mutex);
                condition.wait(lock, [this, count]() {
                    return canceled || bufferedBytes <= bufferLimit - count;
                });
                if (canceled) {
                    return false;
                }
                // httplib provides length accessible bytes. offset/count are
                // bounded by length; no buffer points into JS-owned memory.
                chunks.emplace_back(data + offset, data + offset + count);
                bufferedBytes += count;
                callbackID = readID;
            }
            if (callbackID != 0) {
                const errors::Error error = scheduler->markReady(callbackID);
                if (error.errorCode) {
                    throw std::runtime_error(error.toString());
                }
            }
            offset += count;
        }
        return true;
    }

    void start(httplib::Request request) {
        request.response_handler = [this](const httplib::Response& response) {
            return receiveHeaders(response);
        };
        request.content_receiver = [this](const char* data, std::size_t length, std::size_t, std::size_t) {
            return receiveBody(data, length);
        };
        // NOLINTNEXTLINE(bugprone-exception-escape): Native failures are retained; failed final notification explicitly terminates.
        worker = std::jthread([this, request]() mutable {
            try {
                bool stopped = false;
                {
                    const std::lock_guard<std::mutex> lock(mutex);
                    stopped = canceled;
                }
                if (!stopped) {
                    const httplib::Result result = client->send(request);
                    if (result) {
                        receiveHeaders(result.value());
                    } else {
                        const std::lock_guard<std::mutex> lock(mutex);
                        if (!failure.has_value() && !canceled) {
                            failure = Failure{
                                .type = "TransportError",
                                .message = "HTTP transport failed: " + httplib::to_string(result.error()),
                            };
                        }
                    }
                }
            } catch (...) {
                const std::lock_guard<std::mutex> lock(mutex);
                workerException = std::current_exception();
            }
            int64_t callbackID = 0;
            {
                const std::lock_guard<std::mutex> lock(mutex);
                finished = true;
                callbackID = readID;
            }
            condition.notify_all();
            try {
                const errors::Error headersError = scheduler->markReady(headersID);
                if (headersError.errorCode) {
                    std::terminate();
                }
                if (callbackID != 0) {
                    const errors::Error readError = scheduler->markReady(callbackID);
                    if (readError.errorCode) {
                        std::terminate();
                    }
                }
            } catch (...) {
                std::terminate(); // Do not leave the owner waiting forever.
            }
        });
    }

    void stop() {
        {
            const std::lock_guard<std::mutex> lock(mutex);
            canceled = true;
            chunks.clear();
            bufferedBytes = 0;
        }
        condition.notify_all();
        // stop() is not sticky: repeat to cover send()/connect starting after it.
        while (worker.joinable()) {
            client->stop();
            std::unique_lock<std::mutex> lock(mutex);
            if (finished) {
                break;
            }
            condition.wait_for(lock, chrono::milliseconds(10));
        }
        if (worker.joinable()) {
            worker.join();
        }
    }

    ~Transport() {
        try {
            stop();
        } catch (...) {
            std::terminate();
        }
    }
};

} // namespace

struct RequestState {
    JSContext* jsContext = nullptr;
    int64_t requestID = 0;
    int64_t headersID = 0;
    int64_t readID = 0;
    std::unique_ptr<Transport> transport = nullptr;
    JSValue onHeaders = undefinedValue;
    JSValue onRead = undefinedValue;
    bool headersDelivered = false;
    bool closed = false;
    ~RequestState() {
        JS_FreeValue(jsContext, onHeaders);
        JS_FreeValue(jsContext, onRead);
    }
};

struct FetchState {
    JSContext* jsContext = nullptr;
    Scheduler* scheduler = nullptr;
    std::map<int64_t, std::shared_ptr<RequestState>> requests = {};
    int64_t lastRequestID = 0;
    bool active = false;
};

namespace {

struct NativeHandle {
    std::unique_ptr<FetchImpl> fetchImpl = nullptr;
    std::weak_ptr<FetchState> owner = {};
    std::weak_ptr<RequestState> requestState = {};
};
std::mutex classIDMutex = {};
std::map<JSRuntime*, JSClassID> classIDMap = {}; // NOLINT(bugprone-throwing-static-initialization): startup allocation failure may terminate.

void releaseClassID(JSRuntime* jsRuntime, void* /* argument */) {
    const std::lock_guard<std::mutex> lock(classIDMutex);
    classIDMap.erase(jsRuntime);
}

JSClassID getClassID(JSRuntime* jsRuntime) {
    const std::lock_guard<std::mutex> lock(classIDMutex);
    if (classIDMap.contains(jsRuntime)) {
        return classIDMap.at(jsRuntime);
    }
    JSClassID classID = 0;
    JS_NewClassID(jsRuntime, &classID);
    classIDMap.insert_or_assign(jsRuntime, classID);
    if (JS_AddRuntimeFinalizer(jsRuntime, releaseClassID, nullptr) < 0) {
        classIDMap.erase(jsRuntime);
        throw std::bad_alloc();
    }
    return classID;
}

void finalizeHandle(JSRuntime* /* jsRuntime */, JSValue value) {
    const std::unique_ptr<NativeHandle> nativeHandle(static_cast<NativeHandle*>(JS_GetOpaque(value, JS_GetClassID(value))));
}

std::shared_ptr<RequestState> getRequest(JSContext* jsContext, JSValueConst handle, const std::shared_ptr<FetchState>& fetchState) {
    const NativeHandle* nativeHandle = static_cast<NativeHandle*>(JS_GetOpaque2(jsContext, handle, getClassID(JS_GetRuntime(jsContext))));
    if (nativeHandle == nullptr) {
        return nullptr;
    }
    if (nativeHandle->fetchImpl != nullptr || nativeHandle->owner.lock() != fetchState) {
        JS_ThrowTypeError(jsContext, "Request belongs to a different native Fetch instance");
        return nullptr;
    }
    return nativeHandle->requestState.lock();
}

void closeRequest(const std::shared_ptr<RequestState>& requestState, const std::shared_ptr<FetchState>& fetchState) {
    if (requestState->closed) {
        return;
    }
    requestState->closed = true;
    const errors::Error headersError = fetchState->scheduler->unregisterCallback(requestState->headersID);
    const errors::Error readError = fetchState->scheduler->unregisterCallback(requestState->readID);
    if (headersError.errorCode || readError.errorCode) {
        throw std::runtime_error("Could not unregister HTTP callbacks");
    }
    requestState->transport->stop();
    JS_FreeValue(requestState->jsContext, std::exchange(requestState->onHeaders, undefinedValue));
    JS_FreeValue(requestState->jsContext, std::exchange(requestState->onRead, undefinedValue));
    fetchState->requests.erase(requestState->requestID);
}

// Converts native completion data only. JS creates Errors and settles Promises.
errors::Error deliver(const std::shared_ptr<RequestState>& requestState, bool reading) {
    if (requestState->closed) {
        return {};
    }
    std::optional<Failure> failure;
    int status = 0;
    std::vector<std::pair<std::string, std::string>> headers;
    std::vector<unsigned char> chunk;
    bool done = false;
    {
        const std::lock_guard<std::mutex> lock(requestState->transport->mutex);
        if (requestState->transport->workerException != nullptr) {
            std::rethrow_exception(requestState->transport->workerException);
        }
        if (reading) {
            requestState->transport->readID = 0;
            if (!requestState->transport->chunks.empty()) {
                chunk = requestState->transport->chunks.front();
                requestState->transport->chunks.pop_front();
                requestState->transport->bufferedBytes -= chunk.size();
            } else if (requestState->transport->finished) {
                failure = requestState->transport->failure;
                done = true;
            } else {
                throw std::logic_error("HTTP read notified without data or completion");
            }
        } else if (requestState->transport->headersReceived) {
            status = requestState->transport->status;
            headers = requestState->transport->headers;
        } else {
            failure = requestState->transport->failure;
            if (!failure.has_value()) {
                failure = Failure{
                    .type = "TransportError",
                    .message = "HTTP request ended before response headers",
                };
            }
        }
    }
    requestState->transport->condition.notify_all();
    JSContext* jsContext = requestState->jsContext;
    JSValueOwner callback(jsContext, undefinedValue);
    if (reading) {
        requestState->readID = 0;
        callback.value = std::exchange(requestState->onRead, undefinedValue);
    } else {
        requestState->headersID = 0;
        requestState->headersDelivered = !failure.has_value();
        callback.value = std::exchange(requestState->onHeaders, undefinedValue);
    }
    if (JS_IsUndefined(callback.value)) {
        return {};
    }
    JSValueOwner failureValue(jsContext, nullValue);
    JSValueOwner data(jsContext, nullValue);
    if (failure.has_value()) {
        failureValue.value = JS_NewObjectProto(jsContext, nullValue);
        if (!JS_IsException(failureValue.value)) {
            const std::array<std::pair<const char*, std::string>, 2> fields = {{{"type", failure.value().type}, {"message", failure.value().message}}};
            for (const std::pair<const char*, std::string>& field : fields) {
                const JSValue value = JS_NewStringLen(jsContext, field.second.data(), field.second.size());
                if (JS_IsException(value) || JS_DefinePropertyValueStr(jsContext, failureValue.value, field.first, value, JS_PROP_C_W_E) < 0) {
                    break;
                }
            }
        }
    } else if (reading && !done) {
        const JSValueOwner buffer(jsContext, JS_NewArrayBufferCopy(jsContext, chunk.data(), chunk.size()));
        if (!JS_IsException(buffer.value)) {
            // QuickJS-NG's public helper reads offset/length slots even with argc=1.
            std::array<JSValue, 3> arguments = {buffer.value, undefinedValue, undefinedValue};
            data.value = JS_NewTypedArray(jsContext, static_cast<int>(arguments.size()), arguments.data(), JS_TYPED_ARRAY_UINT8);
        }
    } else if (!reading) {
        data.value = JS_NewObjectProto(jsContext, nullValue);
        JSValueOwner pairs(jsContext, JS_NewArray(jsContext));
        if (!JS_IsException(data.value) && !JS_IsException(pairs.value)) {
            uint32_t index = 0;
            for (const std::pair<std::string, std::string>& header : headers) {
                JSValueOwner pair(jsContext, JS_NewArray(jsContext));
                if (JS_IsException(pair.value)) {
                    break;
                }
                const std::array<std::string_view, 2> fields = {header.first, header.second};
                for (uint32_t field = 0; field < fields.size(); field++) {
                    const std::string_view text = fields.at(field);
                    const JSValue value = JS_NewStringLen(jsContext, text.data(), text.size());
                    if (JS_IsException(value) || JS_DefinePropertyValueUint32(jsContext, pair.value, field, value, JS_PROP_C_W_E) < 0) {
                        break;
                    }
                }
                if (JS_HasException(jsContext) || JS_DefinePropertyValueUint32(jsContext, pairs.value, index, pair.release(), JS_PROP_C_W_E) < 0) {
                    break;
                }
                index++;
            }
            if (!JS_HasException(jsContext) && JS_DefinePropertyValueStr(jsContext, data.value, "status", JS_NewInt32(jsContext, status), JS_PROP_C_W_E) >= 0) {
                JS_DefinePropertyValueStr(jsContext, data.value, "headers", pairs.release(), JS_PROP_C_W_E);
            }
        }
    }
    if (JS_HasException(jsContext)) {
        return errors::Error{
            .errorCode = ErrorCode::JSException,
            .errorMessage = "Could not convert HTTP completion data",
        };
    }
    std::array<JSValue, 2> arguments = {failureValue.value, data.value};
    const JSValueOwner result(jsContext, JS_Call(jsContext, callback.value, undefinedValue, static_cast<int>(arguments.size()), arguments.data()));
    if (JS_IsException(result.value)) {
        return errors::Error{
            .errorCode = ErrorCode::JSException,
            .errorMessage = "HTTP completion callback threw",
        };
    }
    return {};
}

std::optional<std::string> readString(JSContext* jsContext, JSValueConst value, std::size_t limit) {
    if (!JS_IsString(value)) {
        JS_ThrowTypeError(jsContext, "Native Fetch requires strings");
        return std::nullopt;
    }
    std::size_t length = 0;
    const char* data = JS_ToCStringLen(jsContext, &length, value);
    if (data == nullptr) {
        return std::nullopt;
    }
    const auto release = [jsContext](const char* text) {
        JS_FreeCString(jsContext, text);
    };
    const std::unique_ptr<const char, decltype(release)> ownedText(data, release);
    if (length > limit) {
        JS_ThrowRangeError(jsContext, "HTTP string exceeds its size limit");
        return std::nullopt;
    }
    return std::string(data, length);
}

bool isToken(const std::string& text) {
    if (text.empty()) {
        return false;
    }
    for (const char character : text) {
        if ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9')) {
            continue;
        }
        if (std::string_view("!#$%&'*+-.^_`|~").find(character) == std::string_view::npos) {
            return false;
        }
    }
    return true;
}

JSValue fetchOperation(JSContext* jsContext, JSValueConst /* thisValue */, int argc, JSValueConst* argv, int operation, JSValueConst* functionData) {
    const NativeHandle* nativeHandle = static_cast<NativeHandle*>(JS_GetOpaque(functionData[0], JS_GetClassID(functionData[0])));
    if (nativeHandle == nullptr || nativeHandle->fetchImpl == nullptr) {
        return JS_ThrowInternalError(jsContext, "Missing native Fetch instance");
    }
    if (operation == 0 && argc >= 5) {
        return nativeHandle->fetchImpl->start(jsContext, argv[0], argv[1], argv[2], argv[3], argv[4]);
    }
    if (operation == 1 && argc >= 2) {
        return nativeHandle->fetchImpl->read(jsContext, argv[0], argv[1]);
    }
    if (operation == 2 && argc >= 1) {
        return nativeHandle->fetchImpl->close(jsContext, argv[0]);
    }
    return JS_ThrowTypeError(jsContext, "Missing native Fetch arguments");
}

} // namespace

JSValue FetchImpl::start(JSContext* jsContext, JSValueConst url, JSValueConst method, JSValueConst headerPairs, JSValueConst bytes, JSValueConst onHeaders) try {
    if (fetchState == nullptr || !fetchState->active || fetchState->jsContext != jsContext) {
        return JS_ThrowInternalError(jsContext, "Native Fetch instance is inactive or belongs to another context");
    }
    if (!JS_IsFunction(jsContext, onHeaders)) {
        return JS_ThrowTypeError(jsContext, "Headers completion must be a function");
    }
    const std::optional<std::string> urlText = readString(jsContext, url, headerLimit);
    const std::optional<std::string> methodText = readString(jsContext, method, 128);
    if (!urlText.has_value() || !methodText.has_value()) {
        return exceptionValue;
    }
    const ada::result<ada::url_aggregator> parsed = ada::parse<ada::url_aggregator>(urlText.value(), nullptr);
    if (!parsed.has_value() || (parsed.value().get_protocol() != "http:" && parsed.value().get_protocol() != "https:") || !parsed.value().get_username().empty() || !parsed.value().get_password().empty()) {
        return JS_ThrowTypeError(jsContext, "Only HTTP(S) URLs without credentials are supported");
    }
    if (parsed.value().get_href().size() > headerLimit) {
        return JS_ThrowRangeError(jsContext, "Normalized HTTP URL exceeds 64 KiB");
    }
    httplib::Request request;
    request.method = methodText.value();
    for (const char character : request.method) {
        if (character >= 'a' && character <= 'z') {
            return JS_ThrowTypeError(jsContext, "Native HTTP methods must be normalized to uppercase");
        }
    }
    if (!isToken(request.method) || request.method == "CONNECT" || request.method == "TRACE" || request.method == "TRACK") {
        return JS_ThrowTypeError(jsContext, "Invalid HTTP method");
    }
    request.path = std::string(parsed.value().get_pathname()) + std::string(parsed.value().get_search());
    if (request.path.empty()) {
        request.path = "/";
    }
    int64_t count = 0;
    if (!JS_IsArray(headerPairs)) {
        return JS_ThrowTypeError(jsContext, "Headers must be an array of pairs");
    }
    if (JS_GetLength(jsContext, headerPairs, &count) < 0) {
        return exceptionValue;
    }
    if (count < 0 || count > static_cast<int64_t>(headerLimit)) {
        return JS_ThrowRangeError(jsContext, "Too many HTTP headers");
    }
    std::size_t headerBytes = 0;
    for (int64_t index = 0; index < count; index++) {
        const JSValueOwner pair(jsContext, JS_GetPropertyUint32(jsContext, headerPairs, static_cast<uint32_t>(index)));
        if (JS_IsException(pair.value)) {
            return exceptionValue;
        }
        int64_t length = 0;
        if (!JS_IsArray(pair.value)) {
            return JS_ThrowTypeError(jsContext, "Expected header pairs");
        }
        if (JS_GetLength(jsContext, pair.value, &length) < 0) {
            return exceptionValue;
        }
        if (length != 2) {
            return JS_ThrowTypeError(jsContext, "Expected two fields per header pair");
        }
        const JSValueOwner name(jsContext, JS_GetPropertyUint32(jsContext, pair.value, 0));
        const JSValueOwner value(jsContext, JS_GetPropertyUint32(jsContext, pair.value, 1));
        if (JS_IsException(name.value) || JS_IsException(value.value)) {
            return exceptionValue;
        }
        const std::optional<std::string> nameText = readString(jsContext, name.value, headerLimit);
        const std::optional<std::string> valueText = readString(jsContext, value.value, headerLimit);
        if (!nameText.has_value() || !valueText.has_value()) {
            return exceptionValue;
        }
        std::string lowerName = nameText.value();
        for (char& character : lowerName) {
            if (character >= 'A' && character <= 'Z') {
                character = static_cast<char>(character + ('a' - 'A'));
            }
        }
        if (!isToken(lowerName) || lowerName == "host" || lowerName == "content-length" || lowerName == "transfer-encoding" || lowerName == "connection" || lowerName == "upgrade" || lowerName == "expect" || lowerName.starts_with("proxy-")) {
            return JS_ThrowTypeError(jsContext, "Invalid or transport-controlled HTTP header");
        }
        for (const char character : valueText.value()) {
            if ((static_cast<unsigned char>(character) < 32 && character != '\t') || character == 127) {
                return JS_ThrowTypeError(jsContext, "Invalid HTTP header value");
            }
        }
        const std::array<const std::string*, 2> headerFields = {&lowerName, &valueText.value()};
        for (const std::string* field : headerFields) {
            if (field->size() > headerLimit - headerBytes) {
                return JS_ThrowRangeError(jsContext, "HTTP headers exceed 64 KiB");
            }
            headerBytes += field->size();
        }
        request.headers.insert({lowerName, valueText.value()});
    }
    std::size_t offset = 0;
    std::size_t length = 0;
    std::size_t elementSize = 0;
    const JSValueOwner buffer(jsContext, JS_GetTypedArrayBuffer(jsContext, bytes, &offset, &length, &elementSize));
    if (JS_IsException(buffer.value)) {
        return exceptionValue;
    }
    if (elementSize != 1 || length > bodyLimit || (length != 0 && (request.method == "GET" || request.method == "HEAD"))) {
        return JS_ThrowTypeError(jsContext, "Invalid HTTP body type, size, or method");
    }
    if (length != 0) {
        std::size_t bufferLength = 0;
        const uint8_t* data = JS_GetArrayBuffer(jsContext, &bufferLength, buffer.value);
        if (data == nullptr) {
            return exceptionValue;
        }
        if (offset > bufferLength || length > bufferLength - offset) {
            return JS_ThrowRangeError(jsContext, "HTTP body is outside its buffer");
        }
        request.body.assign(data + offset, data + offset + length);
    }
    // Property getters above can reenter JS and clear/start requests.
    if (!fetchState->active) {
        return JS_ThrowInternalError(jsContext, "Native Fetch instance was cleared");
    }
    if (fetchState->requests.size() >= 64 || fetchState->lastRequestID == std::numeric_limits<int64_t>::max()) {
        return JS_ThrowRangeError(jsContext, "HTTP request limit or IDs exhausted");
    }
    const std::shared_ptr<RequestState> requestState = std::make_shared<RequestState>();
    requestState->jsContext = jsContext;
    requestState->requestID = ++fetchState->lastRequestID;
    requestState->onHeaders = JS_DupValue(jsContext, onHeaders);
    requestState->transport = std::make_unique<Transport>();
    requestState->transport->scheduler = fetchState->scheduler;
    requestState->transport->client = std::make_unique<httplib::Client>(parsed.value().get_origin());
    httplib::Client& client = *requestState->transport->client;
    if (!client.is_valid()) {
        return JS_ThrowTypeError(jsContext, "Invalid HTTP origin or unavailable TLS backend");
    }
    client.set_connection_timeout(10, 0);
    client.set_read_timeout(60, 0);
    client.set_write_timeout(30, 0);
    client.set_follow_location(false);
    client.set_keep_alive(false);
    client.set_path_encode(false);
    client.set_decompress(true);
    client.enable_server_certificate_verification(true);
    client.enable_server_hostname_verification(true);
    const JSValueOwner handle(jsContext, JS_NewObjectProtoClass(jsContext, nullValue, getClassID(JS_GetRuntime(jsContext))));
    if (JS_IsException(handle.value)) {
        return exceptionValue;
    }
    std::unique_ptr<NativeHandle> nativeHandle = std::make_unique<NativeHandle>();
    nativeHandle->owner = fetchState;
    nativeHandle->requestState = requestState;
    JS_SetOpaque(handle.value, nativeHandle.release());
    try {
        fetchState->requests.insert_or_assign(requestState->requestID, requestState);
        const std::weak_ptr<RequestState> weakRequestState = requestState;
        const std::expected<int64_t, errors::Error> callbackIDResult = fetchState->scheduler->registerCallback([weakRequestState]() -> errors::Error {
            const std::shared_ptr<RequestState> requestState = weakRequestState.lock();
            if (requestState == nullptr) {
                return {};
            }
            return deliver(requestState, false);
        },
                                                                                                               std::nullopt);
        if (!callbackIDResult.has_value()) {
            throw std::runtime_error(callbackIDResult.error().toString());
        }
        requestState->headersID = callbackIDResult.value();
        requestState->transport->headersID = requestState->headersID;
        requestState->transport->start(request);
    } catch (...) {
        closeRequest(requestState, fetchState);
        throw;
    }
    return JS_DupValue(jsContext, handle.value);
} catch (const std::bad_alloc&) {
    return JS_ThrowOutOfMemory(jsContext);
} catch (const std::exception& exception) {
    if (JS_HasException(jsContext)) {
        return exceptionValue;
    }
    return JS_ThrowInternalError(jsContext, "%s", exception.what());
} catch (...) {
    if (JS_HasException(jsContext)) {
        return exceptionValue;
    }
    return JS_ThrowInternalError(jsContext, "Unexpected native HTTP start failure");
}

JSValue FetchImpl::read(JSContext* jsContext, JSValueConst request, JSValueConst onRead) try {
    if (fetchState == nullptr || !fetchState->active || fetchState->jsContext != jsContext) {
        return JS_ThrowInternalError(jsContext, "Native Fetch instance is inactive or belongs to another context");
    }
    const std::shared_ptr<RequestState> requestState = getRequest(jsContext, request, fetchState);
    if (requestState == nullptr) {
        if (JS_HasException(jsContext)) {
            return exceptionValue;
        }
        return JS_ThrowTypeError(jsContext, "HTTP request is closed");
    }
    if (requestState->closed || !requestState->headersDelivered || !JS_IsUndefined(requestState->onRead)) {
        return JS_ThrowTypeError(jsContext, "HTTP read requires headers and no outstanding read");
    }
    if (!JS_IsFunction(jsContext, onRead)) {
        return JS_ThrowTypeError(jsContext, "Read completion must be a function");
    }
    requestState->onRead = JS_DupValue(jsContext, onRead);
    try {
        const std::weak_ptr<RequestState> weakRequestState = requestState;
        const std::expected<int64_t, errors::Error> callbackIDResult = fetchState->scheduler->registerCallback([weakRequestState]() -> errors::Error {
            const std::shared_ptr<RequestState> requestState = weakRequestState.lock();
            if (requestState == nullptr) {
                return {};
            }
            return deliver(requestState, true);
        },
                                                                                                               std::nullopt);
        if (!callbackIDResult.has_value()) {
            throw std::runtime_error(callbackIDResult.error().toString());
        }
        requestState->readID = callbackIDResult.value();
        bool ready = false;
        {
            const std::lock_guard<std::mutex> lock(requestState->transport->mutex);
            requestState->transport->readID = requestState->readID;
            ready = !requestState->transport->chunks.empty() || requestState->transport->finished;
        }
        if (ready) {
            const errors::Error error = fetchState->scheduler->markReady(requestState->readID);
            if (error.errorCode) {
                throw std::runtime_error(error.toString());
            }
        }
    } catch (...) {
        const errors::Error error = fetchState->scheduler->unregisterCallback(requestState->readID);
        if (error.errorCode) {
            std::terminate();
        }
        JS_FreeValue(jsContext, std::exchange(requestState->onRead, undefinedValue));
        throw;
    }
    return undefinedValue;
} catch (const std::bad_alloc&) {
    return JS_ThrowOutOfMemory(jsContext);
} catch (const std::exception& exception) {
    if (JS_HasException(jsContext)) {
        return exceptionValue;
    }
    return JS_ThrowInternalError(jsContext, "%s", exception.what());
} catch (...) {
    if (JS_HasException(jsContext)) {
        return exceptionValue;
    }
    return JS_ThrowInternalError(jsContext, "Unexpected native HTTP read failure");
}

JSValue FetchImpl::close(JSContext* jsContext, JSValueConst request) try {
    if (fetchState == nullptr || fetchState->jsContext != jsContext) {
        return JS_ThrowTypeError(jsContext, "Native Fetch belongs to another context");
    }
    const std::shared_ptr<RequestState> requestState = getRequest(jsContext, request, fetchState);
    if (requestState == nullptr) {
        if (JS_HasException(jsContext)) {
            return exceptionValue;
        }
        return undefinedValue;
    }
    if (fetchState->active) {
        closeRequest(requestState, fetchState);
    }
    return undefinedValue;
} catch (const std::bad_alloc&) {
    return JS_ThrowOutOfMemory(jsContext);
} catch (const std::exception& exception) {
    if (JS_HasException(jsContext)) {
        return exceptionValue;
    }
    return JS_ThrowInternalError(jsContext, "%s", exception.what());
} catch (...) {
    if (JS_HasException(jsContext)) {
        return exceptionValue;
    }
    return JS_ThrowInternalError(jsContext, "Unexpected native HTTP close failure");
}

JSValue FetchImpl::create(JSContext* jsContext, Scheduler& scheduler) try {
    JSRuntime* jsRuntime = JS_GetRuntime(jsContext);
    const JSClassID classID = getClassID(jsRuntime);
    if (!JS_IsRegisteredClass(jsRuntime, classID)) {
        const JSClassDef definition = {
            .class_name = "FetchImpl",
            .finalizer = finalizeHandle,
            .gc_mark = nullptr,
            .call = nullptr,
            .exotic = nullptr,
        };
        if (JS_NewClass(jsRuntime, classID, &definition) < 0) {
            return JS_ThrowOutOfMemory(jsContext);
        }
    }
    const std::shared_ptr<FetchState> fetchState = std::make_shared<FetchState>();
    fetchState->jsContext = jsContext;
    fetchState->scheduler = &scheduler;
    std::unique_ptr<NativeHandle> nativeHandle = std::make_unique<NativeHandle>();
    nativeHandle->fetchImpl = std::make_unique<FetchImpl>();
    nativeHandle->fetchImpl->fetchState = fetchState;
    JSValueOwner object(jsContext, JS_NewObjectProtoClass(jsContext, nullValue, classID));
    if (JS_IsException(object.value)) {
        return exceptionValue;
    }
    JS_SetOpaque(object.value, nativeHandle.release());
    const std::array<const char*, 3> names = {"start", "read", "close"};
    const std::array<int, 3> argumentCounts = {5, 2, 1};
    for (std::size_t index = 0; index < names.size(); index++) {
        JSValue functionData = object.value;
        const JSValue function = JS_NewCFunctionData(jsContext, fetchOperation, argumentCounts.at(index), static_cast<int>(index), 1, &functionData);
        if (JS_IsException(function) || JS_DefinePropertyValueStr(jsContext, object.value, names.at(index), function, JS_PROP_C_W_E) < 0) {
            return exceptionValue;
        }
    }
    const std::expected<int64_t, errors::Error> cleanupIDResult = scheduler.registerCleanup([fetchState]() -> errors::Error {
        fetchState->active = false;
        std::map<int64_t, std::shared_ptr<RequestState>> requests;
        requests.swap(fetchState->requests);
        errors::Error firstError;
        for (const std::pair<const int64_t, std::shared_ptr<RequestState>>& entry : requests) {
            try {
                closeRequest(entry.second, fetchState);
            } catch (const std::exception& exception) {
                if (!firstError.errorCode) {
                    firstError = errors::message(exception.what());
                }
            } catch (...) {
                if (!firstError.errorCode) {
                    firstError = errors::message("Unexpected HTTP cleanup failure");
                }
            }
        }
        return firstError;
    });
    if (!cleanupIDResult.has_value()) {
        return JS_ThrowInternalError(jsContext, "%s", cleanupIDResult.error().toString().c_str());
    }
    fetchState->active = true;
    return object.release();
} catch (const std::bad_alloc&) {
    return JS_ThrowOutOfMemory(jsContext);
} catch (const std::exception& exception) {
    if (JS_HasException(jsContext)) {
        return exceptionValue;
    }
    return JS_ThrowInternalError(jsContext, "%s", exception.what());
} catch (...) {
    if (JS_HasException(jsContext)) {
        return exceptionValue;
    }
    return JS_ThrowInternalError(jsContext, "Unexpected native Fetch creation failure");
}

JSModuleDef* FetchImpl::createModule(JSContext* jsContext, Scheduler& scheduler) {
    JSValueOwner fetchImpl(jsContext, FetchImpl::create(jsContext, scheduler));
    if (JS_IsException(fetchImpl.value)) {
        return nullptr;
    }
    JSModuleDef* module = JS_NewCModule(jsContext, "amj:fetch", [](JSContext* jsContext, JSModuleDef* module) -> int {
        const JSValue fetchImpl = JS_GetModulePrivateValue(jsContext, module);
        if (JS_IsException(fetchImpl)) {
            return -1;
        }
        return JS_SetModuleExport(jsContext, module, "fetchImpl", fetchImpl);
    });
    if (module == nullptr || JS_AddModuleExport(jsContext, module, "fetchImpl") < 0) {
        return nullptr;
    }
    if (JS_SetModulePrivateValue(jsContext, module, fetchImpl.release()) < 0) {
        return nullptr;
    }
    return module;
}

} // namespace amj::javascript
