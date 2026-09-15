#include "app.h"

#include "webview.h"
#include <windows.h>
#include <shellapi.h>
#include <iostream>
#include <memory>

namespace {

constexpr UINT TRAY_ICON_ID = 1;
constexpr UINT WM_TRAY_ICON = WM_USER + 1;
constexpr int HOTKEY_ID = 1;

NOTIFYICONDATAW nid{};

// the webview instance — lives for the entire app lifetime, shown/hidden on hotkey
std::unique_ptr<webview::webview> wv;
bool window_visible = false;

const char* FINDX_HTML = R"html(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<style>
    * { margin: 0; padding: 0; box-sizing: border-box; }

    body {
        font-family: -apple-system, 'Segoe UI', sans-serif;
        background: #1a1a2e;
        color: #e0e0e0;
        height: 100vh;
        display: flex;
        flex-direction: column;
        padding: 16px;
        overflow: hidden;
    }

    #search-box {
        width: 100%;
        padding: 14px 18px;
        font-size: 16px;
        border: 1px solid #333;
        border-radius: 10px;
        background: #16213e;
        color: #e0e0e0;
        outline: none;
        transition: border-color 0.2s;
    }

    #search-box:focus {
        border-color: #0f3460;
    }

    #search-box::placeholder {
        color: #666;
    }

    #results {
        margin-top: 12px;
        flex: 1;
        overflow-y: auto;
        font-size: 14px;
        line-height: 1.6;
        white-space: pre-wrap;
    }

    #results .answer {
        padding: 12px;
        background: #16213e;
        border-radius: 8px;
        margin-bottom: 8px;
    }

    #results .sources {
        font-size: 12px;
        color: #888;
        padding: 8px 12px;
    }

    #results .source-link {
        color: #4a9eff;
        cursor: pointer;
        text-decoration: underline;
        display: block;
        margin: 4px 0;
    }

    #results .source-link:hover {
        color: #7bb8ff;
    }

    #status {
        font-size: 12px;
        color: #666;
        margin-top: 4px;
        padding: 0 4px;
    }

    .mode-bar {
        display: flex;
        gap: 8px;
        margin-top: 8px;
        margin-bottom: 4px;
    }

    .mode-btn {
        padding: 4px 12px;
        font-size: 12px;
        border: 1px solid #333;
        border-radius: 6px;
        background: #16213e;
        color: #888;
        cursor: pointer;
    }

    .mode-btn.active {
        border-color: #4a9eff;
        color: #4a9eff;
    }
</style>
</head>
<body>
    <input id="search-box" type="text" placeholder="Ask your files anything..." autofocus />
    <div class="mode-bar">
        <button class="mode-btn active" onclick="setMode('auto')">Auto</button>
        <button class="mode-btn" onclick="setMode('fast')">Fast</button>
        <button class="mode-btn" onclick="setMode('deep')">Deep</button>
    </div>
    <div id="status"></div>
    <div id="results"></div>

    <script>
        let currentMode = 'auto';

        function setMode(mode) {
            currentMode = mode;
            document.querySelectorAll('.mode-btn').forEach(btn => {
                btn.classList.toggle('active', btn.textContent.toLowerCase() === mode);
            });
        }

        document.getElementById('search-box').addEventListener('keydown', function(e) {
            if (e.key === 'Enter' && this.value.trim()) {
                const query = this.value.trim();
                document.getElementById('status').textContent = 'Searching...';
                document.getElementById('results').innerHTML = '';
                findx_ask(query, currentMode).then(response => {
                    displayResult(response);
                }).catch(err => {
                    document.getElementById('status').textContent = 'Error: ' + err;
                });
            }
            if (e.key === 'Escape') {
                findx_hide();
            }
        });

        function displayResult(response) {
            const data = typeof response === 'string' ? JSON.parse(response) : response;
            document.getElementById('status').textContent =
                '[' + data.model + '] ' + data.source_count + ' source(s)';

            let html = '<div class="answer">' + escapeHtml(data.answer) + '</div>';
            if (data.sources && data.sources.length > 0) {
                html += '<div class="sources"><strong>Sources:</strong>';
                data.sources.forEach((s, i) => {
                    html += '<span class="source-link" onclick="findx_open(\'' +
                            escapeAttr(s.path) + '\')">[' + (i+1) + '] ' +
                            escapeHtml(s.filename) + '</span>';
                });
                html += '</div>';
            }
            document.getElementById('results').innerHTML = html;
        }

        function escapeHtml(text) {
            const div = document.createElement('div');
            div.textContent = text;
            return div.innerHTML;
        }

        function escapeAttr(text) {
            return text.replace(/\\/g, '\\\\').replace(/'/g, "\\'");
        }
    </script>
</body>
</html>
)html";

void show_window() {
    if (wv && !window_visible) {
        wv->set_size(650, 500, WEBVIEW_HINT_NONE);
        // execute JS to clear previous state and focus the input
        wv->eval("document.getElementById('search-box').value = '';"
                  "document.getElementById('results').innerHTML = '';"
                  "document.getElementById('status').textContent = '';"
                  "document.getElementById('search-box').focus();");
        window_visible = true;
    }
}

void hide_window() {
    if (wv && window_visible) {
        wv->set_size(0, 0, WEBVIEW_HINT_NONE);
        window_visible = false;
    }
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {

    case WM_HOTKEY:
        if (wparam == HOTKEY_ID) {
            if (window_visible) {
                hide_window();
            } else {
                show_window();
            }
        }
        return 0;

    case WM_TRAY_ICON:
        if (LOWORD(lparam) == WM_RBUTTONUP) {
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, 1, L"Open");
            AppendMenuW(menu, MF_STRING, 2, L"Quit");

            POINT pt;
            GetCursorPos(&pt);
            SetForegroundWindow(hwnd);
            int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY,
                                      pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(menu);

            if (cmd == 1) {
                show_window();
            } else if (cmd == 2) {
                if (wv) {
                    wv->terminate();
                }
                PostQuitMessage(0);
            }
        }
        return 0;

    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &nid);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace

int run_app() {
    // register window class and create message-only window for tray + hotkey
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = L"FindXTrayClass";

    if (!RegisterClassExW(&wc)) {
        std::cerr << "Failed to register window class.\n";
        return 1;
    }

    HWND hwnd = CreateWindowExW(0, L"FindXTrayClass", L"FindX", 0,
                                 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                 GetModuleHandle(nullptr), nullptr);
    if (!hwnd) {
        std::cerr << "Failed to create message window.\n";
        return 1;
    }

    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = TRAY_ICON_ID;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY_ICON;
    nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wcscpy_s(nid.szTip, L"FindX — Ctrl+Space to search");
    Shell_NotifyIconW(NIM_ADD, &nid);

    if (!RegisterHotKey(hwnd, HOTKEY_ID, MOD_CONTROL, VK_SPACE)) {
        std::cerr << "Failed to register hotkey (Ctrl+Space may be taken by another app).\n";
    } else {
        std::cout << "FindX running. Press Ctrl+Space to search. Right-click tray icon to quit.\n";
    }

    // create the webview — starts hidden
    wv = std::make_unique<webview::webview>(true, nullptr);
    wv->set_title("FindX");
    wv->set_html(FINDX_HTML);

    // bind JS function: findx_hide() — called when user presses Escape
    wv->bind("findx_hide", [](const std::string&) -> std::string {
        hide_window();
        return "";
    });

    // bind JS function: findx_open(path) — opens a file in the default editor
    wv->bind("findx_open", [](const std::string& args) -> std::string {
        // args comes as a JSON array like ["C:\\path\\to\\file.cpp"]
        std::string path = args.substr(2, args.size() - 4); // strip ["..."]
        ShellExecuteA(nullptr, "open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return "";
    });

    // bind JS function: findx_ask(query, mode) — placeholder for now
    wv->bind("findx_ask", [](const std::string& args) -> std::string {
        // placeholder response — will be wired to real backend in the next stage
        return "{\"answer\":\"Backend not wired yet.\",\"model\":\"none\",\"source_count\":0,\"sources\":[]}";
    });

    // run the webview event loop — this also processes our Win32 messages
    // since webview runs its own message loop internally
    wv->run();

    UnregisterHotKey(hwnd, HOTKEY_ID);
    Shell_NotifyIconW(NIM_DELETE, &nid);
    return 0;
}