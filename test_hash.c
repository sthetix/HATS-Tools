#include <stdio.h>
#include <stddef.h>
#include <string.h>

size_t cexprHash(const char *str, size_t v) {
    return (*str == 0) ? v : 31 * cexprHash(str + 1) + *str;
}

int main() {
    printf("Hash of 'files': %zu\n", cexprHash("files", 0));
    printf("Hash of 'name': %zu\n", cexprHash("name", 0));
    printf("Hash of 'version': %zu\n", cexprHash("version", 0));
    printf("Hash of 'category': %zu\n", cexprHash("category", 0));
    printf("Hash of 'repo': %zu\n", cexprHash("repo", 0));
    return 0;
}
