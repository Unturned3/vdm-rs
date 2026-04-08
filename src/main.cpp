#include "argparse/argparse.hpp"
#include "gf256.hpp"
#include "matrix.hpp"
#include <iostream>
#include <print>
#include <string>

// NOLINTNEXTLINE
// int main(int argc, char* argv[])
int main()
{
    argparse::ArgumentParser parser { };
    parser.add_argument("host").help("IP address");
    parser.add_argument("port").help("Port number");
    parser.add_argument("--resize").help("Resize image to WxH");

    // try {
    // parser.parse_args(argc, argv);
    // } catch (const std::runtime_error& err) {
    // std::println(stderr, "{}", err.what());
    // std::cerr << parser;
    // return 1;
    // }
    //
    // auto host = parser.get<std::string>("host");
    // auto port = parser.get<std::string>("port");
    //
    // std::println("Host: {}, Port: {}", host, port);

    // clang-format off
    matrix::Matrix<int> a(2, 3, {
        1, 2, 3,
        4, 5, 6,
    });
    matrix::Matrix<int> b(3, 2, {
        7, 8,
        9, 10,
        11, 12,
    });
    // clang-format on

    auto c = matrix::matmul(a, b);
    std::println("c:\n{}", matrix::to_string(c));

    auto k = GF256 { 3 };
    std::println("k: {}", k);
    return 0;
}
