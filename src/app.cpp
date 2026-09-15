#include "webview.h"

int main() {
    webview::webview w(true, nullptr);
    w.set_title("FindX");
    w.set_size(600, 400, WEBVIEW_HINT_NONE);
    w.set_html("<h1 style='text-align:center; margin-top:100px; font-family:sans-serif;'>FindX is running</h1>");
    w.run();
    return 0;
}