#include "argparse/argparse.hpp"
#include <iostream>
#include <print>
#include <string>

int main(int argc, char* argv[])
{
    argparse::ArgumentParser parser { };
    parser.add_argument("host").help("IP address").required();
    parser.add_argument("port").help("Port number").required();
    parser.add_argument("--resize").help("Resize image to WxH");

    try {
        parser.parse_args(argc, argv);
    } catch (const std::runtime_error& err) {
        std::println(stderr, "{}", err.what());
        std::cerr << parser;
        return 1;
    }

    auto host = parser.get<std::string>("host");
    auto port = parser.get<std::string>("port");

    std::println("Host: {}, Port: {}", host, port);
    return 0;
}
