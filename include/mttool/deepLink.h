#pragma once
/**************
 * @brief DeepLink 类用于在 Windows 平台上实现自定义 URL scheme 协议的注册和单实例参数转发。
 * @使用例子
 * // 注意：scheme 名必须符合 RFC 3986（字母开头，仅含字母/数字/+/-/.），
    // 下划线不合法，Windows 外壳和浏览器会拒绝解析，导致无法唤起进程
    const std::wstring wsScheme = L"omnistation";
    // ── scheme 协议：注册 + 单实例参数转发 ──
    deeplink::DeepLink<> deepLink(wsScheme);
    try {
        if (!deepLink.isSchemeRegistered()) {   // 已注册且指向当前 exe 则跳过，防止重复注册
            deepLink.registerScheme();
        }
    }
    catch (const std::exception& e) {
        qWarning() << "[deeplink] register failed:" << e.what();
    }

    deepLink.setOnMessage([](const std::string& url) {
        const QString qurl = QString::fromStdString(url);
        // 回调发生在管道线程（或事件循环启动前），统一排队到主线程处理
    });

    // 已有实例运行时，URL 经命名管道转发给旧实例，本进程直接退出
    if (!deepLink.runOrForward(getCommandLineArgs())) {
        return 0;
    }
 */
#ifndef _WIN32
#error This library is for Windows only.
#else

#include <windows.h>
#include <winreg.h>

#include <string>
#include <vector>
#include <functional>
#include <stdexcept>
#include <thread>
#include <atomic>
#include <memory>
#include <iostream>
#include <system_error>
#include <array>

namespace deeplink {

    namespace ipc 
    {
        class IIpcMechanism 
        {
        public:
            virtual ~IIpcMechanism() = default;
            virtual bool isServerRunning() const = 0;
            virtual void sendMessage(const std::string& sMessage) const = 0;
            virtual void startServer(std::function<void(const std::string&)> fOnMessage) = 0;
            virtual void stopServer() = 0;
        };

        class NamedPipeIpcMechanism : public IIpcMechanism {
        public:
            explicit NamedPipeIpcMechanism(const std::wstring& wsUniqueId) : m_wsPipeName(L"\\\\.\\pipe\\" + wsUniqueId)
            {
            }

            ~NamedPipeIpcMechanism() override 
            {
                stopServer();
            }

            bool isServerRunning() const override 
            {
                return WaitNamedPipeW(m_wsPipeName.c_str(), 0) || (GetLastError() == ERROR_PIPE_BUSY);
            }

            void sendMessage(const std::string& sMessage) const override 
            {
                HANDLE hPipe = CreateFileW(
                    m_wsPipeName.c_str(), GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);

                if (hPipe != INVALID_HANDLE_VALUE) 
                {
                    DWORD dwBytesWritten;
                    WriteFile(hPipe, sMessage.c_str(), (DWORD)sMessage.length(), &dwBytesWritten, NULL);
                    CloseHandle(hPipe);
                }
            }

            void startServer(std::function<void(const std::string&)> fOnMessage) override 
            {
                m_fOnMessage = std::move(fOnMessage);
                m_bStopFlag = false;
                m_tServerThread = std::thread([this]() { this->serverLoop(); });
            }

            void stopServer() override 
            {
                if (!m_bStopFlag.exchange(true)) 
                {
                    // 唤醒阻塞在 ConnectNamedPipe 的服务器线程：
                    // serverLoop 创建的是 PIPE_ACCESS_INBOUND（服务器只读）管道，
                    // 客户端必须以 GENERIC_WRITE 打开才能连接成功；
                    // 之前的 GENERIC_READ 会因访问权限不匹配返回 ERROR_ACCESS_DENIED，
                    // 导致 ConnectNamedPipe 永远无法被唤醒，join() 永久阻塞，进程无法退出。
                    HANDLE hPipe = CreateFileW(m_wsPipeName.c_str(), GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
                    if (hPipe != INVALID_HANDLE_VALUE) 
                        CloseHandle(hPipe);

                    if (m_tServerThread.joinable()) 
                        m_tServerThread.join();
                }
            }

        private:
            void serverLoop() 
            {
                while (!m_bStopFlag) 
                {
                    HANDLE hPipe = CreateNamedPipeW( m_wsPipeName.c_str(), PIPE_ACCESS_INBOUND,
                        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                        PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, NULL);

                    if (hPipe == INVALID_HANDLE_VALUE) 
                        continue;

                    BOOL bConnected = ConnectNamedPipe(hPipe, NULL) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
                    if (bConnected && !m_bStopFlag) 
                    {
                        std::array<char, 2048> arrBuffer = {};
                        DWORD dwBytesRead;
                        if (ReadFile(hPipe, arrBuffer.data(), sizeof(arrBuffer) - 1, &dwBytesRead, NULL) && dwBytesRead > 0) 
                        {
                            arrBuffer[dwBytesRead] = '\0';
                            if (m_fOnMessage)
                                m_fOnMessage(std::string(arrBuffer.data()));
                        }
                    }

                    DisconnectNamedPipe(hPipe);
                    CloseHandle(hPipe);
                }
            }

            std::wstring m_wsPipeName;
            std::function<void(const std::string&)> m_fOnMessage;
            std::thread m_tServerThread;
            std::atomic<bool> m_bStopFlag = false;
        };
    } // namespace ipc


    template <typename IpcStrategy = ipc::NamedPipeIpcMechanism>
    class DeepLink {
    public:
        explicit DeepLink(std::wstring wsScheme)  : m_wsScheme(std::move(wsScheme)), m_pIpc(std::make_unique<IpcStrategy>(m_wsScheme)) 
        {
        }

        ~DeepLink() 
        {
            if (m_pIpc)
                m_pIpc->stopServer();
        }

        DeepLink(const DeepLink&) = delete;
        DeepLink& operator=(const DeepLink&) = delete;

        void setOnMessage(std::function<void(const std::string&)> fOnMessage) 
        {
            m_fOnMessage = std::move(fOnMessage);
        }

        void registerScheme() const 
        {
            auto closeKey = [](HKEY hKey) { if (hKey) RegCloseKey(hKey); };
            using RegKeyPtr = std::unique_ptr<std::remove_pointer_t<HKEY>, decltype(closeKey)>;

            try 
            {
                std::array<wchar_t, MAX_PATH> arrExePathBuf;
                if (GetModuleFileNameW(NULL, arrExePathBuf.data(), MAX_PATH) == 0)
                    throw std::system_error(GetLastError(), std::system_category(), "getModuleFileNameW failed");

                std::wstring wsExePath = arrExePathBuf.data();

                const std::wstring wsRegPath = L"Software\\Classes\\" + m_wsScheme;
                const std::wstring wsUrlProtocolValue = L"URL:" + m_wsScheme;
                const std::wstring wsCommandValue = L"\"" + wsExePath + L"\" \"%1\"";
                const std::wstring wsIconValue = wsExePath + L",0";

                HKEY hSchemeKey;
                LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, wsRegPath.c_str(), 0, NULL, REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, NULL, &hSchemeKey, NULL);
                if (status != ERROR_SUCCESS) 
                    throw std::system_error(status, std::system_category(), "failed to create scheme root key");
                RegKeyPtr schemeKeyGuard(hSchemeKey, closeKey);

                status = RegSetValueExW(hSchemeKey, NULL, 0, REG_SZ, (const BYTE*)wsUrlProtocolValue.c_str(), (DWORD)((wsUrlProtocolValue.size() + 1) * sizeof(wchar_t)));
                if (status != ERROR_SUCCESS) 
                    throw std::system_error(status, std::system_category(), "failed to set scheme default value");

                status = RegSetValueExW(hSchemeKey, L"URL Protocol", 0, REG_SZ, (const BYTE*)L"", sizeof(wchar_t));
                if (status != ERROR_SUCCESS) 
                    throw std::system_error(status, std::system_category(), "failed to set url protocol value");

                HKEY hIconKey;
                status = RegCreateKeyExW(hSchemeKey, L"DefaultIcon", 0, NULL, REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, NULL, &hIconKey, NULL);
                if (status != ERROR_SUCCESS) 
                    throw std::system_error(status, std::system_category(), "failed to create defaultIcon key");
                RegKeyPtr iconKeyGuard(hIconKey, closeKey);

                status = RegSetValueExW(hIconKey, NULL, 0, REG_SZ, (const BYTE*)wsIconValue.c_str(), (DWORD)((wsIconValue.size() + 1) * sizeof(wchar_t)));
                if (status != ERROR_SUCCESS) 
                    throw std::system_error(status, std::system_category(), "failed to set defaultIcon value");

                HKEY hCommandKey;
                status = RegCreateKeyExW(hSchemeKey, L"shell\\open\\command", 0, NULL, REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, NULL, &hCommandKey, NULL);
                if (status != ERROR_SUCCESS) 
                    throw std::system_error(status, std::system_category(), "failed to create shell\\open\\command key");
                RegKeyPtr commandKeyGuard(hCommandKey, closeKey);

                status = RegSetValueExW(hCommandKey, NULL, 0, REG_SZ, (const BYTE*)wsCommandValue.c_str(), (DWORD)((wsCommandValue.size() + 1) * sizeof(wchar_t)));
                if (status != ERROR_SUCCESS)
                    throw std::system_error(status, std::system_category(), "failed to set command value");
            }
            catch (const std::exception& e) 
            {
                throw std::runtime_error("scheme registration for '" + toString(m_wsScheme) + "' failed: " + e.what());
            }
        }

        // 检测 scheme 是否已注册且指向当前 exe，避免重复写注册表
        bool isSchemeRegistered() const 
        {
            const std::wstring wsRegPath = L"Software\\Classes\\" + m_wsScheme + L"\\shell\\open\\command";

            HKEY hKey;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, wsRegPath.c_str(), 0, KEY_READ, &hKey) != ERROR_SUCCESS)
                return false;

            wchar_t arrBuf[1024];
            DWORD dwSize = sizeof(arrBuf);
            LSTATUS status = RegQueryValueExW(hKey, NULL, 0, NULL, (LPBYTE)arrBuf, &dwSize);
            RegCloseKey(hKey);
            if (status != ERROR_SUCCESS)
                return false;

            std::wstring wsRegistered(arrBuf, dwSize / sizeof(wchar_t));
            while (!wsRegistered.empty() && wsRegistered.back() == L'\0')
                wsRegistered.pop_back();

            std::array<wchar_t, MAX_PATH> arrExePathBuf;
            if (GetModuleFileNameW(NULL, arrExePathBuf.data(), MAX_PATH) == 0)
                return false;
            const std::wstring wsExpected = L"\"" + std::wstring(arrExePathBuf.data()) + L"\" \"%1\"";

            return wsRegistered == wsExpected;
        }

        void unregisterScheme() const 
        {
            const std::wstring wsRegPath = L"Software\\Classes\\" + m_wsScheme;
            LSTATUS status = RegDeleteTreeW(HKEY_CURRENT_USER, wsRegPath.c_str());
            if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
                throw std::runtime_error("failed to delete registry key. error code: " + std::to_string(status));
        }

        bool runOrForward(const std::vector<std::wstring>& vecArgs) 
        {
            if (m_pIpc->isServerRunning()) 
            {
                if (!vecArgs.empty())
                    m_pIpc->sendMessage(toString(vecArgs.back()));

                return false;
            }

            m_pIpc->startServer(m_fOnMessage);

            if (!vecArgs.empty() && m_fOnMessage)
            {
                const std::string& sMessage = toString(vecArgs.back());
                if (sMessage.rfind(toString(m_wsScheme) + "://", 0) == 0)
                    m_fOnMessage(sMessage);
            }

            return true;
        }

    private:
        // @todo / SapDragon: fix warning on return WideCharToMultiByte
        static std::string toString(const std::wstring& wsInput) {
            if (wsInput.empty()) return {};
            int iSizeNeeded = WideCharToMultiByte(CP_UTF8, 0, &wsInput[0], (int)wsInput.size(), NULL, 0, NULL, NULL);
            std::string sStrTo(iSizeNeeded, 0);
            WideCharToMultiByte(CP_UTF8, 0, &wsInput[0], (int)wsInput.size(), &sStrTo[0], iSizeNeeded, NULL, NULL);
            return sStrTo;
        }

        std::wstring m_wsScheme;
        std::function<void(const std::string&)> m_fOnMessage;
        std::unique_ptr<ipc::IIpcMechanism> m_pIpc;
    };

} // namespace deeplink
#endif