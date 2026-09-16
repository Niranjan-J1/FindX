#include "app.h"

#include "webview.h"
#include "index.h"
#include "tokenizer.h"
#include "ranker.h"
#include "storage.h"
#include "embed_client.h"
#include "ollama_client.h"

#include <windows.h>
#include <shellapi.h>
#include <iostream>
#include <memory>
#include <sstream>

namespace {

constexpr UINT TRAY_ICON_ID = 1;
constexpr UINT WM_TRAY_ICON = WM_USER + 1;
constexpr int HOTKEY_ID = 1;

const std::string MODEL_FAST = "qwen3:1.7b";
const std::string MODEL_STRONG = "phi4-mini";

NOTIFYICONDATAW nid{};
std::unique_ptr<webview::webview> wv;
bool window_visible = false;

// escape a string for safe embedding inside a JS string literal
std::string js_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\'': out += "\\'"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:   out += c; break;
        }
    }
    return out;
}

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
    }

    #answer-text {
        padding: 12px;
        background: #16213e;
        border-radius: 8px;
        margin-bottom: 8px;
        white-space: pre-wrap;
        display: none;
    }

    #sources-list {
        font-size: 12px;
        color: #888;
        padding: 8px 12px;
        display: none;
    }

    .source-link {
        color: #4a9eff;
        cursor: pointer;
        text-decoration: underline;
        display: block;
        margin: 4px 0;
    }

    .source-link:hover {
        color: #7bb8ff;
    }

    .source-preview {
        color: #555;
        font-size: 11px;
        margin-left: 16px;
        display: block;
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
    <div id="results">
        <div id="answer-text"></div>
        <div id="sources-list"></div>
    </div>

    <script>
        let currentMode = 'auto';
        let isProcessing = false;

        function setMode(mode) {
            currentMode = mode;
            document.querySelectorAll('.mode-btn').forEach(btn => {
                btn.classList.toggle('active', btn.textContent.toLowerCase() === mode);
            });
        }

        document.getElementById('search-box').addEventListener('keydown', function(e) {
            if (e.key === 'Enter' && this.value.trim() && !isProcessing) {
                const query = this.value.trim();
                isProcessing = true;
                document.getElementById('status').textContent = 'Searching...';
                document.getElementById('answer-text').style.display = 'none';
                document.getElementById('answer-text').textContent = '';
                document.getElementById('sources-list').style.display = 'none';
                document.getElementById('sources-list').innerHTML = '';
                findx_ask(query, currentMode);
            }
            if (e.key === 'Escape') {
                findx_hide();
            }
        });

        // called from C++ to push each streamed token
        function appendToken(token) {
            const el = document.getElementById('answer-text');
            el.style.display = 'block';
            el.textContent += token;
            el.scrollTop = el.scrollHeight;
        }

        // called from C++ to set the status line
        function setStatus(text) {
            document.getElementById('status').textContent = text;
        }

        // called from C++ to display sources after answer is complete
        function setSources(sourcesJson) {
            const sources = typeof sourcesJson === 'string' ? JSON.parse(sourcesJson) : sourcesJson;
            const el = document.getElementById('sources-list');
            if (sources.length === 0) {
                el.style.display = 'none';
                return;
            }
            let html = '<strong>Sources:</strong>';
            sources.forEach((s, i) => {
                html += '<span class="source-link" onclick="findx_open(\'' +
                        s.path.replace(/\\/g, '\\\\').replace(/'/g, "\\'") +
                        '\')">[' + (i+1) + '] ' + escapeHtml(s.filename) + '</span>';
                if (s.preview) {
                    html += '<span class="source-preview">' + escapeHtml(s.preview) + '</span>';
                }
            });
            el.innerHTML = html;
            el.style.display = 'block';
            isProcessing = false;
        }

        // called from C++ on error
        function showError(msg) {
            document.getElementById('status').textContent = 'Error: ' + msg;
            isProcessing = false;
        }

        function escapeHtml(text) {
            const div = document.createElement('div');
            div.textContent = text;
            return div.innerHTML;
        }
    </script>
</body>
</html>
)html";

void show_window() {
    if (wv && !window_visible) {
        wv->set_size(650, 500, WEBVIEW_HINT_NONE);
        wv->eval("document.getElementById('search-box').value = '';"
                  "document.getElementById('answer-text').style.display = 'none';"
                  "document.getElementById('answer-text').textContent = '';"
                  "document.getElementById('sources-list').style.display = 'none';"
                  "document.getElementById('sources-list').innerHTML = '';"
                  "document.getElementById('status').textContent = '';"
                  "isProcessing = false;"
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
            if (window_visible) hide_window();
            else show_window();
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
            if (cmd == 1) show_window();
            else if (cmd == 2) { if (wv) wv->terminate(); PostQuitMessage(0); }
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
        std::cerr << "Failed to register hotkey.\n";
    } else {
        std::cout << "FindX running. Press Ctrl+Space to search.\n";
    }

    wv = std::make_unique<webview::webview>(true, nullptr);
    wv->set_title("FindX");
    wv->set_html(FINDX_HTML);

    wv->bind("findx_hide", [](const std::string&) -> std::string {
        hide_window();
        return "";
    });

    wv->bind("findx_open", [](const std::string& args) -> std::string {
        std::string path = args.substr(2, args.size() - 4);
        ShellExecuteA(nullptr, "open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return "";
    });

    // the real backend — runs retrieval + streams Ollama response to the UI
    wv->bind("findx_ask", [](const std::string& args) -> std::string {
        // parse args: ["query", "mode"]
        // minimal parsing — find the two quoted strings
        auto first_quote = args.find('"');
        auto second_quote = args.find('"', first_quote + 1);
        auto third_quote = args.find('"', second_quote + 1);
        auto fourth_quote = args.find('"', third_quote + 1);

        if (fourth_quote == std::string::npos) {
            wv->dispatch([]{ wv->eval("showError('Invalid arguments')"); });
            return "";
        }

        std::string query = args.substr(first_quote + 1, second_quote - first_quote - 1);
        std::string mode = args.substr(third_quote + 1, fourth_quote - third_quote - 1);

        std::filesystem::path db_path = "findx.db";

        // Step 1: load index
        std::vector<DocumentRecord> documents;
        InvertedIndex index;
        std::error_code ec;
        if (!load_index(db_path, documents, index, ec)) {
            wv->dispatch([]{ wv->eval("showError('Could not load index. Run findx index first.')"); });
            return "";
        }

        // Step 2: hybrid retrieval
        std::vector<std::string> query_tokens = tokenize(query);
        std::vector<ScoredDocument> bm25_results;
        if (!query_tokens.empty()) {
            bm25_results = rank_bm25(index, query_tokens);
        }

        std::vector<SemanticResult> semantic_results;
        std::error_code embed_ec;
        auto embedding = get_query_embedding(query, embed_ec);
        if (!embed_ec && !embedding.empty()) {
            auto query_bytes = serialize_float_vector(embedding);
            std::error_code search_ec;
            semantic_results = search_semantic(db_path, query_bytes, 10, search_ec);
        }

        auto hybrid = merge_hybrid(bm25_results, documents, semantic_results, 5);

        if (hybrid.empty()) {
            wv->dispatch([]{ wv->eval("setStatus('No relevant sources found.')"); });
            wv->dispatch([]{ wv->eval("setSources([])"); });
            return "";
        }

        // Step 3: build prompt
        std::ostringstream prompt;
        prompt << "You are a helpful assistant that answers questions based ONLY on the provided sources. "
               << "Cite sources using [1], [2], etc. after each claim. "
               << "If the sources don't contain enough information, say so.\n\n";

        struct Source {
            std::filesystem::path path;
            std::string preview;
        };
        std::vector<Source> sources;
        std::unordered_map<std::string, std::size_t> seen_paths;

        for (const auto& r : hybrid) {
            std::string path_str = r.path.string();
            if (seen_paths.find(path_str) == seen_paths.end()) {
                std::size_t num = sources.size() + 1;
                seen_paths[path_str] = num;
                std::string preview = r.chunk_preview;
                if (preview.size() > 100) preview = preview.substr(0, 100) + "...";
                sources.push_back(Source{ r.path, preview });
            }
        }

        for (const auto& r : hybrid) {
            std::string path_str = r.path.string();
            std::size_t num = seen_paths[path_str];
            std::string content = r.chunk_preview;
            for (const auto& sr : semantic_results) {
                if (sr.path.string() == path_str && sr.chunk_text.size() > content.size()) {
                    content = sr.chunk_text;
                }
            }
            prompt << "[Source " << num << ": " << r.path.filename().string() << "]\n"
                   << content << "\n\n";
        }

        prompt << "Question: " << query << "\nAnswer:";

        // Step 4: pick model
        std::string model;
        if (mode == "fast") model = MODEL_FAST;
        else if (mode == "deep") model = MODEL_STRONG;
        else model = MODEL_STRONG; // default to strong for UI

        std::string model_name = model;
        wv->dispatch([model_name]{
            wv->eval("setStatus('[" + js_escape(model_name) + "] Generating...')");
        });

        // Step 5: stream response to UI
        std::error_code ollama_ec;
        query_ollama_stream(prompt.str(), model,
            [](const std::string& token) {
                std::string escaped = js_escape(token);
                wv->dispatch([escaped]{
                    wv->eval("appendToken('" + escaped + "')");
                });
            },
            ollama_ec);

        if (ollama_ec) {
            wv->dispatch([]{ wv->eval("showError('Could not reach Ollama. Is it running?')"); });
            return "";
        }

        // Step 6: send sources to UI
        std::ostringstream sources_json;
        sources_json << "[";
        for (std::size_t i = 0; i < sources.size(); ++i) {
            if (i > 0) sources_json << ",";
            sources_json << "{\"path\":\"" << js_escape(sources[i].path.string())
                          << "\",\"filename\":\"" << js_escape(sources[i].path.filename().string())
                          << "\",\"preview\":\"" << js_escape(sources[i].preview) << "\"}";
        }
        sources_json << "]";

        std::string sj = sources_json.str();
        wv->dispatch([sj, model_name]{
            wv->eval("setSources('" + js_escape(sj) + "')");
            wv->eval("setStatus('[" + js_escape(model_name) + "] Done')");
        });

        return "";
    });

    wv->run();

    UnregisterHotKey(hwnd, HOTKEY_ID);
    Shell_NotifyIconW(NIM_DELETE, &nid);
    return 0;
}