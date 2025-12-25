#include <iostream>
#include <string>

constexpr auto cexprHash(const char *str, std::size_t v = 0) noexcept -> std::size_t {
    return (*str == 0) ? v : 31 * cexprHash(str + 1) + *str;
}

int main() {
    constexpr auto files_hash = cexprHash("files");
    std::cout << "Compile-time hash of 'files': " << files_hash << std::endl;
    std::cout << "Runtime hash of 'files': " << cexprHash("files") << std::endl;
    std::cout << "Runtime hash of 'name': " << cexprHash("name") << std::endl;
    std::cout << "Runtime hash of 'version': " << cexprHash("version") << std::endl;
    std::cout << "Runtime hash of 'category': " << cexprHash("category") << std::endl;
    std::cout << "Runtime hash of 'repo': " << cexprHash("repo") << std::endl;
    return 0;
}
