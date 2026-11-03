#pragma once
#include <windows.h>
#include <winhttp.h>
#include <string>
#include <thread>
#include <mutex>
#include <vector>
#include "settings.h"
#include "json.h"

class AICompanion {
public:
    std::wstring lastPrompt = L"";
    std::wstring lastResponse = L"Hola, soy tu companero Capri. En que te puedo ayudar hoy?";
    bool isThinking = false;
    std::mutex aiMutex;

    // Endpoint and key, written by the app (Configure/SetKey) under aiMutex; Ask copies them before its worker starts
    std::wstring apiHost = L"api.openai.com";
    std::wstring apiPath = L"/v1/chat/completions";
    std::wstring model = L"gpt-4o-mini";
    int apiPort = INTERNET_DEFAULT_HTTPS_PORT;
    bool useHttps = true;
    bool needKey = true; // false for a local server, which answers without a key
    std::wstring apiKey = L"";

    void Configure(const AiEndpoint& e) {
        std::lock_guard<std::mutex> lock(aiMutex);
        apiHost = e.host;
        apiPath = e.path;
        model = e.model;
        apiPort = e.port;
        useHttps = e.https;
        needKey = e.needKey;
    }

    void SetKey(const std::wstring& key) {
        std::lock_guard<std::mutex> lock(aiMutex);
        apiKey = key;
    }

    bool HasKey() {
        std::lock_guard<std::mutex> lock(aiMutex);
        return !apiKey.empty();
    }

    void Ask(const std::wstring& prompt) {
        if (isThinking || prompt.empty()) return;
        isThinking = true;
        lastPrompt = prompt;

        std::wstring host, path, model, key;
        int port;
        bool https, needKey;
        {
            std::lock_guard<std::mutex> lock(aiMutex);
            host = apiHost; path = apiPath; model = this->model; port = apiPort; https = useHttps; needKey = this->needKey; key = apiKey;
        }

        std::thread([this, prompt, host, path, model, key, port, https, needKey]() {
            {
                std::lock_guard<std::mutex> lock(aiMutex);
                lastResponse = L"Pensando respuesta en la nube...";
            }

            // If no API key configured yet, give friendly guidance
            if (key.empty() && needKey) {
                {
                    std::lock_guard<std::mutex> lock(aiMutex);
                    lastResponse = L"Para usar el companero IA sin gastar RAM, ingresa tu API Key en la configuracion o ejecuta Ollama local en el puerto 11434.";
                    isThinking = false;
                }
                return;
            }

            // Convert prompt to UTF-8 JSON payload
            std::string promptUtf8;
            int len = WideCharToMultiByte(CP_UTF8, 0, prompt.c_str(), -1, NULL, 0, NULL, NULL);
            if (len > 0) {
                promptUtf8.resize(len - 1);
                WideCharToMultiByte(CP_UTF8, 0, prompt.c_str(), -1, &promptUtf8[0], len, NULL, NULL);
            }

            // Clean escape prompt for JSON
            std::string safePrompt;
            for (char c : promptUtf8) {
                if (c == '"') safePrompt += "\\\"";
                else if (c == '\\') safePrompt += "\\\\";
                else if (c == '\n') safePrompt += "\n";
                else safePrompt += c;
            }

            std::string jsonBody = "{\"model\": " + JsonQuote(model) + ", \"messages\": [{\"role\": \"system\", \"content\": \"Eres Capri, un companero de escritorio ultraligero y util.\"}, {\"role\": \"user\", \"content\": \"" + safePrompt + "\"}]}";

            HINTERNET hSession = WinHttpOpen(L"CapriNotch-AI/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
            std::wstring answer = L"No se pudo conectar al servicio de IA.";

            if (hSession) {
                HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), port, 0);
                if (hConnect) {
                    DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
                    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", path.c_str(), NULL,
                                                            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
                    if (hRequest) {
                        std::wstring headers = L"Content-Type: application/json\r\n";
                        if (!key.empty()) {
                            headers += L"Authorization: Bearer " + key + L"\r\n";
                        }

                        if (WinHttpSendRequest(hRequest, headers.c_str(), (DWORD)headers.length(),
                                               (LPVOID)jsonBody.c_str(), (DWORD)jsonBody.length(), (DWORD)jsonBody.length(), 0) &&
                            WinHttpReceiveResponse(hRequest, NULL)) {

                            std::string response;
                            DWORD dwSize = 0, dwDownloaded = 0;
                            do {
                                dwSize = 0;
                                if (!WinHttpQueryDataAvailable(hRequest, &dwSize) || dwSize == 0) break;
                                std::vector<char> buf(dwSize + 1);
                                if (WinHttpReadData(hRequest, buf.data(), dwSize, &dwDownloaded)) {
                                    response.append(buf.data(), dwDownloaded);
                                }
                            } while (dwSize > 0);

                            // Extract content from JSON response
                            size_t contentPos = response.find("\"content\": \"");
                            if (contentPos != std::string::npos) {
                                contentPos += 12;
                                size_t endPos = response.find("\"", contentPos);
                                if (endPos != std::string::npos) {
                                    std::string rawContent = response.substr(contentPos, endPos - contentPos);
                                    std::wstring parsed;
                                    for (size_t i = 0; i < rawContent.size(); ++i) {
                                        if (rawContent[i] == '\\' && i + 1 < rawContent.size() && rawContent[i+1] == 'n') {
                                            parsed += L"\n";
                                            i++;
                                        } else {
                                            parsed += (wchar_t)(unsigned char)rawContent[i];
                                        }
                                    }
                                    answer = parsed;
                                }
                            } else {
                                answer = L"Respuesta recibida (verificar clave o cuota de API).";
                            }
                        }
                        WinHttpCloseHandle(hRequest);
                    }
                    WinHttpCloseHandle(hConnect);
                }
                WinHttpCloseHandle(hSession);
            }

            {
                std::lock_guard<std::mutex> lock(aiMutex);
                lastResponse = answer;
                isThinking = false;
            }
        }).detach();
    }
};
