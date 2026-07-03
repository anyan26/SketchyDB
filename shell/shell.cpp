#include "sketchydb.h"

#include <iostream>
#include <string>

namespace {

bool is_exit_command(const std::string& line) {
    return line == ".exit" || line == ".quit";
}

void print_help() {
    std::cout << ".help              Show this message\n";
    std::cout << ".quit or .exit     Exit the shell\n";
}

int print_row(void* user_data, int column_count, char** column_values, char** column_names) {
    (void)user_data;

    for (int column = 0; column < column_count; ++column) {
        if (column > 0) {
            std::cout << " | ";
        }
        std::cout << column_names[column] << "=" << column_values[column];
    }
    std::cout << '\n';
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const char* filename = argc > 1 ? argv[1] : ":memory:";

    skdb* db = nullptr;
    int rc = skdb_open(filename, &db);
    if (rc != SKDB_OK) {
        std::cerr << "cannot open database\n";
        return rc;
    }

    std::cout << "SketchyDB " << skdb_libversion() << '\n';
    std::cout << "Enter .help for usage hints.\n";

    std::string line;
    while (std::cout << "skdb> " && std::getline(std::cin, line)) {
        if (line.empty()) {
            continue;
        }
        if (is_exit_command(line)) {
            break;
        }
        if (line == ".help") {
            print_help();
            continue;
        }

        char* error_message = nullptr;
        rc = skdb_exec(db, line.c_str(), print_row, nullptr, &error_message);
        if (rc != SKDB_OK) {
            std::cerr << "error: " << (error_message == nullptr ? skdb_errmsg(db) : error_message)
                      << '\n';
            skdb_free(error_message);
        }
    }

    skdb_close(db);
    return 0;
}
