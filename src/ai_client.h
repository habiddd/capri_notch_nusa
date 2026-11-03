#pragma once
#include <windows.h>
#include <winhttp.h>
#include <string>
#include <thread>
#include <mutex>
#include <vector>

class AICompanion {
public:
    std::wstring lastPrompt = L"";
    std::wstring lastResponse = L"Hola, soy tu companero Capri. En que te puedo ayudar hoy?";
    bool isThinking = false;
    std::mutex aiMutex;

    // Default configuration: Cloud API (OpenAI compatible) or Local Ollama
    std::wstring apiHost = L"api.openai.com"; // or L"localhost" for local Ollama
    std::wstring apiPath = L"/v1/chat/completions"; // or L"/api/generate"
    int apiPort = INTERNET_DEFAULT_HTTPS_PORT;
    bool useHttps = true;
    std::wstring apiKey = L""; // User enters their API key in config

    void Ask(const std::wstring& prompt) {
        if (isThinking || prompt.empty()) return;
        isThinking = true;
        lastPrompt = prompt;

        std::thread([this, prompt]() {
            {
                std::lock_guard<std::mutex> lock(aiMutex);
                lastResponse = L"Pensando respuesta en la nube...";
            }

            // If no API key configured yet, give friendly guidance
            if (apiKey.empty() && apiHost == L"api.openai.com") {
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
                else if (c == '\n') safePrompt += "\\n";
                else safePrompt += c;
            }

            std::string jsonBody = "{\"model\": \"gpt-4o-mini\", \"messages\": [{\"role\": \"system\", \"content\": \"Eres Capri, un companero de escritorio ultraligero y util.\"}, {\"role\": \"user\", \"content\": \"" + safePrompt + "\"}]}";

            HINTERNET hSession = WinHttpOpen(L"CapriNotch-AI/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
            std::wstring answer = L"No se pudo conectar al servicio de IA.";

            if (hSession) {
                HINTERNET hConnect = WinHttpConnect(hSession, apiHost.c_str(), apiPort, 0);
                if (hConnect) {
                    DWORD flags = useHttps ? WINHTTP_FLAG_SECURE : 0;
                    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", apiPath.c_str(), NULL,
                                                            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
                    if (hRequest) {
                        std::wstring headers = L"Content-Type: application/json\r\n";
                        if (!apiKey.empty()) {
                            headers += L"Authorization: Bearer " + apiKey + L"\r\n";
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

