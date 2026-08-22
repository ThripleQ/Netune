#include <locale.h>
#include <string.h>
#include "app.h"
#include "netune_config.h"

#ifdef _WIN32
#include <windows.h>
#endif

int main(int argc, char **argv) {
    /* Enable UTF-8 multibyte processing for mbrtowc/wcwidth
       (used by Chinese marquee scrolling in song list).
       On Windows the system locale is typically GBK — use the
       explicit UTF-8 locale so UTF-8 input is decoded correctly. */
#ifdef _WIN32
    setlocale(LC_CTYPE, ".UTF-8");
#else
    setlocale(LC_CTYPE, "");
#endif

#ifdef _WIN32
    /* FTXUI writes UTF-8 bytes via std::cout. Without this, the console's
       default code page (e.g. GBK on Chinese Windows) renders them as
       garbled text (乱码). */
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    /* Subcommand: built-in configuration UI (former standalone
       `netune-config` executable, now merged into this binary).
       Dispatched AFTER the UTF-8 setup above so the config TUI also
       renders Chinese correctly on Windows. */
    if (argc > 1 && strcmp(argv[1], "--config") == 0)
        return run_config();

    return run_app(argc, argv);
}
