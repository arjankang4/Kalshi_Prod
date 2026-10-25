// kp::pycompat::py_float_repr against CPython's repr() and json.dumps(), on 50k doubles including
// random bit patterns (subnormals, huge exponents) and the plain/exponent switch-over points.
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>

#include "harness.hpp"
#include "kp/pycompat/repr.hpp"

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "tests/data/golden";
    std::ifstream in(dir + "/pyrepr.vec");
    if (!in) { std::printf("FATAL: no pyrepr.vec\n"); return 1; }
    std::string line;
    long n = 0, bad = 0;
    std::string first;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string fn, hx, want_repr, want_json;
        ss >> fn >> hx >> want_repr >> want_json;
        double x;
        if (hx == "nan") x = std::nan("");
        else if (hx == "inf") x = HUGE_VAL;
        else if (hx == "-inf") x = -HUGE_VAL;
        else x = std::strtod(hx.c_str(), nullptr);
        ++n;
        if (kp::pycompat::py_float_repr(x) != want_repr || kp::pycompat::py_json_float(x) != want_json) {
            if (!bad) first = line + "  got " + kp::pycompat::py_float_repr(x);
            ++bad;
        }
    }
    std::printf("pyrepr: %ld cases, %ld mismatched%s%s\n", n, bad, bad ? "; first: " : "",
                first.c_str());
    return (n == 0 || bad) ? 1 : 0;
}
