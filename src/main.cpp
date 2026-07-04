#include <locale.h>
#include "app.h"

int main(int argc, char **argv) {
    /* Enable UTF-8 multibyte processing for mbrtowc/wcwidth
       (used by Chinese marquee scrolling in song list). */
    setlocale(LC_CTYPE, "");
    return run_app(argc, argv);
}
