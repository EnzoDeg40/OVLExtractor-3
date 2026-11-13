// Write OVL pairs from editable sources.
//
//   ovlwrite text <strings.json> <out>   txt OVL from {"Name": "text", ...}
//                                         -> <out>.common.ovl + <out>.unique.ovl
#include "ovl/OvlImage.hpp"
#include "ovl/TextOvl.hpp"

#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>

int main(int argc, char** argv) {
    if (argc != 4 || std::string(argv[1]) != "text") {
        std::cerr << "usage: ovlwrite text <strings.json> <out base name>\n";
        return 2;
    }
    try {
        std::ifstream f(argv[2], std::ios::binary);
        if (!f) throw std::runtime_error(std::string("cannot open ") + argv[2]);
        std::stringstream ss;
        ss << f.rdbuf();
        auto entries = ovl::read_text_json(ss.str());
        auto im = ovl::build_text_ovl(entries);
        std::string base = argv[3];
        ovl::write_image_file(im[0], base + ".common.ovl");
        ovl::write_image_file(im[1], base + ".unique.ovl");
        std::cout << entries.size() << " strings -> " << base << ".common.ovl / .unique.ovl\n";
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
