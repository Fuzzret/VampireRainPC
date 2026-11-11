// Translates dumped shaders (shaders/{vs,ps}_*.bin) into gen.inc: one C++
// function per shader plus a table, for xlat_test.cpp.
// Usage: xlat_gen <shader dir> <output gen.inc>
#include "../../src/shader_translator.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 3) { fprintf(stderr, "usage: xlat_gen <dir> <out>\n"); return 1; }
    namespace fs = std::filesystem;
    std::vector<fs::path> files;
    for (auto& e : fs::directory_iterator(argv[1]))
        if (e.path().extension() == ".bin") files.push_back(e.path());
    std::sort(files.begin(), files.end());

    std::ostringstream out, table;
    int n = 0, failed = 0;
    for (auto& f : files)
    {
        std::ifstream in(f, std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(in)), {});
        std::vector<uint32_t> words(bytes.size() / 4);
        memcpy(words.data(), bytes.data(), words.size() * 4);
        std::string stem = f.stem().string();
        bool pixel = stem.rfind("ps_", 0) == 0;
        auto t = xlat::Translate(pixel ? xlat::ShaderType::kPixel : xlat::ShaderType::kVertex, words.data(), uint32_t(words.size()));
        if (!t.ok)
        {
            fprintf(stderr, "%s: %s\n", stem.c_str(), t.error.c_str());
            failed++;
            continue;
        }
        out << "static void fn_" << stem << "()\n{\n" << t.body << "}\n\n";
        table << "    { \"" << stem << "\", " << (pixel ? 1 : 0) << ", fn_" << stem << ", \"" << f.string() << "\", " << t.tempCount << " },\n";
        n++;
    }
    std::ofstream o(argv[2]);
    o << out.str();
    o << "struct GenShader { const char* name; int pixel; void (*fn)(); const char* path; unsigned temps; };\n";
    o << "static const GenShader kShaders[] = {\n" << table.str() << "};\n";
    fprintf(stderr, "translated %d shaders (%d failed)\n", n, failed);
    return failed ? 2 : 0;
}
